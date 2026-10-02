// WTA Route 190 bus tracker: ESP32 + 128x64 SSD1306 OLED.
// Reads WTA's GTFS-Realtime feed DIRECTLY (no PC/server needed) and falls back to the
// published schedule (SCH) when the live feed has no prediction for your stop.
// Libraries: Adafruit SSD1306, Adafruit GFX.
// Setup: copy secrets.h.example to secrets.h and enter your Wi-Fi details.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASS

// ---------- EDIT THESE ----------
// Pick ONE board:
#define BOARD_S2_MINI        // LOLIN S2 Mini (breadboard build)
// #define BOARD_WROOM32     // ESP32-WROOM-32 (custom PCB)

const char* ROUTE_ID  = "190";   // route_id in WTA's data
// Highland Dr at Ridgeway: downtown-bound = "664"/"3180", Lincoln St-bound = "665"/"2131"
const char* STOP_ID   = "664";
const char* STOP_CODE = "3180";  // matched too, in case the feed uses the code
const char* TIMEZONE  = "PST8PDT,M3.2.0,M11.1.0";   // Pacific time with daylight saving
// --------------------------------
#ifdef BOARD_S2_MINI
  #define SDA_PIN 15
  #define SCL_PIN 4
#else
  #define SDA_PIN 2
  #define SCL_PIN 4
#endif
const char* HOST = "bustracker.ridewta.com";
const char* PATH = "/gtfsrt/trips";
#define OLED_ADDR 0x3C
const unsigned long REFRESH_MS = 20000;
const unsigned long WIFI_GIVEUP_MS = 60000;   // show NO WIFI only after this long
#define BUF_SZ 6144

// =================== Data types (must stay above all functions) ===================
struct Finder {
  const char *route, *stopA, *stopB;   // route_id, plus stop_id / stop_code to match
  long now;                            // unix seconds
  bool found = false; long bestT = 0; int bestAway = 0;
  int trips = 0, matched = 0;          // debug counters
};

struct PB {
  const uint8_t *p, *e;
  bool more() const { return p < e; }
  bool varint(uint64_t &v) {
    v = 0; int s = 0;
    while (p < e) {
      uint8_t b = *p++; v |= (uint64_t)(b & 0x7f) << s;
      if (!(b & 0x80)) return true;
      s += 7; if (s > 63) return false;
    }
    return false;
  }
  bool next(int &f, int &wt) { uint64_t t; if (!varint(t)) return false; f = (int)(t >> 3); wt = (int)(t & 7); return true; }
  bool sub(PB &out) {
    uint64_t l; if (!varint(l) || (uint64_t)(e - p) < l) return false;
    out.p = p; out.e = p + l; p += l; return true;
  }
  bool skip(int wt) {
    uint64_t v; PB tmp;
    if (wt == 0) return varint(v);
    if (wt == 1) { if (e - p < 8) return false; p += 8; return true; }
    if (wt == 5) { if (e - p < 4) return false; p += 4; return true; }
    if (wt == 2) return sub(tmp);
    return false;
  }
};

struct SchedTbl { const uint16_t* t; int n; };

// =================== Published schedule (minutes after midnight) ===================
// From WTA GTFS, valid 2026-09-20 to 2027-02-06. SU = Sunday, WK = Mon-Fri, SA = Saturday.
const uint16_t S664_SU[] = {529,559,589,619,649,679,709,739,769,799,829,859,889,919,949,979,1009,1039,1069,1099,1129,1159,1189,1219,1249,1286};
const uint16_t S664_WK[] = {409,439,454,469,484,499,514,529,544,559,574,589,604,619,634,649,664,679,694,709,724,739,754,769,784,799,814,829,844,859,874,889,906,921,936,951,966,981,996,1011,1026,1041,1056,1071,1086,1101,1116,1131,1146,1161,1176,1206,1221,1236,1266,1281,1296,1326,1356};
const uint16_t S664_SA[] = {499,529,544,559,589,604,619,649,664,679,709,724,739,769,784,799,844,859,874,904,919,934,964,979,994,1024,1039,1054,1084,1099,1114,1144,1159,1174,1204,1219,1234,1264,1279,1294,1324,1354};
const uint16_t S665_SU[] = {527,557,587,617,647,677,707,737,767,797,827,857,887,917,947,977,1007,1037,1067,1097,1127,1157,1187,1217};
const uint16_t S665_WK[] = {407,422,437,452,467,482,497,512,527,542,557,572,587,602,617,632,647,662,677,692,707,722,737,752,767,782,797,812,827,842,857,874,889,904,919,934,949,964,979,994,1009,1024,1039,1054,1069,1084,1099,1114,1129,1144,1174,1189,1204,1234,1249,1264,1294,1309,1324,1354};
const uint16_t S665_SA[] = {467,497,512,527,557,572,587,617,632,647,677,692,707,737,752,767,812,827,842,872,887,902,932,947,962,992,1007,1022,1052,1067,1082,1112,1127,1142,1172,1187,1202,1232,1247,1262,1292,1322,1352};

#define TBL(a) { a, (int)(sizeof(a) / sizeof(a[0])) }
const SchedTbl T664[3] = { TBL(S664_SU), TBL(S664_WK), TBL(S664_SA) };   // index: 0 Sun, 1 Mon-Fri, 2 Sat
const SchedTbl T665[3] = { TBL(S665_SU), TBL(S665_WK), TBL(S665_SA) };

int kindOf(int wday) { return wday == 0 ? 0 : (wday == 6 ? 2 : 1); }

// Next scheduled bus at our stop. outSecs = seconds from now. Returns false if no schedule for this stop.
bool nextScheduled(long now, long &outSecs, int &outH, int &outM) {
  const SchedTbl* T = nullptr;
  if (!strcmp(STOP_ID, "664") || !strcmp(STOP_CODE, "3180")) T = T664;
  else if (!strcmp(STOP_ID, "665") || !strcmp(STOP_CODE, "2131")) T = T665;
  if (!T) return false;
  time_t n = (time_t)now; struct tm lt; localtime_r(&n, &lt);
  long nowSec = lt.tm_hour * 3600L + lt.tm_min * 60L + lt.tm_sec;
  for (int d = 0; d < 2; d++) {                       // today, then tomorrow
    const SchedTbl& s = T[kindOf((lt.tm_wday + d) % 7)];
    for (int i = 0; i < s.n; i++) {
      long ts = (long)s.t[i] * 60 + d * 86400L;
      if (ts >= nowSec - 30) { outSecs = ts - nowSec; outH = s.t[i] / 60; outM = s.t[i] % 60; return true; }
    }
  }
  return false;
}

// Builds "ETA:7 MINS" / "SCH:12 MIN" / "ETA:1H40M" (always fits 10 characters)
String fmtMins(const char* p, int m) {
  if (m == 0) return String(p) + ": DUE";
  if (m == 1) return String(p) + ":1 MIN";
  if (m < 10) return String(p) + ":" + String(m) + " MINS";
  if (m < 100) return String(p) + ":" + String(m) + " MIN";
  char b[16]; snprintf(b, sizeof(b), "%s:%dH%02dM", p, m / 60, m % 60);
  return String(b);
}

// =================== GTFS-Realtime (protobuf) reader ===================
static bool pbEq(const PB &s, const char *str) {
  size_t n = strlen(str);
  return (size_t)(s.e - s.p) == n && memcmp(s.p, str, n) == 0;
}

// TripUpdate { trip=1 {route_id=5}, stop_time_update=2 {arrival=2/departure=3 {delay=1,time=2}, stop_id=4} }
static void handleTripUpdate(Finder &F, PB tu) {
  int f, wt;
  // pass 1: is this our route?
  bool ours = false; PB a = tu;
  while (a.more() && a.next(f, wt)) {
    if (f == 1 && wt == 2) {
      PB trip; if (!a.sub(trip)) return;
      int g, w2;
      while (trip.more() && trip.next(g, w2)) {
        if (g == 5 && w2 == 2) { PB s; if (!trip.sub(s)) return; ours = pbEq(s, F.route); }
        else if (!trip.skip(w2)) return;
      }
    } else if (!a.skip(wt)) return;
  }
  F.trips++;
  if (!ours) return;
  F.matched++;

  // pass 2: walk the stops in order, counting those still ahead of the bus
  int away = 0, nStops = 0, nTime = 0; bool seen = false; long seenT = 0, seenDly = 0;
  char first[16] = "", last[16] = "";
  PB b = tu;
  while (b.more() && b.next(f, wt)) {
    if (f == 2 && wt == 2) {
      PB st; if (!b.sub(st)) break;
      long t = 0, dly = 0; bool hit = false; char sid[16] = ""; int g, w2;
      while (st.more() && st.next(g, w2)) {
        if ((g == 2 || g == 3) && w2 == 2) {
          PB ev; if (!st.sub(ev)) break;
          int h, w3;
          while (ev.more() && ev.next(h, w3)) {
            uint64_t v;
            if (h == 2 && w3 == 0) { if (!ev.varint(v)) break; if (g == 2 || t == 0) t = (long)v; }
            else if (h == 1 && w3 == 0) { if (!ev.varint(v)) break; dly = (int32_t)v; }
            else if (!ev.skip(w3)) break;
          }
        } else if (g == 4 && w2 == 2) {
          PB s; if (!st.sub(s)) break;
          size_t n = s.e - s.p; if (n > 15) n = 15;
          memcpy(sid, s.p, n); sid[n] = 0;
          hit = pbEq(s, F.stopA) || pbEq(s, F.stopB);
        } else if (!st.skip(w2)) break;
      }
      nStops++; if (t) nTime++;
      if (nStops == 1) strcpy(first, sid);
      strcpy(last, sid);
      if (hit) {
        seen = true; seenT = t; seenDly = dly;
        if (t >= F.now - 30 && (!F.found || t < F.bestT)) { F.found = true; F.bestT = t; F.bestAway = away; }
        break;
      }
      if (t >= F.now) away++;
    } else if (!b.skip(wt)) break;
  }
  Serial.printf("trip: stops=%d withTime=%d first=%s last=%s | our stop %s, secondsAway=%ld delay=%ld\n",
                nStops, nTime, first, last, seen ? "FOUND" : "not in list", seen ? seenT - F.now : 0L, seenDly);
}

// FeedEntity { trip_update=3 }
static void handleEntity(Finder &F, PB ent) {
  int f, wt;
  while (ent.more() && ent.next(f, wt)) {
    if (f == 3 && wt == 2) { PB tu; if (!ent.sub(tu)) return; handleTripUpdate(F, tu); }
    else if (!ent.skip(wt)) return;
  }
}

// =================== Display + networking ===================
Adafruit_SSD1306 display(128, 64, &Wire, -1);
static uint8_t buf[BUF_SZ];
unsigned long lastFetch = 0;
unsigned long wifiDownSince = 0;   // 0 = Wi-Fi is up
String feedErr = "";

void show(const String& l1, const String& l2, const String& l3) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);  display.print(l1);   // yellow band
  display.setCursor(0, 20); display.print(l2);
  display.setCursor(0, 42); display.print(l3);
  display.display();
}

// Reads the HTTP body, undoing chunked encoding if the server uses it.
struct Src {
  WiFiClientSecure* c; bool chunked = false; long left = 0; bool done = false;
  bool line(String& s) {
    s = ""; uint8_t b;
    while (c->readBytes(&b, 1) == 1) { if (b == '\n') { s.trim(); return true; } s += (char)b; }
    return false;
  }
  bool eof() { return chunked ? done : (!c->connected() && !c->available()); }
  size_t read(uint8_t* dst, size_t n) {
    if (!chunked) return c->readBytes(dst, n);
    size_t got = 0;
    while (got < n) {
      if (left == 0) {
        String l; if (!line(l)) break;
        if (l.length() == 0 && !line(l)) break;
        left = strtol(l.c_str(), nullptr, 16);
        if (left == 0) { done = true; break; }
      }
      size_t want = min((size_t)left, n - got);
      size_t r = c->readBytes(dst + got, want);
      got += r; left -= r;
      if (r < want) break;
    }
    return got;
  }
  bool varint(uint64_t& v) {
    v = 0; int s = 0; uint8_t b;
    while (read(&b, 1) == 1) { v |= (uint64_t)(b & 0x7f) << s; if (!(b & 0x80)) return true; s += 7; if (s > 63) return false; }
    return false;
  }
  bool skipN(uint64_t n) {
    while (n > 0) { size_t k = n < BUF_SZ ? (size_t)n : BUF_SZ; if (read(buf, k) != k) return false; n -= k; }
    return true;
  }
};

// Returns true if the feed was read OK; result is in F. On failure, feedErr says why.
bool fetchFeed(Finder& F) {
  feedErr = "";
  WiFiClientSecure client;
  client.setInsecure();          // skips certificate check (fine for public bus data)
  client.setTimeout(15);         // seconds
  if (!client.connect(HOST, 443)) { feedErr = "CONNECT"; Serial.println("connect failed"); return false; }
  client.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (ESP32)\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", PATH, HOST);

  // wait for the server to start answering
  unsigned long t0 = millis();
  while (!client.available() && client.connected() && millis() - t0 < 15000) delay(50);
  if (!client.available()) {
    feedErr = client.connected() ? "TIMEOUT" : "CLOSED";
    Serial.println(feedErr);
    return false;
  }

  Src src; src.c = &client;
  String l;
  if (!src.line(l)) { feedErr = "NO REPLY"; Serial.println("no reply"); return false; }
  Serial.println(l);
  if (l.indexOf(" 200") < 0) { feedErr = "HTTP " + l.substring(9, 12); return false; }
  while (src.line(l) && l.length()) { l.toLowerCase(); if (l.indexOf("chunked") >= 0) src.chunked = true; }

  int entities = 0;
  while (!src.eof()) {
    uint64_t tag; if (!src.varint(tag)) break;
    int f = tag >> 3, wt = tag & 7; uint64_t v;
    if (wt == 2) {
      uint64_t len; if (!src.varint(len)) break;
      if (f == 2 && len <= BUF_SZ) {
        if (src.read(buf, len) != len) break;
        PB e{buf, buf + len}; handleEntity(F, e); entities++;
      } else if (!src.skipN(len)) break;
    } else if (wt == 0) { if (!src.varint(v)) break; }
    else if (wt == 1) { if (!src.skipN(8)) break; }
    else if (wt == 5) { if (!src.skipN(4)) break; }
    else break;
  }
  Serial.printf("entities=%d trips=%d route%s=%d heap=%u\n", entities, F.trips, ROUTE_ID, F.matched, ESP.getFreeHeap());
  if (entities == 0) feedErr = "NO DATA";
  return entities > 0;
}

void update() {
  String bus = "BUS " + String(ROUTE_ID);

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDownSince == 0) wifiDownSince = millis();
    if (millis() - wifiDownSince < WIFI_GIVEUP_MS) {      // keep saying CONNECTING for a full minute
      show(bus, "CONNECTING", "");
      return;
    }
    Serial.printf("WiFi not connected, status=%d\n", (int)WiFi.status());
    show(bus, "NO WIFI", "CODE " + String((int)WiFi.status()));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);                     // keep trying in the background
    return;
  }
  wifiDownSince = 0;

  if (time(nullptr) < 1700000000) {
    configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");   // ask for the time again
    show(bus, "SETTING", "CLOCK");
    return;
  }

  Finder F; F.route = ROUTE_ID; F.stopA = STOP_ID; F.stopB = STOP_CODE; F.now = time(nullptr);
  if (!fetchFeed(F)) { show(bus, "FEED ERR", feedErr); return; }

  long secs = 0; int h = 0, m = 0; bool live = F.found;
  if (live) {
    secs = F.bestT - F.now;
    time_t arr = (time_t)F.bestT; struct tm tmv; localtime_r(&arr, &tmv);
    h = tmv.tm_hour; m = tmv.tm_min;
  } else if (nextScheduled(F.now, secs, h, m)) {
    Serial.println("No live prediction for our stop - using schedule");
  } else {
    show(bus, "NO BUSES", "SOON");
    return;
  }

  int mins = (secs + 30) / 60; if (mins < 0) mins = 0;

  // 12-hour time. Hours 10-12 use just A/P so the line still fits the screen.
  int h12 = h % 12; if (h12 == 0) h12 = 12;
  bool pm = h >= 12;
  const char* suffix = (h12 >= 10) ? (pm ? "P" : "A") : (pm ? "PM" : "AM");
  char tb[16]; snprintf(tb, sizeof(tb), "ARR %d:%02d%s", h12, m, suffix);
  show(bus, String(tb), fmtMins(live ? "ETA" : "SCH", mins));
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("Wi-Fi disconnect reason: %d\n", info.wifi_sta_disconnected.reason);
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long ts = millis();
  while (!Serial && millis() - ts < 2000) delay(10);   // S2 Mini: wait briefly for the USB serial port

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) { Serial.println("OLED not found"); while (true) delay(1000); }
  String bus = "BUS " + String(ROUTE_ID);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // keeps the connection steadier
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWifiEvent);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Keep saying CONNECTING (with moving dots) for up to a minute
  unsigned long t0 = millis(); int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_GIVEUP_MS) {
    String d = ""; for (int i = 0; i < dots % 4; i++) d += ".";
    show(bus, "CONNECTING", d);
    delay(500); dots++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK, IP %s, signal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    show(bus, "CONNECTED", "");
    delay(3000);                                   // show CONNECTED for a few seconds
    show(bus, "SETTING", "CLOCK");
    configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
    t0 = millis();
    while (time(nullptr) < 1700000000 && millis() - t0 < 30000) delay(300);
  } else {
    Serial.printf("WiFi failed, status=%d\n", (int)WiFi.status());
    wifiDownSince = t0;                            // a full minute has passed, so update() shows NO WIFI
  }
  update();
  lastFetch = millis();
}

void loop() {
  if (millis() - lastFetch >= REFRESH_MS) { lastFetch = millis(); update(); }
}
