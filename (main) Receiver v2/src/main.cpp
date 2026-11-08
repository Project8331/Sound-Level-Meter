#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif
#include <ESP_I2S.h>
#include <driver/gpio.h>
#include <math.h>
#include <string.h>

namespace {
// Receiver menggabungkan enam transmitter dan pengukuran mic lokal menjadi tujuh kanal.
constexpr uint8_t SENDER_COUNT = 6;
constexpr uint8_t CHANNEL_COUNT = 7;
constexpr uint32_t MIC_SAMPLE_RATE = 16000;
constexpr size_t MIC_BLOCK_FRAMES = 512;
constexpr uint32_t CYD_SEND_INTERVAL_MS = 1000;
constexpr uint32_t SENDER_TIMEOUT_MS = 5000;
constexpr uint32_t MIC_REPORT_INTERVAL_MS = 500;
constexpr uint8_t ESPNOW_CHANNEL = 1;
constexpr float MIC_REF_DB = 120.0f;
constexpr float MIC_CAL_OFFSET_DB = 0.0f;
constexpr float MIC_FULL_SCALE = 8388607.0f;
constexpr uint8_t MIC_SLOT = 0;

// Isi setiap slot dengan MAC STA transmitter yang benar sebelum menjalankan sistem.
const uint8_t SENDER_MACS[SENDER_COUNT][6] = {
    {0, 0, 0, 0, 0, 0}, // Pengirim 1
    {0, 0, 0, 0, 0, 0}, // Pengirim 2
    {0, 0, 0, 0, 0, 0}, // Pengirim 3
    {0, 0, 0, 0, 0, 0}, // Pengirim 4
    {0, 0, 0, 0, 0, 0}, // Pengirim 5
    {0, 0, 0, 0, 0, 0}, // Pengirim 6
};

// Isi dengan MAC STA CYD yang dicetak pada Serial Monitor saat startup.
const uint8_t CYD_MAC[6] = {0, 0, 0, 0, 0, 0};

// Pin mic I2S mengikuti sketch Receiver lama.
constexpr gpio_num_t I2S_WS_PIN = GPIO_NUM_25;
constexpr gpio_num_t I2S_DATA_PIN = GPIO_NUM_33;
constexpr gpio_num_t I2S_BCLK_PIN = GPIO_NUM_32;

// Format paket agregat yang diterima CYD: enam transmitter diikuti mic Receiver.
struct SoundPacket {
    float db[CHANNEL_COUNT]; // Pengirim 1..6, lalu mic lokal Receiver.
};
static_assert(sizeof(SoundPacket) == sizeof(float) * CHANNEL_COUNT,
              "Format SoundPacket harus sama dengan format paket CYD");

// Filter biquad digital yang menjadi bagian dari implementasi pembobotan frekuensi A.
struct Biquad {
    float b0 = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;

    float process(float sample) {
        const float output = b0 * sample + z1;
        z1 = b1 * sample - a1 * output + z2;
        z2 = b2 * sample - a2 * output;
        return output;
    }

    float magnitude(float frequency, float sampleRate) const {
        const float omega = 2.0f * PI * frequency / sampleRate;
        const float numeratorReal = b0 + b1 * cosf(omega) + b2 * cosf(2.0f * omega);
        const float numeratorImag = -b1 * sinf(omega) - b2 * sinf(2.0f * omega);
        const float denominatorReal = 1.0f + a1 * cosf(omega) + a2 * cosf(2.0f * omega);
        const float denominatorImag = -a1 * sinf(omega) - a2 * sinf(2.0f * omega);
        return sqrtf((numeratorReal * numeratorReal + numeratorImag * numeratorImag) /
                     (denominatorReal * denominatorReal + denominatorImag * denominatorImag));
    }
};

I2SClass i2s;
int32_t micBuffer[MIC_BLOCK_FRAMES * 2];
Biquad aWeighting1;
Biquad aWeighting2;
Biquad aWeighting3;
float aWeightingGain = 1.0f;
float senderDb[SENDER_COUNT] = {NAN, NAN, NAN, NAN, NAN, NAN};
uint32_t senderLastUpdateMillis[SENDER_COUNT] = {};
bool senderReceived[SENDER_COUNT] = {};
float localMicDbA = NAN;
float localMicDbZ = NAN;
double micSum = 0.0;
double micSumSquares = 0.0;
double micSumSquaresA = 0.0;
uint32_t micSampleCount = 0;

portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;
bool espNowReady = false;
bool cydPeerReady = false;
bool microphoneReady = false;
uint32_t lastSendMillis = 0;
uint32_t lastStatusMillis = 0;
uint32_t lastMicReportMillis = 0;

// Memeriksa apakah slot MAC transmitter sudah diisi dengan alamat yang bukan nol.
bool isMacConfigured(const uint8_t *mac) {
    for (uint8_t i = 0; i < 6; ++i) {
        if (mac[i] != 0) return true;
    }
    return false;
}

// Menentukan nomor kanal transmitter berdasarkan MAC sumber paket.
int8_t findSender(const uint8_t *mac) {
    for (uint8_t i = 0; i < SENDER_COUNT; ++i) {
        if (isMacConfigured(SENDER_MACS[i]) && memcmp(mac, SENDER_MACS[i], 6) == 0) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

// Memvalidasi payload satu float dan menyimpan bacaan serta waktu penerimaannya.
void handleSenderPacket(const uint8_t *sourceMac, const uint8_t *payload, int length) {
    if (sourceMac == nullptr || payload == nullptr) return;
    const int8_t source = findSender(sourceMac);
    if (source < 0) {
        Serial.printf("MAC pengirim belum terdaftar: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      sourceMac[0], sourceMac[1], sourceMac[2], sourceMac[3],
                      sourceMac[4], sourceMac[5]);
        return;
    }
    if (length != static_cast<int>(sizeof(float))) {
        Serial.printf("Payload pengirim %d tidak valid: %d byte\n", source + 1, length);
        return;
    }

    float receivedDb = NAN;
    memcpy(&receivedDb, payload, sizeof(receivedDb));
    if (!isfinite(receivedDb)) return;

    portENTER_CRITICAL(&dataMux);
    senderDb[source] = receivedDb;
    senderLastUpdateMillis[source] = millis();
    senderReceived[source] = true;
    portEXIT_CRITICAL(&dataMux);
    Serial.printf("Pengirim %d: %.1f dB\n", source + 1, receivedDb);
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowReceive(const esp_now_recv_info_t *info, const uint8_t *payload, int length) {
    if (info != nullptr) handleSenderPacket(info->src_addr, payload, length);
}
#else
void onEspNowReceive(const uint8_t *sourceMac, const uint8_t *payload, int length) {
    handleSenderPacket(sourceMac, payload, length);
}
#endif

// Menghitung koefisien filter A-weighting dan normalisasi penguatannya pada 1 kHz.
void initializeAWeighting(float sampleRate) {
    const float w1 = 2.0f * PI * 20.598997f;
    const float w2 = 2.0f * PI * 107.65265f;
    const float w3 = 2.0f * PI * 737.86223f;
    const float k = 2.0f * sampleRate;

    const float p1 = (k - w1) / (k + w1);
    float gain = (k * k) / ((k + w1) * (k + w1));
    aWeighting1.b0 = gain;
    aWeighting1.b1 = -2.0f * gain;
    aWeighting1.b2 = gain;
    aWeighting1.a1 = -2.0f * p1;
    aWeighting1.a2 = p1 * p1;

    const float p2 = (k - w2) / (k + w2);
    const float p3 = (k - w3) / (k + w3);
    gain = (k * k) / ((k + w2) * (k + w3));
    aWeighting2.b0 = gain;
    aWeighting2.b1 = -2.0f * gain;
    aWeighting2.b2 = gain;
    aWeighting2.a1 = -(p2 + p3);
    aWeighting2.a2 = p2 * p3;

    constexpr float highFrequencyCorrection = 0.044f;
    aWeighting3.b0 = highFrequencyCorrection;
    aWeighting3.b1 = 1.0f - 2.0f * highFrequencyCorrection;
    aWeighting3.b2 = highFrequencyCorrection;
    aWeightingGain = 1.0f / (aWeighting1.magnitude(1000.0f, sampleRate) *
                             aWeighting2.magnitude(1000.0f, sampleRate) *
                             aWeighting3.magnitude(1000.0f, sampleRate));
}

// Memulai mic INMP441 dengan I2S standar stereo 16 kHz/32-bit.
bool startMicrophone() {
    i2s.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DATA_PIN);
    if (!i2s.begin(I2S_MODE_STD, MIC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("Gagal menginisialisasi I2S");
        return false;
    }
    gpio_pulldown_en(I2S_DATA_PIN);
    const uint32_t warmupStart = millis();
    while (millis() - warmupStart < 600) {
        i2s.readBytes(reinterpret_cast<char *>(micBuffer), sizeof(micBuffer));
    }
    lastMicReportMillis = millis();
    return true;
}

// Mengubah RMS sampel 24-bit menjadi estimasi level dB dengan referensi mic.
float toDb(float rms) {
    const float db = 20.0f * log10f(fmaxf(rms, 1.0f) / MIC_FULL_SCALE) +
                     MIC_REF_DB + MIC_CAL_OFFSET_DB;
    return db < 0.0f ? 0.0f : db;
}

// Menghitung dBA terfilter dan dB(Z) dari blok I2S, lalu menyimpan hasil terbaru.
void updateLocalMicrophone() {
    const size_t bytesRead = i2s.readBytes(reinterpret_cast<char *>(micBuffer), sizeof(micBuffer));
    const size_t framesRead = bytesRead / (2 * sizeof(micBuffer[0]));
    for (size_t i = 0; i < framesRead; ++i) {
        const int32_t raw = micBuffer[2 * i + MIC_SLOT];
        const float sample = static_cast<float>(raw >> 8);
        const float filtered = aWeighting3.process(
            aWeighting2.process(aWeighting1.process(sample))) * aWeightingGain;
        micSum += sample;
        micSumSquares += static_cast<double>(sample) * sample;
        micSumSquaresA += static_cast<double>(filtered) * filtered;
        ++micSampleCount;
    }

    const uint32_t now = millis();
    if (micSampleCount == 0 || now - lastMicReportMillis < MIC_REPORT_INTERVAL_MS) return;

    const double mean = micSum / micSampleCount;
    const double variance = micSumSquares / micSampleCount - mean * mean;
    const float rmsZ = variance > 0.0 ? sqrtf(static_cast<float>(variance)) : 0.0f;
    const float rmsA = sqrtf(static_cast<float>(micSumSquaresA / micSampleCount));
    const float dbA = toDb(rmsA);
    const float dbZ = toDb(rmsZ);

    micSum = 0.0;
    micSumSquares = 0.0;
    micSumSquaresA = 0.0;
    micSampleCount = 0;
    lastMicReportMillis = now;

    portENTER_CRITICAL(&dataMux);
    localMicDbA = dbA;
    localMicDbZ = dbZ;
    portEXIT_CRITICAL(&dataMux);
}

// Membuat paket tujuh float; data transmitter yang kedaluwarsa dikirim sebagai NaN.
void sendAggregateToCyd() {
    if (!espNowReady || !cydPeerReady) return;

    SoundPacket packet;
    const uint32_t now = millis();
    portENTER_CRITICAL(&dataMux);
    for (uint8_t i = 0; i < SENDER_COUNT; ++i) {
        const bool senderIsFresh = senderReceived[i] &&
                                   now - senderLastUpdateMillis[i] <= SENDER_TIMEOUT_MS;
        packet.db[i] = senderIsFresh ? senderDb[i] : NAN;
    }
    packet.db[SENDER_COUNT] = localMicDbA;
    portEXIT_CRITICAL(&dataMux);

    const esp_err_t result = esp_now_send(CYD_MAC,
                                          reinterpret_cast<const uint8_t *>(&packet),
                                          sizeof(packet));
    if (result != ESP_OK) Serial.printf("Gagal mengirim paket ke CYD: %d\n", result);
}

// Mengaktifkan Long Range/channel 1 dan menambahkan CYD sebagai peer ESP-NOW.
void initializeEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_err_t result = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    if (result != ESP_OK) {
        Serial.printf("Gagal mengaktifkan ESP-NOW Long Range: %d\n", result);
        return;
    }
    result = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (result != ESP_OK) {
        Serial.printf("Gagal mengatur channel Wi-Fi %u: %d\n", ESPNOW_CHANNEL, result);
        return;
    }
    Serial.printf("ESP-NOW Long Range aktif, channel %u\n", ESPNOW_CHANNEL);
    Serial.print("MAC Receiver: ");
    Serial.println(WiFi.macAddress());

    if (esp_now_init() != ESP_OK) {
        Serial.println("Gagal inisialisasi ESP-NOW");
        return;
    }
    espNowReady = true;
    esp_now_register_recv_cb(onEspNowReceive);

    esp_now_peer_info_t cydPeer = {};
    memcpy(cydPeer.peer_addr, CYD_MAC, sizeof(CYD_MAC));
    cydPeer.channel = ESPNOW_CHANNEL;
    cydPeer.encrypt = false;
    const esp_err_t peerResult = esp_now_add_peer(&cydPeer);
    cydPeerReady = peerResult == ESP_OK || peerResult == ESP_ERR_ESPNOW_EXIST;
    if (!cydPeerReady) Serial.printf("Gagal menambahkan peer CYD: %d\n", peerResult);
    else Serial.println("Peer CYD siap; paket agregat akan dikirim setiap detik");
}
} // namespace

// Memulai serial, filter audio, mikrofon, dan ESP-NOW.
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("Receiver v2: ESP-NOW Long Range + mic lokal");

    initializeAWeighting(static_cast<float>(MIC_SAMPLE_RATE));
    microphoneReady = startMicrophone();
    if (!microphoneReady) Serial.println("Mic I2S gagal diinisialisasi");
    initializeEspNow();
}

// Memperbarui pembacaan mic, mengirim agregat tiap detik, dan mencetak status berkala.
void loop() {
    if (microphoneReady) updateLocalMicrophone();

    const uint32_t now = millis();
    if (now - lastSendMillis >= CYD_SEND_INTERVAL_MS) {
        lastSendMillis = now;
        sendAggregateToCyd();
    }

    if (now - lastStatusMillis >= 5000) {
        lastStatusMillis = now;
        float statusValues[CHANNEL_COUNT];
        const uint32_t statusNow = millis();
        portENTER_CRITICAL(&dataMux);
        for (uint8_t i = 0; i < SENDER_COUNT; ++i) {
            const bool senderIsFresh = senderReceived[i] &&
                                       statusNow - senderLastUpdateMillis[i] <= SENDER_TIMEOUT_MS;
            statusValues[i] = senderIsFresh ? senderDb[i] : NAN;
        }
        statusValues[SENDER_COUNT] = localMicDbA;
        const float statusDbZ = localMicDbZ;
        portEXIT_CRITICAL(&dataMux);
        Serial.printf("Status: P1 %.1f, P2 %.1f, P3 %.1f, P4 %.1f, P5 %.1f, P6 %.1f, Receiver %.1f dBA / %.1f dB(Z)\n",
                      statusValues[0], statusValues[1], statusValues[2], statusValues[3],
                      statusValues[4], statusValues[5], statusValues[6], statusDbZ);
    }
}
