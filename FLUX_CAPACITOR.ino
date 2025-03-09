//  v2 - Ported code from BTTF logo & clock unit

#include "WiFiManager.h"          // https://github.com/tzapu/WiFiManager
#include "NTPClient.h"            // https://github.com/arduino-libraries/NTPClient
#include "TM1637Display.h"        // https://github.com/avishorp/TM1637
#include "DFRobotDFPlayerMini.h"  // https://github.com/DFRobot/DFRobotDFPlayerMini
#include "ESP32_WS2812_Lib.h"
#include <EEPROM.h>
#include <TimeLib.h>  //https://playground.arduino.cc/Code/Time/


bool enableAudio = false;

//========================USEFUL VARIABLES=============================
uint16_t notification_volume = 25;  //Set volume value. From 0 to 30
int clockBrightness = 2;            // Set displays brightness 0 to 7;
int snooze = 5;                     // Snooze time in minute
//=====================================================================

#define DISPLAY_CLK 21
#define DISPLAY_DIO 22
#define SET_STOP_BUTTON 34
#define HOUR_BUTTON 33
#define MINUTE_BUTTON 32
#define FPSerial Serial1
#define LEDS_PIN 4
#define LEDS_COUNT 24
#define EEPROM_SIZE 12
#define CHANNEL 0

#define UTC_OFFSET 1


const byte RXD2 = 16;  // Connects to module's TX => 16
const byte TXD2 = 17;  // Connects to module's RX => 17

DFRobotDFPlayerMini myDFPlayer;
void printDetail(uint8_t type, int value);

float counter = 0;
int hours = 0;
int minutes = 0;
int flag_alarm = 0;
int alarm_on_off = 1;
int h = 0;
const long utcOffsetInSeconds = 0;  // GMT
int Play_finished = 0;
int easter_egg = 0;
bool res;

int currentMinutes = 0, currentHours = 0, currentYear = 0, currentMonth = 0, monthDay = 0, previousMinutes = 0;
long epochTime = 0;
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
  // and we are not in the middle of processing a button press.
  if (currentYear > 0 && !skipTimerLogic) updateTimeDisplay();

  epochTime += 1;

  // Keep track of how many times we have got here.
  timerCount += 1;
}

void setup() {

  Serial.begin(9600);

  pinMode(SET_STOP_BUTTON, INPUT);
  pinMode(HOUR_BUTTON, INPUT);
  pinMode(MINUTE_BUTTON, INPUT);
  pinMode(26, OUTPUT);
  pinMode(LEDS_PIN, OUTPUT);

  pixels.begin();
  pixels.setBrightness(0);
  pixels.show();

  //Init EEPROM
  EEPROM.begin(EEPROM_SIZE);
  minutes = EEPROM.read(0);
  hours = EEPROM.read(1);

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
  delay(3000);

  timeClient.begin();

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

    myDFPlayer.volume(notification_volume);
    myDFPlayer.play(10);  //Play the first mp3
    delay(100);
    Play_finished = 0;
  }

  // Get the current time, retry if unsuccessful
  for (int i = 0; i <= 20; i++) {
    timeClient.update();
    epochTime = timeClient.getEpochTime();
    if (epochTime > 0) break;
    Serial.print("Could not retrieve time from NTP server...");
    delay(50);
  }

  if (epochTime == 0) ESP.restart();  // Reset and try again

  // Define a timer. The timer will be used to toggle the time
  // colon on/off every 1s.
  uint64_t alarmLimit = 1000000;
  myTimer = timerBegin(1000000);  // timer frequency
  timerAttachInterrupt(myTimer, &onTimer);
  timerAlarm(myTimer, alarmLimit, true, 0);

  for (int v = 0; v < 30; v++) {
    //pixels.clear();
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

  Serial.println("\n Starting");
  if (enableAudio) {
    myDFPlayer.play(14);
  }
}

void loop() {

  //pixels.clear();
  //show_hour();
  h = 1;
  Play_finished = 0;
  pixels.setBrightness(0);

  // Check to see if the time displays need to be updated.
  if (timerCount > 0 && colonVisible) {
    previousMinutes = currentMinutes;
    setTime(epochTime);
    currentYear = year();
    currentMonth = month();
    monthDay = day();
    currentHours = hour();
    currentMinutes = minute();
    if (currentYear >= 2025 && previousMinutes != currentMinutes) {
      updateTimeDisplay();
      red1.setBrightness(clockBrightness);
      checkDSTAndSetOffset();
      timerCount = 0;
    }
  }

  // if (enableAudio) {
  //   if (myDFPlayer.available()) {
  //     printDetail(myDFPlayer.readType(), myDFPlayer.read());  //Print the detail message from DFPlayer to handle different errors and states.
  //   }
  // }

  if (digitalRead(SET_STOP_BUTTON) == true) {
    Setup_alarm();
    if (enableAudio) myDFPlayer.stop();
    digitalWrite(26, LOW);
  }

  if (hours == timeClient.getHours() && flag_alarm == 0 && alarm_on_off == 1) {
    if (minutes == timeClient.getMinutes()) {
      flag_alarm = 1;
      alarm();
      Serial.println("Sortie alarm");
    }
  }

  if (alarm_on_off == 1) {
    digitalWrite(26, HIGH);
  } else digitalWrite(26, LOW);

  if (minutes != timeClient.getMinutes()) { flag_alarm = 0; }

  // if (easter_egg == 1) {
  //   //pixels.clear();
  //   for (int i = 0; i < 8; i++) {
  //     pixels.setLedColorData(i, 255, 110, 14);
  //     pixels.setLedColorData((i + 8), 255, 110, 14);
  //     pixels.setLedColorData((i + 16), 255, 110, 14);
  //     delay(20);
  //     pixels.setBrightness(23);
  //     pixels.show();
  //   }
  // }

  if (digitalRead(MINUTE_BUTTON) == true && digitalRead(HOUR_BUTTON) == true)  // Push Min and Hour simultanetly to switch ON or OFF the alarm
  {
    if (alarm_on_off == 1) {
      alarm_on_off = 0;
    } else alarm_on_off = 1;

    red1.showNumberDecEx(00, 0b01000000, true, 2, 0);
    red1.showNumberDecEx(alarm_on_off, 0b01000000, true, 2, 2);
    delay(1000);

    if (digitalRead(MINUTE_BUTTON) == true)  // Push Min and Hour simultanetly to switch ON or OFF the alarm
    {
      if (easter_egg == 0) {
        easter_egg = 1;
        if (enableAudio) myDFPlayer.play(12);
      } else {
        easter_egg = 0;
        pixels.setBrightness(0);
        if (enableAudio) myDFPlayer.play(13);
        pixels.show();
      }
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
  if (isDST(currentMonth, monthDay, currentYear)) {
    adjustTime(utcOffsetInSeconds + (UTC_OFFSET * 3600));  // Apply DST
    Serial.println("DST active, UTC+1");
  } else {
    adjustTime(utcOffsetInSeconds);  // Standard time
    Serial.println("DST inactive, UTC");
  }
}

void alarm() {
  // count 0 to 88 m/h
  for (int u = 0; u < 89; u++) {
    delay(12 - (u / 8));
    red1.showNumberDecEx(00, 0b01000000, true, 2, 0);
    red1.showNumberDecEx(u, 0b01000000, true, 2, 2);

    //Flux capacitor acceleration
    //pixels.clear();
    for (int i = 0; i < 8; i++) {
      pixels.setLedColorData(i, 255, (110 + h), 13 + h);
      pixels.setLedColorData((i + 8), 255, (110 + h), 13 + h);
      pixels.setLedColorData((i + 16), 255, (110 + h), 13 + h);
      delay(12 - (h / 8));
      pixels.setBrightness(20 + h);
      pixels.show();

      if (digitalRead(SET_STOP_BUTTON) == true) { u = 89; }

      if (digitalRead(MINUTE_BUTTON) == true || digitalRead(HOUR_BUTTON) == true)  // Snooze if you push MIN or HOUR button
      {
        Snooze();
      }
    }
    h = h + 1;
  }

  if (enableAudio) myDFPlayer.play(random(1, 9));  //Playing the alarm sound
  delay(1500);


  while (digitalRead(SET_STOP_BUTTON) == false) {
    h = 1;
    updateTimeDisplay();
    // Serial.println("dans la boucle");
    digitalWrite(26, HIGH);

    // If you're not wake-up at the first song, it plays the next one
    if (enableAudio) {
      if (myDFPlayer.available()) {
        printDetail(myDFPlayer.readType(), myDFPlayer.read());
        if (Play_finished == 1) {
          Play_finished = 0;
          Serial.println("Next song");
          myDFPlayer.play(random(1, 9));  //Playing the alarm sound
        }
      }
    }

    //That's bzzzz the Neopixel
    //pixels.clear();
    for (int i = 0; i < 8; i++) {
      pixels.setLedColorData(i, 255, 200, 105);
      pixels.setLedColorData((i + 8), 255, 200, 105);
      pixels.setLedColorData((i + 16), 255, 200, 105);
      delay(5);
      pixels.setBrightness(110);
      pixels.show();
      //Serial.println("Boucle neopixel");
    }

    if (digitalRead(MINUTE_BUTTON) == true || digitalRead(HOUR_BUTTON) == true)  // Snooze if you push MIN or HOUR button
    {
      Snooze();
    }
  }
  pixels.setBrightness(0);
  pixels.show();
}


// Snooze if you'd like to sleep few minutes more
// You can set the snooze time with the "snooze" variable at the beginning of the code
void Snooze() {

  if (enableAudio) myDFPlayer.stop();
  pixels.setBrightness(5);
  pixels.show();

  for (int i = 0; i < (snooze * 600); i++) {
    waitMilliseconds(68);
    updateTimeDisplay();
    Serial.println("snooze");
    Serial.println(i);
    if (digitalRead(SET_STOP_BUTTON) == true) { i = snooze * 600; }
  }

  alarm();
  Play_finished = 0;
}

void Setup_alarm() {

  while (digitalRead(SET_STOP_BUTTON) == true) {
    if (digitalRead(MINUTE_BUTTON) == true) {
      minutes = minutes + 1;
    }

    if (digitalRead(HOUR_BUTTON) == true) {
      hours = hours + 1;
    }

    red1.showNumberDecEx(hours, 0b01000000, true, 2, 0);
    red1.showNumberDecEx(minutes, 0b01000000, true, 2, 2);

    delay(100);

    if (minutes > 59) { minutes = 0; }
    if (hours > 23) { hours = 0; }
  }
  EEPROM.write(0, minutes);
  EEPROM.write(1, hours);
  EEPROM.commit();
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
      Play_finished = 1;
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

void waitMilliseconds(uint16_t msWait) {
  uint32_t start = millis();

  while ((millis() - start) < msWait) {
    // calling mp3.loop() periodically allows for notifications
    // to be handled without interrupts
    delay(1);
  }
}

bool isDST(int currentMonth, int monthDay, int currentYear) {
  int lastSundayInMarch = getLastSundayOfMonth(3, currentYear);
  int lastSundayInOctober = getLastSundayOfMonth(10, currentYear);
  int dayOfYear = getDayOfYear(currentMonth, monthDay, currentYear);
  return (dayOfYear >= lastSundayInMarch && dayOfYear < lastSundayInOctober);
}

int getDayOfYear(int currentMonth, int monthDay, int year) {
  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;  // Leap year adjustment
  }

  int dayOfYear = 0;
  for (int i = 0; i < currentMonth - 1; i++) {
    dayOfYear += daysInMonth[i];
  }
  return dayOfYear + monthDay;
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
