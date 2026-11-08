# Transmitter_v2

PlatformIO firmware for one ESP32 transmitter with an INMP441 microphone and optional SH1106 I2C OLED. This project is separate from the original transmitter sketch.

## Wiring

### INMP441

| INMP441 | ESP32 |
|---|---|
| VDD | 3V3 |
| GND | GND |
| SCK / BCLK | GPIO32 |
| WS / LRCLK | GPIO25 |
| SD / DOUT | GPIO33 |
| L/R | GND (left channel) |

### SH1106 OLED (optional)

| OLED | ESP32 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

## Before uploading

1. Set `RECEIVER_MAC` in `src/main.cpp` to the actual STA MAC printed by `Receiver_v2`.
2. Set `TRANSMITTER_ID` to 1–6 for the OLED label. The ID is only a label; `Receiver_v2` identifies the sender by its ESP32 MAC.
3. Add this board's printed `Transmitter STA MAC` to an available entry in `SENDER_MACS` in `Receiver_v2/src/main.cpp`.
4. The firmware enables ESP-NOW Long Range on channel 1. All transmitters, Receiver_v2, and CYD must use the same Long Range mode and channel.

The packet is exactly one `float`, which matches the single-float packet expected by `Receiver_v2`. The transmitter calculates RMS from the INMP441's signed 24-bit samples and applies the calibration coefficients without a lower or upper clamp. If I2S stops producing valid samples for 4 seconds, it displays `MIC ERROR` and stops sending until readings recover. Verify/calibrate it against a sound-level meter before treating it as accurate dBA.

## Upload and monitor

Run from this directory:

```sh
pio run -e esp32_transmitter -t upload
pio device monitor -b 115200
```
