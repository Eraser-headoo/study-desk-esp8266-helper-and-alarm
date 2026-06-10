#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// --- WI-FI CREDENTIALS ---
const char* ssid = "ART-5G";
const char* password = "19791982";

// --- PHYSICAL PIN MAPPING (GPIO NUMBERS) ---
#define PIR_PIN 14     // D5
#define GREEN_LED 12   // D6
#define RED_LED 13     // D7
#define BUZZER_PIN 15  // D8
#define BUTTON_PIN 0   // D3

// --- OLED SETUP ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- WEB SERVER & OTA ---
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// --- POMODORO TIMER CONFIG ---
unsigned long sessionStartTime = 0;
unsigned long studyDuration = 60 * 60 * 1000; // Default 1 Hour
unsigned long breakDuration = 10 * 60 * 1000; // Default 10 Mins
bool isBreak = false;
bool repeatSession = true;
bool sessionActive = false;

// PIR Presence Variables
unsigned long lastMovementTime = 0;
const unsigned long presenceTimeout = 15 * 60 * 1000; // 15 mins allowance

// Display & Navigation
int displayPage = 0; // 0 = Active Session, 1 = Next Exam, 2 = Alarm Status
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;

// Default Schedule Strings
String currentSubject = "Biology";
String currentDetails = "Ch 1, 2, 3, & 5";
String nextExamSubject = "Math (4 Chapters)";
String nextExamDate = "June Finals";

// --- ALARM CORE CONFIG ---
struct AlarmConfig {
  int hour = 8;
  int minute = 0;
  bool isPM = false;
  bool enabled = false;
  bool days[7] = {false, false, false, false, false, false, false}; // Sun-Sat
  bool triggeredToday = false;
};
AlarmConfig dailyAlarm;

bool alarmActive = false;
int buttonPressCount = 0;
int targetButtonPresses = 10;
unsigned long lastBuzzerToggle = 0;
int buzzerToggleState = 0;

void handleRoot() {
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>body{font-family:sans-serif; padding:20px; background:#f0f4f8;} .card{background:white; padding:20px; border-radius:8px; margin-bottom:20px; box-shadow:0 2px 4px rgba(0,0,0,0.1);}</style></head><body>";
  html += "<h1>Desk Dashboard Panel</h1>";
  
  // Pomodoro Segment
  html += "<div class='card'><h2>Pomodoro Controls</h2><form action='/setTimer' method='get'>";
  html += "Subject: <input type='text' name='subj' value='" + currentSubject + "'><br><br>";
  html += "Details: <input type='text' name='det' value='" + currentDetails + "'><br><br>";
  html += "Study (mins): <input type='number' name='study' value='60'><br><br>";
  html += "Break (mins): <input type='number' name='break' value='10'><br><br>";
  html += "<input type='submit' value='Start Loop'></form></div>";
  
  // Alarm Clock Segment
  html += "<div class='card'><h2>Morning Wake Alarm</h2><form action='/setAlarm' method='get'>";
  html += "Time: <input type='number' name='hr' min='1' max='12' value='" + String(dailyAlarm.hour) + "'> : ";
  html += "<input type='number' name='mn' min='0' max='59' value='" + String(dailyAlarm.minute) + "'> ";
  html += "<select name='ampm'><option value='0'" + String(!dailyAlarm.isPM?" selected":"") + ">AM</option><option value='1'" + String(dailyAlarm.isPM?" selected":"") + ">PM</option></select><br><br>";
  html += "Repeat Days:<br>";
  const char* dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  for(int i=0; i<7; i++) {
    html += String("<input type='checkbox' name='d") + String(i) + "' value='1'" + (dailyAlarm.days[i]?" checked":"") + "> " + dayNames[i] + "  ";
  }
  html += "<br><br>Enable Alarm: <input type='checkbox' name='en' value='1'" + String(dailyAlarm.enabled?" checked":"") + "><br><br>";
  html += "<input type='submit' value='Save Alarm Configuration'></form></div>";
  
  html += "<br><a href='/update'>Go to Firmware Update Page</a></body></html>";
  server.send(200, "text/html", html);
}

void handleSetTimer() {
  if (server.hasArg("subj")) currentSubject = server.arg("subj");
  if (server.hasArg("det")) currentDetails = server.arg("det");
  if (server.hasArg("study")) studyDuration = server.arg("study").toInt() * 60 * 1000;
  if (server.hasArg("break")) breakDuration = server.arg("break").toInt() * 60 * 1000;
  
  sessionActive = true;
  isBreak = false;
  sessionStartTime = millis();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSetAlarm() {
  if (server.hasArg("hr")) dailyAlarm.hour = server.arg("hr").toInt();
  if (server.hasArg("mn")) dailyAlarm.minute = server.arg("mn").toInt();
  if (server.hasArg("ampm")) dailyAlarm.isPM = (server.arg("ampm") == "1");
  dailyAlarm.enabled = server.hasArg("en");
  
  for(int i=0; i<7; i++) {
    dailyAlarm.days[i] = server.hasArg("d" + String(i));
  }
  dailyAlarm.triggeredToday = false; // Reset trigger log state
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  
  pinMode(PIR_PIN, INPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(4, 5); // SDA (D2), SCL (D1)
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);

  WiFi.begin(ssid, password);
  display.setCursor(0,20);
  display.print("Connecting Wi-Fi...");
  display.display();
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  // Sync internal system time clock with Internet time servers
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // GMT+3 for Iraq
  
  server.on("/", handleRoot);
  server.on("/setTimer", handleSetTimer);
  server.on("/setAlarm", handleSetAlarm);
  
  httpUpdater.setup(&server);
  server.begin();
  
  lastMovementTime = millis();
}

void loop() {
  server.handleClient();
  unsigned long currentMillis = millis();

  // Fetch local hardware system time structure
  time_t now = time(nullptr);
  struct tm* timeInfo = localtime(&now);

  // --- MECHANICAL BUTTON INPUT EXECUTION ---
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState && (currentMillis - lastDebounceTime) > 50) {
    if (reading == LOW) { 
      if (alarmActive) {
        buttonPressCount++;
        if (buttonPressCount >= targetButtonPresses) {
          alarmActive = false; // Shut down active alarm loop
          noTone(BUZZER_PIN);
        }
      } else {
        displayPage = (displayPage + 1) % 3; // Step across pages 0, 1, 2
      }
    }
    lastDebounceTime = currentMillis;
  }
  lastButtonState = reading;

  // --- AUTOMATED WAKE UP ALARM LOGIC ENGINE ---
  if (dailyAlarm.enabled && timeInfo->tm_year > 70) { // Verify system clock has fully synced
    int currentHour24 = timeInfo->tm_hour;
    int alarmHour24 = dailyAlarm.hour;
    
    // Convert 12h parameters to 24h variables
    if (dailyAlarm.isPM && alarmHour24 < 12) alarmHour24 += 12;
    if (!dailyAlarm.isPM && alarmHour24 == 12) alarmHour24 = 0;
    
    int currentDayOfWeek = timeInfo->tm_wday; // 0 = Sunday, 6 = Saturday

    // Evaluate matching schedule state window
    if (dailyAlarm.days[currentDayOfWeek]) {
      if (currentHour24 == alarmHour24 && timeInfo->tm_min == dailyAlarm.minute) {
        if (!dailyAlarm.triggeredToday && !alarmActive) {
          alarmActive = true;
          buttonPressCount = 0;
          targetButtonPresses = random(7, 21); // Choose target between 7 and 20
          dailyAlarm.triggeredToday = true;
        }
      }
      
      // Power Catch-up Logic Loop Fail-Safe Execution
      // Checks if current system time has scaled clean past past scheduled runtime parameters
      if ((currentHour24 > alarmHour24 || (currentHour24 == alarmHour24 && timeInfo->tm_min > dailyAlarm.minute)) && !dailyAlarm.triggeredToday) {
         alarmActive = true;
         buttonPressCount = 0;
         targetButtonPresses = random(7, 21);
         dailyAlarm.triggeredToday = true;
      }
    }
    
    // Reset trigger tracking gate at structural midnight boundaries
    if (currentHour24 == 0 && timeInfo->tm_min == 0) {
      dailyAlarm.triggeredToday = false;
    }
  }

  // --- AUDIO ALARM BEAK GENERATOR ---
  if (alarmActive) {
    if (currentMillis - lastBuzzerToggle > (unsigned long)random(100, 400)) { // Chaos time intervals
      lastBuzzerToggle = currentMillis;
      buzzerToggleState = !buzzerToggleState;
      if (buzzerToggleState) {
        tone(BUZZER_PIN, (int)random(600, 1800)); // Shift pitch profile frequencies dynamically
      } else {
        noTone(BUZZER_PIN);
      }
    }
  } else {
    // --- POMODORO DESK PRESENCE LOGIC DRIVER ---
    if (digitalRead(PIR_PIN) == HIGH) {
      lastMovementTime = currentMillis;
    }

    if (sessionActive && !isBreak && (currentMillis - lastMovementTime > presenceTimeout)) {
      digitalWrite(GREEN_LED, LOW);
      digitalWrite(RED_LED, HIGH);
      if (currentMillis - lastBuzzerToggle > 500) {
        lastBuzzerToggle = currentMillis;
        buzzerToggleState = !buzzerToggleState;
        if (buzzerToggleState) tone(BUZZER_PIN, 880); else noTone(BUZZER_PIN);
      }
    } else {
      noTone(BUZZER_PIN);
      if (sessionActive && !isBreak) {
        digitalWrite(GREEN_LED, HIGH);
        digitalWrite(RED_LED, LOW);
      } else {
        digitalWrite(GREEN_LED, LOW);
        digitalWrite(RED_LED, LOW);
      }
    }

    // Pomodoro Boundary Transitions
    if (sessionActive) {
      unsigned long currentPhaseDuration = isBreak ? breakDuration : studyDuration;
      if (currentMillis - sessionStartTime >= currentPhaseDuration) {
        if (repeatSession) {
          isBreak = !isBreak;
          sessionStartTime = currentMillis;
        } else {
          sessionActive = false;
        }
      }
    }
  }

  // --- OLED GRAPHICS DRAWING FRAME ENGINE ---
  display.clearDisplay();
  
  if (alarmActive) {
    // Override Screen Display with Active Morning Alarm Challenge
    display.setCursor(0, 0);
    display.setTextSize(2);
    display.println("WAKE UP!!");
    display.setTextSize(1);
    display.setCursor(0, 24);
    display.println("Click button to break");
    display.print("Progress: ");
    display.print(buttonPressCount);
    display.print("/");
    display.println(targetButtonPresses);
    
    // Visual progress box tracking
    int width = map(buttonPressCount, 0, targetButtonPresses, 0, 128);
    display.drawRect(0, 48, 128, 12, WHITE);
    display.fillRect(0, 48, width, 12, WHITE);
    
  } else {
    // Normal Operation Navigation Rendering
    if (displayPage == 0) {
      if (sessionActive) {
        display.setCursor(0, 0);
        display.setTextSize(1);
        display.println(isBreak ? "BREAK TIME" : "STUDY SESSION");
        display.setTextSize(2);
        display.setCursor(0, 14);
        display.println(currentSubject);
        display.setTextSize(1);
        display.setCursor(0, 34);
        display.println(currentDetails);

        unsigned long phaseDur = isBreak ? breakDuration : studyDuration;
        unsigned long elapsed = currentMillis - sessionStartTime;
        int barWidth = map(elapsed, 0, phaseDur, 0, 128);
        display.drawRect(0, 52, 128, 10, WHITE);
        display.fillRect(0, 52, barWidth, 10, WHITE);
      } else {
        display.setCursor(0, 15);
        display.setTextSize(1);
        display.println("Ready to focus.");
        display.print("IP: ");
        display.println(WiFi.localIP());
      }
    } 
    else if (displayPage == 1) {
      display.setCursor(0, 0);
      display.setTextSize(1);
      display.println("NEXT EXAM:");
      display.setTextSize(2);
      display.setCursor(0, 14);
      display.println(nextExamSubject);
      display.setTextSize(1);
      display.setCursor(0, 36);
      display.println(nextExamDate);
      display.println("Arabic: Literature");
    } 
    else if (displayPage == 2) {
      display.setCursor(0, 0);
      display.setTextSize(1);
      display.println("ALARM SETUP STATUS:");
      display.setCursor(0, 16);
      if(dailyAlarm.enabled) {
        display.print("Time: ");
        display.print(dailyAlarm.hour);
        display.print(":");
        if(dailyAlarm.minute < 10) display.print("0");
        display.print(dailyAlarm.minute);
        display.println(dailyAlarm.isPM ? " PM" : " AM");
        display.print("Days: ");
        const char* shortDays[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
        for(int i=0; i<7; i++) {
          if(dailyAlarm.days[i]) { display.print(shortDays[i]); display.print(" "); }
        }
      } else {
        display.println("Alarm Status: OFF");
      }
    }
  }
  display.display();
}
