#include <Wire.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <WiFiSettings.h>
#include <ESPNtpClient.h>
#include <StreamUtils.h>

#include "config.h"

extern "C"
{
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
}

#define Sprintf(f, ...) ({ char* s; asprintf(&s, f, __VA_ARGS__); String r = s; free(s); r; })
#define DEVICE_ID (Sprintf("%06" PRIx64, ESP.getEfuseMac() >> 24)) // unique device ID
#define uS_TO_S_FACTOR 1000000                                     // Conversion factor for micro seconds to seconds

String version = "0.1.0";

// timer
TimerHandle_t wifiReconnectTimer;
TimerHandle_t waterpumpTimer;

AsyncWebServer server(80);

// states
bool isUpdating = false;
bool isWifiConnected = false;
bool isPortalActive = false;
bool isWifiSuccess = false;
bool wasTimeSynced = false;

// (old) timers
unsigned long lastInfoSend = 0;

void onWiFiEvent(WiFiEvent_t event)
{
    Serial.printf("[WiFi-event] event: %d\n", event);

    switch (event)
    {
    case SYSTEM_EVENT_STA_GOT_IP:
        isWifiConnected = true;

        Serial.println("WiFi connected");
        Serial.println("IP address: ");
        Serial.println(WiFi.localIP());

        break;

    case SYSTEM_EVENT_STA_DISCONNECTED:
        isWifiConnected = false;

        Serial.println("WiFi disconnected");

        if (!isPortalActive)
        {
            xTimerStart(wifiReconnectTimer, 0);
        }

        break;

    default:
        break;
    }
}

int GetRssiAsQuality(int rssi)
{
    int quality = 0;

    if (rssi <= -100)
    {
        quality = 0;
    }
    else if (rssi >= -50)
    {
        quality = 100;
    }
    else
    {
        quality = 2 * (rssi + 100);
    }

    return quality;
}

DynamicJsonDocument getInfoJson()
{
    DynamicJsonDocument doc(1024);
    doc["version"] = version;

    JsonObject system = doc.createNestedObject("system");
    system["deviceId"] = DEVICE_ID;
    system["freeHeap"] = ESP.getFreeHeap(); // in V
    system["time"] = NTP.getTimeDateStringForJS();
    system["uptime"] = NTP.getUptimeString();

    // network
    JsonObject network = doc.createNestedObject("network");
    int8_t rssi = WiFi.RSSI();
    network["wifiRssi"] = rssi;
    network["wifiQuality"] = GetRssiAsQuality(rssi);
    network["wifiSsid"] = WiFi.SSID();
    network["ip"] = WiFi.localIP().toString();

    return doc;
}

void hardReset()
{
    Serial.println("starting hard-reset");

    SPIFFS.format();

    delay(1000);

    ESP.restart();

    //WiFiSettings.portal();
}

/*void goSleep(unsigned long seconds)
{
    if (seconds <= 0)
    {
        return;
    }

    Serial.print("start deep-sleep for ");
    Serial.print(seconds);
    Serial.println(" seconds");

    StaticJsonDocument<200> doc;
    doc["duration"] = seconds;

    String JS;
    serializeJson(doc, JS);

    mqttClient.publish(getMqttTopic("out/sleep"), 1, false, JS.c_str());

    delay(500);

    esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
}*/

bool connectToWifi()
{
    return WiFiSettings.connect(true, 30);
}

void stopWaterpump()
{
    digitalWrite(WATERPUMP_PIN, LOW);

    Serial.println("watering stopped");
}

void onWaterpumpTimerTriggered()
{
    // finished watering -> stop watering
    stopWaterpump();
}

void setupTimers()
{
    wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void *)1, reinterpret_cast<TimerCallbackFunction_t>(connectToWifi));
    waterpumpTimer = xTimerCreate("waterpumpTimer", pdMS_TO_TICKS(1000), pdFALSE, (void *)2, reinterpret_cast<TimerCallbackFunction_t>(onWaterpumpTimerTriggered));
}

void setupOTA()
{
    ArduinoOTA
        .setHostname(WiFiSettings.hostname.c_str())
        .setPassword(WiFiSettings.password.c_str())
        .onStart([]() {
            isUpdating = true;

            String type;

            if (ArduinoOTA.getCommand() == U_FLASH)
            {
                type = "sketch";
            }
            else
            { // U_FS
                type = "filesystem";
            }

            // NOTE: if updating FS this would be the place to unmount FS using FS.end()
            Serial.println("Start updating " + type);
        })
        .onEnd([]() {
            Serial.println("\nEnd");

            isUpdating = false;
        })
        .onProgress([](unsigned int progress, unsigned int total) {
            unsigned int percentValue = progress / (total / 100);

            Serial.printf("Progress: %u%%\r", percentValue);
        })
        .onError([](ota_error_t error) {
            Serial.printf("Error[%u]: ", error);

            if (error == OTA_AUTH_ERROR)
            {
                Serial.println("Auth Failed");
            }
            else if (error == OTA_BEGIN_ERROR)
            {
                Serial.println("Begin Failed");
            }
            else if (error == OTA_CONNECT_ERROR)
            {
                Serial.println("Connect Failed");
            }
            else if (error == OTA_RECEIVE_ERROR)
            {
                Serial.println("Receive Failed");
            }
            else if (error == OTA_END_ERROR)
            {
                Serial.println("End Failed");
            }
        })
        .begin();
}

void detect_wakeup_reason()
{
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    switch (wakeup_reason)
    {
    case ESP_SLEEP_WAKEUP_EXT0:
        Serial.println("Wakeup caused by external signal using RTC_IO");
        break;

    case ESP_SLEEP_WAKEUP_EXT1:
        Serial.println("Wakeup caused by external signal using RTC_CNTL");
        break;

    case ESP_SLEEP_WAKEUP_TIMER:
        Serial.println("Wakeup caused by timer");
        break;

    case ESP_SLEEP_WAKEUP_TOUCHPAD:
        Serial.println("Wakeup caused by touchpad");
        break;

    case ESP_SLEEP_WAKEUP_ULP:
        Serial.println("Wakeup caused by ULP program");
        break;

    default:
        Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason);
        break;
    }
}

void startWaterpump(unsigned long seconds)
{
    if (xTimerIsTimerActive(waterpumpTimer) == pdTRUE)
    {
        Serial.println("pump already active");
        return;
    }

    // start timer for stop waterpump after specific time
    xTimerChangePeriod(waterpumpTimer, pdMS_TO_TICKS(seconds * 1000), 0);
    xTimerStart(waterpumpTimer, 0);

    // start watering
    digitalWrite(WATERPUMP_PIN, HIGH);

    Serial.println("watering started");
}

void abortWaterpump()
{
    if (xTimerIsTimerActive(waterpumpTimer) == pdFALSE)
    {
        return;
    }

    xTimerStop(waterpumpTimer, 0);
    stopWaterpump();
}

void setupNTP()
{
    NTP.setTimeZone(TIMEZONE);
    NTP.onNTPSyncEvent([](NTPEvent_t ntpEvent) {
        switch (ntpEvent.event)
        {
        case timeSyncd:
            wasTimeSynced = true;
            break;
        case partlySync:
        case syncNotNeeded:
        case accuracyError:
        default:
            break;
        }
    });
    NTP.begin();
}

void setupWebserver()
{
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/index.html", "text/html", false);
    });

    /*server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/wifi-password", "text/plain", false);
    });*/

    server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *request) {
        auto infoJson = getInfoJson();

        StringStream stream;
        auto size = serializeJson(infoJson, stream);

        request->send(stream, "application/json", size);
    });

    /*server.on("/api/manual-start", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->getParam("body");
    });*/

    AsyncCallbackJsonWebHandler *handler = new AsyncCallbackJsonWebHandler("/api/manual-start", [](AsyncWebServerRequest *request, JsonVariant &json) {
        const char *durationKey = "duration";
        StaticJsonDocument<200> data = json.as<JsonObject>();

        if (!data.containsKey(durationKey))
        {
            request->send(400); // bad request
            return;
        }

        auto duration = data[durationKey].as<unsigned long>();

        if (duration < 10 || duration > 600)
        {
            request->send(400); // bad request
            return;
        }

        Serial.print("start pump ");
        Serial.println(duration);

        startWaterpump(duration);

        request->send(200);
    });
    server.addHandler(handler);

    server.begin();
}

void setup()
{
    Serial.begin(115200);

    Wire.begin();

    if (!SPIFFS.begin(true))
    {
        Serial.println("An Error has occurred while mounting SPIFFS");
        return;
    }

    setupTimers();

    WiFi.onEvent(onWiFiEvent);

    WiFiSettings.secure = true;
    WiFiSettings.hostname = "pump-control-"; // will auto add device ID
    WiFiSettings.password = PASSWORD;

    // Set callbacks to start OTA when the portal is active
    WiFiSettings.onPortal = []() {
        isPortalActive = true;

        Serial.println("WiFi config portal active");

        setupOTA();
    };
    WiFiSettings.onPortalWaitLoop = []() {
        ArduinoOTA.handle();
    };

    WiFiSettings.onConfigSaved = []() {
        ESP.restart();
    };

    WiFiSettings.onSuccess = []() {
        isWifiSuccess = true;
    };

    if (!isPortalActive)
    {
        connectToWifi();
    }

    Serial.println("init OTA, NTP and webserver");

    setupOTA();
    setupNTP();
    setupWebserver();

    detect_wakeup_reason();
}

void loop()
{
    ArduinoOTA.handle();

    if (!isPortalActive && !isUpdating)
    {
        if (isWifiConnected && isWifiSuccess && wasTimeSynced)
        {
            if (lastInfoSend == 0 || millis() - lastInfoSend >= 45000) // every 45 seconds
            {
                Serial.print("time: ");
                Serial.println(NTP.getTimeDateString());

                //sendInfo(); // TODO move to async timer

                lastInfoSend = millis();
            }
        }
    }
}
