/*
**
* Heyho!
* ESP32 Smart Glasses release 2 by HeyTech - Simple Software v0.22
*
* ESP32 BLE + OLED ~ 30 mA
* ESP32 WiFi ~ peaks to 140mA
* 
*/

// https://randomnerdtutorials.com/esp32-pinout-reference-gpios/ - OK pins

#define BLE 0
#define WEB_UPDATE 1

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

#define MAX_RECEIVED_ADATA_SIZE 10

//#define TOUCH_SENSOR_GPIO 13
//#define TOUCH_SENSOR_THRESHOLD 20

#define BATTERY_GPIO 35

//#define POWER_SAVE_INTERVAL 3000

#define EEPROM_SIZE 1
#define EEPROM_PLACE 0

#include <EEPROM.h>

// Prepare to deepsleep
#include "driver/adc.h"
#include <esp_wifi.h>
#include <esp_bt.h>

// Disable brownout detector
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// OLED
#include <Arduino.h>
#include <U8g2lib.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

// BLE
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// WiFi
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <Update.h>

/* -------------- Variables -------------- */
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_MIRROR, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ SCL, /* data=*/ SDA);   // pin remapping with ESP8266 HW I2C
bool shouldDrawToOled = false;
String *aOledSendData;
unsigned long powerSavePreviousMillis = 0;
unsigned long powerSaveCurrentMillis = 0;

const char* ble_name = "ESP32 (SmartGlasses)";
bool isDeviceConnected = false;

WebServer server(80);
const char* ssid = "ESP32Update";
const char* password = "12345678";

RTC_DATA_ATTR bool isDeepSleepBoot = false;

// Variables changeable during BLE connect
int _MarginX = 0;
int _MarginY = 0;

int _TouchSensorGPIO = 13;
int _TouchSensorThreshold = 20;
int _PowerSaveInterval = 8000;

float _BatteryVoltageFlat       = 3.1f; // In my case 3.1V from ADC means ~3.6V from battery ( ADC in ESP is not linear! so we cant calculate exact value of battery just estimate!)

/* --- Operating Mode --- */
int iOperatingMode = -1;        // 0 - BLE, 1 - WEB_UPDATE, default = BLE

int getOperatingMode() {
  if (iOperatingMode == -1) iOperatingMode = EEPROM.read(EEPROM_PLACE);
  return iOperatingMode;
}

void setOperatingMode(int om, bool restart) {
  if (iOperatingMode == om) return;       // if already set

  iOperatingMode = om;

  if (restart) {
    Serial.print("[Warning] Writting to EEPROM. Mode= "); Serial.println(om);
    EEPROM.write(EEPROM_PLACE, om); EEPROM.commit();
    delay(100);
    ESP.restart();
  }
}

/* --- Button Class --- */
class ButtonClass {
  int lastState=0;

  long clickBreakTime=50;
  long lastClickTime=0;
  
  int multiClickCounter =0;
  long multiClickMaxTime = 250;
  
  bool longClickDetected = false;
  long longClickTime=500;
  
  bool shouldUpdate = false;

  int toReturn=-1;

  int samplesCurrent = 0;         // Positive reads until negative happen (prevent situation when accidfentally device detect one touch for microsecond)
  int samplesLimit = 20;
  
  public:
  // -1 - no action, 0 - long click, 1 - click, x - multiple click
  int detect(int GPIO) {    
    long now = millis();
    int btnState = (touchRead(GPIO) < _TouchSensorThreshold) ? 1 : 0;

    // Samples - prevent situation when accidfentally device detect one touch for microsecond
    if (btnState != lastState) {
      samplesCurrent++;

      if (samplesCurrent < samplesLimit) return -1;
    }
    samplesCurrent = 0;

//    Serial.println(touchRead(GPIO));
//    Serial.println(btnState);
  
    if (btnState != lastState && (now - lastClickTime) > clickBreakTime) {
      if (btnState == 0 && !longClickDetected) {                                                        // Button click detected when button goes up
        multiClickCounter++;
      }
      lastState = btnState;
      lastClickTime=now;
      longClickDetected=false;
    }
  
    if ( (now - lastClickTime) > longClickTime) {
      if (btnState == 1 && !longClickDetected) {
        longClickDetected=true;
        shouldUpdate=true;
      }    
    }
  
    if ( ((now - lastClickTime) > multiClickMaxTime) && multiClickCounter > 0 ) shouldUpdate=true;

    toReturn=-1;
    if (shouldUpdate) {
      if      (longClickDetected    ) { toReturn = 0; }
      else if (multiClickCounter==1 ) { toReturn = 1; }
      else if (multiClickCounter >1 ) { toReturn = multiClickCounter;  }
      shouldUpdate=false;
      multiClickCounter=0;
    }

    return toReturn;
  }
};

/* --- Custom Display Functions --- */
class DisplayClass {
  public:
    int __tick = 150; // ms
    
    int __previousScreen=0;
    int __currentScreen=0;
    
    long __lastTickTime=0;

    // Offset for scroll/other simple animations
    int __Offset=0;  

    void tickScreen(String *aDataStr) {      
      __currentScreen = aDataStr[0].toInt();

      // Tick every 250 ms
      long tickTime = millis();
      
      if ( (tickTime - __lastTickTime) > __tick ) {
        if (__previousScreen != __currentScreen)  __Offset=0;        // When powersavemode cut a text for example from SMS u can just continue reading - just call this screen again it will continue from prev stop

        switch (__currentScreen) {
          case 0: default : screenMain   (aDataStr[1]        , aDataStr[2]        , aDataStr[3]        , aDataStr[4].toInt(), aDataStr[5] ); break;
          case 1          : screenMsgNoti(aDataStr[1].toInt(), aDataStr[2]        , aDataStr[3]                                           ); break;
          case 2          : screenCall   (aDataStr[1]                                                                                     ); break;
          case 3          : screenNav    (aDataStr[1]        , aDataStr[2]        , aDataStr[3]        , aDataStr[4].toInt()              ); break;
          case 4          : screenList   (aDataStr[1].toInt(), aDataStr[2]        , aDataStr[3].toInt(), aDataStr[4]                      ); break;
          case 5          : screenMusic  (aDataStr[1].toInt(), aDataStr[2]        , aDataStr[3].toInt(), aDataStr[4].toInt()              ); break;
        }
        
        __previousScreen = __currentScreen;
        __lastTickTime = tickTime;
      }
    }

    // --- Screens ---
    void screenMusic(int musicIcon, String title, int symbolPlayStop, int symbolNext) {
      int titleFontSize = 8;

      //setFontSize(titleFontSize);
      int d_width = u8g2.getDisplayWidth();
      //int t_width = getStringWidth(title);

      // Draw
      u8g2.clearBuffer();

      // Scroll text
      drawSymbol(0, 9, musicIcon, 1);            
      
      setFontSize(titleFontSize);
      drawString(13, 9, title);

      // Symbols
      drawSymbol(0 , 30, symbolPlayStop, 2);
      drawSymbol(22, 30, symbolNext, 2);
      
      u8g2.sendBuffer();
    }
    
    void screenList(int symbolMain, String title, int symbolSub, String text) {                                    // List - Shopping List, cooking recpeie; max 1 text on page
      // Max 2 lines
      String line1 = text.substring(0 , 10);
      String line2 = text.substring(11, 20);

      u8g2.clearBuffer();

      drawSymbol(0, 9, symbolMain, 1); setFontSize(8 ); drawString(13, 9, title);

      // Lines
      drawSymbol(0, 22, symbolSub, 1);
      
      setFontSize(8);
      drawString(10, 21, line1);
      drawString(10, 30, line2);
      u8g2.sendBuffer();
    }

    void screenNav(String maxSpeed, String distance, String distanceToDes, int symbol) {
      u8g2.clearBuffer();
      drawSymbol(0, 22, symbol, 2);

      setFontSize(7 ); drawString(18, 7, maxSpeed);
      setFontSize(12); drawString(21, 21, distance);
      setFontSize(7 ); drawString(18, 31, distanceToDes);
      u8g2.sendBuffer();
    }

    void screenCall(String from) {
      // Restart offset when 3 dots reached but give 2 more updates for larger delay beetween 3 dots and back to 1 dot
      if (__Offset >= 6) __Offset=0;
            
      u8g2.clearBuffer();
      drawSymbol(0, 9, 260, 1);
      
      setFontSize(9);
      if      (__Offset == 0) drawString(12, 9, "Calling.");
      else if (__Offset == 1) drawString(12, 9, "Calling..");
      else                    drawString(12, 9, "Calling...");
      setFontSize(8); drawString(0, 22, from);
      
      u8g2.sendBuffer();

      // Increase dots offset
      __Offset++;
    }

    void screenMsgNoti(int symbol, String from, String text) {                                    // Msg/Notification
      int msgFontSize = 10;
      int d_width = u8g2.getDisplayWidth();
      String cut = text;

      // Restart scroll when reaches end
      if (__Offset >= text.length() ) __Offset=0;

      u8g2.clearBuffer();
      
      // Same text
      drawSymbol(0, 9, symbol, 1);
      setFontSize(8 ); drawString(13, 9, from);
      
      // Scroll Text
      setFontSize(msgFontSize);
      
      cut = text.substring(0 + __Offset, d_width - 108 + __Offset);                       // For this font 108
      
      drawString(0, 25, cut);
      u8g2.sendBuffer();

      // Increase offset for scroll
      __Offset++;
    }

    void screenMain(String HH, String mm, String date, int symbol, String degrees) {
      // Serial.println(time + " OK");
      int addDePx = 0;
      if      (degrees .length() <  3) addDePx = 4;

      if (__Offset >= 10) __Offset=0;

      // Time always in format: HH:mm
      // Degrees format: DD'C' - when only one D then we have to move everything

      u8g2.clearBuffer();
      setFontSize(12);

      // Blinking ":" -> 15:40
      drawString(0, 17, HH);
      if      (__Offset < 4) drawString(20, 17, ":");
      drawString(25, 17, mm);
      
      setFontSize(8 ); drawString(4          , 29, date);

      drawSymbol(52         , 22, symbol, 2);
      setFontSize(7 ); drawString(50 + addDePx, 30, degrees);

      u8g2.sendBuffer();
      __Offset++;
    }

    // Utilities - Symbols
    void drawSymbol(int x, int y, int index, int size) {                                        // https://github.com/olikraus/u8g2/wiki/u8g2reference#drawglyph
      switch (size) {
        case 1  : u8g2.setFont(u8g2_font_open_iconic_all_1x_t ); break;                         // 8 px height
        case 2  : u8g2.setFont(u8g2_font_open_iconic_all_2x_t ); break;                         // 16 px height
        case 4  : u8g2.setFont(u8g2_font_open_iconic_all_4x_t ); break;
        case 6  : u8g2.setFont(u8g2_font_open_iconic_all_6x_t ); break;
        case 8  : u8g2.setFont(u8g2_font_open_iconic_all_8x_t ); break;
        default : u8g2.setFont(u8g2_font_open_iconic_all_1x_t ); break;
      }

      u8g2.drawGlyph(_MarginX+x, _MarginY+y, index);                                                         // podawanie w hex lub index (na stronie u8g2 w wybranej czcionce po lewej stronie pisze ktory to znak (dec/hex) tylko to index 1 znaku w linii, jak chcesz inny to dodaj sobie i masz :)
    }

    // Utilities - Text
    int getStringWidth(String text) {               // Estimated
      int t_width = 0;
      String s = "";

      for (int i = 0; i < text.length(); i++) {
        s = text.charAt(i);
        t_width += u8g2.getStrWidth(s.c_str()) + 1;        // 1 space between characters
      }

      return t_width;
    }

    void setFontSize(int size) {
      switch (size) {
        case 4 : u8g2.setFont(u8g2_font_u8glib_4_tr)      ; break;
        case 5 : u8g2.setFont(u8g2_font_micro_tr)         ; break;
        case 6 : u8g2.setFont(u8g2_font_5x8_tr)           ; break;                                              // 6 px height - default
        case 7 : u8g2.setFont(u8g2_font_profont11_tr)     ; break;
        case 8 : u8g2.setFont(u8g2_font_profont12_tr)     ; break;
        case 9 : u8g2.setFont(u8g2_font_t0_14_tr)         ; break;
        case 10: u8g2.setFont(u8g2_font_unifont_tr)       ; break;
        case 12: u8g2.setFont(u8g2_font_samim_16_t_all)   ; break;
        case 18: u8g2.setFont(u8g2_font_ncenR18_tr)       ; break;
        default: u8g2.setFont(u8g2_font_5x8_tr)           ; break;
      }
    }

    void drawString(int x, int y, String text) { u8g2.drawStr(_MarginX+x, _MarginY+y, text.c_str() ); }

    // --- Other ---
    void sendBuffer() { u8g2.sendBuffer(); }

    void clearBuffer() { u8g2.clearBuffer(); }
    void clear() { u8g2.clear(); }

    int lastPowerSaveMode = -1;
    bool setPowerSave(int i) {
      bool changed=false;
      
      if (lastPowerSaveMode != i) {
        if (i == 1) Serial.println("[INFO - OLED] Power save mode");
        u8g2.setPowerSave(i);
        changed = true;
      }
      lastPowerSaveMode = i;

      return changed;
    }
};

/* --- BLE - Data receive --- */
class BLEReceive {
  public:

    // --- Connected/Disconnected ---
    class BLEConnectState : public BLEServerCallbacks {
        void onConnect(BLEServer* pServer) {
          isDeviceConnected = true;
          Serial.println("[INFO - BLE] Device connected");
          //BLEDevice::startAdvertising();
        }

        void onDisconnect(BLEServer* pServer) {
          isDeviceConnected = false;
          Serial.println("[INFO - BLE] Device disconnected");
          delay(100);
          ESP.restart();    // Bug in BLE cant connect after disconnecting so just restart for now
        }
    };

    // --- Receive ---
    class BLEReceiveClass : public BLECharacteristicCallbacks {
        String aReceivedData[MAX_RECEIVED_ADATA_SIZE];
        
        int    indexRD = 0;

        void addReceivedData(String s) {
          aReceivedData[indexRD++] = s;
        }
        String getReceivedData(int i) {
          return aReceivedData[i];
        }
        void clearReceivedData() {
          for (int i = 0; i < MAX_RECEIVED_ADATA_SIZE; i++) {
            aReceivedData[i] = "";
          };
          indexRD = 0;
        }

        void onWrite(BLECharacteristic *pCharacteristic) {
          String sReceived = "";
          std::string receivedValue = pCharacteristic->getValue();

          if (receivedValue.length() > 0) {
            sReceived = receivedValue.c_str();
            Serial.print("[INFO - BLE] Received: "); Serial.println(sReceived);

            // --- Change Operating Mode - from ble to wu ---
            if      (sReceived == "#OM=0"   )  { setOperatingMode(BLE        , true); return; }
            else if (sReceived == "#OM=1"   )  { setOperatingMode(WEB_UPDATE , true); return; }
            else if (sReceived == "#RESTART")  { ESP.restart()                      ; return; }

            else if (sReceived.startsWith("#MX="  ))  { _MarginX              =sReceived.substring(4, sReceived.length()).toInt(); Serial.println(_MarginX)             ; return; }  // Margin X             Def: 0
            else if (sReceived.startsWith("#MY="  ))  { _MarginY              =sReceived.substring(4, sReceived.length()).toInt(); Serial.println(_MarginY)             ; return; }  // Margin Y             Def: 0
            else if (sReceived.startsWith("#PSI=" ))  { _PowerSaveInterval    =sReceived.substring(5, sReceived.length()).toInt(); Serial.println(_PowerSaveInterval)   ; return; }  // PowerSaveInterval    Def: 8000
            else if (sReceived.startsWith("#TSG=" ))  { _TouchSensorGPIO      =sReceived.substring(5, sReceived.length()).toInt(); Serial.println(_TouchSensorThreshold); return; }  // TouchSensorGPIO      Def: 13
            else if (sReceived.startsWith("#TST=" ))  { _TouchSensorThreshold =sReceived.substring(5, sReceived.length()).toInt(); Serial.println(_TouchSensorThreshold); return; }  // TouchSensorThreshold Def: 20
            
            else if (sReceived.startsWith("#BF="  ))  { _BatteryVoltageFlat   =sReceived.substring(4, sReceived.length()).toFloat(); Serial.println(_BatteryVoltageFlat); return; }  // Battery Flat Voltage Def: 3.1f // In my case 3.1V from ADC means ~3.6V from battery ( ADC in ESP is not linear! so we cant calculate exact value of battery just estimate!)

            // #0,Text,saf....  - #0 - screen select - 0=main
            if (sReceived.startsWith("#")) {
              sReceived = sReceived.substring(1, sReceived.length());                             // Getting rid of #

              clearReceivedData();

              // Split String by delimeter - '|'
              for (int i = 0; i < sReceived.length(); i++) {
                if (indexRD >= MAX_RECEIVED_ADATA_SIZE) break;                                                        // Over the array param limit - something gone wrong

                if (sReceived.charAt(i) == '|') {
                  indexRD++;
                  continue;
                }
                aReceivedData[indexRD] += sReceived.charAt(i);
              }

              aOledSendData = aReceivedData;
              shouldDrawToOled = true;
              powerSavePreviousMillis = millis();                                                // Measure time from now (ble send)
              return;
            }
          }
        }
    };

    // --- Send ---
    BLECharacteristic *pCharacteristic;
    void sendValue(String value) {
      pCharacteristic->setValue(value.c_str());
      pCharacteristic->notify();
      delay(30);
    }

    // --- Init ---
    void init() {
      BLEDevice::init(ble_name);

      BLEServer *pServer = BLEDevice::createServer();
      pServer->setCallbacks(new BLEConnectState());

      BLEService *pService = pServer->createService(SERVICE_UUID);

      pCharacteristic = pService->createCharacteristic(
                          CHARACTERISTIC_UUID,
                          BLECharacteristic::PROPERTY_READ |
                          BLECharacteristic::PROPERTY_WRITE
                        );

      pCharacteristic->setCallbacks(new BLEReceiveClass());
      pCharacteristic->setValue("1");

      pService->start();

      BLEAdvertising *pAdvertising = pServer->getAdvertising();
      pAdvertising->start();

      String sBleMacAddress = BLEDevice::getAddress().toString().c_str();

      Serial.print("[INFO - BLE] Starting with MAC address: ");
      Serial.println(sBleMacAddress);
    }
};

/* --- WU - WebUpdate OTA --- */
/* Server Index Page */
const char* serverIndex =
  "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
  "<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
  "<input type='file' name='update'>"
  "<input type='submit' value='Update'>"
  "</form>"
  "<div><a href='/ble'>Go back to BLE</a></div>"
  "<div><a href='/mac'>Check MAC Address</a></div>"
  "<div id='prg'>progress: 0%</div>"
  "<script>"
  "$('form').submit(function(e){"
  "e.preventDefault();"
  "var form = $('#upload_form')[0];"
  "var data = new FormData(form);"
  " $.ajax({"
  "url: '/update',"
  "type: 'POST',"
  "data: data,"
  "contentType: false,"
  "processData:false,"
  "xhr: function() {"
  "var xhr = new window.XMLHttpRequest();"
  "xhr.upload.addEventListener('progress', function(evt) {"
  "if (evt.lengthComputable) {"
  "var per = evt.loaded / evt.total;"
  "$('#prg').html('progress: ' + Math.round(per*100) + '%');"
  "}"
  "}, false);"
  "return xhr;"
  "},"
  "success:function(d, s) {"
  "console.log('success!')"
  "},"
  "error: function (a, b, c) {"
  "}"
  "});"
  "});"
  "</script>";

class WebUpdate {

  public:
    void init() {
      WiFi.begin(ssid, password);
      for (int i = 0; WiFi.status() != WL_CONNECTED; i++) {
        if (i >= 50) {
          setOperatingMode(BLE, true);  // after 50 loops if not connected go back to BLE ~25 seconds
          break;
        }
        delay(500);
        Serial.print(".");
      }

      Serial.println("");
      Serial.print("[INFO - WebUpdate] Connected. IP address:");
      Serial.println(WiFi.localIP());

      server.on("/", HTTP_GET, []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/html", serverIndex);
      });
      // Back to BLE mode
      server.on("/ble", []() {
        server.send(200, "text/plain", "OK - Turning on BLE");
        setOperatingMode(BLE, true);
      });
      // Print MAC Address
      server.on("/mac", []() {
        server.send(200, "text/plain", "MAC Address: " + WiFi.macAddress());
      });
      /*handling uploading firmware file */
      server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        ESP.restart();
      }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          Serial.printf("Update: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          /* flashing firmware to ESP*/
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.end(true)) { //true to set the size to the current progress
            Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
          } else {
            Update.printError(Serial);
          }
        }
      });
      server.begin();
    }
};

// --- Main ---
DisplayClass dc;
BLEReceive ble;
ButtonClass button;

void setup() {
  Serial.begin(115200);
  pinMode(BATTERY_GPIO, INPUT);

  // EEPROM init
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(EEPROM_PLACE) != 0 && EEPROM.read(EEPROM_PLACE) != 1) {
    EEPROM.write(EEPROM_PLACE, BLE);  // First run - init EEPROM
    EEPROM.commit();
    Serial.println("[INFO] EEPROM init done!");
  }

  // Info
  Serial.println("[INFO] Version: 0.22");
  Serial.println("[WARNING] Disable brownout detector");
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  
  // My oled needs ESP boot after 3 sec if yours doesnt delete "delay(3000);"
  delay(3000);                                                            
  u8g2.begin();
  
  if ( getOperatingMode() == WEB_UPDATE ) {
    Serial.println("[INFO] OperatingMode: WEB_UPDATE");
    WebUpdate webUpdate; webUpdate.init();
  } else if (getOperatingMode() == BLE) {
    Serial.println("[INFO] OperatingMode: BLE");
    ble.init();
  }

  // Info OLED
  delay(100);
  dc.setFontSize(7); dc.drawString(10, 10, "Ready! Mode:" ); dc.drawString(10, 20, (getOperatingMode() == WEB_UPDATE) ? "WU" : "BLE" ); dc.sendBuffer(); powerSavePreviousMillis = millis();
}

void loop() {

  // If battery flat go to deep sleep
  int batteryStatus = getBatteryStatus();
  if ( batteryStatus == 0 ) {
    Serial.println("[WARNING] Battery is flat. Entering deep sleep.");
    dc.setFontSize(7); dc.drawString(20, 30, "LOW BAT!"); dc.sendBuffer();
    delay(2000);
    prepareToDeepSleep();
    delay(100);
    esp_deep_sleep_start(); 
    return;
  }
  
  // OLED power save
  oledPowerSave();
  
  if ( getOperatingMode() == WEB_UPDATE ) {
    server.handleClient();
    delay(1);
  } else {
    // Touch Sensor
    int action = button.detect(_TouchSensorGPIO);
    if ( action > -1 ) {
      if      (action == 0) { Serial.println("[INFO - TouchSensor1] Detected long click!");                         ble.sendValue("#TS0"      ); } // 0 - long click
      else if (action  > 0) { Serial.print  ("[INFO - TouchSensor1] Detected click x "   ); Serial.println(action); ble.sendValue("#TS"+String(action)); }
    }

    // Draw to OLED
    if (shouldDrawToOled) {
      dc.setPowerSave(0); delay(3);
      dc.tickScreen(aOledSendData);
    }
  }
}

/* --- Other functions --- */
float voltage;
long lastTimeCheck=0;
long checkTime=2000;

// 0 - flat, 1 - OK
int getBatteryStatus() {
  if ( (millis() - lastTimeCheck) < checkTime ) return 1;
  
  voltage = ( analogRead(BATTERY_GPIO) / 4095.0f ) * 4.25;
  Serial.print("[INFO] Approximate battery voltage: "); Serial.println(voltage); // Serial.println(analogRead(BATTERY_GPIO));
  
  lastTimeCheck=millis();
  
  if (voltage <= _BatteryVoltageFlat) return 0;
  return 1;   
}

void prepareToDeepSleep() {
  dc.setPowerSave(1);

  if (getOperatingMode() == WEB_UPDATE) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
  } else {
    btStop();  
    esp_bt_controller_disable();
  }
  adc_power_off();
}

void oledPowerSave() {
  powerSaveCurrentMillis = millis();
  if ( (powerSaveCurrentMillis - powerSavePreviousMillis) >= _PowerSaveInterval ) {
    bool changed = dc.setPowerSave(1); delay(3);
    shouldDrawToOled = false;
    if ( changed && getOperatingMode() == BLE ) ble.sendValue("1");       // Screen is off
    powerSavePreviousMillis = powerSaveCurrentMillis;
  }
}

void goDeepSleep(int seconds) {
  isDeepSleepBoot = true;
  esp_sleep_enable_timer_wakeup(seconds * 1000000);
  Serial.println("[WARNING!] Entering deep sleep ...");
  esp_deep_sleep_start();
}
