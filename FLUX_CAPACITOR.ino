//  v2 - Ported code from BTTF logo & clock unit.
//  v3 - Prevent display switching to time and back when buttons are pressed.
//     - Clear the neopixels correctly.
//     - Removed snooze code for the time being.
//     - Don't update the display colon when the alarm is in progress to avoid gaps in between neopixel updates.
//  v4 - Rework and re-implement snooze function.
//       Commented out all audio-related code for troubleshooting.
//       Added more serial output debugging statements.
//  v5 - Moved NTP server time retrieval to a function.
//       Implement regular NTP server syncs to ensure that the clock doesn't drift too far.
//       Only write the alarm time to EEPROM if necessary to avoid wear and tear.
//       Only update the alarm on/off LED if there is a state change.
//       Save the alarm enabled status in the EEPROM and retrieve at startup.
//  v6 - Activate sounds.
//       Modified to get the sounds to work.
//       Use the playMp3Folder function to guarantee which file plays when requested by file number, e.g. not by the order added to the SD card.
//  v7 - Add additional WiFi settings to try to solve occasional connection failures.
//
//       TODO: Make use of EEPROM to snooze status in case of accidental restart.
//       TODO: Modify to only update the time from the NTP server if in the middle of the minute so as not to avoid the time jumping backwards.

#include "WiFiManager.h"          // https://github.com/tzapu/WiFiManager
#include "NTPClient.h"            // https://github.com/arduino-libraries/NTPClient
#include "TM1637Display.h"        // https://github.com/avishorp/TM1637
#include "DFRobotDFPlayerMini.h"  // https://github.com/DFRobot/DFRobotDFPlayerMini
#include "ESP32_WS2812_Lib.h"     // https://github.com/Zhentao-Lin/ESP32_WS2812_Lib
#include <EEPROM.h>
#include <TimeLib.h>  //https://playground.arduino.cc/Code/Time/

bool enableAudio = true;

//========================USEFUL VARIABLES=============================
uint16_t notification_volume = 10;          //Set volume value. From 0 to 30
int clockBrightness = 1;                    // Set displays brightness 0 to 7
int snoozeLengthMinutes = 1;                // Snooze time in minute
int refreshTimeFromNTPIntervalMinutes = 1;  // The minimum time inbetween time syncs from the NTP server.
//=====================================================================

#define DISPLAY_CLK 21
#define DISPLAY_DIO 22
#define SET_STOP_BUTTON 34
#define HOUR_BUTTON 33
#define MINUTE_BUTTON 32
//#define FPSerial Serial1
#define LEDS_PIN 4
#define LEDS_COUNT 24
#define EEPROM_SIZE 12
#define CHANNEL 0
#define UTC_OFFSET 1
#define ALARM_ON_OFF_LED 26

HardwareSerial FPSerial(1); 
const byte RXD2 = 16;  // Connects to module's TX => 16
const byte TXD2 = 17;  // Connects to module's RX => 17

const long utcOffsetInSeconds = 0;  // GMT

DFRobotDFPlayerMini myDFPlayer;
void printDetail(uint8_t type, int value);

int alarmHours = 0;
int alarmMinutes = 0;
int flag_alarm = 0;
int alarm_on_off = 1;
int h = 0;
int audioFinished = 0;

int currentMinutes = 0, currentHours = 0, currentYear = 0, currentMonth = 0, currentDay = 0, previousMinutes = 0;
long epochTimeCurrent = 0;
long epochTimeSnoozed = 0;
long epochTimeNTP = 0;
long epochTimeLocalLastRefreshFromNTP = 0;  //Tracks the local calculated time that the last NTP refresh was attempted.
bool snoozed = false;
unsigned long lastColonToggleTime = 0;
bool colonVisible = true;                         // Start with the colon visible
const unsigned long COLON_FLASH_INTERVAL = 1000;  // Interval for flashing (1000ms)

ESP32_WS2812 pixels = ESP32_WS2812(LEDS_COUNT, LEDS_PIN, CHANNEL, TYPE_GRB);

TM1637Display red1(DISPLAY_CLK, DISPLAY_DIO);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds* UTC_OFFSET);

int timerCount = 5;  // Set the timerCount artificially high so that the first display update happens immediately.
bool skipTimerLogic = false;

hw_timer_t* myTimer = NULL;
// timer interrupt ISR
void IRAM_ATTR onTimer() {
  // Toggle the colon
  colonVisible = !colonVisible;

  // Only update if we have previously retrieved the time
  // and we are not in the middle of processing a button press or
  // other time-sensitive action.
  if (currentYear > 0 && !skipTimerLogic) updateTimeDisplay();

  // Increment the time by 1s.
  epochTimeCurrent += 1;

  // Keep track of how many times we have got here.
  timerCount += 1;
}

void setup() {

  Serial.begin(9600);
  Serial.println("Initialising.");

  pinMode(SET_STOP_BUTTON, INPUT);
  pinMode(HOUR_BUTTON, INPUT);
  pinMode(MINUTE_BUTTON, INPUT);
  pinMode(ALARM_ON_OFF_LED, OUTPUT);
  pinMode(LEDS_PIN, OUTPUT);

  pixels.begin();
  pixels.setBrightness(0);
  pixels.show();

  // Switch off the time display.
  Serial.println("Switching off the time display.");
  red1.setBrightness(0, false);
  red1.showNumberDecEx(0, 0b00000000, true);

  //Init EEPROM
  Serial.println("Retrieving the alarm time and status from EEPROM.");
  EEPROM.begin(EEPROM_SIZE);
  alarmMinutes =  EEPROM.read(0);
  alarmHours =    EEPROM.read(1);
  alarm_on_off =  EEPROM.read(2);

  Serial.print("Alarm time: ");
  Serial.print(alarmHours);
  Serial.print(":");
  Serial.println(alarmMinutes);
  
  Serial.print("Alarm enabled status: ");
  Serial.println(alarm_on_off);

  // Check for out of range values and reset to 00:00 if found.
  if (alarmMinutes > 59 || alarmHours > 23) {
    Serial.println("Out of range value found for alarm time, resetting to 00:00.");
    EEPROM.write(0, 0);
    EEPROM.write(1, 0);
    EEPROM.commit();
    alarmMinutes = 0;
    alarmHours = 0;
    }

  if (alarm_on_off > 1) {
    Serial.println("Out of range value found for alarm on/off, resetting to ON as a precaution.");
    EEPROM.write(2, 1);
    EEPROM.commit();
    alarm_on_off = 1;
  }
 

  Serial.println("Connecting to WiFi.");
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFiManager manager;
  manager.setConnectTimeout(10);
  manager.setConnectRetries(5);
  if (manager.getWiFiIsSaved()) {
    manager.setEnableConfigPortal(false);
    if (!manager.autoConnect("FLUX_CAPACITOR", "password")) {
      manager.setEnableConfigPortal(true);
      manager.setTimeout(60);
      if (!manager.autoConnect("FLUX_CAPACITOR", "password")) {
        Serial.println("Connection failed, restarting...");
        ESP.restart();  // Reset and try again
      }
    }
  }
  Serial.println("Successfully connected to WiFi.");
  delay(3000);

  timeClient.begin();

  // Initialise the audio player.
  if (enableAudio) {
    FPSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);
    Serial.println();
    Serial.println(F("DFRobot DFPlayer Mini Demo"));
    Serial.println(F("Initializing DFPlayer ... (May take 3~5 seconds)"));

    if (!myDFPlayer.begin(FPSerial, /*isACK = */ true, /*doReset = */ true)) {  //Use serial to communicate with mp3.
      Serial.println(F("Unable to begin:"));
      Serial.println(F("1.Please recheck the connection!"));
      Serial.println(F("2.Please insert the SD card!"));
      while (true) {
        delay(0);  // Code to compatible with ESP8266 watch dog.
      }
    }

    Serial.println(F("DFPlayer Mini online."));

    myDFPlayer.setTimeOut(500); //Set serial communictaion time out 500ms

    myDFPlayer.volume(notification_volume);
    myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
    myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
  }


  Serial.println("Getting the time from the NTP server.");
  epochTimeNTP = getEpochTimeFromNTPServer();
  if (epochTimeNTP == 0) {
    Serial.println("Could not retrieve time from NTP server, restarting...");
    ESP.restart();  // Reset and try again
  } else {
    Serial.print("Epoch time from NTP server: ");
    Serial.println(epochTimeNTP);
  }
  epochTimeLocalLastRefreshFromNTP = epochTimeNTP;
  epochTimeCurrent = epochTimeNTP;

  // Define a timer. The timer will be used to toggle the time
  // colon on/off every 1s.
  Serial.println("Defining the 1000ms timer.");
  uint64_t alarmLimit = 1000000;
  myTimer = timerBegin(1000000);  // timer frequency
  timerAttachInterrupt(myTimer, &onTimer);
  timerAlarm(myTimer, alarmLimit, true, 0);


  //Play the first mp3
  myDFPlayer.playMp3Folder(10);
  delay(100);
  audioFinished = 0;

  Serial.println("Flux capacitor startup flash.");
  for (int v = 0; v < 30; v++) {
    // clear the pixels, equivalent to pixels.clear();
    for (int i = 0; i < 24; i++) {
      pixels.setLedColorData(i, 0, 0, 0);
    }

    for (int i = 0; i < 8; i++) {
      pixels.setLedColorData(i, 100, 100, 100);
      pixels.setLedColorData((i + 8), 100, 100, 100);
      pixels.setLedColorData((i + 16), 100, 100, 100);
      delay(10);
      pixels.setBrightness(20 + v);
      pixels.show();
    }
  }

  for (int t = 50; t > -1; t--) {
    pixels.setBrightness(t);
    delay(40);
    pixels.show();
  }

  if (alarm_on_off == 1) {
    digitalWrite(ALARM_ON_OFF_LED, HIGH);
  } else digitalWrite(ALARM_ON_OFF_LED, LOW);

  Serial.println("Starting main loop.");
  if (enableAudio) {
    myDFPlayer.playMp3Folder(14);
  }
}

void loop() {

  //pixels.clear();
  //show_hour();
  h = 1;
  audioFinished = 0;
  pixels.setBrightness(0);

  // Maybe update the time from the NTP server.
  // TODO: Check seconds so as not to set the time backwards if the local timekeeper has advanced too much.
  if (epochTimeCurrent - epochTimeLocalLastRefreshFromNTP > refreshTimeFromNTPIntervalMinutes * 60) {
    Serial.println("Getting the time from the NTP server.");
    epochTimeNTP = 0;
    epochTimeNTP = getEpochTimeFromNTPServer();
    if (epochTimeNTP > 0) {
      epochTimeCurrent = epochTimeNTP;
    }

    if (epochTimeNTP == 0) {
      Serial.println("Could not retrieve time from NTP server, try again next time.");
    } else {
      Serial.print("Epoch time from NTP server: ");
      Serial.println(epochTimeNTP);
    }

    // Store the current time so that we don't get stuck in a loop
    // if the NTP server was unavailable.
    epochTimeLocalLastRefreshFromNTP = epochTimeCurrent;
  }

  // Check to see if the time displays need to be updated.
  maybeUpdateClock();

  if (digitalRead(MINUTE_BUTTON) || digitalRead(HOUR_BUTTON) || digitalRead(SET_STOP_BUTTON)) {
    skipTimerLogic = true;
  } else {
    skipTimerLogic = false;
  }

  if (enableAudio) {
    if (myDFPlayer.available()) {
      printDetail(myDFPlayer.readType(), myDFPlayer.read());  //Print the detail message from DFPlayer to handle different errors and states.
    }
  }

  if (digitalRead(SET_STOP_BUTTON)) {
    Serial.println("Entering alarm setup.");
    Setup_alarm();
    if (enableAudio) myDFPlayer.stop();
    Serial.println("Exit alarm setup.");
  }

  if (alarmHours == currentHours && flag_alarm == 0 && alarm_on_off == 1) {
    if (alarmMinutes == currentMinutes) {
      Serial.println("Triggering the alarm.");
      flag_alarm = 1;
      alarm();
      Serial.println("Exit alarm");
    }
  }

  // if (alarm_on_off == 1) {
  //   digitalWrite(ALARM_ON_OFF_LED, HIGH);
  // } else digitalWrite(ALARM_ON_OFF_LED, LOW);

  if (alarmMinutes != currentMinutes) { flag_alarm = 0; }

  if (digitalRead(MINUTE_BUTTON) && digitalRead(HOUR_BUTTON))  // Push Min and Hour simultaneouslyly to switch ON or OFF the alarm
  {
    if (alarm_on_off == 1) {
      Serial.println("Setting the alarm off.");
      alarm_on_off = 0;
      digitalWrite(ALARM_ON_OFF_LED, LOW);
      snoozed = 0;
    } else {
      Serial.println("Setting the alarm on.");
      alarm_on_off = 1;
      digitalWrite(ALARM_ON_OFF_LED, HIGH);
    }

    Serial.println("Writing new alarm enabled status to EEPROM.");
    EEPROM.write(2, alarm_on_off);
    EEPROM.commit();

    red1.showNumberDecEx(00, 0b00000000, true, 2, 0);
    red1.showNumberDecEx(alarm_on_off, 0b00000000, true, 2, 2);
    delay(1000);

    if (digitalRead(MINUTE_BUTTON))  // Push Min and Hour simultaneously to switch ON or OFF the alarm
    {
      pixels.setBrightness(0);
      if (enableAudio) myDFPlayer.playMp3Folder(13);
      pixels.show();
    }
  }

  // Check whether we are snoozed and the snooze duration has been passed
  if (snoozed && epochTimeCurrent - epochTimeSnoozed >= snoozeLengthMinutes * 60 && alarm_on_off == 1) {
    Serial.println("Snooze duration expired, triggering alarm.");
    alarm();
    Serial.println("Exit alarm");
  }
}


void maybeUpdateClock() {
  if (timerCount > 0 && colonVisible && !digitalRead(MINUTE_BUTTON) && !digitalRead(HOUR_BUTTON) && !digitalRead(SET_STOP_BUTTON)) {

    //Serial.println("Entered time display update...");
    previousMinutes = currentMinutes;
    setTime(epochTimeCurrent);
    currentYear = year();
    currentMonth = month();
    currentDay = day();
    currentHours = hour();
    currentMinutes = minute();
    if (currentYear >= 2025 && previousMinutes != currentMinutes) {
      Serial.println("Updating time display.");
      updateTimeDisplay();
      red1.setBrightness(clockBrightness);
      checkDSTAndSetOffset();
      timerCount = 0;
    }
  }
}

void updateTimeDisplay() {

  // For the time, add the flashing colon logic
  uint8_t hour = currentHours;
  uint8_t minute = currentMinutes;

  if (colonVisible) {
    red1.showNumberDecEx(hour, 0b01000000, true, 2, 0);    // Display hours with colon
    red1.showNumberDecEx(minute, 0b01000000, true, 2, 2);  // Display minutes with colon
  } else {
    red1.showNumberDecEx(hour, 0b00000000, true, 2, 0);    // Display hours without colon
    red1.showNumberDecEx(minute, 0b00000000, true, 2, 2);  // Display minutes without colon
  }
}

void checkDSTAndSetOffset() {
  if (isDST(currentMonth, currentDay, currentYear)) {
    adjustTime(utcOffsetInSeconds + (UTC_OFFSET * 3600));  // Apply DST
    Serial.println("DST active, UTC+1");
  } else {
    adjustTime(utcOffsetInSeconds);  // Standard time
    Serial.println("DST inactive, UTC");
  }
}

void alarm() {
  skipTimerLogic = true;
  snoozed = false;

  Serial.println("Count to 88.");

  // count 0 to 88 m/h
  for (int u = 0; u < 89; u++) {
    delay(12 - (u / 8));
    red1.showNumberDecEx(00, 0b01000000, true, 2, 0);
    red1.showNumberDecEx(u, 0b01000000, true, 2, 2);

    // Flux capacitor acceleration section.
    // Clear the pixels, equivalent to pixels.clear();
    for (int i = 0; i < 24; i++) {
      pixels.setLedColorData(i, 0, 0, 0);
    }

    for (int i = 0; i < 8; i++) {
      pixels.setLedColorData(i, 255, (110 + h), 13 + h);
      pixels.setLedColorData((i + 8), 255, (110 + h), 13 + h);
      pixels.setLedColorData((i + 16), 255, (110 + h), 13 + h);
      delay(12 - (h / 8));
      pixels.setBrightness(20 + h);
      pixels.show();

      if (digitalRead(SET_STOP_BUTTON)) { u = 89; }

      // Check for snooze. If requested then record the snooze request time (so that we can trigger the alarm
      // again at the end of the snooze period), and then exit the loop.
      if (digitalRead(MINUTE_BUTTON) || digitalRead(HOUR_BUTTON))
      {
        Serial.println("Snoozing.");
        snoozed = true;
        epochTimeSnoozed = epochTimeCurrent;
        break;
      }
    }
    
    // If snooze has been requested then exit the loop.
    if (snoozed) break;

    h = h + 1;
  }

  // If we haven't yet snoozed the continue.
  if (!snoozed) {
    //skipTimerLogic = false;
    if (enableAudio) myDFPlayer.playMp3Folder(random(1, 9));  //Playing the alarm sound
    delay(1500);

    colonVisible = true;
    updateTimeDisplay();

    
    while (!digitalRead(SET_STOP_BUTTON) && !snoozed) {
      h = 1;
      maybeUpdateClock();
      //digitalWrite(ALARM_ON_OFF_LED, HIGH);

      //If you're not wake-up at the first song, it plays the next one
      if (enableAudio) {
        if (myDFPlayer.available()) {
          printDetail(myDFPlayer.readType(), myDFPlayer.read());
          if (audioFinished == 1) {
            audioFinished = 0;
            Serial.println("Next song");
            myDFPlayer.playMp3Folder(random(1, 9));  //Playing the alarm sound
          }
        }
      }

      //That's bzzzz the Neopixel
      for (int i = 0; i < 24; i++) {
        pixels.setLedColorData(i, 0, 0, 0);
      }
      for (int i = 0; i < 8; i++) {
        pixels.setLedColorData(i, 255, 200, 105);
        pixels.setLedColorData((i + 8), 255, 200, 105);
        pixels.setLedColorData((i + 16), 255, 200, 105);
        delay(5);
        pixels.setBrightness(110);
        pixels.show();
        //Serial.println("Boucle neopixel");
      }

      // If snooze has been requested then set variables to exit the WHILE loop.
      // Record the snooze request time so that we can trigger the alarm again at the end of the snooze period.
      if (digitalRead(MINUTE_BUTTON) || digitalRead(HOUR_BUTTON))  // Snooze if you push MIN or HOUR button
      {
        Serial.println("Snoozing.");
        snoozed = true;
        epochTimeSnoozed = epochTimeCurrent;
      }
    }
  }

  if (enableAudio) myDFPlayer.stop();
  audioFinished = 0;

  pixels.setBrightness(0);
  pixels.show();
  skipTimerLogic = false;
}

void Setup_alarm() {
  int alarmHoursOriginal = alarmHours;
  int alarmMinutesOriginal = alarmMinutes;

  while (digitalRead(SET_STOP_BUTTON)) {
    if (digitalRead(MINUTE_BUTTON)) {
      alarmMinutes = alarmMinutes + 1;
    }

    if (digitalRead(HOUR_BUTTON)) {
      alarmHours = alarmHours + 1;
    }

    red1.showNumberDecEx(alarmHours, 0b01000000, true, 2, 0);
    red1.showNumberDecEx(alarmMinutes, 0b01000000, true, 2, 2);

    delay(100);

    if (alarmMinutes > 59) { alarmMinutes = 0; }
    if (alarmHours > 23) { alarmHours = 0; }
  }

  // Only write to EEPROM if necessary.
  if (alarmHoursOriginal != alarmHours || alarmMinutesOriginal != alarmMinutes) {
    Serial.println("Saving the alarm time to EEPROM.");
    EEPROM.write(0, alarmMinutes);
    EEPROM.write(1, alarmHours);
    EEPROM.commit();
  }
}

void printDetail(uint8_t type, int value) {
  switch (type) {
    case TimeOut:
      Serial.println(F("Time Out!"));
      break;
    case WrongStack:
      Serial.println(F("Stack Wrong!"));
      break;
    case DFPlayerCardInserted:
      Serial.println(F("Card Inserted!"));
      break;
    case DFPlayerCardRemoved:
      Serial.println(F("Card Removed!"));
      break;
    case DFPlayerCardOnline:
      Serial.println(F("Card Online!"));
      break;
    case DFPlayerUSBInserted:
      Serial.println("USB Inserted!");
      break;
    case DFPlayerUSBRemoved:
      Serial.println("USB Removed!");
      break;
    case DFPlayerPlayFinished:
      Serial.print(F("Number:"));
      Serial.print(value);
      Serial.println(F(" Play Finished!"));
      audioFinished = 1;
      break;
    case DFPlayerError:
      Serial.print(F("DFPlayerError:"));
      switch (value) {
        case Busy:
          Serial.println(F("Card not found"));
          break;
        case Sleeping:
          Serial.println(F("Sleeping"));
          break;
        case SerialWrongStack:
          Serial.println(F("Get Wrong Stack"));
          break;
        case CheckSumNotMatch:
          Serial.println(F("Check Sum Not Match"));
          break;
        case FileIndexOut:
          Serial.println(F("File Index Out of Bound"));
          break;
        case FileMismatch:
          Serial.println(F("Cannot Find File"));
          break;
        case Advertise:
          Serial.println(F("In Advertise"));
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }
}

bool isDST(int month, int day, int year) {
  int lastSundayInMarch = getLastSundayOfMonth(3, year);
  int lastSundayInOctober = getLastSundayOfMonth(10, year);
  int dayOfYear = getDayOfYear(month, day, year);
  return (dayOfYear >= lastSundayInMarch && dayOfYear < lastSundayInOctober);
}

int getDayOfYear(int month, int day, int year) {
  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;  // Leap year adjustment
  }

  int dayOfYear = 0;
  for (int i = 0; i < month - 1; i++) {
    dayOfYear += daysInMonth[i];
  }
  return dayOfYear + day;
}

bool isLeapYear(int year) {
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

int getLastSundayOfMonth(int month, int year) {
  // Array with number of days per month
  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;  // February has 29 days in a leap year
  }
  int lastDay = daysInMonth[month - 1];  // Last day of the month

  // Time structure to hold information about the last day of the month
  tm timeinfo;
  timeinfo.tm_year = year - 1900;  // Year since 1900
  timeinfo.tm_mon = month - 1;     // Month (0-11)
  timeinfo.tm_mday = lastDay;
  timeinfo.tm_hour = 12;  // Set to noon to avoid DST issues
  timeinfo.tm_min = 0;
  timeinfo.tm_sec = 0;
  mktime(&timeinfo);  // Normalize tm struct

  // Get the weekday (0 = Sunday, 1 = Monday, ..., 6 = Saturday)
  int weekday = timeinfo.tm_wday;

  // Calculate the last Sunday of the month
  int lastSunday = lastDay - weekday;
  return getDayOfYear(month, lastSunday, year);  // Convert to day of the year
}

long getEpochTimeFromNTPServer() {
  long epochTime = 0;
  // Get the current time, retry if unsuccessful
  for (int i = 0; i <= 20; i++) {
    timeClient.update();
    epochTime = timeClient.getEpochTime();
    if (epochTime > 0) {
      break;
    }
    delay(50);
  }
  return epochTime;
}
