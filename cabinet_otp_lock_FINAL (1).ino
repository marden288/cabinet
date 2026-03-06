/*
 * ================================================================
 *   CABINET OTP DOOR LOCK — RAM OPTIMIZED
 * ================================================================
 *  WIRING:
 *    SIM800L TX→D10  RX→D11
 *    Relay   IN→D12
 *    Keypad  R1-R4→D2,D3,D4,D5  C1-C4→D6,D7,D8,D9
 *    LCD I2C SDA→A4  SCL→A5
 *  SERIAL COMMANDS (9600 baud):
 *    L = print log    R = reset all
 * ================================================================
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>

// ── PINS ─────────────────────────────────────────────────────
#define RELAY_PIN    12
#define SIM_RX       10
#define SIM_TX       11

// ── CONFIG ───────────────────────────────────────────────────
#define UNLOCK_MS      5000UL
#define LOCKOUT_MS     30000UL
#define MAX_ATTEMPTS   5
#define OTP_LEN        4
#define PHONE_LEN      13
#define MAX_USERS      10     // reduced from 20 to save RAM

// Relay: change to LOW if your module is active-low
#define RELAY_ON   HIGH
#define RELAY_OFF  LOW

// ── LCD ──────────────────────────────────────────────────────
// Change 0x27 to 0x3F if screen is blank
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ── KEYPAD ───────────────────────────────────────────────────
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {2,3,4,5};
byte colPins[COLS]  = {6,7,8,9};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ── SIM800L ──────────────────────────────────────────────────
SoftwareSerial sim800(SIM_RX, SIM_TX);

// ── EEPROM LOG ───────────────────────────────────────────────
// [0-1] = count  [2..] = 10 x 16-byte entries
// entry: 13 bytes phone + 1 byte event + 2 padding
#define LOG_START   2
#define LOG_SIZE    16
#define LOG_MAX     10
#define EV_NEW      1
#define EV_OPEN     2
#define EV_FAIL     3

// ── USER RECORDS ─────────────────────────────────────────────
struct User {
  char phone[PHONE_LEN + 1];  // 14 bytes
  char otp[OTP_LEN + 1];      //  5 bytes
  bool active;                 //  1 byte
};
User users[MAX_USERS];
byte userCount = 0;

// ── STATE ────────────────────────────────────────────────────
enum State : byte {
  S_IDLE, S_PHONE, S_SENDING, S_OTP, S_OPEN, S_LOCKOUT
};
State state = S_IDLE;

// ── GLOBALS ──────────────────────────────────────────────────
char buf[12];
byte bufLen = 0;
int8_t userIdx = -1;
byte fails = 0;
unsigned long timer = 0;

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(9600);
  sim800.begin(9600);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  lcd.init();
  lcd.backlight();
  randomSeed(analogRead(A0));

  for (byte i = 0; i < MAX_USERS; i++) users[i].active = false;

  lcdMsg(F("  CABINET  LOCK "), F(" Initializing.. "), F("                "), F("                "));
  delay(1500);

  lcdMsg(F("Checking SIM800L"), F("Please wait...  "), F("                "), F("                "));
  sim800.println(F("AT"));
  delay(1000);
  bool ok = simHas("OK", 1500);
  if (ok) {
    lcdMsg(F("SIM800L  Ready! "), F("GSM OK          "), F("                "), F("                "));
  } else {
    lcdMsg(F("SIM800L ERROR!  "), F("Check wiring/SIM"), F("                "), F("                "));
  }
  delay(1500);
  showIdle();
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'L' || c == 'l') dumpLog();
    if (c == 'R' || c == 'r') resetAll();
  }

  char key = keypad.getKey();

  switch (state) {
    case S_IDLE:
      if (key == '#') startPhone();
      break;

    case S_PHONE:
      handlePhone(key);
      break;

    case S_SENDING:
      break;

    case S_OTP:
      handleOtp(key);
      break;

    case S_OPEN:
      if (millis() - timer >= UNLOCK_MS) lockDoor();
      break;

    case S_LOCKOUT:
      if (millis() - timer >= LOCKOUT_MS) {
        fails = 0;
        showIdle();
      } else {
        static unsigned long lu = 0;
        if (millis() - lu > 500) {
          lu = millis();
          byte sec = (LOCKOUT_MS - (millis() - timer)) / 1000 + 1;
          lcd.setCursor(0, 3);
          lcd.print(F("Wait: "));
          lcd.print(sec);
          lcd.print(F("s    "));
        }
      }
      break;
  }
}

// ================================================================
//  LCD HELPER
// ================================================================
void lcdMsg(const __FlashStringHelper* r0,
            const __FlashStringHelper* r1,
            const __FlashStringHelper* r2,
            const __FlashStringHelper* r3) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(r0);
  lcd.setCursor(0,1); lcd.print(r1);
  lcd.setCursor(0,2); lcd.print(r2);
  lcd.setCursor(0,3); lcd.print(r3);
}

void showIdle() {
  lcdMsg(F("===================="),
         F("  CABINET  LOCK   "),
         F("  Press # to Open "),
         F("===================="));
  state = S_IDLE;
}

// ================================================================
//  PHONE INPUT
// ================================================================
void startPhone() {
  bufLen = 0;
  memset(buf, 0, sizeof(buf));
  buf[0]='0'; buf[1]='9'; bufLen=2;
  state = S_PHONE;
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(F("Enter Phone No.:"));
  lcd.setCursor(0,1); lcd.print(F("09"));
  lcd.setCursor(0,3); lcd.print(F("*=del  #=confirm"));
}

void handlePhone(char key) {
  if (!key) return;
  if (key == 'A') { showIdle(); return; }
  if (key == '*') {
    if (bufLen > 2) { buf[--bufLen] = 0; redrawPhone(); }
    return;
  }
  if (key == '#') {
    if (bufLen == 11) processPhone();
    else {
      lcd.setCursor(0,2); lcd.print(F("Need 11 digits! "));
      delay(1200);
      lcd.setCursor(0,2); lcd.print(F("                "));
    }
    return;
  }
  if (key >= '0' && key <= '9' && bufLen < 11) {
    buf[bufLen++] = key;
    redrawPhone();
  }
}

void redrawPhone() {
  lcd.setCursor(0,1); lcd.print(F("           "));
  lcd.setCursor(0,1); lcd.print(buf);
}

void processPhone() {
  char phone[PHONE_LEN + 1];
  snprintf(phone, sizeof(phone), "+63%s", buf + 1);
  int idx = findUser(phone);
  if (idx >= 0) {
    userIdx = idx;
    lcdMsg(F(" Welcome  back! "),
           F("Use your OTP to "),
           F("   open door.   "),
           F("                "));
    delay(1800);
    startOtp();
  } else {
    if (userCount >= MAX_USERS) {
      lcdMsg(F("  System  Full! "),
             F(" Contact  Admin "),
             F("                "),
             F("                "));
      delay(3000);
      showIdle();
      return;
    }
    sendFlow(phone);
  }
}

// ================================================================
//  USER MANAGEMENT
// ================================================================
int findUser(const char* phone) {
  for (byte i = 0; i < MAX_USERS; i++)
    if (users[i].active && strcmp(users[i].phone, phone) == 0) return i;
  return -1;
}

int addUser(const char* phone, const char* otp) {
  for (byte i = 0; i < MAX_USERS; i++) {
    if (!users[i].active) {
      strncpy(users[i].phone, phone, PHONE_LEN);
      strncpy(users[i].otp,   otp,   OTP_LEN);
      users[i].active = true;
      userCount++;
      return i;
    }
  }
  return -1;
}

// ================================================================
//  OTP SEND
// ================================================================
void sendFlow(const char* phone) {
  char otp[OTP_LEN + 1];
  for (byte i = 0; i < OTP_LEN; i++) otp[i] = '0' + random(10);
  otp[OTP_LEN] = 0;

  userIdx = addUser(phone, otp);
  state = S_SENDING;

  lcd.clear();
  lcd.setCursor(0,0); lcd.print(F("Sending OTP...  "));
  lcd.setCursor(0,1); lcd.print(phone);
  lcd.setCursor(0,2); lcd.print(F("Please wait...  "));

  bool sent = sendSMS(phone, otp);

  if (sent) {
    writeLog(phone, EV_NEW);
    lcdMsg(F("   OTP  Sent!   "),
           F(" Check your SMS "),
           F("                "),
           F("                "));
    delay(2000);
    startOtp();
  } else {
    if (userIdx >= 0) {
      users[userIdx].active = false;
      userCount--;
      userIdx = -1;
    }
    lcdMsg(F("  SMS  Failed!  "),
           F("Check SIM/Load  "),
           F("                "),
           F("                "));
    delay(3000);
    showIdle();
  }
}

bool sendSMS(const char* phone, const char* otp) {
  flushSIM();
  sim800.println(F("AT+CMGF=1"));
  delay(600); flushSIM();

  sim800.print(F("AT+CMGS=\""));
  sim800.print(phone);
  sim800.println(F("\""));
  delay(1000); flushSIM();

  sim800.print(F("CABINET LOCK OTP\nCode: "));
  sim800.print(otp);
  sim800.print(F("\nDo not share."));
  sim800.write(26);

  return simHas("+CMGS:", 15000);
}

bool simHas(const char* expect, unsigned long timeout) {
  char resp[64];
  byte ri = 0;
  unsigned long start = millis();
  memset(resp, 0, sizeof(resp));
  while (millis() - start < timeout) {
    while (sim800.available() && ri < 63) resp[ri++] = sim800.read();
    resp[ri] = 0;
    if (strstr(resp, expect))  return true;
    if (strstr(resp, "ERROR")) return false;
    delay(100);
  }
  return false;
}

void flushSIM() {
  delay(200);
  while (sim800.available()) sim800.read();
}

// ================================================================
//  OTP INPUT
// ================================================================
void startOtp() {
  bufLen = 0;
  memset(buf, 0, sizeof(buf));
  state = S_OTP;
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(F("Enter 4-Digit OTP"));
  lcd.setCursor(6,2); lcd.print(F("_ _ _ _"));
  lcd.setCursor(0,3); lcd.print(F("*=del  A=cancel "));
}

void handleOtp(char key) {
  if (!key) return;
  if (key == 'A') { showIdle(); return; }
  if (key == '*') {
    if (bufLen > 0) { buf[--bufLen] = 0; redrawOtp(); }
    return;
  }
  if (key >= '0' && key <= '9' && bufLen < OTP_LEN) {
    buf[bufLen++] = key;
    redrawOtp();
    if (bufLen == OTP_LEN) { delay(200); verifyOtp(); }
  }
}

void redrawOtp() {
  lcd.setCursor(6,2);
  for (byte i = 0; i < OTP_LEN; i++) {
    lcd.print(i < bufLen ? '*' : '_');
    if (i < OTP_LEN-1) lcd.print(' ');
  }
}

void verifyOtp() {
  if (userIdx < 0) { showIdle(); return; }
  if (strcmp(buf, users[userIdx].otp) == 0) {
    fails = 0;
    writeLog(users[userIdx].phone, EV_OPEN);
    unlockDoor();
  } else {
    fails++;
    writeLog(users[userIdx].phone, EV_FAIL);
    if (fails >= MAX_ATTEMPTS) {
      lcdMsg(F(" ACCESS BLOCKED!"),
             F("Too many fails. "),
             F("  Wait 30 secs  "),
             F("                "));
      timer = millis();
      state = S_LOCKOUT;
    } else {
      lcd.clear();
      lcd.setCursor(0,0); lcd.print(F("  Wrong  OTP!   "));
      lcd.setCursor(0,1); lcd.print(F("Attempts left: "));
      lcd.print(MAX_ATTEMPTS - fails);
      delay(2000);
      startOtp();
    }
  }
}

// ================================================================
//  LOCK / UNLOCK
// ================================================================
void unlockDoor() {
  digitalWrite(RELAY_PIN, RELAY_ON);
  timer = millis();
  state = S_OPEN;
  lcdMsg(F("===================="),
         F("  ACCESS GRANTED  "),
         F("  Door  Unlocked! "),
         F("===================="));
}

void lockDoor() {
  digitalWrite(RELAY_PIN, RELAY_OFF);
  lcdMsg(F("                "),
         F("  Door  Locked. "),
         F(" Have a nice day"),
         F("                "));
  delay(2000);
  showIdle();
}

// ================================================================
//  EEPROM LOG
// ================================================================
void writeLog(const char* phone, byte ev) {
  uint16_t cnt = 0;
  EEPROM.get(0, cnt);
  int addr = LOG_START + ((cnt % LOG_MAX) * LOG_SIZE);
  for (byte i = 0; i < 13; i++)
    EEPROM.write(addr+i, i < (byte)strlen(phone) ? phone[i] : 0);
  EEPROM.write(addr+13, ev);
  EEPROM.write(addr+14, 0);
  EEPROM.write(addr+15, 0);
  EEPROM.put(0, ++cnt);
}

void dumpLog() {
  uint16_t cnt = 0;
  EEPROM.get(0, cnt);
  Serial.println(F("===== LOCK LOG ====="));
  Serial.print(F("Total: ")); Serial.println(cnt);
  byte total = (byte)min((int)cnt, LOG_MAX);
  for (byte i = 0; i < total; i++) {
    byte slot = (cnt <= LOG_MAX) ? i : (byte)((cnt - total + i) % LOG_MAX);
    int addr = LOG_START + (slot * LOG_SIZE);
    char phone[14] = {0};
    for (byte j = 0; j < 13; j++) phone[j] = EEPROM.read(addr+j);
    byte ev = EEPROM.read(addr+13);
    Serial.print(i+1); Serial.print(F(". "));
    Serial.print(phone); Serial.print(F(" -> "));
    if      (ev==EV_OPEN) Serial.println(F("OPENED"));
    else if (ev==EV_FAIL) Serial.println(F("WRONG OTP"));
    else if (ev==EV_NEW)  Serial.println(F("NEW USER"));
    else                  Serial.println(F("?"));
  }
  Serial.println(F("===================="));
}

void resetAll() {
  for (byte i = 0; i < MAX_USERS; i++) users[i].active = false;
  userCount = 0;
  uint16_t z = 0;
  EEPROM.put(0, z);
  Serial.println(F("Reset done."));
  lcdMsg(F("  System Reset! "),
         F(" Data  Cleared  "),
         F("                "),
         F("                "));
  delay(2000);
  showIdle();
}
