/*
  ESP32-WROOM-32 Attendance Kiosk - Full Firmware (Nextion touchscreen edition)
  ------------------------------------------------------------------------------
  Wiring:
    R307S VCC (red)    -> ESP32 5V
    R307S GND (black)  -> ESP32 GND
    R307S TX (yellow)  -> ESP32 GPIO16 (RX2)
    R307S RX (green)   -> ESP32 GPIO17 (TX2)

    Buzzer +  (I/O)    -> ESP32 GPIO4
    Buzzer -  (GND)    -> ESP32 GND

    DS3231 SDA         -> ESP32 GPIO21
    DS3231 SCL         -> ESP32 GPIO22
    DS3231 VCC         -> ESP32 3.3V
    DS3231 GND         -> ESP32 GND

    Nextion RX         -> ESP32 GPIO25 (direct wire)
    Nextion TX         -> 1k resistor -> ESP32 GPIO26
                           (junction also runs through 2x 1k in series to GND -
                            voltage divider dropping Nextion's 5V TX to ~3.3V)
    Nextion GND        -> common ground with ESP32 AND its own 5V supply's GND
    Nextion 5V         -> external 5V supply (not the ESP32's own rail)

  Libraries required:
    - Adafruit Fingerprint Sensor Library
    - RTClib (Adafruit)

  Nextion pages/components (built in Nextion Editor, ids confirmed via testing):
    main:     t0 (status text), b0=Enroll(id2), b1=Delete(id3),
              tclock (live clock, top-right, updated every second)
    enroll:   t0 (status text), b0=Cancel(id2)
    delete:   t0 (entry display), digits d1-d9/d0 (ids 4-13), dc=Clear(id14),
              dk=Enter(id2), b3=Cancel(id3)
    keyboard: t0 (entry display), letters q-m (ids 1-26), bksp(id27),
              kcancel(id28), space(id29), kenter(id30), kbmode(id32) -
              123/ABC toggle between letters and numbers/symbols
    roleselect: t0 (title), r1=Teacher(id2), r2=TA(id3), r3=Admin(id4),
              r4=Support(id5), rcancel(id6)
    adminmenu: t0 (title), amenroll(id2), amdelete(id3), amwifi(id5),
              amback(id4), amreboot(id6)

  Remote access:
    - OTA firmware updates: checks OTA_VERSION_URL (hosted on GitHub) once
      an hour and at boot. To push an update, compile via Sketch > Export
      Compiled Binary, upload the .bin to your GitHub repo as firmware.bin,
      and bump the number in version.txt to be higher than FIRMWARE_VERSION
      below.
    - Remote logs: pushes short status lines to a "Logs" tab in the same
      Google Sheet used for attendance - view them from anywhere.
    - Remote reboot: checks a "RemoteControl" tab every 2 minutes for a
      REBOOT command - type REBOOT into that tab's Command cell from
      anywhere and the kiosk restarts within a couple of minutes.
    - Connection monitoring: logs WiFi disconnect/reconnect events
      immediately, logs signal strength (RSSI) every 10 minutes, and logs
      any failed attendance push with its HTTP result code - useful for
      spotting patterns like intermittent drops or possible ISP throttling.
    - BEFORE FIRST USE: replace YOUR_GITHUB_USERNAME in OTA_VERSION_URL and
      OTA_FIRMWARE_URL below with your actual GitHub username.
*/

#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_Fingerprint.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>

Preferences preferences;

// ---- Firmware version / OTA update config ----
// Bump FIRMWARE_VERSION each time you publish a new build to GitHub, and
// update version.txt in the repo to match. The kiosk checks this
// periodically and self-updates when it finds a newer number.
#define FIRMWARE_VERSION 17
// NOTE: the actual GitHub host/path are hardcoded directly inside
// checkForOTAUpdate() below (using the explicit host/port/path HTTPClient
// method, not these as URL strings) - that's the fix for an intermittent
// GitHub CDN/SNI issue. If you ever change your GitHub username or repo
// name, update the two hardcoded paths inside checkForOTAUpdate() instead
// of here.
#define OTA_CHECK_INTERVAL_MS (60UL * 60UL * 1000UL) // check once an hour
uint32_t lastOtaCheck = 0;

#define COMMAND_CHECK_INTERVAL_MS (2UL * 60UL * 1000UL) // check every 2 minutes
uint32_t lastCommandCheck = 0;

#define WIFI_SIGNAL_CHECK_INTERVAL_MS (10UL * 60UL * 1000UL) // log signal every 10 minutes
#define WIFI_WEAK_RSSI_THRESHOLD -75 // dBm - below this is considered a weak signal
uint32_t lastWifiSignalCheck = 0;
bool wifiWasConnected = true; // assume true; setup() already waited for initial connection

// ---- WiFi / Google Sheets config ----
// SSID/password are mutable now - loaded from flash if the admin has set
// them via the on-screen WiFi Setup, otherwise fall back to these defaults.
String wifiSSID = "Sivaton Cloud";
String wifiPassword = "TheEndGame$123";
const char* SHEETS_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbzyIx9rX7TnitcxhKsF2yRl_PTiAPmpiM8lpCg5SWReRe2NKZqs8tb-RVirg1Gh6eOV/exec";

RTC_DS3231 rtc;

// ---- Struct for passing attendance event data through the upload queue ----
struct AttendanceEvent {
  uint8_t id;
  char timestamp[20];
  char direction[4];
  char name[21];
  char role[10];
  uint8_t retryCount;
};

#define MAX_ATTENDANCE_RETRIES 5
#define ATTENDANCE_RETRY_BACKOFF_MS 5000

QueueHandle_t sheetsQueue;

// ---- Lightweight remote logging - pushes short status lines to a "Logs"
//      tab in the same Google Sheet, so the kiosk can be monitored from
//      anywhere without needing a USB-connected Serial Monitor. ----
struct LogEvent {
  char message[100];
};
QueueHandle_t logQueue;

void remoteLog(String message) {
  Serial.println("[LOG] " + message);
  if (logQueue == NULL) return; // not created yet (very early boot)
  LogEvent evt;
  message.toCharArray(evt.message, sizeof(evt.message));
  xQueueSend(logQueue, &evt, 0);
}

// Sends a log message directly and waits for it to actually complete,
// rather than handing it to the background queue - used only for the rare,
// critical case (remote reboot) where we're about to restart and can't
// rely on the async task finishing in time on a slow connection.
void remoteLogSync(String message) {
  Serial.println("[LOG] " + message);
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String(SHEETS_SCRIPT_URL) + "?action=log&message=" + urlEncode(message);
  http.begin(url);
  http.setTimeout(8000);
  http.GET();
  http.end();
}

void logUploaderTask(void *param) {
  LogEvent evt;
  for (;;) {
    if (xQueueReceive(logQueue, &evt, portMAX_DELAY) == pdTRUE) {
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = String(SHEETS_SCRIPT_URL) + "?action=log&message=" + urlEncode(String(evt.message));
        http.begin(url);
        http.setTimeout(10000);
        http.GET();
        http.end();
      }
    }
  }
}

// ---- Keeps a "Roster" tab in the same Sheet always in sync with exactly
//      who's currently enrolled - updated the moment someone is added or
//      removed, so the current roster is always one look away. ----
struct RosterEvent {
  uint8_t id;
  char name[21];
  char role[10];
  char action[8]; // "enroll" or "delete"
};
QueueHandle_t rosterQueue; // sized generously below to handle a full one-time roster backfill

void sendRosterUpdate(uint8_t id, String name, String role, String action) {
  if (rosterQueue == NULL) return;
  RosterEvent evt;
  evt.id = id;
  name.toCharArray(evt.name, sizeof(evt.name));
  role.toCharArray(evt.role, sizeof(evt.role));
  action.toCharArray(evt.action, sizeof(evt.action));
  xQueueSend(rosterQueue, &evt, 0);
}

void rosterUploaderTask(void *param) {
  RosterEvent evt;
  for (;;) {
    if (xQueueReceive(rosterQueue, &evt, portMAX_DELAY) == pdTRUE) {
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = String(SHEETS_SCRIPT_URL) +
                     "?action=roster&id=" + String(evt.id) +
                     "&name=" + urlEncode(String(evt.name)) +
                     "&role=" + urlEncode(String(evt.role)) +
                     "&rosteraction=" + String(evt.action);
        http.begin(url);
        http.setTimeout(10000);
        http.GET();
        http.end();
      }
    }
  }
}

// ---- UART2 setup: R307S on GPIO16 (RX2) / GPIO17 (TX2) ----
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger(&mySerial);

// ---- Buzzer ----
#define BUZZER_PIN 4
#define BUZZER_ACTIVE  0
#define BUZZER_PASSIVE 1
#define BUZZER_TYPE BUZZER_PASSIVE
#if BUZZER_TYPE == BUZZER_PASSIVE
  #define BUZZER_TONE_HZ 2700
#endif

// ---- Per-employee state: false = out, true = in. Index 0 unused (IDs start at 1) ----
bool checkedIn[128] = { false };

// ---- Which IDs currently have a fingerprint enrolled ----
bool enrolled[128] = { false };

uint32_t lastMatchTime[128] = { 0 };
#define MATCH_COOLDOWN_MS 60000 // 60s - long enough that a confused re-scan doesn't flip IN/OUT again

uint32_t lastScanAttemptTime = 0;
#define SCAN_COOLDOWN_MS 1000

uint32_t lastGoodMatchTime = 0;
uint32_t lastActivityTime = 0;

#define COLOR_WHITE 65535
#define COLOR_GREEN 2016
#define COLOR_RED 63488

uint32_t bgRevertAt = 0;
uint32_t lastClockUpdate = 0;

// =================== NEXTION DISPLAY ===================

#define PAGE_MAIN 0
#define PAGE_ENROLL 1
#define PAGE_DELETE 2
#define PAGE_KEYBOARD 3
#define PAGE_IDLE 4
#define PAGE_ADMINMENU 5
#define PAGE_ROLESELECT 6

#define ROLE_TEACHER 2
#define ROLE_TA 3
#define ROLE_ADMIN 4
#define ROLE_SUPPORT 5
#define ROLE_CANCEL 6

#define ADMIN_HOTSPOT 2
#define AM_ENROLL 2
#define AM_DELETE 3
#define AM_WIFI 5
#define AM_BACK 4
#define AM_REBOOT 6

#define IDLE_WAKE 2
#define IDLE_TIMEOUT_MS 30000

#define ENROLL_BTN_CANCEL 2

#define DELETE_BTN_ENTER 2
#define DELETE_BTN_CANCEL 3
#define DELETE_BTN_CLEAR 14

#define KB_BKSP 27
#define KB_CANCEL 28
#define KB_SPACE 29
#define KB_ENTER 30
#define KB_MODE_TOGGLE 32

#define KB_TARGET_NAME 0
#define KB_TARGET_WIFI_SSID 1
#define KB_TARGET_WIFI_PASSWORD 2

const char keyboardLetters[26] = {
  'Q','W','E','R','T','Y','U','I','O','P',
  'A','S','D','F','G','H','J','K','L',
  'Z','X','C','V','B','N','M'
};

// Same 26 key positions, shown when the "123" toggle is active
const char keyboardNumbers[26] = {
  '1','2','3','4','5','6','7','8','9','0',
  '!','@','#','$','%','^','&','*','-',
  '_','+','=','.',',','?','/'
};

// objnames in the same order as the two arrays above, so we can rewrite
// each button's displayed text when the mode is toggled
const char* keyboardObjNames[26] = {
  "q","w","e","r","t","y","u","i","o","p",
  "a","s","d","f","g","h","j","k","l",
  "z","x","c","v","b","n","m"
};

int kbMode = 0; // 0 = letters, 1 = numbers/symbols
int keyboardTarget = KB_TARGET_NAME;
String wifiSsidTemp = "";

String nameBuffer = "";
String deleteIdBuffer = "";
String currentPage = "main";
uint8_t pendingEnrollId = 0;
String pendingEnrollName = "";

// Non-blocking "show a message on main, then revert" timer
uint32_t mainMsgRevertAt = 0;

void nextionSend(const String &cmd) {
  Serial1.print(cmd);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
}

void nextionSetText(const String &component, String text) {
  text.replace("\"", "\\\"");
  nextionSend(component + ".txt=\"" + text + "\"");
}

void nextionSetPage(const String &page) {
  nextionSend("page " + page);
}

void goToPage(const String &page) {
  nextionSetPage(page);
  currentPage = page;
}

char digitForCompId(uint8_t compId) {
  switch (compId) {
    case 4: return '1';
    case 5: return '2';
    case 6: return '3';
    case 7: return '4';
    case 8: return '5';
    case 9: return '6';
    case 10: return '7';
    case 11: return '8';
    case 12: return '9';
    case 13: return '0';
    default: return 0;
  }
}

// ---- Peek/consume a single 7-byte touch packet from the Nextion, if the
//      button pressed was specifically the enroll-page Cancel button. Used
//      to allow aborting out of the otherwise-blocking enrollment wait loops. ----
bool checkForEnrollCancel() {
  while (Serial1.available() >= 7) {
    if (Serial1.peek() == 0x65) {
      uint8_t buf[7];
      Serial1.readBytes(buf, 7);
      if (buf[1] == PAGE_ENROLL && buf[2] == ENROLL_BTN_CANCEL) return true;
      // any other touch during a blocking enroll wait is ignored - nothing
      // else on screen is meaningful to press at this moment
    } else {
      Serial1.read();
    }
  }
  return false;
}

// Rewrites all 26 keyboard buttons to show uppercase letters, lowercase
// letters, or numbers/symbols depending on mode (0/1/2), and updates the
// toggle button's own label to show what the NEXT tap will switch to.
void setKeyboardMode(int mode) {
  kbMode = mode;
  for (int i = 0; i < 26; i++) {
    char c;
    if (mode == 0) c = keyboardLetters[i];
    else if (mode == 1) c = tolower(keyboardLetters[i]);
    else c = keyboardNumbers[i];
    nextionSetText(String("keyboard.") + keyboardObjNames[i], String(c));
  }
  const char* nextLabel = (mode == 0) ? "abc" : (mode == 1) ? "123" : "ABC";
  nextionSetText("keyboard.kbmode", nextLabel);
}

// Returns the character a given key (1-26) should type, based on the
// current keyboard mode.
char charForKeyboardKey(uint8_t compId) {
  int i = compId - 1;
  if (kbMode == 0) return keyboardLetters[i];
  if (kbMode == 1) return tolower(keyboardLetters[i]);
  return keyboardNumbers[i];
}

void handleTouch(uint8_t pageId, uint8_t compId, uint8_t event) {
  Serial.printf("Touch event -> page=%d comp=%d event=%d\n", pageId, compId, event);
  lastActivityTime = millis();

  if (pageId == PAGE_IDLE) {
    if (compId == IDLE_WAKE) {
      goToPage("main");
      nextionSetText("main.t0", "Ready - place finger");
    }
    return;
  }

  if (pageId == PAGE_MAIN) {
    if (compId == ADMIN_HOTSPOT) {
      goToPage("adminmenu");
    }
  } else if (pageId == PAGE_ADMINMENU) {
    if (compId == AM_ENROLL) {
      startEnrollFlow();
    } else if (compId == AM_DELETE) {
      deleteIdBuffer = "";
      goToPage("delete");
      nextionSetText("delete.t0", "Enter ID");
    } else if (compId == AM_WIFI) {
      keyboardTarget = KB_TARGET_WIFI_SSID;
      wifiSsidTemp = "";
      nameBuffer = "";
      goToPage("keyboard");
      setKeyboardMode(0);
      nextionSetText("keyboard.t0", "Enter WiFi Name");
    } else if (compId == AM_BACK) {
      goToPage("main");
      nextionSetText("main.t0", "Ready - place finger");
    } else if (compId == AM_REBOOT) {
      goToPage("main");
      nextionSetText("main.t0", "Rebooting...");
      delay(1000);
      ESP.restart();
    }
  } else if (pageId == PAGE_DELETE) {
    char digit = digitForCompId(compId);
    if (digit != 0) {
      if (deleteIdBuffer.length() < 3) {
        deleteIdBuffer += digit;
        nextionSetText("delete.t0", deleteIdBuffer);
      }
    } else if (compId == DELETE_BTN_CLEAR) {
      deleteIdBuffer = "";
      nextionSetText("delete.t0", "Enter ID");
    } else if (compId == DELETE_BTN_ENTER) {
      if (deleteIdBuffer.length() > 0) {
        int idToDelete = deleteIdBuffer.toInt();
        deleteIdBuffer = "";
        processDeleteRequest((uint8_t)idToDelete);
      }
    } else if (compId == DELETE_BTN_CANCEL) {
      deleteIdBuffer = "";
      goToPage("main");
      nextionSetText("main.t0", "Ready - place finger");
    }
  } else if (pageId == PAGE_KEYBOARD) {
    if (compId >= 1 && compId <= 26) {
      if (nameBuffer.length() < 30) {
        nameBuffer += charForKeyboardKey(compId);
        nextionSetText("keyboard.t0", nameBuffer);
      }
    } else if (compId == KB_MODE_TOGGLE) {
      setKeyboardMode((kbMode + 1) % 3);
    } else if (compId == KB_SPACE) {
      if (nameBuffer.length() > 0 && nameBuffer.length() < 30) {
        nameBuffer += ' ';
        nextionSetText("keyboard.t0", nameBuffer);
      }
    } else if (compId == KB_BKSP) {
      if (nameBuffer.length() > 0) {
        nameBuffer.remove(nameBuffer.length() - 1);
        nextionSetText("keyboard.t0", nameBuffer.length() > 0 ? nameBuffer : "Enter name");
      }
    } else if (compId == KB_CANCEL) {
      if (keyboardTarget != KB_TARGET_NAME) {
        // Cancelling a WiFi entry - nothing to roll back, just go back to admin menu
        nameBuffer = "";
        wifiSsidTemp = "";
        keyboardTarget = KB_TARGET_NAME;
        goToPage("adminmenu");
        return;
      }
      // Fingerprint was already captured and stored before we got here -
      // roll that back so we don't leave an orphaned, unnamed enrollment.
      if (pendingEnrollId > 0) {
        finger.deleteModel(pendingEnrollId);
        enrolled[pendingEnrollId] = false;
        preferences.putBytes("enrolled", enrolled, sizeof(enrolled));
        pendingEnrollId = 0;
      }
      nameBuffer = "";
      goToPage("main");
      nextionSetText("main.t0", "Ready - place finger");
    } else if (compId == KB_ENTER) {
      if (nameBuffer.length() == 0) return;

      if (keyboardTarget == KB_TARGET_NAME) {
        if (pendingEnrollId > 0) {
          preferences.putString(("name" + String(pendingEnrollId)).c_str(), nameBuffer);
          pendingEnrollName = nameBuffer;
          nameBuffer = "";
          goToPage("roleselect");
        }
      } else if (keyboardTarget == KB_TARGET_WIFI_SSID) {
        wifiSsidTemp = nameBuffer;
        nameBuffer = "";
        keyboardTarget = KB_TARGET_WIFI_PASSWORD;
        setKeyboardMode(0);
        nextionSetText("keyboard.t0", "Enter WiFi Password");
      } else if (keyboardTarget == KB_TARGET_WIFI_PASSWORD) {
        preferences.putString("wifiSSID", wifiSsidTemp);
        preferences.putString("wifiPassword", nameBuffer);
        nameBuffer = "";
        wifiSsidTemp = "";
        keyboardTarget = KB_TARGET_NAME;
        goToPage("main");
        nextionSetText("main.t0", "WiFi updated - restarting...");
        delay(1500);
        ESP.restart();
      }
    }
  } else if (pageId == PAGE_ROLESELECT) {
    if (compId == ROLE_CANCEL) {
      // Fingerprint and name were already saved - roll the whole enrollment back
      if (pendingEnrollId > 0) {
        finger.deleteModel(pendingEnrollId);
        enrolled[pendingEnrollId] = false;
        preferences.putBytes("enrolled", enrolled, sizeof(enrolled));
        preferences.remove(("name" + String(pendingEnrollId)).c_str());
        pendingEnrollId = 0;
      }
      pendingEnrollName = "";
      goToPage("main");
      nextionSetText("main.t0", "Ready - place finger");
      return;
    }

    String role = "";
    if (compId == ROLE_TEACHER) role = "Teacher";
    else if (compId == ROLE_TA) role = "TA";
    else if (compId == ROLE_ADMIN) role = "Admin";
    else if (compId == ROLE_SUPPORT) role = "Support";

    if (role != "" && pendingEnrollId > 0) {
      preferences.putString(("role" + String(pendingEnrollId)).c_str(), role);
      Serial.printf("Enrolled ID %d as '%s' (%s)\n", pendingEnrollId, pendingEnrollName.c_str(), role.c_str());
      remoteLog("Enrolled ID " + String(pendingEnrollId) + " as '" + pendingEnrollName + "' (" + role + ")");
      sendRosterUpdate(pendingEnrollId, pendingEnrollName, role, "enroll");
      String confirmedName = pendingEnrollName;
      pendingEnrollId = 0;
      pendingEnrollName = "";
      goToPage("main");
      showMainMessage("Enrolled: " + confirmedName, 2500);
    }
  }
}

void checkNextionTouch() {
  while (Serial1.available() >= 7) {
    if (Serial1.peek() == 0x65) {
      uint8_t buf[7];
      Serial1.readBytes(buf, 7);
      handleTouch(buf[1], buf[2], buf[3]);
    } else {
      Serial1.read();
    }
  }
}

// ---- Show a temporary message on main.t0, then automatically revert to the
//      ready prompt after durationMs - without blocking the main loop. ----
void showMainMessage(const String &text, uint32_t durationMs) {
  nextionSetText("main.t0", text);
  mainMsgRevertAt = millis() + durationMs;
}

void flashMainBackground(uint16_t color, uint32_t durationMs) {
  nextionSend("main.bco=" + String(color));
  bgRevertAt = millis() + durationMs;
}

void serviceMainMessageRevert() {
  if (mainMsgRevertAt != 0 && millis() > mainMsgRevertAt && currentPage == "main") {
    nextionSetText("main.t0", "Ready - place finger");
    mainMsgRevertAt = 0;
  }
}

void serviceBgRevert() {
  if (bgRevertAt != 0 && millis() > bgRevertAt && currentPage == "main") {
    nextionSend("main.bco=" + String(COLOR_WHITE));
    bgRevertAt = 0;
  }
}

// =================== ENROLLMENT / DELETE FLOWS ===================

uint8_t nextFreeId() {
  for (uint8_t id = 1; id <= 127; id++) {
    if (!enrolled[id]) return id;
  }
  return 0; // none free
}

void startEnrollFlow() {
  uint8_t newId = nextFreeId();
  if (newId == 0) {
    showMainMessage("Memory full - cannot enroll", 2500);
    return;
  }

  goToPage("enroll");
  bool ok = captureEnrollment(newId);

  if (!ok) {
    goToPage("main");
    return; // captureEnrollment already showed the relevant error/cancel message
  }

  pendingEnrollId = newId;
  keyboardTarget = KB_TARGET_NAME;
  goToPage("keyboard");
  setKeyboardMode(0);
  nameBuffer = "";
  nextionSetText("keyboard.t0", "Enter name");
}

// ---- Two-scan capture + store. Blocking (same as the original serial-menu
//      design) since the user is standing at the kiosk during this. Polls for
//      a Cancel press from the enroll page on every iteration so it's not a
//      dead end if a scan goes wrong. ----
bool captureEnrollment(uint8_t id) {
  int p = -1;

  nextionSetText("enroll.t0", "Place finger");
  beepPrompt();
  while (p != FINGERPRINT_OK) {
    if (checkForEnrollCancel()) {
      nextionSetText("enroll.t0", "Cancelled");
      delay(800);
      return false;
    }
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) continue;
    if (p != FINGERPRINT_OK) {
      nextionSetText("enroll.t0", "Image capture error");
      delay(1500);
      return false;
    }
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    nextionSetText("enroll.t0", "Failed - try again");
    delay(1500);
    return false;
  }

  nextionSetText("enroll.t0", "Remove finger");
  beepPrompt();
  delay(1000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    if (checkForEnrollCancel()) {
      nextionSetText("enroll.t0", "Cancelled");
      delay(800);
      return false;
    }
    p = finger.getImage();
  }

  nextionSetText("enroll.t0", "Place same finger again");
  beepPrompt();
  p = -1;
  while (p != FINGERPRINT_OK) {
    if (checkForEnrollCancel()) {
      nextionSetText("enroll.t0", "Cancelled");
      delay(800);
      return false;
    }
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) continue;
    if (p != FINGERPRINT_OK) {
      nextionSetText("enroll.t0", "Image capture error");
      delay(1500);
      return false;
    }
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    nextionSetText("enroll.t0", "Failed - try again");
    delay(1500);
    return false;
  }

  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    nextionSetText("enroll.t0", "Fingerprints didn't match");
    delay(1800);
    return false;
  }

  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    nextionSetText("enroll.t0", "Failed to store");
    delay(1500);
    return false;
  }

  enrolled[id] = true;
  preferences.putBytes("enrolled", enrolled, sizeof(enrolled));
  return true;
}

void processDeleteRequest(uint8_t id) {
  if (id < 1 || id > 127 || !enrolled[id]) {
    showMainMessage("ID " + String(id) + " not found", 2000);
    goToPage("main");
    return;
  }

  int p = finger.deleteModel(id);
  if (p == FINGERPRINT_OK) {
    enrolled[id] = false;
    checkedIn[id] = false;
    preferences.putBytes("enrolled", enrolled, sizeof(enrolled));
    preferences.putBytes("checkedIn", checkedIn, sizeof(checkedIn));
    preferences.remove(("name" + String(id)).c_str());
    preferences.remove(("role" + String(id)).c_str());
    goToPage("main");
    showMainMessage("Deleted ID " + String(id), 2000);
    remoteLog("Deleted ID " + String(id));
    sendRosterUpdate(id, "", "", "delete");
  } else {
    nextionSetText("delete.t0", "Delete failed");
    delay(1500);
    goToPage("main");
  }
}

// =================== REMOTE OTA UPDATE ===================

// Checks a version number hosted on GitHub against our own; if the remote
// one is higher, downloads and flashes the new firmware automatically.
// Runs on a timer (see loop()) so updates can be pushed with zero physical
// access to the kiosk - just upload a new firmware.bin and bump version.txt.
void checkForOTAUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;

  remoteLog("Checking for firmware update...");

  WiFiClientSecure client;
  client.setInsecure(); // skip cert validation for simplicity - acceptable
                         // here since we're only ever downloading our own
                         // known firmware file, not handling sensitive data

  HTTPClient http;
  String versionPath = "/timmytrader1-coder/attendance-kiosk-firmware/main/version.txt?cb=" + String(millis());
  http.begin(client, "raw.githubusercontent.com", 443, versionPath, true);
  http.setTimeout(10000);
  int httpCode = http.GET();

  if (httpCode != 200) {
    remoteLog("OTA version check failed, HTTP code: " + String(httpCode));
    http.end();
    client.stop();
    return;
  }

  int remoteVersion = http.getString().toInt();
  http.end();

  if (remoteVersion <= FIRMWARE_VERSION) {
    remoteLog("Firmware up to date (v" + String(FIRMWARE_VERSION) + ").");
    client.stop();
    return;
  }

  remoteLog("New firmware found (v" + String(remoteVersion) + ") - updating...");
  nextionSetText("main.t0", "Updating firmware...");

  httpUpdate.rebootOnUpdate(true);
  client.setTimeout(15000); // safety bound so a stalled download can never hang forever
  String firmwarePath = "/timmytrader1-coder/attendance-kiosk-firmware/main/firmware.bin";
  t_httpUpdate_return result = httpUpdate.update(client, "raw.githubusercontent.com", 443, firmwarePath);

  switch (result) {
    case HTTP_UPDATE_FAILED:
      remoteLog("OTA update failed: " + httpUpdate.getLastErrorString());
      nextionSetText("main.t0", "Update failed");
      delay(2000);
      nextionSetText("main.t0", "Ready - place finger");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      remoteLog("OTA reported no updates available.");
      break;
    case HTTP_UPDATE_OK:
      remoteLog("OTA update succeeded - rebooting.");
      break;
  }
  client.stop(); // no-op if HTTP_UPDATE_OK already triggered a reboot
}

// =================== SETUP / LOOP ===================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n=== Attendance Kiosk - Nextion Edition ===");
  Serial.println("Firmware version: " + String(FIRMWARE_VERSION));

  preferences.begin("attendance", false);

  size_t storedLen = preferences.getBytesLength("checkedIn");
  if (storedLen == sizeof(checkedIn)) {
    preferences.getBytes("checkedIn", checkedIn, sizeof(checkedIn));
    Serial.println("Restored clock-in state from flash.");
  } else {
    Serial.println("No saved clock-in state - starting fresh.");
  }

  size_t enrolledLen = preferences.getBytesLength("enrolled");
  if (enrolledLen == sizeof(enrolled)) {
    preferences.getBytes("enrolled", enrolled, sizeof(enrolled));
    Serial.println("Restored enrolled-ID list from flash.");
  } else {
    Serial.println("No saved enrolled-ID list - starting fresh.");
  }

  wifiSSID = preferences.getString("wifiSSID", wifiSSID);
  wifiPassword = preferences.getString("wifiPassword", wifiPassword);

  // ---- Connect WiFi and stand up remote logging FIRST, before any
  // hardware check that could halt the device - so a failed RTC or sensor
  // can actually be reported remotely instead of the kiosk just going
  // silent with nobody physically there to see the Serial Monitor. ----
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(300);
    Serial.print(".");
  }

  sheetsQueue = xQueueCreate(20, sizeof(AttendanceEvent));
  xTaskCreatePinnedToCore(sheetsUploaderTask, "SheetsUploader", 8192, NULL, 1, NULL, 0);
  logQueue = xQueueCreate(20, sizeof(LogEvent));
  xTaskCreatePinnedToCore(logUploaderTask, "LogUploader", 8192, NULL, 1, NULL, 0);
  rosterQueue = xQueueCreate(50, sizeof(RosterEvent));
  xTaskCreatePinnedToCore(rosterUploaderTask, "RosterUploader", 8192, NULL, 1, NULL, 0);

  // One-time backfill: if the Roster tab has never been populated (this
  // being the first boot after the roster-sync feature was added), push
  // every already-enrolled employee to it now, using their saved name/role.
  if (!preferences.getBool("rosterSynced", false)) {
    for (uint8_t id = 1; id <= 127; id++) {
      if (enrolled[id]) {
        String bfName = preferences.getString(("name" + String(id)).c_str(), "ID " + String(id));
        String bfRole = preferences.getString(("role" + String(id)).c_str(), "");
        sendRosterUpdate(id, bfName, bfRole, "enroll");
      }
    }
    preferences.putBool("rosterSynced", true);
    remoteLog("Backfilled roster sync for all previously-enrolled IDs.");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
    remoteLog("Booting - firmware v" + String(FIRMWARE_VERSION) + ", IP " + WiFi.localIP().toString());

    configTime(2 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // South Africa (SAST) = UTC+2, no DST
  } else {
    Serial.println("\nWiFi NOT connected - continuing without Sheets logging.");
  }

  pinMode(BUZZER_PIN, OUTPUT);
#if BUZZER_TYPE == BUZZER_PASSIVE
  ledcAttach(BUZZER_PIN, BUZZER_TONE_HZ, 8);
  ledcWriteTone(BUZZER_PIN, 0);
#else
  digitalWrite(BUZZER_PIN, LOW);
#endif

  Wire.begin(21, 22);
  if (!rtc.begin()) {
    Serial.println("DS3231 not found. Check wiring.");
    remoteLog("FATAL: DS3231 not found at boot - halting.");
    while (1) { delay(1000); }
  }
  if (rtc.lostPower()) {
    Serial.println("RTC lost power - setting time to compile time as fallback.");
    remoteLog("RTC lost power - using compile-time fallback until NTP sync.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Sync the RTC from NTP every boot - keeps device time accurate even
    // if the DS3231's backup coin-cell battery is weak or dead, rather than
    // only correcting time when lostPower() happens to trigger.
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
      rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
      Serial.println("RTC synced from NTP: " + formatTimestamp(rtc.now()));
    } else {
      Serial.println("NTP sync failed - keeping existing RTC time.");
      remoteLog("NTP sync failed at boot - keeping existing RTC time.");
    }
  }
  Serial.print("RTC time: ");
  Serial.println(formatTimestamp(rtc.now()));

  mySerial.begin(57600, SERIAL_8N1, 16, 17);
  delay(200);
  finger.begin(57600);
  delay(200);

  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor found and password verified.");
  } else {
    Serial.println("Fingerprint sensor NOT found. Check wiring.");
    remoteLog("FATAL: Fingerprint sensor not found at boot - halting.");
    while (1) { delay(1000); }
  }
  finger.getParameters();
  Serial.print("Capacity: "); Serial.println(finger.capacity);

  // ---- Nextion display init ----
  Serial1.begin(9600, SERIAL_8N1, 26, 25); // RX=GPIO26, TX=GPIO25
  delay(300);
  nextionSend("bkcmd=0");
  delay(50);
  while (Serial1.available()) Serial1.read();
  goToPage("main");
  nextionSetText("main.t0", "Ready - place finger");
  lastActivityTime = millis();

  checkForOTAUpdate();
  lastOtaCheck = millis();

  Serial.println("Setup complete. Ready for touchscreen operation.");
  remoteLog("Setup complete - ready for touchscreen operation.");
}

void loop() {
  checkNextionTouch();
  serviceMainMessageRevert();
  serviceBgRevert();
  serviceIdleTimeout();
  serviceOtaCheck();
  serviceCommandCheck();
  serviceWifiMonitor();
  serviceClockDisplay();
  serviceDailyReset();
  if (currentPage == "main" || currentPage == "idle") {
    getFingerprintMatch();
  }
  delay(50);
}

// Clears everyone back to "out" when the calendar date changes. Without this,
// someone who forgot to clock out stays "in" overnight, so their next
// morning's first scan was treated as OUT (an OUT with no IN). The last date
// seen is kept in flash so a reboot doesn't miss the rollover.
uint32_t lastDailyResetCheck = 0;
void serviceDailyReset() {
  if (millis() - lastDailyResetCheck < 30000) return;
  lastDailyResetCheck = millis();

  DateTime now = rtc.now();
  int today = now.year() * 10000 + now.month() * 100 + now.day();
  int lastDay = preferences.getInt("lastDay", 0);
  if (lastDay == today) return;

  if (lastDay != 0) {
    int stillIn = 0;
    for (int i = 1; i < 128; i++) if (checkedIn[i]) stillIn++;
    memset(checkedIn, 0, sizeof(checkedIn));
    preferences.putBytes("checkedIn", checkedIn, sizeof(checkedIn));
    remoteLog("New day - reset clock-in state (" + String(stillIn) + " were still IN).");
  }
  preferences.putInt("lastDay", today);
}

void serviceClockDisplay() {
  if (currentPage != "main") return;
  if (millis() - lastClockUpdate < 1000) return;
  lastClockUpdate = millis();

  DateTime now = rtc.now();
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  nextionSetText("main.tclock", buf);
}

// Polls the "RemoteControl" tab's command cell every couple of minutes.
// Currently supports remote REBOOT, issued from anywhere by just typing
// REBOOT into that cell - the cell resets itself once picked up.
void serviceCommandCheck() {
  if (millis() - lastCommandCheck < COMMAND_CHECK_INTERVAL_MS) return;
  lastCommandCheck = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(SHEETS_SCRIPT_URL) + "?action=command";
  http.begin(url);
  http.setTimeout(10000);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String command = http.getString();
    command.trim();
    if (command == "REBOOT") {
      http.end();
      // Send this specific log synchronously (not via the async queue) and
      // actually wait for it to finish, since we're about to restart and
      // can't rely on the background task completing in time - especially
      // on a slow/struggling connection, which is exactly when this matters most.
      remoteLogSync("Remote reboot command received - restarting.");
      goToPage("main");
      nextionSetText("main.t0", "Remote reboot requested...");
      delay(500);
      ESP.restart();
    }
  }
  http.end();
}

// Tracks WiFi connection quality so intermittent internet trouble (weak
// signal, drops, possible ISP throttling) shows up in the remote Logs tab
// instead of being invisible. Detects connect/disconnect transitions
// immediately, and logs the current signal strength every 10 minutes
// regardless, so patterns over time (e.g. always weak at a certain time of
// day) are visible in the log history.
void serviceWifiMonitor() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiWasConnected && !isConnected) {
    remoteLog("WiFi disconnected.");
  } else if (!wifiWasConnected && isConnected) {
    remoteLog("WiFi reconnected. Signal: " + String(WiFi.RSSI()) + " dBm");
  }
  wifiWasConnected = isConnected;

  if (millis() - lastWifiSignalCheck < WIFI_SIGNAL_CHECK_INTERVAL_MS) return;
  lastWifiSignalCheck = millis();

  if (isConnected) {
    int rssi = WiFi.RSSI();
    String note = (rssi < WIFI_WEAK_RSSI_THRESHOLD) ? " (weak)" : "";
    remoteLog("WiFi signal: " + String(rssi) + " dBm" + note);
  }
}

void serviceOtaCheck() {
  if (millis() - lastOtaCheck > OTA_CHECK_INTERVAL_MS) {
    lastOtaCheck = millis();
    checkForOTAUpdate();
  }
}

void serviceIdleTimeout() {
  if (currentPage == "main" && millis() - lastActivityTime > IDLE_TIMEOUT_MS) {
    goToPage("idle");
  }
}

// =================== HELPERS (mostly unchanged from original) ===================

String formatTimestamp(DateTime dt) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
  return String(buf);
}

void buzzerOn() {
#if BUZZER_TYPE == BUZZER_PASSIVE
  ledcWriteTone(BUZZER_PIN, BUZZER_TONE_HZ);
#else
  digitalWrite(BUZZER_PIN, HIGH);
#endif
}

void buzzerOff() {
#if BUZZER_TYPE == BUZZER_PASSIVE
  ledcWriteTone(BUZZER_PIN, 0);
#else
  digitalWrite(BUZZER_PIN, LOW);
#endif
}

void beepIn() {
  buzzerOn(); delay(150); buzzerOff();
}

void beepOut() {
  for (int i = 0; i < 2; i++) { buzzerOn(); delay(120); buzzerOff(); delay(120); }
}

void beepError() {
  for (int i = 0; i < 3; i++) { buzzerOn(); delay(60); buzzerOff(); delay(60); }
}

void beepPrompt() {
  buzzerOn(); delay(80); buzzerOff();
}

void sheetsUploaderTask(void *param) {
  AttendanceEvent evt;
  for (;;) {
    if (xQueueReceive(sheetsQueue, &evt, portMAX_DELAY) == pdTRUE) {
      bool success = sendAttendanceToSheets(evt.id, String(evt.timestamp), String(evt.direction), String(evt.name), String(evt.role));
      if (!success) {
        beepError();
        if (evt.retryCount < MAX_ATTENDANCE_RETRIES) {
          evt.retryCount++;
          remoteLog("Retrying attendance push for ID " + String(evt.id) + " (" + String(evt.retryCount) + "/" + String(MAX_ATTENDANCE_RETRIES) + ") in " + String(ATTENDANCE_RETRY_BACKOFF_MS / 1000) + "s");
          vTaskDelay(pdMS_TO_TICKS(ATTENDANCE_RETRY_BACKOFF_MS));
          xQueueSend(sheetsQueue, &evt, 0);
        } else {
          remoteLog("Attendance push permanently failed for ID " + String(evt.id) + " after " + String(MAX_ATTENDANCE_RETRIES) + " retries - event lost.");
        }
      }
    }
  }
}

void launchSheetsPush(uint8_t id, String timestamp, const char* direction, String name, String role) {
  AttendanceEvent evt;
  evt.id = id;
  timestamp.toCharArray(evt.timestamp, sizeof(evt.timestamp));
  strncpy(evt.direction, direction, sizeof(evt.direction) - 1);
  evt.direction[sizeof(evt.direction) - 1] = '\0';
  name.toCharArray(evt.name, sizeof(evt.name));
  role.toCharArray(evt.role, sizeof(evt.role));
  evt.retryCount = 0;

  if (xQueueSend(sheetsQueue, &evt, 0) != pdTRUE) {
    Serial.println("Upload queue full - event dropped.");
    remoteLogSync("Upload queue full - attendance event for ID " + String(id) + " dropped.");
  }
}

bool sendAttendanceToSheets(uint8_t id, String timestamp, String direction, String name, String role) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected - skipping Sheets push.");
    return false;
  }

  // Built purely from data that stays identical across retries of the same
  // scan (id, timestamp, direction), so the server can recognize a retry as
  // "already handled" and skip reprocessing it - this is what makes the
  // retry-on-failure logic safe from creating duplicate/orphan rows.
  String eventId = String(id) + "_" + timestamp + "_" + direction;

  HTTPClient http;
  String url = String(SHEETS_SCRIPT_URL) +
               "?id=" + String(id) +
               "&timestamp=" + urlEncode(timestamp) +
               "&direction=" + direction +
               "&name=" + urlEncode(name) +
               "&role=" + urlEncode(role) +
               "&eventId=" + urlEncode(eventId);

  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  int httpCode = http.GET();

  bool success = false;
  if (httpCode == 200) {
    Serial.println("Sheets push OK: " + http.getString());
    success = true;
  } else {
    Serial.print("Sheets push failed, HTTP code: ");
    Serial.println(httpCode);
    // Negative codes mean the request never got a real response at all
    // (timeout, DNS failure, connection refused) - a strong signal of
    // internet trouble rather than a server-side error.
    remoteLog("Attendance push failed, HTTP code: " + String(httpCode) +
               (httpCode < 0 ? " (connection-level failure)" : ""));
  }
  http.end();
  return success;
}

String urlEncode(String str) {
  String encoded = "";
  for (size_t i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

int getFingerprintMatch() {
  int p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;

  lastActivityTime = millis();
  if (currentPage == "idle") {
    goToPage("main");
    nextionSetText("main.t0", "Ready - place finger");
  }

  uint32_t now_ms = millis();
  if (now_ms - lastScanAttemptTime < SCAN_COOLDOWN_MS) return -1;
  lastScanAttemptTime = now_ms;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) {
    Serial.println("No match found.");
    // Only beep/show an error if this isn't right on the heels of a genuine
    // match - avoids a cosmetic "Not recognized" flash as a finger lifts off
    // right after a successful scan.
    if (millis() - lastGoodMatchTime > 1500) {
      beepError();
      showMainMessage("Not recognized", 2000);
      flashMainBackground(COLOR_RED, 700);
    }
    return -1;
  }

  uint8_t id = finger.fingerID;
  Serial.print("Match found - ID #"); Serial.print(id);
  Serial.print("  confidence: "); Serial.println(finger.confidence);
  lastGoodMatchTime = millis();

  now_ms = millis();
  if (now_ms - lastMatchTime[id] < MATCH_COOLDOWN_MS) {
    String existingName = preferences.getString(("name" + String(id)).c_str(), "ID " + String(id));
    showMainMessage(existingName + " - already recorded", 2000);
    beepPrompt();
    return id;
  }
  lastMatchTime[id] = now_ms;

  DateTime now = rtc.now();
  String timestamp = formatTimestamp(now);
  String name = preferences.getString(("name" + String(id)).c_str(), "ID " + String(id));
  String role = preferences.getString(("role" + String(id)).c_str(), "");

  checkedIn[id] = !checkedIn[id];
  preferences.putBytes("checkedIn", checkedIn, sizeof(checkedIn));

  if (checkedIn[id]) {
    Serial.println(name + " clocked IN at " + timestamp);
    beepIn();
    showMainMessage("Welcome IN - " + name, 2500);
    flashMainBackground(COLOR_GREEN, 700);
    launchSheetsPush(id, timestamp, "IN", name, role);
  } else {
    Serial.println(name + " clocked OUT at " + timestamp);
    beepOut();
    showMainMessage("Goodbye - " + name, 2500);
    flashMainBackground(COLOR_GREEN, 700);
    launchSheetsPush(id, timestamp, "OUT", name, role);
  }

  return id;
}
