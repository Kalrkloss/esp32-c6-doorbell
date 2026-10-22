# ESP32-C6 Zigbee doorbell button

Battery-friendly Zigbee push button (a doorbell) on an ESP32-C6, built with
ESP-IDF + ESP-Zigbee-SDK in C++. The device sits in **deep sleep (~7 µA)**, wakes
on the doorbell contact at GPIO0, reports the press to the coordinator and goes
back to sleep.

## What the device looks like on the Zigbee side

| Item | Value |
| --- | --- |
| Endpoint | 1 (HA profile `0x0104`, device id `0x0006` *Remote Control*) |
| Server clusters | Basic, Identify, Multistate Input (`0x0012`) |
| Signal | `MultistateInput.PresentValue`: `1` = contact closed (press), `0` = released |
| Model / manufacturer | `C6-DOORBELL` / `DIY` |

The button is deliberately **not** an on/off switch: no toggle commands, no
switch entity. A press is a multistate value change, the cluster ZCL defines for
push buttons; Zigbee2MQTT turns it into an `action` (`single`), Home Assistant
shows it as a device trigger. That is the doorbell semantic.

## Sleep model (deep sleep, not light sleep)

* Deep sleep: CPU/radio/digital domain off, RTC domain keeps a few words
  (`RTC_DATA_ATTR`), typical current 7 µA (ESP32-C6 datasheet, table 5-11).
* Every wake-up is a **cold boot**: NVS → Zigbee stack restores its network
  state → re-attach to the parent → report → sleep. Measured boot-to-report path
  is ~0.6–1.5 s, which is the price for the ~25× lower idle current compared to
  light sleep (35–180 µA).
* Wake sources: the contact (EXT1, active low) and optionally a maintenance
  timer (`Doorbell firmware → Periodic keepalive wake-up`, default off).
* Because deep sleep loses the parent link, a press after a very long idle
  performs a rejoin first (one extra round trip, no user-visible difference).

## Behaviour

1. Boot, NVS init, GPIO0 configured as input with pull-up (also in the RTC
   domain, so the line is defined while the digital domain is down).
2. Zigbee stack starts; if NVS holds a network it re-attaches, otherwise it
   searches for a network (permit-join must be open).
3. First join only: stay awake `CONFIG_DOORBELL_JOIN_AWAKE_S` (default 120 s),
   polling every second, so the coordinator can interview the device.
4. Press → `PresentValue = 1` → wait for release → `PresentValue = 0` →
   deep sleep. The trailing `0` guarantees the next press is a value *change*.
5. A press that cannot be delivered (no network) is kept in RTC memory and
   retried on the next wake-up (max 5 queued presses).
6. If the contact is still closed at sleep time, the chip sleeps on the timer
   only (otherwise the level-triggered wake-up would boot-loop).

## Build & flash

```bash
. /opt/esp-idf/export.sh               # ESP-IDF v5.5.x
cd /opt/projects/esp32c6-zigbee-doorbell
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM2 flash monitor
```

`main/idf_component.yml` pulls `espressif/esp-zigbee-lib` (^2.0.5) from the
component registry on the first build.

Kconfig options (menu `Doorbell firmware`): button GPIO (default 0), ED timeout,
poll interval while awake, join-aware window, optional keepalive wake-up,
self-test mode (`DOORBELL_SELFTEST`, one simulated press per boot, then restart
instead of sleeping — for bench tests without touching the hardware).

## Zigbee2MQTT

`z2m/esp32c6_doorbell.js` is the external converter. Install it either by
copying it to `/opt/zigbee2mqtt/data/external_converters/` or, without a
restart, through the bridge API:

```bash
python3 - <<'PY'
import json, pathlib, subprocess
code = pathlib.Path('z2m/esp32c6_doorbell.js').read_text()
subprocess.run(['mosquitto_pub', '-t', 'zigbee2mqtt/bridge/request/converter/save',
                '-m', json.dumps({'name': 'esp32c6_doorbell.js', 'code': code})], check=True)
PY
```

Pairing: open permit-join (`zigbee2mqtt/bridge/request/permit_join`
`{"value": true, "time": 254}`), then power-cycle the board — a factory-new
device steers into the network and stays awake for the interview.

Every press publishes `zigbee2mqtt/<friendly_name>/action` = `single` and a
device trigger, e.g. for a Home Assistant automation:

```yaml
trigger:
  - platform: device
    domain: mqtt
    device_id: <doorbell device>
    type: action
    subtype: single
```

## Power budget

| State | Current |
| --- | --- |
| Deep sleep | 7 µA (datasheet typ.) |
| Wake-up + report (~1 s, radio on) | tens of mA for ~1 s |

With only button presses and no keepalive, a 2000 mAh cell is dominated by the
idle current (~1.2 mAh/day → years). Each hourly keepalive wake-up would add
roughly 6 mAh/day, which is why it is off by default.

Note: the board is currently fed from the USB hub, so no inline current
measurement was possible; the figures are the datasheet values for the parts of
the chip this firmware switches off.
