/*
   Written by W. Hoogervorst
   Fire alarm detector
   Version 7, HTTPupdateserver added
*/

#include <ESP8266WiFi.h>
#include <TimeLib.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal

#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>

#include <PubSubClient.h>
#include <credentials.h>          //mySSID, myPASSWORD, IFTTT_KEY
#include <IFTTTMaker.h>
#include <WiFiClientSecure.h>

// for HTTPupdate
const char* host = "fire-alarm";
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

/*credentials & definitions */
//MQTTT
const char* mqtt_server = "192.168.10.104";
const char* mqtt_willTopic = "sensor/smokedetector/state";
const char* mqtt_id = "Firedetection-Client";

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

String tmp_str; // String for publishing the int's as a string to MQTT
char buf[5];

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
    program_mode = OTAFLASH_MODE;
    for (int i = 0 ; i < 5; i++)
    {
      digitalWrite(REDLEDPIN, HIGH);    // blink green led on
      delay(100);
      digitalWrite(REDLEDPIN, LOW);    // blink green led off
      delay(100);
    }
  }
  else
  {
    for (int i = 0 ; i < 5; i++)
    {
      digitalWrite(GREENLEDPIN, HIGH);    // blink green led on
      delay(100);
      digitalWrite(GREENLEDPIN, LOW);    // blink green led off
      delay(100);
    }
  }
  setup_wifi();     //connect tot wifi

  // for HTTPudate
  MDNS.begin(host);
  httpUpdater.setup(&httpServer);
  httpServer.begin();

  MDNS.addService("http", "tcp", 80);
  client.setServer(mqtt_server, 1883);
  //client.setCallback(callback);     
  if (!client.connected()) {
    digitalWrite(GREENLEDPIN, LOW);
    reconnect();
  }
  if (client.connected())
    digitalWrite(GREENLEDPIN, HIGH);
  client.publish("sensor/smokedetector/debug", "Started");
  statustime = now();

  // switch in setup
  switch (program_mode)
  {
    case NORMAL_MODE:
      {
        if (digitalRead(FIREDETECTPIN) == HIGH)    // no fire detected
        {
          client.publish("sensor/smokedetector/alarm", "NOFIRE");
          client.publish("sensor/smokedetector/state", "Status OK");
        }
        break;
      }
    case OTAFLASH_MODE:
      {
        client.publish("sensor/smokedetector/state", "OTA mode");
        ArduinoOTA.begin();
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
          if (time1 > lastReconnectAttempt + MQTT_CONNECT_DELAY)
          {
            lastReconnectAttempt = time1;
            // Attempt to reconnect
            if (reconnect()) {
              lastReconnectAttempt = 0;
              digitalWrite(GREENLEDPIN, HIGH);
              client.publish("sensor/smokedetector/state", "reconnected");
              client.publish("sensor/smokedetector/alarm", "NOFIRE");
            }
          }
        }

        /* Client connected
            main program here
        */
        client.loop();
        httpServer.handleClient();    // for HTTPupdate
        
        //check whether Fire is detected
        if (digitalRead(FIREDETECTPIN) == LOW && firedetected == false)    // fire detected and previously not
        {
          time2 = millis();
          client.publish("sensor/smokedetector/debug", "fire! debounce");
          delay(10);
          yield();
          while (millis() < time2 + DEBOUNCE)
          {
            if (digitalRead(FIREDETECTPIN) == LOW)
              firedetected = true;
            else
            {
              firedetected = false;

              client.publish("sensor/smokedetector/debug", "alarm reset");
              break;
            }
          }
          client.publish("sensor/smokedetector/debug", "debounce ended");
          if (firedetected)
          {
            digitalWrite(REDLEDPIN, HIGH);
            client.publish("sensor/smokedetector/alarm", "FIRE");
            client.publish("sensor/smokedetector/state", "fire!");
            statustime = now();

            //IFTTT actions
            if (ifttt.triggerEvent(EVENT_NAME1, mySSID, ip.toString()))
              client.publish("sensor/smokedetector/debug", "IFTTT 1 sent ok");
            if (ifttt.triggerEvent(EVENT_NAME2, mySSID, ip.toString()))
              client.publish("sensor/smokedetector/debug", "IFTTT 2 sent ok");
          }
        }
        if (digitalRead(FIREDETECTPIN) == HIGH && firedetected == true)    // fire previously detected and not any more
        {
          firedetected = false;
          client.publish("sensor/smokedetector/alarm", "NOFIRE");
          client.publish("sensor/smokedetector/debug", "no more fire");
          client.publish("sensor/smokedetector/state", "Status OK");
          digitalWrite(REDLEDPIN, LOW);
          statustime = now();
        }

        // publish status value
        if (now() > statustime + OKSENDDELAY)
        {
          client.publish("sensor/smokedetector/state", "Status OK");
          client.publish("sensor/smokedetector/alarm", "NOFIRE");
          statustime = now();
        }

        //publish status when fire is detected
        if (now() > statustime + FIRESENDDELAY && firedetected == true)
        {
          client.publish("sensor/smokedetector/alarm", "FIRE");
          client.publish("sensor/smokedetector/state", "fire!");
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
  /*
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(mySSID);
  */
  WiFi.mode(WIFI_STA);
  WiFi.begin(mySSID, myPASSWORD);
  time1 = now();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    // Serial.print(".");
    if (now() > time1 + WIFI_CONNECT_TIMEOUT)
      ESP.restart();
  }
  /*
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
    ip = WiFi.localIP();
  */
}

/*
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
  //Serial.println("Attempting MQTT connection...");
  if (client.connect(mqtt_id, mqtt_willTopic, 0, 0, "ERROR")) {    // connect(const char *id, const char* willTopic, uint8_t willQos, boolean willRetain, const char* willMessage)
    //Serial.println("connected");
    // ... and resubscribe
  }
  //Serial.println(client.connected());
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
