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
#include <ESPmDNS.h>
#include <RTClib.h>
//#include<time.h>

#include "config.h"

extern "C"
{
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
}

#define Sprintf(f, ...) ({ char* s; asprintf(&s, f, __VA_ARGS__); String r = s; free(s); r; })
#define DEVICE_ID (Sprintf("%06" PRIx64, ESP.getEfuseMac() >> 24)) // unique device ID
#define uS_TO_S_FACTOR 1000000                                     // Conversion factor for micro seconds to seconds

const char *autoStartsFilename = "/auto-starts";

String version = "0.1.0";

// timer
TimerHandle_t wifiReconnectTimer;
TimerHandle_t waterpumpTimer;

AsyncWebServer server(80);
RTC_DS3231 rtc;

// states
bool isUpdating = false;
bool isWifiConnected = false;
bool isPortalActive = false;
bool isWifiSuccess = false;
bool timeWasSynced = false;

// (old) timers
unsigned long lastInfoSend = 0;
unsigned long lastAlarmCheck = 0;

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

StaticJsonDocument<1024> getInfoJson()
{
    StaticJsonDocument<1024> doc;
    doc["version"] = version;

    JsonObject system = doc.createNestedObject("system");
    system["deviceId"] = DEVICE_ID;
    system["freeHeap"] = ESP.getFreeHeap(); // in V
    system["time"] = NTP.getTimeDateStringForJS();
    system["uptime"] = NTP.getUptimeString();

    JsonObject fileSystem = doc.createNestedObject("fileSystem");
    fileSystem["totalBytes"] = SPIFFS.totalBytes();
    fileSystem["usedBytes"] = SPIFFS.usedBytes();

    JsonObject network = doc.createNestedObject("network");
    int8_t rssi = WiFi.RSSI();
    network["wifiRssi"] = rssi;
    network["wifiQuality"] = GetRssiAsQuality(rssi);
    network["wifiSsid"] = WiFi.SSID();
    network["ip"] = WiFi.localIP().toString();

    return doc;
}

StaticJsonDocument<1024> getAutoStartsJson()
{
    File file = SPIFFS.open(autoStartsFilename, FILE_READ);

    StaticJsonDocument<1024> doc;
    auto error = deserializeJson(doc, file);

    file.close();

    if (error)
    {
        Serial.println("error on deserializing 'auto-starts' file");
    }

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
        Serial.println("waterpump not running");
        return;
    }

    Serial.println("abort waterpump");

    xTimerStop(waterpumpTimer, 0);
    stopWaterpump();
}

void setupRTC()
{
    if (!rtc.begin())
    {
        Serial.println("Couldn't find RTC!");
        Serial.flush();
        abort();
    }

    //we don't need the 32K Pin, so disable it
    rtc.disable32K();

    // stop oscillating signals at SQW Pin
    // otherwise setAlarm1 will fail
    rtc.writeSqwPinMode(DS3231_OFF);
}

void setupNTP()
{
    NTP.setTimeZone(TIMEZONE);
    NTP.setInterval(21600); // each 6h
    NTP.onNTPSyncEvent([](NTPEvent_t ntpEvent) {
        switch (ntpEvent.event)
        {
        case timeSyncd:
        {
            Serial.println("NTP synced");

            timeWasSynced = true;

            char *iso8601dateTime = NTP.getTimeDateString(time(NULL), "%04Y-%02m-%02dT%02H:%02M:%02S");
            rtc.adjust(DateTime(iso8601dateTime));

            break;
        }
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

    // only for debug reasons (raw file output)
    server.on("/auto-starts", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, autoStartsFilename, "application/json", false);
    });

    server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *request) {
        auto infoJson = getInfoJson();

        StringStream stream;
        auto size = serializeJson(infoJson, stream);

        request->send(stream, "application/json", size);
    });

    server.on("/api/auto-starts", HTTP_GET, [](AsyncWebServerRequest *request) {
        auto autoStarts = getAutoStartsJson();

        StringStream stream;
        auto size = serializeJson(autoStarts, stream);

        request->send(stream, "application/json", size);
    });

    server.on("/api/manual-stop", HTTP_POST, [](AsyncWebServerRequest *request) {
        abortWaterpump();

        request->send(200);
    });

    AsyncCallbackJsonWebHandler *postManualStartHandler = new AsyncCallbackJsonWebHandler("/api/manual-start", [](AsyncWebServerRequest *request, JsonVariant &json) {
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
    server.addHandler(postManualStartHandler);

    AsyncCallbackJsonWebHandler *postsAutoStartsHandler = new AsyncCallbackJsonWebHandler("/api/auto-starts", [](AsyncWebServerRequest *request, JsonVariant &json) {
        const char *timeKey = "time";
        const char *durationKey = "duration";

        JsonArray autoStarts = json.as<JsonArray>();

        // check JSON is in correct format
        // TODO use better schema check
        for (JsonVariant start : autoStarts)
        {
            if (!start.containsKey(timeKey) || !start.containsKey(durationKey))
            {
                request->send(400); // bad request
                return;
            }
        }

        // save into file
        File file = SPIFFS.open(autoStartsFilename, FILE_WRITE);

        if (serializeJson(autoStarts, file) == 0)
        {
            Serial.println("error on writing 'auto-starts' file");
        }

        file.close();

        Serial.println("auto-starts saved");

        request->send(200);

        ESP.restart();
    });
    server.addHandler(postsAutoStartsHandler);

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
    WiFiSettings.hostname = HOSTNAME_PREFIX; // will auto add device ID
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

    auto hostname = WiFiSettings.hostname.c_str();
    Serial.print("hostname: ");
    Serial.println(hostname);

    if (!MDNS.begin(hostname))
    {
        Serial.println("Error starting mDNS");
    }

    Serial.println("init OTA, NTP and webserver");

    setupOTA();
    setupRTC();
    setupNTP();
    setupWebserver();

    detect_wakeup_reason();
}

bool isAlarmFired(String timeValue)
{
    unsigned int alarmHour = timeValue.substring(0, 2).toInt();
    unsigned int alarmMinute = timeValue.substring(3, 5).toInt();

    Serial.print(timeValue);
    Serial.print(" - ");
    Serial.print(alarmHour);
    Serial.print("_");
    Serial.println(alarmMinute);

    DateTime now = rtc.now();

    return now.hour() == alarmHour && now.minute() == alarmMinute;
}

void loop()
{
    ArduinoOTA.handle();

    if (!isPortalActive && !isUpdating)
    {
        if (isWifiConnected && isWifiSuccess && timeWasSynced)
        {
            if (lastInfoSend == 0 || millis() - lastInfoSend >= UPDATE_INTERVAL)
            {
                /*Serial.print("time: ");
                Serial.println(NTP.getTimeDateString());*/

                time_t tnow = time(nullptr);
                Serial.println(ctime(&tnow));

                lastInfoSend = millis();
            }
        }

        if (lastAlarmCheck == 0 || millis() - lastAlarmCheck >= ALARM_CHECK_INTERVAL)
        {
            StaticJsonDocument<1024> doc = getAutoStartsJson();
            JsonArray autoStarts = doc.as<JsonArray>();

            Serial.print("autostart count ");
            Serial.println(autoStarts.size());

            for (JsonVariant autoStart : autoStarts)
            {
                if (isAlarmFired(autoStart["time"]))
                {
                    unsigned int duration = autoStart["duration"].as<int>();

                    Serial.print("Alarm was fired - watering for ");
                    Serial.print(duration);
                    Serial.println(" seconds");
                }
            }

            lastAlarmCheck = millis();
        }

        /*if (rtc.alarmFired(1))
        {
            Serial.println("Alarm fired");
            rtc.clearAlarm(1);
        }*/
    }
}
