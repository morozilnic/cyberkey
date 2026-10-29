#include <M5Cardputer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include "icon_jpeg.h"
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <ArduinoJson.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "CyberTOTP.h"
#include "CyberTyping.h"
#include "CyberPass.h"
#include "icon_data.h"
#include "icon_web.h"

#define MAX_ACCOUNTS 30
#define MAX_WIFI 5
#define NTP_SERVER1 "time.google.com"
#define NTP_SERVER2 "time.cloudflare.com"
#define NTP_SERVER3 "pool.ntp.org"

#define FIRMWARE_VERSION "1.2.4"
#define OTA_VERSION_URL  "https://cyberkey.sndjy.us/version.json"

String remoteOtaVersion = "";
String remoteOtaBinUrl = "";
bool otaUpdateAvailable = false;

// Cardputer SD SPI Pins
#define SD_SPI_SCK_PIN  40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_SPI_CS_PIN   12

struct Account {
    String issuer;  // e.g. "GitHub", "Google", "AWS"
    String name;    // e.g. "user@example.com", "developer"
    String secret;
};

struct WiFiCred {
    String ssid;
    String pass;
};

Account accounts[MAX_ACCOUNTS];
int accountCount = 0;
int currentIndex = 0;

WiFiCred wifiList[MAX_WIFI];
int wifiCount = 0;

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
M5Canvas canvas(&M5Cardputer.Display);
USBHIDKeyboard Keyboard;

// Bridging functions for CyberTyping
void usbTypePrint(const char* str) {
    Keyboard.print(str);
}
void usbTypeKey(uint8_t key) {
    Keyboard.write(key);
}
bool isUSBActive() {
    return true;
}


bool isAPMode = false;
bool timeSynced = false;
bool beepEnabled = false;
bool wifiSleeping = false;
String connectedSSID = "";
String deviceIP = "";
String statusMessage = "";
unsigned long statusMsgUntil = 0;

// Device & Standby Settings
String tzPosix = "EST5EDT,M3.2.0,M11.1.0"; // Default: US Eastern Time
String tzName = "US/Eastern";
bool use24Hour = false;
bool standbyEnabled = true;
int standbyTimeoutSec = 45;
float weatherLat = 40.7128;
float weatherLon = -74.0060;
String weatherCity = "New York, NY";
String weatherUnits = "fahrenheit";

// Live Weather Cache
float currentWeatherTemp = 999.0;
int currentWeatherCode = -1;
String currentWeatherDesc = "";
unsigned long lastWeatherFetch = 0;

// Power Management / Standby State
unsigned long lastActivityTime = 0;
bool isStandby = false;
#define BRIGHTNESS_NORMAL 80
#define BRIGHTNESS_STANDBY 30

// Wipe confirmation state
bool wipeConfirmActive = false;
unsigned long wipeConfirmUntil = 0;

// Forward declarations
void loadAccounts();
void saveAccounts();
void loadWiFi();
void saveWiFi();
void loadSettings();
void saveSettings();
void applyTimezone();
void fetchWeather();
String getWeatherDescription(int code);
void setupWebServer();
void drawUI(uint32_t code, int remaining, time_t now);
void drawStandbyScreen(time_t now);
void startCaptivePortal(bool silent = false);
void connectWiFi(bool silent = false);
void powerDownWiFi();
bool ensureSD();
bool backupToSD(bool force = false);
bool parseAndApplyBackupJSON(const String& jsonContent);
bool restoreFromSD();
void wipeAllData();
void handleKeyPress(char key);
void sendTOTPKeyboard(uint32_t code);
void checkForOTAUpdates(bool notify = false);
void performOTAUpdate();

static const char* OTA_LAN_URL = "http://192.168.5.55:8099/version.json";

void checkForOTAUpdates(bool notify) {
    if (WiFi.status() != WL_CONNECTED || isAPMode || wifiSleeping) {
        if (notify) {
            statusMessage = "No Wi-Fi for OTA";
            statusMsgUntil = millis() + 2500;
        }
        return;
    }

    HTTPClient http;
    http.setTimeout(2500);
    bool success = false;
    String payload = "";

    // 1. Try instant LAN HTTP check directly to host server
    if (http.begin(OTA_LAN_URL)) {
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            payload = http.getString();
            success = true;
        }
        http.end();
    }

    // 2. Fallback to public domain HTTPS if not at home
    if (!success) {
        WiFiClientSecure secureClient;
        secureClient.setInsecure();
        http.setTimeout(5000);
        if (http.begin(secureClient, OTA_VERSION_URL)) {
            int code = http.GET();
            if (code == HTTP_CODE_OK) {
                payload = http.getString();
                success = true;
            } else if (notify) {
                statusMessage = "OTA HTTP: " + String(code);
                statusMsgUntil = millis() + 3000;
            }
            http.end();
        } else if (notify) {
            statusMessage = "Conn Failed: -1";
            statusMsgUntil = millis() + 3000;
        }
    }

    if (success && payload.length() > 0) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err) {
            const char* ver = doc["version"];
            const char* url = doc["bin_url"];
            if (ver && url) {
                remoteOtaVersion = String(ver);
                remoteOtaBinUrl = String(url);
                if (remoteOtaVersion != FIRMWARE_VERSION) {
                    otaUpdateAvailable = true;
                    statusMessage = "Update " + remoteOtaVersion + " Avail!";
                    statusMsgUntil = millis() + 4000;
                } else if (notify) {
                    statusMessage = "Up-to-date (v" + String(FIRMWARE_VERSION) + ")";
                    statusMsgUntil = millis() + 2500;
                }
            }
        }
    }
}

static void otaProgressCallback(int current, int total) {
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.setTextDatum(middle_center);
    canvas.drawString("CyberKey OTA Update", 120, 25, 2);

    int pct = (total > 0) ? (current * 100 / total) : 0;
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.drawString(String(pct) + "% (" + String(current / 1024) + " KB)", 120, 55, 2);

    int barWidth = (total > 0) ? map(current, 0, total, 0, 200) : 0;
    canvas.drawRect(19, 79, 202, 12, TFT_DARKGRAY);
    canvas.fillRect(20, 80, barWidth, 10, TFT_GREEN);

    canvas.setTextColor(0xFBE0, TFT_BLACK); // Gold/Orange warning
    canvas.drawString("DO NOT POWER OFF", 120, 105, 2);
    canvas.pushSprite(0, 0);
    yield();
}

void performOTAUpdate() {
    if (WiFi.status() != WL_CONNECTED || isAPMode || wifiSleeping) {
        statusMessage = "Need Wi-Fi for OTA";
        statusMsgUntil = millis() + 2500;
        return;
    }

    canvas.fillScreen(TFT_BLACK);
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.setTextDatum(middle_center);
    canvas.drawString("CyberKey OTA Update", 120, 30, 2);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.drawString("Connecting to update server...", 120, 60, 1);
    canvas.drawString("DO NOT POWER OFF", 120, 85, 2);
    canvas.pushSprite(0, 0);

    // Try LAN direct streaming first (ultra fast, rock solid)
    String targetUrl = "http://192.168.5.55:8099/CyberKey-app.bin";
    HTTPClient http;
    http.setTimeout(3000);
    bool useLan = false;

    if (http.begin(targetUrl)) {
        int code = http.sendRequest("HEAD");
        if (code == 200) {
            useLan = true;
        }
        http.end();
    }

    if (!useLan) {
        targetUrl = remoteOtaBinUrl.length() > 0 ? remoteOtaBinUrl : "https://cyberkey.sndjy.us/CyberKey-app.bin";
    }

    WiFiClient client;
    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    if (targetUrl.startsWith("https://")) {
        http.begin(secureClient, targetUrl);
    } else {
        http.begin(client, targetUrl);
    }

    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        statusMessage = "HTTP Error: " + String(httpCode);
        statusMsgUntil = millis() + 3000;
        http.end();
        return;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        statusMessage = "Invalid Size";
        statusMsgUntil = millis() + 3000;
        http.end();
        return;
    }

    if (!Update.begin(contentLength)) {
        statusMessage = "Not Enough Space";
        statusMsgUntil = millis() + 3000;
        http.end();
        return;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t written = 0;
    uint8_t buff[1024];

    while (http.connected() && (written < (size_t)contentLength)) {
        size_t avail = stream->available();
        if (avail) {
            int readBytes = stream->readBytes(buff, ((avail > sizeof(buff)) ? sizeof(buff) : avail));
            Update.write(buff, readBytes);
            written += readBytes;
            otaProgressCallback(written, contentLength);
        }
        delay(1);
    }

    if (Update.end() && Update.isFinished()) {
        canvas.fillScreen(TFT_BLACK);
        canvas.setTextColor(TFT_GREEN, TFT_BLACK);
        canvas.setTextDatum(middle_center);
        canvas.drawString("Update Success!", 120, 50, 2);
        canvas.drawString("Rebooting...", 120, 80, 2);
        canvas.pushSprite(0, 0);
        delay(1500);
        ESP.restart();
    } else {
        statusMessage = "Flash Error: " + String(Update.getError());
        statusMsgUntil = millis() + 3000;
    }

    http.end();
}

void applyTimezone() {
    setenv("TZ", tzPosix.c_str(), 1);
    tzset();
}

String getWeatherDescription(int code) {
    switch (code) {
        case 0: return "Clear";
        case 1: return "Mainly Clear";
        case 2: return "Partly Cloudy";
        case 3: return "Overcast";
        case 45: case 48: return "Foggy";
        case 51: case 53: case 55: return "Drizzle";
        case 56: case 57: return "Frz Drizzle";
        case 61: case 63: case 65: return "Rain";
        case 66: case 67: return "Frz Rain";
        case 71: case 73: case 75: case 77: return "Snow";
        case 80: case 81: case 82: return "Rain Showers";
        case 85: case 86: return "Snow Showers";
        case 95: case 96: case 99: return "Storm";
        default: return "";
    }
}

void fetchWeather() {
    if (WiFi.status() != WL_CONNECTED || isAPMode || wifiSleeping) return;

    HTTPClient http;
    http.setTimeout(4000);
    String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(weatherLat, 4) +
                 "&longitude=" + String(weatherLon, 4) +
                 "&current=temperature_2m,weather_code&temperature_unit=" + weatherUnits;

    if (http.begin(url)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload);
            if (!err && doc["current"].is<JsonObject>()) {
                currentWeatherTemp = doc["current"]["temperature_2m"].as<float>();
                currentWeatherCode = doc["current"]["weather_code"].as<int>();
                currentWeatherDesc = getWeatherDescription(currentWeatherCode);
                lastWeatherFetch = millis();
            }
        }
        http.end();
    }
}

void setup() {
    auto cfg = M5.config();
    cfg.internal_spk = true;
    cfg.internal_mic = false;
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(BRIGHTNESS_NORMAL);

    M5Cardputer.Speaker.setVolume(60);

    // Initialize USB HID Keyboard Emulation
    Keyboard.begin();
    USB.begin();

    // Initialize flicker-free double buffer canvas
    canvas.createSprite(240, 135);
    canvas.setTextWrap(false);

    // Boot Logo & Splash
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.pushImage((240 - ICON_WIDTH) / 2, 6, ICON_WIDTH, ICON_HEIGHT, icon_rgb565);
    M5Cardputer.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5Cardputer.Display.setTextDatum(top_center);
    M5Cardputer.Display.drawString("CyberKey 2FA v1.2.4 🚀", 120, 106, 2);
    M5Cardputer.Display.setTextDatum(top_left);
    delay(1800);

    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(0x07E0, TFT_BLACK); // Bright Neon Green
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.println("CyberKey 2FA Vault");
    M5Cardputer.Display.println("------------------");

    initBLEKeyboard();
    loadSettings();
    applyTimezone();
    loadAccounts();
    if (ensureSD()) {
        CyberPassVault::init();
    }
    loadWiFi();
    connectWiFi();
    setupWebServer();

    lastActivityTime = millis();
}

void loop() {
    M5Cardputer.update();

    if (isAPMode) {
        dnsServer.processNextRequest();
    }
    if (!wifiSleeping) {
        server.handleClient();
    }

    // Time & TOTP calculation
    time_t now;
    time(&now);
    int remaining = 30 - (now % 30);

    uint32_t code = 0;
    if (accountCount > 0 && currentIndex < accountCount && timeSynced) {
        code = CyberTOTP::generateCode(accounts[currentIndex].secret.c_str(), (uint64_t)now, 30);
    }

    // Keyboard handling
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        lastActivityTime = millis();
        if (isStandby) {
            isStandby = false;
            M5Cardputer.Display.setBrightness(BRIGHTNESS_NORMAL);
            // Don't process the wake keypress as an action
            return;
        }

        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

        // Check for Panic Wipe: Fn + Backspace / Del
        if (status.del) {
            if (!wipeConfirmActive) {
                wipeConfirmActive = true;
                wipeConfirmUntil = millis() + 5000;
                statusMessage = "PRESS DEL AGAIN TO WIPE!";
                statusMsgUntil = millis() + 5000;
            } else {
                wipeAllData();
                wipeConfirmActive = false;
                statusMessage = "VAULT WIPED CLEAN!";
                statusMsgUntil = millis() + 3000;
            }
        } else if (status.enter) {
            // USB Auto-Type TOTP
            if (code > 0) {
                sendTOTPKeyboard(code);
            }
        } else {
            for (auto i : status.word) {
                handleKeyPress(i);
            }
        }
    }

    // Standby Trigger
    if (standbyEnabled && !isStandby && (millis() - lastActivityTime > (unsigned long)standbyTimeoutSec * 1000)) {
        isStandby = true;
        M5Cardputer.Display.setBrightness(BRIGHTNESS_STANDBY);
    }

    // Wipe confirmation timeout
    if (wipeConfirmActive && millis() > wipeConfirmUntil) {
        wipeConfirmActive = false;
    }

    // Periodic Weather Refresh (Every 30 mins)
    if (WiFi.status() == WL_CONNECTED && !isAPMode && !wifiSleeping) {
        if (lastWeatherFetch == 0 || millis() - lastWeatherFetch > 1800000) {
            fetchWeather();
        }
    }

    // Expiration Audio Tick (3, 2, 1 seconds left) - only when active
    static int lastBeepSec = -1;
    if (!isStandby && beepEnabled && accountCount > 0 && timeSynced) {
        if (remaining <= 3 && remaining >= 1 && remaining != lastBeepSec) {
            lastBeepSec = remaining;
            M5Cardputer.Speaker.tone(remaining == 1 ? 2800 : 2200, 30);
        } else if (remaining > 3) {
            lastBeepSec = -1;
        }
    }

    // Auto-resync NTP if time is before year 2024
    if (now < 1704067200 && WiFi.isConnected() && !isAPMode) {
        static unsigned long lastNtpTry = 0;
        if (millis() - lastNtpTry > 5000) {
            lastNtpTry = millis();
            configTime(0, 0, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
            applyTimezone();
            struct tm t;
            if (getLocalTime(&t, 2000)) {
                timeSynced = true;
            }
        }
    }

    static unsigned long lastDraw = 0;
    if (millis() - lastDraw >= 200) {
        lastDraw = millis();
        if (isStandby) {
            drawStandbyScreen(now);
        } else {
            drawUI(code, remaining, now);
        }
    }

    delay(20);
}

void sendTOTPKeyboard(uint32_t code) {
    char codeStr[8];
    snprintf(codeStr, sizeof(codeStr), "%06lu", (unsigned long)code);
    typeStringWithEnter(codeStr);
    statusMessage = isBLEConnected() ? "Code Typed (BLE)!" : "Code Typed (USB)!";
    statusMsgUntil = millis() + 2000;
}

void autoTypeUsername() {
    if (accountCount == 0 || currentIndex >= accountCount) return;
    String user = accounts[currentIndex].name;
    int pIdx = CyberPassVault::findMatch(accounts[currentIndex].issuer, accounts[currentIndex].name);
    if (pIdx >= 0 && CyberPassVault::passwords[pIdx].username.length() > 0) {
        user = CyberPassVault::passwords[pIdx].username;
    }
    if (user.length() > 0) {
        typeString(user);
        statusMessage = isBLEConnected() ? "User Typed (BLE)!" : "User Typed (USB)!";
    } else {
        statusMessage = "No Username Found";
    }
    statusMsgUntil = millis() + 2000;
}

void autoTypePassword() {
    if (accountCount == 0 || currentIndex >= accountCount) return;
    int pIdx = CyberPassVault::findMatch(accounts[currentIndex].issuer, accounts[currentIndex].name);
    if (pIdx >= 0 && CyberPassVault::passwords[pIdx].password.length() > 0) {
        typeString(CyberPassVault::passwords[pIdx].password);
        statusMessage = isBLEConnected() ? "Pass Typed (BLE)!" : "Pass Typed (USB)!";
    } else {
        statusMessage = "No SD Pass Found";
    }
    statusMsgUntil = millis() + 2000;
}

void handleKeyPress(char key) {
    if (key == ';' || key == ',' || key == 'a' || key == 'A') { // Up / Prev
        if (accountCount > 0) {
            currentIndex = (currentIndex - 1 + accountCount) % accountCount;
        }
    } else if (key == '.' || key == '/' || key == 'd' || key == 'D' || key == ' ') { // Down / Next
        if (accountCount > 0) {
            currentIndex = (currentIndex + 1) % accountCount;
        }
    } else if (key == 's' || key == 'S') { // Manual Standby / Clock Mode
        isStandby = true;
        M5Cardputer.Display.setBrightness(BRIGHTNESS_STANDBY);
    } else if (key == 'b' || key == 'B') { // Quick Backup to SD
        if (backupToSD()) {
            statusMessage = "SD Backup OK!";
        } else {
            statusMessage = "SD Backup Failed";
        }
        statusMsgUntil = millis() + 2500;
    } else if (key == 'o' || key == 'O') { // Trigger OTA Check/Flash
        if (WiFi.isConnected()) {
            checkForOTAUpdates(true);
            if (otaUpdateAvailable) {
                performOTAUpdate();
            }
        } else {
            statusMessage = "Wi-Fi Needed for OTA";
            statusMsgUntil = millis() + 2000;
        }
    } else if (key == 'r' || key == 'R') { // Quick Restore from SD
        if (restoreFromSD()) {
            statusMessage = "SD Restore OK!";
        } else {
            statusMessage = "SD Restore Failed";
        }
        statusMsgUntil = millis() + 2500;
    } else if (key == 'u' || key == 'U') { // Type Username
        autoTypeUsername();
    } else if (key == 'p' || key == 'P') { // Type Password
        autoTypePassword();
    } else if (key == 'm' || key == 'M') { // Toggle Mute / Beep
        beepEnabled = !beepEnabled;
        statusMessage = beepEnabled ? "Chirp: ON" : "Chirp: OFF";
        statusMsgUntil = millis() + 2000;
    } else if (key == 'w' || key == 'W') { // Toggle Wi-Fi / Web Portal On-Demand
        if (wifiSleeping) {
            wifiSleeping = false;
            statusMessage = "Connecting Wi-Fi...";
            statusMsgUntil = millis() + 4000;
            connectWiFi(true); // Silent reconnect without overwriting screen
            fetchWeather();
            statusMessage = WiFi.isConnected() ? "Wi-Fi: " + connectedSSID : "Wi-Fi: Not Found";
            statusMsgUntil = millis() + 2500;
        } else {
            powerDownWiFi();
            statusMessage = "Wi-Fi: Sleep (Batt Save)";
            statusMsgUntil = millis() + 2500;
        }
    } else if (((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) && key != 'o' && key != 'O' && key != 'u' && key != 'U' && key != 'p' && key != 'P') {
        // Quick Jump / Search by first letter of Issuer (or Name)
        char target = tolower(key);
        for (int i = 0; i < accountCount; ++i) {
            int checkIdx = (currentIndex + 1 + i) % accountCount;
            String label = accounts[checkIdx].issuer.length() > 0 ? accounts[checkIdx].issuer : accounts[checkIdx].name;
            if (label.length() > 0 && tolower(label.charAt(0)) == target) {
                currentIndex = checkIdx;
                statusMessage = "Jump -> " + label;
                statusMsgUntil = millis() + 1500;
                break;
            }
        }
    }
}

void drawStandbyScreen(time_t now) {
    canvas.fillScreen(TFT_BLACK);

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // Date Header (Top, Centered)
    const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char dateStr[32];
    snprintf(dateStr, sizeof(dateStr), "%s, %s %d", days[timeinfo.tm_wday], months[timeinfo.tm_mon], timeinfo.tm_mday);

    canvas.setTextColor(0x7BEF, TFT_BLACK);
    canvas.setTextDatum(top_center);
    canvas.drawString(dateStr, 120, 8, 2);

    // Big Digital Clock (Center)
    char timeStr[16];
    char ampmStr[4] = "";
    if (use24Hour) {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        int hour12 = timeinfo.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        snprintf(timeStr, sizeof(timeStr), "%d:%02d", hour12, timeinfo.tm_min);
        snprintf(ampmStr, sizeof(ampmStr), "%s", timeinfo.tm_hour >= 12 ? "PM" : "AM");
    }

    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.setTextDatum(top_center);
    canvas.drawString(timeStr, use24Hour ? 120 : 108, 30, 7); // Font 7: 7-segment digital font

    if (!use24Hour) {
        canvas.setTextColor(TFT_GREEN, TFT_BLACK);
        canvas.setTextDatum(top_left);
        canvas.drawString(ampmStr, 185, 36, 2);
    }

    // Weather Card Pill (Bottom Middle)
    canvas.fillRoundRect(8, 86, 224, 26, 6, 0x10A2); // Deep navy blue pill
    canvas.drawRoundRect(8, 86, 224, 26, 6, 0x2A69);

    if (currentWeatherTemp < 900.0) {
        char wStr[40];
        snprintf(wStr, sizeof(wStr), "%.0f`%s  %s", currentWeatherTemp, weatherUnits == "fahrenheit" ? "F" : "C", currentWeatherDesc.c_str());
        canvas.setTextColor(TFT_WHITE, 0x10A2);
        canvas.setTextDatum(middle_center);
        canvas.drawString(wStr, 120, 99, 2);
    } else {
        canvas.setTextColor(0x7BEF, 0x10A2);
        canvas.setTextDatum(middle_center);
        canvas.drawString(WiFi.isConnected() ? "Weather syncing..." : weatherCity.c_str(), 120, 99, 2);
    }

    // Subtle bottom bar: Battery & Wake hint
    int batt = M5Cardputer.Power.getBatteryLevel();
    char bStr[32];
    snprintf(bStr, sizeof(bStr), "BAT: %d%% | Press any key for 2FA", batt);
    canvas.setTextColor(0x52AA, TFT_BLACK);
    canvas.setTextDatum(top_center);
    canvas.drawString(bStr, 120, 118, 1);
    canvas.setTextDatum(top_left); // Reset datum

    canvas.pushSprite(0, 0);
}

void drawUI(uint32_t code, int remaining, time_t now) {
    canvas.fillScreen(TFT_BLACK);

    // Top Header
    canvas.fillRect(0, 0, 240, 20, 0x01E4); // Dark Cyan Header
    canvas.setTextColor(TFT_WHITE, 0x01E4);
    canvas.setCursor(6, 6);
    canvas.setTextSize(1);

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char timeStr[16];
    if (use24Hour) {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        int hour12 = timeinfo.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        snprintf(timeStr, sizeof(timeStr), "%d:%02d %s", hour12, timeinfo.tm_min, timeinfo.tm_hour >= 12 ? "PM" : "AM");
    }

    if (wifiSleeping) {
        canvas.print("⚡ Eco-Mode [W]");
    } else if (isAPMode) {
        canvas.print("AP: CyberKey");
    } else {
        canvas.print(WiFi.isConnected() ? deviceIP : "No WiFi [W]");
    }

    // BLE status badge on header
    if (isBLEConnected()) {
        canvas.fillRect(138, 4, 17, 12, 0x001F);
        canvas.setTextColor(TFT_WHITE, 0x001F);
        canvas.drawString("BT", 140, 6, 1);
    }

    // Display Local Time on header
    canvas.setCursor(158, 6);
    canvas.setTextColor(timeSynced ? TFT_GREEN : TFT_YELLOW, 0x01E4);
    canvas.print(timeStr);

    if (accountCount == 0) {
        canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
        canvas.setCursor(20, 45);
        canvas.setTextSize(2);
        canvas.println("No Keys Stored");
        canvas.setTextSize(1);
        canvas.setTextColor(TFT_WHITE, TFT_BLACK);
        canvas.setCursor(10, 75);
        canvas.printf("Open web browser to:\n http://%s", deviceIP.c_str());
        canvas.setCursor(10, 110);
        canvas.setTextColor(0x07E0, TFT_BLACK);
        canvas.println("Add keys via Web or SD.");

        // Status Banner Toast Overlay
        if (millis() < statusMsgUntil && statusMessage.length() > 0) {
            canvas.fillRoundRect(8, 24, 224, 22, 5, 0x02CD);
            canvas.drawRoundRect(8, 24, 224, 22, 5, TFT_CYAN);
            canvas.setTextColor(TFT_WHITE, 0x02CD);
            canvas.setTextDatum(middle_center);
            canvas.drawString(statusMessage.c_str(), 120, 35, 2);
            canvas.setTextDatum(top_left);
        }

        canvas.pushSprite(0, 0);
        return;
    }

    if (!timeSynced) {
        canvas.setTextColor(TFT_RED, TFT_BLACK);
        canvas.setCursor(10, 45);
        canvas.setTextSize(2);
        canvas.println("Waiting for NTP...");
        canvas.setTextSize(1);
        canvas.setTextColor(TFT_WHITE, TFT_BLACK);
        canvas.setCursor(10, 80);
        canvas.println("Clock not synced to Internet.");
        canvas.println("Connect to WiFi with Internet access.");

        // Status Banner Toast Overlay
        if (millis() < statusMsgUntil && statusMessage.length() > 0) {
            canvas.fillRoundRect(8, 24, 224, 22, 5, 0x02CD);
            canvas.drawRoundRect(8, 24, 224, 22, 5, TFT_CYAN);
            canvas.setTextColor(TFT_WHITE, 0x02CD);
            canvas.setTextDatum(middle_center);
            canvas.drawString(statusMessage.c_str(), 120, 35, 2);
            canvas.setTextDatum(top_left);
        }

        canvas.pushSprite(0, 0);
        return;
    }

    // Line 1: Primary Site / Issuer Name (Large Cyan)
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.setTextSize(2);
    canvas.setCursor(8, 24);
    String issuer = accounts[currentIndex].issuer;
    if (issuer.length() == 0) issuer = accounts[currentIndex].name;
    if (issuer.length() > 14) issuer = issuer.substring(0, 14);
    canvas.print(issuer);

    // Account Counter (e.g. [1/5])
    canvas.setTextSize(1);
    canvas.setTextColor(0x7BEF, TFT_BLACK);
    canvas.setCursor(195, 26);
    canvas.printf("[%d/%d]", currentIndex + 1, accountCount);

    // Line 2: Sub-Account / Username (Small Muted Gray/Cyan)
    canvas.setTextSize(1);
    canvas.setTextColor(0x9E79, TFT_BLACK);
    canvas.setCursor(8, 43);
    String username = accounts[currentIndex].name;
    if (accounts[currentIndex].issuer.length() > 0 && username.length() > 0) {
        if (username.length() > 32) username = username.substring(0, 32);
        canvas.print(username);
    }

    // 6-digit Code (Big Bright Green Text)
    char codeStr[8];
    snprintf(codeStr, sizeof(codeStr), "%06lu", (unsigned long)code);
    char fmtCode[10];
    snprintf(fmtCode, sizeof(fmtCode), "%.3s %.3s", codeStr, codeStr + 3);

    canvas.setTextSize(4);
    canvas.setTextColor(TFT_GREEN, TFT_BLACK);
    canvas.setCursor(15, 58);
    canvas.print(fmtCode);

    // Progress Bar (30s countdown)
    int barWidth = map(remaining, 0, 30, 0, 220);
    uint16_t barColor = (remaining <= 3) ? TFT_RED : ((remaining <= 10) ? TFT_YELLOW : TFT_GREEN);
    canvas.drawRect(9, 99, 222, 10, 0x4208);
    canvas.fillRect(10, 100, barWidth, 8, barColor);

    // Bottom Navigation Help
    canvas.setTextSize(1);
    canvas.setTextColor(0x7BEF, TFT_BLACK);
    canvas.setCursor(6, 118);
    int matchIdx = CyberPassVault::findMatch(accounts[currentIndex].issuer, accounts[currentIndex].name);
    if (matchIdx >= 0) {
        canvas.printf("%ds|[Ent]2FA [U]Usr [P]Pass [S]Clk", remaining);
    } else {
        canvas.printf("%ds|[Ent]2FA [U]Usr [S]Clk [A-Z]Find", remaining);
    }

    // Status Banner Toast Overlay (rendered last so it floats cleanly on top of all text)
    if (millis() < statusMsgUntil && statusMessage.length() > 0) {
        canvas.fillRoundRect(8, 22, 224, 24, 5, wipeConfirmActive ? 0xA000 : 0x02CD);
        canvas.drawRoundRect(8, 22, 224, 24, 5, wipeConfirmActive ? TFT_RED : TFT_CYAN);
        canvas.setTextColor(TFT_WHITE, wipeConfirmActive ? 0xA000 : 0x02CD);
        canvas.setTextDatum(middle_center);
        canvas.drawString(statusMessage.c_str(), 120, 34, 2);
        canvas.setTextDatum(top_left);
    }

    canvas.pushSprite(0, 0);
}

void loadAccounts() {
    prefs.begin("cyberkey", true);
    accountCount = prefs.getInt("count", 0);
    if (accountCount > MAX_ACCOUNTS) accountCount = MAX_ACCOUNTS;

    for (int i = 0; i < accountCount; ++i) {
        String nKey = "n" + String(i);
        String sKey = "s" + String(i);
        String isKey = "is" + String(i);
        accounts[i].name = prefs.getString(nKey.c_str(), "Account");
        accounts[i].secret = prefs.getString(sKey.c_str(), "");
        accounts[i].issuer = prefs.getString(isKey.c_str(), "");
    }
    prefs.end();
}

void saveAccounts() {
    prefs.begin("cyberkey", false);
    prefs.putInt("count", accountCount);
    for (int i = 0; i < accountCount; ++i) {
        String nKey = "n" + String(i);
        String sKey = "s" + String(i);
        String isKey = "is" + String(i);
        prefs.putString(nKey.c_str(), accounts[i].name);
        prefs.putString(sKey.c_str(), accounts[i].secret);
        prefs.putString(isKey.c_str(), accounts[i].issuer);
    }
    prefs.end();
}

void loadSettings() {
    prefs.begin("ck_settings", true);
    tzPosix = prefs.getString("tz_posix", "EST5EDT,M3.2.0,M11.1.0");
    tzName = prefs.getString("tz_name", "US/Eastern");
    use24Hour = prefs.getBool("use_24h", false);
    standbyEnabled = prefs.getBool("standby_en", true);
    standbyTimeoutSec = prefs.getInt("standby_sec", 45);
    weatherLat = prefs.getFloat("w_lat", 40.7128);
    weatherLon = prefs.getFloat("w_lon", -74.0060);
    weatherCity = prefs.getString("w_city", "New York, NY");
    weatherUnits = prefs.getString("w_units", "fahrenheit");
    prefs.end();
}

void saveSettings() {
    prefs.begin("ck_settings", false);
    prefs.putString("tz_posix", tzPosix);
    prefs.putString("tz_name", tzName);
    prefs.putBool("use_24h", use24Hour);
    prefs.putBool("standby_en", standbyEnabled);
    prefs.putInt("standby_sec", standbyTimeoutSec);
    prefs.putFloat("w_lat", weatherLat);
    prefs.putFloat("w_lon", weatherLon);
    prefs.putString("w_city", weatherCity);
    prefs.putString("w_units", weatherUnits);
    prefs.end();
}

void loadWiFi() {
    prefs.begin("ck_wifi", true);
    wifiCount = prefs.getInt("w_count", 0);
    if (wifiCount > MAX_WIFI) wifiCount = MAX_WIFI;

    if (wifiCount == 0 && prefs.isKey("ssid")) {
        String oldSSID = prefs.getString("ssid", "");
        String oldPass = prefs.getString("pass", "");
        if (oldSSID.length() > 0) {
            wifiList[0].ssid = oldSSID;
            wifiList[0].pass = oldPass;
            wifiCount = 1;
        }
    } else {
        for (int i = 0; i < wifiCount; ++i) {
            wifiList[i].ssid = prefs.getString(("ws" + String(i)).c_str(), "");
            wifiList[i].pass = prefs.getString(("wp" + String(i)).c_str(), "");
        }
    }
    prefs.end();
}

void saveWiFi() {
    prefs.begin("ck_wifi", false);
    prefs.putInt("w_count", wifiCount);
    for (int i = 0; i < wifiCount; ++i) {
        prefs.putString(("ws" + String(i)).c_str(), wifiList[i].ssid);
        prefs.putString(("wp" + String(i)).c_str(), wifiList[i].pass);
    }
    prefs.end();
}

void wipeAllData() {
    prefs.begin("cyberkey", false);
    prefs.clear();
    prefs.end();

    prefs.begin("ck_wifi", false);
    prefs.clear();
    prefs.end();

    prefs.begin("ck_settings", false);
    prefs.clear();
    prefs.end();

    accountCount = 0;
    currentIndex = 0;
    wifiCount = 0;
    connectedSSID = "";
    CyberPassVault::wipe();
    loadSettings();
}

bool ensureSD() {
    static bool initialized = false;
    if (initialized && SD.cardType() != CARD_NONE) return true;

    // Retry with multiple SPI frequencies if initial attempt fails
    const uint32_t freqs[] = {25000000, 20000000, 10000000, 4000000};
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);

    for (int i = 0; i < 4; ++i) {
        if (SD.begin(SD_SPI_CS_PIN, SPI, freqs[i])) {
            if (SD.cardType() != CARD_NONE) {
                initialized = true;
                return true;
            }
        }
        delay(10);
    }
    initialized = false;
    return false;
}

bool backupToSD(bool force) {
    if (!ensureSD()) {
        statusMessage = "SD Card Not Detected";
        return false;
    }

    // Safety guard: Prevent accidental overwrite of an existing backup with 0 accounts
    if (accountCount == 0 && !force && SD.exists("/cyberkey_backup.json")) {
        statusMessage = "Blocked: Vault Empty";
        return false;
    }

    // If a backup already exists and has accounts, preserve it as a rolling .bak
    if (SD.exists("/cyberkey_backup.json") && SD.exists("/cyberkey_backup.bak")) {
        SD.remove("/cyberkey_backup.bak");
    }
    if (SD.exists("/cyberkey_backup.json")) {
        // Copy current to .bak before replacing
        File src = SD.open("/cyberkey_backup.json", FILE_READ);
        if (src) {
            File dst = SD.open("/cyberkey_backup.bak", FILE_WRITE);
            if (dst) {
                while (src.available()) {
                    dst.write(src.read());
                }
                dst.close();
            }
            src.close();
        }
    }

    File file = SD.open("/cyberkey_backup.json", FILE_WRITE);
    if (!file) {
        statusMessage = "SD Write Error";
        return false;
    }

    JsonDocument doc;
    JsonArray arr = doc["accounts"].to<JsonArray>();
    for (int i = 0; i < accountCount; ++i) {
        JsonObject obj = arr.add<JsonObject>();
        obj["issuer"] = accounts[i].issuer;
        obj["name"] = accounts[i].name;
        obj["secret"] = accounts[i].secret;
    }

    JsonArray wArr = doc["wifi"].to<JsonArray>();
    for (int i = 0; i < wifiCount; ++i) {
        JsonObject wObj = wArr.add<JsonObject>();
        wObj["ssid"] = wifiList[i].ssid;
        wObj["pass"] = wifiList[i].pass;
    }

    JsonObject sObj = doc["settings"].to<JsonObject>();
    sObj["tz_posix"] = tzPosix;
    sObj["tz_name"] = tzName;
    sObj["use_24h"] = use24Hour;
    sObj["standby_en"] = standbyEnabled;
    sObj["standby_sec"] = standbyTimeoutSec;
    sObj["w_lat"] = weatherLat;
    sObj["w_lon"] = weatherLon;
    sObj["w_city"] = weatherCity;
    sObj["w_units"] = weatherUnits;

    serializeJson(doc, file);
    file.close();
    statusMessage = "SD Backup OK!";
    return true;
}

bool parseAndApplyBackupJSON(const String& jsonContent) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonContent);
    if (error) return false;

    if (doc["accounts"].is<JsonArray>()) {
        JsonArray arr = doc["accounts"].as<JsonArray>();
        if (arr.size() > 0) {
            accountCount = 0;
            for (JsonObject obj : arr) {
                if (accountCount >= MAX_ACCOUNTS) break;
                accounts[accountCount].issuer = obj["issuer"].is<String>() ? obj["issuer"].as<String>() : "";
                accounts[accountCount].name = obj["name"].as<String>();
                accounts[accountCount].secret = obj["secret"].as<String>();
                accountCount++;
            }
            saveAccounts();
        }
    }

    if (doc["wifi"].is<JsonArray>()) {
        JsonArray wArr = doc["wifi"].as<JsonArray>();
        if (wArr.size() > 0) {
            wifiCount = 0;
            for (JsonObject wObj : wArr) {
                if (wifiCount >= MAX_WIFI) break;
                wifiList[wifiCount].ssid = wObj["ssid"].as<String>();
                wifiList[wifiCount].pass = wObj["pass"].as<String>();
                wifiCount++;
            }
            saveWiFi();
        }
    }

    if (doc["settings"].is<JsonObject>()) {
        JsonObject sObj = doc["settings"].as<JsonObject>();
        if (sObj["tz_posix"].is<String>()) tzPosix = sObj["tz_posix"].as<String>();
        if (sObj["tz_name"].is<String>()) tzName = sObj["tz_name"].as<String>();
        if (sObj["use_24h"].is<bool>()) use24Hour = sObj["use_24h"].as<bool>();
        if (sObj["standby_en"].is<bool>()) standbyEnabled = sObj["standby_en"].as<bool>();
        if (sObj["standby_sec"].is<int>()) standbyTimeoutSec = sObj["standby_sec"].as<int>();
        if (sObj["w_lat"].is<float>()) weatherLat = sObj["w_lat"].as<float>();
        if (sObj["w_lon"].is<float>()) weatherLon = sObj["w_lon"].as<float>();
        if (sObj["w_city"].is<String>()) weatherCity = sObj["w_city"].as<String>();
        if (sObj["w_units"].is<String>()) weatherUnits = sObj["w_units"].as<String>();
        saveSettings();
        applyTimezone();
    }

    currentIndex = 0;
    return true;
}

bool restoreFromSD() {
    if (!ensureSD()) {
        statusMessage = "SD Card Not Detected";
        return false;
    }

    const char* targets[] = {"/cyberkey_backup.json", "/cyberkey_backup.bak"};
    for (int t = 0; t < 2; ++t) {
        if (SD.exists(targets[t])) {
            File file = SD.open(targets[t], FILE_READ);
            if (file) {
                String content = "";
                while (file.available()) {
                    content += (char)file.read();
                }
                file.close();

                if (parseAndApplyBackupJSON(content)) {
                    statusMessage = (t == 0) ? "SD Restore OK!" : "Restored from .bak!";
                    return true;
                }
            }
        }
    }

    statusMessage = "No Backup File Found";
    return false;
}

void powerDownWiFi() {
    wifiSleeping = true;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    connectedSSID = "";
}

void connectWiFi(bool silent) {
    if (wifiCount > 0) {
        WiFi.mode(WIFI_STA);

        if (!silent) {
            M5Cardputer.Display.print("  Scanning Wi-Fi...");
        }
        int n = WiFi.scanNetworks();
        if (!silent) {
            M5Cardputer.Display.printf(" %d found\n", n);
        }

        int bestIndex = -1;
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                String foundSSID = WiFi.SSID(i);
                for (int w = 0; w < wifiCount; ++w) {
                    if (foundSSID == wifiList[w].ssid) {
                        bestIndex = w;
                        break;
                    }
                }
                if (bestIndex != -1) break;
            }
        }

        for (int attempt = 0; attempt < wifiCount; ++attempt) {
            int w = (bestIndex != -1 && attempt == 0) ? bestIndex : attempt;
            String ssid = wifiList[w].ssid;
            String pass = wifiList[w].pass;

            if (!silent) {
                M5Cardputer.Display.printf("  Connecting: %s\n", ssid.c_str());
            }
            WiFi.begin(ssid.c_str(), pass.c_str());

            int timeout = 16;
            while (WiFi.status() != WL_CONNECTED && timeout > 0) {
                delay(500);
                if (!silent) {
                    M5Cardputer.Display.print(".");
                }
                timeout--;
            }

            if (WiFi.status() == WL_CONNECTED) {
                connectedSSID = ssid;
                deviceIP = WiFi.localIP().toString();
                if (!silent) {
                    M5Cardputer.Display.println("\n  WiFi Connected!");
                    M5Cardputer.Display.printf("  IP: %s\n", deviceIP.c_str());
                    M5Cardputer.Display.print("  Syncing NTP Time...");
                }

                // Sync NTP
                configTime(0, 0, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
                applyTimezone();
                struct tm timeinfo;
                if (getLocalTime(&timeinfo, 10000)) {
                    timeSynced = true;
                    if (!silent) M5Cardputer.Display.println(" OK!");
                } else {
                    if (!silent) M5Cardputer.Display.println(" Failed!");
                }
                fetchWeather();
                if (!silent) delay(1000);
                return;
            }
            if (!silent) M5Cardputer.Display.println(" Failed");
        }
    }

    startCaptivePortal(silent);
}

void startCaptivePortal(bool silent) {
    isAPMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("CyberKey-Setup", "");
    dnsServer.start(53, "*", WiFi.softAPIP());
    deviceIP = WiFi.softAPIP().toString();

    if (!silent) {
        M5Cardputer.Display.println("\n  >> Setup Mode Active <<");
        M5Cardputer.Display.println("  1. Connect to WiFi: CyberKey-Setup");
        M5Cardputer.Display.printf("  2. Open: http://%s\n", deviceIP.c_str());
        delay(2000);
    }
}

const char HTML_TOP[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>CyberKey - 2FA Vault</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #0f172a; color: #f8fafc; padding: 20px; max-width: 520px; margin: auto; }
    h1 { color: #38bdf8; text-align: center; margin-bottom: 5px; }
    p.sub { text-align: center; color: #94a3b8; font-size: 14px; margin-top: 0; }
    .card { background: #1e293b; border-radius: 12px; padding: 16px; margin-bottom: 16px; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.2); }
    h2 { font-size: 16px; color: #38bdf8; margin-top: 0; border-bottom: 1px solid #334155; padding-bottom: 8px; }
    input, select { width: 100%; box-sizing: border-box; background: #0f172a; border: 1px solid #475569; color: #fff; padding: 10px; border-radius: 8px; margin-bottom: 12px; font-size: 14px; }
    button { width: 100%; background: #0284c7; color: #fff; border: none; padding: 12px; border-radius: 8px; font-weight: bold; cursor: pointer; font-size: 14px; }
    button:hover { background: #0369a1; }
    .btn-green { background: #10b981; }
    .btn-green:hover { background: #059669; }
    .btn-purple { background: #8b5cf6; }
    .btn-purple:hover { background: #7c3aed; }
    .btn-gray { background: #475569; }
    .item { display: flex; justify-content: space-between; align-items: center; padding: 10px 0; border-bottom: 1px solid #334155; }
    .item:last-child { border-bottom: none; }
    .del-btn { background: #ef4444; width: auto; padding: 6px 12px; font-size: 12px; border-radius: 6px; }
    .del-btn:hover { background: #dc2626; }
    .edit-btn { background: #3b82f6; width: auto; padding: 6px 12px; font-size: 12px; border-radius: 6px; margin-right: 6px; }
    .edit-btn:hover { background: #2563eb; }
    .dropzone { border: 2px dashed #475569; border-radius: 8px; padding: 20px; text-align: center; color: #94a3b8; cursor: pointer; margin-bottom: 12px; }
    .dropzone:hover { border-color: #38bdf8; color: #38bdf8; }
    label { font-size: 13px; color: #94a3b8; display: block; margin-bottom: 4px; }
  </style>
</head>
<body>
  <div style="text-align: center; margin-bottom: 12px;">
    <img src="/icon.jpg" style="width: 80px; height: 80px; border-radius: 16px; box-shadow: 0 4px 12px rgba(56, 189, 248, 0.3);">
  </div>
  <h1>CyberKey Vault</h1>
  <p class="sub">Hardware 2FA & Smart Desk Clock</p>

  <div class="card">
    <h2>Scan / Upload QR Screenshot</h2>
    <div class="dropzone" onclick="document.getElementById('qrInput').click()">
      Click or Drag & Drop QR Image / Screenshot Here
      <input type="file" id="qrInput" accept="image/*" style="display:none;" onchange="decodeQR(this.files[0])">
    </div>
    <div id="qrStatus" style="font-size:13px; color:#38bdf8; margin-bottom:8px;"></div>
  </div>

  <div class="card">
    <h2 id="formTitle">Add 2FA Account Manually</h2>
    <form id="addForm" action="/add" method="POST">
      <input type="hidden" id="editIdx" name="idx" value="-1">
      <input type="text" id="accIssuer" name="issuer" placeholder="Service / Site Name (e.g. GitHub, Google, Proton)" required maxlength="20">
      <input type="text" id="accName" name="name" placeholder="Account / Username (e.g. user@example.com)" required maxlength="30">
      <input type="text" id="accSecret" name="secret" placeholder="Secret Key (Base32, spaces allowed)" required autocomplete="off">
      <button type="submit" id="submitBtn">+ Add Key to CyberKey</button>
      <button type="button" id="cancelEditBtn" onclick="cancelEdit()" style="display:none; background:#64748b; margin-top:8px;">Cancel Edit</button>
    </form>
  </div>

  <div class="card">
    <h2>Saved 2FA Accounts (%COUNT%/%MAX%)</h2>
)rawliteral";

const char HTML_BOTTOM[] PROGMEM = R"rawliteral(  </div>

  <div class="card">
    <h2>🕒 Standby Clock & Weather Settings</h2>
    <form action="/settings" method="POST">
      <label>Timezone</label>
      <select name="tz_name" id="tzSelect" onchange="updateTzPosix()">
        <option value="US/Eastern" %TZ_EST%>Eastern Time (US/New York)</option>
        <option value="US/Central" %TZ_CST%>Central Time (US/Chicago)</option>
        <option value="US/Mountain" %TZ_MST%>Mountain Time (US/Denver)</option>
        <option value="US/Pacific" %TZ_PST%>Pacific Time (US/Los Angeles)</option>
        <option value="UTC" %TZ_UTC%>UTC</option>
      </select>
      <input type="hidden" name="tz_posix" id="tzPosix" value="%TZ_POSIX%">

      <label>Clock Format</label>
      <select name="use_24h">
        <option value="0" %FMT_12%>12-Hour (AM/PM)</option>
        <option value="1" %FMT_24%>24-Hour (Military)</option>
      </select>

      <label>Standby Desk Clock</label>
      <select name="standby_en">
        <option value="1" %STBY_ON%>Enabled</option>
        <option value="0" %STBY_OFF%>Disabled</option>
      </select>

      <label>Standby Idle Timeout</label>
      <select name="standby_sec">
        <option value="30" %SEC_30%>30 Seconds</option>
        <option value="45" %SEC_45%>45 Seconds</option>
        <option value="60" %SEC_60%>1 Minute</option>
        <option value="120" %SEC_120%>2 Minutes</option>
        <option value="300" %SEC_300%>5 Minutes</option>
      </select>

      <label>Weather Location Label</label>
      <input type="text" name="w_city" value="%W_CITY%" placeholder="e.g. New York, NY">

      <div style="display: flex; gap: 8px;">
        <div style="flex: 1;">
          <label>Latitude</label>
          <input type="text" id="wLat" name="w_lat" value="%W_LAT%" required>
        </div>
        <div style="flex: 1;">
          <label>Longitude</label>
          <input type="text" id="wLon" name="w_lon" value="%W_LON%" required>
        </div>
      </div>

      <button type="button" class="btn-gray" onclick="detectLocation()" style="margin-bottom:12px;">📍 Detect My Location Automatically</button>

      <label>Temperature Units</label>
      <select name="w_units">
        <option value="fahrenheit" %UNIT_F%>Fahrenheit (°F)</option>
        <option value="celsius" %UNIT_C%>Celsius (°C)</option>
      </select>

      <button type="submit" class="btn-green">💾 Save Device Settings</button>
    </form>
  </div>

  <div class="card">
    <h2>Saved Wi-Fi Networks (%WCOUNT%/%WMAX%)</h2>
    %WIFI_LIST%
    <h3 style="font-size: 14px; color: #94a3b8; margin-top: 15px;">+ Add Another Wi-Fi Network</h3>
    <form action="/wifi_add" method="POST">
      <input type="text" name="ssid" placeholder="Wi-Fi SSID (e.g. Home, Hotspot, Work)" required>
      <input type="password" name="pass" placeholder="Wi-Fi Password">
      <button type="submit" class="btn-green">+ Add Wi-Fi Network</button>
    </form>
  </div>

  <div class="card">
    <h2>🔑 Google Password Manager Import (MicroSD)</h2>
    <p style="font-size: 13px; color: #94a3b8; margin-top: 0;">Upload your exported <code>passwords.csv</code> from Google Password Manager. Stored safely onto the MicroSD card.</p>
    <div class="dropzone" onclick="document.getElementById('csvInput').click()">
      Click or Drag &amp; Drop passwords.csv Here
      <input type="file" id="csvInput" accept=".csv,text/csv" style="display:none;" onchange="uploadCSVFile(this.files[0])">
    </div>
    <div id="csvStatus" style="font-size: 13px; color: #38bdf8; margin-bottom: 8px;"></div>
    <p style="font-size: 12px; color: #64748b; margin: 0;">Stored on MicroSD: <strong>%PASS_COUNT% passwords</strong> (%PASS_MATCHES% linked to active 2FA)</p>
  </div>

  <div class="card">
    <h2>🚀 Over-The-Air (OTA) Firmware</h2>
    <p style="font-size: 13px; color: #94a3b8; margin-top: 0;">Current: <strong>v%FW_VER%</strong> %OTA_STATUS%</p>
    <div style="display: flex; gap: 8px;">
      <form action="/ota_check" method="POST" style="flex: 1;">
        <button type="submit" class="btn-gray" style="padding: 10px;">🔍 Check Updates</button>
      </form>
      <form action="/ota_flash" method="POST" style="flex: 1;">
        <button type="submit" class="btn-purple" style="padding: 10px;" %OTA_DISABLED%>⚡ Flash Update</button>
      </form>
    </div>
  </div>
  <div class="card">
    <h2>MicroSD Card Vault Backup & Safety Guard</h2>
    <p style="font-size: 13px; color: #94a3b8; margin-top: 0;">Backups are guarded against accidental blank overwrites and include rolling <code>.bak</code> copies.</p>
    <div style="display: flex; gap: 10px; margin-bottom: 12px;">
      <form action="/backup" method="POST" style="flex:1;">
        <button type="submit" class="btn-purple">💾 Save to SD</button>
      </form>
      <form action="/restore" method="POST" style="flex:1;">
        <button type="submit" class="btn-green">📂 Restore from SD</button>
      </form>
    </div>
    <h3 style="font-size: 14px; color: #94a3b8; margin-top: 15px;">Paste or Upload Backup JSON</h3>
    <div class="dropzone" onclick="document.getElementById('jsonFile').click()" style="padding: 12px; margin-bottom: 8px;">
      Click or Drag &amp; Drop Backup .json file
      <input type="file" id="jsonFile" accept=".json,application/json" style="display:none;" onchange="loadJSONFile(this.files[0])">
    </div>
    <form action="/restore_json" method="POST">
      <textarea id="jsonPaste" name="json_content" placeholder='{"accounts":[{"issuer":"Google","name":"me@gmail.com","secret":"..."}]}' style="width:100%; height:70px; background:#0f172a; border:1px solid #475569; color:#fff; border-radius:8px; padding:8px; font-family:monospace; font-size:12px; box-sizing:border-box; margin-bottom:8px;"></textarea>
      <button type="submit" class="btn-green" style="padding:10px;">📥 Apply JSON Directly to Vault</button>
    </form>
  </div>

  <!-- Browser QR Code Scanner Library -->
  <script src="https://cdn.jsdelivr.net/npm/jsqr@1.4.0/dist/jsQR.min.js"></script>
  <script>
    function updateTzPosix() {
      const sel = document.getElementById('tzSelect').value;
      const posixInput = document.getElementById('tzPosix');
      if (sel === 'US/Eastern') posixInput.value = 'EST5EDT,M3.2.0,M11.1.0';
      else if (sel === 'US/Central') posixInput.value = 'CST6CDT,M3.2.0,M11.1.0';
      else if (sel === 'US/Mountain') posixInput.value = 'MST7MDT,M3.2.0,M11.1.0';
      else if (sel === 'US/Pacific') posixInput.value = 'PST8PDT,M3.2.0,M11.1.0';
      else if (sel === 'UTC') posixInput.value = 'UTC0';
    }

    function detectLocation() {
      if (!navigator.geolocation) {
        alert("Geolocation is not supported by your browser");
        return;
      }
      navigator.geolocation.getCurrentPosition(function(pos) {
        document.getElementById('wLat').value = pos.coords.latitude.toFixed(4);
        document.getElementById('wLon').value = pos.coords.longitude.toFixed(4);
      }, function() {
        alert("Unable to retrieve your location. Please enter coordinates manually.");
      });
    }

    function loadJSONFile(file) {
      if (!file) return;
      const reader = new FileReader();
      reader.onload = function(e) {
        document.getElementById('jsonPaste').value = e.target.result;
      };
      reader.readAsText(file);
    }

    function uploadCSVFile(file) {
      if (!file) return;
      const status = document.getElementById('csvStatus');
      status.innerText = "Uploading " + file.name + " (" + (file.size / 1024).toFixed(1) + " KB)...";

      const formData = new FormData();
      formData.append("file", file, "passwords.csv");

      const xhr = new XMLHttpRequest();
      xhr.open("POST", "/upload_csv", true);
      xhr.onload = function() {
        if (xhr.status === 200 || xhr.status === 303) {
          status.innerText = "Passwords imported successfully! Reloading...";
          setTimeout(() => { window.location.reload(); }, 1200);
        } else {
          status.innerText = "Import failed (HTTP " + xhr.status + ").";
        }
      };
      xhr.onerror = function() {
        status.innerText = "Upload error: connection interrupted.";
      };
      xhr.send(formData);
    }

    function decodeQR(file) {
      if (!file) return;
      const status = document.getElementById('qrStatus');
      status.innerText = "Analyzing QR image...";

      const reader = new FileReader();
      reader.onload = function(e) {
        const img = new Image();
        img.onload = function() {
          const canvas = document.createElement('canvas');
          canvas.width = img.width;
          canvas.height = img.height;
          const ctx = canvas.getContext('2d');
          ctx.drawImage(img, 0, 0);
          const imageData = ctx.getImageData(0, 0, img.width, img.height);
          const code = jsQR(imageData.data, imageData.width, imageData.height);

          if (code) {
            status.innerText = "QR Code found!";
            try {
              const url = new URL(code.data);
              if (url.protocol === 'otpauth:') {
                let rawPath = decodeURIComponent(url.pathname.replace(/^\/\/?totp\//i, '').replace(/^\/\/?/, ''));
                let issuer = url.searchParams.get('issuer') || '';
                let account = rawPath;

                if (rawPath.includes(':')) {
                  const parts = rawPath.split(':');
                  if (!issuer) issuer = parts[0];
                  account = parts[1];
                }

                const secret = url.searchParams.get('secret');

                if (secret) {
                  document.getElementById('accIssuer').value = issuer || "Service";
                  document.getElementById('accName').value = account || "User";
                  document.getElementById('accSecret').value = secret;
                  status.innerText = "Decoded: " + (issuer ? issuer + " (" + account + ")" : account) + " - Click '+ Add Key' to save!";
                  return;
                }
              }
            } catch(err) {}
            document.getElementById('accSecret').value = code.data;
            status.innerText = "Imported raw secret from QR code.";
          } else {
            status.innerText = "No QR code detected in image. Please try a clearer screenshot.";
          }
        };
        img.src = e.target.result;
      };
      reader.readAsDataURL(file);
    }

    function editAccount(idx, issuer, name, secret) {
      document.getElementById('editIdx').value = idx;
      document.getElementById('accIssuer').value = issuer;
      document.getElementById('accName').value = name;
      document.getElementById('accSecret').value = secret;
      document.getElementById('formTitle').innerText = "Edit 2FA Account";
      document.getElementById('submitBtn').innerText = "💾 Save Changes";
      document.getElementById('cancelEditBtn').style.display = "block";
      document.getElementById('accIssuer').focus();
      window.scrollTo({ top: 120, behavior: 'smooth' });
    }

    function cancelEdit() {
      document.getElementById('editIdx').value = "-1";
      document.getElementById('accIssuer').value = "";
      document.getElementById('accName').value = "";
      document.getElementById('accSecret').value = "";
      document.getElementById('formTitle').innerText = "Add 2FA Account Manually";
      document.getElementById('submitBtn').innerText = "+ Add Key to CyberKey";
      document.getElementById('cancelEditBtn').style.display = "none";
    }
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
    server.sendHeader("Cache-Control", "no-cache");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");

    // Part 1: Top HTML with replacements
    String top = FPSTR(HTML_TOP);
    top.replace("%COUNT%", String(accountCount));
    top.replace("%MAX%", String(MAX_ACCOUNTS));
    server.sendContent(top);

    // Part 2: Accounts List (Streamed dynamically per-account, zero memory overhead!)
    if (accountCount == 0) {
        server.sendContent("<p style='color: #94a3b8;'>No accounts stored yet.</p>");
    } else {
        for (int i = 0; i < accountCount; ++i) {
            String item = "<div class='item'>";
            String displayName = accounts[i].issuer.length() > 0 ? "<strong>" + accounts[i].issuer + "</strong> <span style='color:#94a3b8; font-size:13px;'>(" + accounts[i].name + ")</span>" : "<strong>" + accounts[i].name + "</strong>";
            item += "<div>" + displayName + "</div>";

            String isStr = accounts[i].issuer;
            isStr.replace("'", "\'");
            String nStr = accounts[i].name;
            nStr.replace("'", "\'");
            String sStr = accounts[i].secret;
            sStr.replace("'", "\'");

            item += "<div style='display:flex;'>";
            item += "<button type='button' class='edit-btn' onclick=\"editAccount(" + String(i) + ", '" + isStr + "', '" + nStr + "', '" + sStr + "')\">Edit</button>";
            item += "<form action='/del' method='POST' style='margin:0;'>";
            item += "<input type='hidden' name='idx' value='" + String(i) + "'>";
            item += "<button type='submit' class='del-btn'>Delete</button>";
            item += "</form>";
            item += "</div>";
            item += "</div>";
            server.sendContent(item);
        }
    }

    // Part 3: Bottom HTML with Settings & Wi-Fi replacements
    String bottom = FPSTR(HTML_BOTTOM);
    bottom.replace("%FW_VER%", FIRMWARE_VERSION);
    if (otaUpdateAvailable) {
        bottom.replace("%OTA_STATUS%", "<span style='color:#10b981; font-weight:bold;'>— Update " + remoteOtaVersion + " Available!</span>");
        bottom.replace("%OTA_DISABLED%", "");
    } else {
        bottom.replace("%OTA_STATUS%", "<span style='color:#64748b;'>— Up to date</span>");
        bottom.replace("%OTA_DISABLED%", "disabled style='opacity:0.5;'");
    }
    bottom.replace("%WCOUNT%", String(wifiCount));
    bottom.replace("%WMAX%", String(MAX_WIFI));
    bottom.replace("%PASS_COUNT%", String(CyberPassVault::count));
    int passLinked = 0;
    for (int i = 0; i < accountCount; ++i) {
        if (CyberPassVault::findMatch(accounts[i].issuer, accounts[i].name) >= 0) passLinked++;
    }
    bottom.replace("%PASS_MATCHES%", String(passLinked));

    // Timezone replacements
    bottom.replace("%TZ_EST%", tzName == "US/Eastern" ? "selected" : "");
    bottom.replace("%TZ_CST%", tzName == "US/Central" ? "selected" : "");
    bottom.replace("%TZ_MST%", tzName == "US/Mountain" ? "selected" : "");
    bottom.replace("%TZ_PST%", tzName == "US/Pacific" ? "selected" : "");
    bottom.replace("%TZ_UTC%", tzName == "UTC" ? "selected" : "");
    bottom.replace("%TZ_POSIX%", tzPosix);

    // Format & Standby replacements
    bottom.replace("%FMT_12%", !use24Hour ? "selected" : "");
    bottom.replace("%FMT_24%", use24Hour ? "selected" : "");
    bottom.replace("%STBY_ON%", standbyEnabled ? "selected" : "");
    bottom.replace("%STBY_OFF%", !standbyEnabled ? "selected" : "");
    bottom.replace("%SEC_30%", standbyTimeoutSec == 30 ? "selected" : "");
    bottom.replace("%SEC_45%", standbyTimeoutSec == 45 ? "selected" : "");
    bottom.replace("%SEC_60%", standbyTimeoutSec == 60 ? "selected" : "");
    bottom.replace("%SEC_120%", standbyTimeoutSec == 120 ? "selected" : "");
    bottom.replace("%SEC_300%", standbyTimeoutSec == 300 ? "selected" : "");

    // Weather replacements
    bottom.replace("%W_CITY%", weatherCity);
    bottom.replace("%W_LAT%", String(weatherLat, 4));
    bottom.replace("%W_LON%", String(weatherLon, 4));
    bottom.replace("%UNIT_F%", weatherUnits == "fahrenheit" ? "selected" : "");
    bottom.replace("%UNIT_C%", weatherUnits == "celsius" ? "selected" : "");

    String wList = "";
    if (wifiCount == 0) {
        wList = "<p style='color: #94a3b8;'>No Wi-Fi networks saved.</p>";
    } else {
        for (int i = 0; i < wifiCount; ++i) {
            wList += "<div class='item'>";
            wList += "<div>📶 <strong>" + wifiList[i].ssid + "</strong>" + (wifiList[i].ssid == connectedSSID ? " <span style='color:#10b981;'>(Connected)</span>" : "") + "</div>";
            wList += "<form action='/wifi_del' method='POST' style='margin:0;'>";
            wList += "<input type='hidden' name='idx' value='" + String(i) + "'>";
            wList += "<button type='submit' class='del-btn'>Forget</button>";
            wList += "</form>";
            wList += "</div>";
        }
    }
    bottom.replace("%WIFI_LIST%", wList);

    server.sendContent(bottom);
    server.sendContent(""); // Final empty chunk closes chunked transfer
}

void handleSettings() {
    if (server.hasArg("tz_posix")) {
        tzPosix = server.arg("tz_posix");
        if (server.hasArg("tz_name")) tzName = server.arg("tz_name");
        if (server.hasArg("use_24h")) use24Hour = server.arg("use_24h") == "1";
        if (server.hasArg("standby_en")) standbyEnabled = server.arg("standby_en") == "1";
        if (server.hasArg("standby_sec")) standbyTimeoutSec = server.arg("standby_sec").toInt();
        if (server.hasArg("w_lat")) weatherLat = server.arg("w_lat").toFloat();
        if (server.hasArg("w_lon")) weatherLon = server.arg("w_lon").toFloat();
        if (server.hasArg("w_city")) weatherCity = server.arg("w_city");
        if (server.hasArg("w_units")) weatherUnits = server.arg("w_units");

        saveSettings();
        applyTimezone();
        fetchWeather();
        statusMessage = "Settings Saved!";
        statusMsgUntil = millis() + 2500;
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleAdd() {
    if (server.hasArg("name") && server.hasArg("secret")) {
        int editIdx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
        String issuer = server.hasArg("issuer") ? server.arg("issuer") : "";
        String name = server.arg("name");
        String secret = server.arg("secret");
        secret.replace(" ", "");
        secret.replace("-", "");
        secret.toUpperCase();

        if (editIdx >= 0 && editIdx < accountCount) {
            accounts[editIdx].issuer = issuer;
            accounts[editIdx].name = name;
            accounts[editIdx].secret = secret;
            saveAccounts();
        } else if (accountCount < MAX_ACCOUNTS) {
            accounts[accountCount].issuer = issuer;
            accounts[accountCount].name = name;
            accounts[accountCount].secret = secret;
            accountCount++;
            saveAccounts();
        }
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleDelete() {
    if (server.hasArg("idx")) {
        int idx = server.arg("idx").toInt();
        if (idx >= 0 && idx < accountCount) {
            for (int i = idx; i < accountCount - 1; ++i) {
                accounts[i] = accounts[i + 1];
            }
            accountCount--;
            if (currentIndex >= accountCount && accountCount > 0) {
                currentIndex = accountCount - 1;
            }
            saveAccounts();
        }
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleWiFiAdd() {
    if (server.hasArg("ssid")) {
        if (wifiCount < MAX_WIFI) {
            String ssid = server.arg("ssid");
            String pass = server.arg("pass");

            bool exists = false;
            for (int i = 0; i < wifiCount; ++i) {
                if (wifiList[i].ssid == ssid) {
                    wifiList[i].pass = pass;
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                wifiList[wifiCount].ssid = ssid;
                wifiList[wifiCount].pass = pass;
                wifiCount++;
            }
            saveWiFi();
        }
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleWiFiDel() {
    if (server.hasArg("idx")) {
        int idx = server.arg("idx").toInt();
        if (idx >= 0 && idx < wifiCount) {
            for (int i = idx; i < wifiCount - 1; ++i) {
                wifiList[i] = wifiList[i + 1];
            }
            wifiCount--;
            saveWiFi();
        }
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleBackup() {
    backupToSD();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleRestore() {
    restoreFromSD();
    statusMsgUntil = millis() + 3000;
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleRestoreJSON() {
    if (server.hasArg("json_content")) {
        String json = server.arg("json_content");
        if (parseAndApplyBackupJSON(json)) {
            backupToSD(true); // Persist safely onto SD
            statusMessage = "JSON Vault Restored!";
        } else {
            statusMessage = "Invalid Backup JSON";
        }
        statusMsgUntil = millis() + 3000;
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleIcon() {
    server.sendHeader("Cache-Control", "max-age=86400");
    server.send_P(200, "image/jpeg", (const char*)ICON_JPEG_BYTES, ICON_JPEG_LEN);
}

void handleOTACheck() {
    checkForOTAUpdates(true);
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleOTAFlash() {
    server.send(200, "text/html", "<html><body style='background:#0f172a;color:#fff;font-family:sans-serif;text-align:center;padding:50px;'><h2>Flashing Firmware Update...</h2><p>CyberKey is updating and will reboot in 10-15 seconds.</p><script>setTimeout(()=>{window.location.href='/';},15000);</script></body></html>");
    delay(200);
    performOTAUpdate();
}

static File csvUploadFile;
static bool csvUploadPendingParse = false;

void handleUploadCSVProcess() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        ensureSD();
        if (SD.exists("/passwords.csv")) SD.remove("/passwords.csv");
        csvUploadFile = SD.open("/passwords.csv", FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (csvUploadFile) {
            csvUploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (csvUploadFile) {
            csvUploadFile.close();
            csvUploadPendingParse = true;
        }
    }
}

void handleUploadCSVComplete() {
    server.send(200, "text/plain", "OK");
    if (csvUploadPendingParse) {
        csvUploadPendingParse = false;
        File readFile = SD.open("/passwords.csv", FILE_READ);
        int imported = 0;
        if (readFile) {
            imported = CyberPassVault::importCSVFile(readFile);
            readFile.close();
            SD.remove("/passwords.csv");
        }
        statusMessage = "Imported " + String(imported) + " Passwords!";
        statusMsgUntil = millis() + 3500;
    }
}

void setupWebServer() {
    server.on("/", handleRoot);
    server.on("/icon.jpg", handleIcon);
    server.on("/ota_check", HTTP_POST, handleOTACheck);
    server.on("/ota_flash", HTTP_POST, handleOTAFlash);
    server.on("/add", HTTP_POST, handleAdd);
    server.on("/del", HTTP_POST, handleDelete);
    server.on("/settings", HTTP_POST, handleSettings);
    server.on("/wifi_add", HTTP_POST, handleWiFiAdd);
    server.on("/wifi_del", HTTP_POST, handleWiFiDel);
    server.on("/backup", HTTP_POST, handleBackup);
    server.on("/restore", HTTP_POST, handleRestore);
    server.on("/restore_json", HTTP_POST, handleRestoreJSON);
    server.on("/upload_csv", HTTP_POST, handleUploadCSVComplete, handleUploadCSVProcess);
    server.onNotFound([]() {
        if (isAPMode) {
            server.sendHeader("Location", "http://" + deviceIP + "/");
            server.send(302, "text/plain", "");
        } else {
            server.send(404, "text/plain", "Not Found");
        }
    });
    server.begin();
}
