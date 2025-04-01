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
//  v8 - Reduced volume.
//       Changed EQ setting to add more bass.
//       Increased snooze length.
//  V9 - Amended actions taken when combinations of buttons are pressed.
//          1. Hour pressed and held, then set/stop button pressed toggles alarm status.
//          2. Hour pressed and held, then minute button pressed increments volume level, loops around to level 1.
//          3. Minute pressed and held, then hour buttom pressed increments display brightness, loops around to level 1.
//  v10 - Pause the timer logic when the set/stop button is pressed.
//  v11 - Made the date display format configurable - supports MMDD or DDMM.
//      - Optimised variable types.
//      - Allowed hard-coded WiFi credentials. If not specified then the config portal will launch.
//      - Removed enableAudio variable, it was only used during testing when waiting for audio board to arrive.
//  v12 - Prevent a minutes value of 60 or an hours value of 24 being shown briefly when setting the alarm.
//      - Added command to explicitly stop audio before playing next random file during snooze to prevent odd transition sounds.
//  v13 - Ensure all MP3 files are played at least once without repetition in a given alarm activation.
//      - Added additional key combination to manually trigger the alarm:
//          Minute pressed and held, then set/stop button pressed.
//      - Don't trigger the alarm when setting the alarm.
//      - Enhanced alarm cancel behaviour.
//      - Added various Serial.print statements.
//  v14 - Allowed volume increase past MP3 level 15.
//      - Volume now scaled. Only allows setting of 1-10, but these are multiplied by 3 when setting the volume level, 10 * 3 = 30 which is the max.
//  v15 - Ported latest time keeping code from BTTF logo & clock unit.
//
//       TODO: Make use of EEPROM to snooze status in case of accidental restart.
//       TODO: Modify to only update the time from the NTP server if in the middle of the minute so as not to avoid the time jumping backwards.
//       TODO: Port timekeeping code from BTTF clock project.

#include <WiFiManager.h>          // https://github.com/tzapu/WiFiManager
#include <TM1637Display.h>        // https://github.com/avishorp/TM1637
#include <ESP32_WS2812_Lib.h>     // https://github.com/Zhentao-Lin/ESP32_WS2812_Lib
#include <DFRobotDFPlayerMini.h>  // https://github.com/DFRobot/DFRobotDFPlayerMini
#include <EEPROM.h>

#define DISPLAY_CLK 21
#define DISPLAY_DIO 22
#define SET_STOP_BUTTON 34
#define HOUR_BUTTON 33
#define MINUTE_BUTTON 32
#define LEDS_PIN 4
#define LEDS_COUNT 24
#define EEPROM_SIZE 5
#define CHANNEL 0
#define ALARM_ON_OFF_LED 26
#define EEPROM_ALARM_MINUTES 0
#define EEPROM_ALARM_HOURS 1
#define EEPROM_ALARM_STATE 2
#define EEPROM_NOTIFICATION_VOLUME 3
#define EEPROM_CLOCK_BRIGHTNESS 4

//========================USEFUL VARIABLES=============================
const char* ntpServer = "pool.ntp.org";
String timezone = "GMT0BST,M3.5.0/1,M10.5.0";  // https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
int refreshTimeFromNTPIntervalSeconds = 1800;  // The minimum time between time syncs with the NTP server.
int snoozeLengthMinutes = 5;                   // Snooze time in minutes
const char* ssid = "";                         // YOUR SSID HERE - leave empty to use WiFi management portal
const char* password = "";                     // WIFI PASSWORD HERE
//=====================================================================

bool verboseDebug = false;
byte clockBrightness;
struct tm timeinfo;

byte currentSeconds = 0, currentMinutes = 0, currentHours = 0, previousMinutes = 0;
int currentYear = 0;
unsigned long epochTimeSnoozed = 0;
unsigned long epochTimeIncremented = 0;
unsigned long epochTime = 0;
unsigned long epochTimeLocalLastRefreshFromNTP = 0;  //Tracks the local calculated time that the last NTP refresh was attempted.
unsigned long lastColonToggleTime = 0;
bool colonVisible = true;                         // Start with the colon visible
const unsigned long COLON_FLASH_INTERVAL = 1000;  // Interval for flashing (1000ms)

ESP32_WS2812 pixels = ESP32_WS2812(LEDS_COUNT, LEDS_PIN, CHANNEL, TYPE_GRB);

TM1637Display red1(DISPLAY_CLK, DISPLAY_DIO);

WiFiUDP ntpUDP;

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

  epochTimeIncremented += 1;

  // Keep track of how many times we have got here.
  timerCount += 1;
}

HardwareSerial FPSerial(1);
const byte RXD2 = 16;  // Connects to module's TX => 16
const byte TXD2 = 17;  // Connects to module's RX => 17

DFRobotDFPlayerMini myDFPlayer;
void printDetail(uint8_t type, int value);

byte alarmHours = 0, alarmMinutes = 0;
int flag_alarm = 0;
int alarm_on_off = 1;
bool snoozed = false;

byte notification_volume;
byte audioFinished = 0;

byte mp3PlayOrder[9];

void setup() {

  Serial.begin(9600);  // Start Serial for Debugging
  Serial.println("Initialising.");

  // Pin initialization
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
  Serial.println("Retrieving the saved setting values from EEPROM.");
  EEPROM.begin(EEPROM_SIZE);
  alarmMinutes = EEPROM.read(EEPROM_ALARM_MINUTES);
  alarmHours = EEPROM.read(EEPROM_ALARM_HOURS);
  alarm_on_off = EEPROM.read(EEPROM_ALARM_STATE);
  notification_volume = EEPROM.read(EEPROM_NOTIFICATION_VOLUME);
  clockBrightness = EEPROM.read(EEPROM_CLOCK_BRIGHTNESS);

  Serial.print("Alarm time: ");
  Serial.print(alarmHours);
  Serial.print(":");
  Serial.println(alarmMinutes);

  Serial.print("Alarm enabled status: ");
  Serial.println(alarm_on_off);

  Serial.print("Notification volume: ");
  Serial.println(notification_volume);

  Serial.print("Clock brightness: ");
  Serial.println(clockBrightness);

  // Check for out of range values and reset to 00:00 if found.
  if (alarmMinutes > 59 || alarmHours > 23) {
    Serial.println("Out of range value found for alarm time, resetting to 00:00.");
    alarmMinutes = 0;
    alarmHours = 0;
    EEPROM.write(EEPROM_ALARM_MINUTES, alarmMinutes);
    EEPROM.write(EEPROM_ALARM_HOURS, alarmHours);
    EEPROM.commit();
  }

  if (alarm_on_off > 1) {
    Serial.println("Out of range value found for alarm on/off, resetting to ON as a precaution.");
    alarm_on_off = 1;
    EEPROM.write(EEPROM_ALARM_STATE, alarm_on_off);
    EEPROM.commit();
  }

  if (notification_volume < 1 || notification_volume > 10) {
    Serial.println("Out of range value found for notification volume, resetting to low level as a precaution.");
    notification_volume = 2;
    EEPROM.write(EEPROM_NOTIFICATION_VOLUME, notification_volume);
    EEPROM.commit();
  }

  if (clockBrightness > 3) {
    Serial.println("Out of range value found for clock brightness, resetting to level 2 as a precaution.");
    clockBrightness = 2;
    EEPROM.write(EEPROM_CLOCK_BRIGHTNESS, clockBrightness);
    EEPROM.commit();
  }

  // Connect to WiFi. Attempt to connect to saved details first.
  Serial.println("Connecting to WiFi.");
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);

  if (strlen(ssid) == 0) {
    Serial.println("Using credentials saved via portal.");
    WiFiManager manager;
    manager.setConnectTimeout(10);
    manager.setConnectRetries(5);
    if (manager.getWiFiIsSaved()) {
      manager.setEnableConfigPortal(false);
      if (!manager.autoConnect("FLUX_CAPACITOR", "password")) {
        manager.setEnableConfigPortal(true);
        manager.setTimeout(180);
        if (!manager.autoConnect("FLUX_CAPACITOR", "password")) {
          Serial.println("Connection failed, restarting...");
          ESP.restart();  // Reset and try again
        }
      }
    }
  } else {
    Serial.println("Using hard-coded credentials.");
    WiFi.begin(ssid, password);
  }

  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("Successfully connected to WiFi.");

  configTime(0, 0, ntpServer);

  epochTime = getTime();

  if (epochTime == 0) {
    Serial.println("Could not retrieve time from NTP server, restarting...");
    ESP.restart();  // Reset and try again
  } else {
    Serial.print("Epoch time from NTP server: ");
    Serial.println(epochTime);
    setTimezone(timezone);
  }

  epochTimeLocalLastRefreshFromNTP = epochTime;
  epochTimeIncremented = epochTime;

  // Initialise the audio player.
  FPSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);
  Serial.println();
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

  myDFPlayer.setTimeOut(500);  //Set serial communictaion time out 500ms
  myDFPlayer.volume(notification_volume);
  myDFPlayer.EQ(DFPLAYER_EQ_BASS);
  myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);

  randomSeed(analogRead(0));  // Seed randomness

  // Initialize the MP3 play order array with numbers 1-9
  for (int i = 0; i < 9; i++) {
    mp3PlayOrder[i] = i + 1;
  }

  // Define a timer. The timer will be used to toggle the time
  // colon on/off every 1s.
  Serial.println("Defining the 1000ms timer.");
  uint64_t alarmLimit = 1000000;
  myTimer = timerBegin(1000000);  // timer frequency
  timerAttachInterrupt(myTimer, &onTimer);
  timerAlarm(myTimer, alarmLimit, true, 0);

  // Play the startup mp3
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
  myDFPlayer.playMp3Folder(14);
}

void loop() {

  audioFinished = 0;
  pixels.setBrightness(0);

  if (!snoozed) {
    if (currentSeconds >= 30 && epochTimeIncremented - epochTimeLocalLastRefreshFromNTP >= refreshTimeFromNTPIntervalSeconds) {
      Serial.println("Time to resync with NTP server.");
      epochTimeLocalLastRefreshFromNTP = epochTimeIncremented;
      reconnectWiFi();
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Getting local time from NTP server.");
    } else if (verboseDebug) {
      Serial.println("Getting local time from onboard RTC.");
    }

    epochTime = getTime();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Disconnecting from WiFi.");
      WiFi.disconnect();
      delay(1000);
    }

    if (verboseDebug) Serial.print("Local time from onboard RTC is: ");
    if (verboseDebug) Serial.println(epochTime);

    if (epochTime == 0) {
      Serial.println("Could not get local time from onboard RTC, setting onboard RTC from incremented epoch time.");
      setTime(epochTimeIncremented);
      Serial.println("Getting local time from onboard RTC after setting from incremented epoch time.");
      epochTime = getTime();
      Serial.print("Local time from onboard RTC after setting from incremented epoch time is: ");
      Serial.println(epochTime);
    }

    if (epochTime == 0) {
      Serial.println("Could not retrieve local time, reconnecting to WiFi to sync from NTP server");
      reconnectWiFi();
    } else {
      if (verboseDebug) Serial.print("Epoch time from onboard RTC: ");
      if (verboseDebug) Serial.println(epochTime);
      epochTimeIncremented = epochTime;
    }
  }

  // Check to see if the time displays need to be updated.
  maybeUpdateClock();

  if (myDFPlayer.available()) {
    printDetail(myDFPlayer.readType(), myDFPlayer.read());  //Print the detail message from DFPlayer to handle different errors and states.
  }

  if (digitalRead(SET_STOP_BUTTON)) {
    skipTimerLogic = true;
    Serial.println("Entering alarm setup.");
    Setup_alarm();
    myDFPlayer.stop();
    Serial.println("Exit alarm setup.");
    skipTimerLogic = false;
  }

  if (alarmHours == currentHours && flag_alarm == 0 && alarm_on_off == 1 && !digitalRead(SET_STOP_BUTTON)) {
    if (alarmMinutes == currentMinutes) {
      Serial.println("Triggering the alarm.");
      flag_alarm = 1;
      alarm();
      Serial.println("Exit alarm");
    }
  }

  if (alarmMinutes != currentMinutes) { flag_alarm = 0; }

  // Push and hold Hour button and then and press set/stop button to switch ON or OFF the alarm
  if (digitalRead(HOUR_BUTTON) && !digitalRead(SET_STOP_BUTTON) && !digitalRead(MINUTE_BUTTON)) {
    // Disable the colon update for the duration of the button handler
    skipTimerLogic = true;
    while (digitalRead(HOUR_BUTTON)) {

      if (digitalRead(SET_STOP_BUTTON)) {
        if (alarm_on_off == 1) {
          Serial.println("Setting the alarm off.");
          alarm_on_off = 0;
          digitalWrite(ALARM_ON_OFF_LED, LOW);
          if (snoozed) Serial.println("Cancelling snooze.");
          snoozed = false;
        } else {
          Serial.println("Setting the alarm on.");
          alarm_on_off = 1;
          digitalWrite(ALARM_ON_OFF_LED, HIGH);
        }

        Serial.println("Writing new alarm enabled status to EEPROM.");
        EEPROM.write(EEPROM_ALARM_STATE, alarm_on_off);
        EEPROM.commit();

        red1.showNumberDecEx(00, 0b00000000, true, 2, 0);
        red1.showNumberDecEx(alarm_on_off, 0b00000000, true, 2, 2);
        delay(750);

        if (digitalRead(MINUTE_BUTTON)) {
          pixels.setBrightness(0);
          myDFPlayer.playMp3Folder(13);
          pixels.show();
        }
      }

      if (digitalRead(MINUTE_BUTTON)) {
        notification_volume = notification_volume + 1;
        if (notification_volume > 10) {
          notification_volume = 1;
        }

        Serial.println("Writing new notification volume level to EEPROM.");
        EEPROM.write(EEPROM_NOTIFICATION_VOLUME, notification_volume);
        EEPROM.commit();

        myDFPlayer.volume(notification_volume * 3);
        red1.showNumberDecEx(00, 0b00000000, true, 2, 0);
        red1.showNumberDecEx(notification_volume, 0b00000000, true, 2, 2);
        myDFPlayer.playMp3Folder(14);
        delay(750);
      }
    }
    skipTimerLogic = false;
  }

  // Push and hold Minute button and then press Hour button to increment the clock brightness
  if (digitalRead(MINUTE_BUTTON) && !digitalRead(HOUR_BUTTON)) {
    // Disable the colon update for the duration of the button handler
    skipTimerLogic = true;
    while (digitalRead(MINUTE_BUTTON)) {
      if (digitalRead(HOUR_BUTTON)) {
        clockBrightness = (clockBrightness + 1) % 4;  // Cycle through 0-4

        Serial.println("Writing new clock brightness level to EEPROM.");
        EEPROM.write(EEPROM_CLOCK_BRIGHTNESS, clockBrightness);
        EEPROM.commit();

        red1.setBrightness(clockBrightness);

        red1.showNumberDecEx(00, 0b00000000, true, 2, 0);
        red1.showNumberDecEx(clockBrightness + 1, 0b00000000, true, 2, 2);
        delay(750);  // Delay to allow display showing the new brightness level to be seen
      }

      if (digitalRead(SET_STOP_BUTTON)) {
        Serial.println("Triggering alarm manually.");
        delay(1000);
        alarm();
      }
    }
    skipTimerLogic = false;
  }

  // Check whether we are snoozed and the snooze duration has been passed
  if (snoozed && epochTimeIncremented - epochTimeSnoozed >= snoozeLengthMinutes * 60 && alarm_on_off == 1) {
    Serial.println("Snooze duration expired, triggering alarm.");
    alarm();
    Serial.println("Exit alarm");
  }
}

void reconnectWiFi() {
  int wifiConnectLoopCounter = 0;

  if (WiFi.status() == WL_CONNECTED) { WiFi.disconnect(); }
  Serial.print("Reconnecting to WiFi.");
  WiFi.reconnect();
  while (WiFi.status() != WL_CONNECTED && wifiConnectLoopCounter <= 50) {
    delay(100);
    Serial.print(".");
    wifiConnectLoopCounter += 1;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("Successfully reconnected to WiFi.");
  } else {
    Serial.println("");
    Serial.println("Timeout when connecting to WiFi.");
    WiFi.disconnect();
  }
}

void maybeUpdateClock() {
  if (timerCount > 0 && colonVisible && !digitalRead(MINUTE_BUTTON) && !digitalRead(HOUR_BUTTON) && !digitalRead(SET_STOP_BUTTON)) {

    //Serial.println("Entered time display update...");
    previousMinutes = currentMinutes;
    epochTime = getTime();
    currentYear = timeinfo.tm_year + 1900;
    currentHours = timeinfo.tm_hour;
    currentMinutes = timeinfo.tm_min;
    currentSeconds = timeinfo.tm_sec;

    if (currentYear >= 2025 && previousMinutes != currentMinutes) {
      Serial.println("Updating time display.");
      updateTimeDisplay();
      red1.setBrightness(clockBrightness);
      timerCount = 0;
    }
  }
}

void updateTimeDisplay() {
  // For the time, add the flashing colon logic
  if (colonVisible) {
    red1.showNumberDecEx(currentHours, 0b01000000, true, 2, 0);    // Display hours with colon
    red1.showNumberDecEx(currentMinutes, 0b01000000, true, 2, 2);  // Display minutes with colon
  } else {
    red1.showNumberDecEx(currentHours, 0b00000000, true, 2, 0);    // Display hours without colon
    red1.showNumberDecEx(currentMinutes, 0b00000000, true, 2, 2);  // Display minutes without colon
  }
}

void alarm() {
  byte h = 1;
  boolean cancelAlarm = false;
  skipTimerLogic = true;
  snoozed = false;
  byte mp3PlayOrderIndex = 0;
  shuffleMP3OrderArray();

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

      if (digitalRead(SET_STOP_BUTTON)) {
        u = 89;
        Serial.println("Alarm cancelled.");
        cancelAlarm = true;
      }

      // Check for snooze. If requested then record the snooze request time (so that we can trigger the alarm
      // again at the end of the snooze period), and then exit the loop.
      if (digitalRead(MINUTE_BUTTON) || digitalRead(HOUR_BUTTON)) {
        Serial.println("Snoozing.");
        snoozed = true;
        epochTimeSnoozed = epochTimeIncremented;
        break;
      }
    }

    // If snooze has been requested then exit the loop.
    if (snoozed) break;

    h = h + 1;
  }

  // If we haven't yet snoozed then continue.
  if (!snoozed && !cancelAlarm) {

    Serial.print("Playing first MP3, number: ");
    Serial.println(mp3PlayOrder[mp3PlayOrderIndex]);
    myDFPlayer.playMp3Folder(mp3PlayOrder[mp3PlayOrderIndex]);  //Playing the alarm sound
    delay(1500);

    colonVisible = true;
    updateTimeDisplay();

    while (!cancelAlarm && !snoozed) {
      maybeUpdateClock();

      //If you're not wake-up at the first song, it plays the next one
      if (myDFPlayer.available()) {
        printDetail(myDFPlayer.readType(), myDFPlayer.read());
        if (audioFinished == 1) {
          audioFinished = 0;
          myDFPlayer.stop();
          // Increment the play order index to select the next song
          mp3PlayOrderIndex += 1;
          // If we have exceeded the array bound then reset and reshuffle the array.
          if (mp3PlayOrderIndex > 8) {
            mp3PlayOrderIndex = 0;
            shuffleMP3OrderArray();
          }
          Serial.print("Playing next MP3, number: ");
          Serial.println(mp3PlayOrder[mp3PlayOrderIndex]);
          myDFPlayer.playMp3Folder(mp3PlayOrder[mp3PlayOrderIndex]);  //Playing the alarm sound
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
      }

      // If snooze has been requested then set variables to exit the WHILE loop.
      // Record the snooze request time so that we can trigger the alarm again at the end of the snooze period.
      if (digitalRead(MINUTE_BUTTON) || digitalRead(HOUR_BUTTON))  // Snooze if you push MIN or HOUR button
      {
        Serial.println("Snoozing.");
        snoozed = true;
        epochTimeSnoozed = epochTimeIncremented;
      }

      cancelAlarm = digitalRead(SET_STOP_BUTTON);
      if (cancelAlarm) Serial.println("Alarm cancelled.");
    }
  }

  myDFPlayer.stop();
  audioFinished = 0;

  pixels.setBrightness(0);
  pixels.show();

  // Prevent the alarm time from being displayed when the set/stop button is pressed
  // to cancel the alarm. The delay just prevents the main loop from being returned-to immediately
  // and the set/stop button handler from being entered.
  delay(200);

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

    if (alarmMinutes > 59) { alarmMinutes = 0; }
    if (alarmHours > 23) { alarmHours = 0; }

    red1.showNumberDecEx(alarmHours, 0b01000000, true, 2, 0);
    red1.showNumberDecEx(alarmMinutes, 0b01000000, true, 2, 2);

    delay(100);
  }

  // Only write to EEPROM if necessary.
  if (alarmHoursOriginal != alarmHours || alarmMinutesOriginal != alarmMinutes) {
    Serial.println("Saving the alarm time to EEPROM.");
    EEPROM.write(EEPROM_ALARM_MINUTES, alarmMinutes);
    EEPROM.write(EEPROM_ALARM_HOURS, alarmHours);
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

// Function to shuffle the array using Fisher-Yates algorithm
void shuffleMP3OrderArray() {
  for (int i = 8; i > 0; i--) {
    int j = random(0, i + 1);
    int temp = mp3PlayOrder[i];
    mp3PlayOrder[i] = mp3PlayOrder[j];
    mp3PlayOrder[j] = temp;
  }
}

// Function that gets current epoch time
unsigned long getTime() {
  time_t now;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return (0);
  }
  time(&now);
  return now;
}

void setTime(int epoch) {
  struct timeval now = { .tv_sec = epoch };

  Serial.print("Setting onboard RTC to: ");
  Serial.println(epoch);
  settimeofday(&now, NULL);
}

void setTimezone(String timezone) {
  Serial.printf("Setting Timezone to %s\n", timezone.c_str());
  setenv("TZ", timezone.c_str(), 1);  //  Now adjust the TZ.  Clock settings are adjusted to show the new local time.
  tzset();
}
