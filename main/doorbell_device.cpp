// SPDX-License-Identifier: MIT
// Zigbee side: sleepy end device that exposes a doorbell push button.
//
// Device model:
//   endpoint 1, HA profile, device id "Remote Control" (0x0006)
//   server clusters: Basic, Identify, Multistate Input (0x0012)
//
// The button is NOT modelled as an on/off switch: a press is reported as
// MultistateInput.PresentValue (1 = pressed, 0 = released), which is the cluster
// ZCL defines for push buttons and which Zigbee2MQTT maps to an `action`
// ("single"). That is what makes it a doorbell instead of a light switch.
//
// Deep sleep: every wake-up is a cold boot. The stack restores its network state
// from NVS and re-attaches to the parent; zigbee_wait_ready() blocks until that
// happened, then the application reports and the chip is powered down again.

#include "doorbell_device.hpp"

#include <atomic>
#include <cinttypes>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/identify_desc.h"
#include "ezbee/zcl/cluster/multistate_input_desc.h"

#include "doorbell_config.hpp"

namespace doorbell {
namespace {

constexpr const char *TAG = "ZB";

// PresentValue of the multistate input cluster: 0..number_of_states-1.
constexpr uint16_t kStateReleased = 0;
constexpr uint16_t kStatePressed = 1;

std::atomic<bool> s_joined{false};
std::atomic<bool> s_fresh_join{false};

SemaphoreHandle_t s_ready_sem = nullptr;
SemaphoreHandle_t s_report_cnf_sem = nullptr;

ezb_bdb_comm_mode_mask_t s_retry_mode = EZB_BDB_MODE_NETWORK_STEERING;

void on_report_cnf(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    (void)user_ctx;
    if (cnf != nullptr && cnf->status != 0) {
        ESP_LOGW(TAG, "report was not delivered (status 0x%02x)", cnf->status);
    }
    if (s_report_cnf_sem != nullptr) {
        xSemaphoreGive(s_report_cnf_sem);
    }
}

void mark_network_ready(bool fresh_join)
{
    s_joined.store(true);
    s_fresh_join.store(fresh_join);
    if (s_ready_sem != nullptr) {
        xSemaphoreGive(s_ready_sem);
    }
}

void schedule_commissioning(ezb_bdb_comm_mode_mask_t mode, uint32_t delay_ms)
{
    s_retry_mode = mode;
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_bdb_start_top_level_commissioning(mode);
    esp_zigbee_lock_release();
}

ezb_err_t create_doorbell_endpoint()
{
    ezb_af_ep_config_t ep_config = {};
    ep_config.ep_id = kEndpointId;
    ep_config.app_profile_id = EZB_AF_HA_PROFILE_ID;
    ep_config.app_device_id = EZB_ZHA_REMOTE_CONTROL_DEVICE_ID;
    ep_config.app_device_version = 1;

    ezb_af_ep_desc_t ep = ezb_af_create_endpoint_desc(&ep_config);
    ESP_RETURN_ON_FALSE(ep != EZB_INVALID_AF_EP_DESC, EZB_ERR_FAIL, TAG, "endpoint descriptor alloc failed");

    ezb_zcl_basic_cluster_server_config_t basic_config = {
        .zcl_version = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    ezb_zcl_cluster_desc_t basic = ezb_zcl_basic_create_cluster_desc(&basic_config, EZB_ZCL_CLUSTER_SERVER);
    ESP_RETURN_ON_FALSE(basic != EZB_INVALID_ZCL_CLUSTER_DESC, EZB_ERR_FAIL, TAG, "basic cluster alloc failed");
    ESP_RETURN_ON_FALSE(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                                            kManufacturerName) == EZB_ERR_NONE,
                        EZB_ERR_FAIL, TAG, "manufacturer name attribute failed");
    ESP_RETURN_ON_FALSE(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                                            kModelIdentifier) == EZB_ERR_NONE,
                        EZB_ERR_FAIL, TAG, "model identifier attribute failed");
    ESP_RETURN_ON_FALSE(ezb_af_endpoint_add_cluster_desc(ep, basic) == EZB_ERR_NONE, EZB_ERR_FAIL, TAG,
                        "basic cluster registration failed");

    ezb_zcl_identify_cluster_server_config_t identify_config = {
        .identify_time = EZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t identify = ezb_zcl_identify_create_cluster_desc(&identify_config, EZB_ZCL_CLUSTER_SERVER);
    ESP_RETURN_ON_FALSE(identify != EZB_INVALID_ZCL_CLUSTER_DESC, EZB_ERR_FAIL, TAG, "identify cluster alloc failed");
    ESP_RETURN_ON_FALSE(ezb_af_endpoint_add_cluster_desc(ep, identify) == EZB_ERR_NONE, EZB_ERR_FAIL, TAG,
                        "identify cluster registration failed");

    ezb_zcl_multistate_input_cluster_server_config_t button_config = {
        .number_of_states = 2,
        .out_of_service = false,
        .present_value = kStateReleased,
        .status_flags = EZB_ZCL_MULTISTATE_INPUT_STATUS_FLAGS_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t button = ezb_zcl_multistate_input_create_cluster_desc(&button_config, EZB_ZCL_CLUSTER_SERVER);
    ESP_RETURN_ON_FALSE(button != EZB_INVALID_ZCL_CLUSTER_DESC, EZB_ERR_FAIL, TAG, "button cluster alloc failed");
    ESP_RETURN_ON_FALSE(ezb_af_endpoint_add_cluster_desc(ep, button) == EZB_ERR_NONE, EZB_ERR_FAIL, TAG,
                        "button cluster registration failed");

    ezb_af_device_desc_t device = ezb_af_create_device_desc();
    ESP_RETURN_ON_FALSE(device != EZB_INVALID_AF_DEVICE_DESC, EZB_ERR_FAIL, TAG, "device descriptor alloc failed");
    ESP_RETURN_ON_FALSE(ezb_af_device_add_endpoint_desc(device, ep) == EZB_ERR_NONE, EZB_ERR_FAIL, TAG,
                        "endpoint registration failed");
    ESP_RETURN_ON_FALSE(ezb_af_device_desc_register(device) == EZB_ERR_NONE, EZB_ERR_FAIL, TAG,
                        "device descriptor registration failed");

    ESP_LOGI(TAG, "endpoint %d created (remote control, multistate input)", kEndpointId);
    return EZB_ERR_NONE;
}

bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    const ezb_app_signal_type_t type = ezb_app_signal_get_type(app_signal);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "stack ready, initializing BDB");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const auto status = *static_cast<const ezb_bdb_comm_status_t *>(ezb_app_signal_get_params(app_signal));
        if (status != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "%s failed (0x%02x), retrying", ezb_app_signal_to_string(type), status);
            schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, 1000);
            break;
        }
        if (ezb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "factory new device, searching for a network to join");
            schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, 0);
        } else {
            ESP_LOGI(TAG, "network state restored from NVS, re-attached (PAN 0x%04x, channel %d, short 0x%04x)",
                     ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            mark_network_ready(false);
        }
    } break;

    case EZB_BDB_SIGNAL_STEERING: {
        const auto status = *static_cast<const ezb_bdb_comm_status_t *>(ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "joined network (PAN 0x%04x, channel %d, short 0x%04x)", ezb_nwk_get_panid(),
                     ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            mark_network_ready(true);
        } else {
            ESP_LOGW(TAG, "network steering failed (0x%02x), is permit-join open?", status);
            schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, 3000);
        }
    } break;

    case EZB_ZDO_SIGNAL_LEAVE: {
        const auto *leave = static_cast<const ezb_zdo_signal_leave_params_t *>(ezb_app_signal_get_params(app_signal));
        ESP_LOGW(TAG, "left the network (type 0x%02x)", leave != nullptr ? leave->leave_type : 0xff);
        s_joined.store(false);
        schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, 2000);
    } break;

    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const auto duration = *static_cast<const uint8_t *>(ezb_app_signal_get_params(app_signal));
        if (duration > 0) {
            ESP_LOGI(TAG, "network 0x%04x open for joining, %d s", ezb_nwk_get_panid(), duration);
        }
    } break;

    default:
        ESP_LOGD(TAG, "signal %s (0x%02x)", ezb_app_signal_to_string(type), type);
        break;
    }
    return true;
}

void zigbee_task(void *)
{
    esp_zigbee_config_t config = {};
    config.device_config.device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE;
    config.device_config.install_code_policy = false;
    config.device_config.zed_config.ed_timeout = CONFIG_DOORBELL_ED_TIMEOUT;
    config.device_config.zed_config.keep_alive = CONFIG_DOORBELL_KEEPALIVE_MS;
    config.platform_config.storage_partition_name = "nvs";
    config.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE;

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    // Receiver off when idle: the device only hears the coordinator while it
    // polls. Mandatory for a sleeping end device.
    ezb_nwk_set_rx_on_when_idle(false);
    ezb_aps_secur_enable_distributed_security(false);

    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler) == EZB_ERR_NONE ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(create_doorbell_endpoint() == EZB_ERR_NONE ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t zigbee_start()
{
    s_ready_sem = xSemaphoreCreateBinary();
    s_report_cnf_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_ready_sem != nullptr && s_report_cnf_sem != nullptr, ESP_ERR_NO_MEM, TAG,
                        "semaphore alloc failed");

    if (xTaskCreate(zigbee_task, "zigbee", 6144, nullptr, 5, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool zigbee_joined() { return s_joined.load(); }

bool zigbee_fresh_join() { return s_fresh_join.load(); }

bool zigbee_wait_ready(uint32_t timeout_ms)
{
    if (s_joined.load()) {
        return true;
    }
    return xSemaphoreTake(s_ready_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

esp_err_t report_button_state(uint16_t state)
{
    if (!s_joined.load()) {
        ESP_LOGW(TAG, "not attached to a network, dropping button state %u", state);
        return ESP_ERR_INVALID_STATE;
    }

    ezb_zcl_report_attr_cmd_t report = {};
    report.cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
    report.cmd_ctrl.dst_addr.u.short_addr = kCoordinatorShortAddress;
    report.cmd_ctrl.dst_ep = kCoordinatorEndpoint;
    report.cmd_ctrl.src_ep = kEndpointId;
    report.cmd_ctrl.cluster_id = EZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT;
    report.cmd_ctrl.fc.direction = EZB_ZCL_CMD_DIRECTION_TO_CLI;
    report.cmd_ctrl.fc.dis_default_rsp = true;
    report.cmd_ctrl.cnf_ctx.cb = on_report_cnf;
    report.payload.attr_id = EZB_ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE_ID;

    // Never let a confirmation of an earlier report satisfy this one.
    xSemaphoreTake(s_report_cnf_sem, 0);

    esp_zigbee_lock_acquire(portMAX_DELAY);
    const ezb_zcl_status_t status =
        ezb_zcl_set_attr_value(kEndpointId, EZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT, EZB_ZCL_CLUSTER_SERVER,
                               EZB_ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE_ID, EZB_ZCL_STD_MANUF_CODE,
                               const_cast<uint16_t *>(&state), false);
    ezb_err_t ret = EZB_ERR_FAIL;
    if (status == EZB_ZCL_STATUS_SUCCESS) {
        ret = ezb_zcl_report_attr_cmd_req(&report);
    } else {
        ESP_LOGE(TAG, "set PresentValue failed with status 0x%02x", status);
    }
    esp_zigbee_lock_release();

    if (ret != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "report request failed with error 0x%04x", ret);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_report_cnf_sem, pdMS_TO_TICKS(kReportTimeoutMs)) != pdTRUE) {
        ESP_LOGW(TAG, "no confirmation for state %u within %" PRIu32 " ms", state, kReportTimeoutMs);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "reported button state %u", state);
    return ESP_OK;
}

}  // namespace doorbell
