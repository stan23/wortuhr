#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <stdlib.h>

#include "LedCtrl.h"
#include "Ota.h"
#include "Persistent.h"
#include "Time.h"
#include "html_cfg.h"
#include "html_wifi_cfg.h"

enum Mode { MODE_INITIAL_CONFIG, MODE_NORMAL };

/* VARIABLES */
Mode mode;

LedCtrl ledCtrl;
Ota ota;
TimeGetter timeGetter;
AsyncWebServer server(80);
Persistent persistent;

void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}

void setupForInitialConfig(void) {
  mode = MODE_INITIAL_CONFIG;

  IPAddress local_IP(192, 168, 25, 1);
  IPAddress gateway(192, 168, 25, 1);
  IPAddress subnet(255, 255, 255, 0);

  // Enable WiFi accesspoint so that user can configure its WiFi SSID and
  // password
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP("WORTUHR_AP");
  Serial.print("softAP IP:");
  Serial.println(WiFi.softAPIP());

  ledCtrl.setup(persistent.color().r, persistent.color().g, persistent.color().b);
  ledCtrl.showNoWlan();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Client connected");
    request->send(200, "text/html", FPSTR(htmlCfgWifi));
  });
  server.on("/", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println("Configuration set");
    if (request->hasParam("ssid", true)) {
      persistent.ssid(request->getParam("ssid", true)->value().c_str());
      Serial.print("SSID set to: ");
      Serial.println(persistent.ssid());
      request->send(200, "text/html", FPSTR(htmlCfgWifiConfirm));
    } else {
      request->send(200, "text/html", FPSTR(htmlCfgReject));
      Serial.println("No SSID");
    }
    if (request->hasParam("passwd", true)) {
      persistent.wifiPwd(request->getParam("passwd", true)->value().c_str());
      Serial.print("WiFi password set to: ");
      Serial.println(persistent.wifiPwd());
    } else {
      Serial.println("No WiFi password");
    }
    persistent.updateToFlash();
    ESP.restart();
  });
}

char configBuffer[428]; // 428 comes from ArduinoJson Assistant
void setupForNormal(void) {
  mode = MODE_NORMAL;

  Serial.print("Connecting to SSID: ");
  Serial.println(persistent.ssid());

  ledCtrl.setup(persistent.color().r, persistent.color().g, persistent.color().b);
  ledCtrl.showNoWlan();

  // Set WiFi to station mode and disconnect from an AP if it was previously
  // connected
  WiFi.mode(WIFI_STA);
  WiFi.begin(persistent.ssid(), persistent.wifiPwd());
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  ledCtrl.showWlan();
  ota.setup();
  timeGetter.setup();

  // Serving the standard configuration page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Client connected");
    request->send(200, "text/html", FPSTR(htmlCfg));
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Configuration requested");
    const size_t capacity = JSON_OBJECT_SIZE(14);
    DynamicJsonDocument doc(capacity);
    doc["hostname"] = persistent.hostname();
    doc["timeZoneOffset"] = persistent.timeZoneOffset();
    doc["dayLightSaving"] = persistent.dayLightSaving();
    doc["colorRed"] = persistent.color().r;
    doc["colorGreen"] = persistent.color().g;
    doc["colorBlue"] = persistent.color().b;
    doc["nightOffActive"] = persistent.nightOff().active;
    doc["nightOffOffHour"] = persistent.nightOff().offHour;
    doc["nightOffOffMinute"] = persistent.nightOff().offMinute;
    doc["nightOffOnHour"] = persistent.nightOff().onHour;
    doc["nightOffOnMinute"] = persistent.nightOff().onMinute;
    doc["dimActive"] = persistent.dim().active;
    doc["dimBase"] = persistent.dim().base;
    doc["dimScale"] = persistent.dim().scale;
    size_t usedBytes = serializeJson(doc, configBuffer, sizeof(configBuffer));
    Serial.print(usedBytes);
    Serial.print(" of ");
    Serial.print(sizeof(configBuffer));
    Serial.println(" bytes buffer were used");
    request->send(200, "application/json", configBuffer);
  });

  // retrieving the configuration
  AsyncCallbackJsonWebHandler *cfgHandler = new AsyncCallbackJsonWebHandler(
      "/config", [](AsyncWebServerRequest *request, JsonVariant &json) {
        Serial.println("Configuration received");
        JsonObject jsonObj = json.as<JsonObject>();
        if (jsonObj.containsKey("hostname")) {
          const char *hostname = jsonObj["hostname"];
             persistent.hostname(hostname);
        } else {
          Serial.println("No hostname found");
        }
        if (jsonObj.containsKey("timeZoneOffset")) {
          int timeZoneOffset = jsonObj["timeZoneOffset"];
          persistent.timeZoneOffset(timeZoneOffset);
        } else {
          Serial.println("No time zone offset found");
        }
        if (jsonObj.containsKey("dayLightSaving")) {
          bool dayLightSaving = jsonObj["dayLightSaving"];
          persistent.dayLightSaving(dayLightSaving);
        } else {
          Serial.println("No day light saving found");
        }

        Persistent::Color c;
        if (jsonObj.containsKey("colorRed")) {
          c.r = jsonObj["colorRed"];
        }
        if (jsonObj.containsKey("colorGreen")) {
          c.g = jsonObj["colorGreen"];
        }
        if (jsonObj.containsKey("colorBlue")) {
          c.b = jsonObj["colorBlue"];
        }
        persistent.color(c);
        ledCtrl.setColor(c.r, c.g, c.b);

        Persistent::NightOff no;
        if (jsonObj.containsKey("nightOffActive")) {
          no.active = jsonObj["nightOffActive"];
        }
        if (jsonObj.containsKey("nightOffOffHour")) {
          no.offHour = jsonObj["nightOffOffHour"];
        }
        if (jsonObj.containsKey("nightOffOffMinute")) {
          no.offMinute = jsonObj["nightOffOffMinute"];
        }
        if (jsonObj.containsKey("nightOffOnHour")) {
          no.onHour = jsonObj["nightOffOnHour"];
        }
        if (jsonObj.containsKey("nightOffOnMinute")) {
          no.onMinute = jsonObj["nightOffOnMinute"];
        }
        persistent.nightOff(no);

        Persistent::Dim d;
        if (jsonObj.containsKey("dimActive")) {
          d.active = jsonObj["dimActive"];
        }
        if (jsonObj.containsKey("dimBase")) {
          d.base = jsonObj["dimBase"];
        }
        if (jsonObj.containsKey("dimScale")) {
          d.scale = jsonObj["dimScale"];
        }
        persistent.dim(d);
        persistent.print();

        persistent.updateToFlash();
        ledCtrl.forceUpdate = true;
      });
  server.addHandler(cfgHandler);

  // testing color
  AsyncCallbackJsonWebHandler *colorHandler = new AsyncCallbackJsonWebHandler(
      "/color", [](AsyncWebServerRequest *request, JsonVariant &json) {
        Serial.println("Color received");
        uint8_t r = persistent.color().r;
        uint8_t g = persistent.color().g;
        uint8_t b = persistent.color().b;
        JsonObject jsonObj = json.as<JsonObject>();
        if (jsonObj.containsKey("colorRed")) {
          r = jsonObj["colorRed"];
        }
        if (jsonObj.containsKey("colorGreen")) {
          g = jsonObj["colorGreen"];
        }
        if (jsonObj.containsKey("colorBlue")) {
          b = jsonObj["colorBlue"];
        }
        Serial.print("New Color: ");
        Serial.print(r);
        Serial.print(" ");
        Serial.print(g);
        Serial.print(" ");
        Serial.print(b);
        ledCtrl.setColor(r, g, b);
        ledCtrl.forceUpdate = true;
      });
  server.addHandler(colorHandler);

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void setup() {
    Serial.begin(115200);
    persistent.setup();

    if (!strcmp(persistent.ssid(), "")) {
      setupForInitialConfig();
    } else {
      setupForNormal();
    }

    server.onNotFound(notFound);
    server.begin();

    Serial.println("Setup done");
}

void loop() {
    ota.loop();
    if (mode == MODE_NORMAL) {
      int h;
      int m;
      timeGetter.getTime(h, m);

      h += persistent.timeZoneOffset() + (persistent.dayLightSaving()?1:0);
      ledCtrl.setClock(h, m);

      delay(100);
    }
}