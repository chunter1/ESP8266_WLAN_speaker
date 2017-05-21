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
// From version V4 on, pwm dithering is added and enabled by default.
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
// V4 (21.05.2017)
// - pwm dithering added and enabled by default
// - status LED on gpio12 added (fast blinking when wifi is disconnected; short flash every 2 seconds when connected to AP; slow blining during playback)
// - fixed crash playing streams shorten than rx-buffer and disconnects during buffering

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

// valid buffer Sizes are e.g. 0x1000, 0x2000, 0x4000
#define BUFFER_SIZE   0x4000
#define ONBOARD_LED   2
#define STATUS_LED    12

uint8_t buffer8b[BUFFER_SIZE];
uint16_t bufferPtrIn;
uint16_t bufferPtrOut;
uint32_t ultimeout;
uint32_t byteCnt = 0;
uint8_t toggleOnOff = 1;

volatile enum STATUS_LED_MODES { DISCONNECTED, CONNECTED, STREAMING } statusLEDmode = DISCONNECTED;
uint32_t statusLED_on_ticks_mode[] = {160000 * 100, 160000 * 20, 160000 * 500};
uint32_t statusLED_off_ticks_mode[] = {160000 * 100, 160000 * 2000, 160000 * 500};

#define USE_DITHERING

// **************************************************
// **************************************************
// **************************************************
void ICACHE_RAM_ATTR statusLED_ISR()
{
  if (toggleOnOff)
  {
    timer0_write(ESP.getCycleCount() + statusLED_on_ticks_mode[statusLEDmode]);
    digitalWrite(STATUS_LED, LOW);    
  } else {
    timer0_write(ESP.getCycleCount() + statusLED_off_ticks_mode[statusLEDmode]);
    digitalWrite(STATUS_LED, HIGH);
  }

  toggleOnOff ^= 0x01;
}

// **************************************************
// **************************************************
// **************************************************
void setStatusLEDmode(STATUS_LED_MODES new_statusLEDmode)
{
  if (statusLEDmode != new_statusLEDmode)
  {
    toggleOnOff = 1;
    statusLEDmode = new_statusLEDmode;

    // update timer immediately
    timer0_write(ESP.getCycleCount() + 160000);
  }
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
  setStatusLEDmode(CONNECTED);

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
void setup() 
{  
  system_update_cpu_freq(SYS_CPU_160MHZ);

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH);

  Serial.begin(115200);
  delay(1);

  timer0_isr_init();
  timer0_attachInterrupt(statusLED_ISR);
  timer0_write(ESP.getCycleCount() + 160000000);  
  interrupts();

  WiFi.mode(WIFI_STA);
  WiFiStart();

  i2s_begin();
  i2s_set_rate(96000);      // 33 ksps

  wifi_status_led_uninstall();
  
  pinMode(ONBOARD_LED, OUTPUT);
  digitalWrite(ONBOARD_LED, HIGH);
}

// **************************************************
// **************************************************
// **************************************************
// ctrl = 0 : reset the PWM
// ctrl = 1 : normal playback
inline void doPWM(uint8_t ctrl, uint8_t value8b)
{
  static uint8_t dly = 0;
  static uint8_t sample = 0;
  static uint8_t dWordNr = 0;
  static uint32_t i2sbuf32b[4];
  static uint8_t prng;
  uint8_t shift;

  // reset
  if (ctrl == 0)
  {
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
          if (dly++ > 5)
          {
            dly = 0;
            if (sample > 0)
              sample--;
          }     
        }
      } else {
        // direct output mode
        sample = value8b;
      }

      shift = sample >> 1;

#ifdef USE_DITHERING
      if (!(sample & 0x01))
      {
        shift -= (prng & 0x01);     // subtract 0 or 1 from "shift" for dithering
        prng ^= 0x01;
      }
#endif

      yield();

      shift = 0x80 - shift;     // inverse shift
      
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
      } else if (shift < 0x80)
      {
        i2sbuf32b[0] = 0x00000000;
        i2sbuf32b[1] = 0x00000000;
        i2sbuf32b[2] = 0x00000000;
        i2sbuf32b[3] = 0xFFFFFFFF >> (shift - 0x60);
      } else {
        i2sbuf32b[0] = 0x00000000;
        i2sbuf32b[1] = 0x00000000;
        i2sbuf32b[2] = 0x00000000;
        i2sbuf32b[3] = 0x00000000;
      }
    }
  }
}

// **************************************************
// **************************************************
// **************************************************
inline void startStreaming(WiFiClient *client)
{
  uint8_t i;
  uint8_t dl;

  setStatusLEDmode(STREAMING);
  
  byteCnt = 0;
  bufferPtrIn = 0;
  bufferPtrOut = 0;

  // ===================================================================================
  // fill buffer
  
  ultimeout = millis() + 1000;
  do
  {
    if (client->available())
    {
      buffer8b[bufferPtrIn] = client->read();
      bufferPtrIn = ++bufferPtrIn & (BUFFER_SIZE - 1);
      byteCnt++;
      ultimeout = millis() + 1000;
    }
  } while ((bufferPtrIn < (BUFFER_SIZE - 1)) && (client->connected()) && (millis() < ultimeout));
  
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

  ultimeout = millis() + 1000;
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
  {
    setStatusLEDmode(DISCONNECTED);
    WiFiStart();
  } else {
    setStatusLEDmode(CONNECTED);
  }

  WiFiClient client = server.available();

  // new client?
  if (client)
  {
    Serial.println("\nnew client");
    if (client.connected())
      startStreaming(&client);
    client.stop();
    Serial.printf("Client disonnected. %d bytes received\n", byteCnt);    
  }
}

