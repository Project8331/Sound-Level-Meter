# ESP32 DS3231 RTC Test

Standalone PlatformIO test sketch for reading and setting a DS3231 RTC using an ESP32. The INMP441 microphone is not used in this project.

## Wiring (ESP32 DevKit)

| DS3231 | ESP32 |
|---|---|
| VCC | 3V3 (if supported by the RTC module) |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |
| SQW / 32K (if present) | Leave unconnected |

Use the pin labels on the module. If your ESP32 board has different I2C pins, update `RTC_SDA_PIN` and `RTC_SCL_PIN` in `src/main.cpp`.

## Upload and test

Run from this folder:

```sh
pio run -e esp32_ds3231_test -t upload
pio device monitor -b 115200
```

The sketch prints the time every second. Send these commands in Serial Monitor with a newline:

```text
SET 2026-10-30 14:30:00
GET
HELP
```

If the DS3231 reports lost power, the sketch initializes it from the firmware build time. Use `SET` to enter the correct local time. Ensure the backup battery is installed so time continues while the ESP32 is off.
