#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <Roomba.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Timezone.h>
#include "config.h"
extern "C" {
#include "user_interface.h"
}

// Remote debugging over telnet. Just run:
// `telnet roomba.local` OR `nc roomba.local 23`
#if LOGGING
#include <RemoteDebug.h>
#define DLOG(msg, ...) if(Debug.isActive(Debug.DEBUG)){Debug.printf(msg, ##__VA_ARGS__);}
#define VLOG(msg, ...) if(Debug.isActive(Debug.VERBOSE)){Debug.printf(msg, ##__VA_ARGS__);}
RemoteDebug Debug;
#else
#define DLOG(msg, ...)
#endif

// Roomba setup
Roomba roomba(&Serial, Roomba::Baud115200);

// Roomba state
typedef struct {
  // Sensor values
  int16_t distance;
  uint8_t chargingState;
  uint16_t voltage;
  int16_t current;
  // Supposedly unsigned according to the OI docs, but I've seen it
  // underflow to ~65000mAh, so I think signed will work better.
  int16_t charge;
  uint16_t capacity;

  // Derived state
  bool cleaning;
  bool docked;

  int timestamp;
  bool sent;
} RoombaState;

RoombaState roombaState = {};

// Roomba sensor packet
uint8_t roombaPacket[100];
uint8_t sensors[] = {
  Roomba::SensorDistance, // PID 19, 2 bytes, mm, signed
  Roomba::SensorChargingState, // PID 21, 1 byte
  Roomba::SensorVoltage, // PID 22, 2 bytes, mV, unsigned
  Roomba::SensorCurrent, // PID 23, 2 bytes, mA, signed
  Roomba::SensorBatteryCharge, // PID 25, 2 bytes, mAh, unsigned
  Roomba::SensorBatteryCapacity // PID 26, 2 bytes, mAh, unsigned
};

// Central European Time (Frankfurt, Paris)
TimeChangeRule CEST = {"CEST", Last, Sun, Mar, 2, 120};     // Central European Summer Time
TimeChangeRule CET = {"CET ", Last, Sun, Oct, 3, 60};       // Central European Standard Time
Timezone tz(CEST, CET);

// Network setup
WiFiClient wifiClient;
bool OTAStarted;

// MQTT setup
PubSubClient mqttClient(wifiClient);
const char *commandTopic = MQTT_COMMAND_TOPIC;
const char *stateTopic = MQTT_STATE_TOPIC;
const char *configTopic = MQTT_CONFIG_TOPIC;

void wakeup() {
  DLOG("Wakeup Roomba\n");
  pinMode(BRC_PIN,OUTPUT);
  digitalWrite(BRC_PIN,LOW);
  delay(200);
  pinMode(BRC_PIN,INPUT);
  delay(200);
  Serial.write(128); // Start
}

void wakeOnDock() {
  DLOG("Wakeup Roomba on dock\n");
  wakeup();
#ifdef ROOMBA_650_SLEEP_FIX
  // Some black magic from @AndiTheBest to keep the Roomba awake on the dock
  // See https://github.com/johnboiles/esp-roomba-mqtt/issues/3#issuecomment-402096638
  delay(10);
  Serial.write(135); // Clean
  delay(150);
  Serial.write(143); // Dock
#endif
}

void wakeOffDock() {
  DLOG("Wakeup Roomba off Dock\n");
  Serial.write(131); // Safe mode
  delay(300);
  Serial.write(130); // Passive mode
}

bool performCommand(const char *cmdchar) {
  wakeup();

  // Char* string comparisons dont always work
  String cmd(cmdchar);

  // MQTT protocol commands
  if (cmd == "turn_on") {
    DLOG("Turning on\n");
    roomba.cover();
    roombaState.cleaning = true;
  } else if (cmd == "turn_off") {
    DLOG("Turning off\n");
    roomba.power();
    roombaState.cleaning = false;
  } else if (cmd == "start" || cmd == "pause") {
    DLOG("Toggling\n");
    roomba.cover();
  } else if (cmd == "stop") {
    if (roombaState.cleaning) {
      DLOG("Stopping\n");
      roomba.cover();
    } else {
      DLOG("Not cleaning, can't stop\n");
    }
  } else if (cmd == "clean_spot") {
    DLOG("Cleaning Spot\n");
    roombaState.cleaning = true;
    roomba.spot();
  } else if (cmd == "locate") {
    DLOG("Playing song #0\n");
    roomba.safeMode();
    delay(50);
    roomba.playSong(0);
    delay(4000);
    roomba.playSong(1);
    delay(4000);
    roomba.playSong(2);
    delay(3500);
    roomba.playSong(3);
  } else if (cmd == "return_to_base") {
    DLOG("Returning to Base\n");
    roombaState.cleaning = true;
    roomba.dock();
  } else {
    return false;
  }
  return true;
}

char* getMAC(const char* divider = "") {
  byte MAC[6];
  WiFi.macAddress(MAC);
  static char MACc[30];
  sprintf(MACc, "%02X%s%02X%s%02X%s%02X%s%02X%s%02X", MAC[0], divider, MAC[1], divider, MAC[2], divider, MAC[3], divider, MAC[4], divider, MAC[5]);
  return strlwr(MACc);
}

char* getEntityID() {
  char entityID[100];
  sprintf(entityID, "%s%s", MQTT_IDPREFIX, getMAC());
  // avoid confusions with lower/upper case differences in IDs
  return strlwr(entityID);
}

char* getMQTTTopic(const char* topic) {
  // build mqtt target topic
  static char mqttTopic[200];
  sprintf(mqttTopic, "%s%s%s%s", MQTT_TOPIC_BASE, getEntityID(), MQTT_DIVIDER, topic);
  return mqttTopic;
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  DLOG("Received mqtt callback for topic %s\n", topic);
  if (strcmp(getMQTTTopic(commandTopic), topic) == 0) {
    // turn payload into a null terminated string
    char *cmd = (char *)malloc(length + 1);
    memcpy(cmd, payload, length);
    cmd[length] = 0;

    if(!performCommand(cmd)) {
      DLOG("Unknown command %s\n", cmd);
    }
    free(cmd);
  }
}

float readADC(int samples) {
  // Basic code to read from the ADC
  int adc = 0;
  for (int i = 0; i < samples; i++) {
    delay(1);
    adc += analogRead(A0);
  }
  adc = adc / samples;
  float mV = adc * ADC_VOLTAGE_DIVIDER;
  VLOG("ADC for %d is %.1fmV with %d samples\n", adc, mV, samples);
  return mV;
}

void setDateTime() {
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    now = time(nullptr);
  }
  time_t local = tz.toLocal(now);
  roomba.setDayTime(dayOfWeek(local)-1, hour(local), minute(local));
}

void debugCallback() {
  String cmd = Debug.getLastCommand();

  // Debugging commands via telnet
  if (performCommand(cmd.c_str())) {
  } else if (cmd == "quit") {
    DLOG("Stopping Roomba\n");
    Serial.write(173);
  } else if (cmd == "rreset") {
    DLOG("Resetting Roomba\n");
    roomba.reset();
  } else if (cmd == "mqtthello") {
    mqttClient.publish("vacuum/hello", "hello there");
  } else if (cmd == "version") {
    const char compile_date[] = __DATE__ " " __TIME__;
    DLOG("Compiled on: %s\n", compile_date);
  } else if (cmd == "baud115200") {
    DLOG("Setting baud to 115200\n");
    Serial.begin(115200);
    delay(100);
  } else if (cmd == "baud19200") {
    DLOG("Setting baud to 19200\n");
    Serial.begin(19200);
    delay(100);
  } else if (cmd == "baud57600") {
    DLOG("Setting baud to 57600\n");
    Serial.begin(57600);
    delay(100);
  } else if (cmd == "baud38400") {
    DLOG("Setting baud to 38400\n");
    Serial.begin(38400);
    delay(100);
  } else if (cmd == "sleep5") {
    DLOG("Going to sleep for 5 seconds\n");
    delay(100);
    ESP.deepSleep(5e6);
  } else if (cmd == "wake") {
    DLOG("Toggle BRC pin\n");
    wakeup();
  } else if (cmd == "readadc") {
    float adc = readADC(10);
    DLOG("ADC voltage is %.1fmV\n", adc);
  } else if (cmd == "streamresume") {
    DLOG("Resume streaming\n");
    roomba.streamCommand(Roomba::StreamCommandResume);
  } else if (cmd == "streampause") {
    DLOG("Pause streaming\n");
    roomba.streamCommand(Roomba::StreamCommandPause);
  } else if (cmd == "stream") {
    DLOG("Requesting stream\n");
    roomba.stream(sensors, sizeof(sensors));
  } else if (cmd == "streamreset") {
    DLOG("Resetting stream\n");
    roomba.stream({}, 0);
  } else if (cmd == "time") {
    setDateTime();
  } else {
    DLOG("Unknown command %s\n", cmd.c_str());
  }
}

void sleepIfNecessary() {
#ifdef ENABLE_ADC_SLEEP
  // Check the battery, if it's too low, sleep the ESP (so we don't murder the battery)
  float mV = readADC(10);
  // According to this post, you want to stop using NiMH batteries at about 0.9V per cell
  // https://electronics.stackexchange.com/a/35879 For a 12 cell battery like is in the Roomba,
  // That's 10.8 volts.
  if (mV < 10800) {
    // Fire off a quick message with our most recent state, if MQTT is connected
    DLOG("Battery voltage is low (%.1fV). Sleeping for 10 minutes\n", mV / 1000);
    if (mqttClient.connected()) {
      StaticJsonDocument<200> root;
      root["battery_level"] = 0;
      root["cleaning"] = false;
      root["docked"] = false;
      root["charging"] = false;
      root["voltage"] = mV / 1000;
      root["charge"] = 0;
      String jsonStr;
      serializeJson(root, jsonStr);
      mqttClient.publish(getMQTTTopic(stateTopic), jsonStr.c_str(), true);
    }
    delay(200);

    // Sleep for 10 minutes
    ESP.deepSleep(600e6);
  }
#endif
}

bool parseRoombaStateFromStreamPacket(uint8_t *packet, int length, RoombaState *state) {
  state->timestamp = millis();
  int i = 0;
  while (i < length) {
    switch(packet[i]) {
      case Roomba::Sensors7to26: // 0
        i += 27;
        break;
      case Roomba::Sensors7to16: // 1
        i += 11;
        break;
      case Roomba::SensorVirtualWall: // 13
        i += 2;
        break;
      case Roomba::SensorDistance: // 19
        state->distance = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorChargingState: // 21
        state->chargingState = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorVoltage: // 22
        state->voltage = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorCurrent: // 23
        state->current = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorBatteryCharge: // 25
        state->charge = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorBatteryCapacity: //26
        state->capacity = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorBumpsAndWheelDrops: // 7
        i += 2;
        break;
      case 128: // Unknown
        i += 2;
        break;
      default:
        VLOG("Unhandled Packet ID %d\n", packet[i]);
        return false;
        break;
    }
  }
  return true;
}

void verboseLogPacket(uint8_t *packet, uint8_t length) {
    VLOG("Packet: ");
    for (int i = 0; i < length; i++) {
      VLOG("%d ", packet[i]);
    }
    VLOG("\n");
}

void readSensorPacket() {
  uint8_t packetLength;
  bool received = roomba.pollSensors(roombaPacket, sizeof(roombaPacket), &packetLength);
  if (received) {
    RoombaState rs = {};
    bool parsed = parseRoombaStateFromStreamPacket(roombaPacket, packetLength, &rs);
    verboseLogPacket(roombaPacket, packetLength);
    if (parsed) {
      roombaState = rs;
      VLOG("Got Packet of len=%d! Distance:%dmm ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh\n", packetLength, roombaState.distance, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity);
      roombaState.cleaning = false;
      roombaState.docked = false;
      if (roombaState.current < -400) {
        roombaState.cleaning = true;
      } else if (roombaState.current > -50) {
        roombaState.docked = true;
      }
    } else {
      VLOG("Failed to parse packet\n");
    }
  }
}

void onOTAStart() {
  DLOG("Starting OTA session\n");
  DLOG("Pause streaming\n");
  roomba.streamCommand(Roomba::StreamCommandPause);
  OTAStarted = true;
}

void setup() {
  // High-impedence on the BRC_PIN
  pinMode(BRC_PIN,INPUT);

  // Sleep immediately if ENABLE_ADC_SLEEP and the battery is low
  sleepIfNecessary();

  // Set Hostname.
  String hostname(HOSTNAME);
  WiFi.hostname(hostname);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  ArduinoOTA.setHostname((const char *)hostname.c_str());
  ArduinoOTA.begin();
  ArduinoOTA.onStart(onOTAStart);

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  #if LOGGING
  Debug.begin((const char *)hostname.c_str());
  Debug.setResetCmdEnabled(true);
  Debug.setCallBackProjectCmds(debugCallback);
  Debug.setSerialEnabled(false);
  #endif

  // Learn locate song
  roomba.safeMode();
  byte locateSong0[18] = {55, 32, 55, 32, 55, 32, 51, 24, 58, 8, 55, 32, 51, 24, 58, 8, 55, 64};
  byte locateSong1[18] = {62, 32, 62, 32, 62, 32, 63, 24, 58, 8, 54, 32, 51, 24, 58, 8, 55, 64};
  byte locateSong2[24] = {67, 32, 55, 24, 55, 8, 67, 32, 66, 24, 65, 8, 64, 8, 63, 8, 64, 16, 30, 16, 56, 16, 61, 32};
  byte locateSong3[28] = {60, 24, 59, 8, 58, 8, 57, 8, 58, 16, 10, 16, 52, 16, 54, 32, 51, 24, 58, 8, 55, 32, 51, 24, 58, 8, 55, 64};
  roomba.song(0, locateSong0, 18);
  roomba.song(1, locateSong1, 18);
  roomba.song(2, locateSong2, 24);
  roomba.song(3, locateSong3, 28);

  roomba.start();
  delay(100);
  // Reset stream sensor values
  roomba.stream({}, 0);
  delay(100);
  // Request sensor stream
  roomba.stream(sensors, sizeof(sensors));

  #ifdef SET_DATETIME
  wakeup();
  // set time
  setDateTime();
  #endif
}

void reconnect() {
  DLOG("Attempting MQTT connection...\n");
  // Attempt to connect
  if (mqttClient.connect(HOSTNAME, MQTT_USER, MQTT_PASSWORD)) {
    DLOG("MQTT connected\n");
    mqttClient.subscribe(getMQTTTopic(commandTopic));
  } else {
    DLOG("MQTT failed rc=%d try again in 5 seconds\n", mqttClient.state());
  }
}

void sendConfig() {
  if (!mqttClient.connected()) {
    DLOG("MQTT Disconnected, not sending config\n");
    return;
  }
  StaticJsonDocument<500> root;
  root["name"] = String("Roomba ") + getMAC();
  root["unique_id"] = getEntityID();
  root["schema"] = "state";
  char baseTopic[200];
  sprintf(baseTopic, "%s%s", MQTT_TOPIC_BASE, getEntityID());
  root["~"] = baseTopic;
  root["stat_t"] = String("~/") + stateTopic;
  root["cmd_t"] = String("~/") + commandTopic;
  root["send_cmd_t"] = String("~/") + commandTopic;
  root["json_attr_t"] = String("~/") + stateTopic;
  root["sup_feat"][0] = "start";
  root["sup_feat"][1] = "stop";
  root["sup_feat"][2] = "pause";
  root["sup_feat"][3] = "return_home";
  root["sup_feat"][4] = "locate";
  root["sup_feat"][5] = "clean_spot";
  root["dev"]["name"] = String("Roomba ") + getMAC();
  root["dev"]["ids"][0] = getEntityID();
  root["dev"]["mf"] = "iRobot";
  root["dev"]["mdl"] = ROOMBA_MODEL;
  String jsonStr;
  serializeJson(root, jsonStr);
  DLOG("Reporting config: %s\n", jsonStr.c_str());
  mqttClient.publish(getMQTTTopic(configTopic), jsonStr.c_str());
}

void sendStatus() {
  if (!mqttClient.connected()) {
    DLOG("MQTT Disconnected, not sending status\n");
    return;
  }
  StaticJsonDocument<200> root;
  root["battery_level"] = (roombaState.charge * 100)/roombaState.capacity;
  root["cleaning"] = roombaState.cleaning;
  root["docked"] = roombaState.docked;
  root["charging"] = roombaState.chargingState == Roomba::ChargeStateReconditioningCharging
  || roombaState.chargingState == Roomba::ChargeStateFullCharging
  || roombaState.chargingState == Roomba::ChargeStateTrickleCharging;
  root["voltage"] = roombaState.voltage;
  root["current"] = roombaState.current;
  root["charge"] = roombaState.charge;
  String curState = "idle";
  if (roombaState.docked) {
    curState = "docked";
  } else {
    if (roombaState.cleaning) {
      curState = "cleaning";
    }
  }
  root["state"] = curState;
  String jsonStr;
  serializeJson(root, jsonStr);
  DLOG("Reporting status: %s\n", jsonStr.c_str());
  mqttClient.publish(getMQTTTopic(stateTopic), jsonStr.c_str());
}

int lastStateMsgTime = 0;
int lastWakeupTime = 0;
int lastConnectTime = 0;
int configLoop = 0;

void loop() {
  // Important callbacks that _must_ happen every cycle
  ArduinoOTA.handle();
  yield();
  Debug.handle();

  // Skip all other logic if we're running an OTA update
  if (OTAStarted) {
    return;
  }

  long now = millis();
  // If MQTT client can't connect to broker, then reconnect every 30 seconds
  if ((now - lastConnectTime) > 30000) {
    lastConnectTime = now;
    if (!mqttClient.connected()) {
      DLOG("Reconnecting MQTT\n");
      reconnect();
      sendConfig();
    } else {
      // resend config every now and then to reconfigure entity e.g. in case homeassistant has been restarted
      if (configLoop == 19) {
        sendConfig();
        configLoop = 0;
      } else {
        configLoop++;
      }
    }
  }
  // Wakeup the roomba at fixed intervals - every 50 seconds
  if (now - lastWakeupTime > 50000) {
    lastWakeupTime = now;
    if (!roombaState.cleaning) {
      if (roombaState.docked) {
        wakeOnDock();
      } else {
        // wakeOffDock();
        wakeup();
      }
    } else {
      wakeup();
    }
  }
  // Report the status over mqtt at fixed intervals
  if (now - lastStateMsgTime > 10000) {
    lastStateMsgTime = now;
    if (now - roombaState.timestamp > 30000 || roombaState.sent) {
      DLOG("Roomba state already sent (%.1fs old)\n", (now - roombaState.timestamp)/1000.0);
      DLOG("Request stream\n");
      roomba.stream(sensors, sizeof(sensors));
    } else {
      sendStatus();
      roombaState.sent = true;
    }
    sleepIfNecessary();
  }

  readSensorPacket();
  mqttClient.loop();
}
