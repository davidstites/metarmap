/*
 * Aviation Weather Map - Complete Arduino-Compatible Version
 * Features: Adjustable brightness, wind warnings, thunderstorm flashing
 * Settings persist across reboots in LittleFS
 */

#include <Arduino.h>
#include <AsyncTCP.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <FastLED.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <WiFiClientSecure.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

#define SW_VERS "1.4"

// Hardware

#define DIG2GO 1

#ifdef DIG2GO
#define BUTTON_PIN 0
#define DATA_PIN 16
#define LED_POWER_PIN 12
#else
#define LED_INTERNAL_PIN 2
#define DATA_PIN 25
#define BUTTON_PIN 22
#endif
#define BUTTON_LONG_PRESS_DURATION 7000
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

// LED Settings
#define MAX_AIRPORTS 50
#define DEFAULT_BRIGHTNESS 30
#define WIND_THRESHOLD 25

// Timing
#define LOOP_INTERVAL 5000
#define REQUEST_INTERVAL 900000
#define RETRY_TIMEOUT 15000
#define HTTP_TIMEOUT 10000
#define MEMORY_CHECK_INTERVAL 30000
#define WIFI_CHECK_INTERVAL 10000
#define WIFI_CONNECT_TIMEOUT 20000
#define TEMPLATE_CACHE_DURATION 300000
#define THUNDERSTORM_FLASH_INTERVAL 100
#define WIND_BLINK_INTERVAL 1000

// Network
#define AVIATION_WEATHER_HOST "aviationweather.gov"
#define AVIATION_WEATHER_PORT 443
#define MAX_WIFI_RETRIES 3

// Memory
#define CRITICAL_MEMORY_THRESHOLD 8000
#define MIN_HEAP_FOR_OPERATION 8000

// Files
#define SSID_PATH "/ssid.txt"
#define PASS_PATH "/pass.txt"
#define SETTINGS_PATH "/settings.json"

// Sizes
#define ICAO_CODE_LENGTH 4
#define ICAO_BUFFER_SIZE 5
#define CREDENTIAL_BUFFER_SIZE 64
#define STATUS_BUFFER_SIZE 64

// Features
#define DO_WINDS true
#define DO_THUNDERSTORMS true

// Colors
namespace LEDColors {
    const CRGB VFR = CRGB::Green;
    const CRGB MVFR = CRGB::Blue;
    const CRGB IFR = CRGB::Red;
    const CRGB LIFR = CRGB::Magenta;
    const CRGB NO_DATA = CRGB::OrangeRed;
    const CRGB OFF = CRGB::Black;
    const CRGB WIND_WARNING = 0xFFC003;
    const CRGB THUNDERSTORM = CRGB::White;
}

// Flight Category Thresholds
namespace FlightCategory {
    const int LIFR_CEILING = 500;
    const float LIFR_VISIBILITY = 1.0;
    const int IFR_CEILING = 1000;
    const float IFR_VISIBILITY = 3.0;
    const int MVFR_CEILING = 3000;
    const float MVFR_VISIBILITY = 5.0;
}

// Utilities
inline uint32_t millisToSeconds(uint32_t ms) { return ms / 1000; }
inline uint32_t secondsToMillis(uint32_t s) { return s * 1000; }

#define safeCopy(dest, src) do { \
    if (src) { \
        strncpy(dest, src, sizeof(dest) - 1); \
        dest[sizeof(dest) - 1] = '\0'; \
    } else { \
        dest[0] = '\0'; \
    } \
} while(0)

// ============================================================================
// DATA STRUCTURES
// ============================================================================

enum METAR_STATUS {
    SUCCESS = 0,
    CONNECTION_FAILURE = -1,
    CONNECTION_TIMEOUT = -2,
    PARSING_FAILURE = -3,
    RESPONSE_FAILURE = -4,
};

struct METAR {
    char icaoId[ICAO_BUFFER_SIZE];
    char receiptTime[24];
    char obsTime[24];
    char rawText[128];
    char fltcat[5];
    char cloudConditions[64];
    
    float temp;
    float dewp;
    float wspd;
    int wdir;
    float visib;
    float altim;
    uint16_t flags;
    
    static const uint16_t HAS_TEMP = 0x01;
    static const uint16_t HAS_DEWP = 0x02;
    static const uint16_t HAS_WSPD = 0x04;
    static const uint16_t HAS_WDIR = 0x08;
    static const uint16_t HAS_VISIB = 0x10;
    static const uint16_t HAS_ALTIM = 0x20;
    static const uint16_t HAS_FLTCAT = 0x40;
    static const uint16_t HAS_THUNDERSTORM = 0x80;
    
    METAR() : temp(0), dewp(0), wspd(0), wdir(0), visib(0), altim(0), flags(0) {
        memset(icaoId, 0, sizeof(icaoId));
        memset(receiptTime, 0, sizeof(receiptTime));
        memset(obsTime, 0, sizeof(obsTime));
        memset(rawText, 0, sizeof(rawText));
        memset(fltcat, 0, sizeof(fltcat));
        memset(cloudConditions, 0, sizeof(cloudConditions));
    }
    
    bool hasTemp() const { return flags & HAS_TEMP; }
    bool hasDewp() const { return flags & HAS_DEWP; }
    bool hasWspd() const { return flags & HAS_WSPD; }
    bool hasWdir() const { return flags & HAS_WDIR; }
    bool hasVisib() const { return flags & HAS_VISIB; }
    bool hasAltim() const { return flags & HAS_ALTIM; }
    bool hasFltcat() const { return flags & HAS_FLTCAT; }
    bool hasThunderstorm() const { return flags & HAS_THUNDERSTORM; }
    
    void setTemp(float value) { temp = value; flags |= HAS_TEMP; }
    void setDewp(float value) { dewp = value; flags |= HAS_DEWP; }
    void setWspd(float value) { wspd = value; flags |= HAS_WSPD; }
    void setWdir(int value) { wdir = value; flags |= HAS_WDIR; }
    void setVisib(float value) { visib = value; flags |= HAS_VISIB; }
    void setAltim(float value) { altim = value; flags |= HAS_ALTIM; }
    void setFltcat(const char* value) { safeCopy(fltcat, value); flags |= HAS_FLTCAT; }
    void setThunderstorm(bool value) { 
        if (value) flags |= HAS_THUNDERSTORM; 
        else flags &= ~HAS_THUNDERSTORM; 
    }
    
    bool isValid() const {
        return strlen(icaoId) == ICAO_CODE_LENGTH && hasFltcat();
    }
};

struct AirportLED {
    char icaoCode[ICAO_BUFFER_SIZE];
    METAR metarData;
    bool hasValidData;
    
    AirportLED() : hasValidData(false) {
        memset(icaoCode, 0, sizeof(icaoCode));
    }
    
    AirportLED(const char* code) : hasValidData(false) {
        setIcaoCode(code);
    }
    
    void setIcaoCode(const char* code) {
        safeCopy(icaoCode, code);
        hasValidData = false;
    }
    
    bool isEmpty() const { return strlen(icaoCode) == 0; }
};

// ============================================================================
// SYSTEM STATE
// ============================================================================

Preferences preferences;

struct SystemState {
    char lastFetchTime[32];
    char lastAttemptTime[32];
    char programStatus[STATUS_BUFFER_SIZE];
    METAR_STATUS lastFetchStatus;
    unsigned long programStartTime;
    bool ledPowerState;
    uint8_t ledBrightness;
    bool windCheckEnabled;
    bool thunderstormsCheckEnabled;
    uint8_t windCheckThreshold;
    
    void init() {
        safeCopy(lastFetchTime, "Never");
        safeCopy(lastAttemptTime, "Never");
        safeCopy(programStatus, "Starting up");
        lastFetchStatus = SUCCESS;
        programStartTime = millis();
        preferences.begin("metarmap", false);
        loadSettings();
    }
    
    void loadSettings() {
        File file = LittleFS.open(SETTINGS_PATH, "r");
        if (!file) {
            Serial.println("No settings file, using defaults");
            ledBrightness = DEFAULT_BRIGHTNESS;
            windCheckEnabled = DO_WINDS;
            thunderstormsCheckEnabled = DO_THUNDERSTORMS;
            windCheckThreshold = WIND_THRESHOLD;
            ledPowerState = true;
            return;
        }
        
        String content = file.readString();
        file.close();
        
        int brightIdx = content.indexOf("\"brightness\":");
        if (brightIdx >= 0) {
            int valStart = content.indexOf(':', brightIdx) + 1;
            int valEnd = content.indexOf(',', valStart);
            if (valEnd < 0) valEnd = content.indexOf('}', valStart);
            String val = content.substring(valStart, valEnd);
            val.trim();
            ledBrightness = constrain(val.toInt(), 0, 255);
        } else {
            ledBrightness = DEFAULT_BRIGHTNESS;
        }
        
        int windIdx = content.indexOf("\"windEnabled\":");
        if (windIdx >= 0) {
            windCheckEnabled = (content.indexOf("true", windIdx) > windIdx && 
                                content.indexOf("true", windIdx) < windIdx + 20);
        } else {
            windCheckEnabled = DO_WINDS;
        }
        
        int tsIdx = content.indexOf("\"thunderstormsEnabled\":");
        if (tsIdx >= 0) {
            thunderstormsCheckEnabled = (content.indexOf("true", tsIdx) > tsIdx && 
                                        content.indexOf("true", tsIdx) < tsIdx + 30);
        } else {
            thunderstormsCheckEnabled = DO_THUNDERSTORMS;
        }

        int thresholdIdx = content.indexOf("\"windThreshold\":");
        if (thresholdIdx >= 0) {
            int valStart = content.indexOf(':', thresholdIdx) + 1;
            int valEnd = content.indexOf(',', valStart);
            if (valEnd < 0) valEnd = content.indexOf('}', valStart);
            String val = content.substring(valStart, valEnd);
            val.trim();
            windCheckThreshold = constrain(val.toInt(), 5, 100);
        } else {
            windCheckThreshold = WIND_THRESHOLD;
        }

        ledPowerState = preferences.getBool("ledsEnabled", true);
        
        Serial.println("Settings loaded:");
        Serial.printf("  LED power state: %s", ledPowerState ? "on" : "off");
        Serial.printf("  Brightness: %d\n", ledBrightness);
        Serial.printf("  Wind warnings: %s\n", windCheckEnabled ? "enabled" : "disabled");
        Serial.printf("  Wind threshold: %d kt\n", windCheckThreshold); 
        Serial.printf("  Thunderstorms: %s\n", thunderstormsCheckEnabled ? "enabled" : "disabled");
    }
    
    void saveSettings() {
        File file = LittleFS.open(SETTINGS_PATH, "w");
        if (!file) {
            Serial.println("Failed to save settings");
            return;
        }
        
        file.print("{\"brightness\":");
        file.print(ledBrightness);
        file.print(",\"windEnabled\":");
        file.print(windCheckEnabled ? "true" : "false");
        file.print(",\"thunderstormsEnabled\":");
        file.print(thunderstormsCheckEnabled ? "true" : "false");
        file.print(",\"windThreshold\":");
        file.print(windCheckThreshold);
        file.print("}");
        file.close();
        
        Serial.println("Settings saved");
    }
    
    void updateTime(char* buffer) {
        time_t now = time(nullptr);
        char* timeStr = ctime(&now);
        if (timeStr) {
            safeCopy(buffer, timeStr);
            char* newline = strchr(buffer, '\n');
            if (newline) *newline = '\0';
        }
    }
    
    void updateFetchTime() { updateTime(lastFetchTime); }
    void updateAttemptTime() { updateTime(lastAttemptTime); }
    void setStatus(const char* status) { safeCopy(programStatus, status); }
    void setFetchStatus(METAR_STATUS status) { lastFetchStatus = status; }
    
    void setBrightness(uint8_t brightness) {
        ledBrightness = constrain(brightness, 0, 255);
        saveSettings();
    }
    
    uint8_t getBrightness() const { return ledBrightness; }
    
    void setWindEnabled(bool enabled) { 
        windCheckEnabled = enabled;
        saveSettings();
    }
    bool windEnabled() const { return windCheckEnabled; }
    
    void setThunderstormsEnabled(bool enabled) { 
        thunderstormsCheckEnabled = enabled;
        saveSettings();
    }
    bool thunderstormsEnabled() const { return thunderstormsCheckEnabled; }

    void setWindThreshold(uint8_t threshold) {
        windCheckThreshold = constrain(threshold, 5, 100);
        saveSettings();
    }
    uint8_t windThreshold() const { return windCheckThreshold; }

    void setLEDState(bool state) {
        ledPowerState = state;
        preferences.putBool("ledsEnabled", ledPowerState);
    }
    bool ledsEnabled() const { return ledPowerState; }
    
    bool checkMemory() {
        if (ESP.getFreeHeap() < CRITICAL_MEMORY_THRESHOLD) {
            Serial.println("CRITICAL: Low memory, restarting...");
            delay(2000);
            ESP.restart();
            return false;
        }
        return true;
    }
};

SystemState g_state;

// ============================================================================
// METAR PARSER
// ============================================================================

class MetarParser {
public:
    static float parseVisibility(const char* visStr) {
        if (!visStr) return 0.0;
        String vis = String(visStr);
        vis.trim();
        
        if (vis.endsWith("+")) {
            return vis.substring(0, vis.length() - 1).toFloat();
        }
        
        int slashPos = vis.indexOf('/');
        if (slashPos > 0 && vis.indexOf(' ') < 0) {
            float num = vis.substring(0, slashPos).toFloat();
            float denom = vis.substring(slashPos + 1).toFloat();
            return (denom > 0) ? num / denom : 0.0;
        }
        
        int spacePos = vis.indexOf(' ');
        if (spacePos > 0) {
            float whole = vis.substring(0, spacePos).toFloat();
            float fraction = parseVisibility(vis.substring(spacePos + 1).c_str());
            return whole + fraction;
        }
        
        return vis.toFloat();
    }
    
    static float parseTemperatureValue(const char* tempStr) {
        if (!tempStr) return 0.0;
        bool negative = (tempStr[0] == 'M');
        float temp = atof(negative ? tempStr + 1 : tempStr);
        return negative ? -temp : temp;
    }
    
    static int calculateCeiling(const char* metar) {
        int ceiling = 0;
        
        const char* bknPtr = strstr(metar, "BKN");
        const char* ovcPtr = strstr(metar, "OVC");
        
        if (bknPtr) {
            const char* altPtr = bknPtr + 3;
            if (isdigit(altPtr[0]) && isdigit(altPtr[1]) && isdigit(altPtr[2])) {
                char altStr[4] = {altPtr[0], altPtr[1], altPtr[2], '\0'};
                ceiling = atoi(altStr) * 100;
            }
        }
        
        if (ovcPtr) {
            const char* altPtr = ovcPtr + 3;
            if (isdigit(altPtr[0]) && isdigit(altPtr[1]) && isdigit(altPtr[2])) {
                char altStr[4] = {altPtr[0], altPtr[1], altPtr[2], '\0'};
                int ovcCeiling = atoi(altStr) * 100;
                if (ceiling == 0 || ovcCeiling < ceiling) {
                    ceiling = ovcCeiling;
                }
            }
        }
        
        return ceiling;
    }
    
    static String deriveFlightCategory(float visibility, int ceiling) {
        using namespace FlightCategory;
        
        if ((ceiling > 0 && ceiling < LIFR_CEILING) || 
            (visibility > 0 && visibility < LIFR_VISIBILITY)) {
            return "LIFR";
        }
        
        if ((ceiling > 0 && ceiling >= LIFR_CEILING && ceiling < IFR_CEILING) || 
            (visibility > 0 && visibility >= LIFR_VISIBILITY && visibility < IFR_VISIBILITY)) {
            return "IFR";
        }
        
        if ((ceiling > 0 && ceiling >= IFR_CEILING && ceiling <= MVFR_CEILING) || 
            (visibility > 0 && visibility >= IFR_VISIBILITY && visibility < MVFR_VISIBILITY)) {
            return "MVFR";
        }
        
        return "VFR";
    }
    
    static METAR parse(const char* rawMetarLine) {
        METAR metar;
        
        if (!rawMetarLine || strlen(rawMetarLine) < 10) {
            return metar;
        }
        
        safeCopy(metar.rawText, rawMetarLine);
        Serial.printf("📋 Parsing: %s\n", rawMetarLine);
        
        // Check for thunderstorms in weather phenomena (not in airport codes)
        // Thunderstorms appear in present weather section, typically after visibility
        // Common patterns: TS, TSRA, TSGR, +TSRA, -TS, VCTS, etc.
        // Look for TS after the wind (which ends with KT) to avoid matching airport codes
        const char* ktPtr = strstr(rawMetarLine, "KT");
        if (ktPtr != nullptr) {
            // Search for TS after the wind indicator
            const char* tsPtr = strstr(ktPtr, "TS");
            if (tsPtr != nullptr) {
                // Check if it's TSNO (no longer occurring)
                if (strncmp(tsPtr, "TSNO", 4) != 0) {
                    metar.setThunderstorm(true);
                    Serial.println("⚡ Thunderstorm detected!");
                } else {
                    Serial.println("TSNO detected - thunderstorms no longer occurring");
                }
            }
        }
        
        // Extract ICAO
        if (strlen(rawMetarLine) >= 10) {
            char icao[ICAO_BUFFER_SIZE];
            strncpy(icao, rawMetarLine + 6, ICAO_CODE_LENGTH);
            icao[ICAO_CODE_LENGTH] = '\0';
            safeCopy(metar.icaoId, icao);
        }
        
        // Extract wind
        const char* windPtr = strstr(rawMetarLine, "KT");
        if (windPtr && windPtr > rawMetarLine + 4) {
            const char* windStart = windPtr - 5;
            if (windStart >= rawMetarLine) {
                if (isdigit(windStart[0]) && isdigit(windStart[1]) && isdigit(windStart[2])) {
                    char dirStr[4] = {windStart[0], windStart[1], windStart[2], '\0'};
                    metar.setWdir(atoi(dirStr));
                }
                if (isdigit(windStart[3]) && isdigit(windStart[4])) {
                    char spdStr[3] = {windStart[3], windStart[4], '\0'};
                    metar.setWspd(atof(spdStr));
                }
            }
        }
        
        // Extract visibility
        const char* visPtr = strstr(rawMetarLine, "SM");
        if (visPtr && visPtr > rawMetarLine) {
            const char* visStart = visPtr - 1;
            while (visStart > rawMetarLine && 
                   (isdigit(*(visStart-1)) || *(visStart-1) == '/' || *(visStart-1) == '+')) {
                visStart--;
            }
            if (visStart < visPtr) {
                char visStr[10];
                int len = visPtr - visStart;
                if (len >= sizeof(visStr)) len = sizeof(visStr) - 1;
                strncpy(visStr, visStart, len);
                visStr[len] = '\0';
                metar.setVisib(parseVisibility(visStr));
            }
        }
        
        // Extract temperature
        const char* tempPtr = strchr(rawMetarLine, '/');
        if (tempPtr && tempPtr > rawMetarLine + 10) {
            const char* tempStart = tempPtr - 1;
            while (tempStart > rawMetarLine && (isdigit(*(tempStart-1)) || *(tempStart-1) == 'M')) {
                tempStart--;
            }
            if (tempStart < tempPtr) {
                char tempStr[6];
                int tempLen = tempPtr - tempStart;
                if (tempLen >= sizeof(tempStr)) tempLen = sizeof(tempStr) - 1;
                strncpy(tempStr, tempStart, tempLen);
                tempStr[tempLen] = '\0';
                metar.setTemp(parseTemperatureValue(tempStr));
                
                const char* dewStart = tempPtr + 1;
                const char* dewEnd = dewStart;
                while (*dewEnd && (isdigit(*dewEnd) || *dewEnd == 'M')) {
                    dewEnd++;
                }
                if (dewEnd > dewStart) {
                    char dewStr[6];
                    int dewLen = dewEnd - dewStart;
                    if (dewLen >= sizeof(dewStr)) dewLen = sizeof(dewStr) - 1;
                    strncpy(dewStr, dewStart, dewLen);
                    dewStr[dewLen] = '\0';
                    metar.setDewp(parseTemperatureValue(dewStr));
                }
            }
        }
        
        // Extract altimeter
        const char* altPtr = strchr(rawMetarLine, 'A');
        if (altPtr && strlen(altPtr) >= 5) {
            char altStr[5];
            strncpy(altStr, altPtr + 1, 4);
            altStr[4] = '\0';
            if (isdigit(altStr[0]) && isdigit(altStr[1]) && 
                isdigit(altStr[2]) && isdigit(altStr[3])) {
                metar.setAltim(atof(altStr) / 100.0);
            }
        }
        
        // Derive flight category
        int ceiling = calculateCeiling(rawMetarLine);
        String fltcat = deriveFlightCategory(metar.visib, ceiling);
        metar.setFltcat(fltcat.c_str());
        
        if (metar.isValid()) {
            Serial.printf("✈️ %s: %s, vis=%.1f SM, temp=%.1f°C, wind=%d°@%.0fkt\n",
                         metar.icaoId, metar.fltcat, metar.visib, 
                         metar.temp, metar.wdir, metar.wspd);
        }
        
        return metar;
    }
};

// ============================================================================
// WIFI MANAGER
// ============================================================================

class WiFiManager {
private:
    char ssid[CREDENTIAL_BUFFER_SIZE];
    char pass[CREDENTIAL_BUFFER_SIZE];
    char hostname[32];
    int reconnectFailures;
    
    bool readFile(const char* path, char* buffer, size_t bufferSize) {
        File file = LittleFS.open(path, "r");
        if (!file || file.isDirectory()) {
            buffer[0] = '\0';
            return false;
        }
        size_t bytesRead = 0;
        while (file.available() && bytesRead < bufferSize - 1) {
            char c = file.read();
            if (c == '\n' || c == '\r') break;
            buffer[bytesRead++] = c;
        }
        buffer[bytesRead] = '\0';
        file.close();
        return true;
    }
    
    void writeFile(const char* path, const char* message) {
        File file = LittleFS.open(path, "w");
        if (!file) return;
        file.print(message);
        file.close();
    }
    
    void startMDNS() {
        MDNS.end();
        delay(100);
        if (MDNS.begin(hostname)) {
            Serial.printf("mDNS started: http://%s.local\n", hostname);
            MDNS.addService("http", "tcp", 80);
        }
    }
    
public:
    WiFiManager(const char* host) : reconnectFailures(0) {
        safeCopy(hostname, host);
        ssid[0] = '\0';
        pass[0] = '\0';
    }
    
    bool loadCredentials() {
        readFile(SSID_PATH, ssid, sizeof(ssid));
        readFile(PASS_PATH, pass, sizeof(pass));
        Serial.printf("WiFi SSID: %s\n", ssid);
        return strlen(ssid) > 0;
    }
    
    bool connect() {
        if (strlen(ssid) == 0) return false;
        
        WiFi.mode(WIFI_STA);
        if (strlen(pass) == 0) {
            WiFi.begin(ssid, NULL);
        } else {
            WiFi.begin(ssid, pass);
        }
        Serial.println("Connecting to WiFi...");
        
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && 
               (millis() - startTime) < WIFI_CONNECT_TIMEOUT) {
            delay(500);
            Serial.print(".");
        }
        Serial.println();
        
        if (WiFi.status() != WL_CONNECTED) return false;
        
        Serial.printf("📡 WiFi connected: %s\n", WiFi.localIP().toString().c_str());
        startMDNS();
        reconnectFailures = 0;
        return true;
    }
    
    bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
    bool hasTooManyFailures() const { return reconnectFailures >= MAX_WIFI_RETRIES; }
    int getSignalStrength() const { return WiFi.RSSI(); }
    IPAddress getIPAddress() const { return WiFi.localIP(); }
    const char* getSSID() const { return ssid; }
    
    bool attemptReconnection() {
        Serial.println("❌ WiFi lost. Reconnecting...");
        WiFi.disconnect();
        WiFi.reconnect();
        
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && 
               (millis() - startTime) < WIFI_CONNECT_TIMEOUT) {
            delay(500);
            Serial.print(".");
        }
        Serial.println();
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("✅ Reconnected!");
            startMDNS();
            reconnectFailures = 0;
            return true;
        }
        
        reconnectFailures++;
        Serial.printf("Failed (%d/%d)\n", reconnectFailures, MAX_WIFI_RETRIES);
        return false;
    }
    
    void saveCredentials(const char* newSsid, const char* newPass) {
        safeCopy(ssid, newSsid);
        safeCopy(pass, newPass);
        writeFile(SSID_PATH, ssid);
        writeFile(PASS_PATH, pass);
    }
    
    void resetCredentials() {
        LittleFS.remove(SSID_PATH);
        LittleFS.remove(PASS_PATH);
        ssid[0] = '\0';
        pass[0] = '\0';
    }
    
    void startAccessPoint(const char* apName) {
        Serial.println("Starting AP mode");
        WiFi.softAP(apName, nullptr);
        Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    }
};

// ============================================================================
// WEATHER API CLIENT
// ============================================================================

class WeatherAPIClient {
public:
    METAR_STATUS fetchWeatherData(AirportLED* airports, int numAirports) {
        Serial.println("\n=== Fetching Weather ===");
        Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
        
        if (ESP.getFreeHeap() < MIN_HEAP_FOR_OPERATION) {
            Serial.println("ERROR: Insufficient memory");
            return CONNECTION_FAILURE;
        }
        
        String apiPath = buildAPIPath(airports, numAirports);
        String rawData;
        METAR_STATUS status = fetchRawData(apiPath, rawData);
        
        if (status != SUCCESS) return status;
        
        return parseWeatherData(rawData, airports, numAirports);
    }
    
private:
    String buildAPIPath(AirportLED* airports, int numAirports) {
        String path = "/api/data/metar?ids=";
        for (int i = 0; i < numAirports; i++) {
            if (strlen(airports[i].icaoCode) == 0) continue;
            if (!path.endsWith("=")) path += ",";
            path += airports[i].icaoCode;
        }
        path += "&format=raw&taf=false&hours=1";
        return path;
    }
    
    METAR_STATUS fetchRawData(const String& apiPath, String& rawData) {
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(20000);
        
        bool connected = false;
        for (int attempt = 1; attempt <= 3; attempt++) {
            Serial.printf("Connection attempt %d/3\n", attempt);
            if (client.connect(AVIATION_WEATHER_HOST, AVIATION_WEATHER_PORT)) {
                connected = true;
                Serial.println("✅ Connected");
                break;
            }
            delay(2000);
        }
        
        if (!connected) {
            client.stop();
            return CONNECTION_FAILURE;
        }
        
        String request = "GET " + apiPath + " HTTP/1.1\r\n" +
                        "Host: aviationweather.gov\r\n" +
                        "User-Agent: ESP32\r\n" +
                        "Connection: close\r\n\r\n";
        
        if (client.print(request) == 0) {
            client.stop();
            return CONNECTION_FAILURE;
        }
        
        unsigned long startTime = millis();
        while (!client.available() && client.connected() && 
               (millis() - startTime) < 25000) {
            delay(250);
        }
        
        if (!client.available()) {
            client.stop();
            return CONNECTION_TIMEOUT;
        }
        
        String fullResponse = "";
        while (client.available()) {
            fullResponse += client.readString();
        }
        client.stop();
        
        Serial.printf("Response: %d bytes\n", fullResponse.length());
        
        if (fullResponse.indexOf("200 OK") < 0) {
            return RESPONSE_FAILURE;
        }
        
        int headerEnd = fullResponse.indexOf("\r\n\r\n");
        if (headerEnd < 0) headerEnd = fullResponse.indexOf("\n\n");
        if (headerEnd < 0) return RESPONSE_FAILURE;
        
        headerEnd += (fullResponse.charAt(headerEnd + 2) == '\r') ? 4 : 2;
        rawData = fullResponse.substring(headerEnd);
        rawData.trim();
        
        return (rawData.length() > 0) ? SUCCESS : RESPONSE_FAILURE;
    }
    
    METAR_STATUS parseWeatherData(const String& rawData, AirportLED* airports, int numAirports) {
        Serial.println("=== Parsing METAR Data ===");
        
        int parsedCount = 0;
        int startPos = 0;
        
        while (true) {
            int endPos = rawData.indexOf('\n', startPos);
            if (endPos == -1) break;
            
            String line = rawData.substring(startPos, endPos);
            line.trim();
            
            if (line.length() > 10) {
                METAR metar = MetarParser::parse(line.c_str());
                
                if (strlen(metar.icaoId) > 0) {
                    for (int i = 0; i < numAirports; i++) {
                        if (strcmp(airports[i].icaoCode, metar.icaoId) == 0) {
                            airports[i].metarData = metar;
                            airports[i].hasValidData = true;
                            parsedCount++;
                            break;
                        }
                    }
                }
            }
            
            startPos = endPos + 1;
        }
        
        Serial.printf("📊 Parsed %d reports\n", parsedCount);
        return (parsedCount > 0) ? SUCCESS : PARSING_FAILURE;
    }
};

// ============================================================================
// LED CONTROLLER
// ============================================================================

class LEDController {
private:
    CRGB* leds;
#if !DIG2GO
    CRGB* internalLeds;
#endif
    int numLeds;
    CRGB* baseColors;
    unsigned long lastFlashTime;
    unsigned long lastWindBlinkTime;
    bool flashState;
    bool windBlinkState;
    
    CRGB getFlightCategoryColor(const char* fltcat) {
        if (strcmp(fltcat, "VFR") == 0) return LEDColors::VFR;
        if (strcmp(fltcat, "MVFR") == 0) return LEDColors::MVFR;
        if (strcmp(fltcat, "IFR") == 0) return LEDColors::IFR;
        if (strcmp(fltcat, "LIFR") == 0) return LEDColors::LIFR;
        return LEDColors::NO_DATA;
    }
    
public:
    LEDController(CRGB* leds, int numLeds) 
        : leds(leds), numLeds(numLeds), lastFlashTime(0), lastWindBlinkTime(0), 
          flashState(false), windBlinkState(false) {
#if !DIG2GO
        internalLeds = new CRGB[1];
#endif
        baseColors = new CRGB[numLeds];
    }
    
    ~LEDController() {
#if !DIG2GO
        delete[] internalLeds;
#endif
        delete[] baseColors;
    }
    
    void begin() {
        FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, numLeds).setCorrection(TypicalLEDStrip);
#if !DIG2GO
        FastLED.addLeds<LED_TYPE, LED_INTERNAL_PIN, COLOR_ORDER>(internalLeds, 1).setCorrection(TypicalLEDStrip);
#endif        
        FastLED.setBrightness(g_state.getBrightness());
        Serial.printf("LEDs initialized: %d\n", numLeds);
#if !DIG2GO
        fill_solid(internalLeds, 1, CRGB::White);
#endif
        setAll(LEDColors::NO_DATA);
    }
    
    void updateFromAirports(const AirportLED* airports, int numAirports) {
        Serial.println("\n🌈 Updating LEDs...");
        
        for (int i = 0; i < numAirports && i < numLeds; i++) {
            if (!g_state.ledsEnabled()) {
                leds[i] = LEDColors::OFF;
                baseColors[i] = LEDColors::OFF;
            } else if (airports[i].isEmpty()) {
                leds[i] = LEDColors::OFF;
                baseColors[i] = LEDColors::OFF;
            } else if (!airports[i].hasValidData || !airports[i].metarData.hasFltcat()) {
                leds[i] = LEDColors::NO_DATA;
                baseColors[i] = LEDColors::NO_DATA;
            } else {
                CRGB color = getFlightCategoryColor(airports[i].metarData.fltcat);
                
                // Store the base flight category color
                baseColors[i] = color;
                
                // Check for high winds - store wind warning in base color but don't set LED yet
                // (let updateWindBlink handle the actual blinking)
                if (g_state.windEnabled() && 
                    airports[i].metarData.hasWspd() && 
                    airports[i].metarData.wspd > g_state.windThreshold()) {
                    baseColors[i] = LEDColors::WIND_WARNING;
                    Serial.println("LED " + String(i) + " (" + String(airports[i].icaoCode) + 
                                  "): YELLOW (High winds: " + String(airports[i].metarData.wspd) + " kt)");
                } else {
                    Serial.println("LED " + String(i) + " (" + String(airports[i].icaoCode) + 
                                  "): " + String(airports[i].metarData.fltcat));
                }
                
                // Set initial LED color (will be overridden by blink/flash effects in loop)
                leds[i] = baseColors[i];
            }
        }
        
        FastLED.show();
        Serial.println("✅ LEDs updated");
    }
    
    void updateThunderstorms(const AirportLED* airports, int numAirports) {
        if (!g_state.thunderstormsEnabled()) { return; }
        if (!g_state.ledsEnabled()) { return; }
        
        // Don't show effects if weather fetch failed
        if (g_state.lastFetchStatus != SUCCESS) { return; }
        
        unsigned long currentMillis = millis();
        
        // Check if we have any active thunderstorms
        bool hasThunderstorms = false;
        for (int i = 0; i < numAirports && i < numLeds; i++) {
            if (airports[i].hasValidData && airports[i].metarData.hasThunderstorm()) {
                hasThunderstorms = true;
                break;
            }
        }
        
        if (!hasThunderstorms) return;
        
        // Lightning flash logic: brief flash, then long pause
        if (flashState) {
            // Currently flashing - turn off after brief duration (100ms)
            if (currentMillis - lastFlashTime >= 100) {
                flashState = false;
                // Restore base colors
                for (int i = 0; i < numAirports && i < numLeds; i++) {
                    if (airports[i].hasValidData && airports[i].metarData.hasThunderstorm()) {
                        leds[i] = baseColors[i];
                    }
                }
                FastLED.show();
                lastFlashTime = currentMillis;
            }
        } else {
            // Not flashing - randomly trigger a flash every 3-8 seconds
            if (currentMillis - lastFlashTime >= 3000) {
                // Random chance to flash (approximately every 5 seconds on average)
                if (random(0, 100) < 20) {  // 20% chance per check
                    flashState = true;
                    // Flash white
                    for (int i = 0; i < numAirports && i < numLeds; i++) {
                        if (airports[i].hasValidData && airports[i].metarData.hasThunderstorm()) {
                            leds[i] = LEDColors::THUNDERSTORM;
                        }
                    }
                    FastLED.show();
                    lastFlashTime = currentMillis;
                }
            }
        }
    }
    
    void updateWindBlink(const AirportLED* airports, int numAirports) {
        if (!g_state.windEnabled()) { return; }
        if (!g_state.ledsEnabled()) { return; }
        
        // Don't show effects if weather fetch failed
        if (g_state.lastFetchStatus != SUCCESS) { return; }
        
        unsigned long currentMillis = millis();
        
        // Wind LED blink effect
        if (currentMillis - lastWindBlinkTime >= WIND_BLINK_INTERVAL) {
            lastWindBlinkTime = currentMillis;
            windBlinkState = !windBlinkState;
            
            bool hasWindWarnings = false;
            for (int i = 0; i < numAirports && i < numLeds; i++) {
                // Only blink if: has valid data, high winds, AND no thunderstorm
                if (airports[i].hasValidData && 
                    airports[i].metarData.hasWspd() && 
                    airports[i].metarData.wspd > g_state.windThreshold() &&
                    !airports[i].metarData.hasThunderstorm()) { // Don't blink if thunderstorm is active
                    
                    if (windBlinkState) {
                        leds[i] = LEDColors::WIND_WARNING;
                    } else {
                        leds[i] = LEDColors::OFF;
                    }
                    hasWindWarnings = true;
                }
            }
            
            if (hasWindWarnings) {
                FastLED.show();
            }
        }
    }
    
    void rainbowSweep() {
        if (!g_state.ledsEnabled()) { return; }

        for (int hue = 0; hue < 256; hue++) {
            fill_solid(leds, numLeds, CHSV(hue, 255, 255));
            FastLED.show();
            delay(10);
        }
    }
    
    void showErrorState() {
        for (int brightness = 0; brightness < 256; brightness++) {
            fill_solid(leds, numLeds, CHSV(0, 255, brightness));
            FastLED.show();
            delay(10);
        }
        for (int brightness = 255; brightness >= 0; brightness--) {
            fill_solid(leds, numLeds, CHSV(0, 255, brightness));
            FastLED.show();
            delay(10);
        }
    }
    
    void setAll(CRGB color) {
        fill_solid(leds, numLeds, color);
        fill_solid(baseColors, numLeds, color);
        FastLED.show();
    }
    
    void setBrightness(uint8_t brightness) {
        FastLED.setBrightness(brightness);
        FastLED.show();
    }
};

// Forward declarations
class WebServerHandler;
extern AirportLED airportLEDs[];
extern const int NUM_AIRPORTS;
extern unsigned long previousMillis;
extern LEDController ledController;

// ============================================================================
// WEB SERVER
// ============================================================================

class WebServerHandler {
private:
    AsyncWebServer& server;
    AirportLED* airports;
    int numAirports;
    WiFiManager& wifiManager;
    
    struct TemplateCache {
        String content;
        unsigned long cacheTime;
        bool isValid;
        TemplateCache() : cacheTime(0), isValid(false) {}
    };
    
    TemplateCache mainTemplate;
    TemplateCache detailedTemplate;
    TemplateCache statusTemplate;
    TemplateCache settingsTemplate;
    
public:
    WebServerHandler(AsyncWebServer& srv, AirportLED* apt, int num, WiFiManager& wm)
        : server(srv), airports(apt), numAirports(num), wifiManager(wm) {}
    
    void setupRoutes() {
        server.on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
            this->handleMainPage(request);
        });
        
        server.on("/detailed", HTTP_GET, [this](AsyncWebServerRequest* request) {
            this->handleDetailedPage(request);
        });
        
        server.on("/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
            this->handleStatusPage(request);
        });
        
        server.on("/settings", HTTP_GET, [this](AsyncWebServerRequest* request) {
            this->handleSettingsPage(request);
        });
        
        server.on("/api/settings", HTTP_POST, [this](AsyncWebServerRequest* request) {
            this->handleSettingsUpdate(request);
        });

        server.onNotFound(handleNotFound);
        
        server.serveStatic("/", LittleFS, "/");
        Serial.println("Web routes configured");
    }
    
    void setupWiFiManagerRoutes() {
        server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
            request->send(LittleFS, "/wifimanager.html", "text/html");
        });
        
        server.serveStatic("/", LittleFS, "/");
        
        server.on("/", HTTP_POST, [this](AsyncWebServerRequest* request) {
            String newSsid = "";
            String newPass = "";
            
            int params = request->params();
            for (int i = 0; i < params; i++) {
                const AsyncWebParameter* p = request->getParam(i);
                if (p->isPost()) {
                    if (p->name() == "ssid") newSsid = p->value();
                    else if (p->name() == "pass") newPass = p->value();
                }
            }
            
            if (newSsid.length() > 0) {
                wifiManager.saveCredentials(newSsid.c_str(), newPass.c_str());
                request->send(200, "text/plain", "Done. ESP will restart.");
                delay(2000);
                ESP.restart();
            }
        });

        server.onNotFound(handleNotFound);
    }
    
    void begin() {
        server.begin();
        Serial.println("Web server started");
    }

    static void handleNotFound(AsyncWebServerRequest *request) {
        File file = LittleFS.open("/404.html", "r");
        if (!file) {
            request->send(404, "text/plain", "404: Page Not Found");
            return;
        }

        String content = file.readString();
        file.close();
        
        request->send(200, "text/html", content);
    }
    
private:
    bool loadTemplate(TemplateCache& cache, const char* filename) {
        if (cache.isValid && (millis() - cache.cacheTime) < TEMPLATE_CACHE_DURATION) {
            return true;
        }
        
        File file = LittleFS.open(filename, "r");
        if (!file) {
            Serial.printf("Template not found: %s\n", filename);
            return false;
        }
        
        cache.content = file.readString();
        cache.cacheTime = millis();
        cache.isValid = true;
        file.close();
        
        Serial.printf("Template loaded: %s\n", filename);
        return true;
    }
    
    void handleMainPage(AsyncWebServerRequest* request) {
        if (ESP.getFreeHeap() < MIN_HEAP_FOR_OPERATION) {
            request->send(500, "text/plain", "Low Memory");
            return;
        }
        
        if (!loadTemplate(mainTemplate, "/index.html")) {
            request->send(500, "text/plain", "Template Error: index.html not found");
            return;
        }
        
        String html = processMainTemplate();
        request->send(200, "text/html", html);
    }
    
    void handleDetailedPage(AsyncWebServerRequest* request) {
        Serial.println("Handling detailed weather request...");
        
        if (!loadTemplate(detailedTemplate, "/detailed_weather.html")) {
            Serial.println("ERROR: Could not load detailed_weather.html template");
            request->send(500, "text/plain", "Template Error: detailed_weather.html not found. Please upload to LittleFS.");
            return;
        }
        
        Serial.printf("Free heap before detailed processing: %d bytes\n", ESP.getFreeHeap());
        
        // Use chunked response to avoid memory issues with many airports
        AsyncWebServerResponse* response = request->beginChunkedResponse("text/html", [this](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            static String templateHtml;
            static String detailedData;
            static bool initialized = false;
            static size_t currentPos = 0;
            
            if (!initialized) {
                // Generate detailed data incrementally
                detailedData = "";
                for (int i = 0; i < numAirports; i++) {
                    if (airports[i].isEmpty() || !airports[i].hasValidData) continue;
                    
                    detailedData += "<div class='airport-detail'>";
                    detailedData += "<h2>" + String(airports[i].icaoCode) + " - Detailed Weather</h2>";
                    detailedData += "<div class='weather-grid'>";
                    
                    const METAR& m = airports[i].metarData;
                    
                    if (m.hasFltcat()) {
                        detailedData += "<div class='weather-item'><strong>Flight Category</strong>" + String(m.fltcat) + "</div>";
                    }
                    if (m.hasTemp()) {
                        detailedData += "<div class='weather-item'><strong>Temperature</strong>" + String(m.temp, 1) + "°C</div>";
                    }
                    if (m.hasDewp()) {
                        detailedData += "<div class='weather-item'><strong>Dewpoint</strong>" + String(m.dewp, 1) + "°C</div>";
                    }
                    if (m.hasWspd()) {
                        detailedData += "<div class='weather-item'><strong>Wind Speed</strong>" + String(m.wspd, 0) + " kt</div>";
                    }
                    if (m.hasWdir()) {
                        detailedData += "<div class='weather-item'><strong>Wind Direction</strong>" + String(m.wdir) + "°</div>";
                    }
                    if (m.hasVisib()) {
                        detailedData += "<div class='weather-item'><strong>Visibility</strong>" + String(m.visib, 1) + " SM</div>";
                    }
                    if (m.hasAltim()) {
                        detailedData += "<div class='weather-item'><strong>Altimeter</strong>" + String(m.altim, 2) + " inHg</div>";
                    }
                    
                    detailedData += "</div>";
                    
                    if (strlen(m.cloudConditions) > 0) {
                        detailedData += "<div class='weather-item' style='grid-column: 1/-1;'><strong>Cloud Conditions</strong>" + String(m.cloudConditions) + "</div>";
                    }
                    
                    if (strlen(m.rawText) > 0) {
                        detailedData += "<div class='raw-metar'><strong>Raw METAR:</strong><br>" + String(m.rawText) + "</div>";
                    }
                    
                    detailedData += "</div>";
                    
                    // Check memory and yield if needed
                    if (ESP.getFreeHeap() < 15000) {
                        Serial.printf("Low memory during generation: %d bytes, processed %d airports\n", ESP.getFreeHeap(), i);
                        delay(10);
                    }
                }
                
                templateHtml = detailedTemplate.content;
                templateHtml.replace("%DETAILED_AIRPORT_DATA%", detailedData);
                templateHtml.replace("%LAST_FETCH%", String(g_state.lastFetchTime));
                
                currentPos = 0;
                initialized = true;
                
                Serial.printf("Detailed HTML generated: %d bytes, Free heap: %d bytes\n", templateHtml.length(), ESP.getFreeHeap());
            }
            
            size_t remaining = templateHtml.length() - currentPos;
            if (remaining == 0) {
                initialized = false;
                templateHtml = "";
                detailedData = "";
                currentPos = 0;
                return 0;
            }
            
            size_t chunkSize = min(maxLen, remaining);
            memcpy(buffer, templateHtml.c_str() + currentPos, chunkSize);
            currentPos += chunkSize;
            
            return chunkSize;
        });
        
        request->send(response);
        Serial.println("Detailed page chunked response initiated");
    }
    
    void handleStatusPage(AsyncWebServerRequest* request) {
        if (!loadTemplate(statusTemplate, "/api_status.html")) {
            request->send(500, "text/plain", "Template Error: api_status.html not found");
            return;
        }
        
        String html = processStatusTemplate();
        request->send(200, "text/html", html);
    }
    
    void handleSettingsPage(AsyncWebServerRequest* request) {
        if (!loadTemplate(settingsTemplate, "/settings.html")) {
            request->send(500, "text/plain", "Template Error: settings.html not found");
            return;
        }
        
        String html = processSettingsTemplate();
        request->send(200, "text/html", html);
    }
    
    void handleSettingsUpdate(AsyncWebServerRequest* request) {
        bool updated = false;
        
        if (request->hasParam("brightness", true)) {
            int brightness = request->getParam("brightness", true)->value().toInt();
            g_state.setBrightness(brightness);
            ledController.setBrightness(brightness);
            Serial.printf("Brightness: %d\n", brightness);
            updated = true;
        }
        
        if (request->hasParam("winds", true)) {
            bool windEnabled = request->getParam("winds", true)->value() == "true";
            g_state.setWindEnabled(windEnabled);
            Serial.printf("Wind warnings: %s\n", windEnabled ? "enabled" : "disabled");
            updated = true;
        }

        if (request->hasParam("windThreshold", true)) {
            int threshold = request->getParam("windThreshold", true)->value().toInt();
            g_state.setWindThreshold(threshold);
            Serial.printf("Wind threshold: %d kt\n", threshold);
            updated = true;
        }
        
        if (request->hasParam("thunderstorms", true)) {
            bool tsEnabled = request->getParam("thunderstorms", true)->value() == "true";
            g_state.setThunderstormsEnabled(tsEnabled);
            Serial.printf("Thunderstorms: %s\n", tsEnabled ? "enabled" : "disabled");
            updated = true;
        }
        
        if (updated) {
            ledController.updateFromAirports(airports, numAirports);
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            request->send(400, "application/json", "{\"status\":\"error\"}");
        }
    }
    
    String processMainTemplate() {
        String html = mainTemplate.content;
        
        int validCount = 0;
        for (int i = 0; i < numAirports; i++) {
            if (airports[i].hasValidData) validCount++;
        }
        
        String airportData = "";
        for (int i = 0; i < numAirports; i++) {
            if (airports[i].isEmpty()) continue;
            
            String cssClass = "no-data";
            String status = "NO DATA";
            
            if (airports[i].hasValidData && airports[i].metarData.hasFltcat()) {
                String fltcat = String(airports[i].metarData.fltcat);
                fltcat.toLowerCase();
                cssClass = fltcat;
                status = String(airports[i].metarData.fltcat);
                
                if (airports[i].metarData.hasTemp()) {
                    status += " - " + String(airports[i].metarData.temp, 1) + "°C";
                }
                if (airports[i].metarData.hasWspd()) {
                    status += " - " + String(airports[i].metarData.wspd, 0) + "kt";
                }
            }
            
            airportData += "<div class='airport " + cssClass + "'>";
            airportData += "<h3>" + String(airports[i].icaoCode) + "</h3>";
            airportData += "<p>" + status + "</p></div>";
        }
        
        html.replace("%PROGRAM_STATUS%", String(g_state.programStatus));
        html.replace("%LAST_FETCH%", String(g_state.lastFetchTime));
        html.replace("%SW_VERS%", String(SW_VERS));
        html.replace("%FREE_MEMORY%", String(ESP.getFreeHeap()));
        html.replace("%VALID_COUNT%", String(validCount));
        html.replace("%TOTAL_COUNT%", String(numAirports));
        html.replace("%AIRPORT_DATA%", airportData);
        
        return html;
    }
        
    String processStatusTemplate() {
        String html = statusTemplate.content;
        
        unsigned long uptimeSeconds = (millis() - g_state.programStartTime) / 1000;
        unsigned long days = uptimeSeconds / 86400;
        unsigned long hours = (uptimeSeconds % 86400) / 3600;
        unsigned long minutes = (uptimeSeconds % 3600) / 60;
        String uptime = String(days) + "d " + String(hours) + "h " + String(minutes) + "m";
        
        String wifiStatus = wifiManager.isConnected() ? "Connected" : "Disconnected";
        String wifiStatusClass = wifiManager.isConnected() ? "status-ok" : "status-error";
        
        String apiStatus = "Unknown";
        String apiStatusClass = "status-error";
        switch (g_state.lastFetchStatus) {
            case SUCCESS: 
                apiStatus = "OK"; 
                apiStatusClass = "status-ok";
                break;
            case CONNECTION_FAILURE: 
                apiStatus = "Connection Failed"; 
                break;
            case CONNECTION_TIMEOUT: 
                apiStatus = "Timeout"; 
                apiStatusClass = "status-warning";
                break;
            case PARSING_FAILURE: 
                apiStatus = "Parse Error"; 
                break;
            case RESPONSE_FAILURE: 
                apiStatus = "Response Error"; 
                break;
        }
        
        int validCount = 0;
        for (int i = 0; i < numAirports; i++) {
            if (airports[i].hasValidData) validCount++;
        }
        
        String apiUrl = "https://aviationweather.gov/api/data/metar?ids=";
        for (int i = 0; i < numAirports; i++) {
            if (airports[i].isEmpty()) continue;
            if (!apiUrl.endsWith("=")) apiUrl += ",";
            apiUrl += airports[i].icaoCode;
        }
        apiUrl += "&format=raw&taf=false&hours=1";
        
        unsigned long currentMillis = millis();
        unsigned long elapsedMillis = currentMillis - getPreviousMillis();
        unsigned long nextUpdateMs = (elapsedMillis < REQUEST_INTERVAL) ? 
                                     (REQUEST_INTERVAL - elapsedMillis) : 0;
        String nextUpdate = String(nextUpdateMs / 60000) + "m " + String((nextUpdateMs % 60000) / 1000) + "s";
        
        html.replace("%SW_VERS%", String(SW_VERS));
        html.replace("%FREE_MEMORY%", String(ESP.getFreeHeap()));
        html.replace("%UPTIME%", uptime);
        html.replace("%WIFI_STATUS%", wifiStatus);
        html.replace("%WIFI_STATUS_CLASS%", wifiStatusClass);
        html.replace("%IP_ADDRESS%", wifiManager.getIPAddress().toString());
        html.replace("%WIFI_RSSI%", String(wifiManager.getSignalStrength()));
        html.replace("%API_STATUS%", apiStatus);
        html.replace("%API_STATUS_CLASS%", apiStatusClass);
        html.replace("%LAST_FETCH%", String(g_state.lastFetchTime));
        html.replace("%LAST_ATTEMPT%", String(g_state.lastAttemptTime));
        html.replace("%NEXT_UPDATE%", nextUpdate);
        html.replace("%API_URL%", apiUrl);
        html.replace("%TOTAL_COUNT%", String(numAirports));
        html.replace("%VALID_COUNT%", String(validCount));
        html.replace("%MAX_AIRPORTS%", String(MAX_AIRPORTS));
        
        return html;
    }
    
    String processSettingsTemplate() {
        String html = settingsTemplate.content;
        
        html.replace("%BRIGHTNESS%", String(g_state.getBrightness()));
        
        String windChecked = g_state.windEnabled() ? "checked" : "";
        html.replace("%WIND_ENABLED%", windChecked);

        html.replace("%WIND_THRESHOLD%", String(g_state.windThreshold()));
        
        String tsChecked = g_state.thunderstormsEnabled() ? "checked" : "";
        html.replace("%THUNDERSTORM_ENABLED%", tsChecked);
        
        html.replace("%WIND_THRESHOLD%", String(WIND_THRESHOLD));
        
        return html;
    }
};

// ============================================================================
// AIRPORT CONFIGURATION
// ============================================================================

AirportLED airportLEDs[] = {
    // Controller 1
    
    // AirportLED("KVIS"),
    // AirportLED("KHJO"),
    // AirportLED("KNLC"),
    // AirportLED("KO32"),
    // AirportLED("KFAT"),
    // AirportLED("KFCH"),
    // AirportLED("KMAE"),
    // AirportLED("KMCE"),
    // AirportLED("KMOD"),
    // AirportLED("KSCK"),
    // AirportLED("KC83"),
    // AirportLED("KTCY"),
    // AirportLED("KLVK"),
    AirportLED("KE16"),
    // AirportLED("KRHV"),
    // AirportLED("KSNS"),
    // AirportLED("KMRY"),
    // AirportLED("KOAR"),
    // AirportLED("KCVH"),
    // AirportLED("KWVI"),
    // AirportLED("KSJC"),
    // AirportLED("KNUQ"),
    // AirportLED("KHAF"),
    // AirportLED("KSFO"),
    // AirportLED("KSQL"),
    // AirportLED("KPAO"),
    // AirportLED("KHWD"),
    // AirportLED("KOAK"),
    // AirportLED("KCCR"),
    // AirportLED("KSUU"),
    // AirportLED("KAPC"),
    // AirportLED("KDVO"),
    // AirportLED("KO69"),
    // AirportLED("KSTS"),
    // NULL,
    // AirportLED("KUKI"),
    // AirportLED("KLLR"),
    // NULL,
    // NULL,
    // AirportLED("KOVE"),
    // AirportLED("KMYV"),
    // AirportLED("KBAB"),
    // AirportLED("KLHM"),
    // AirportLED("KSMF"),
    // AirportLED("KMCC"),
    // AirportLED("KMHR"),
    // AirportLED("KSAC"),
    // AirportLED("KEDU"),
    // AirportLED("KDWA"),
    // AirportLED("KVCB"),

    // Controller 2

    // AirportLED("KO26"),
    // NULL,
    // AirportLED("KBIH"),
    // AirportLED("KMMH"),
    // NULL,
    // AirportLED("KHTH"),
    // NULL,
    // AirportLED("KNFL"),
    // NULL,
    // AirportLED("KRNO"),
    // AirportLED("KCXP"),
    // AirportLED("KMEV"),
    // AirportLED("KTVL"),
    // AirportLED("KTRK"),
    // AirportLED("KBLU"),
    // AirportLED("KGOO"),
    // AirportLED("KAUN"),
    // AirportLED("KPVT"),
    // AirportLED("KO61"),
    // AirportLED("KJAQ"),
    // AirportLED("KCPU"),
    // AirportLED("KO22"),
    // NULL,
    // NULL,
    // NULL,
};

const int NUM_AIRPORTS = sizeof(airportLEDs) / sizeof(airportLEDs[0]);
CRGB leds[NUM_AIRPORTS];

// Helper function for web server
unsigned long getPreviousMillis() { return previousMillis; }

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================

WiFiManager wifiManager("metarmap");
// WiFiManager wifiManager("metarmap2");
WeatherAPIClient weatherClient;
LEDController ledController(leds, NUM_AIRPORTS);
AsyncWebServer webServer(80);
WebServerHandler webHandler(webServer, airportLEDs, NUM_AIRPORTS, wifiManager);

unsigned long previousMillis = 0;
unsigned long interval = LOOP_INTERVAL;
unsigned long memoryCheckMillis = 0;
unsigned long wifiCheckMillis = 0;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

void initializeFileSystem();
void initializeTime();
void handleButtonPress();
void handleWeatherUpdates(unsigned long currentMillis);
void printAirportSummary();

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    Serial.println("\n\n=== Aviation Weather Map Starting ===");
    Serial.printf("Software Version: %s\n", SW_VERS);
    
    initializeFileSystem();
    g_state.init();

    pinMode(BUTTON_PIN, INPUT_PULLUP);
#if DIG2GO  
    pinMode(LED_POWER_PIN, OUTPUT);
    digitalWrite(LED_POWER_PIN, HIGH);
#endif
    ledController.begin();
    ledController.setBrightness(g_state.getBrightness());
    
    if (wifiManager.loadCredentials() && wifiManager.connect()) {
        g_state.setStatus("WiFi Connected");
        ledController.rainbowSweep();
        ledController.setAll(LEDColors::NO_DATA);
        initializeTime();
        webHandler.setupRoutes();
        webHandler.begin();
        Serial.println("=== System Ready ===");
    } else {
        g_state.setStatus("WiFi Failed - AP Mode");
        Serial.println("=== Starting AP Mode ===");
        wifiManager.startAccessPoint("ESP-WIFI-MANAGER");
        webHandler.setupWiFiManagerRoutes();
        webHandler.begin();
    }
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {
    unsigned long currentMillis = millis();
    
    if (currentMillis - memoryCheckMillis > MEMORY_CHECK_INTERVAL) {
        Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
        g_state.checkMemory();
        memoryCheckMillis = currentMillis;
    }
    
    if (currentMillis - wifiCheckMillis >= WIFI_CHECK_INTERVAL) {
        if (!wifiManager.isConnected()) {
            if (!wifiManager.attemptReconnection()) {
                ledController.showErrorState();
                if (wifiManager.hasTooManyFailures()) {
                    Serial.println("Too many WiFi failures - restarting...");
                    delay(2000);
                    ESP.restart();
                }
            } else {
                ledController.rainbowSweep();
                ledController.setAll(LEDColors::NO_DATA);
            }
        }
        wifiCheckMillis = currentMillis;
    }
    
    ledController.updateThunderstorms(airportLEDs, NUM_AIRPORTS);
    ledController.updateWindBlink(airportLEDs, NUM_AIRPORTS);
    
    handleButtonPress();
    handleWeatherUpdates(currentMillis);
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

void initializeFileSystem() {
    if (!LittleFS.begin()) {
        Serial.println("ERROR: LittleFS failed");
    } else {
        Serial.println("✅ LittleFS mounted");
    }
}

void initializeTime() {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Syncing time");
    time_t now = time(nullptr);
    int attempts = 0;
    while (now < 8 * 3600 * 2 && attempts < 20) {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
        attempts++;
    }
    Serial.println(now >= 8 * 3600 * 2 ? "\n✅ Time synced" : "\n⚠️ Time sync timeout");
}

void handleButtonPress() {
    static unsigned long pressStartTime = 0;
    static bool buttonWasPressed = false;
    static bool longPressHandled = false;
    
    bool buttonPressed = (digitalRead(BUTTON_PIN) == LOW);
    
    if (buttonPressed && !buttonWasPressed) {
        pressStartTime = millis();
        buttonWasPressed = true;
        longPressHandled = false;
    } else if (buttonPressed && buttonWasPressed) {
        unsigned long pressDuration = millis() - pressStartTime;
        
        if (pressDuration >= BUTTON_LONG_PRESS_DURATION && !longPressHandled) {
            Serial.println("Long press detected - resetting WiFi.");
            wifiManager.resetCredentials();
            delay(1000);
            ESP.restart();
            longPressHandled = true;
        }
    } else if (!buttonPressed && buttonWasPressed) {
        unsigned long pressDuration = millis() - pressStartTime;
        
        if (pressDuration < BUTTON_LONG_PRESS_DURATION) {
            Serial.println("Button pressed - toggling LEDs.");
            g_state.setLEDState(!g_state.ledsEnabled());
#if DIG2GO
            digitalWrite(LED_POWER_PIN, g_state.ledsEnabled() ? HIGH : LOW);
#endif
        }
        
        buttonWasPressed = false;
    }
}

void handleWeatherUpdates(unsigned long currentMillis) {
    if (currentMillis - previousMillis < interval) return;
    
    if (!wifiManager.isConnected()) {
        g_state.setStatus("WiFi Disconnected");
        ledController.showErrorState();
        Serial.println("❌ No WiFi - retrying in " + String(millisToSeconds(RETRY_TIMEOUT)) + "s");
        interval = RETRY_TIMEOUT;
        previousMillis = currentMillis;
        return;
    }
    
    Serial.println("\n=== Weather Update ===");
    g_state.updateAttemptTime();
    g_state.checkMemory();
    
    METAR_STATUS result = weatherClient.fetchWeatherData(airportLEDs, NUM_AIRPORTS);
    g_state.setFetchStatus(result);
    
    if (result == SUCCESS) {
        g_state.setStatus("Weather Data Current");
        g_state.updateFetchTime();
        printAirportSummary();
        ledController.updateFromAirports(airportLEDs, NUM_AIRPORTS);
        Serial.println("✅ Weather updated - next in " + String(millisToSeconds(REQUEST_INTERVAL)) + "s");
        interval = REQUEST_INTERVAL;
    } else {
        g_state.setStatus("Weather Fetch Failed");
        ledController.showErrorState();
        Serial.println("❌ Weather fetch failed - retry in " + String(millisToSeconds(RETRY_TIMEOUT)) + "s");
        interval = RETRY_TIMEOUT;
    }
    
    previousMillis = currentMillis;
}

void printAirportSummary() {
    Serial.println("\n🗂️ Airport Summary:");
    Serial.println("========================");
    for (int i = 0; i < NUM_AIRPORTS; i++) {
        if (airportLEDs[i].isEmpty()) continue;
        Serial.print("LED " + String(i) + ": " + String(airportLEDs[i].icaoCode));
        if (airportLEDs[i].hasValidData) {
            const METAR& m = airportLEDs[i].metarData;
            if (m.hasFltcat()) Serial.print(" (" + String(m.fltcat) + ")");
            if (m.hasTemp()) Serial.print(" " + String(m.temp, 1) + "°C");
            if (m.hasWspd()) Serial.print(" " + String(m.wspd, 0) + "kt");
            Serial.println(" ✅");
        } else {
            Serial.println(" ❌");
        }
    }
    Serial.println("========================");
}