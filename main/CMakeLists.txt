idf_component_register(
    SRCS "app_main.cpp" "doorbell_button.cpp" "doorbell_device.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash driver
)

if(CONFIG_DOORBELL_GPIO_PROBE)
    idf_component_get_property(lib main COMPONENT_LIB)
    target_sources(${lib} PRIVATE "gpio_probe.cpp")
endif()
