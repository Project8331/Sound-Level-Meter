#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// Test sketch untuk OLED I2C SH1106 128x64, mengikuti tipe OLED pada sketch Receiver lama.
// Untuk ESP32 DevKit biasa: SDA=GPIO21, SCL=GPIO22.
// Konfigurasi OLED SH1106 128x64 dan alamat I2C yang umum digunakan.
constexpr uint8_t OLED_SDA_PIN = 21;
constexpr uint8_t OLED_SCL_PIN = 22;
constexpr uint8_t OLED_WIDTH = 128;
constexpr uint8_t OLED_HEIGHT = 64;
constexpr uint8_t OLED_ADDR_CANDIDATES[] = {0x3C, 0x3D};

// Objek layar dan status animasi untuk memastikan tampilan dapat diperbarui.
Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
uint8_t oledAddress = 0;
uint32_t lastRefresh = 0;
uint32_t frame = 0;

// Memindai bus I2C dan memilih alamat yang cocok dengan OLED yang didukung.
uint8_t findOledAddress() {
    Serial.println("Scanning I2C bus...");
    for (uint8_t address = 1; address < 127; ++address) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) {
            Serial.printf("I2C device found at 0x%02X\n", address);
            for (uint8_t candidate : OLED_ADDR_CANDIDATES) {
                if (address == candidate) return address;
            }
        }
    }
    return 0;
}

// Menggambar teks uji dan progress bar yang bergerak untuk memeriksa refresh.
void showTestScreen() {
    display.clearDisplay();
    display.drawRoundRect(0, 0, OLED_WIDTH, OLED_HEIGHT, 5, SH110X_WHITE);
    display.drawFastHLine(8, 20, 112, SH110X_WHITE);

    display.setTextColor(SH110X_WHITE);
    display.setTextSize(1);
    display.setCursor(10, 7);
    display.print("OLED DTS-013 TEST");

    display.setCursor(10, 29);
    display.print("ESP32 + SH1106");
    display.setCursor(10, 41);
    display.printf("I2C address: 0x%02X", oledAddress);

    // Bergerak setiap detik untuk menguji refresh layar.
    display.drawRoundRect(10, 53, 108, 6, 2, SH110X_WHITE);
    const uint8_t progressWidth = static_cast<uint8_t>((frame % 17) * 6);
    if (progressWidth > 0) {
        display.fillRect(12, 55, progressWidth, 2, SH110X_WHITE);
    }
    display.display();
}

// Memulai I2C, mencari OLED, lalu menghentikan program jika layar tidak terdeteksi.
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("OLED DTS-013 I2C test");

    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    Wire.setClock(100000);
    oledAddress = findOledAddress();

    if (oledAddress == 0) {
        Serial.println("OLED tidak ditemukan pada 0x3C/0x3D.");
        Serial.println("Periksa VCC, GND, SDA, SCL, dan pin ESP32.");
        while (true) delay(1000);
    }

    if (!display.begin(oledAddress, true)) {
        Serial.println("Inisialisasi SH1106 gagal.");
        Serial.println("Jika scanner menemukan alamat tetapi layar kosong, modul mungkin SSD1306.");
        while (true) delay(1000);
    }

    Serial.println("OLED SH1106 siap.");
    showTestScreen();
}

// Memperbarui animasi layar setiap satu detik.
void loop() {
    const uint32_t now = millis();
    if (now - lastRefresh >= 1000) {
        lastRefresh = now;
        ++frame;
        showTestScreen();
    }
}
