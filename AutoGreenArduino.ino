#include <Bonezegei_DHT11.h>
#include <Servo.h>
#include "WiFiS3.h"
#include "WiFiSSLClient.h"
#include "arduino_secrets.h" 
#include "Arduino_LED_Matrix.h"
#include <stdint.h>
#include <ArduinoJson.h>

#define PUMP1_PIN_O 9
#define HEATER_PIN_O 6
#define VENT_PIN_O 13
#define VENT_PIN_O_2 12

#define CAP_PIN_I A2
#define TEMP_INDOOR_PIN_I 4
#define TEMP_OUTDOOR_PIN_I 3

#define MANUAL_MODE 0
#define PRESET_MODE 1
#define LEARNING_MODE 2
#define IDLE_MODE 3

#define HEATING 0
#define COOLING 1
#define PRESERVE_TEMP 2

#define FLOW_RATE 33.75 // mL/second
#define wet 300
#define dry 900

char ssid[] = SECRET_SSID;       
char pass[] = SECRET_PASS;    
int keyIndex = 0;           

int status = WL_IDLE_STATUS;

char server[] = "autogreen-capstone.ca";

//WiFiClient client;
WiFiSSLClient client;

Servo ventServo;
Servo ventServo2;

Bonezegei_DHT11 dhtIndoor(TEMP_INDOOR_PIN_I);
Bonezegei_DHT11 dhtOutdoor(TEMP_OUTDOOR_PIN_I);

int heaterCheck = 0;

//HTTP
int lastHTTPCheckTime = 0;
int HTTPCheckFrequency = 5000;
int sensorPostFrequency = 5000;

//Temperature and Humidity variables and constants
float targetTemperature = 21;
const int tempCheckFrequency = 10000;
int lastTempCheckTime = 0;
float currentIndoorTemperature = 21;
float currentIndoorHumidity = 50;
float currentOutdoorTemperature = 21;
float currentOutdoorHumidity = 50;
int heatingMode = PRESERVE_TEMP;

//Capacitance
int currentSoilCapacitance = 400;

//Watering modes
int currentMode = IDLE_MODE;
int previousMode = IDLE_MODE;

//Manual Mode variables
int manualModeAmount = 0;
int thisManualModeCommandId = -1;

int thisLearningModeCommandId = -1;

//preset mode variables
int presetModeFrequency = 0;
int presetModeAmount = 0;
int presetModePreviousWateringTime = 0;
int thisPresetModeCommandId = -1;
int isFirstAutoModeWater = 0; //if this is 1, it is our first water in watering mode

int lastSensorPostTime = 0;
int sensorPostInterval = 30000; 
int firstPost = 1;

bool debugging = true;

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  ventServo.attach(VENT_PIN_O); //vent 13
  ventServo2.attach(VENT_PIN_O_2);

  ventServo.write(0);
  ventServo2.write(180);

  pinMode(PUMP1_PIN_O, OUTPUT);
  pinMode(HEATER_PIN_O, OUTPUT);

  digitalWrite(PUMP1_PIN_O, HIGH);
  digitalWrite(HEATER_PIN_O, HIGH);

  // check for the WiFi module:
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed!");
    // don't continue
    while (true);
  }
  
  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Please upgrade the firmware");
  }
  
  // attempt to connect to WiFi network:
  while (status != WL_CONNECTED) {
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(ssid);
    // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    status = WiFi.begin(ssid, pass);
     
    // wait 10 seconds for connection:
    delay(10000);
  }
  
  printWifiStatus();
 
  Serial.println("\nStarting connection to server...");
  // if you get a connection, report back via serial:
  if (client.connect(server, 443)) {
    Serial.println("connected to server");
  } else {
    Serial.println("connection to server failed");
  }
}

void loop() {
  //Step 1 in flowchart: poll endpoint for data
  int currentTime = millis();
  // if (currentTime - lastHTTPCheckTime >= HTTPCheckFrequency) {
  if (debugging) {
    Serial.println("Target temperature:");
    Serial.println(targetTemperature);
    Serial.println("CHECKING ENDPOINT");
  }

  if(!client.connected()) {
    Serial.println("Client disconnected, reconnecting");
    if (client.connect(server, 80)) {
      Serial.println("connected to server again");
    } else {
      Serial.println("Failed to reconnect to server");
    }
  }

  getRequest("/api/commands/2");
  //while (client.available() == 0) {}
  String jsonBody = read_response_json();

  if (debugging) {
    Serial.println("COMMAND:");
    Serial.println(jsonBody);
  }
  
  // Parse the JSON body
  const size_t capacity = JSON_OBJECT_SIZE(1) + 30;
  // DynamicJsonDocument doc(capacity);
  // deserializeJson(doc, jsonBody);
  DynamicJsonDocument command(capacity);
  deserializeJson(command, jsonBody);

  thisManualModeCommandId = -1;
  thisPresetModeCommandId = -1;
  thisLearningModeCommandId = -1;

  //JsonArray commands = doc.as<JsonArray>();
  //for (JsonObject command : commands) {
    int commandId = command["command_id"];
    String commandType = command["command_body"]["commandType"];
    String commandStatus = command["command_status"];

    if (debugging) {
      Serial.print("CommandId:");
      Serial.println(commandId);
      Serial.print("CommandType:");
      Serial.println(commandType);
      Serial.print("commandStatus:");
      Serial.println(commandStatus);
    }
    
    if (commandType == "SetManualWaterAmount" && commandStatus == "pending") {
      int amount = command["command_body"]["amount"];

      currentMode = MANUAL_MODE;
      manualModeAmount = amount;
      thisManualModeCommandId = commandId;
    } else if (commandType == "SetAutomaticWatering" && commandStatus == "pending") {
      if (debugging) {
        Serial.println("Made it into setAutoWatering location");
      }
      
      int amount = command["command_body"]["amount"];
      int frequency = command["command_body"]["interval"];

      currentMode = PRESET_MODE;
      presetModeAmount = amount;
      presetModeFrequency = frequency*60*1000; //convert to milliseconds from minutes
      presetModePreviousWateringTime = 0;
      thisPresetModeCommandId = commandId;
      isFirstAutoModeWater = 1;
    } else if (commandType == "SetLearningWatering"  && commandStatus == "pending") {
      currentMode = LEARNING_MODE;
      thisLearningModeCommandId = commandId;
    } else if (commandType == "SetTemperature"  && commandStatus == "pending") {
      int tempValue = command["command_body"]["temp"];
      targetTemperature = tempValue;
      String path = "/api/commands/ack/" + String(commandId);
      postRequest(path, "");
    }
  //}

  lastHTTPCheckTime = millis();

  if (debugging) {
    Serial.println("CHECK TEMPERATURE, CHANGE IF NECESSARY");
  }
  
  //Check indoor CAP_PIN_I
  if (dhtIndoor.getData()) {                         // get All data from DHT11
    currentIndoorTemperature = dhtIndoor.getTemperature();      // return temperature in celsius
    currentIndoorHumidity = dhtIndoor.getHumidity();
  }

  //check outdoor CAP_PIN_I
  if (dhtOutdoor.getData()) {                         // get All data from DHT11
    currentOutdoorTemperature = dhtOutdoor.getTemperature();      // return temperature in celsius
    currentOutdoorHumidity = dhtOutdoor.getHumidity();    
  }

  // //we need to check how current indoor temperature compares to outdoor temperatures
  if (currentIndoorTemperature < targetTemperature - 2) {
    //temp too low; keep vent closed, turn on heater
    digitalWrite(HEATER_PIN_O, LOW);
    ventServo.write(0);
    ventServo2.write(180);
    heatingMode = HEATING;
  } else if (currentIndoorTemperature > targetTemperature + 2) {
    //temp too high; heater should always be low, open vent if cooler outside
    digitalWrite(HEATER_PIN_O, HIGH);
    ventServo.write(32);
    ventServo2.write(135);
    heatingMode = COOLING;
  } else {
    //if we are in the target range for temperature, keep heater off and vents closed
    digitalWrite(HEATER_PIN_O, HIGH);
    ventServo.write(0);
    ventServo2.write(180);
    heatingMode = PRESERVE_TEMP;
  }

  if (debugging) {
    if (heatingMode == HEATING) {
      Serial.println("Heating on");
    } else if (heatingMode == PRESERVE_TEMP) {
      Serial.println("Preserving Temp");
    } else {
      Serial.println("Heating off");
    }
  }
  

  lastTempCheckTime = millis();

  if (currentMode == MANUAL_MODE) {
    if (debugging) {
      Serial.print("DETECTED MANUAL MODE, WATERING");
    }
    int wateringSeconds = manualModeAmount/FLOW_RATE;
    digitalWrite(PUMP1_PIN_O, LOW);
    delay(wateringSeconds * 1000);
    digitalWrite(PUMP1_PIN_O, HIGH);
    currentMode = IDLE_MODE;
    if (thisManualModeCommandId != -1) {
      String path = "/api/commands/ack/" + String(thisManualModeCommandId);
      postRequest(path, "");
    }
  } else if (currentMode == PRESET_MODE) {
    if (debugging) {
      Serial.println("In preset_mode logic");
    }
    
    //PRESET MODE
    currentTime = millis();
    if (currentTime - presetModePreviousWateringTime >= presetModeFrequency || isFirstAutoModeWater == 1) {
      if (debugging) {
        Serial.print("PRESET MODE, WATERING");
      }
      
      int wateringSeconds = presetModeAmount/FLOW_RATE;
      digitalWrite(PUMP1_PIN_O, LOW);
      delay(wateringSeconds * 1000);
      digitalWrite(PUMP1_PIN_O, HIGH);
      presetModePreviousWateringTime = millis();
      if (thisPresetModeCommandId != -1 && isFirstAutoModeWater == 1) {
        String path = "/api/commands/ack/" + String(thisPresetModeCommandId);
        postRequest(path, "");
      }
      isFirstAutoModeWater = 0;
    }
  } else if (currentMode == LEARNING_MODE) {
    if (thisLearningModeCommandId != -1) {
      String path = "/api/commands/ack/" + String(thisLearningModeCommandId);
      postRequest(path, "");
    }
  }

  if(!client.connected()) {
    Serial.println("Client disconnected, reconnecting");
    if (client.connect(server, 80)) {
      Serial.println("connected to server again");
    } else {
      Serial.println("Failed to reconnect to server");
    }
  }

  if (debugging) {
    Serial.print("SENDING SENSOR DATA");
  }
  
  int capacitance = analogRead(CAP_PIN_I);
  int percentage = map(capacitance, wet, dry, 100, 0);

  if (debugging) {
    Serial.println("Capacitance:");
    Serial.println(capacitance);
  }

  //Check indoor CAP_PIN_I
  if (dhtIndoor.getData()) {                         
    currentIndoorTemperature = dhtIndoor.getTemperature();     
    currentIndoorHumidity = dhtIndoor.getHumidity();

    if (debugging) {
      Serial.println("Indoor temperature:");
      Serial.println(currentIndoorTemperature);
      Serial.println("Indoor humidity: ");
      Serial.println(currentIndoorHumidity);
    }
  }

  String sensorPath = "/api/sensor-data/2";
  String statePath = "/api/device-status/2";

  // Create a JSON object with sensor data
  DynamicJsonDocument sensorDataDoc(200);

  sensorDataDoc["temperature"] = currentIndoorTemperature; 
  sensorDataDoc["humidity"] = currentIndoorHumidity;
  sensorDataDoc["soil_moisture_level"] = percentage;
  sensorDataDoc["water_level"] = 100;

  DynamicJsonDocument stateDataDoc(200);

  if(currentMode == PRESET_MODE) {
    String wateringSchedule = "{ interval: " + String(presetModeFrequency) + ", amount: " + String(presetModeAmount) + "}";

    stateDataDoc["watering_schedule"] = wateringSchedule;
    stateDataDoc["watering_mode"] = "Automatic";
  } else {
    stateDataDoc["watering_schedule"] = "{}";
    if(currentMode == MANUAL_MODE) {
      stateDataDoc["watering_mode"] = "Manual";
    } else if (currentMode == LEARNING_MODE) {
      stateDataDoc["watering_mode"] = "Learning";
    } else {
      stateDataDoc["watering_mode"] = "Idle";
    }
  }

  stateDataDoc["target_temperature"] = targetTemperature;

  if(heatingMode == HEATING) {
    stateDataDoc["heating_mode"] = "heating";
    stateDataDoc["heater_status"] = "on";
    stateDataDoc["vent_status"] = "closed";
  } else if (heatingMode == COOLING) {
    stateDataDoc["heating_mode"] = "cooling";
    stateDataDoc["heater_status"] = "off";
    stateDataDoc["vent_status"] = "open";
  } else {
    stateDataDoc["heating_mode"] = "maintaining temperatures";
    stateDataDoc["heater_status"] = "off";
    stateDataDoc["vent_status"] = "closed";
  }
  
  String jsonDataSensors;
  String jsonDataState;

  serializeJson(sensorDataDoc, jsonDataSensors);
  serializeJson(stateDataDoc, jsonDataState);

  int currentPostTime = millis();
  if (currentPostTime - lastSensorPostTime >= sensorPostInterval || firstPost == 1) {
    postRequest(sensorPath, jsonDataSensors);
    postRequest(statePath, jsonDataState);
    firstPost = 0;
    lastSensorPostTime = millis();
  }
}

/* -------------------------------------------------------------------------- */
void printWifiStatus() {
/* -------------------------------------------------------------------------- */  
  // print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your board's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print the received signal strength:
  long rssi = WiFi.RSSI();
  Serial.print("signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
}

void getRequest(String path) {
  String firstLine = "GET " + path + " HTTP/1.1";

  client.println(firstLine);
  client.println("Host: autogreen-capstone.ca");
  client.println("Accept: application/json");
  client.println("Connection: keep-alive");
  client.println();
}

String read_response_json() {
  String response = "";
  if (debugging) {
    Serial.println("Entered read response function");
  }
  
  // Read the entire response
  const unsigned long timeout = 2000;  // 5 seconds timeout
  unsigned long startTime = millis();

  // Read the response
  // while ((millis() - startTime) < timeout) {
  //   while (client.available()) {
  //     char c = client.read();
  //     response += c;
  //     startTime = millis();  // Reset the timeout counter when data is received
  //   }
  //   delay(10);  // Short delay to allow data to gather
  // }
  while (client.available() == 0) {}

  while (client.available()) {
      char c = client.read();
      response += c;
      //startTime = millis();  // Reset the timeout counter when data is received
  }
  
  // Print the entire response
  //Serial.println(response);

  // Find the start of the JSON body
  int jsonStartIndex = response.indexOf("\r\n\r\n");
  if (jsonStartIndex == -1) {
    Serial.println("Invalid response, JSON not found");
    return "";
  }

  // Extract the JSON body
  String jsonBody = response.substring(jsonStartIndex + 4);
  // Serial.println("JSON Body:");
  // Serial.println(jsonBody);

  return jsonBody;
}

void postRequest(String path, String jsonData) {
  String firstLine = "POST " + path + " HTTP/1.1";

  if (debugging) {
    Serial.println("Post request being made:");
    Serial.println(firstLine);
  }

  int dataLength = jsonData.length();

  client.println(firstLine);
  client.println("Host: autogreen-capstone.ca");
  client.println("Content-Type: application/json");
  client.println("User-Agent: Arduino/1.0");
  client.println("Connection: keep-alive");
  client.print("Content-Length: ");
  client.println(dataLength);
  client.println(); 

  client.println(jsonData);

  String response = "";
  const unsigned long timeout = 2000;  // 5 seconds timeout
  unsigned long startTime = millis();

  // while ((millis() - startTime) < timeout) {
  //   while (client.available()) {
  //     char c = client.read();
  //     response += c;
  //     startTime = millis();
  //   }
  //   delay(10);
  // }

  while (client.available() == 0) {}

  while (client.available()) {
      char c = client.read();
      response += c;
      startTime = millis();
  }

  if (debugging) {
    Serial.println("Response:");
    Serial.println(response);
  }
}
