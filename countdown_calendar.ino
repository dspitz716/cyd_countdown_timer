/*
 * Countdown Calendar for ESP32-2432S028R (CYD)
 * Dark neon theme | NTP sync | 5 custom slots | Holiday homepage
 * Swipe left/right | On-screen keyboard | Persistent storage (fixed)
 *
 * Libraries needed (Library Manager):
 *   - TFT_eSPI
 *   - XPT2046_Touchscreen  by Paul Stoffregen
 *   - NTPClient            by Fabrice Weinberg
 *
 * Built-in (no install needed):
 *   - WiFi, time.h, Preferences
 */

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>

// ── WiFi credentials (entered by user, stored in flash) ───────
char savedSSID[33]     = "";
char savedPassword[65] = "";

// ── NTP / Timezone ────────────────────────────────────────────
#define NTP_SERVER      "pool.ntp.org"
#define WIFI_TIMEOUT_MS  8000

// POSIX timezone string — handles DST automatically.
// Uncomment the one line that matches your timezone:
//#define TIMEZONE  "EST5EDT,M3.2.0,M11.1.0"     // US Eastern
//#define TIMEZONE  "CST6CDT,M3.2.0,M11.1.0"    // US Central
//#define TIMEZONE  "MST7MDT,M3.2.0,M11.1.0"    // US Mountain
#define TIMEZONE  "PST8PDT,M3.2.0,M11.1.0"      // US Pacific
//#define TIMEZONE  "GMT0BST,M3.5.0/1,M10.5.0"  // UK
//#define TIMEZONE  "CET-1CEST,M3.5.0,M10.5.0/3"// Central Europe
//#define TIMEZONE  "AEST-10AEDT,M10.1.0,M4.1.0/3" // Australia Eastern

// ═══════════════════════════════════════════════════════════════
//   BIRTHDAYS — edit name, month, day here
//   Year is ignored — always counts to next occurrence
// ═══════════════════════════════════════════════════════════════
struct Birthday { const char* name; int month; int day; };
const Birthday BIRTHDAYS[] = {
  // ADD BIRTHDAYS: { "Name", month, day },
};
const int NUM_BIRTHDAYS = sizeof(BIRTHDAYS) / sizeof(BIRTHDAYS[0]);

// ═══════════════════════════════════════════════════════════════
//   HOLIDAYS — auto-calculated each year
// ═══════════════════════════════════════════════════════════════
// Included: Christmas, Thanksgiving, Halloween, New Year
// (Easter is calculated via algorithm)

// ── Hardware ──────────────────────────────────────────────────
#define TOUCH_CS  33
#define TOUCH_IRQ 36
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
TFT_eSPI tft = TFT_eSPI();
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER, 0, 60000);
Preferences prefs;

// ── Screen ────────────────────────────────────────────────────
#define SW 320
#define SH 240

// ── Colors ────────────────────────────────────────────────────
#define COL_BG       0x0841
#define COL_PANEL    0x1082
#define COL_ACCENT   0x07FF
#define COL_ACCENT2  0xF81F
#define COL_GREEN    0x07E0
#define COL_RED      0xF800
#define COL_ORANGE   0xFC00
#define COL_YELLOW   0xFFE0
#define COL_PURPLE   0x780F
#define COL_PINK     0xF81F
#define COL_TEXT     0xFFFF
#define COL_DIM      0x4208
#define COL_SELECTED 0x0410
#define COL_BORDER   0x02EF
#define COL_KEYBG    0x2104
#define COL_KEYFACE  0x3186

// Slot accent colors
const uint16_t SLOT_COLORS[5] = {
  0x07FF, 0xF81F, 0xFFE0, 0x07E0, 0xFC00
};

// ── Touch calibration ─────────────────────────────────────────
#define TOUCH_X_MIN  574
#define TOUCH_X_MAX  3705
#define TOUCH_Y_MIN  474
#define TOUCH_Y_MAX  3692

// ── Custom countdown slots ────────────────────────────────────
#define NUM_SLOTS 5
struct CountdownSlot { char name[20]; int year,month,day,hour,minute; };
CountdownSlot slots[NUM_SLOTS];
int activeSlot = 0;
const char* DEFAULT_NAMES[NUM_SLOTS] = {
  "Event 1","Event 2","Event 3","Event 4","Event 5"
};

// ── Homepage entries ──────────────────────────────────────────
struct HomeEntry {
  char  label[24];
  int   month, day;    // fixed date; year computed dynamically
  uint16_t color;
  const char* icon;    // short emoji-like text label
};
#define MAX_HOME_ENTRIES 12
HomeEntry homeEntries[MAX_HOME_ENTRIES];
int numHomeEntries = 0;

// ── Screens ───────────────────────────────────────────────────
enum Screen {
  SCREEN_WIFI,
  SCREEN_WIFI_SETUP,    // first-boot credential entry
  SCREEN_WIFI_CONFIRM,  // confirm/clear saved credentials
  SCREEN_HOME,         // holiday/birthday homepage (leftmost)
  SCREEN_COUNTDOWN,    // custom slots
  SCREEN_HOLIDAY_CD,   // full countdown for a tapped holiday/birthday
  SCREEN_SET_DATE,
  SCREEN_KEYBOARD,
  SCREEN_CONFIRM_CLEAR
};
Screen currentScreen = SCREEN_WIFI;

// ── Navigation state ─────────────────────────────────────────
// pageIndex: 0 = HOME, 1..5 = custom slots 1-5
int pageIndex = 0;

// Which home entry is open in full countdown view
int activeHolidayEntry = 0;

// ── Countdown display ─────────────────────────────────────────
long cdYears, cdMonths, cdDays, cdHours, cdMins, cdSecs;
unsigned long lastSecond = 0;

// ── Spinners ─────────────────────────────────────────────────
#define NUM_SPINNERS 5
struct Spinner { int value,minVal,maxVal,x,y,width; const char* label; };
Spinner spinners[NUM_SPINNERS];
int  activeSpinner=-1, dragStartY=0, dragStartVal=0;
bool dragging=false;

// ── Keyboard ─────────────────────────────────────────────────
char kbBuffer[20];
int  kbLen=0, kbTargetSlot=0;
enum KbMode { KB_LOWER, KB_UPPER, KB_NUM };
KbMode kbMode=KB_LOWER;

// ── Month names ───────────────────────────────────────────────
const char* MON[] = {
  "","JAN","FEB","MAR","APR","MAY","JUN",
  "JUL","AUG","SEP","OCT","NOV","DEC"
};

// ─────────────────────────────────────────────────────────────
//  DATE MATH
// ─────────────────────────────────────────────────────────────

int daysInMonth(int m, int y) {
  if (m==2) return (y%4==0&&(y%100!=0||y%400==0))?29:28;
  if (m==4||m==6||m==9||m==11) return 30;
  return 31;
}

void getNow(int &yr,int &mo,int &dy,int &hr,int &mn,int &sc) {
  time_t n=time(nullptr); struct tm* t=localtime(&n);
  yr=t->tm_year+1900; mo=t->tm_mon+1; dy=t->tm_mday;
  hr=t->tm_hour; mn=t->tm_min; sc=t->tm_sec;
}

// Returns the next occurrence year for a given month/day
int nextYearFor(int month, int day) {
  int yr,mo,dy,hr,mn,sc; getNow(yr,mo,dy,hr,mn,sc);
  if (mo > month || (mo==month && dy>=day)) return yr+1;
  return yr;
}

// Thanksgiving = 4th Thursday of November
void getThanksgiving(int year, int &mo, int &dy) {
  mo=11;
  struct tm t={}; t.tm_year=year-1900; t.tm_mon=10; t.tm_mday=1;
  mktime(&t);
  int wday=t.tm_wday; // 0=Sun,4=Thu
  int firstThursday = (wday<=4) ? (5-wday) : (12-wday);
  dy = firstThursday + 21; // 4th Thursday
}

// Easter (Anonymous Gregorian algorithm)
void getEaster(int year, int &mo, int &dy) {
  int a=year%19, b=year/100, c=year%100;
  int d=b/4, e=b%4, f=(b+8)/25, g=(b-f+1)/3;
  int h=(19*a+b-d-g+15)%30;
  int i=c/4, k=c%4;
  int l=(32+2*e+2*i-h-k)%7;
  int m=(a+11*h+22*l)/451;
  mo=(h+l-7*m+114)/31;
  dy=((h+l-7*m+114)%31)+1;
}

// Build homepage entries (called after time sync)
void buildHomeEntries() {
  numHomeEntries=0;

  auto addEntry=[&](const char* label, int month, int day, uint16_t color, const char* icon) {
    if (numHomeEntries>=MAX_HOME_ENTRIES) return;
    HomeEntry &e=homeEntries[numHomeEntries++];
    strncpy(e.label,label,23); e.label[23]='\0';
    e.month=month; e.day=day; e.color=color; e.icon=icon;
  };

  // Fixed holidays
  addEntry("Christmas",    12, 25, COL_RED,    "XMAS");
  addEntry("Thanksgiving",  0,  0, COL_ORANGE, "THNK"); // computed below
  addEntry("Halloween",    10, 31, COL_ORANGE, "HWIN");
  addEntry("New Year",      1,  1, COL_ACCENT, "NYR!");

  // Fix Thanksgiving (dynamic)
  int tmo,tdy; getThanksgiving(nextYearFor(11,1),tmo,tdy);
  homeEntries[1].month=tmo; homeEntries[1].day=tdy;

  // Birthdays
  for (int i=0;i<NUM_BIRTHDAYS;i++) {
    addEntry(BIRTHDAYS[i].name, BIRTHDAYS[i].month, BIRTHDAYS[i].day,
             COL_PINK, "BDAY");
  }
}

// Compute countdown to a fixed month/day (next occurrence)
void computeFixedCountdown(int month, int day) {
  int yr=nextYearFor(month,day);
  struct tm tgt={};
  tgt.tm_year=yr-1900; tgt.tm_mon=month-1; tgt.tm_mday=day;
  tgt.tm_hour=0; tgt.tm_min=0; tgt.tm_sec=0;
  time_t tgtE=mktime(&tgt);
  long diff=(long)(tgtE-time(nullptr));
  if (diff<=0){cdYears=cdMonths=cdDays=cdHours=cdMins=cdSecs=0;return;}
  cdSecs =diff%60; diff/=60;
  cdMins =diff%60; diff/=60;
  cdHours=diff%24; diff/=24;
  cdYears=diff/365; cdMonths=(diff%365)/30; cdDays=(diff%365)%30;
}

void computeCountdown() {
  if (currentScreen==SCREEN_HOME) return; // handled separately
  CountdownSlot &s=slots[activeSlot];
  struct tm tgt={};
  tgt.tm_year=s.year-1900; tgt.tm_mon=s.month-1; tgt.tm_mday=s.day;
  tgt.tm_hour=s.hour; tgt.tm_min=s.minute; tgt.tm_sec=0;
  time_t tgtE=mktime(&tgt);
  long diff=(long)(tgtE-time(nullptr));
  if (diff<=0){cdYears=cdMonths=cdDays=cdHours=cdMins=cdSecs=0;return;}
  cdSecs =diff%60; diff/=60;
  cdMins =diff%60; diff/=60;
  cdHours=diff%24; diff/=24;
  cdYears=diff/365; cdMonths=(diff%365)/30; cdDays=(diff%365)%30;
}

// ─────────────────────────────────────────────────────────────
//  STORAGE  (fixed — no longer pre-seeds with resetSlot)
// ─────────────────────────────────────────────────────────────

void saveSlot(int i) {
  prefs.begin("cdown",false);
  char k[12];
  snprintf(k,sizeof(k),"nm%d",  i); prefs.putString(k,slots[i].name);
  snprintf(k,sizeof(k),"yr%d",  i); prefs.putInt(k,  slots[i].year);
  snprintf(k,sizeof(k),"mo%d",  i); prefs.putInt(k,  slots[i].month);
  snprintf(k,sizeof(k),"dy%d",  i); prefs.putInt(k,  slots[i].day);
  snprintf(k,sizeof(k),"hr%d",  i); prefs.putInt(k,  slots[i].hour);
  snprintf(k,sizeof(k),"mn%d",  i); prefs.putInt(k,  slots[i].minute);
  // Mark slot as saved so we know it exists on next boot
  snprintf(k,sizeof(k),"ok%d",  i); prefs.putBool(k, true);
  prefs.end();
}

void resetSlotData(int i) {
  strncpy(slots[i].name,DEFAULT_NAMES[i],19); slots[i].name[19]='\0';
  slots[i].year=2026; slots[i].month=1; slots[i].day=1;
  slots[i].hour=0;    slots[i].minute=0;
  saveSlot(i);
}

void loadAllSlots() {
  prefs.begin("cdown",true); // read-only
  char k[12];
  for (int i=0;i<NUM_SLOTS;i++) {
    snprintf(k,sizeof(k),"ok%d",i);
    bool exists=prefs.getBool(k,false);
    if (exists) {
      // Slot was previously saved — load it
      snprintf(k,sizeof(k),"nm%d",i);
      String n=prefs.getString(k,DEFAULT_NAMES[i]);
      strncpy(slots[i].name,n.c_str(),19); slots[i].name[19]='\0';
      snprintf(k,sizeof(k),"yr%d",i); slots[i].year  =prefs.getInt(k,2026);
      snprintf(k,sizeof(k),"mo%d",i); slots[i].month =prefs.getInt(k,1);
      snprintf(k,sizeof(k),"dy%d",i); slots[i].day   =prefs.getInt(k,1);
      snprintf(k,sizeof(k),"hr%d",i); slots[i].hour  =prefs.getInt(k,0);
      snprintf(k,sizeof(k),"mn%d",i); slots[i].minute=prefs.getInt(k,0);
    } else {
      // First boot — set defaults in RAM only (saved when user edits)
      strncpy(slots[i].name,DEFAULT_NAMES[i],19); slots[i].name[19]='\0';
      slots[i].year=2026; slots[i].month=1; slots[i].day=1;
      slots[i].hour=0;    slots[i].minute=0;
    }
  }
  prefs.end();
}

void saveWifiCredentials(const char* ssid, const char* pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putBool("saved", true);
  prefs.end();
  strncpy(savedSSID,     ssid, 32); savedSSID[32]    = '\0';
  strncpy(savedPassword, pass, 64); savedPassword[64]= '\0';
}

bool loadWifiCredentials() {
  prefs.begin("wifi", true);
  bool exists = prefs.getBool("saved", false);
  if (exists) {
    String s = prefs.getString("ssid", "");
    String p = prefs.getString("pass", "");
    strncpy(savedSSID,     s.c_str(), 32); savedSSID[32]    = '\0';
    strncpy(savedPassword, p.c_str(), 64); savedPassword[64]= '\0';
  }
  prefs.end();
  return exists;
}

void clearWifiCredentials() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  savedSSID[0]     = '\0';
  savedPassword[0] = '\0';
}

// ─────────────────────────────────────────────────────────────
//  KEYBOARD LAYOUT (used by both WiFi setup and name editor)
// ─────────────────────────────────────────────────────────────

const char* KB_ROWS[3][3] = {
  {"qwertyuiop","QWERTYUIOP","1234567890"},
  {"asdfghjkl", "ASDFGHJKL", "!@#$%^&*("},
  {"zxcvbnm",   "ZXCVBNM",   "-_., /)+" }
};
#define KB_Y0 98
#define KB_RH 34
#define KB_KW 29
#define KB_KH 28

// ─────────────────────────────────────────────────────────────
//  WIFI SETUP SCREEN  (keyboard-driven SSID + password entry)
// ─────────────────────────────────────────────────────────────

// Setup state: which field is being edited
enum WifiSetupField { FIELD_SSID, FIELD_PASS };
WifiSetupField wifiSetupField = FIELD_SSID;
char wifiSetupSSID[33] = "";
char wifiSetupPass[65] = "";

void drawWifiSetupScreen() {
  tft.fillScreen(COL_BG);
  tft.fillRect(0,0,SW,28,COL_PANEL);
  tft.drawFastHLine(0,28,SW,COL_ACCENT);
  tft.setTextColor(COL_ACCENT,COL_PANEL); tft.setTextSize(2);
  tft.setCursor(8,7); tft.print("WIFI SETUP");

  // SSID field
  bool ssidActive = (wifiSetupField == FIELD_SSID);
  uint16_t ssidCol = ssidActive ? COL_ACCENT : COL_DIM;
  tft.drawRoundRect(8,32,304,22,4,ssidCol);
  if (ssidActive) tft.fillRoundRect(8,32,304,22,4,COL_SELECTED);
  tft.setTextColor(COL_DIM,ssidActive?COL_SELECTED:COL_BG); tft.setTextSize(1);
  tft.setCursor(14,34); tft.print("SSID:");
  tft.setTextColor(COL_TEXT,ssidActive?COL_SELECTED:COL_BG); tft.setTextSize(1);
  char ssidDisp[36];
  snprintf(ssidDisp,sizeof(ssidDisp),"%s%s", wifiSetupSSID, ssidActive?"_":"");
  tft.setCursor(48,34); tft.print(ssidDisp);

  // Password field
  bool passActive = (wifiSetupField == FIELD_PASS);
  uint16_t passCol = passActive ? COL_ACCENT : COL_DIM;
  tft.drawRoundRect(8,58,304,22,4,passCol);
  if (passActive) tft.fillRoundRect(8,58,304,22,4,COL_SELECTED);
  tft.setTextColor(COL_DIM,passActive?COL_SELECTED:COL_BG); tft.setTextSize(1);
  tft.setCursor(14,60); tft.print("PASS:");
  tft.setTextColor(COL_TEXT,passActive?COL_SELECTED:COL_BG);
  tft.setCursor(48,60); tft.print(wifiSetupPass);
  if (passActive) tft.print("_");

  // Keyboard — starts at y=84, rows compressed to fit
  // 3 letter rows at KH=26 each = 78px, special row 26px = 104px total → ends at 188
  #define WK_Y0  84
  #define WK_RH  26
  #define WK_KH  24

  int mode = (kbMode==KB_LOWER)?0:(kbMode==KB_UPPER)?1:2;
  for (int row=0;row<3;row++) {
    const char* r=KB_ROWS[row][mode]; int len=strlen(r),rowW=len*KB_KW;
    int xs=(SW-rowW)/2, ky=WK_Y0+row*WK_RH;
    for (int k=0;k<len;k++) {
      int kx=xs+k*KB_KW;
      tft.fillRoundRect(kx+1,ky+1,KB_KW-2,WK_KH-2,4,COL_KEYFACE);
      tft.drawRoundRect(kx+1,ky+1,KB_KW-2,WK_KH-2,4,COL_BORDER);
      tft.setTextColor(COL_TEXT,COL_KEYFACE); tft.setTextSize(2);
      tft.setCursor(kx+7,ky+5); tft.print(r[k]);
    }
  }

  // Special row: 123 | CAP | SPC | <DEL | NEXT/CONNECT
  int sy = WK_Y0 + 3*WK_RH;
  bool numOn=(kbMode==KB_NUM), capOn=(kbMode==KB_UPPER);

  tft.fillRoundRect(2,   sy+1, 44, WK_KH-2, 4, numOn?COL_ACCENT:COL_KEYFACE);
  tft.drawRoundRect(2,   sy+1, 44, WK_KH-2, 4, COL_BORDER);
  tft.setTextColor(numOn?COL_BG:COL_TEXT, numOn?COL_ACCENT:COL_KEYFACE);
  tft.setTextSize(1); tft.setCursor(8, sy+9); tft.print(numOn?"ABC":"123");

  tft.fillRoundRect(50,  sy+1, 38, WK_KH-2, 4, capOn?COL_ACCENT:COL_KEYFACE);
  tft.drawRoundRect(50,  sy+1, 38, WK_KH-2, 4, COL_BORDER);
  tft.setTextColor(capOn?COL_BG:COL_TEXT, capOn?COL_ACCENT:COL_KEYFACE);
  tft.setTextSize(1); tft.setCursor(55, sy+9); tft.print("CAP");

  tft.fillRoundRect(92,  sy+1, 52, WK_KH-2, 4, COL_KEYFACE);
  tft.drawRoundRect(92,  sy+1, 52, WK_KH-2, 4, COL_BORDER);
  tft.setTextColor(COL_TEXT, COL_KEYFACE);
  tft.setTextSize(1); tft.setCursor(106, sy+9); tft.print("SPC");

  tft.fillRoundRect(148, sy+1, 48, WK_KH-2, 4, COL_KEYFACE);
  tft.drawRoundRect(148, sy+1, 48, WK_KH-2, 4, COL_BORDER);
  tft.setTextColor(COL_RED, COL_KEYFACE);
  tft.setTextSize(1); tft.setCursor(152, sy+9); tft.print("<DEL");

  // NEXT or CONNECT
  uint16_t actionCol = ssidActive ? COL_ACCENT : COL_GREEN;
  tft.fillRoundRect(200, sy+1, 118, WK_KH-2, 4, actionCol);
  tft.setTextColor(COL_BG, actionCol);
  tft.setTextSize(1); tft.setCursor(ssidActive?222:206, sy+9);
  tft.print(ssidActive ? "NEXT >" : "CONNECT");
}

void handleWifiSetupTouch(int tx, int ty) {
  #define WK_Y0  84
  #define WK_RH  26
  #define WK_KH  24
  int sy = WK_Y0 + 3*WK_RH;

  // Field selection by tapping the input boxes
  if (ty>=32&&ty<=54&&tx>=8&&tx<=312)  { wifiSetupField=FIELD_SSID; drawWifiSetupScreen(); return; }
  if (ty>=58&&ty<=80&&tx>=8&&tx<=312)  { wifiSetupField=FIELD_PASS; drawWifiSetupScreen(); return; }

  // Special bottom row
  if (ty>=sy && ty<=sy+WK_KH) {
    if (tx>=2  &&tx<=46)  { kbMode=(kbMode==KB_NUM)?KB_LOWER:KB_NUM;     drawWifiSetupScreen(); return; }
    if (tx>=50 &&tx<=88)  { kbMode=(kbMode==KB_UPPER)?KB_LOWER:KB_UPPER; drawWifiSetupScreen(); return; }
    if (tx>=92 &&tx<=144) {
      // Space
      if (wifiSetupField==FIELD_SSID && strlen(wifiSetupSSID)<32) { int l=strlen(wifiSetupSSID); wifiSetupSSID[l]=' '; wifiSetupSSID[l+1]='\0'; }
      if (wifiSetupField==FIELD_PASS && strlen(wifiSetupPass)<64) { int l=strlen(wifiSetupPass); wifiSetupPass[l]=' '; wifiSetupPass[l+1]='\0'; }
      drawWifiSetupScreen(); return;
    }
    if (tx>=148&&tx<=196) {
      // Backspace
      if (wifiSetupField==FIELD_SSID) { int l=strlen(wifiSetupSSID); if(l>0) wifiSetupSSID[l-1]='\0'; }
      if (wifiSetupField==FIELD_PASS) { int l=strlen(wifiSetupPass); if(l>0) wifiSetupPass[l-1]='\0'; }
      drawWifiSetupScreen(); return;
    }
    if (tx>=200&&tx<=318) {
      if (wifiSetupField==FIELD_SSID) {
        // NEXT — move to password field
        wifiSetupField=FIELD_PASS; kbMode=KB_LOWER; drawWifiSetupScreen();
      } else {
        // CONNECT — save and attempt connection
        if (strlen(wifiSetupSSID)>0) {
          saveWifiCredentials(wifiSetupSSID, wifiSetupPass);
          bool ok=connectAndSync();
          if (!ok) {
            clearWifiCredentials();
            wifiSetupSSID[0]='\0'; wifiSetupPass[0]='\0';
            tft.fillScreen(COL_BG);
            tft.setTextColor(COL_RED,COL_BG); tft.setTextSize(2);
            tft.setCursor(20,100); tft.print("Connection failed!");
            tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
            tft.setCursor(20,130); tft.print("Check SSID and password.");
            delay(2500);
            currentScreen=SCREEN_WIFI_SETUP;
            drawWifiSetupScreen();
          } else {
            loadAllSlots();
            buildHomeEntries();
            pageIndex=0; currentScreen=SCREEN_HOME; drawHomePage();
          }
        }
      }
      return;
    }
  }

  // Letter rows
  int mode=(kbMode==KB_LOWER)?0:(kbMode==KB_UPPER)?1:2;
  for (int row=0;row<3;row++) {
    int ky=WK_Y0+row*WK_RH;
    if (ty<ky||ty>ky+WK_KH) continue;
    const char* r=KB_ROWS[row][mode]; int len=strlen(r),rowW=len*KB_KW;
    int xs=(SW-rowW)/2;
    for (int k=0;k<len;k++) {
      int kx=xs+k*KB_KW;
      if (tx>=kx&&tx<=kx+KB_KW) {
        char c=r[k];
        if (wifiSetupField==FIELD_SSID && strlen(wifiSetupSSID)<32) { int l=strlen(wifiSetupSSID); wifiSetupSSID[l]=c; wifiSetupSSID[l+1]='\0'; }
        if (wifiSetupField==FIELD_PASS && strlen(wifiSetupPass)<64) { int l=strlen(wifiSetupPass); wifiSetupPass[l]=c; wifiSetupPass[l+1]='\0'; }
        if (kbMode==KB_UPPER) kbMode=KB_LOWER;
        drawWifiSetupScreen(); return;
      }
    }
  }
}

// Confirm clear WiFi screen
void drawWifiConfirmScreen() {
  tft.fillScreen(COL_BG);
  tft.fillRoundRect(30,60,260,130,12,COL_PANEL);
  tft.drawRoundRect(30,60,260,130,12,COL_ORANGE);
  tft.setTextColor(COL_ORANGE,COL_PANEL); tft.setTextSize(2);
  tft.setCursor(52,78); tft.print("CHANGE WIFI?");
  tft.setTextColor(COL_TEXT,COL_PANEL); tft.setTextSize(1);
  tft.setCursor(46,104);
  char buf[34]; snprintf(buf,sizeof(buf),"Current: %.28s",savedSSID);
  tft.print(buf);
  tft.setCursor(46,118); tft.print("This will clear saved credentials.");
  tft.fillRoundRect(44,162,96,24,6,COL_DIM);
  tft.setTextColor(COL_TEXT,COL_DIM); tft.setTextSize(2);
  tft.setCursor(68,167); tft.print("NO");
  tft.fillRoundRect(180,162,96,24,6,COL_ORANGE);
  tft.setTextColor(COL_BG,COL_ORANGE); tft.setTextSize(2);
  tft.setCursor(194,167); tft.print("YES");
}

bool getTouchPoint(int &tx,int &ty) {
  if (!ts.tirqTouched()||!ts.touched()) return false;
  TS_Point p=ts.getPoint();
  tx=constrain(map(p.x,TOUCH_X_MIN,TOUCH_X_MAX,0,SW),0,SW-1);
  ty=constrain(map(p.y,TOUCH_Y_MIN,TOUCH_Y_MAX,0,SH),0,SH-1);
  return true;
}

// ─────────────────────────────────────────────────────────────
//  WIFI ICON
// ─────────────────────────────────────────────────────────────

void drawWifiRing(int cx,int cy,int ring,uint16_t col) {
  int r[]={8,17,26};
  for (int a=225;a<=315;a+=3) {
    float rad=a*3.14159f/180.0f;
    tft.fillCircle(cx+(int)(r[ring]*cosf(rad)),cy+(int)(r[ring]*sinf(rad))+14,2,col);
  }
}
void drawWifiIcon(int cx,int cy,uint16_t col) {
  tft.fillRect(cx-36,cy-30,72,50,COL_BG);
  for (int r=0;r<3;r++) drawWifiRing(cx,cy,r,col);
  tft.fillCircle(cx,cy+14,4,col);
}

// ─────────────────────────────────────────────────────────────
//  WIFI / NTP
// ─────────────────────────────────────────────────────────────

bool connectAndSync() {
  tft.fillScreen(COL_BG);
  tft.fillRect(0,0,SW,30,COL_PANEL); tft.drawFastHLine(0,30,SW,COL_ACCENT);
  tft.setTextColor(COL_ACCENT,COL_PANEL); tft.setTextSize(2);
  tft.setCursor(8,7); tft.print("CONNECTING...");
  drawWifiIcon(SW/2,78,COL_DIM);
  tft.setTextColor(COL_TEXT,COL_BG); tft.setTextSize(2);
  tft.setCursor((SW-min((int)strlen(savedSSID)*12,SW-16))/2,118); tft.print(savedSSID);

  WiFi.disconnect(true); delay(200);
  WiFi.begin(savedSSID, savedPassword);
  unsigned long start=millis(); int frame=0;
  while (WiFi.status()!=WL_CONNECTED) {
    if (millis()-start>WIFI_TIMEOUT_MS) break;
    int rings=(frame/5)%4;
    drawWifiIcon(SW/2,78,COL_DIM);
    for (int r=0;r<rings;r++) drawWifiRing(SW/2,78,r,COL_ACCENT);
    tft.fillCircle(SW/2,92,4,rings>0?COL_ACCENT:COL_DIM);
    tft.fillRect(0,188,SW,14,COL_BG); tft.setTextColor(COL_ACCENT,COL_BG); tft.setTextSize(1);
    char dots[10]="        "; for(int j=0;j<=(frame/3)%8;j++) dots[j]='.';
    char buf[28]; snprintf(buf,sizeof(buf),"Connecting%s",dots);
    tft.setCursor((SW-(int)strlen(buf)*6)/2,190); tft.print(buf);
    frame++; delay(120);
  }
  if (WiFi.status()==WL_CONNECTED) {
    drawWifiIcon(SW/2,78,COL_GREEN);
    tft.fillRect(0,108,SW,90,COL_BG);
    tft.setTextColor(COL_GREEN,COL_BG); tft.setTextSize(2);
    tft.setCursor((SW-168)/2,115); tft.print("CONNECTED!");
    tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
    tft.setCursor((SW-(int)strlen(savedSSID)*6)/2,140); tft.print(savedSSID);
    tft.setCursor(34,158); tft.print("Fetching time from NTP server...");
    configTime(0, 0, NTP_SERVER);
    setenv("TZ", TIMEZONE, 1); tzset();
    struct tm ti; int att=0; bool synced=false;
    while (att<20&&!synced) {
      if (getLocalTime(&ti)) synced=true;
      tft.fillRect(0,172,SW,12,COL_BG); tft.setTextColor(COL_ACCENT,COL_BG);
      char dots[10]="        "; for(int j=0;j<=att%8;j++) dots[j]='.';
      char buf[24]; snprintf(buf,sizeof(buf),"NTP sync%s",dots);
      tft.setCursor((SW-(int)strlen(buf)*6)/2,174); tft.print(buf);
      att++; delay(400);
    }
    if (!synced) { delay(2000); return false; }
    tft.fillRect(0,172,SW,40,COL_BG); tft.setTextColor(COL_GREEN,COL_BG); tft.setTextSize(1);
    char tbuf[36];
    snprintf(tbuf,sizeof(tbuf),"%04d-%02d-%02d   %02d:%02d:%02d",
      ti.tm_year+1900,ti.tm_mon+1,ti.tm_mday,ti.tm_hour,ti.tm_min,ti.tm_sec);
    tft.setCursor((SW-(int)strlen(tbuf)*6)/2,178); tft.print(tbuf);
    tft.setTextColor(COL_ACCENT,COL_BG);
    tft.setCursor((SW-78)/2,196); tft.print("Loading app...");
    delay(1600); WiFi.disconnect(true); return true;
  }
  return false;
}

// ─────────────────────────────────────────────────────────────
//  HOMEPAGE  (swipe left of slot 1)
// ─────────────────────────────────────────────────────────────

int homeSelectedEntry = 0; // which entry is expanded/active

// Compute days until a home entry
long daysUntilEntry(int idx) {
  HomeEntry &e=homeEntries[idx];
  int yr=nextYearFor(e.month,e.day);
  struct tm tgt={};
  tgt.tm_year=yr-1900; tgt.tm_mon=e.month-1; tgt.tm_mday=e.day;
  time_t tgtE=mktime(&tgt);
  long diff=(long)(tgtE-time(nullptr));
  return diff>0 ? diff/86400 : 0;
}

void drawHomePage() {
  tft.fillScreen(COL_BG);

  HomeEntry &e = homeEntries[homeSelectedEntry];
  uint16_t ac  = e.color;

  // ── Top bar ──
  tft.fillRect(0,0,SW,28,COL_PANEL);
  tft.drawFastHLine(0,28,SW,COL_YELLOW);
  tft.setTextColor(COL_YELLOW,COL_PANEL); tft.setTextSize(2);
  tft.setCursor(8,7); tft.print("HOLIDAYS");

  // Live clock
  int yr,mo,dy,hr,mn,sc; getNow(yr,mo,dy,hr,mn,sc);
  tft.setTextColor(COL_DIM,COL_PANEL); tft.setTextSize(1);
  char tbuf[12]; snprintf(tbuf,sizeof(tbuf),"%02d:%02d:%02d",hr,mn,sc);
  tft.setCursor(SW-(int)strlen(tbuf)*6-4,10); tft.print(tbuf);

  // Swipe right hint
  tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
  tft.setCursor(SW-8,112); tft.print(">");

  // ── UP / DOWN nav buttons (left column) ──
  // UP arrow button
  bool hasUp   = (homeSelectedEntry > 0);
  bool hasDown = (homeSelectedEntry < numHomeEntries-1);

  uint16_t upCol   = hasUp   ? COL_ACCENT : COL_DIM;
  uint16_t downCol = hasDown ? COL_ACCENT : COL_DIM;

  // UP button  (top-left area)
  tft.fillRoundRect(4, 38, 28, 36, 5, hasUp ? 0x0C20 : COL_BG);
  tft.drawRoundRect(4, 38, 28, 36, 5, upCol);
  tft.setTextColor(upCol, hasUp ? 0x0C20 : COL_BG); tft.setTextSize(2);
  tft.setCursor(10, 47); tft.print("^");

  // DOWN button (bottom-left area)
  tft.fillRoundRect(4, 140, 28, 36, 5, hasDown ? 0x0C20 : COL_BG);
  tft.drawRoundRect(4, 140, 28, 36, 5, downCol);
  tft.setTextColor(downCol, hasDown ? 0x0C20 : COL_BG); tft.setTextSize(2);
  tft.setCursor(10, 149); tft.print("v");

  // ── Entry counter (x of n) ──
  tft.setTextColor(COL_DIM, COL_BG); tft.setTextSize(1);
  char cntbuf[12];
  snprintf(cntbuf, sizeof(cntbuf), "%d / %d", homeSelectedEntry+1, numHomeEntries);
  tft.setCursor(10, 92); tft.print(cntbuf);

  // ── Main entry card (right of nav buttons) ──
  int cardX = 38, cardY = 34, cardW = SW-46, cardH = 152;
  tft.fillRoundRect(cardX, cardY, cardW, cardH, 8, COL_PANEL);
  tft.drawRoundRect(cardX, cardY, cardW, cardH, 8, ac);

  // Color accent bar at top of card
  tft.fillRoundRect(cardX, cardY, cardW, 5, 4, ac);

  // Icon badge
  tft.fillRoundRect(cardX+8, cardY+12, 36, 22, 4, ac);
  tft.setTextColor(COL_BG, ac); tft.setTextSize(1);
  int iw = strlen(e.icon)*6;
  tft.setCursor(cardX+8+(36-iw)/2, cardY+17); tft.print(e.icon);

  // Holiday name
  tft.setTextColor(ac, COL_PANEL); tft.setTextSize(2);
  tft.setCursor(cardX+52, cardY+12); tft.print(e.label);

  // Date line
  int nextYr = nextYearFor(e.month, e.day);
  tft.setTextColor(COL_DIM, COL_PANEL); tft.setTextSize(1);
  char dbuf[24];
  snprintf(dbuf, sizeof(dbuf), "%s %d, %04d", MON[e.month], e.day, nextYr);
  tft.setCursor(cardX+52, cardY+32); tft.print(dbuf);

  // Divider
  tft.drawFastHLine(cardX+8, cardY+46, cardW-16, COL_BORDER);

  // Days remaining big number
  long days = daysUntilEntry(homeSelectedEntry);
  tft.setTextColor(ac, COL_PANEL); tft.setTextSize(4);
  char daysbuf[8];
  if (days==0) snprintf(daysbuf, sizeof(daysbuf), "NOW!");
  else         snprintf(daysbuf, sizeof(daysbuf), "%ld", days);
  int dw = strlen(daysbuf)*24;
  tft.setCursor(cardX+(cardW-dw)/2, cardY+54); tft.print(daysbuf);

  tft.setTextColor(COL_DIM, COL_PANEL); tft.setTextSize(1);
  if (days>0) {
    int lw2 = 4*6;
    tft.setCursor(cardX+(cardW-lw2)/2, cardY+96); tft.print("DAYS");
  }

  // H:M:S row
  computeFixedCountdown(e.month, e.day);
  tft.setTextColor(COL_TEXT, COL_PANEL); tft.setTextSize(1);
  char hmsbuf[32];
  snprintf(hmsbuf, sizeof(hmsbuf), "%02ldh  %02ldm  %02lds",
           cdHours, cdMins, cdSecs);
  int hw = strlen(hmsbuf)*6;
  tft.setCursor(cardX+(cardW-hw)/2, cardY+112); tft.print(hmsbuf);

  // Tap card hint
  tft.setTextColor(COL_DIM, COL_PANEL); tft.setTextSize(1);
  const char* hint = "tap for full countdown";
  int hintw = strlen(hint)*6;
  tft.setCursor(cardX+(cardW-hintw)/2, cardY+130); tft.print(hint);

  // ── Bottom: full countdown button ──
  tft.fillRoundRect(cardX, cardY+cardH-1, cardW, 22, 6, ac);
  tft.setTextColor(COL_BG, ac); tft.setTextSize(1);
  const char* btn = "OPEN FULL COUNTDOWN";
  int bw2 = strlen(btn)*6;
  tft.setCursor(cardX+(cardW-bw2)/2, cardY+cardH+5); tft.print(btn);

  // Page dots
  drawAllPageDots();
}

void updateHomeClock() {
  // Only refresh the clock text and the H:M:S line — no full redraw
  int yr,mo,dy,hr,mn,sc; getNow(yr,mo,dy,hr,mn,sc);
  tft.fillRect(SW-60,4,56,18,COL_PANEL);
  tft.setTextColor(COL_DIM,COL_PANEL); tft.setTextSize(1);
  char tbuf[12]; snprintf(tbuf,sizeof(tbuf),"%02d:%02d:%02d",hr,mn,sc);
  tft.setCursor(SW-(int)strlen(tbuf)*6-4,10); tft.print(tbuf);

  // Refresh H:M:S line inside card
  HomeEntry &e = homeEntries[homeSelectedEntry];
  computeFixedCountdown(e.month, e.day);
  int cardX=38, cardY=34, cardW=SW-46;
  tft.fillRect(cardX+1, cardY+108, cardW-2, 12, COL_PANEL);
  tft.setTextColor(COL_TEXT, COL_PANEL); tft.setTextSize(1);
  char hmsbuf[32];
  snprintf(hmsbuf,sizeof(hmsbuf),"%02ldh  %02ldm  %02lds",cdHours,cdMins,cdSecs);
  int hw=strlen(hmsbuf)*6;
  tft.setCursor(cardX+(cardW-hw)/2, cardY+112); tft.print(hmsbuf);
}

// ─────────────────────────────────────────────────────────────
//  PAGE INDICATOR DOTS  (home + 5 slots = 6 total)
// ─────────────────────────────────────────────────────────────

void drawAllPageDots() {
  // 6 dots total: index 0 = home, 1-5 = slots
  int totalPages=1+NUM_SLOTS;
  int spacing=14;
  int totalW=totalPages*spacing;
  int sx=(SW-totalW)/2+spacing/2;
  for (int i=0;i<totalPages;i++) {
    int r=(i==pageIndex)?4:2;
    uint16_t col;
    if (i==pageIndex) col=(pageIndex==0)?COL_YELLOW:SLOT_COLORS[pageIndex-1];
    else              col=COL_DIM;
    tft.fillCircle(sx+i*spacing,SH-7,r,col);
  }
}

// ─────────────────────────────────────────────────────────────
//  TRASH ICON
// ─────────────────────────────────────────────────────────────

void drawTrashIcon(int x,int y,uint16_t col) {
  tft.fillRect(x,   y,   14, 2,col);
  tft.fillRect(x+4, y-3,  6, 3,col);
  tft.fillRect(x+1, y+3, 12,11,col);
  tft.fillRect(x+3, y+5,  2, 7,COL_BG);
  tft.fillRect(x+6, y+5,  2, 7,COL_BG);
  tft.fillRect(x+9, y+5,  2, 7,COL_BG);
}

// ─────────────────────────────────────────────────────────────
//  HOLIDAY FULL COUNTDOWN SCREEN
// ─────────────────────────────────────────────────────────────

// Forward declaration — defined later in COUNTDOWN SCREEN section
void drawCdBox(int x,int y,int w,int h,long val,const char* lbl,uint16_t ac);

void drawHolidayCdScreen() {
  HomeEntry &e = homeEntries[activeHolidayEntry];
  uint16_t ac  = e.color;
  computeFixedCountdown(e.month, e.day);

  tft.fillScreen(COL_BG);

  // Top bar
  tft.fillRect(0,0,SW,28,COL_PANEL);
  tft.drawFastHLine(0,28,SW,ac);

  // Holiday name
  tft.setTextColor(ac,COL_PANEL); tft.setTextSize(2);
  tft.setCursor(8,7); tft.print(e.label);

  // Live clock
  int yr,mo,dy,hr,mn,sc; getNow(yr,mo,dy,hr,mn,sc);
  tft.setTextColor(COL_DIM,COL_PANEL); tft.setTextSize(1);
  char tbuf[12]; snprintf(tbuf,sizeof(tbuf),"%02d:%02d:%02d",hr,mn,sc);
  tft.setCursor(SW-(int)strlen(tbuf)*6-4,10); tft.print(tbuf);

  // BACK button
  tft.fillRoundRect(SW-62,34,56,22,4,ac);
  tft.setTextColor(COL_BG,ac); tft.setTextSize(1);
  tft.setCursor(SW-57,41); tft.print("< BACK");

  // Target date label
  int nextYr = nextYearFor(e.month, e.day);
  tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
  char buf[36];
  snprintf(buf,sizeof(buf),"TO: %s %d, %04d", MON[e.month], e.day, nextYr);
  tft.setCursor(6,41); tft.print(buf);

  // Top row: 2 wide boxes — MONTHS | DAYS
  // Each = 155px wide with 4px gap, starting at x=3
  drawCdBox(  3, 60,155,78,cdMonths,"MONTHS",ac);
  drawCdBox(162, 60,155,78,cdDays,  "DAYS",  ac);

  // Bottom row: 3 equal boxes — HOURS | MINS | SECS
  // Each = 100px wide with 5px gap, starting at x=3
  drawCdBox(  3,148,100,76,cdHours,"HOURS",ac);
  drawCdBox(109,148,100,76,cdMins, "MINS", ac);
  drawCdBox(215,148,100,76,cdSecs, "SECS", ac);
}

void updateHolidayCdValues() {
  HomeEntry &e = homeEntries[activeHolidayEntry];
  uint16_t ac  = e.color;
  computeFixedCountdown(e.month, e.day);

  auto rv=[&](int x,int y,int w,long val){
    tft.fillRect(x,y+8,w,28,COL_BG);
    tft.setTextColor(ac,COL_BG); tft.setTextSize(3);
    char buf[8]; snprintf(buf,sizeof(buf),"%ld",val);
    int tw=strlen(buf)*18;
    tft.setCursor(x+(w-tw)/2,y+10); tft.print(buf);
  };
  rv(  3, 60,155,cdMonths);
  rv(162, 60,155,cdDays);
  rv(  3,148,100,cdHours);
  rv(109,148,100,cdMins);
  rv(215,148,100,cdSecs);

  // Refresh clock
  int yr,mo,dy,hr,mn,sc; getNow(yr,mo,dy,hr,mn,sc);
  tft.fillRect(SW-60,4,56,18,COL_PANEL);
  tft.setTextColor(COL_DIM,COL_PANEL); tft.setTextSize(1);
  char tbuf[12]; snprintf(tbuf,sizeof(tbuf),"%02d:%02d:%02d",hr,mn,sc);
  tft.setCursor(SW-(int)strlen(tbuf)*6-4,10); tft.print(tbuf);
}

// ─────────────────────────────────────────────────────────────
//  COUNTDOWN SCREEN  (custom slots)
// ─────────────────────────────────────────────────────────────

void drawCdBox(int x,int y,int w,int h,long val,const char* lbl,uint16_t ac) {
  tft.drawRoundRect(x-1,y-1,w+2,h+2,6,COL_BORDER);
  tft.setTextColor(ac,COL_BG); tft.setTextSize(3);
  char buf[8]; snprintf(buf,sizeof(buf),"%ld",val);
  int tw=strlen(buf)*18;
  tft.setCursor(x+(w-tw)/2,y+10); tft.print(buf);
  tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
  int lw=strlen(lbl)*6;
  tft.setCursor(x+(w-lw)/2,y+h-14); tft.print(lbl);
}

void drawCountdownScreen() {
  uint16_t ac=SLOT_COLORS[activeSlot];
  tft.fillScreen(COL_BG);
  tft.fillRect(0,0,SW,28,COL_PANEL);
  tft.drawFastHLine(0,28,SW,ac);
  tft.setTextColor(ac,COL_PANEL); tft.setTextSize(2);
  tft.setCursor(8,7); tft.print(slots[activeSlot].name);
  int yr,mo,dy,hr,mn,sc; getNow(yr,mo,dy,hr,mn,sc);
  tft.setTextColor(COL_DIM,COL_PANEL); tft.setTextSize(1);
  char tbuf[12]; snprintf(tbuf,sizeof(tbuf),"%02d:%02d:%02d",hr,mn,sc);
  tft.setCursor(SW-(int)strlen(tbuf)*6-4,10); tft.print(tbuf);
  tft.fillRoundRect(SW-72,34,68,22,4,ac);
  tft.setTextColor(COL_BG,ac); tft.setTextSize(1);
  tft.setCursor(SW-67,41); tft.print("SET DATE");
  tft.fillRoundRect(SW-96,34,22,22,4,0x4000);
  tft.drawRoundRect(SW-96,34,22,22,4,COL_RED);
  drawTrashIcon(SW-92,39,COL_RED);
  // WiFi settings button (bottom-right)
  tft.fillRoundRect(SW-46,SH-22,44,18,3,COL_DIM);
  tft.setTextColor(COL_TEXT,COL_DIM); tft.setTextSize(1);
  tft.setCursor(SW-42,SH-17); tft.print("WiFi");
  tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
  char buf[36];
  snprintf(buf,sizeof(buf),"TO: %s %02d, %04d  %02d:%02d",
    MON[slots[activeSlot].month],slots[activeSlot].day,
    slots[activeSlot].year,slots[activeSlot].hour,slots[activeSlot].minute);
  tft.setCursor(6,41); tft.print(buf);
  drawCdBox(  8, 62,92,72,cdYears, "YEARS",  ac);
  drawCdBox(114, 62,92,72,cdMonths,"MONTHS", ac);
  drawCdBox(220, 62,92,72,cdDays,  "DAYS",   ac);
  drawCdBox(  8,146,92,72,cdHours, "HOURS",  ac);
  drawCdBox(114,146,92,72,cdMins,  "MINS",   ac);
  drawCdBox(220,146,92,72,cdSecs,  "SECS",   ac);
  tft.drawFastVLine(106, 59,78,COL_BORDER);
  tft.drawFastVLine(212, 59,78,COL_BORDER);
  tft.drawFastVLine(106,143,78,COL_BORDER);
  tft.drawFastVLine(212,143,78,COL_BORDER);
  tft.drawFastHLine(0,143,SW,COL_BORDER);
  tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
  if (pageIndex>1)              { tft.setCursor(2,112); tft.print("<"); }
  if (pageIndex<NUM_SLOTS)      { tft.setCursor(SW-8,112); tft.print(">"); }
  drawAllPageDots();
}

void updateCountdownValues() {
  uint16_t ac=SLOT_COLORS[activeSlot];
  auto rv=[&](int x,int y,int w,long val){
    tft.fillRect(x,y+8,w,28,COL_BG);
    tft.setTextColor(ac,COL_BG); tft.setTextSize(3);
    char buf[8]; snprintf(buf,sizeof(buf),"%ld",val);
    int tw=strlen(buf)*18;
    tft.setCursor(x+(w-tw)/2,y+10); tft.print(buf);
  };
  rv(  8, 62,92,cdYears);  rv(114, 62,92,cdMonths); rv(220, 62,92,cdDays);
  rv(  8,146,92,cdHours);  rv(114,146,92,cdMins);   rv(220,146,92,cdSecs);
  int yr,mo,dy,hr,mn,sc; getNow(yr,mo,dy,hr,mn,sc);
  tft.fillRect(SW-60,4,56,18,COL_PANEL);
  tft.setTextColor(COL_DIM,COL_PANEL); tft.setTextSize(1);
  char tbuf[12]; snprintf(tbuf,sizeof(tbuf),"%02d:%02d:%02d",hr,mn,sc);
  tft.setCursor(SW-(int)strlen(tbuf)*6-4,10); tft.print(tbuf);
}

// ─────────────────────────────────────────────────────────────
//  CONFIRM CLEAR
// ─────────────────────────────────────────────────────────────

void drawConfirmClear() {
  uint16_t ac=SLOT_COLORS[activeSlot];
  tft.fillScreen(COL_BG);
  tft.fillRoundRect(30,50,260,140,12,COL_PANEL);
  tft.drawRoundRect(30,50,260,140,12,COL_RED);
  drawTrashIcon(SW/2-7,70,COL_RED);
  tft.setTextColor(COL_RED,COL_PANEL); tft.setTextSize(2);
  tft.setCursor(70,92); tft.print("CLEAR SLOT?");
  tft.setTextColor(COL_TEXT,COL_PANEL); tft.setTextSize(1);
  char buf[32]; snprintf(buf,sizeof(buf),"Reset \"%s\"",slots[activeSlot].name);
  tft.setCursor((SW-(int)strlen(buf)*6)/2,116); tft.print(buf);
  tft.setCursor(54,130); tft.print("Name and date will be cleared.");
  tft.fillRoundRect(44,158,96,24,6,COL_DIM);
  tft.setTextColor(COL_TEXT,COL_DIM); tft.setTextSize(2);
  tft.setCursor(68,163); tft.print("NO");
  tft.fillRoundRect(180,158,96,24,6,COL_RED);
  tft.setTextColor(COL_TEXT,COL_RED); tft.setTextSize(2);
  tft.setCursor(200,163); tft.print("YES");
}

// ─────────────────────────────────────────────────────────────
//  SET DATE / SPINNERS
// ─────────────────────────────────────────────────────────────

void initSpinners() {
  int cy=120; CountdownSlot &s=slots[activeSlot];
  spinners[0]={s.month, 1,   12,    35,cy,58,"MONTH"};
  spinners[1]={s.day,   1,   31,   100,cy,46,"DAY"  };
  spinners[2]={s.year,  2025,2050, 170,cy,70,"YEAR" };
  spinners[3]={s.hour,  0,   23,   245,cy,46,"HOUR" };
  spinners[4]={s.minute,0,   59,   295,cy,46,"MIN"  };
}

void drawSpinner(int idx) {
  Spinner &s=spinners[idx];
  int cx=s.x,cy=s.y,w=s.width,x0=cx-w/2,rowH=26;
  tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
  int lw=strlen(s.label)*6; tft.setCursor(cx-lw/2,cy-52); tft.print(s.label);
  for (int off=-2;off<=2;off++) {
    int val=s.value+off;
    if (val<s.minVal) val=s.maxVal-(s.minVal-val-1);
    if (val>s.maxVal) val=s.minVal+(val-s.maxVal-1);
    int yTop=cy+off*rowH-12;
    if (yTop<30||yTop>SH-20) continue;
    uint16_t col; int ts2;
    if      (off==0)      {col=COL_TEXT;ts2=2;}
    else if (abs(off)==1) {col=0x6B6D; ts2=1;}
    else                  {col=COL_DIM; ts2=1;}
    tft.setTextColor(col,off==0?COL_SELECTED:COL_BG); tft.setTextSize(ts2);
    char buf[8];
    if (idx==0) strncpy(buf,MON[constrain(val,1,12)],7);
    else        snprintf(buf,sizeof(buf),"%02d",val);
    int tw2=strlen(buf)*(ts2==2?12:6);
    if (off!=0) tft.fillRect(x0,yTop,w,ts2==2?16:10,COL_BG);
    tft.setCursor(cx-tw2/2,yTop+(off==0?2:1)); tft.print(buf);
  }
  if (idx<NUM_SPINNERS-1) tft.drawFastVLine(cx+w/2+2,35,175,COL_BORDER);
}

void redrawSpinnerCol(int idx) {
  Spinner &s=spinners[idx]; int x0=s.x-s.width/2;
  tft.fillRect(x0,35,s.width,175,COL_BG);
  tft.fillRect(x0,s.y-12,s.width,26,COL_SELECTED);
  drawSpinner(idx);
}

void drawSpinnerScreen() {
  uint16_t ac=SLOT_COLORS[activeSlot];
  tft.fillScreen(COL_BG);
  tft.fillRect(0,0,SW,28,COL_PANEL); tft.drawFastHLine(0,28,SW,ac);
  tft.setTextColor(ac,COL_PANEL); tft.setTextSize(2);
  tft.setCursor(8,7); tft.print("SET DATE");
  tft.setTextColor(COL_DIM,COL_PANEL); tft.setTextSize(1);
  char nbuf[24]; snprintf(nbuf,sizeof(nbuf),"[%s]",slots[activeSlot].name);
  tft.setCursor(SW-strlen(nbuf)*6-66,10); tft.print(nbuf);
  tft.fillRoundRect(SW-62,4,56,20,4,ac);
  tft.setTextColor(COL_BG,ac); tft.setTextSize(1);
  tft.setCursor(SW-52,9); tft.print("< BACK");
  int bandY=spinners[0].y-12;
  tft.fillRect(0,bandY,SW,26,COL_SELECTED);
  tft.drawFastHLine(0,bandY,    SW,ac);
  tft.drawFastHLine(0,bandY+25,SW,ac);
  for (int i=0;i<NUM_SPINNERS;i++) drawSpinner(i);
  tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
  tft.setCursor(8,210); tft.print("Drag UP/DOWN on each column to change value");
  tft.fillRoundRect(100,198,120,26,6,COL_GREEN);
  tft.setTextColor(COL_BG,COL_GREEN); tft.setTextSize(2);
  tft.setCursor(116,204); tft.print("CONFIRM");
}

// ─────────────────────────────────────────────────────────────
//  KEYBOARD  (name editor)
// ─────────────────────────────────────────────────────────────

void drawKeyboard() {
  uint16_t ac=SLOT_COLORS[activeSlot];
  tft.fillScreen(COL_BG);
  tft.fillRect(0,0,SW,28,COL_PANEL); tft.drawFastHLine(0,28,SW,ac);
  tft.setTextColor(ac,COL_PANEL); tft.setTextSize(2);
  tft.setCursor(8,7); tft.print("EDIT NAME");
  tft.fillRect(0,30,SW,40,COL_KEYBG); tft.drawFastHLine(0,70,SW,ac);
  tft.setTextColor(COL_TEXT,COL_KEYBG); tft.setTextSize(2);
  char disp[22]; snprintf(disp,sizeof(disp),"%s_",kbBuffer);
  int tw=strlen(disp)*12;
  tft.setCursor(max(0,(SW-tw)/2),40); tft.print(disp);
  tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
  tft.setCursor(8,75); tft.print("Max 19 chars");
  int mode=(kbMode==KB_LOWER)?0:(kbMode==KB_UPPER)?1:2;
  for (int row=0;row<3;row++) {
    const char* r=KB_ROWS[row][mode]; int len=strlen(r),rowW=len*KB_KW;
    int xs=(SW-rowW)/2;
    for (int k=0;k<len;k++) {
      int kx=xs+k*KB_KW,ky=KB_Y0+row*KB_RH;
      tft.fillRoundRect(kx+1,ky+1,KB_KW-2,KB_KH-2,4,COL_KEYFACE);
      tft.drawRoundRect(kx+1,ky+1,KB_KW-2,KB_KH-2,4,COL_BORDER);
      tft.setTextColor(COL_TEXT,COL_KEYFACE); tft.setTextSize(2);
      tft.setCursor(kx+7,ky+7); tft.print(r[k]);
    }
  }
  int sy=KB_Y0+3*KB_RH;
  bool numOn=(kbMode==KB_NUM),capOn=(kbMode==KB_UPPER);
  tft.fillRoundRect(2,  sy+1,54,KB_KH-2,4,numOn?ac:COL_KEYFACE);
  tft.drawRoundRect(2,  sy+1,54,KB_KH-2,4,COL_BORDER);
  tft.setTextColor(numOn?COL_BG:COL_TEXT,numOn?ac:COL_KEYFACE);
  tft.setTextSize(1); tft.setCursor(8,sy+10); tft.print(numOn?"ABC":"123");
  tft.fillRoundRect(60, sy+1,44,KB_KH-2,4,capOn?ac:COL_KEYFACE);
  tft.drawRoundRect(60, sy+1,44,KB_KH-2,4,COL_BORDER);
  tft.setTextColor(capOn?COL_BG:COL_TEXT,capOn?ac:COL_KEYFACE);
  tft.setTextSize(1); tft.setCursor(66,sy+10); tft.print("CAP");
  tft.fillRoundRect(108,sy+1,80,KB_KH-2,4,COL_KEYFACE);
  tft.drawRoundRect(108,sy+1,80,KB_KH-2,4,COL_BORDER);
  tft.setTextColor(COL_TEXT,COL_KEYFACE); tft.setTextSize(1); tft.setCursor(133,sy+10); tft.print("SPC");
  tft.fillRoundRect(192,sy+1,54,KB_KH-2,4,COL_KEYFACE);
  tft.drawRoundRect(192,sy+1,54,KB_KH-2,4,COL_BORDER);
  tft.setTextColor(COL_RED,COL_KEYFACE); tft.setTextSize(1); tft.setCursor(198,sy+10); tft.print("<DEL");
  tft.fillRoundRect(250,sy+1,68,KB_KH-2,4,COL_GREEN);
  tft.setTextColor(COL_BG,COL_GREEN); tft.setTextSize(1); tft.setCursor(262,sy+10); tft.print("DONE");
}

void handleKbTouch(int tx,int ty) {
  int sy=KB_Y0+3*KB_RH;
  if (ty>=sy&&ty<=sy+KB_KH) {
    if (tx>=2  &&tx<=56)  { kbMode=(kbMode==KB_NUM)?KB_LOWER:KB_NUM;    drawKeyboard(); return; }
    if (tx>=60 &&tx<=104) { kbMode=(kbMode==KB_UPPER)?KB_LOWER:KB_UPPER; drawKeyboard(); return; }
    if (tx>=108&&tx<=188) { if(kbLen<19){kbBuffer[kbLen++]=' ';kbBuffer[kbLen]='\0';} drawKeyboard(); return; }
    if (tx>=192&&tx<=246) { if(kbLen>0){kbBuffer[--kbLen]='\0';} drawKeyboard(); return; }
    if (tx>=250&&tx<=318) {
      if (kbLen==0) strncpy(kbBuffer,DEFAULT_NAMES[kbTargetSlot],19);
      strncpy(slots[kbTargetSlot].name,kbBuffer,19); slots[kbTargetSlot].name[19]='\0';
      saveSlot(kbTargetSlot);
      currentScreen=SCREEN_SET_DATE; initSpinners(); drawSpinnerScreen(); return;
    }
  }
  int mode=(kbMode==KB_LOWER)?0:(kbMode==KB_UPPER)?1:2;
  for (int row=0;row<3;row++) {
    int ky=KB_Y0+row*KB_RH;
    if (ty<ky||ty>ky+KB_KH) continue;
    const char* r=KB_ROWS[row][mode]; int len=strlen(r),rowW=len*KB_KW;
    int xs=(SW-rowW)/2;
    for (int k=0;k<len;k++) {
      int kx=xs+k*KB_KW;
      if (tx>=kx&&tx<=kx+KB_KW) {
        if (kbLen<19){kbBuffer[kbLen++]=r[k];kbBuffer[kbLen]='\0';}
        if (kbMode==KB_UPPER) kbMode=KB_LOWER;
        drawKeyboard(); return;
      }
    }
  }
}

void openKeyboard(int slotIdx) {
  kbTargetSlot=slotIdx; kbMode=KB_LOWER;
  strncpy(kbBuffer,slots[slotIdx].name,19); kbBuffer[19]='\0'; kbLen=strlen(kbBuffer);
  currentScreen=SCREEN_KEYBOARD; drawKeyboard();
}

// ─────────────────────────────────────────────────────────────
//  NAVIGATION HELPER
// ─────────────────────────────────────────────────────────────

void goToPage(int page) {
  pageIndex=page;
  if (page==0) {
    currentScreen=SCREEN_HOME;
    drawHomePage();
  } else {
    activeSlot=page-1;
    currentScreen=SCREEN_COUNTDOWN;
    computeCountdown();
    drawCountdownScreen();
  }
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_BL,HIGH);
  tft.init(); tft.setRotation(3); tft.fillScreen(COL_BG);
  touchSPI.begin(25,39,32,TOUCH_CS);
  ts.begin(touchSPI); ts.setRotation(3);

  // Boot splash
  tft.setTextColor(COL_ACCENT); tft.setTextSize(3);
  tft.setCursor(60,85); tft.print("COUNTDOWN");
  tft.setTextColor(COL_ACCENT2); tft.setTextSize(1);
  tft.setCursor(108,122); tft.print("CALENDAR");
  delay(900);

  bool hasCreds = loadWifiCredentials();

  if (!hasCreds) {
    // First boot — show WiFi setup screen
    wifiSetupSSID[0]='\0'; wifiSetupPass[0]='\0';
    kbMode=KB_LOWER; wifiSetupField=FIELD_SSID;
    currentScreen=SCREEN_WIFI_SETUP;
    drawWifiSetupScreen();
    // Loop is handled in main loop below — setup() returns here
    return;
  }

  // Credentials exist — connect directly
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
  tft.setCursor(82,150); tft.print("Connecting to WiFi...");

  if (!connectAndSync()) {
    // Connection failed — clear creds and show setup
    clearWifiCredentials();
    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_RED,COL_BG); tft.setTextSize(2);
    tft.setCursor(20,90); tft.print("Connection failed!");
    tft.setTextColor(COL_DIM,COL_BG); tft.setTextSize(1);
    tft.setCursor(20,118); tft.print("Saved network unreachable.");
    tft.setCursor(20,132); tft.print("Please re-enter WiFi details.");
    delay(2500);
    wifiSetupSSID[0]='\0'; wifiSetupPass[0]='\0';
    kbMode=KB_LOWER; wifiSetupField=FIELD_SSID;
    currentScreen=SCREEN_WIFI_SETUP;
    drawWifiSetupScreen();
    return;
  }

  loadAllSlots();
  buildHomeEntries();
  pageIndex=0; currentScreen=SCREEN_HOME; drawHomePage();
}

// ─────────────────────────────────────────────────────────────
//  MAIN LOOP
// ─────────────────────────────────────────────────────────────

void loop() {
  int tx,ty;
  bool touched=getTouchPoint(tx,ty);

  // ── HOMEPAGE ───────────────────────────────────────────────
  if (currentScreen==SCREEN_HOME) {
    if (millis()-lastSecond>=1000) {
      lastSecond=millis();
      updateHomeClock();
    }

    // Track touch: record down-position and last-seen position
    static bool  homeWas     = false;
    static int   homeDownX   = 0, homeDownY   = 0;   // where finger landed
    static int   homeLastX   = 0, homeLastY   = 0;   // most recent position
    static unsigned long homeDownTime = 0;

    if (touched) {
      if (!homeWas) {
        // Finger just went down
        homeDownX=tx; homeDownY=ty;
        homeDownTime=millis();
      }
      // Always update last-seen while finger is down
      homeLastX=tx; homeLastY=ty;
    } else if (homeWas) {
      // Finger just lifted — evaluate gesture using saved coords
      unsigned long held = millis()-homeDownTime;
      int totalDX = homeLastX - homeDownX;
      int totalDY = homeLastY - homeDownY;

      bool isTap   = (abs(totalDX)<20 && abs(totalDY)<20 && held<400);
      bool isSwipe = (abs(totalDX)>50 && abs(totalDY)<60  && held<600);

      if (isTap) {
        int x=homeDownX, y=homeDownY;
        // UP button
        if (x>=4 && x<=32 && y>=38 && y<=74) {
          if (homeSelectedEntry>0) { homeSelectedEntry--; drawHomePage(); }
        }
        // DOWN button
        else if (x>=4 && x<=32 && y>=140 && y<=176) {
          if (homeSelectedEntry<numHomeEntries-1) { homeSelectedEntry++; drawHomePage(); }
        }
        // Card tap → open full countdown
        else if (x>=38 && y>=34 && y<=210) {
          activeHolidayEntry=homeSelectedEntry;
          currentScreen=SCREEN_HOLIDAY_CD;
          drawHolidayCdScreen();
        }
      } else if (isSwipe && totalDX<0) {
        goToPage(1); // swipe left → slot 1
      }
    }
    homeWas=touched;
  }

  // ── COUNTDOWN ──────────────────────────────────────────────
  else if (currentScreen==SCREEN_COUNTDOWN) {
    if (millis()-lastSecond>=1000) {
      lastSecond=millis(); computeCountdown(); updateCountdownValues();
    }

    static bool  cdWas      = false;
    static int   cdDownX    = 0, cdDownY    = 0;
    static int   cdLastX    = 0, cdLastY    = 0;
    static unsigned long cdDownTime = 0;

    if (touched) {
      if (!cdWas) {
        cdDownX=tx; cdDownY=ty; cdDownTime=millis();
      }
      cdLastX=tx; cdLastY=ty;

      // Spinner drag — handle continuously while touched
      if (!dragging) {
        for (int i=0;i<NUM_SPINNERS;i++) {
          // only start drag if in set date screen — skip here
        }
      }
    } else if (cdWas) {
      unsigned long held = millis()-cdDownTime;
      int totalDX = cdLastX - cdDownX;
      int totalDY = cdLastY - cdDownY;
      bool isTap   = (abs(totalDX)<20 && abs(totalDY)<20 && held<400);
      bool isSwipe = (abs(totalDX)>50 && abs(totalDY)<60  && held<600);

      if (isSwipe) {
        if (totalDX>0 && pageIndex>0)        goToPage(pageIndex-1);
        if (totalDX<0 && pageIndex<NUM_SLOTS) goToPage(pageIndex+1);
      } else if (isTap) {
        int x=cdDownX, y=cdDownY;
        if (x>SW-75 && y>33 && y<60) {
          delay(150); initSpinners(); currentScreen=SCREEN_SET_DATE; drawSpinnerScreen();
        } else if (x>=SW-98 && x<=SW-74 && y>=33 && y<=57) {
          currentScreen=SCREEN_CONFIRM_CLEAR; drawConfirmClear();
        } else if (y<28 && x<SW-10) {
          openKeyboard(activeSlot);
        } else if (x>=SW-48 && x<=SW-2 && y>=SH-24 && y<=SH-4) {
          currentScreen=SCREEN_WIFI_CONFIRM; drawWifiConfirmScreen();
        }
      }
    }
    cdWas=touched;
  }

  // ── HOLIDAY COUNTDOWN ──────────────────────────────────────
  else if (currentScreen==SCREEN_HOLIDAY_CD) {
    if (millis()-lastSecond>=1000) {
      lastSecond=millis();
      updateHolidayCdValues();
    }
    // BACK button — single tap, no swipe needed
    static bool hcWas=false;
    if (touched&&!hcWas) {
      if (tx>SW-65&&ty>33&&ty<60) {
        currentScreen=SCREEN_HOME;
        drawHomePage();
      }
    }
    hcWas=touched;
  }

  // ── CONFIRM CLEAR ──────────────────────────────────────────
  else if (currentScreen==SCREEN_CONFIRM_CLEAR) {
    static bool ccWas=false;
    if (touched&&!ccWas) {
      if (tx>=44&&tx<=140&&ty>=158&&ty<=182) {
        currentScreen=SCREEN_COUNTDOWN; computeCountdown(); drawCountdownScreen();
      } else if (tx>=180&&tx<=276&&ty>=158&&ty<=182) {
        resetSlotData(activeSlot);
        currentScreen=SCREEN_COUNTDOWN; computeCountdown(); drawCountdownScreen();
      }
    }
    ccWas=touched;
  }

  // ── SET DATE ───────────────────────────────────────────────
  else if (currentScreen==SCREEN_SET_DATE) {
    if (touched) {
      if (tx>SW-65&&ty<28) {
        delay(150); currentScreen=SCREEN_COUNTDOWN; computeCountdown(); drawCountdownScreen(); return;
      }
      if (tx>98&&tx<222&&ty>196&&ty<226) {
        CountdownSlot &s=slots[activeSlot];
        s.month =spinners[0].value; s.year=spinners[2].value;
        s.day   =constrain(spinners[1].value,1,daysInMonth(s.month,s.year));
        s.hour  =spinners[3].value; s.minute=spinners[4].value;
        saveSlot(activeSlot);
        delay(150); currentScreen=SCREEN_COUNTDOWN; computeCountdown(); drawCountdownScreen(); return;
      }
      if (ty>=4&&ty<=22&&tx>60&&tx<SW-66) { openKeyboard(activeSlot); return; }
      if (!dragging) {
        for (int i=0;i<NUM_SPINNERS;i++) {
          int x0=spinners[i].x-spinners[i].width/2, x1=spinners[i].x+spinners[i].width/2;
          if (tx>=x0&&tx<=x1&&ty>30&&ty<210) {
            activeSpinner=i; dragStartY=ty; dragStartVal=spinners[i].value; dragging=true; break;
          }
        }
      }
      if (dragging&&activeSpinner>=0) {
        int delta=(dragStartY-ty)/18;
        Spinner &s=spinners[activeSpinner];
        int range=s.maxVal-s.minVal+1;
        int nv=((dragStartVal-s.minVal+delta)%range+range)%range+s.minVal;
        if (nv!=s.value) {
          s.value=nv;
          if (activeSpinner==0||activeSpinner==2) {
            int maxD=daysInMonth(spinners[0].value,spinners[2].value);
            spinners[1].maxVal=maxD;
            if (spinners[1].value>maxD) spinners[1].value=maxD;
            redrawSpinnerCol(1);
          }
          redrawSpinnerCol(activeSpinner);
        }
      }
    } else {
      if (dragging){dragging=false;activeSpinner=-1;}
    }
  }

  // ── KEYBOARD ───────────────────────────────────────────────
  else if (currentScreen==SCREEN_KEYBOARD) {
    static bool kbWas=false;
    if (touched&&!kbWas) handleKbTouch(tx,ty);
    kbWas=touched;
  }

  // ── WIFI SETUP ─────────────────────────────────────────────
  else if (currentScreen==SCREEN_WIFI_SETUP) {
    static bool wsWas=false;
    if (touched&&!wsWas) handleWifiSetupTouch(tx,ty);
    wsWas=touched;
  }

  // ── WIFI CONFIRM (change network) ──────────────────────────
  else if (currentScreen==SCREEN_WIFI_CONFIRM) {
    static bool wcWas=false;
    if (touched&&!wcWas) {
      // NO
      if (tx>=44&&tx<=140&&ty>=162&&ty<=186) {
        currentScreen=SCREEN_COUNTDOWN; drawCountdownScreen();
      }
      // YES — clear credentials and show setup screen
      else if (tx>=180&&tx<=276&&ty>=162&&ty<=186) {
        clearWifiCredentials();
        wifiSetupSSID[0]='\0'; wifiSetupPass[0]='\0';
        kbMode=KB_LOWER; wifiSetupField=FIELD_SSID;
        currentScreen=SCREEN_WIFI_SETUP;
        drawWifiSetupScreen();
      }
    }
    wcWas=touched;
  }

  delay(20);
}

