
/*
  Version 0.1 - Jan 05 2019
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <WebSocketsClient.h> //  https://github.com/kakopappa/sinric/wiki/How-to-add-dependency-libraries
#include <ArduinoJson.h> // https://github.com/kakopappa/sinric/wiki/How-to-add-dependency-libraries
#include <StreamString.h>
#include "DHT.h"        // including the library of DHT11 temperature and humidity sensor

#define DEBUG_WEBSOCKETS true

ESP8266WiFiMulti WiFiMulti;
WebSocketsClient webSocket;

#define DHTTYPE DHT11   // DHT 11

#define dht_dpin 2
DHT dht(dht_dpin, DHTTYPE);

#define MyApiKey "027fbd2d-1d8e-4495-9441-e07bc66932b7" // TODO: Change to your sinric API Key. Your API Key is displayed on sinric.com dashboard
#define MySSID "CMU-DEVICE" // TODO: Change to your Wifi network SSID

#define HEARTBEAT_INTERVAL  500000 // 5 Minutes 
#define TEMPRATURE_INTERVAL 3000 // 3 Minutes 

#define SERVER_URL "iot.sinric.com" //"iot.sinric.com"
#define SERVER_PORT 80 // 80

uint64_t heartbeatTimestamp = 0;
uint64_t tempratureUpdateTimestamp = 0;
bool isConnected = false;

float targetTemp = 0;
bool isOn = false;

void setPowerStateOnServer(String deviceId, String value);
void setSetTemperatureSettingOnServer(String deviceId, float setPoint, String scale, float ambientTemperature, float ambientHumidity);
float readTempature();

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      isConnected = false;
      Serial.printf("[WSc] Webservice disconnected from sinric.com!\n");
      break;
    case WStype_CONNECTED: {
        isConnected = true;
        Serial.printf("[WSc] Service connected to sinric.com at url: %s\n", payload);
        Serial.printf("Waiting for commands from sinric.com ...\n");
      }
      break;
    case WStype_TEXT: {
        Serial.printf("[WSc] get text: %s\n", payload);
        // Example payloads

        // For Thermostat
        // {"deviceId": xxxx, "action": "setPowerState", value: "ON"} // https://developer.amazon.com/docs/device-apis/alexa-thermostatcontroller.html
        // {"deviceId": xxxx, "action": "SetTargetTemperature", value: "targetSetpoint": { "value": 20.0, "scale": "CELSIUS"}} // https://developer.amazon.com/docs/device-apis/alexa-thermostatcontroller.html#settargettemperature
        // {"deviceId": xxxx, "action": "AdjustTargetTemperature", value: "targetSetpointDelta": { "value": 2.0, "scale": "FAHRENHEIT" }} // https://developer.amazon.com/docs/device-apis/alexa-thermostatcontroller.html#adjusttargettemperature
        // {"deviceId": xxxx, "action": "SetThermostatMode", value: "thermostatMode" : { "value": "COOL" }} // https://developer.amazon.com/docs/device-apis/alexa-thermostatcontroller.html#setthermostatmode

#if ARDUINOJSON_VERSION_MAJOR == 5
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject((char*)payload);
#endif
#if ARDUINOJSON_VERSION_MAJOR == 6
        DynamicJsonDocument json(1024);
        deserializeJson(json, (char*) payload);
#endif
        String deviceId = json ["deviceId"];
        String action = json ["action"];

        /*if (deviceId == "5axxxxxxxxxxxxxxxxxxx") // Device ID of first device
          {
          // Check device id if you have multiple devices.
          } */

        if (action == "setPowerState") { // On or Off
          String value = json ["value"];

          if (value == "ON") {
            isOn = true;
            digitalWrite(5, LOW);
          }
          else if (value == "OFF") {
            isOn = false;
            digitalWrite(5, HIGH);
          }

          Serial.println("[WSc] setPowerState" + value);
        }
        else if (action == "SetTargetTemperature") {
          // Alexa, set thermostat to 20
          //String value = json ["value"];
          String value = json["value"]["targetSetpoint"]["value"];
          String scale = json["value"]["targetSetpoint"]["scale"];

          targetTemp = value.toFloat();

          Serial.println("[WSc] SetTargetTemperature value: " + value);
          Serial.println("[WSc] SetTargetTemperature scale: " + scale);
        }
        else if (action == "AdjustTargetTemperature") {
          //Alexa, make it warmer in here
          //Alexa, make it cooler in here
          String value = json["value"]["targetSetpointDelta"]["value"];
          String scale = json["value"]["targetSetpointDelta"]["scale"];

          targetTemp = value.toFloat();

          Serial.println("[WSc] AdjustTargetTemperature value: " + value);
          Serial.println("[WSc] AdjustTargetTemperature scale: " + scale);
        }
        else if (action == "SetThermostatMode") {
          //Alexa, set thermostat name to mode
          //Alexa, set thermostat to automatic
          //Alexa, set kitchen to off
          String value = json["value"]["thermostatMode"]["value"];

          Serial.println("[WSc] SetThermostatMode value: " + value);
        }
        else if (action == "test") {
          Serial.println("[WSc] received test command from sinric.com");
        }
      }

      break;
    case WStype_BIN:
      Serial.printf("[WSc] get binary length: %u\n", length);
      break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);

  WiFiMulti.addAP(MySSID);
  Serial.println();
  Serial.print("Connecting to Wifi: ");
  Serial.println(MySSID);

  // Waiting for Wifi connect
  while (WiFiMulti.run() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  if (WiFiMulti.run() == WL_CONNECTED) {
    Serial.println("");
    Serial.print("WiFi connected. ");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  }

  //set up the relay control pin
  pinMode(5, OUTPUT);

  // server address, port and URL
  webSocket.begin(SERVER_URL, SERVER_PORT, "/");

  // event handler
  webSocket.onEvent(webSocketEvent);
  webSocket.setAuthorization("apikey", MyApiKey);

  // try again every 5000ms if connection has failed
  webSocket.setReconnectInterval(5000);   // If you see 'class WebSocketsClient' has no member named 'setReconnectInterval' error update arduinoWebSockets
}

void loop() {
  webSocket.loop();

  if (isConnected) {
    uint64_t now = millis();


    // Send heartbeat in order to avoid disconnections during ISP resetting IPs over night. Thanks @MacSass
    if ((now - heartbeatTimestamp) > HEARTBEAT_INTERVAL) {
      heartbeatTimestamp = now;
      webSocket.sendTXT("H");
    }

    // Send the tempature settings to server
    if ((now - tempratureUpdateTimestamp) > TEMPRATURE_INTERVAL) {
      tempratureUpdateTimestamp = now;
      float currentTemp = readTempature();

      //compare current temp to set temp
      if (isOn) {
        if (currentTemp >= targetTemp) {
          Serial.println("In here");
          // turn off the heat
          digitalWrite(5, HIGH);

        }
        else {
          // turn on the heat
          digitalWrite(5, LOW);
        }
      }
    }
  }
}

// Read tempratre from DHT Sensor

float readTempature() {
  //delay(dht.getMinimumSamplingPeriod());

  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature(true);

  Serial.print("Current humidity = ");
  Serial.print(humidity);
  Serial.print("%  ");
  Serial.print("temperature = ");
  Serial.print(temperature);
  Serial.println("F  ");

  setSetTemperatureSettingOnServer("5de16d8f080be54524821e53", targetTemp, "FAHRENHEIT", temperature, humidity);

  return temperature;
}

// If you are going to use a push button to on/off the switch manually, use this function to update the status on the server
// so it will reflect on Alexa app.
// eg: setPowerStateOnServer("deviceid", "ON")

// Call ONLY If status changed. DO NOT CALL THIS IN loop() and overload the server.

void setPowerStateOnServer(String deviceId, String value) {
#if ARDUINOJSON_VERSION_MAJOR == 5
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
#endif
#if ARDUINOJSON_VERSION_MAJOR == 6
  DynamicJsonDocument root(1024);
#endif
  root["deviceId"] = deviceId;
  root["action"] = "setPowerState";
  root["value"] = value;
  StreamString databuf;
#if ARDUINOJSON_VERSION_MAJOR == 5
  root.printTo(databuf);
#endif
#if ARDUINOJSON_VERSION_MAJOR == 6
  serializeJson(root, databuf);
#endif

  webSocket.sendTXT(databuf);
}

// Call ONLY If status changed. DO NOT CALL THIS IN loop() and overload the server.

//eg: setSetTemperatureSettingOnServer("deviceid", 25.0, "CELSIUS" or "FAHRENHEIT", 23.0, 45.3)
// setPoint: Indicates the target temperature to set on the termostat.
void setSetTemperatureSettingOnServer(String deviceId, float setPoint, String scale, float ambientTemperature, float ambientHumidity) {
#if ARDUINOJSON_VERSION_MAJOR == 5
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
#endif
#if ARDUINOJSON_VERSION_MAJOR == 6
  DynamicJsonDocument root(1024);
#endif
  root["action"] = "SetTemperatureSetting";
  root["deviceId"] = deviceId;

#if ARDUINOJSON_VERSION_MAJOR == 5
  JsonObject& valueObj = root.createNestedObject("value");
  JsonObject& temperatureSetting = valueObj.createNestedObject("temperatureSetting");
#endif
#if ARDUINOJSON_VERSION_MAJOR == 6
  JsonObject valueObj = root.createNestedObject("value");
  JsonObject temperatureSetting = valueObj.createNestedObject("temperatureSetting");
#endif
  temperatureSetting["setPoint"] = setPoint;
  temperatureSetting["scale"] = scale;
  temperatureSetting["ambientTemperature"] = ambientTemperature;
  temperatureSetting["ambientHumidity"] = ambientHumidity;

  StreamString databuf;
#if ARDUINOJSON_VERSION_MAJOR == 5
  root.printTo(databuf);
#endif
#if ARDUINOJSON_VERSION_MAJOR == 6
  serializeJson(root, databuf);
#endif

  webSocket.sendTXT(databuf);
}
// Call ONLY If status changed. DO NOT CALL THIS IN loop() and overload the server.

void setThermostatModeOnServer(String deviceId, String thermostatMode) {
#if ARDUINOJSON_VERSION_MAJOR == 5
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
#endif
#if ARDUINOJSON_VERSION_MAJOR == 6
  DynamicJsonDocument root(1024);
#endif
  root["deviceId"] = deviceId;
  root["action"] = "SetThermostatMode";
  root["value"] = thermostatMode;
  StreamString databuf;
#if ARDUINOJSON_VERSION_MAJOR == 5
  root.printTo(databuf);
#endif
#if ARDUINOJSON_VERSION_MAJOR == 6
  serializeJson(root, databuf);
#endif

  webSocket.sendTXT(databuf);
}
