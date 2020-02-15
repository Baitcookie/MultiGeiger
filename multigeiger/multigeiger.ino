//====================================================================================================================================
// Project: Simple Multi-Geiger
// (c) 2019,2020 by the authors, see AUTHORS file in toplevel directory.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
// (see LICENSE file in toplevel directory)
//
// Description: With minimal external components you are able to build a Geiger Counter that:
//   - is precise
//   - cheap
//   - makes the typical tick sounds
//   - produces a listing via RS232 (via USB)
//   - is adaptable to your application
//
// Information about the new Heltec board ESP32 WIFI OLED:
// - how to get the device up and running:
//   - https://robotzero.one/heltec-wifi-kit-32/
// - driver for the USB=>UART-Chip CP2102:
//   - http://esp32.net/usb-uart/#SiLabs
// - infos from the Heltec, the board manufacturer:
//   - http://www.heltec.cn/project/wifi-kit-32/?lang=en
// - it is sold on ebay e.g. under the following names:
//   - "1X(ESP32 WIFI Bluetooth Entwicklungsboard OLED 0.96 "Display IOT Kit Modul GY"
// - there is also a variant with LoRaWAN:
//   - http://fambach.net/esp32-wifi-lora-433/
//   - https://www.hackerspace-ffm.de/wiki/index.php?title=Heltec_Wifi_LoRa_32
//
//

// Fix Parameters
// Values for Serial_Print_Mode to configure Serial (USB) output mode.  DON'T TOUCH!
#define Serial_None 0            // No Serial output
#define Serial_Debug 1           // Only debug and error messages
#define Serial_Logging 2         // Log measurements as a table
#define Serial_One_Minute_Log 3  // "One Minute logging"
#define Serial_Statistics_Log 4  // Logs time [us] between two events
//
// At sensor.community predefined counter tubes:
#define TUBE_UNKNOWN 0
#define SBM20 1
#define SBM19 2
#define Si22G 3


// Values for CPU (board types)
// WIFI -> Heltec Wifi Kit 32
#define WIFI 0
// LORA -> Heltec Wifi Lora 32 (V2)
#define LORA 1
// STICK -> Heltec Wireless Stick (has LoRa on board)
#define STICK 2


// Includes
//====================================================================================================================================
#include "version.h"
#include "log.h"
#include "userdefines.h"
#include <Arduino.h>
#include <U8x8lib.h>
#include <HTTPClient.h>

#include "thp_sensor.h"
#include "tube.h"
#include "switches.h"
#include "speaker.h"
#include "webconf.h"

// Check if a CPU (board) with LoRa is selected. If not, deactivate SEND2LORA.
#if !((CPU==LORA) || (CPU==STICK))
#undef SEND2LORA
#define SEND2LORA 0
#endif

// for LoRa
#if SEND2LORA==1
#include "loraWan.h"
#endif

//====================================================================================================================================
// IOs
// used for OLED_SDA 4
// used for OLED_SCL 15
// used for OLED_RST 16
//
// used for optional LoRa    SX1276 (pin) => ESP32 (pin)
// used for optional LoRa    ==========================
// used for optional LoRa    SCK = GPIO5
// used for optional LoRa    MISO = GPIO19
// used for optional LoRa    MOSI = GPIO27
// used for optional LoRa    CS = GPIO18
// used for optional LoRa    RESET = GPIO14
// used for optional LoRa    DIO0 (8) = GPIO26 (15)
// used for optional LoRa    DIO1 (9) = GPIO33 (13)
// used for optional LoRa    DIO2 (10) = GPIO32 (12)

// What to send to sensor.community etc.
enum {SEND_CPM, SEND_BME};

#if CPU == STICK
#define PIN_DISPLAY_ON 25
#endif

// Measurement interval (default 2.5min) [sec]
#define MEASUREMENT_INTERVAL 150

// MAX time to wait until connected. [msec]
// If there is still no connection after that time,
// measurements will start, but won't be sent to servers.
#define MAX_WAIT_TIME 300000

// Max time the greeting display will be on. [msec]
#define AFTERSTART 5000

// Dummy server for debugging
#define SEND2DUMMY 0

typedef struct {
  const char *type;          // type string for sensor.community
  const char nbr;            // number to be sent by LoRa
  const float cps_to_uSvph;  // factor to convert counts per second to µSievert per hour
} TUBETYPE;

TUBETYPE tubes[] = {
  // use 0.0 conversion factor for unknown tubes, so it computes an "obviously-wrong" 0.0 uSv/h value rather than a confusing one.
  {"Radiation unknown", 0, 0.0},
  // The conversion factors for SBM-20 and SBM-19 are taken from the datasheets (according to Jürgen)
  {"Radiation SBM-20", 20, 1 / 2.47},
  {"Radiation SBM-19", 19, 1 / 9.81888},
  // The Si22G conversion factor was determined by Juergen Boehringer like this:
  // Set up a Si22G based MultiGeiger close to the official odlinfo.bfs.de measurement unit in Sindelfingen.
  // Determine how many counts the Si22G gives within the same time the odlinfo unit needs for 1uSv.
  // Result: 44205 counts on the Si22G for 1 uSv.
  // So, to convert from cps to uSv/h, the calculation is: uSvh = cps * 3600 / 44205 = cps / 12.2792
  {"Radiation Si22G", 22, 1 / 12.2792}
};

//====================================================================================================================================
// Constants

// Hosts for data delivery
#define MADAVI "http://api-rrd.madavi.de/data.php"
#define SENSORCOMMUNITY "http://api.sensor.community/v1/push-sensor-data/"
#define TOILET "http://ptsv2.com/t/enbwck3/post"

//====================================================================================================================================
// Variables

unsigned int GMC_counts = 0;
unsigned int GMC_counts_2send = 0;
unsigned int accumulated_GMC_counts = 0;
unsigned long count_timestamp = millis();
unsigned long count_timestamp_2send = millis();
unsigned long last_count_timestamp = millis();
unsigned long last_count_timestamp_2send = millis();
unsigned long accumulated_time = 0;
unsigned int last_GMC_counts = 0;
unsigned int hvpulsecnt2send = 0;
float Count_Rate = 0.0;
float Dose_Rate = 0.0;
float accumulated_Count_Rate = 0.0;
float accumulated_Dose_Rate = 0.0;
unsigned long lastMinuteLog = millis();
unsigned int lastMinuteLogCounts = 0;
unsigned int current_cpm = 0;

unsigned long toSendTime = millis();
unsigned long afterStartTime = 0;
unsigned long time2display = millis();

bool speakerTick = SPEAKER_TICK;
bool ledTick = LED_TICK;
bool showDisplay = SHOW_DISPLAY;
bool playSound = PLAY_SOUND;
bool displayIsClear = false;
float GMC_factor_uSvph = 0.0;
const char *Serial_Logging_Header = "%10s %15s %10s %9s %9s %8s %9s %9s %9s";
const char *Serial_Logging_Body = "%10d %15d %10f %9f %9d %8d %9d %9f %9f";
const char *Serial_One_Minute_Log_Header = "%4s %10s %29s";
const char *Serial_One_Minute_Log_Body = "%4d %10d %29d";
const char *Serial_Logging_Name = "Simple Multi-Geiger";
unsigned int lora_software_version;
const char *dashes = "-------------------------------------------------------------------------------------------------";
int Serial_Print_Mode = SERIAL_DEBUG;


//====================================================================================================================================
// Function Prototypes

void DisplayGMC(int TimeSec, int RadNSvph, int CPS);
void DisplayStartscreen(void);
void sendData2TTN(int sendwhat, unsigned int hvpulses, unsigned int timediff);
void sendData2http(const char *host, int sendwhat, unsigned int hvpulses, unsigned int timediff, bool debug);
String buildhttpHeaderandBodyBME(HTTPClient *head, float t, float h, float p, bool addname);
String buildhttpHeaderandBodySBM(HTTPClient *head, int radiation_cpm, unsigned int hvpulses, unsigned int timediff, bool addname);
void displayStatusLine(String txt);
void clearDisplayLine(int line);
char *nullFill(int n, int digits);


// Type of OLED display
#if CPU != STICK
U8X8_SSD1306_128X64_NONAME_HW_I2C u8x8(/* reset=*/ 16, /* clock=*/ 15, /* data=*/ 4);
#else
U8X8_SSD1306_64X32_NONAME_HW_I2C u8x8(/* reset=*/ 16, /* clock=*/ 15, /* data=*/ 4);
#endif

//====================================================================================================================================
// ******* SETUP *******
//====================================================================================================================================
void setup() {
  setup_log(DEFAULT_LOG_LEVEL);

  // OLED-Display
  u8x8.begin();

  setup_speaker();
  setup_switches();

  #if CPU == STICK
  pinMode(PIN_DISPLAY_ON, OUTPUT);
  digitalWrite(PIN_DISPLAY_ON, HIGH);
  #endif

  #if SEND2LORA
  int major, minor, patch;
  sscanf(VERSION_STR, "V%d.%d.%d", &major, &minor, &patch);
  lora_software_version = (major << 12) + (minor << 4) + patch;
  #endif

  setup_thp_sensor();
  setup_webconf();

  // Set up conversion factor to uSv/h according to GM tube type:
  GMC_factor_uSvph = tubes[TUBE_TYPE].cps_to_uSvph;

  // Write Header of Table, depending on the logging mode:

  if (Serial_Print_Mode == Serial_Logging) {
    log(INFO, dashes);
    log(INFO, "%s, Version %s", Serial_Logging_Name, VERSION_STR);
    log(INFO, dashes);
    log(INFO, Serial_Logging_Header,
        "GMC_counts", "Time_difference", "Count_Rate", "Dose_Rate", "HV Pulses", "Accu_GMC", "Accu_Time", "Accu_Rate", "Accu_Dose");
    log(INFO, Serial_Logging_Header,
        "[Counts]",   "[ms]",            "[cps]",      "[uSv/h]",   "[-]",       "[Counts]", "[ms]",      "[cps]",     "[uSv/h]");
    log(INFO, dashes);
  }

  if (Serial_Print_Mode == Serial_One_Minute_Log) {
    log(INFO, dashes);
    log(INFO, "%s, Version %s", Serial_Logging_Name, VERSION_STR);
    log(INFO, dashes);
    log(INFO, Serial_One_Minute_Log_Header,
        "Time", "Count_Rate", "Counts");
    log(INFO, Serial_One_Minute_Log_Header,
        "[s]",  "[cpm]",      "[Counts per last measurement]");
    log(INFO, dashes);
  }

  if (Serial_Print_Mode == Serial_Statistics_Log) {
    log(INFO, dashes);
    log(INFO, "%s, Version %s", Serial_Logging_Name, VERSION_STR);
    log(INFO, dashes);
    log(INFO, "Time between two impacts");
    log(INFO, "[usec]");
    log(INFO, dashes);
  }

  #if SEND2LORA
  // init LoRa
  lorawan_setup();
  #endif

  DisplayStartscreen();
  displayIsClear = false;

  if (playSound)
    play_start_sound();

  afterStartTime = AFTERSTART;

  setup_tube();
}

// ===================================================================================================================================
// ************* LOOP *************************
// ===================================================================================================================================

#define DISPLAYREFRESH 10000
#define MAXCOUNTS 100
#define HVPULSE_MS 1000

void loop() {
  unsigned long time_difference;
  unsigned int HV_pulse_count;
  char *sw;
  unsigned long current_ms = millis();  // to save multiple calls to millis()
  bool update_display;

  sw = read_switches();

  // copy values from ISR
  portENTER_CRITICAL(&mux_GMC_count);                            // enter critical section
  GMC_counts = isr_GMC_counts;
  // Check if there are enough pulses detected or if enough time has elapsed.
  // If yes, then it is time to calculate the pulse rate, update the display and recharge the HV capacitor.
  update_display = (GMC_counts >= MAXCOUNTS) || ((current_ms - time2display) >= DISPLAYREFRESH);
  if (update_display) isr_GMC_counts = 0;
  count_timestamp = isr_count_timestamp;
  portEXIT_CRITICAL(&mux_GMC_count);                             // leave critical section

  // Pulse the high voltage if we got enough GMC pulses to update the display or at least every 1000ms.
  if (update_display || (current_ms - hvpulse_timestamp) >= HVPULSE_MS) {
    HV_pulse_count = gen_charge_pulses(false);               // charge HV capacitor - sets hvpulse_timestamp!
    hvpulsecnt2send += HV_pulse_count;                       // count for sending
  }

  if (update_display) {
    time2display = current_ms;
    time_difference = count_timestamp - last_count_timestamp;  // calculate all derived values
    last_count_timestamp = count_timestamp;                    // notice the old timestamp
    accumulated_time += time_difference;                       // accumulate all the time
    accumulated_GMC_counts += GMC_counts;                      // accumulate all the pulses
    lastMinuteLogCounts += GMC_counts;

    Count_Rate = 0.0;
    if (time_difference != 0) {
      Count_Rate = (float)GMC_counts * 1000.0 / (float)time_difference; // calculate the current count rate
    }

    Dose_Rate = Count_Rate * GMC_factor_uSvph;                          // ... and dose rate

    // calculate the radiation over the complete time from start
    accumulated_Count_Rate = 0.0;
    if (accumulated_time != 0) {
      accumulated_Count_Rate = (float)accumulated_GMC_counts * 1000.0 / (float)accumulated_time;
    }
    accumulated_Dose_Rate = accumulated_Count_Rate * GMC_factor_uSvph;

    // ... and display it.
    if (showDisplay && sw[DISPLAY_ON]) {
      DisplayGMC(((int)accumulated_time / 1000), (int)(accumulated_Dose_Rate * 1000), (int)(Count_Rate * 60));
      displayIsClear = false;
    } else {
      if (!displayIsClear) {
        u8x8.clear();
        clearDisplayLine(4);
        clearDisplayLine(5);
        displayIsClear = true;
      }
    }

    if (Serial_Print_Mode == Serial_Logging) {                       // Report all
      log(INFO, Serial_Logging_Body,
          GMC_counts, time_difference, Count_Rate, Dose_Rate, HV_pulse_count,
          accumulated_GMC_counts, accumulated_time, accumulated_Count_Rate, accumulated_Dose_Rate);
    }

    if (Serial_Print_Mode == Serial_One_Minute_Log) {                // 1 Minute Log active?
      if (current_ms > (lastMinuteLog + 60000)) {                    // Time reached for next 1-Minute log?
        unsigned int lastMinuteLogCountRate = ((lastMinuteLogCounts * 60000) / (current_ms - lastMinuteLog));   // = *60 /1000
        if (((((lastMinuteLogCounts * 60000) % (current_ms - lastMinuteLog)) * 2) / (current_ms - lastMinuteLog)) >= 1) {
          lastMinuteLogCountRate++;                              // Rounding
        }
        log(INFO, Serial_One_Minute_Log_Body,
            (current_ms / 1000),
            lastMinuteLogCountRate,  // = *60 /1000 +0.5: to reduce rounding errors
            lastMinuteLogCounts);
        lastMinuteLogCounts = 0;
        lastMinuteLog = current_ms;
      }
    }
  }

  if ((Serial_Print_Mode == Serial_Statistics_Log) && isr_gotGMCpulse) {  // statistics log active?
    unsigned int count_time_between;
    portENTER_CRITICAL(&mux_GMC_count);
    count_time_between = isr_count_time_between;
    isr_gotGMCpulse = 0;
    portEXIT_CRITICAL(&mux_GMC_count);
    log(INFO, "%d", count_time_between);
  }

  // If there were no pulses after 3 secs after start,
  // clear display anyway and show 0 counts.
  if (afterStartTime && ((current_ms - toSendTime) >= afterStartTime)) {
    afterStartTime = 0;
    if (showDisplay) {
      DisplayGMC(((int)accumulated_time / 1000), (int)(accumulated_Dose_Rate * 1000), (int)(Count_Rate * 60));
      displayIsClear = false;
    }
  }

  // Check, if we have to send to sensor.community etc.
  if ((current_ms - toSendTime) >= (MEASUREMENT_INTERVAL * 1000)) {
    toSendTime = current_ms;
    portENTER_CRITICAL(&mux_GMC_count);
    GMC_counts_2send = isr_GMC_counts_2send;                    // copy values from ISR
    count_timestamp_2send = isr_count_timestamp_2send;
    isr_GMC_counts_2send = 0;
    portEXIT_CRITICAL(&mux_GMC_count);
    unsigned int hvp = hvpulsecnt2send;
    hvpulsecnt2send = 0;
    time_difference = count_timestamp_2send - last_count_timestamp_2send;
    last_count_timestamp_2send = count_timestamp_2send;

    current_cpm = 0;
    if (time_difference != 0) {
      current_cpm = (int)(GMC_counts_2send * 60000 / time_difference);
    }

    if (haveBME280) {                                       // read in the BME280 values
      read_thp_sensor();
      log(DEBUG, "Measured: cpm= %d HV=%d T=%.2f H=%.f P=%.f", current_cpm, hvp, bme_temperature, bme_humidity, bme_pressure);
    } else {
      log(DEBUG, "Measured: cpm= %d HV=%d", current_cpm, hvp);
    }

    #if SEND2DUMMY
    displayStatusLine("Toilet");
    log(INFO, "SENDING TO TOILET ...");
    sendData2http(TOILET, SEND_CPM, hvp, time_difference, true);
    if (haveBME280) {
      sendData2http(TOILET, SEND_BME, hvp, time_difference, true);
    }
    delay(300);
    #endif

    #if SEND2MADAVI
    log(INFO, "Sending to Madavi ...");
    displayStatusLine("Madavi");
    sendData2http(MADAVI, SEND_CPM, hvp, time_difference, false);
    if (haveBME280) {
      sendData2http(MADAVI, SEND_BME, time_difference, hvp, false);
    }
    delay(300);
    #endif

    #if SEND2SENSORCOMMUNITY
    log(INFO, "Sending to sensor.community ...");
    displayStatusLine("sensor.community");
    sendData2http(SENSORCOMMUNITY, SEND_CPM, hvp, time_difference, false);
    if (haveBME280) {
      sendData2http(SENSORCOMMUNITY, SEND_BME, time_difference, hvp, false);
    }
    delay(300);
    #endif

    #if SEND2LORA
    log(INFO, "Sending to TTN ...");
    displayStatusLine("TTN");
    sendData2TTN(SEND_CPM, hvp, time_difference);
    if (haveBME280) {
      sendData2TTN(SEND_BME, hvp, time_difference);
    }
    #endif

    displayStatusLine(" ");
  }

  if (GMC_counts != last_GMC_counts) {
    tick(ledTick && sw[LED_ON], speakerTick && sw[SPEAKER_ON]);
    last_GMC_counts = GMC_counts;         // notice old value
  }

  // Loop for IoTWebConf
  iotWebConf.doLoop();  // see webconf.cpp
}

// ===================================================================================================================================
// ===================================================================================================================================
// Subfunctions

// ===================================================================================================================================
// OLED sub functions
void DisplayStartscreen(void) {
  char line[20];

  u8x8.clear();
  #if CPU == STICK
  // Display is only 4 lines by 8 characters; lines counting from 2 to 5
  u8x8.setFont(u8x8_font_5x8_f);        // use really small font
  for (int i = 2; i < 6; i++) {
    u8x8.drawString(0, i, "        ");  // clear all 4 lines
  }
  u8x8.drawString(0, 2, "Geiger-");
  u8x8.drawString(0, 3, " Counter");
  u8x8.drawString(0, 4, "Version:");
  snprintf(line, 9, "%s", VERSION_STR);  // 8 chars + \0 termination
  u8x8.drawString(0, 5, line);
  #else
  u8x8.setFont(u8x8_font_7x14_1x2_f);
  u8x8.println("Geiger-Counter");
  u8x8.println("==============");
  snprintf(line, 15, "%s", VERSION_STR);  // 14 chars + \0 termination
  u8x8.setCursor(7 - strlen(line) / 2, 4);
  u8x8.print(line);
  u8x8.setCursor(1, 6);
  u8x8.print("Info:boehri.de");
  #endif
};

// ===================================================================================================================================
void DisplayGMC(int TimeSec, int RadNSvph, int CPS) {
  u8x8.clear();

  #if CPU != STICK
  char output[80];
  int TimeMin = TimeSec / 60;         // calculate number of minutes
  if (TimeMin >= 999) TimeMin = 999;  // limit minutes to max. 999

  // print the upper line including time and measured radation
  u8x8.setFont(u8x8_font_7x14_1x2_f);

  if (TimeMin >= 1) {                 // >= 1 minute -> display in minutes
    sprintf(output, "%3d", TimeMin);
    u8x8.print(output);
  } else {                            // < 1 minute -> display in seconds, inverse
    sprintf(output, "%3d", TimeSec);
    u8x8.inverse();
    u8x8.print(output);
    u8x8.noInverse();
  }

  sprintf(output, "%7d nSv/h", RadNSvph);
  u8x8.print(output);
  #endif

  // print the lower line including time and CPM value
  #if CPU != STICK
  u8x8.setFont(u8x8_font_inb33_3x6_n);
  u8x8.drawString(0, 2, nullFill(CPS, 5));
  #else
  u8x8.setFont(u8x8_font_5x8_f);
  u8x8.drawString(0, 2, nullFill(RadNSvph, 8));
  u8x8.draw2x2String(0, 3, nullFill(CPS, 4));
  u8x8.drawString(0, 5, "     cpm");
  #endif

  #if CPU != STICK
  // Print 'connecting...' as long as we aren't connected
  if (iotWebConf.getState() != IOTWEBCONF_STATE_ONLINE) {
    displayStatusLine("connecting...");
  } else {
    displayStatusLine(" ");
  }
  #endif
};

#if CPU != STICK
void clearDisplayLine(int line) {
  String blank = "                ";
  u8x8.drawString(0, line, blank.c_str());
}

void displayStatusLine(String txt) {
  u8x8.setFont(u8x8_font_5x8_f);
  clearDisplayLine(7);
  u8x8.drawString(0, 7, txt.c_str());
}
#else
void clearDisplayLine(int line) {
  String blank = "        ";
  u8x8.drawString(0, line, blank.c_str());
}

void displayStatusLine(String txt) {
  u8x8.setFont(u8x8_font_5x8_f);
  clearDisplayLine(5);
  u8x8.drawString(0, 5, txt.c_str());
}
#endif

// ===================================================================================================================================
// Send to Server Subfunctions

String buildhttpHeaderandBodySBM(HTTPClient *head, unsigned int hvpulses, unsigned int timediff, boolean addname, bool debug) {
  head->addHeader("Content-Type", "application/json; charset=UTF-8");
  head->addHeader("X-PIN", "19");
  String chipID = String(ssid);
  chipID.replace("ESP32", "esp32");
  head->addHeader("X-Sensor", chipID);
  head->addHeader("Connection", "close");
  String tubetype = tubes[TUBE_TYPE].type;
  tubetype = tubetype.substring(10);
  String valuetype = (addname ? tubetype + "_" : "");
  valuetype += "counts_per_minute";
  String body = "{\"software_version\":\"" + String(VERSION_STR) + "\",\"sensordatavalues\":[";
  body += "{\"value_type\":\"" + valuetype + "\",\"value\":\"" + current_cpm + "\"}";
  body += ",{\"value_type\":\"hv_pulses\",\"value\":\"" + String(hvpulses) + "\"}";
  body += ",{\"value_type\":\"counts\",\"value\":\"" + String(GMC_counts_2send) + "\"}";
  body += ",{\"value_type\":\"sample_time_ms\",\"value\":\"" + String(timediff) + "\"}";
  body += "]}";
  if (DEBUG_SERVER_SEND == 1) {
    log(DEBUG, "body: %s", body.c_str());
  }
  return body;
}

String buildhttpHeaderandBodyBME(HTTPClient *head, boolean addname, bool debug) {
  head->addHeader("Content-Type", "application/json; charset=UTF-8");
  head->addHeader("X-PIN", "11");
  String chipID = String(ssid);
  chipID.replace("ESP32", "esp32");
  head->addHeader("X-Sensor", chipID);
  head->addHeader("Connection", "close");
  String temp = (addname ? "BME280_" : "");
  temp += "temperature";
  String humi = (addname ? "BME280_" : "");
  humi += "humidity";
  String press = (addname ? "BME280_" : "");
  press += "pressure";
  String body = "{\"software_version\":\"" + String(VERSION_STR) + "\",\"sensordatavalues\":[\
{\"value_type\":\"" + temp + "\",\"value\":\"" + String(bme_temperature, 2) + "\"},\
{\"value_type\":\"" + humi + "\",\"value\":\"" + String(bme_humidity, 2) + "\"},\
{\"value_type\":\"" + press + "\",\"value\":\"" + String(bme_pressure, 2) + "\"}\
]}";
  if (DEBUG_SERVER_SEND == 1) {
    log(DEBUG, "body: %s", body.c_str());
  }
  return body;
}

void sendData2http(const char *host, int sendwhat, unsigned int hvpulses, unsigned int timediff, bool debug) {
  HTTPClient http;
  String body;
  http.begin(host);
  if (sendwhat == SEND_CPM) {
    body = buildhttpHeaderandBodySBM(&http, hvpulses, timediff, false, debug);
  }
  if (sendwhat == SEND_BME) {
    body = buildhttpHeaderandBodyBME(&http, false, debug);
  }
  int httpResponseCode = http.POST(body);
  if (httpResponseCode > 0) {
    String response = http.getString();
    if (DEBUG_SERVER_SEND == 1) {
      log(DEBUG, "http code: %d", httpResponseCode);
      log(DEBUG, "http response: %s", response.c_str());
    }
  } else {
    log(ERROR, "Error on sending POST: %d", httpResponseCode);
  }
  http.end();
}

#if SEND2LORA
// LoRa payload:
// To minimise airtime and follow the 'TTN Fair Access Policy', we only send necessary bytes.
// We do NOT use Cayenne LPP.
// The payload will be translated via http integration and a small program
// to be compatible with sensor.community.
// For byte definitions see ttn2luft.pdf in docs directory.
void sendData2TTN(int sendwhat, unsigned int hvpulses, unsigned int timediff) {
  unsigned char ttnData[20];
  int cnt;
  if (sendwhat == SEND_CPM) {
    // first the number of counts
    ttnData[0] = (GMC_counts_2send >> 24) & 0xFF;
    ttnData[1] = (GMC_counts_2send >> 16) & 0xFF;
    ttnData[2] = (GMC_counts_2send >> 8) & 0xFF;
    ttnData[3] = GMC_counts_2send & 0xFF;
    // now 3 bytes for the time in ms for this numer of counts (max ca. 4 hours)
    ttnData[4] = (timediff >> 16) & 0xFF;
    ttnData[5] = (timediff >> 8) & 0xFF;
    ttnData[6] = timediff & 0xFF;
    // next two bytes are software version
    ttnData[7] = (lora_software_version >> 8) & 0xFF;
    ttnData[8] = lora_software_version & 0xFF;
    // next byte is the tube version
    ttnData[9] = tubes[TUBE_TYPE].nbr;
    cnt = 10;
    lorawan_send(1, ttnData, cnt, false, NULL, NULL, NULL);
  };
  if (sendwhat == SEND_BME) {
    ttnData[0] = ((int)(bme_temperature * 10)) >> 8;
    ttnData[1] = ((int)(bme_temperature * 10)) & 0xFF;
    ttnData[2] = (int)(bme_humidity * 2);
    ttnData[3] = ((int)(bme_pressure / 10)) >> 8;
    ttnData[4] = ((int)(bme_pressure / 10)) & 0xFF;
    cnt = 5;
    lorawan_send(2, ttnData, cnt, false, NULL, NULL, NULL);
  }
}
#endif

char *nullFill(int n, int digits) {
  static char erg[9];  // max. 8 digits possible!
  if (digits > 8) {
    digits = 8;
  }
  char format[5];
  sprintf(format, "%%%dd", digits);
  sprintf(erg, format, n);
  return erg;
}
