#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif
#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <math.h>
#include <string.h>

namespace {
// ID hanya untuk label OLED; Receiver mengenali perangkat berdasarkan MAC.
// Update this number for the OLED label only. Receiver identifies senders by MAC.
constexpr uint8_t TRANSMITTER_ID = 3;

// Destination from the provided transmitter sketch. Replace if this is not
// the current STA MAC printed by Receiver_v2.
// MAC tujuan Receiver yang menerima satu nilai dB dari transmitter ini.
const uint8_t RECEIVER_MAC[6] = {0, 0, 0, 0, 0, 0};

constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr uint16_t OLED_WIDTH = 128;
constexpr uint16_t OLED_HEIGHT = 64;
constexpr int OLED_RESET = -1;

// INMP441 pins, matching the provided transmitter and Receiver_v2 pin map.
constexpr gpio_num_t I2S_WS_PIN = GPIO_NUM_25;
constexpr gpio_num_t I2S_DATA_PIN = GPIO_NUM_33;
constexpr gpio_num_t I2S_BCLK_PIN = GPIO_NUM_32;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr uint32_t I2S_SAMPLE_RATE = 44100;
constexpr size_t I2S_BUFFER_SAMPLES = 256;
constexpr uint32_t SEND_INTERVAL_MS = 1000;
constexpr uint32_t DISPLAY_INTERVAL_MS = 250;
constexpr uint32_t READING_TIMEOUT_MS = 4000;
constexpr uint32_t ERROR_LOG_INTERVAL_MS = 5000;
constexpr uint8_t ESPNOW_CHANNEL = 1;

constexpr uint8_t SMOOTHING_LENGTH = 10;
constexpr float MIC_SENSITIVITY = 25.0f;
constexpr float REFERENCE_SOUND_PRESSURE = 20e-6f;
// Preserve the calibration coefficients from the provided transmitter code.
// Recalibrate these against a sound-level meter for accurate dBA readings.
constexpr float CALIBRATION_GAIN = 1.0514f;
constexpr float CALIBRATION_OFFSET = -1.7242f;

// Format payload ESP-NOW: satu float agar sesuai dengan Receiver_v2.
struct SoundPacket {
    float dB2;
};
static_assert(sizeof(SoundPacket) == sizeof(float), "Receiver expects one float per sender packet");

Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
esp_now_peer_info_t receiverPeer = {};
int32_t i2sBuffer[I2S_BUFFER_SAMPLES];
float smoothingValues[SMOOTHING_LENGTH] = {};
uint8_t smoothingIndex = 0;
uint8_t smoothingCount = 0;
float smoothingTotal = 0.0f;
float latestDb = NAN;
bool displayReady = false;
uint32_t lastSendMs = 0;
uint32_t lastDisplayMs = 0;
uint32_t lastValidReadingMs = 0;
uint32_t lastReadErrorLogMs = 0;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataSent(const wifi_tx_info_t *, esp_now_send_status_t status) {
#else
void onDataSent(const uint8_t *, esp_now_send_status_t status) {
#endif
    Serial.printf("Status link Receiver: %s\n",
                  status == ESP_NOW_SEND_SUCCESS ? "ACK diterima" : "GAGAL");
}

// Menginisialisasi I2S 32-bit untuk membaca data audio dari INMP441.
bool initializeI2S() {
    const i2s_config_t config = {
        .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = I2S_BUFFER_SAMPLES,
        .use_apll = false
    };

    esp_err_t result = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
    if (result != ESP_OK) {
        Serial.printf("I2S driver install failed: %d\n", result);
        return false;
    }

    const i2s_pin_config_t pins = {
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DATA_PIN
    };
    result = i2s_set_pin(I2S_PORT, &pins);
    if (result != ESP_OK) {
        Serial.printf("I2S pin setup failed: %d\n", result);
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    result = i2s_start(I2S_PORT);
    if (result != ESP_OK) {
        Serial.printf("I2S start failed: %d\n", result);
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    i2s_zero_dma_buffer(I2S_PORT);
    return true;
}

// Mengaktifkan ESP-NOW Long Range pada channel 1 dan mendaftarkan Receiver sebagai peer.
bool initializeEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    esp_err_t result = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    if (result != ESP_OK) {
        Serial.printf("Gagal mengaktifkan ESP-NOW Long Range: %d\n", result);
        return false;
    }
    result = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (result != ESP_OK) {
        Serial.printf("Gagal mengatur channel Wi-Fi %u: %d\n", ESPNOW_CHANNEL, result);
        return false;
    }
    Serial.printf("ESP-NOW Long Range aktif, channel %u\n", ESPNOW_CHANNEL);
    Serial.print("Transmitter STA MAC: ");
    Serial.println(WiFi.macAddress());
    Serial.printf("Receiver destination MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  RECEIVER_MAC[0], RECEIVER_MAC[1], RECEIVER_MAC[2],
                  RECEIVER_MAC[3], RECEIVER_MAC[4], RECEIVER_MAC[5]);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW initialization failed");
        return false;
    }
    const esp_err_t callbackResult = esp_now_register_send_cb(onDataSent);
    if (callbackResult != ESP_OK) {
        Serial.printf("Gagal mendaftarkan callback ESP-NOW: %d\n", callbackResult);
        return false;
    }

    memcpy(receiverPeer.peer_addr, RECEIVER_MAC, sizeof(RECEIVER_MAC));
    receiverPeer.channel = ESPNOW_CHANNEL;
    receiverPeer.encrypt = false;

    const esp_err_t peerResult = esp_now_add_peer(&receiverPeer);
    if (peerResult != ESP_OK && peerResult != ESP_ERR_ESPNOW_EXIST) {
        Serial.printf("Failed to add Receiver peer: %d\n", peerResult);
        return false;
    }

    Serial.println("ESP-NOW ready");
    return true;
}

// Menghitung RMS dari sampel INMP441, menerapkan kalibrasi, lalu menghaluskan hasil.
float measureDecibels() {
    size_t bytesRead = 0;
    const esp_err_t result = i2s_read(I2S_PORT, i2sBuffer, sizeof(i2sBuffer),
                                      &bytesRead, pdMS_TO_TICKS(100));
    if (result != ESP_OK || bytesRead < sizeof(int32_t)) {
        const uint32_t now = millis();
        if (now - lastReadErrorLogMs >= ERROR_LOG_INTERVAL_MS) {
            lastReadErrorLogMs = now;
            Serial.printf("I2S read gagal: err=%d, bytes=%u\n",
                          static_cast<int>(result), static_cast<unsigned>(bytesRead));
        }
        return NAN;
    }

    const size_t sampleCount = bytesRead / sizeof(int32_t);
    double squareSum = 0.0;
    for (size_t i = 0; i < sampleCount; ++i) {
        // INMP441 audio is signed 24-bit data left-aligned in each 32-bit slot.
        const int32_t sample = i2sBuffer[i] >> 8;
        const double normalizedSample = static_cast<double>(sample) / 8388608.0;
        squareSum += normalizedSample * normalizedSample;
    }

    const float normalizedLevel = static_cast<float>(sqrt(squareSum / sampleCount));
    if (normalizedLevel <= 0.0f) return NAN;
    const float pressureRatio = normalizedLevel * MIC_SENSITIVITY / REFERENCE_SOUND_PRESSURE;
    const float rawDb = 20.0f * log10f(pressureRatio);
    const float newReading = CALIBRATION_GAIN * rawDb + CALIBRATION_OFFSET;
    if (!isfinite(newReading)) return NAN;

    smoothingTotal -= smoothingValues[smoothingIndex];
    smoothingValues[smoothingIndex] = newReading;
    smoothingTotal += newReading;
    smoothingIndex = static_cast<uint8_t>((smoothingIndex + 1) % SMOOTHING_LENGTH);
    if (smoothingCount < SMOOTHING_LENGTH) ++smoothingCount;

    return smoothingTotal / smoothingCount;
}

// Menampilkan ID transmitter dan bacaan mikrofon atau pesan MIC ERROR pada OLED.
void updateDisplay() {
    if (!displayReady) return;

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    display.setTextSize(1);
    display.setCursor(37, 10);
    display.printf("PENGIRIM %u", TRANSMITTER_ID);
    display.drawFastHLine(12, 25, 104, SH110X_WHITE);

    if (!isfinite(latestDb) || millis() - lastValidReadingMs > READING_TIMEOUT_MS) {
        display.setTextSize(1);
        display.setCursor(26, 40);
        display.print("MIC ERROR");
        display.display();
        return;
    }

    char levelText[20];
    snprintf(levelText, sizeof(levelText), "%.1f dB", latestDb);

    display.setTextSize(2);
    display.setCursor(8, 36);
    display.print(levelText);
    display.display();
}

// Mengirim bacaan terbaru ke Receiver hanya jika pengukuran masih valid.
void sendReading() {
    if (!isfinite(latestDb) || millis() - lastValidReadingMs > READING_TIMEOUT_MS) {
        Serial.println("Pengukuran mic tidak tersedia; paket tidak dikirim");
        return;
    }

    SoundPacket packet{latestDb};
    const esp_err_t result = esp_now_send(RECEIVER_MAC,
                                          reinterpret_cast<const uint8_t *>(&packet),
                                          sizeof(packet));
    if (result != ESP_OK) {
        Serial.printf("ESP-NOW send failed: %d\n", result);
    } else {
        Serial.printf("P%u queued: %.1f dB\n", TRANSMITTER_ID, latestDb);
    }
}
} // namespace

// Menyiapkan serial, OLED, ESP-NOW, dan mikrofon saat perangkat dinyalakan.
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("Transmitter_v2 - Pengirim %u\n", TRANSMITTER_ID);

    Wire.begin();
    displayReady = display.begin(OLED_ADDRESS, true);
    if (!displayReady) Serial.println("SH1106 OLED not found; continuing without display");

    if (!initializeEspNow()) {
        Serial.println("Stopped: ESP-NOW setup failed");
        while (true) delay(1000);
    }

    if (!initializeI2S()) {
        Serial.println("Stopped: INMP441/I2S setup failed");
        while (true) delay(1000);
    }

    Serial.println("INMP441 ready. Check calibration before treating values as dBA.");
    lastSendMs = millis();
    lastDisplayMs = millis();
}

// Memperbarui bacaan, layar, dan pengiriman paket sesuai interval masing-masing.
void loop() {
    const float reading = measureDecibels();
    if (isfinite(reading)) {
        latestDb = reading;
        lastValidReadingMs = millis();
    }

    const uint32_t now = millis();
    if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
        lastDisplayMs = now;
        updateDisplay();
    }
    if (now - lastSendMs >= SEND_INTERVAL_MS) {
        lastSendMs = now;
        sendReading();
    }
}
