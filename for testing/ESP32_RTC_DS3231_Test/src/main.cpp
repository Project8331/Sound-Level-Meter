#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <RTClib.h>
#include <stdio.h>
#include <string.h>

// Default ESP32 DevKit I2C pins. Change if your board uses different pins.
constexpr uint8_t RTC_SDA_PIN = 21;
constexpr uint8_t RTC_SCL_PIN = 22;
constexpr uint32_t REPORT_INTERVAL_MS = 1000;

// Objek RTC DS3231 serta buffer untuk menerima perintah dari Serial Monitor.
RTC_DS3231 rtc;
uint32_t lastReportMs = 0;
char serialCommand[48];
size_t serialCommandLength = 0;

// Menampilkan waktu RTC dalam format YYYY-MM-DD HH:MM:SS.
void printDateTime(const DateTime &dateTime) {
    Serial.printf("%04u-%02u-%02u %02u:%02u:%02u",
                  static_cast<unsigned>(dateTime.year()),
                  static_cast<unsigned>(dateTime.month()),
                  static_cast<unsigned>(dateTime.day()),
                  static_cast<unsigned>(dateTime.hour()),
                  static_cast<unsigned>(dateTime.minute()),
                  static_cast<unsigned>(dateTime.second()));
}

// Menjelaskan perintah SET, GET, dan HELP yang dapat dikirim melalui Serial.
void printHelp() {
    Serial.println("Commands (Serial Monitor, Newline):");
    Serial.println("  SET YYYY-MM-DD HH:MM:SS  set RTC time");
    Serial.println("  GET                       read RTC now");
    Serial.println("  HELP                      show commands");
}

// Membaca perintah serial per baris dan mengatur atau membaca waktu RTC.
void processSerialCommands() {
    while (Serial.available() > 0) {
        const char incoming = static_cast<char>(Serial.read());
        if (incoming == '\r') continue;

        if (incoming == '\n') {
            serialCommand[serialCommandLength] = '\0';
            int year, month, day, hour, minute, second;
            if (sscanf(serialCommand, "SET %d-%d-%d %d:%d:%d",
                       &year, &month, &day, &hour, &minute, &second) == 6) {
                const DateTime requested(year, month, day, hour, minute, second);
                if (requested.isValid()) {
                    rtc.adjust(requested);
                    Serial.print("RTC set to: ");
                    printDateTime(rtc.now());
                    Serial.println();
                } else {
                    Serial.println("Invalid date/time values.");
                }
            } else if (strcmp(serialCommand, "GET") == 0) {
                Serial.print("RTC: ");
                printDateTime(rtc.now());
                Serial.println();
            } else if (strcmp(serialCommand, "HELP") == 0 || serialCommandLength == 0) {
                printHelp();
            } else {
                Serial.println("Unknown command. Send HELP.");
            }
            serialCommandLength = 0;
        } else if (serialCommandLength < sizeof(serialCommand) - 1) {
            serialCommand[serialCommandLength++] = incoming;
        } else {
            serialCommandLength = 0;
            Serial.println("Command too long; discarded.");
        }
    }
}

// Memulai serial, bus I2C, dan RTC; waktu awal diisi dari waktu kompilasi bila baterai RTC habis.
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("ESP32 + DS3231 RTC test");

    WiFi.mode(WIFI_STA);
    Serial.print("ESP32 STA MAC: ");
    Serial.println(WiFi.macAddress());

    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    Wire.setClock(100000);

    if (!rtc.begin()) {
        Serial.println("DS3231 not found. Check VCC, GND, SDA, and SCL.");
        while (true) delay(1000);
    }

    if (rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        Serial.println("RTC lost power; initialized from firmware build time.");
        Serial.println("Set the correct local time using the SET command.");
    }

    printHelp();
    lastReportMs = millis();
}

// Memproses perintah pengguna dan mencetak waktu RTC secara berkala.
void loop() {
    processSerialCommands();

    const uint32_t nowMs = millis();
    if (nowMs - lastReportMs >= REPORT_INTERVAL_MS) {
        lastReportMs = nowMs;
        const DateTime now = rtc.now();
        Serial.print("RTC: ");
        if (now.isValid()) printDateTime(now);
        else Serial.print("INVALID");
        Serial.println();
    }
}
