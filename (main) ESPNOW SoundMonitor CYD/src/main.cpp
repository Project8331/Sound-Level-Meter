#include <Arduino.h>
#include <Network.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <RTClib.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {
// Ukuran layar, warna antarmuka, jumlah sumber, dan channel ESP-NOW CYD.
constexpr uint16_t SCREEN_W = 320;
constexpr uint16_t SCREEN_H = 240;
constexpr uint32_t UI_BACKGROUND = 0x080D14;
constexpr uint32_t UI_CARD = 0x111A26;
constexpr uint32_t UI_CARD_PRESSED = 0x192638;
constexpr uint32_t UI_BORDER = 0x27364A;
constexpr uint32_t UI_CHART = 0x0C1420;
constexpr uint32_t UI_ACCENT = 0x55D6C2;
constexpr uint32_t UI_TEXT = 0xEAF1F8;
constexpr uint32_t UI_MUTED = 0x91A2B5;
constexpr uint8_t SOURCE_COUNT = 7;
constexpr uint8_t ESPNOW_WIFI_CHANNEL = 1;

constexpr uint8_t HISTORY_LENGTH = 24;
constexpr uint8_t EVENT_QUEUE_LENGTH = 16;
constexpr uint8_t STATUS_ITEM_COUNT = 10;
constexpr uint32_t SOURCE_ONLINE_TIMEOUT_MS = 5000;
constexpr uint32_t LOG_ROW_IDLE_TIMEOUT_MS = 150;
constexpr float DISPLAY_DB_MAX = 120.0f;
constexpr float HUMAN_REFERENCE_DB = 85.0f; // Acuan paparan bising 85 dBA selama 8 jam.
constexpr bool SET_RTC_FROM_BUILD_TIME_IF_LOST = true;
constexpr uint8_t SD_CS_PIN = 5;
constexpr uint8_t SD_SCK_PIN = 18;
constexpr uint8_t SD_MISO_PIN = 19;
constexpr uint8_t SD_MOSI_PIN = 23;
constexpr uint8_t TOUCH_IRQ_PIN = 36;
constexpr uint8_t TOUCH_MOSI_PIN = 32;
constexpr uint8_t TOUCH_MISO_PIN = 39;
constexpr uint8_t TOUCH_CLK_PIN = 25;
constexpr uint8_t TOUCH_CS_PIN = 33;
constexpr uint8_t RTC_SDA_PIN = 22;
constexpr uint8_t RTC_SCL_PIN = 27;
constexpr char LOG_PATH[] = "/noise.csv";
constexpr char ALTERNATE_LOG_PATH[] = "/noise_wide.csv";

// Isi MAC semua enam pengirim dan node receiver pada SOURCE_MACS.
// Isi dengan MAC STA keenam transmitter dan Receiver agregator; MAC nol diabaikan.
// MAC sumber menentukan kartu yang menerima nilai; slot terakhir adalah Receiver agregator.
const uint8_t SOURCE_MACS[SOURCE_COUNT][6] = {
    {0, 0, 0, 0, 0, 0}, // Pengirim 1
    {0, 0, 0, 0, 0, 0}, // Pengirim 2
    {0, 0, 0, 0, 0, 0}, // Pengirim 3
    {0, 0, 0, 0, 0, 0}, // Pengirim 4
    {0, 0, 0, 0, 0, 0}, // Pengirim 5
    {0, 0, 0, 0, 0, 0}, // Pengirim 6
    {0, 0, 0, 0, 0, 0}, // Receiver aggregator
};

const char *const SOURCE_NAMES[SOURCE_COUNT] = {
    "Transmitter 1", "Transmitter 2", "Transmitter 3", "Transmitter 4",
    "Transmitter 5", "Transmitter 6", "Receiver"
};

// Menyimpan bacaan terbaru, nilai maksimum, dan timestamp untuk setiap kanal tampilan.
struct Sample {
    float db = NAN;
    float maxDb = NAN;
    uint32_t receivedMillis = 0;
    char timestamp[20] = "--";
    char maxTimestamp[20] = "--";
};

// Event ringkas dari callback ESP-NOW agar paket diproses di loop utama dengan aman.
struct ReceiveEvent {
    uint8_t source;
    float db;
    uint32_t receivedMillis;
    bool valid;
};

// Menampung bacaan berdekatan agar beberapa kanal dapat ditulis dalam satu baris CSV.
struct PendingLogRow {
    float db[SOURCE_COUNT] = {};
    bool hasSample[SOURCE_COUNT] = {};
    char timestamp[20] = "";
    uint32_t lastUpdateMillis = 0;
    bool active = false;
};

TFT_eSPI tft(SCREEN_W, SCREEN_H);
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
RTC_DS3231 rtc;

lv_color_t drawBuffer[SCREEN_W * 6];
lv_display_t *display = nullptr;
lv_obj_t *splashScreen = nullptr;
lv_obj_t *homeScreen = nullptr;
lv_obj_t *detailScreen = nullptr;
lv_obj_t *splashBar = nullptr;
lv_obj_t *statusScreen = nullptr;
lv_obj_t *detailTitle = nullptr;
lv_obj_t *detailValue = nullptr;
lv_obj_t *detailMax = nullptr;
lv_obj_t *detailLimit = nullptr;
lv_obj_t *detailClock = nullptr;
lv_obj_t *detailLog = nullptr;
lv_obj_t *detailChart = nullptr;
lv_chart_series_t *detailSeries = nullptr;
lv_obj_t *homeValues[SOURCE_COUNT] = {};
lv_obj_t *homeArcs[SOURCE_COUNT] = {};
lv_obj_t *homeCards[SOURCE_COUNT] = {};
lv_obj_t *homeIds[SOURCE_COUNT] = {};
lv_obj_t *statusLabels[STATUS_ITEM_COUNT] = {};
lv_obj_t *statusCard = nullptr;
lv_timer_t *splashTimer = nullptr;

Sample samples[SOURCE_COUNT];
ReceiveEvent eventQueue[EVENT_QUEUE_LENGTH];
PendingLogRow pendingLogRow;
bool sourceNameWritten[SOURCE_COUNT] = {};
volatile uint8_t queueHead = 0;
volatile uint8_t queueTail = 0;
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t activeSource = 0;
uint8_t splashProgress = 0;
bool sdReady = false;
bool espNowReady = false;
bool rtcReady = false;
bool rtcTimeValid = false;
uint32_t lastUiRefresh = 0;
const char *activeLogPath = LOG_PATH;
enum class SharedSpiRoute : uint8_t { Touch, SdCard };
SharedSpiRoute sharedSpiRoute = SharedSpiRoute::Touch;

// Mengalihkan bus SPI bersama ke pin touch sebelum membaca layar sentuh.
void useTouchSpiPins() {
    if (sharedSpiRoute == SharedSpiRoute::Touch) return;
    touchSpi.end();
    touchSpi.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
    sharedSpiRoute = SharedSpiRoute::Touch;
}

// Mengalihkan bus SPI bersama ke pin microSD sebelum operasi file.
void useSdSpiPins() {
    if (sharedSpiRoute == SharedSpiRoute::SdCard) return;
    touchSpi.end();
    touchSpi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    sharedSpiRoute = SharedSpiRoute::SdCard;
}

lv_color_t colorForDb(float db) {
    if (!isfinite(db)) return lv_color_hex(0x526070);
    if (db < 50.0f) return lv_color_hex(0x168BFF); // biru
    if (db < 65.0f) return lv_color_hex(0x27C878); // hijau
    if (db < 80.0f) return lv_color_hex(0xF2C230); // kuning
    if (db < 95.0f) return lv_color_hex(0xF04444); // merah
    return lv_color_hex(0x050505);                 // hitam
}

// Mengirim area gambar LVGL yang berubah ke layar TFT.
void displayFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelMap) {
    const uint32_t width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
    const uint32_t height = static_cast<uint32_t>(area->y2 - area->y1 + 1);
    if (LV_COLOR_16_SWAP) {
        lv_draw_sw_rgb565_swap(pixelMap, lv_area_get_size(area));
    }
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, width, height);
    tft.pushColors(reinterpret_cast<uint16_t *>(pixelMap), width * height, true);
    tft.endWrite();
    lv_display_flush_ready(disp);
}

// Membaca koordinat touch dan menyerahkannya ke sistem input LVGL.
void touchRead(lv_indev_t *, lv_indev_data_t *data) {
    useTouchSpiPins();
    if (!touch.touched()) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    const TS_Point point = touch.getPoint();
    const int32_t mappedX = constrain(map(point.x, 200, 3700, 0, SCREEN_W - 1), 0, SCREEN_W - 1);
    const int32_t mappedY = constrain(map(point.y, 240, 3800, 0, SCREEN_H - 1), 0, SCREEN_H - 1);
    data->point.x = static_cast<int16_t>(SCREEN_W - 1 - mappedX);
    data->point.y = static_cast<int16_t>(SCREEN_H - 1 - mappedY);
    data->state = LV_INDEV_STATE_PRESSED;
}

uint32_t lvglTick() { return millis(); }

bool macIsConfigured(const uint8_t *mac) {
    for (uint8_t i = 0; i < 6; ++i) if (mac[i] != 0) return true;
    return false;
}

int8_t sourceIndexForMac(const uint8_t *mac) {
    for (uint8_t source = 0; source < SOURCE_COUNT; ++source) {
        if (macIsConfigured(SOURCE_MACS[source]) && memcmp(mac, SOURCE_MACS[source], 6) == 0) {
            return static_cast<int8_t>(source);
        }
    }
    return -1;
}

void enqueueReceiveEvent(uint8_t source, float db, uint32_t receivedMillis, bool valid = true) {
    if (valid && !isfinite(db)) return;
    portENTER_CRITICAL(&queueMux);
    const uint8_t nextHead = static_cast<uint8_t>((queueHead + 1) % EVENT_QUEUE_LENGTH);
    if (nextHead != queueTail) {
        eventQueue[queueHead] = {source, db, receivedMillis, valid};
        queueHead = nextHead;
    }
    portEXIT_CRITICAL(&queueMux);
}

// Menerima payload langsung satu float atau paket agregat tujuh float dari Receiver.
void handleEspNowPayload(const uint8_t *sourceMac, const uint8_t *payload, int length) {
    if (sourceMac == nullptr || payload == nullptr) return;
    const int8_t source = sourceIndexForMac(sourceMac);
    if (source < 0) return;
    const uint32_t receivedMillis = millis();

    // Accept one float per configured node, or seven floats aggregated by the
    // receiver node (last float is its local microphone).
    if (length == static_cast<int>(sizeof(float))) {
        float db = NAN;
        memcpy(&db, payload, sizeof(db));
        if (!isfinite(db)) return;
        enqueueReceiveEvent(static_cast<uint8_t>(source), db, receivedMillis);
        return;
    }

    if (source == SOURCE_COUNT - 1 && length == static_cast<int>(sizeof(float) * SOURCE_COUNT)) {
        float values[SOURCE_COUNT];
        memcpy(values, payload, sizeof(values));
        for (uint8_t i = 0; i < SOURCE_COUNT; ++i) {
            enqueueReceiveEvent(i, values[i], receivedMillis, isfinite(values[i]));
        }
    }
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowReceive(const esp_now_recv_info_t *info, const uint8_t *payload, int length) {
    if (info != nullptr) handleEspNowPayload(info->src_addr, payload, length);
}
#else
void onEspNowReceive(const uint8_t *sourceMac, const uint8_t *payload, int length) {
    handleEspNowPayload(sourceMac, payload, length);
}
#endif

// Membuat timestamp kalender dari RTC, atau penanda RTC_UNSET berbasis uptime.
void makeTimestamp(char *out, size_t length, uint32_t fallbackMillis) {
    if (rtcReady && rtcTimeValid) {
        const DateTime now = rtc.now();
        snprintf(out, length, "%04u-%02u-%02u %02u:%02u:%02u",
                 static_cast<unsigned>(now.year()), static_cast<unsigned>(now.month()),
                 static_cast<unsigned>(now.day()), static_cast<unsigned>(now.hour()),
                 static_cast<unsigned>(now.minute()), static_cast<unsigned>(now.second()));
    } else {
        snprintf(out, length, "RTC_UNSET+%lus", static_cast<unsigned long>(fallbackMillis / 1000));
    }
}

bool hasWideCsvHeader(const char *path) {
    File file = SD.open(path, FILE_READ);
    if (!file) return false;
    char header[128] = {};
    size_t headerLength = file.readBytesUntil('\n', header, sizeof(header) - 1);
    file.close();
    while (headerLength > 0 &&
           (header[headerLength - 1] == '\r' || header[headerLength - 1] == '\n')) {
        header[--headerLength] = '\0';
    }
    return strcmp(header, "DATA 1,,,,DATA 2,,,,DATA 3,,,,DATA 4,,,,DATA 5,,,,DATA 6,,,,DATA 7,,") == 0;
}

bool writeWideCsvHeader(File &file) {
    bool ok = true;
    for (uint8_t column = 0; column < SOURCE_COUNT * 4 - 1; ++column) {
        if (column > 0) ok = file.print(',') > 0 && ok;
        if (column % 4 == 0) ok = file.printf("DATA %u", static_cast<unsigned>(column / 4 + 1)) > 0 && ok;
    }
    ok = file.println() > 0 && ok;

    for (uint8_t column = 0; column < SOURCE_COUNT * 4 - 1; ++column) {
        if (column > 0) ok = file.print(',') > 0 && ok;
        const uint8_t field = column % 4;
        if (field == 0) ok = file.print("timestamp") > 0 && ok;
        else if (field == 1) ok = file.print("source") > 0 && ok;
        else if (field == 2) ok = file.print("db") > 0 && ok;
    }
    return file.println() > 0 && ok;
}

// Menulis satu baris CSV dari sampel yang sudah dikumpulkan untuk beberapa sumber.
void flushPendingLogRow() {
    if (!sdReady || !pendingLogRow.active) return;
    useSdSpiPins();
    File file = SD.open(activeLogPath, FILE_APPEND);
    if (!file) {
        Serial.printf("SD: gagal membuka %s untuk append\n", activeLogPath);
        pendingLogRow.active = false;
        return;
    }

    bool ok = true;
    for (uint8_t column = 0; column < SOURCE_COUNT * 4 - 1; ++column) {
        if (column > 0) ok = file.print(',') > 0 && ok;
        const uint8_t source = column / 4;
        const uint8_t field = column % 4;
        if (source < SOURCE_COUNT && pendingLogRow.hasSample[source]) {
            if (field == 0) ok = file.print(pendingLogRow.timestamp) > 0 && ok;
            else if (field == 1 && !sourceNameWritten[source]) {
                ok = file.print(SOURCE_NAMES[source]) > 0 && ok;
            } else if (field == 2) {
                ok = file.printf("%.1f", pendingLogRow.db[source]) > 0 && ok;
            }
        }
    }
    ok = file.println() > 0 && ok;
    file.close();
    if (ok) {
        for (uint8_t source = 0; source < SOURCE_COUNT; ++source) {
            if (pendingLogRow.hasSample[source]) sourceNameWritten[source] = true;
        }
    } else {
        Serial.printf("SD: gagal menulis baris CSV ke %s\n", activeLogPath);
    }
    pendingLogRow.active = false;
}

// Menambahkan sampel ke baris CSV tertunda dan menggabungkannya bila timestamp berdekatan.
void appendLog(uint8_t source, float db, const char *timestamp, uint32_t receivedMillis) {
    if (!sdReady || source >= SOURCE_COUNT) return;
    if (pendingLogRow.active && strcmp(pendingLogRow.timestamp, timestamp) != 0) {
        flushPendingLogRow();
    }
    if (!pendingLogRow.active) {
        strlcpy(pendingLogRow.timestamp, timestamp, sizeof(pendingLogRow.timestamp));
        pendingLogRow.active = true;
    }
    pendingLogRow.db[source] = db;
    pendingLogRow.hasSample[source] = true;
    pendingLogRow.lastUpdateMillis = receivedMillis;
}


// Detail-page ring buffers, one rolling 24-point dB history per source.
float chartValues[SOURCE_COUNT][HISTORY_LENGTH];
char chartTimes[SOURCE_COUNT][HISTORY_LENGTH][9];
uint8_t chartWriteIndex[SOURCE_COUNT] = {};
uint8_t chartCount[SOURCE_COUNT] = {};

void refreshDetailChart() {
    if (detailChart == nullptr || detailSeries == nullptr) return;
    for (uint8_t point = 0; point < HISTORY_LENGTH; ++point) {
        lv_chart_set_value_by_id(detailChart, detailSeries, point, LV_CHART_POINT_NONE);
    }
    const uint8_t count = chartCount[activeSource];
    const uint8_t first = static_cast<uint8_t>((chartWriteIndex[activeSource] + HISTORY_LENGTH - count) % HISTORY_LENGTH);
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t index = static_cast<uint8_t>((first + i) % HISTORY_LENGTH);
        lv_chart_set_value_by_id(detailChart, detailSeries, i,
                                 static_cast<lv_coord_t>(lroundf(chartValues[activeSource][index])));
    }
    lv_chart_refresh(detailChart);
}

void refreshDetailText() {
    if (detailTitle == nullptr) return;
    const Sample &sample = samples[activeSource];
    char sourceTitle[24];
    snprintf(sourceTitle, sizeof(sourceTitle), "Pengirim %u", static_cast<unsigned>(activeSource + 1));
    lv_label_set_text(detailTitle, sourceTitle);
    char valueText[32];
    if (isfinite(sample.db)) snprintf(valueText, sizeof(valueText), "%.1f dB", sample.db);
    else snprintf(valueText, sizeof(valueText), "--.- dB");
    lv_label_set_text(detailValue, valueText);

    char maxText[72];
    if (isfinite(sample.maxDb)) {
        const char *maxTime = strlen(sample.maxTimestamp) >= 8 ? sample.maxTimestamp + strlen(sample.maxTimestamp) - 8 : sample.maxTimestamp;
        snprintf(maxText, sizeof(maxText), "Maks: %.1f dB\n%s", sample.maxDb, maxTime);
    } else snprintf(maxText, sizeof(maxText), "Maksimum:\\nmenunggu data");
    lv_label_set_text(detailMax, maxText);

    char limitText[64];
    snprintf(limitText, sizeof(limitText), "Acuan: %.0f dBA / 8 jam | RTC %s",
             HUMAN_REFERENCE_DB, rtcTimeValid ? "READY" : "ERROR");
    lv_label_set_text(detailLimit, limitText);
    char clockText[52];
    snprintf(clockText, sizeof(clockText), "Terakhir diterima: %s", sample.timestamp);
    lv_label_set_text(detailClock, clockText);

    char logText[120] = "Sampel terbaru:\n";
    const uint8_t count = chartCount[activeSource];
    const uint8_t first = static_cast<uint8_t>((chartWriteIndex[activeSource] + HISTORY_LENGTH - count) % HISTORY_LENGTH);
    const uint8_t rows = count < 2 ? count : 2;
    for (uint8_t row = 0; row < rows; ++row) {
        const uint8_t index = static_cast<uint8_t>((first + count - rows + row) % HISTORY_LENGTH);
        char line[42];
        snprintf(line, sizeof(line), "%s  %.1f dB\n", chartTimes[activeSource][index], chartValues[activeSource][index]);
        strlcat(logText, line, sizeof(logText));
    }
    lv_label_set_text(detailLog, logText);
    refreshDetailChart();
}

void updateHomeCard(uint8_t source) {
    if (homeValues[source] == nullptr) return;
    const Sample &sample = samples[source];
    char valueText[20];
    if (isfinite(sample.db)) snprintf(valueText, sizeof(valueText), "%.1f dB", sample.db);
    else snprintf(valueText, sizeof(valueText), "--.- dB");
    lv_label_set_text(homeValues[source], valueText);

    const int32_t gaugeValue = isfinite(sample.db) ? static_cast<int32_t>(constrain(sample.db, 0.0f, DISPLAY_DB_MAX)) : 0;
    lv_arc_set_value(homeArcs[source], gaugeValue);
    const lv_color_t levelColor = colorForDb(sample.db);
    lv_obj_set_style_arc_color(homeArcs[source], levelColor, LV_PART_INDICATOR);
}

void updateStatusPage();

void showDetail(lv_event_t *event) {
    const uintptr_t encodedSource = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (encodedSource == 0 || encodedSource > SOURCE_COUNT) return;
    activeSource = static_cast<uint8_t>(encodedSource - 1);
    refreshDetailText();
    lv_screen_load_anim(detailScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

void showHome(lv_event_t *) {
    lv_screen_load_anim(homeScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
}

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, int x, int y, int width, int height,
                    uint32_t color = 0xE8EDF4, const lv_font_t *font = &lv_font_montserrat_14) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

void styleScreen(lv_obj_t *screen) {
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
}

void stylePanel(lv_obj_t *panel) {
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_outline_width(panel, 0, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 9, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
}

void styleNavigationButton(lv_obj_t *button) {
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_outline_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(button, LV_SCROLLBAR_MODE_OFF);
}

void buildSplash() {
    splashScreen = lv_obj_create(nullptr);
    styleScreen(splashScreen);
    lv_obj_t *title = lv_label_create(splashScreen);
    lv_label_set_text(title, "SOUND MONITOR");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -38);
    lv_obj_t *accentLine = lv_obj_create(splashScreen);
    lv_obj_set_size(accentLine, 42, 3);
    lv_obj_align(accentLine, LV_ALIGN_CENTER, 0, -17);
    lv_obj_set_style_bg_color(accentLine, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_bg_opa(accentLine, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(accentLine, 0, 0);
    lv_obj_set_style_radius(accentLine, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *subtitle = lv_label_create(splashScreen);
    lv_label_set_text(subtitle, "ESP-NOW  |  CYD 320x240");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(UI_MUTED), 0);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 2);
    splashBar = lv_bar_create(splashScreen);
    lv_obj_set_size(splashBar, 216, 7);
    lv_obj_align(splashBar, LV_ALIGN_CENTER, 0, 39);
    lv_obj_set_style_bg_color(splashBar, lv_color_hex(UI_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(splashBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(splashBar, lv_color_hex(UI_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(splashBar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(splashBar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(splashBar, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(splashBar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(splashBar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(splashBar, 0, 100);
    lv_bar_set_value(splashBar, 0, LV_ANIM_OFF);
}

void showStatusScreen(lv_event_t *) {
    updateStatusPage();
    lv_screen_load_anim(statusScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

// Membuat homescreen dengan tujuh kartu nilai dan gauge kebisingan.
void buildHome() {
    homeScreen = lv_obj_create(nullptr);
    styleScreen(homeScreen);

    lv_obj_t *heading = makeLabel(homeScreen, "KEBISINGAN (dB)", 0, 6, SCREEN_W, 20, UI_TEXT, &lv_font_montserrat_14);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *headerAccent = lv_obj_create(homeScreen);
    lv_obj_set_size(headerAccent, 34, 2);
    lv_obj_align(headerAccent, LV_ALIGN_TOP_MID, 0, 29);
    lv_obj_set_style_bg_color(headerAccent, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_bg_opa(headerAccent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(headerAccent, 0, 0);
    lv_obj_set_style_radius(headerAccent, LV_RADIUS_CIRCLE, 0);

    for (uint8_t source = 0; source < SOURCE_COUNT; ++source) {
        const uint8_t col = source % 2;
        const uint8_t row = source / 2;
        const int x = 7 + col * 157;
        const int y = 39 + row * 43;
        homeCards[source] = lv_obj_create(homeScreen);
        lv_obj_set_pos(homeCards[source], x, y);
        lv_obj_set_size(homeCards[source], 150, 40);
        lv_obj_add_flag(homeCards[source], LV_OBJ_FLAG_CLICKABLE);
        stylePanel(homeCards[source]);
        lv_obj_add_event_cb(homeCards[source], showDetail, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(source + 1)));

        homeValues[source] = lv_label_create(homeCards[source]);
        lv_label_set_text(homeValues[source], "--.- dB");
        lv_obj_set_size(homeValues[source], 100, 20);
        lv_obj_align(homeValues[source], LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_set_style_text_color(homeValues[source], lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_text_font(homeValues[source], &lv_font_montserrat_14, 0);
        lv_obj_add_flag(homeValues[source], LV_OBJ_FLAG_EVENT_BUBBLE);

        homeArcs[source] = lv_arc_create(homeCards[source]);
        lv_obj_set_size(homeArcs[source], 32, 32);
        lv_obj_align(homeArcs[source], LV_ALIGN_RIGHT_MID, -3, 0);
        lv_arc_set_range(homeArcs[source], 0, static_cast<int32_t>(DISPLAY_DB_MAX));
        lv_arc_set_bg_angles(homeArcs[source], 135, 405);
        lv_arc_set_value(homeArcs[source], 0);
        lv_obj_set_style_bg_opa(homeArcs[source], LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_border_opa(homeArcs[source], LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_arc_width(homeArcs[source], 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(homeArcs[source], lv_color_hex(0x29384A), LV_PART_MAIN);
        lv_obj_set_style_arc_width(homeArcs[source], 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(homeArcs[source], colorForDb(NAN), LV_PART_INDICATOR);
        lv_obj_add_flag(homeArcs[source], LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(homeArcs[source], LV_OBJ_FLAG_CLICKABLE);

        char idText[4];
        snprintf(idText, sizeof(idText), "%u", static_cast<unsigned>(source + 1));
        homeIds[source] = lv_label_create(homeCards[source]);
        lv_label_set_text(homeIds[source], idText);
        lv_obj_set_size(homeIds[source], 28, 18);
        lv_obj_align(homeIds[source], LV_ALIGN_RIGHT_MID, -5, 0);
        lv_obj_set_style_text_color(homeIds[source], lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_text_font(homeIds[source], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(homeIds[source], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(homeIds[source], LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    const int statusY = 39 + 3 * 43;
    statusCard = lv_obj_create(homeScreen);
    lv_obj_set_pos(statusCard, 164, statusY);
    lv_obj_set_size(statusCard, 150, 40);
    stylePanel(statusCard);
    lv_obj_add_flag(statusCard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(statusCard, showStatusScreen, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *statusText = lv_label_create(statusCard);
    lv_label_set_text(statusText, "Status");
    lv_obj_set_style_text_color(statusText, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(statusText, &lv_font_montserrat_14, 0);
    lv_obj_center(statusText);
    lv_obj_add_flag(statusText, LV_OBJ_FLAG_EVENT_BUBBLE);

}

// Membuat halaman detail kanal dengan nilai, maksimum, chart, dan catatan terbaru.
void buildDetail() {
    detailScreen = lv_obj_create(nullptr);
    styleScreen(detailScreen);

    lv_obj_t *back = lv_button_create(detailScreen);
    lv_obj_set_pos(back, 8, 6);
    lv_obj_set_size(back, 40, 28);
    styleNavigationButton(back);
    lv_obj_add_event_cb(back, showHome, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backText = lv_label_create(back);
    lv_label_set_text(backText, "<<");
    lv_obj_set_style_text_color(backText, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_text_font(backText, &lv_font_montserrat_14, 0);
    lv_obj_center(backText);

    detailTitle = makeLabel(detailScreen, "Pengirim 1", 54, 11, 258, 18, UI_TEXT, &lv_font_montserrat_14);
    lv_obj_set_style_text_align(detailTitle, LV_TEXT_ALIGN_CENTER, 0);

    detailValue = makeLabel(detailScreen, "--.- dB", 10, 42, 138, 20, UI_TEXT, &lv_font_montserrat_12);
    detailMax = makeLabel(detailScreen, "Maksimum:\nmenunggu data", 160, 38, 152, 34, 0xF2C968, &lv_font_montserrat_12);
    detailLimit = makeLabel(detailScreen, "Acuan: 85 dBA / 8 jam", 10, 76, 300, 17, UI_MUTED, &lv_font_montserrat_14);

    detailChart = lv_chart_create(detailScreen);
    lv_obj_set_pos(detailChart, 8, 97);
    lv_obj_set_size(detailChart, 304, 72);
    lv_chart_set_type(detailChart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(detailChart, HISTORY_LENGTH);
    lv_chart_set_range(detailChart, LV_CHART_AXIS_PRIMARY_Y, 0, 120);
    lv_chart_set_div_line_count(detailChart, 4, 5);
    lv_obj_set_style_bg_color(detailChart, lv_color_hex(UI_CHART), 0);
    lv_obj_set_style_bg_opa(detailChart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(detailChart, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_border_width(detailChart, 1, 0);
    lv_obj_set_style_radius(detailChart, 9, 0);
    lv_obj_set_style_line_color(detailChart, lv_color_hex(0x253447), LV_PART_MAIN);
    lv_obj_set_style_line_width(detailChart, 1, LV_PART_MAIN);
    detailSeries = lv_chart_add_series(detailChart, lv_color_hex(UI_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(detailChart, detailSeries, LV_CHART_POINT_NONE);

    detailClock = makeLabel(detailScreen, "Terakhir diterima: --", 10, 173, 300, 17, UI_MUTED, &lv_font_montserrat_14);
    detailLog = makeLabel(detailScreen, "Sampel terbaru:\n--", 10, 193, 300, 45, UI_TEXT, &lv_font_montserrat_14);
}

// Membuat halaman status koneksi ESP-NOW, RTC, SD, dan sumber data.
void buildStatus() {
    statusScreen = lv_obj_create(nullptr);
    styleScreen(statusScreen);

    lv_obj_t *back = lv_button_create(statusScreen);
    lv_obj_set_pos(back, 8, 6);
    lv_obj_set_size(back, 40, 28);
    styleNavigationButton(back);
    lv_obj_add_event_cb(back, showHome, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backText = lv_label_create(back);
    lv_label_set_text(backText, "<<");
    lv_obj_set_style_text_color(backText, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_text_font(backText, &lv_font_montserrat_14, 0);
    lv_obj_center(backText);

    lv_obj_t *heading = makeLabel(statusScreen, "STATUS SISTEM", 54, 11, 258, 18, UI_TEXT, &lv_font_montserrat_14);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);

    for (uint8_t i = 0; i < STATUS_ITEM_COUNT; ++i) {
        const uint8_t column = i < 5 ? 0 : 1;
        const uint8_t row = i % 5;
        const int x = column == 0 ? 9 : 163;
        const int y = 48 + row * 34;
        statusLabels[i] = makeLabel(statusScreen, "", x, y, 148, 26, UI_TEXT, &lv_font_montserrat_12);
        stylePanel(statusLabels[i]);
        lv_obj_set_style_pad_left(statusLabels[i], 0, 0);
        lv_obj_set_style_pad_top(statusLabels[i], 5, 0);
        lv_obj_set_style_pad_bottom(statusLabels[i], 5, 0);
        lv_obj_set_style_text_align(statusLabels[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    updateStatusPage();
}

void showLoadingFinished(lv_timer_t *timer) {
    splashProgress = splashProgress >= 90 ? 100 : static_cast<uint8_t>(splashProgress + 10);
    lv_bar_set_value(splashBar, splashProgress, LV_ANIM_ON);
    if (splashProgress >= 100) {
        lv_timer_del(timer);
        splashTimer = nullptr;
        lv_screen_load_anim(homeScreen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
    }
}

void updateStatusPage() {
    if (statusLabels[0] == nullptr) return;
    static const char *const names[STATUS_ITEM_COUNT] = {
        "ESP-NOW", "RTC", "SD Card", "Pengirim 1", "Pengirim 2",
        "Pengirim 3", "Pengirim 4", "Pengirim 5", "Pengirim 6", "Pengirim 7"
    };
    bool ready[STATUS_ITEM_COUNT] = {};
    ready[0] = espNowReady;
    ready[1] = rtcReady && rtcTimeValid;
    ready[2] = sdReady;
    const uint32_t now = millis();
    for (uint8_t source = 0; source < SOURCE_COUNT; ++source) {
        ready[source + 3] = isfinite(samples[source].db) &&
                            now - samples[source].receivedMillis <= SOURCE_ONLINE_TIMEOUT_MS;
    }

    for (uint8_t i = 0; i < STATUS_ITEM_COUNT; ++i) {
        char text[40];
        snprintf(text, sizeof(text), "%s   %s", names[i], ready[i] ? "READY" : "ERROR");
        lv_label_set_text(statusLabels[i], text);
        lv_obj_set_style_text_color(statusLabels[i],
                                    lv_color_hex(ready[i] ? 0x56D6A1 : 0xFF7185), 0);
    }
}

// Memproses antrean paket di loop utama, memperbarui UI, chart, dan logging SD.
void processReceiveEvents() {
    while (true) {
        ReceiveEvent event{};
        bool hasEvent = false;
        portENTER_CRITICAL(&queueMux);
        if (queueTail != queueHead) {
            event = eventQueue[queueTail];
            queueTail = static_cast<uint8_t>((queueTail + 1) % EVENT_QUEUE_LENGTH);
            hasEvent = true;
        }
        portEXIT_CRITICAL(&queueMux);
        if (!hasEvent) break;

        Sample &sample = samples[event.source];
        if (!event.valid) {
            sample.db = NAN;
            sample.receivedMillis = event.receivedMillis;
            updateHomeCard(event.source);
            if (lv_scr_act() == detailScreen && activeSource == event.source) refreshDetailText();
            if (lv_scr_act() == statusScreen) updateStatusPage();
            continue;
        }
        sample.db = event.db;
        sample.receivedMillis = event.receivedMillis;
        makeTimestamp(sample.timestamp, sizeof(sample.timestamp), event.receivedMillis);
        if (!isfinite(sample.maxDb) || event.db > sample.maxDb) {
            sample.maxDb = event.db;
            strlcpy(sample.maxTimestamp, sample.timestamp, sizeof(sample.maxTimestamp));
        }
        const uint8_t writeAt = chartWriteIndex[event.source];
        chartValues[event.source][writeAt] = event.db;
        if (rtcReady && rtcTimeValid) {
            const DateTime now = rtc.now();
            snprintf(chartTimes[event.source][writeAt], sizeof(chartTimes[event.source][writeAt]),
                     "%02u:%02u:%02u", static_cast<unsigned>(now.hour()),
                     static_cast<unsigned>(now.minute()), static_cast<unsigned>(now.second()));
        } else {
            snprintf(chartTimes[event.source][writeAt], sizeof(chartTimes[event.source][writeAt]),
                     "+%lus", static_cast<unsigned long>(event.receivedMillis / 1000));
        }
        chartWriteIndex[event.source] = static_cast<uint8_t>((writeAt + 1) % HISTORY_LENGTH);
        if (chartCount[event.source] < HISTORY_LENGTH) ++chartCount[event.source];
        appendLog(event.source, event.db, sample.timestamp, event.receivedMillis);
        updateHomeCard(event.source);
        if (lv_scr_act() == detailScreen && activeSource == event.source) refreshDetailText();
        if (lv_scr_act() == statusScreen) updateStatusPage();
    }
}

// Mengaktifkan ESP-NOW Long Range pada channel 1 agar cocok dengan node jaringan.
void initializeEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    const esp_err_t protocolResult = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    if (protocolResult != ESP_OK) {
        Serial.printf("Gagal mengaktifkan ESP-NOW Long Range: %d\n", protocolResult);
        return;
    }
    const esp_err_t channelResult =
        esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (channelResult != ESP_OK) {
        Serial.printf("Gagal mengatur channel Wi-Fi %u: %d\n",
                      ESPNOW_WIFI_CHANNEL, channelResult);
        return;
    }
    Serial.printf("ESP-NOW Long Range aktif, channel %u\n", ESPNOW_WIFI_CHANNEL);
    Serial.print("CYD MAC: ");
    Serial.println(WiFi.macAddress());
    if (esp_now_init() != ESP_OK) {
        Serial.println("Gagal inisialisasi ESP-NOW");
        return;
    }
    espNowReady = true;
    esp_now_register_recv_cb(onEspNowReceive);
    Serial.println("ESP-NOW receiver siap");
}

// Menginisialisasi RTC DS3231 dan microSD untuk timestamp serta pencatatan.
void initializeClockAndSd() {
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    rtcReady = rtc.begin();
    rtcTimeValid = rtcReady && !rtc.lostPower();
    if (!rtcReady) Serial.println("RTC DS3231 tidak terdeteksi");
    else if (!rtcTimeValid && SET_RTC_FROM_BUILD_TIME_IF_LOST) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        rtcTimeValid = true;
        Serial.println("RTC disetel dari waktu kompilasi; verifikasi waktu dan zona waktu");
    } else if (!rtcTimeValid) {
        Serial.println("RTC kehilangan daya; setel waktu DS3231 sebelum logging bertanggal");
    }

    useSdSpiPins();
    sdReady = SD.begin(SD_CS_PIN, touchSpi);
    if (sdReady) {
        if (SD.exists(LOG_PATH) && !hasWideCsvHeader(LOG_PATH)) {
            activeLogPath = ALTERNATE_LOG_PATH;
            Serial.println("SD: noise.csv berisi format lama; format baru memakai noise_wide.csv");
        }
        if (SD.exists(activeLogPath) && !hasWideCsvHeader(activeLogPath)) {
            Serial.printf("SD: header %s tidak sesuai; logging dinonaktifkan\n", activeLogPath);
            sdReady = false;
            return;
        }
        if (!SD.exists(activeLogPath)) {
            File file = SD.open(activeLogPath, FILE_WRITE);
            if (!file) {
                Serial.printf("SD: gagal membuat %s\n", activeLogPath);
                sdReady = false;
                return;
            }
            const bool headerWritten = writeWideCsvHeader(file);
            file.close();
            if (!headerWritten) {
                Serial.printf("SD: gagal menulis header ke %s\n", activeLogPath);
                sdReady = false;
                return;
            }
        }
        Serial.printf("SD card siap; logging ke %s\n", activeLogPath);
    } else {
        Serial.println("SD card tidak terdeteksi; logging dinonaktifkan");
    }
}
} // namespace

// Menginisialisasi layar, touch, RTC/SD, dan ESP-NOW saat CYD dinyalakan.
void setup() {
    Serial.begin(115200);
    delay(100);

    lv_init();
    tft.begin();
    tft.setRotation(1);
    touchSpi.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
    touch.begin(touchSpi);
    touch.setRotation(3);

    display = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_buffers(display, drawBuffer, nullptr, sizeof(drawBuffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, displayFlush);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, touchRead);
    lv_tick_set_cb(lvglTick);

    buildSplash();
    buildHome();
    buildDetail();
    buildStatus();
    lv_screen_load(splashScreen);

    initializeClockAndSd();
    initializeEspNow();
    updateStatusPage();
    splashTimer = lv_timer_create(showLoadingFinished, 100, nullptr);
}

// Menjalankan LVGL dan menangani event, pembaruan status, serta data masuk.
void loop() {
    processReceiveEvents();
    const uint32_t now = millis();
    if (pendingLogRow.active &&
        now - pendingLogRow.lastUpdateMillis >= LOG_ROW_IDLE_TIMEOUT_MS) {
        flushPendingLogRow();
    }
    if (now - lastUiRefresh >= 1000) {
        lastUiRefresh = now;
        if (lv_scr_act() == statusScreen) updateStatusPage();
        if (lv_scr_act() == detailScreen) refreshDetailText();
    }
    lv_timer_handler();
    delay(5);
}
