#include <ESP8266WiFi.h>
#include <TimeLib.h>
//OTA
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal

#include <PubSubClient.h>
#include <credentials.h>          //mySSID, myPASSWORD, IFTTT_KEY

#include <IFTTTMaker.h>
#include <WiFiClientSecure.h>

/*credentials & definitions */
//MQTTT
const char* mqtt_server = "192.168.10.104";
WiFiClient espClient;
PubSubClient client(espClient);

#define SECOND 1       // in s
#define MINUTE 60*SECOND  // in s
#define DEBOUNCE 1000      // in ms

//IFTTT
#define EVENT_NAME1 "fire_alarm_1" // Name of your event name, set when you are creating the applet
#define EVENT_NAME2 "fire_alarm_2" // Name of your event name, set when you are creating the applet
WiFiClientSecure Sclient;
IFTTTMaker ifttt(IFTTT_KEY, Sclient);
IPAddress ip;

// Important, since we use GPIO1 (default TX) and GPIO3 (default RX) of the ESP-01 module, we cannot use or initialize the
// Serial port (TX and RX), because our pinMode declarations of these pins become void.
#define GREENLEDPIN 0 // Green Led on GPIO0
#define FIREDETECTPIN 1 // Smoke detector signal on GPIO1
#define REDLEDPIN 2 // Red Led on GPIO2
#define OTAPIN 3 // Switch for starting OTA flashmode on GPIO3

#define OKSENDDELAY 60*MINUTE     // send status every ##
#define FIRESENDDELAY 5*MINUTE    // send detection status every ##
#define WIFI_CONNECT_TIMEOUT 10*SECOND
#define MQTT_CONNECT_DELAY 10*SECOND

//program modes
#define NORMAL_MODE 1
#define OTAFLASH_MODE 2

// global variables
boolean firedetected = false;
uint32_t time1, time2, lastReconnectAttempt = 0, statustime = 0;

int program_mode = NORMAL_MODE; // this defines the mode of the program (normal or OTAflash)

void setup()
{
  // set pinmodes
  pinMode(GREENLEDPIN, OUTPUT);
  pinMode(REDLEDPIN, OUTPUT);
  pinMode(OTAPIN, INPUT_PULLUP);
  pinMode(FIREDETECTPIN, INPUT_PULLUP);
  digitalWrite(GREENLEDPIN, LOW);
  digitalWrite(REDLEDPIN, LOW);

  // serial is not initialized (see previous comment), and all following calls to serial end dead.

  // set program mode
  if (digitalRead(OTAPIN) == LOW)
  {
    program_mode = OTAFLASH_MODE;       // blink red led
    for (int i = 0 ; i < 5; i++)
    {
      digitalWrite(REDLEDPIN, HIGH);   
      delay(100);
      digitalWrite(REDLEDPIN, LOW);    
      delay(100);
    }
  }
  else
  {
    for (int i = 0 ; i < 5; i++)        // blink green led
    {
      digitalWrite(GREENLEDPIN, HIGH);     
      delay(100);
      digitalWrite(GREENLEDPIN, LOW);    
      delay(100);
    }
  }
  setup_wifi();
  client.setServer(mqtt_server, 1883);
  //client.setCallback(callback);
  if (!client.connected()) {
    digitalWrite(GREENLEDPIN, LOW);
    reconnect();
  }
  if (client.connected())
    digitalWrite(GREENLEDPIN, HIGH);
  client.publish("sensor/smokedetector/state", "Smoke detector Started");
  statustime = now();

  // switch in setup
  switch (program_mode)
  {
    case NORMAL_MODE:
      {
        if (digitalRead(FIREDETECTPIN) == HIGH)    // no fire detected
        {
          client.publish("sensor/smokedetector/alarm", "OFF");
          client.publish("sensor/smokedetector/state", "Status ok, no fire detected");
        }
        break;
      }
    case OTAFLASH_MODE:
      {
        client.publish("sensor/smokedetector/state", "Smoke detector in OTA mode");
        ArduinoOTA.onStart([]() {
          String type;
          if (ArduinoOTA.getCommand() == U_FLASH)
            type = "sketch";
          else // U_SPIFFS
            type = "filesystem";

          // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
          Serial.println("Start updating " + type);
        });
        ArduinoOTA.onEnd([]() {
          Serial.println("\nEnd");
        });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
          Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
        });
        ArduinoOTA.onError([](ota_error_t error) {
          Serial.printf("Error[%u]: ", error);
          if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
          else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
          else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
          else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
          else if (error == OTA_END_ERROR) Serial.println("End Failed");
        });
        ArduinoOTA.begin();
        Serial.print("OTA Ready on IP address: ");
        Serial.println(WiFi.localIP());
        // end OTA routine
        break;
      }
  }
}

void loop()
{
  // switch in loop
  switch (program_mode)
  {
    case NORMAL_MODE:
      {
        if (!client.connected())
        {
          digitalWrite(GREENLEDPIN, LOW);
          time1 = now();
          if (time1 < lastReconnectAttempt + MQTT_CONNECT_DELAY)
          {
            lastReconnectAttempt = time1;
            // Attempt to reconnect
            if (reconnect()) {
              lastReconnectAttempt = 0;
              digitalWrite(GREENLEDPIN, HIGH);
            }
          }
        }

        /* Client connected
            main program here
        */
        client.loop();
        //check whether Fire is detected
        if (digitalRead(FIREDETECTPIN) == LOW && firedetected == false)    // fire detected and previously not
        {
          time2 = millis();
          client.publish("sensor/smokedetector/state", "fire is detected!, wait for debounce");
          delay(10);
          yield();
          while (millis() < time2 + DEBOUNCE)
          {
            if (digitalRead(FIREDETECTPIN) == LOW)
              firedetected = true;
            else
            {
              firedetected = false;

              client.publish("sensor/smokedetector/state", "alarm reset during debounce");
              break;
            }
          }
          client.publish("sensor/smokedetector/state", "debounce ended");
          if (firedetected)
          {
            digitalWrite(REDLEDPIN, HIGH);
            client.publish("sensor/smokedetector/alarm", "ON");
            client.publish("sensor/smokedetector/state", "fire is detected!");
            statustime = now();

            //IFTTT actions
            if (ifttt.triggerEvent(EVENT_NAME1, mySSID, ip.toString()))
              client.publish("sensor/smokedetector/state", "IFTTT 1 sent ok");
            if (ifttt.triggerEvent(EVENT_NAME2, mySSID, ip.toString()))
              client.publish("sensor/smokedetector/state", "IFTTT 2 sent ok");
          }
        }
        if (digitalRead(FIREDETECTPIN) == HIGH && firedetected == true)    // fire previously detected and not any more
        {
          firedetected = false;
          client.publish("sensor/smokedetector/alarm", "OFF");
          client.publish("sensor/smokedetector/state", "no more fire detected");
          digitalWrite(REDLEDPIN, LOW);
          statustime = now();
        }

        // publish status value
        if (now() > statustime + OKSENDDELAY)
        {
          client.publish("sensor/smokedetector/state", "Status OK");
          statustime = now();
        }

        //publish status when fire is detected
        if (now() > statustime + FIRESENDDELAY && firedetected == true)
        {
          client.publish("sensor/smokedetector/alarm", "ON");
          client.publish("sensor/smokedetector/state", "fire is detected!");
          statustime = now();
        }

        break;
      }
    case OTAFLASH_MODE:
      {
        ArduinoOTA.handle();
        break;
      }
  }
}

void setup_wifi()
{
  // We connect to the WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(mySSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(mySSID, myPASSWORD);
  time1 = now();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (now() > time1 + WIFI_CONNECT_TIMEOUT)
      ESP.restart();
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  //  ip = WiFi.localIP();
}

/*
  not subscibed to any topic, no callback function needed
  void callback(char* topic, byte * payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
  }
*/
boolean reconnect()
{
  if (WiFi.status() != WL_CONNECTED) {    // check if WiFi connection is present
    setup_wifi();
  }
  Serial.println("Attempting MQTT connection...");
  if (client.connect("Firedetection-Client")) {
    Serial.println("connected");
    // ... and resubscribe
  }
  Serial.println(client.connected());
  return client.connected();
}

void debounce(uint16_t debouncetimems)
{
  time1 = millis();
  while (millis() < (time1 + debouncetimems))
  {
    yield();
  }
}
