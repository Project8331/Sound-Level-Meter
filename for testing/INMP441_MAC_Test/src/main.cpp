#include <Arduino.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <math.h>

// Pin mengikuti konfigurasi mic pada Receiver_v2.
// Konfigurasi pin INMP441, format audio, dan interval pelaporan level sinyal.
constexpr gpio_num_t I2S_BCLK_PIN = GPIO_NUM_32;
constexpr gpio_num_t I2S_WS_PIN = GPIO_NUM_25;
constexpr gpio_num_t I2S_DATA_PIN = GPIO_NUM_33;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t SAMPLE_COUNT = 256;
constexpr uint32_t REPORT_INTERVAL_MS = 1000;
constexpr float FULL_SCALE_24BIT = 8388608.0f;

// Buffer sampel I2S dan akumulator RMS/puncak untuk satu periode laporan.
int32_t sampleBuffer[SAMPLE_COUNT];
double accumulatedSquares = 0.0;
uint64_t accumulatedSamples = 0;
float peakMagnitude = 0.0f;
uint32_t lastReportMs = 0;

// Memasang driver I2S dan menghubungkan pin clock serta data mikrofon.
bool startMicrophone() {
    const i2s_config_t config = {
        .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = SAMPLE_COUNT,
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

    i2s_zero_dma_buffer(I2S_PORT);
    return true;
}

// Membaca satu blok sampel 32-bit; data INMP441 24-bit berada di bagian atas slot.
void readMicrophoneBlock() {
    size_t bytesRead = 0;
    const esp_err_t result = i2s_read(I2S_PORT, sampleBuffer, sizeof(sampleBuffer),
                                      &bytesRead, pdMS_TO_TICKS(100));
    if (result != ESP_OK || bytesRead < sizeof(int32_t)) return;

    const size_t count = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < count; ++i) {
        // INMP441 outputs 24-bit signed audio in a 32-bit I2S slot.
        const int32_t sample = sampleBuffer[i] >> 8;
        const float magnitude = fabsf(static_cast<float>(sample));
        accumulatedSquares += static_cast<double>(sample) * sample;
        if (magnitude > peakMagnitude) peakMagnitude = magnitude;
        ++accumulatedSamples;
    }
}

// Menghitung RMS dan puncak dalam dBFS, lalu mengosongkan akumulator periode.
void reportMicrophone() {
    if (accumulatedSamples == 0) {
        Serial.println("No I2S samples; check BCLK, WS, SD, and L/R wiring.");
        return;
    }

    const float rms = sqrtf(static_cast<float>(accumulatedSquares / accumulatedSamples));
    const float rmsDbfs = 20.0f * log10f(fmaxf(rms / FULL_SCALE_24BIT, 1e-9f));
    const float peakDbfs = 20.0f * log10f(fmaxf(peakMagnitude / FULL_SCALE_24BIT, 1e-9f));

    Serial.printf("Samples: %llu | RMS: %.0f | Peak: %.0f | RMS: %.1f dBFS | Peak: %.1f dBFS\n",
                  static_cast<unsigned long long>(accumulatedSamples), rms, peakMagnitude,
                  rmsDbfs, peakDbfs);

    accumulatedSquares = 0.0;
    accumulatedSamples = 0;
    peakMagnitude = 0.0f;
}

// Menampilkan MAC ESP32 dan memulai komunikasi I2S untuk pengujian mikrofon.
void setup() {
    Serial.begin(115200);
    delay(300);

    WiFi.mode(WIFI_STA);
    Serial.print("ESP32 STA MAC: ");
    Serial.println(WiFi.macAddress());

    Serial.println("Starting INMP441 I2S test...");
    if (!startMicrophone()) {
        Serial.println("Microphone initialization failed; stopping.");
        while (true) delay(1000);
    }

    Serial.println("INMP441 ready. Speak or tap near the microphone.");
    Serial.println("Note: dBFS is a digital signal level, not calibrated acoustic dB SPL.");
    lastReportMs = millis();
}

// Mengumpulkan sampel terus-menerus dan melaporkan hasil sesuai interval.
void loop() {
    readMicrophoneBlock();

    const uint32_t now = millis();
    if (now - lastReportMs >= REPORT_INTERVAL_MS) {
        lastReportMs = now;
        reportMicrophone();
    }
}
