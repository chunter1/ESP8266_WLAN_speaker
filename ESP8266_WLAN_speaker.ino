
// *******************************************************************************************
//
// THIS PROJECT IS IN A VERY EARLY STATE PROOF OF CONCEPT STATE!!
// STABILITY NEEDS TO BE IMPROVED!!
// CODE NEEDS CLEAN UP ASWELL :)
//
// Set CPU clock to be 160 MHz!
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


#include <ESP8266WiFi.h>
#include "i2s.h"

const char* ssid = "yourSSID";
const char* password = "yourPASSWORD";

WiFiServer server(5522);
WiFiClient client;

unsigned long ultimeout;
uint8_t buffer8b[0x2000];
uint16_t bufferPtrIn;
uint16_t bufferPtrOut;


void setup() 
{  
  Serial.begin(115200);
  delay(1);
  
  WiFi.mode(WIFI_STA);
  WiFiStart();

  i2s_begin();
  i2s_set_rate(96000);
}


void WiFiStart()
{
  // connect to wifi
  Serial.println("");
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi connected");
  
  // server start
  server.begin();
  Serial.println("Server started");

  // show obtained IP address
  Serial.println(WiFi.localIP());
}


// ctrl = 0 : reset the PWM
// ctrl = 1 : normal playback
inline void play128bit(uint8_t ctrl, uint8_t value8b)
{
  static uint8_t shi = 0;
  static uint8_t dWordNr = 0;
  static uint32_t i2sbuf32b[4];
  static uint16_t dl = 0;
  uint8_t shiInv;

  // reset
  if (ctrl == 0)
  {
    dl = 0;
    dWordNr = 0;
    shi = 0;
    i2sbuf32b[0] = 0;
    i2sbuf32b[1] = 0;
    i2sbuf32b[2] = 0;
    i2sbuf32b[3] = 0;
    return;
  }
  
  // playback
  if (i2s_write_sample_nb(i2sbuf32b[dWordNr]))
  {
    // write 4 32bit I2S pseudo PWM values to get a 128bit PWM
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
          bufferPtrOut = (bufferPtrOut + 1) & 0x1FFF;
          shi = buffer8b[bufferPtrOut] >> 1;    // 128 bit
        } else {
          // NOTE: To avoid blobs at EOF (buffer underrun), fade out PWM to 0% (0V) starting at last valid PWM value
          if (dl++ > 100)
          {
            dl = 0;
            if (buffer8b[bufferPtrOut] > 0)
            {
              --buffer8b[bufferPtrOut];
              shi = buffer8b[bufferPtrOut] >> 1;    // 128 bit
            }
          }
        }
      } else {
        // direct output mode
        shi = value8b >> 1;    // 128 bit
      }

      shiInv = 128 - shi;

      if (shiInv < 32)
      {
        i2sbuf32b[0] = 0xFFFFFFFF >> shiInv;
        i2sbuf32b[1] = 0xFFFFFFFF;
        i2sbuf32b[2] = 0xFFFFFFFF;
        i2sbuf32b[3] = 0xFFFFFFFF;
      } else if (shiInv < 64)
      {
        i2sbuf32b[0] = 0x00000000;
        i2sbuf32b[1] = 0xFFFFFFFF >> (shiInv - 32);
        i2sbuf32b[2] = 0xFFFFFFFF;
        i2sbuf32b[3] = 0xFFFFFFFF;
      } else if (shiInv < 96)
      {
        i2sbuf32b[0] = 0x00000000;
        i2sbuf32b[1] = 0x00000000;
        i2sbuf32b[2] = 0xFFFFFFFF >> (shiInv -64);
        i2sbuf32b[3] = 0xFFFFFFFF;
      } else
      {
        i2sbuf32b[0] = 0x00000000;
        i2sbuf32b[1] = 0x00000000;
        i2sbuf32b[2] = 0x00000000;
        i2sbuf32b[3] = 0xFFFFFFFF >> (shiInv - 96);
      }

// *****************************************
// QUICK & DIRTY WORKAROUND
// Still unclear why outputing 0 to PWM results in
// a PWM output of "1" (= LSB bit set instead of all bits cleared)
// So for now we do it the hard way an force all bits zero.

      if (shiInv == 128)
      {
        i2sbuf32b[0] = 0x00000000;
        i2sbuf32b[1] = 0x00000000;
        i2sbuf32b[2] = 0x00000000;
        i2sbuf32b[3] = 0x00000000;
      }
// *****************************************    
    }
  }
}


void loop() 
{
  // wifi connected?
  if (WiFi.status() != WL_CONNECTED)
    WiFiStart();

  client = server.available();

  yield();

  // client connected?
  if (client)
  {
    Serial.println("Client connected");

    // Wait for client so send data
    ultimeout = millis() + 1000;
    while ((!client.available()) && (millis() < ultimeout))
      delay(1);
  
    if (millis() > ultimeout) 
    { 
      Serial.println("Client connection time-out!");
      return; 
    }

    // ===================================================================================

    Serial.print("Buffering...");

    bufferPtrIn = 0;
    bufferPtrOut = 0;

    do
    {      
      if (client.available())
      {
        buffer8b[bufferPtrIn] = client.read();
        bufferPtrIn = ++bufferPtrIn & 0x1FFF;
      }
    } while (bufferPtrIn < 0x1FFF);
   
    Serial.println("done");

    // ===================================================================================
    
    Serial.println("Starting playback");

    // reset PWM
    play128bit(0,0);
    
    // ramp up PWM to 50% to avoid "blops"
    uint8_t i;
    uint8_t dl;
    i = 0;    
    dl = 0;

    do
    {
      play128bit(2, i);
      
      if (dl++ > 200)
      {
        dl = 0;
        i++;
      }
      
    } while (i < 128);
    
    do
    {
      play128bit(1,0);

      if (client.available() && (((bufferPtrIn + 1) & 0x1FFF) != bufferPtrOut))
      {
        buffer8b[bufferPtrIn] = client.read();
        bufferPtrIn = ++bufferPtrIn & 0x1FFF;
        ultimeout = millis() + 1000;
      }
      
    } while (client.available() || (millis() < ultimeout) || (bufferPtrOut != bufferPtrIn));

    client.stop();
  
    Serial.println("Client disonnected");
  }
}

