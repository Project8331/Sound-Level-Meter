#pragma once

// TFT_eSPI setup for the common ESP32-2432S028R (ILI9341) board.
#define ILI9341_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320
#define TFT_MISO   12
#define TFT_MOSI   13
#define TFT_SCLK   14
#define TFT_CS     15
#define TFT_DC      2
#define TFT_RST    -1
#define TFT_BL     21
#define TFT_BACKLIGHT_ON HIGH
#define TOUCH_CS 33
#define TFT_RGB_ORDER TFT_BGR
#define USE_HSPI_PORT
#define SPI_FREQUENCY  40000000
#define SPI_READ_FREQUENCY  16000000
#define SUPPORT_TRANSACTIONS
