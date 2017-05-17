// *******************************************************************************************
//
// THIS PROJECT IS IN A VERY EARLY PROOF OF CONCEPT STATE!!
// STABILITY NEEDS TO BE IMPROVED!!
// CODE NEEDS CLEAN UP ASWELL :)
//
// In Arduino IDE, set CPU clock to be 160 MHz!
//
// The code provides a webserver listening on port 5522 for an 8-bit mono PCM data stream.
// On pin IO3 (RXD) a pseudo 7-bit PWM is ouput.
// This pin is setup as I2S interface and normally provides a 32 bit raw audio data stream.
// However in this usecase we output 32 bits x 4 = 128 bit of data to form a 7-bit pseudo PWM.
//
// Comments and improvements welcome:
// huawatuam@gmail.com
//
// To provide the 8 bit PCM mono stream expected form the host, we need to convertthe e.g. mp3-file using "avconv" or similar.
//
// EXAMPLE: 
// ========
// avconv -i gong.mp3 -f s32be -acodec pcm_u8 -ac 1 -ar 33000 tcp://192.168.1.100:5522
//
// When you want to use it with TTS in FHEM, you can e.g. use a DOIF to forward the "lastFilename" reading in TTS,
// which contains the link to the last played mp3 file, to the ESP8266 wifi speaker.
// To call avconv in FHEM, i use "system('avconv....$filename...');"
//
// V1 (14.05.2017)
// - initial version
//
// V2 (16.05.2017)
// - added option to set static IP, netmask and gateway
// - 160MHz cpu speed is automatically
// - fixed 0% PWM bug
// - increased buffer size
// - added "setNoDelay" to server & client
// - added "flush" when disconnected
//
// V3 (17.05.2017)
// - playback code removed from main loop
// - webserver connection handling reworked
// - stability improved by removing "delay(1)"
//
// TODO:
// - check if dithered PWM is possible and enhances sound quality
// - handle case where stream size is smaller than buffer size

#include <ESP8266WiFi.h>
#include "i2s.h"

extern "C" {
  #include "user_interface.h"
}

const char* ssid = "yourSSID";
const char* password = "yourPASSWORD";

const uint8_t staticIP[] = {0, 0, 0, 0};   // set to 0, 0, 0, 0 for DHCP
const uint8_t gwIP[] = {192, 168, 1, 1};
const uint8_t subnet[] = {255, 255, 255, 0};
const uint16_t port = 5522;

WiFiServer server(port);

// valid buffer sizes are e.g. 0x1000, 0x2000, 0x4000
#define BUFFER_SIZE   0x4000
#define BLUE_LED      2

uint8_t buffer8b[BUFFER_SIZE];
uint16_t bufferPtrIn;
uint16_t bufferPtrOut;
unsigned long ultimeout;
uint32_t byteCnt = 0;

// **************************************************
// **************************************************
// **************************************************
void setup() 
{  
  system_update_cpu_freq(SYS_CPU_160MHZ);

  Serial.begin(115200);
  delay(1);
  
  WiFi.mode(WIFI_STA);
  WiFiStart();

  i2s_begin();
  i2s_set_rate(96000);

  pinMode(BLUE_LED, OUTPUT);      // blue LED indicates "client connected"
  digitalWrite(BLUE_LED, HIGH);
}

// **************************************************
// **************************************************
// **************************************************
void WiFiStart()
{
  // connect to wifi
  Serial.print("\nConnecting to ");
  Serial.print(ssid);

  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }
  
  Serial.print("WiFi connected\n");  

  // use static IP if specified - otherwise use DHCP
  if (*staticIP != '\0') 
  {
    IPAddress ip(staticIP[0], staticIP[1], staticIP[2], staticIP[3]);
    IPAddress gateway(gwIP[0], gwIP[1], gwIP[2], gwIP[3]);
    IPAddress subnet(subnet[0], subnet[1], subnet[2], subnet[3]);
    WiFi.config(ip, gateway, subnet);
  }

  // start server
  server.begin();
  
  Serial.print("Server running at ");
  Serial.print(WiFi.localIP());
  Serial.print(":");
  Serial.print(port);
  
  Serial.print("\nGateway set to ");
  Serial.print(WiFi.gatewayIP());
}

// **************************************************
// **************************************************
// **************************************************
// ctrl = 0 : reset the PWM
// ctrl = 1 : normal playback
inline void doPWM(uint8_t ctrl, uint8_t value8b)
{
  static uint8_t sample = 0;
  static uint8_t dWordNr = 0;
  static uint32_t i2sbuf32b[4];
  static uint16_t dl = 0;
  uint8_t shift;

  // reset
  if (ctrl == 0)
  {
    dl = 0;
    dWordNr = 0;
    sample = 0;
    i2sbuf32b[0] = 0;
    i2sbuf32b[1] = 0;
    i2sbuf32b[2] = 0;
    i2sbuf32b[3] = 0;
    return;
  }
  
  // playback
  if (i2s_write_sample_nb(i2sbuf32b[dWordNr]))
  {
    dWordNr = ++dWordNr & 0x03;

    // previous 4 DWORDS written?
    if (dWordNr == 0x00)
    {
      yield();
      
      // normal playback
      if (ctrl == 1)
      {
        if (bufferPtrOut != bufferPtrIn)
        {
          bufferPtrOut = (bufferPtrOut + 1) & (BUFFER_SIZE - 1);
          sample = buffer8b[bufferPtrOut];
        } else {
          // NOTE: To avoid blobs at EOF (buffer underrun), fade out PWM to 0% (0V) starting at last valid PWM value
          if (dl++ > 10)
          {
            dl = 0;
            if (buffer8b[bufferPtrOut] > 0)
            {
              --buffer8b[bufferPtrOut];
              sample = buffer8b[bufferPtrOut];
            }
          }
        }
      } else {
        // direct output mode
        sample = value8b;
      }

      shift = (sample >> 1) ^ 0x7F;

      if (shift < 0x20)
      {
        i2sbuf32b[0] = 0xFFFFFFFF >> shift;
        i2sbuf32b[1] = 0xFFFFFFFF;
        i2sbuf32b[2] = 0xFFFFFFFF;
        i2sbuf32b[3] = 0xFFFFFFFF;
      } else if (shift < 0x40)
      {
        i2sbuf32b[0] = 0x00000000;
        i2sbuf32b[1] = 0xFFFFFFFF >> (shift - 0x20);
        i2sbuf32b[2] = 0xFFFFFFFF;
        i2sbuf32b[3] = 0xFFFFFFFF;
      } else if (shift < 0x60)
      {
        i2sbuf32b[0] = 0x00000000;
        i2sbuf32b[1] = 0x00000000;
        i2sbuf32b[2] = 0xFFFFFFFF >> (shift - 0x40);
        i2sbuf32b[3] = 0xFFFFFFFF;
      } else
      {
        i2sbuf32b[0] = 0x00000000;
        i2sbuf32b[1] = 0x00000000;
        i2sbuf32b[2] = 0x00000000;
        i2sbuf32b[3] = 0xFFFFFFFF >> (shift - 0x60);
      }     
    }
  }
}

// **************************************************
// **************************************************
// **************************************************
static inline void doStreaming(WiFiClient *client)
{
  uint8_t i;
  uint8_t dl;
  
  byteCnt = 0;
  byteCnt = 0;
  bufferPtrIn = 0;
  bufferPtrOut = 0;

  // ===================================================================================
  // fill buffer
  
  do 
  {
    if (client->available())
    {
      buffer8b[bufferPtrIn] = client->read();
      bufferPtrIn = ++bufferPtrIn & (BUFFER_SIZE - 1);
      byteCnt++;
    }
  } while (bufferPtrIn < (BUFFER_SIZE - 1));
  
  // ===================================================================================
  // start playback
  
  // reset PWM
  doPWM(0,0);
      
  // ramp up PWM to 50% to avoid "blops"  
  i = 0;
  dl = 0;
  
  do
  {
    doPWM(2, i);
    if (dl++ > 200)
    {
      dl = 0;
      i++;
    }
  } while (i < 128);
  
  do
  {
    doPWM(1,0);
  
    if (client->available() && (((bufferPtrIn + 1) & (BUFFER_SIZE - 1)) != bufferPtrOut))
    {
      buffer8b[bufferPtrIn] = client->read();
      bufferPtrIn = ++bufferPtrIn & (BUFFER_SIZE - 1);        
      byteCnt++;        
    }
    
    if (bufferPtrOut != bufferPtrIn)
      ultimeout = millis() + 1000;

  } while (client->available() || (millis() < ultimeout) || (bufferPtrOut != bufferPtrIn));
}

// **************************************************
// **************************************************
// **************************************************
void loop() 
{  
  // reconnect if wifi got disconnected
  if (WiFi.status() != WL_CONNECTED)
    WiFiStart();

  WiFiClient client = server.available();

  // new client?
  if (client)
  {
    digitalWrite(BLUE_LED, LOW);
    Serial.println("\nnew client");

    if (client.connected())
      doStreaming(&client);

    client.stop();
    digitalWrite(BLUE_LED, HIGH);
    Serial.printf("Client disonnected. %d bytes received\n", byteCnt);    
  }
}

