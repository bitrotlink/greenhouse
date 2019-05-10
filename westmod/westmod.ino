// Adapted from:
// https://github.com/knolleary/pubsubclient/commits/master/examples/mqtt_esp8266/mqtt_esp8266.ino
// https://github.com/PaulStoffregen/OneWire/blob/master/examples/DS18x20_Temperature/DS18x20_Temperature.pde

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>

#define DEBUG 0

#define HEARTBEAT_PERIOD 5000 // ms
// Dx here are NodeMCU pin numbers
#define ONEWIRE_DQ D3
#define PUMP_STATE D2
#define VENT_STATE D6
#define VENT_CTRL D7

#define BUF_SIZE 64
#define MAX_SENSORS 32
#define ONLINE_SENTINEL 0xff // User byte values to indicate that a sensor has not been power-reset since the last conversion, so if the temperature reading is known to be valid even if it's 85°C (which is the power-on default value).
#define RESET_SENTINEL 0 // User byte values to indicate both that a sensor has been power-reset and ONLINE_SENTINEL hasn't been set yet, and that the device is a previously-known one. New devices (never seen before) typically have neither RESET_SENTINEL nor ONLINE_SENTINEL stored in EEPROM for the user byte values.

const char* ssid = "ghpi";
const char* password = "PKFVH5munb";
// const char* mqtt_server = "foo.local";
IPAddress mqtt_server(192,168,42,1);
//const char* mqtt_server_fqdn = "foo.local";
// const int server_port = 8883;
const int server_port = 1883;
const char* fingerprint = "xx:xx..."; //foo

// WiFiClientSecure espClient;
WiFiClient espClient;
PubSubClient client(espClient);
long last_hb_time = 0;
char topic_buf[BUF_SIZE];
char msg_buf[BUF_SIZE];
unsigned long heartbeat = 0;
int vent_state = -1;
int pump_state = -1;
int conv_initiated = 0;
OneWire w1bus(ONEWIRE_DQ);

typedef struct {
  byte addr[8];
  byte present;
  byte heartbeat;
  int centi_celsius;
} Sensor;

byte zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
byte sensors_list_full_p = 0;
Sensor sensors[MAX_SENSORS];

// Find sensor in list, or add to list if not already in it
Sensor* assoc_add_sensor(byte* addr) {
  int i;
  for(i=0; i<MAX_SENSORS; i++) {
    if(memcmp(addr, sensors[i].addr, 8)==0) return &(sensors[i]);
    if(memcmp(zeros, sensors[i].addr, 8)==0) break; // Empty slot
  }
  if(i==MAX_SENSORS) return NULL; // All slots full
  memcpy(sensors[i].addr, addr, 8); // Add to list
  return &(sensors[i]);
}

void flash_LED(int count, int period) {
  for(int c=0; c<count; c++) {
    digitalWrite(BUILTIN_LED, HIGH);
    delay(period/2);
    digitalWrite(BUILTIN_LED, LOW);
    delay(period/2);
  }
  digitalWrite(BUILTIN_LED, HIGH);
}

void reboot() { // Trigger watchdog timeout
  flash_LED(10, 150);
  unsigned long c=0;
  while(1) c++;
}

void setup_wifi() {
  delay(10);
  if(DEBUG) {
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    flash_LED(2, 150);
    if(DEBUG) Serial.print(".");
  }
  if(DEBUG) {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  }
}

// Set both user bytes in the scratchpad of all devices
void set_scratchpad(byte value) {
  if(!(w1bus.reset())) return;
  w1bus.skip(); // Select all devices
  w1bus.write(0x4E); // Write scratchpad
  w1bus.write(value); // User byte #1 (aka T_H)
  w1bus.write(value); // User byte #2 (aka T_L)
  w1bus.write(0x7F); // Config (12-bit resolution, t_conv = 750ms)
}

// Set both user bytes in EEPROM of all devices to zero, so that at device startup, they'll be zero in the scratchpad (automatically copied from EEPROM).
void set_eeprom() {
  set_scratchpad(RESET_SENTINEL);
  if(!(w1bus.reset())) return;
  w1bus.skip();
  w1bus.write(0x48); // Copy to EEPROM
}

void callback(char* topic, byte* payload, unsigned int length) {
  if(DEBUG) {
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    for (int i = 0; i < length; i++) {
      Serial.print((char)payload[i]);
    }
    Serial.println();
  }
  switch((char)payload[0]) {
  case 'L': digitalWrite(BUILTIN_LED, LOW); break; // LED on
  case 'l': digitalWrite(BUILTIN_LED, HIGH); break;
  case 'V': digitalWrite(VENT_CTRL, HIGH); break; // Vent open
  case 'v': digitalWrite(VENT_CTRL, LOW); break;
  case 'R': reboot();
  }
}

void reconnect() {
  int c=0;
  while (!client.connected()) {
    if(DEBUG) Serial.print("Attempting MQTT connection...");
    if (client.connect("westmod", "", "", "hb/westmod", 2, 0, "-1", 1)) {
      if(DEBUG) Serial.println("connected");
      /* if (espClient.verify(fingerprint, mqtt_server_fqdn)) { */
      /*   Serial.println("certificate matches"); */
      /* } else { */
      /*   Serial.println("certificate doesn't match"); */
      /* } */
      client.publish("hb/westmod", "0");
      heartbeat++;
      client.subscribe("act/westmod", 1);
    } else {
      if(DEBUG) {
	Serial.print("failed, rc=");
	Serial.print(client.state());
	Serial.println(" try again in 5 seconds");
      }
      delay(5000);
    }
    c++;
    if(c>3) reboot();
  }
}

void setup() {
  memset(&sensors, 0, sizeof(Sensor)*MAX_SENSORS);
  pinMode(BUILTIN_LED, OUTPUT);
  pinMode(VENT_CTRL, OUTPUT);
  pinMode(VENT_STATE, INPUT_PULLUP);
  pinMode(PUMP_STATE, INPUT);
  flash_LED(2, 400);
  if(DEBUG) Serial.begin(115200);
  setup_wifi();
  client.setServer(mqtt_server, server_port);
  client.setCallback(callback);
}

void trigger_conversion() {
  if(!(w1bus.reset())) {
    // client.publish("sensors/w1/errors", "no devices present");
    if(DEBUG) Serial.println("no devices present");
    return;
  }
  w1bus.skip();
  w1bus.write(0x44);
  conv_initiated = 1;
}

const char base_topic_template[] = "sensors/%02x-%02x%02x%02x%02x%02x%02x";
const char data_topic_template[] = "sensors/%02x-%02x%02x%02x%02x%02x%02x/data";
const char error_topic_template[] = "sensors/%02x-%02x%02x%02x%02x%02x%02x/errors";

void make_topic(const char* topic_template, const byte* addr) {
  // Match the byte ordering used by the Linux /sys/bus/w1
  snprintf (topic_buf, BUF_SIZE,
	    topic_template,
	    addr[0],addr[6],addr[5],addr[4],addr[3],addr[2],addr[1]);
}

void report_sensor_error(byte* addr, const char* err) {
  make_topic(error_topic_template, addr);
  client.publish(topic_buf, err);
  if(DEBUG) Serial.println(err);
}

void read_onewire_all() {
  byte i;
  byte present = 0;
  byte data[12];
  byte addr[8];
  for(int i=0; i<MAX_SENSORS; i++)
    sensors[i].heartbeat = 0; // HB set when sensor detected in loop below
  while(1) {
    client.loop();
    if (!w1bus.search(addr)) { // Finished reading all sensors
      for(int i=0; i<MAX_SENSORS; i++) {
	if((sensors[i].present) && !(sensors[i].heartbeat)) {
	  sensors[i].present = 0;
	  report_sensor_error(sensors[i].addr, "disappeared");
	  make_topic(data_topic_template, sensors[i].addr);
	  client.publish(topic_buf, "offline", 1);
	}
      }
      if(DEBUG) {
	Serial.println("No more addresses.");
	Serial.println();
      }
      return;
    }
    if(DEBUG) {
      Serial.print("ROM =");
      for( i = 0; i < 8; i++) {
	Serial.write(' ');
	Serial.print(addr[i], HEX);
      }
    }
    if (OneWire::crc8(addr, 7) != addr[7]) {
      client.publish("sensors/w1/errors", "addr CRC");
      if(DEBUG) Serial.println("Invalid addr CRC");
      continue;
    }
    present = w1bus.reset();
    if(!present) {
      report_sensor_error(addr, "phantom appearance");
      continue;
    }
    if(addr[0]!=0x28) continue; // Ignore all devices other than DS18B20
    w1bus.select(addr);
    w1bus.write(0xBE); // Read Scratchpad
    if(DEBUG) {
      Serial.print(" Data = ");
      Serial.print(present, HEX);
      Serial.print(" ");
    }
    for ( i = 0; i < 9; i++) { // read 8 data bytes plus 1 CRC byte
      data[i] = w1bus.read();
      if(DEBUG) {
	Serial.print(data[i], HEX);
	Serial.print(" ");
      }
    }
    byte CRC = OneWire::crc8(data, 8);
    if(data[8]!=CRC) {
      report_sensor_error(addr, "Invalid data CRC");
      continue;
    }
    if(DEBUG) {
      Serial.print(" CRC=");
      Serial.print(CRC, HEX);
      Serial.println();
    }

    Sensor* sensor = assoc_add_sensor(addr);
    if(sensor==NULL) {
      if(sensors_list_full_p == 0) {
	sensors_list_full_p = 1;
	client.publish("sensors/w1/errors", "list full");
      }
      continue;
    }
    sensor->heartbeat = 1;
    if((data[2]!=ONLINE_SENTINEL)||(data[3]!=ONLINE_SENTINEL)) { // Newly attached
      sensor->present = 1;
      make_topic(base_topic_template, addr);
      if((data[2]!=RESET_SENTINEL)||(data[3]!=RESET_SENTINEL)) { // New device
	set_eeprom();
	client.publish(topic_buf, "new");
      } else client.publish(topic_buf, "attached");
      set_scratchpad(ONLINE_SENTINEL);
      continue; // Wait until next conversion cycle before reporting a reading, to avoid race condition of reading newly-attached device after the most recent conversion command was issued, since that results in the default value of 85°C, which is indistinguishable from an actual 85°C.
    }
    int prev_present = sensor->present;
    if(!(sensor->present)) { // Reappeared without being newly attached; could indicate flaky DQ connection. Also happens when controller module is rebooted without power-cycling the onewire bus.
      sensor->present = 1;
      make_topic(base_topic_template, addr);
      client.publish(topic_buf, "reappeared");
    }

    // Convert the data to actual temperature
    // because the result is a 16 bit signed integer, it should
    // be stored to an "int16_t" type, which is always 16 bits
    // even when compiled on a 32 bit processor.
    int16_t raw = (data[1] << 8) | data[0];
    byte cfg = (data[4] & 0x60);
    // at lower res, the low bits are undefined, so let's zero them
    if (cfg == 0x00) raw = raw & ~7; // 9 bit resolution, 93.75 ms
    else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
    else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
    //// default is 12 bit resolution, 750 ms conversion time

    // float celsius = (float)raw / 16.0;
    int centi_celsius = ((int32_t)raw) * 25 / 4; // Ok to store result in int, since largest possible value is 12500, and smallest is -5500.

    /* snprintf (msg_buf, BUF_SIZE, */
    /*	      "%02x%02x%02x%02x%02x%02x%02x%02x", */
    /*	      data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7]); */
    if(!(prev_present) || (centi_celsius != sensor->centi_celsius)) {
      sensor->centi_celsius = centi_celsius;
      snprintf(msg_buf, BUF_SIZE, "%d", centi_celsius);
      make_topic(data_topic_template, addr);
      client.publish(topic_buf, msg_buf, 1);
      if(DEBUG) {
	Serial.print("Publishing temp: ");
	Serial.print(topic_buf);
	Serial.print(" ");
	Serial.print(msg_buf);
	Serial.println();
      }
    }
  }
}

void loop() {
  if(WiFi.status() != WL_CONNECTED) reboot();
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  long now = millis();
  if ((now - last_hb_time > HEARTBEAT_PERIOD) || (now < last_hb_time)) {
    last_hb_time = now;
    snprintf (msg_buf, BUF_SIZE, "%lu", heartbeat);
    heartbeat++;
    client.publish("hb/westmod", msg_buf);
    if(DEBUG) {
      Serial.print("Publish message: ");
      Serial.println(msg_buf);
    }
  }
  int vent_state_new = digitalRead(VENT_STATE);
  int pump_state_new = digitalRead(PUMP_STATE);
  if(vent_state_new != vent_state) {
    vent_state = vent_state_new;
    snprintf (msg_buf, BUF_SIZE, "%d", vent_state);
    client.publish("sensors/vent", msg_buf, 1);
  }
  if(pump_state_new != pump_state) {
    pump_state = pump_state_new;
    snprintf (msg_buf, BUF_SIZE, "%d", pump_state);
    client.publish("sensors/swamp_pump", msg_buf, 1);
  }
  byte sensors_status = w1bus.read();
  if((sensors_status != (byte)0xff)) return; // At least one device is busy with conversion
  if(conv_initiated) {
    conv_initiated = 0;
    read_onewire_all();
    w1bus.reset_search();
  }
  trigger_conversion();
}
