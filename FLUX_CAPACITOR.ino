
#include "WiFiManager.h"          // https://github.com/tzapu/WiFiManager
#include "NTPClient.h"            // https://github.com/arduino-libraries/NTPClient
#include "TM1637Display.h"        // https://github.com/avishorp/TM1637
#include "DFRobotDFPlayerMini.h"  // https://github.com/DFRobot/DFRobotDFPlayerMini
#include "ESP32_WS2812_Lib.h"
#include <EEPROM.h>


bool enableAudio = false;

//========================USEFUL VARIABLES=============================
int UTC = 1;                        //Set your time zone ex: france = UTC+2, UK = UTC+1
uint16_t notification_volume = 25;  //Set volume value. From 0 to 30
int Display_backlight = 3;          // Set displays brightness 0 to 7;
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

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds *UTC);
TM1637Display red1(DISPLAY_CLK, DISPLAY_DIO);
//Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
ESP32_WS2812 pixels = ESP32_WS2812(LEDS_COUNT, LEDS_PIN, CHANNEL, TYPE_GRB);

void setup() {

  pinMode(SET_STOP_BUTTON, INPUT);
  pinMode(HOUR_BUTTON, INPUT);
  pinMode(MINUTE_BUTTON, INPUT);
  pinMode(26, OUTPUT);
  pinMode(LEDS_PIN, OUTPUT);

  red1.setBrightness(Display_backlight);
  pixels.begin();
  pixels.setBrightness(0);
  pixels.show();
  Serial.begin(9600);

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


  // Refer to this https://randomnerdtutorials.com/esp32-useful-wi-fi-functions-arduino/

  // WiFi.mode(WIFI_STA);
  // WiFi.begin("WLAN4", "MARZIPAN");

  // while ( WiFi.status() != WL_CONNECTED ) {
  //   delay ( 500 );
  //   Serial.print ( "." );
  // }



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
  show_hour();
  h = 1;
  Play_finished = 0;
  pixels.setBrightness(0);

  timeClient.update();
  Serial.print("Time: ");
  Serial.println(timeClient.getFormattedTime());
  unsigned long epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime((time_t *)&epochTime);
  int currentYear = ptm->tm_year + 1900;
  Serial.print("Year: ");
  Serial.println(currentYear);

  int monthDay = ptm->tm_mday;
  Serial.print("Month day: ");
  Serial.println(monthDay);

  int currentMonth = ptm->tm_mon + 1;
  Serial.print("Month: ");
  Serial.println(currentMonth);

  if (enableAudio) {
    if (myDFPlayer.available()) {
      printDetail(myDFPlayer.readType(), myDFPlayer.read());  //Print the detail message from DFPlayer to handle different errors and states.
    }
  }

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

  if (easter_egg == 1) {
    //pixels.clear();
    for (int i = 0; i < 8; i++) {
      pixels.setLedColorData(i, 255, 110, 14);
      pixels.setLedColorData((i + 8), 255, 110, 14);
      pixels.setLedColorData((i + 16), 255, 110, 14);
      delay(20);
      pixels.setBrightness(23);
      pixels.show();
    }
  }

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

  // if((currentMonth*30 + monthDay) >= 121 && (currentMonth*30 + monthDay) < 331){
  // timeClient.setTimeOffset(utcOffsetInSeconds*UTC);} // Change daylight saving time - Summer - change 31/03 at 00:00
  // else {timeClient.setTimeOffset((utcOffsetInSeconds*UTC) - 3600);} // Change daylight saving time - Winter - change 31/10 at 00:00

  // Check if DST is active for the current date
  if (isDST(currentMonth, monthDay, currentYear)) {
    // Summer - DST is in effect (last Sunday of March to last Sunday of October)
    timeClient.setTimeOffset(utcOffsetInSeconds + (UTC * 3600));  // Add 1 hour for DST
    Serial.println("DST is active, UTC+1");
  } else {
    // Winter - DST is not in effect
    timeClient.setTimeOffset(utcOffsetInSeconds);  // Standard time
    Serial.println("DST is not active, UTC");
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
    show_hour();
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

void show_hour() {
  timeClient.update();
  red1.showNumberDecEx(timeClient.getHours(), 0b01000000, true, 2, 0);
  red1.showNumberDecEx(timeClient.getMinutes(), 0b01000000, true, 2, 2);
}

// Snooze if you'd like to sleep few minutes more
// You can set the snooze time with the "snooze" variable at the beginning of the code
void Snooze() {

    if (enableAudio) myDFPlayer.stop();
  pixels.setBrightness(5);
  pixels.show();

  for (int i = 0; i < (snooze * 600); i++) {
    waitMilliseconds(68);
    show_hour();
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



// Function to check if the year is a leap year
bool isLeapYear(int year) {
  // Leap year if divisible by 4, but not divisible by 100 unless also divisible by 400
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

// Function to calculate the day of the year (1 = Jan 1st, 365 = Dec 31st)
int getDayOfYear(int currentMonth, int monthDay, int year) {
  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };  // Normal year

  // Adjust February for leap year
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;  // February has 29 days in a leap year
  }

  int dayOfYear = 0;

  // Add the days of the previous months
  for (int i = 0; i < currentMonth - 1; i++) {
    dayOfYear += daysInMonth[i];
  }

  // Add the current month's days
  dayOfYear += monthDay;

  return dayOfYear;
}

// Function to calculate if a given year has daylight saving time (DST)
bool isDST(int currentMonth, int monthDay, int year) {
  // Get the last Sunday in March (DST starts in the UK)
  int lastSundayInMarch = getLastSundayOfMonth(3, year);

  // Get the last Sunday in October (DST ends in the UK)
  int lastSundayInOctober = getLastSundayOfMonth(10, year);

  // Check if the current date is between the last Sunday in March and the last Sunday in October
  int dayOfYear = getDayOfYear(currentMonth, monthDay, year);

  // If we're between March and October, DST is in effect
  return dayOfYear >= lastSundayInMarch && dayOfYear <= lastSundayInOctober;
}

// Function to calculate the last Sunday of a given month in a given year
int getLastSundayOfMonth(int month, int year) {
  // Get the last day of the month (e.g., 31 for March, 30 for April, etc.)
  int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;  // February has 29 days in a leap year
  }
  int lastDay = daysInMonth[month - 1];

  // Find the day of the week for the last day of the month
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
