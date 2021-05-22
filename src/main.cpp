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

#include "config.h"

#define Sprintf(f, ...) ({ char* s; asprintf(&s, f, __VA_ARGS__); String r = s; free(s); r; })
#define DEVICE_ID (Sprintf("%06" PRIx64, ESP.getEfuseMac() >> 24)) // unique device ID
#define uS_TO_S_FACTOR 1000000                                     // Conversion factor for micro seconds to seconds

struct NextAlarmResult
{
    byte index;
    DateTime dateTime;
    unsigned int wateringDuration;
};

const char *autoStartsFilename = "/auto-starts";

String version = "1.0.0";

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
bool wasInitAlarmsSet = false;

NextAlarmResult nextAlarmResult;

// (old) timers
unsigned long lastInfoSend = 0;

char *getFormatedRtcNow()
{
    DateTime now = rtc.now();

    String dateFormat = "YYYY/MM/DD hh:mm:ss";
    int strLen = dateFormat.length() + 1;
    char charArray[strLen];

    dateFormat.toCharArray(charArray, strLen);
    return now.toString(charArray);
}

/**
 * returns the date in format "YYYY-MM-DD"
 */
char *getDateString(DateTime now)
{
    String dateFormat = "YYYY-MM-DD";
    int strLen = dateFormat.length() + 1;
    char charArray[strLen];

    dateFormat.toCharArray(charArray, strLen);

    return now.toString(charArray);
}

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
    system["freeHeap"] = ESP.getFreeHeap(); // in bytes
    system["time"] = getFormatedRtcNow();   //NTP.getTimeDateStringForJS();
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

DateTime createDateTimeFromAlarmTime(DateTime day, String timeString)
{
    char *dateString = getDateString(day);

    // create date string in format: "2020-06-25T15:29:37"
    StringPrint dateStream;
    dateStream.print(dateString);
    dateStream.print("T");
    dateStream.print(timeString); // e.g. "08:45"
    dateStream.print(":00");      // add seconds

    String completeDateString = dateStream.str();
    int strLen = completeDateString.length() + 1;
    char charArray[strLen];

    completeDateString.toCharArray(charArray, strLen);

    return DateTime(charArray);
}

NextAlarmResult getNextAlarm(DateTime now, JsonArray autoStarts)
{
    int32_t minTotalSecondsDiff = INT32_MAX;
    byte nextAlarmIndex = 255; // 255 = no next alarm
    DateTime nextAlarm;

    byte alarmIndex = 0;

    for (JsonVariant autoStart : autoStarts)
    {
        DateTime alarm = createDateTimeFromAlarmTime(now, autoStart["time"]);

        if (alarm > now)
        {
            TimeSpan diff = alarm - now;
            int32_t totalSecondsDiff = diff.totalseconds();

            if (totalSecondsDiff < minTotalSecondsDiff)
            {
                minTotalSecondsDiff = totalSecondsDiff;
                nextAlarmIndex = alarmIndex;
                nextAlarm = alarm;
            }
        }

        alarmIndex++;
    }

    unsigned int nextAlarmWateringDuration = 0;

    if (nextAlarmIndex != 255) // 255 = no next alarm
    {
        JsonVariant autoStart = autoStarts.getElement(nextAlarmIndex);
        nextAlarmWateringDuration = autoStart["duration"].as<unsigned int>();
    }

    return {nextAlarmIndex, nextAlarm, nextAlarmWateringDuration};
}

void setNextAlarm(DateTime now, JsonArray autoStarts, bool tryNextDay)
{
    if (autoStarts.size() == 0)
    {
        Serial.println("couldn't set alarm because no autostarts configured yet.");
    }

    Serial.print("setNextAlarm for ");
    Serial.println(getDateString(now));

    NextAlarmResult nextAlarm = getNextAlarm(now, autoStarts);

    Serial.print("nextAlarm index ");
    Serial.println(nextAlarm.index);

    if (nextAlarm.index == 255) // 255 = no next alarm
    {
        // no alarm found for current day
        Serial.print("no next alarm found for today: ");

        if (!tryNextDay)
        {
            Serial.println("stop searching");
            return;
        }

        Serial.println("try find alarm on next day");

        // try to find alarm for next day
        DateTime nextDay = now + TimeSpan(1, 0, 0, 0);
        setNextAlarm(nextDay, autoStarts, false);
    }
    else
    {
        char nextAlarmDateString[18] = "YYYY-MM-DD hh:mm";
        nextAlarm.dateTime.toString(nextAlarmDateString);

        Serial.print("next alarm found ");
        Serial.println(nextAlarmDateString);

        // set next alarm
        if (!rtc.setAlarm1(nextAlarm.dateTime, DS3231_A1_Hour))
        {
            Serial.println("Error, alarm wasn't set!");
        }

        nextAlarmResult = nextAlarm;
    }
}

void setNextAlarm()
{
    Serial.println("setNextAlarm()");

    // reset old alarm states
    rtc.clearAlarm(1);
    rtc.clearAlarm(2);

    nextAlarmResult.index = 255; // 255 = no next alarm
    nextAlarmResult.wateringDuration = 0;

    StaticJsonDocument<1024> doc = getAutoStartsJson();
    JsonArray autoStarts = doc.as<JsonArray>();

    DateTime now = rtc.now();
    setNextAlarm(now, autoStarts, true);
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
        //.setPassword(WiFiSettings.password.c_str())
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

    Serial.print("watering started for ");
    Serial.print(seconds);
    Serial.println(" seconds");
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

    if (rtc.lostPower())
    {
        Serial.println("RTC time isn't configured");
        //rtc.adjust(staticDate);
    }

    //we don't need the 32K Pin, so disable it
    rtc.disable32K();

    // stop oscillating signals at SQW Pin
    // otherwise setAlarm1 will fail
    rtc.writeSqwPinMode(DS3231_OFF);

    rtc.clearAlarm(1);
    rtc.clearAlarm(2);
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

            char *iso8601dateTime = NTP.getTimeDateString(time(NULL), "%04Y-%02m-%02dT%02H:%02M:%02S");
            DateTime date = DateTime(iso8601dateTime);
            Serial.println(date.isValid());

            // set time twice because of data errors (RTC year was set to "2165" on errors)

            delay(200);
            rtc.adjust(date);

            delay(200);
            rtc.adjust(date);

            timeWasSynced = true;
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

    server.on("/api/next-auto-start", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (nextAlarmResult.index == 255) // 255 = no next alarm
        {
            request->send(204);
            return;
        }

        char date[24] = "DD-MMM-YYYYThh:mm:ss";
        nextAlarmResult.dateTime.toString(date);

        StaticJsonDocument<200> doc;
        doc["date"] = date; // TODO format with timezone
        doc["watering-duration"] = nextAlarmResult.wateringDuration;

        StringStream stream;
        auto size = serializeJson(doc, stream);

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

    /*WiFiSettings.onConfigSaved = []() {
        ESP.restart();
    };*/

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

void loop()
{
    ArduinoOTA.handle();

    if (!isPortalActive && !isUpdating)
    {
        if (isWifiConnected && isWifiSuccess && timeWasSynced)
        {
            if (lastInfoSend == 0 || millis() - lastInfoSend >= UPDATE_INTERVAL)
            {
                Serial.print("NTP time: ");
                Serial.println(NTP.getTimeDateString());

                Serial.print("RTC time: ");
                Serial.println(getFormatedRtcNow());

                lastInfoSend = millis();
            }

            if (!wasInitAlarmsSet)
            {
                wasInitAlarmsSet = true;

                setNextAlarm();
            }
        }

        if (timeWasSynced && rtc.alarmFired(1))
        {
            Serial.println("alarm fired");

            if (nextAlarmResult.index != 255)
            {
                startWaterpump(nextAlarmResult.wateringDuration);
            }

            setNextAlarm();
        }
    }
}
