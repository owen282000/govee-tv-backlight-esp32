// Drives the LED strip of a Govee TV Backlight 3 Pro (H66A0, sold as R2A80)
// from an ESP32, without the Govee controller box. Takes colors over DDP (from
// Hyperion, HyperHDR or anything else that speaks DDP) and has a small HTTP and
// serial interface. Wi-Fi is set up over Improv Serial, straight from the web
// installer, or at build time with include/secrets.h.
//
// Strip protocol, measured with a logic analyzer on the data pin (A10/B10 of
// the strip's USB-C plug). See docs/protocol.md.
//   - single wire, 3.3 V logic, 800 kHz
//   - bit 0 = ~300 ns high + ~950 ns low, bit 1 = ~950 ns high + ~300 ns low
//   - 90 chips, no header, > 80 us low ends the frame
//   - per chip 5 channels in the order green, red, blue, cold white, warm white
//   - per channel 2 bits of current setting, then a 16-bit value, MSB first
// The Govee controller sends a frame every 21 ms; this firmware every 20 ms.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include "driver/rmt.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef STRIP_PIN
#define STRIP_PIN 16
#endif
// Set by the build from the git tag (see platformio.ini); "dev" for local builds.
#ifndef FW_VERSION
#define FW_VERSION ""
#endif
static const char *fwVersion() { return sizeof(FW_VERSION) > 1 ? FW_VERSION : "dev"; }

static const char *FW_NAME = "govee-tv-backlight-esp32";
static const int MAX_CHIPS = 120;
static const int CH = 5;  // wire order: G, R, B, cold white, warm white
static const int BITS_PER_CH = 18;
static const rmt_channel_t RMT_CH = RMT_CHANNEL_0;
// RMT clock 80 MHz / 2 = 40 MHz, 1 tick = 25 ns
static const uint16_t T_SHORT = 12;  // 300 ns
static const uint16_t T_LONG = 38;   // 950 ns
static const uint16_t DDP_PORT = 4048;
static const uint32_t DDP_TIMEOUT_MS = 3000;
static const uint32_t FRAME_MS = 20;
// Setup access point when there is no Wi-Fi: "govee-backlight-xxxx", this password.
static const char *AP_PASS = "backlight";
static const uint32_t AP_AFTER_MS = 120000;  // configured but unreachable this long: start the AP too

enum { G, R, B, CW, WW };

// Settings, kept in flash (NVS) so they survive a restart.
Preferences prefs;
int chips = 90;
// The 2 bits before each channel set the drive current: 3 is the brightest and
// what the Govee controller always sends, 0 is the dimmest (still on).
uint8_t prefix = 0b11;
// Upper limit per channel as a share of 65535. "max 0.25" while testing with
// thin jumper wires.
float maxLevel = 1.0f;
String wifiSsid, wifiPass, otaPass, hostname;

uint16_t staticColor[CH] = {0, 0, 0, 0, 0};
uint16_t frame[MAX_CHIPS][CH];
rmt_item32_t items[MAX_CHIPS * CH * BITS_PER_CH];
uint16_t gammaLut[256];

bool ddpActive = false;
uint32_t lastDdp = 0, lastShow = 0, ddpFrames = 0;
bool testMode = false;
uint32_t testStep = 0, testAt = 0;
// Optional power switch for the LED output (GPIO 18 on the Gledopto
// GL-C-015WL-M): on as soon as anything is lit, off after 5 s fully dark.
bool relayOn = false;
uint32_t lastLit = 0;

WiFiUDP udp;
WebServer server(80);
bool netStarted = false, mdnsStarted = false, apMode = false;
uint32_t wifiStartedAt = 0, lastRetry = 0;
DNSServer dns;
uint8_t packet[1500];

// ---------------------------------------------------------------- strip

static inline void setBit(rmt_item32_t &it, bool one) {
  it.level0 = 1;
  it.duration0 = one ? T_LONG : T_SHORT;
  it.level1 = 0;
  it.duration1 = one ? T_SHORT : T_LONG;
}

void relay(bool on) {
#ifdef RELAY_PIN
  if (on == relayOn) return;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  if (on) delay(30);  // let the supply come up before the first data
#endif
  relayOn = on;
}

void show() {
  uint32_t limit = (uint32_t)(maxLevel * 65535.0f);
  bool lit = false;
  for (int c = 0; c < chips && !lit; c++)
    for (int k = 0; k < CH; k++)
      if (frame[c][k] && limit) lit = true;
  if (lit) {
    lastLit = millis();
    relay(true);
  } else if (relayOn && millis() - lastLit > 5000) {
    relay(false);
  }
  size_t n = 0;
  for (int c = 0; c < chips; c++) {
    for (int k = 0; k < CH; k++) {
      uint32_t v = frame[c][k];
      if (v > limit) v = limit;
      v |= (uint32_t)prefix << 16;
      for (int b = BITS_PER_CH - 1; b >= 0; b--) setBit(items[n++], (v >> b) & 1);
    }
  }
  rmt_write_items(RMT_CH, items, n, true);
  lastShow = millis();
}

void fillStatic() {
  for (int c = 0; c < chips; c++)
    for (int k = 0; k < CH; k++) frame[c][k] = staticColor[k];
}

void setStatic(uint16_t g, uint16_t r, uint16_t b, uint16_t cw, uint16_t ww) {
  staticColor[G] = g;
  staticColor[R] = r;
  staticColor[B] = b;
  staticColor[CW] = cw;
  staticColor[WW] = ww;
  testMode = false;
  if (!ddpActive) fillStatic();
}

void runTest() {
  if (millis() - testAt < 1000) return;
  testAt = millis();
  static const uint16_t F = 65535;
  static const uint16_t steps[6][CH] = {
      {0, F, 0, 0, 0}, {F, 0, 0, 0, 0}, {0, 0, F, 0, 0}, {0, 0, 0, F, 0}, {0, 0, 0, 0, F}, {0, 0, 0, 0, 0}};
  const uint16_t *s = steps[testStep++ % 6];
  for (int c = 0; c < chips; c++)
    for (int k = 0; k < CH; k++) frame[c][k] = s[k];
}

// ---------------------------------------------------------------- commands

String status() {
  char buf[400];
  bool up = WiFi.status() == WL_CONNECTED;
  snprintf(buf, sizeof(buf),
           "{\"firmware\":\"%s\",\"version\":\"%s\",\"hostname\":\"%s\",\"chips\":%d,\"prefix\":%u,\"max\":%.2f,"
           "\"g\":%u,\"r\":%u,\"b\":%u,\"cw\":%u,\"ww\":%u,\"ddp\":%s,\"ddp_frames\":%lu,"
           "\"wifi\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ota\":%s}",
           FW_NAME, fwVersion(), hostname.c_str(), chips, prefix, maxLevel, staticColor[G], staticColor[R],
           staticColor[B], staticColor[CW], staticColor[WW], ddpActive ? "true" : "false", (unsigned long)ddpFrames,
           wifiSsid.c_str(), up ? WiFi.localIP().toString().c_str() : "", up ? WiFi.RSSI() : 0,
           otaPass.length() ? "true" : "false");
  return String(buf);
}

// Commands shared by serial and HTTP. Channel values are 0-65535, rgb takes
// 0-255 and applies gamma 2.2. "fromSerial" allows the commands that need
// physical access (changing an existing OTA password, forgetting Wi-Fi).
String command(String line, bool fromSerial) {
  line.trim();
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? "" : line.substring(sp + 1);
  rest.trim();
  long v[5] = {0, 0, 0, 0, 0};
  int got = sscanf(rest.c_str(), "%ld %ld %ld %ld %ld", &v[0], &v[1], &v[2], &v[3], &v[4]);
  if (cmd == "c" && got >= 1) {
    setStatic(v[0], v[1], v[2], v[3], v[4]);
  } else if (cmd == "rgb" && got == 3) {
    setStatic(gammaLut[v[1] & 255], gammaLut[v[0] & 255], gammaLut[v[2] & 255], 0, 0);
  } else if (cmd == "off") {
    setStatic(0, 0, 0, 0, 0);
  } else if (cmd == "chips" && got == 1 && v[0] > 0 && v[0] <= MAX_CHIPS) {
    chips = v[0];
    prefs.putInt("chips", chips);
    if (!ddpActive) fillStatic();
  } else if (cmd == "prefix" && got == 1) {
    prefix = v[0] & 3;
    prefs.putUChar("prefix", prefix);
  } else if (cmd == "max" && rest.length()) {
    maxLevel = constrain(rest.toFloat(), 0.0f, 1.0f);
    prefs.putFloat("max", maxLevel);
  } else if (cmd == "test") {
    testMode = true;
    testStep = 0;
    testAt = 0;
  } else if (cmd == "otapass" && rest.length() >= 8) {
    // Over HTTP only the first time (trust on first use); changing it needs serial.
    if (otaPass.length() && !fromSerial) return "{\"error\":\"OTA password already set, change it over serial\"}";
    otaPass = rest;
    prefs.putString("otapass", otaPass);
  } else if (cmd == "forget" && fromSerial) {
    prefs.remove("ssid");
    prefs.remove("pass");
    WiFi.disconnect(true, true);  // also clear what the Wi-Fi driver may have kept
    Serial.println("wifi: forgotten, restarting");
    delay(200);
    ESP.restart();
  } else if (cmd == "restart") {
    delay(200);
    ESP.restart();
  } else if (cmd != "status" && cmd != "") {
    return "{\"error\":\"commands: c g r b cw ww | rgb r g b | off | test | chips n | prefix 0-3 | max 0.0-1.0 | "
           "otapass <8+ chars> | restart | status\"}";
  }
  return status();
}

// ---------------------------------------------------------------- network

void startNetwork();

// A station that is still trying to connect makes every scan come back empty,
// so stop that first. In setup mode networkLoop() retries the saved network.
int scanNow() {
  for (uint32_t t0 = millis(); WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - t0 < 10000;) delay(50);
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect(false, false);
  // 150 ms per channel keeps a scan around 3 s, also while the setup access point runs.
  return WiFi.scanNetworks(false, false, false, 150);
}

String htmlEscape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

String setupPage() {
  String opts;
  int n = WiFi.scanComplete();
  // The list comes from the scan made before the access point started (or the
  // latest Improv scan). Typing a network name works too.
  for (int i = 0; i < n; i++) {
    if (!WiFi.SSID(i).length()) continue;
    opts += "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";
  }
  return "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>Govee backlight setup</title><style>body{font:16px/1.5 sans-serif;max-width:420px;margin:32px auto;"
         "padding:0 16px}input,button{font:inherit;width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box}"
         "button{background:#2f7d5b;color:#fff;border:0;border-radius:8px}</style>"
         "<h2>Govee backlight Wi-Fi</h2><form method=post action=/wifi>"
         "<label>Network<input name=ssid list=nets required autocomplete=off></label><datalist id=nets>" + opts +
         "</datalist><label>Password<input name=pass type=password></label><button>Save and restart</button></form>"
         "<p style='color:#666;font-size:14px'>" + String(FW_NAME) + " " + fwVersion() + "</p>";
}

// Setup mode in two steps. First a scan as a plain station, without blocking,
// so Improv keeps answering (the web installer gives up after a second of
// silence). A scan started while the access point comes up fails, and leaves
// later scans failing too, so the access point only starts once it is done.
bool setupPending = false;

void startAp() {
  if (apMode || setupPending) return;
  setupPending = true;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  WiFi.scanNetworks(true, false, false, 150);
}

void finishAp() {
  setupPending = false;
  apMode = true;
  WiFi.mode(WIFI_AP_STA);  // station side stays on: later scans and retrying a saved network
  WiFi.softAP(hostname.c_str(), AP_PASS);
  dns.start(53, "*", WiFi.softAPIP());
  startNetwork();
  Serial.printf("setup: Wi-Fi \"%s\" (password %s), open http://192.168.4.1\n", hostname.c_str(), AP_PASS);
}

void startNetwork() {
  if (netStarted) return;
  netStarted = true;
  udp.begin(DDP_PORT);
  server.on("/", []() {
    if (apMode && WiFi.status() != WL_CONNECTED) return server.send(200, "text/html", setupPage());
    server.send(200, "application/json", status());
  });
  // Wi-Fi setup form, only while the setup access point is up.
  server.on("/wifi", HTTP_POST, []() {
    if (!apMode) return server.send(403, "text/plain", "only in setup mode\n");
    String ssid = server.arg("ssid"), pass = server.arg("pass");
    if (!ssid.length()) return server.send(400, "text/plain", "no network name\n");
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    server.send(200, "text/html", "<meta name=viewport content='width=device-width'><p style='font:16px sans-serif'>Saved. The board restarts and joins <b>" + htmlEscape(ssid) + "</b>. Find it at <b>http://" + hostname + ".local</b> or in your router.</p>");
    delay(500);
    ESP.restart();
  });
  server.onNotFound([]() {
    if (apMode) {  // captive portal: send every unknown URL to the setup page
      server.sendHeader("Location", "http://192.168.4.1/", true);
      return server.send(302, "text/plain", "");
    }
    server.send(404, "text/plain", "not found\n");
  });
  server.on("/cmd", []() { server.send(200, "application/json", command(server.arg("q"), false)); });
  // Over-the-air update: curl -u ota:<password> -F "update=@firmware.bin" http://<ip>/update
  // The ESP32 never connects back (unlike ArduinoOTA), so this also works
  // across a firewalled IoT network. Off until an OTA password is set.
  server.on(
      "/update", HTTP_POST,
      []() {
        if (!otaPass.length()) return server.send(403, "text/plain", "set an OTA password first\n");
        if (!server.authenticate("ota", otaPass.c_str())) return server.requestAuthentication();
        bool ok = !Update.hasError();
        server.send(ok ? 200 : 500, "text/plain", ok ? "ok, restarting\n" : "update failed\n");
        delay(300);
        if (ok) ESP.restart();
      },
      []() {
        if (!otaPass.length() || !server.authenticate("ota", otaPass.c_str())) return;
        HTTPUpload &up = server.upload();
        if (up.status == UPLOAD_FILE_START) {
          relay(false);
          Update.begin(UPDATE_SIZE_UNKNOWN);
        } else if (up.status == UPLOAD_FILE_WRITE) {
          Update.write(up.buf, up.currentSize);
        } else if (up.status == UPLOAD_FILE_END) {
          Update.end(true);
        }
      });
  server.begin();
}

void stopAp() {
  if (!apMode) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  apMode = false;
}

void connectWifi() {
  if (!wifiSsid.length()) return;
  setupPending = false;
  stopAp();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname.c_str());
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  wifiStartedAt = millis();
  startNetwork();
}

void networkLoop() {
  if (apMode && wifiSsid.length() && WiFi.status() == WL_CONNECTED) {
    stopAp();  // the saved network came back
    WiFi.mode(WIFI_STA);
  }
  if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
    mdnsStarted = MDNS.begin(hostname.c_str());
    if (mdnsStarted) MDNS.addService("http", "tcp", 80);
    Serial.printf("wifi: %s, http://%s.local\n", WiFi.localIP().toString().c_str(), hostname.c_str());
  }
  if (wifiSsid.length() && !apMode && WiFi.status() != WL_CONNECTED && millis() - wifiStartedAt > AP_AFTER_MS)
    startAp();  // keeps trying the saved network meanwhile
  if (setupPending && WiFi.scanComplete() != WIFI_SCAN_RUNNING) finishAp();
  if (apMode) {
    dns.processNextRequest();
    // Setup mode with a saved network: keep trying it (a scan stops the attempt).
    if (wifiSsid.length() && WiFi.status() != WL_CONNECTED && WiFi.scanComplete() != WIFI_SCAN_RUNNING &&
        millis() - lastRetry > 30000) {
      lastRetry = millis();
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
  }
  if (netStarted) server.handleClient();
}

// ---------------------------------------------------------------- DDP

// DDP: 10-byte header (14 with a timecode), then RGB per pixel from the offset.
// Pixel i goes to chip i, RGB channels only, with gamma 2.2 in 16 bits.
void handleDdp() {
  if (!netStarted) return;
  int len = udp.parsePacket();
  if (len <= 0) return;
  len = udp.read(packet, sizeof(packet));
  if (len < 10) return;
  uint8_t flags = packet[0];
  int hdr = (flags & 0x10) ? 14 : 10;
  uint32_t offset = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) | ((uint32_t)packet[6] << 8) | packet[7];
  int dlen = (packet[8] << 8) | packet[9];
  if (hdr + dlen > len) dlen = len - hdr;
  if (!ddpActive) {
    ddpActive = true;
    testMode = false;
  }
  lastDdp = millis();
  int first = offset / 3;
  for (int i = 0; i + 2 < dlen; i += 3) {
    int c = first + i / 3;
    if (c >= chips) break;
    frame[c][R] = gammaLut[packet[hdr + i]];
    frame[c][G] = gammaLut[packet[hdr + i + 1]];
    frame[c][B] = gammaLut[packet[hdr + i + 2]];
    frame[c][CW] = 0;
    frame[c][WW] = 0;
  }
  if (flags & 0x01) {
    ddpFrames++;
    show();
  }
}

// ---------------------------------------------------------------- Improv Serial
// https://www.improv-wifi.com/serial/ - lets the web installer (ESP Web Tools)
// set up Wi-Fi right after flashing. Packet: "IMPROV", version 1, type, length,
// data, checksum (sum of all previous bytes, modulo 256).

enum { IMP_CURRENT_STATE = 1, IMP_ERROR_STATE = 2, IMP_RPC = 3, IMP_RPC_RESULT = 4 };
enum { IMP_READY = 2, IMP_PROVISIONING = 3, IMP_PROVISIONED = 4 };
enum { IMP_ERR_NONE = 0, IMP_ERR_INVALID = 1, IMP_ERR_UNKNOWN_CMD = 2, IMP_ERR_CONNECT = 3 };
enum { CMD_WIFI = 1, CMD_STATE = 2, CMD_INFO = 3, CMD_SCAN = 4 };

void improvSend(uint8_t type, const uint8_t *data, uint8_t len) {
  uint8_t head[9] = {'I', 'M', 'P', 'R', 'O', 'V', 1, type, len};
  uint8_t sum = 0;
  for (uint8_t b : head) sum += b;
  for (int i = 0; i < len; i++) sum += data[i];
  Serial.write(head, 9);
  Serial.write(data, len);
  Serial.write(sum);
  Serial.write('\n');
  Serial.flush();
}

void improvByte(uint8_t type, uint8_t value) { improvSend(type, &value, 1); }

void improvResult(uint8_t cmd, const String *strs, int n) {
  uint8_t buf[250];
  int p = 2;
  for (int i = 0; i < n; i++) {
    int l = min((int)strs[i].length(), 240 - p);
    if (l < 0) break;
    buf[p++] = l;
    memcpy(buf + p, strs[i].c_str(), l);
    p += l;
  }
  buf[0] = cmd;
  buf[1] = p - 2;
  improvSend(IMP_RPC_RESULT, buf, p);
}

String deviceUrl() { return "http://" + WiFi.localIP().toString() + "/"; }

void improvRpc(const uint8_t *d, int len) {
  if (len < 2 || d[1] != len - 2) return improvByte(IMP_ERROR_STATE, IMP_ERR_INVALID);
  improvByte(IMP_ERROR_STATE, IMP_ERR_NONE);
  switch (d[0]) {
    case CMD_WIFI: {
      int sl = d[2];
      if (3 + sl >= len) return improvByte(IMP_ERROR_STATE, IMP_ERR_INVALID);
      int pl = d[3 + sl];
      if (4 + sl + pl > len) return improvByte(IMP_ERROR_STATE, IMP_ERR_INVALID);
      String ssid, pass;
      for (int i = 0; i < sl; i++) ssid += (char)d[3 + i];
      for (int i = 0; i < pl; i++) pass += (char)d[4 + sl + i];
      improvByte(IMP_CURRENT_STATE, IMP_PROVISIONING);
      WiFi.disconnect();
      wifiSsid = ssid;
      wifiPass = pass;
      connectWifi();
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        delay(100);
        if (millis() - lastShow >= FRAME_MS) show();
      }
      if (WiFi.status() != WL_CONNECTED) {
        improvByte(IMP_ERROR_STATE, IMP_ERR_CONNECT);
        improvByte(IMP_CURRENT_STATE, IMP_READY);
        return;
      }
      prefs.putString("ssid", wifiSsid);
      prefs.putString("pass", wifiPass);
      improvByte(IMP_CURRENT_STATE, IMP_PROVISIONED);
      String url = deviceUrl();
      improvResult(CMD_WIFI, &url, 1);
      return;
    }
    case CMD_STATE: {
      bool up = WiFi.status() == WL_CONNECTED;
      improvByte(IMP_CURRENT_STATE, up ? IMP_PROVISIONED : IMP_READY);
      if (up) {
        String url = deviceUrl();
        improvResult(CMD_STATE, &url, 1);
      }
      return;
    }
    case CMD_INFO: {
      String info[4] = {FW_NAME, fwVersion(), "ESP32", hostname};
      improvResult(CMD_INFO, info, 4);
      return;
    }
    case CMD_SCAN: {
      int n = scanNow();
      for (int i = 0; i < n; i++) {
        bool dup = false;  // only the strongest entry per SSID
        for (int j = 0; j < i; j++)
          if (WiFi.SSID(j) == WiFi.SSID(i)) dup = true;
        if (dup || !WiFi.SSID(i).length()) continue;
        String net[3] = {WiFi.SSID(i), String(WiFi.RSSI(i)), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"};
        improvResult(CMD_SCAN, net, 3);
      }
      improvResult(CMD_SCAN, nullptr, 0);
      return;
    }
    default:
      improvByte(IMP_ERROR_STATE, IMP_ERR_UNKNOWN_CMD);
  }
}

// Serial carries both Improv packets and plain text commands. Bytes that start
// "IMPROV" are collected as a packet; everything else builds a text line.
void serialLoop() {
  static uint8_t ib[300];
  static int ip = 0;
  static String line;
  static const char *magic = "IMPROV";
  while (Serial.available()) {
    uint8_t ch = Serial.read();
    if (ip < 6) {
      if (ch == (uint8_t)magic[ip]) {
        ib[ip++] = ch;
        continue;
      }
      for (int i = 0; i < ip; i++) line += (char)ib[i];  // not Improv after all
      ip = 0;
      if (ch == (uint8_t)magic[0]) {
        ib[ip++] = ch;
        continue;
      }
      if (ch == '\n' || ch == '\r') {
        if (line.length()) Serial.println(command(line, true));
        line = "";
      } else if (line.length() < 200) {
        line += (char)ch;
      }
      continue;
    }
    ib[ip++] = ch;
    if (ip >= 9 && ip == 9 + ib[8] + 1) {  // header + data + checksum
      uint8_t sum = 0;
      for (int i = 0; i < ip - 1; i++) sum += ib[i];
      if (ib[6] == 1 && sum == ib[ip - 1] && ib[7] == IMP_RPC) improvRpc(ib + 9, ib[8]);
      else if (ib[6] == 1 && sum != ib[ip - 1]) improvByte(IMP_ERROR_STATE, IMP_ERR_INVALID);
      ip = 0;
    } else if (ip >= (int)sizeof(ib)) {
      ip = 0;
    }
  }
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  WiFi.persistent(false);  // Wi-Fi settings live in our own preferences only
#ifdef RELAY_PIN
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
#endif
  for (int i = 0; i < 256; i++) gammaLut[i] = (uint16_t)(powf(i / 255.0f, 2.2f) * 65535.0f + 0.5f);
  rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)STRIP_PIN, RMT_CH);
  cfg.clk_div = 2;
  // All RMT memory for this one channel: fewer refills while sending, so less
  // chance that a Wi-Fi interrupt mangles a bit.
  cfg.mem_block_num = 8;
  rmt_config(&cfg);
  rmt_driver_install(RMT_CH, 0, 0);
  fillStatic();
  show();

  prefs.begin("gtb", false);
  // isKey first: reading a key that was never written logs an error on a fresh board.
  if (prefs.isKey("chips")) chips = constrain(prefs.getInt("chips", 90), 1, MAX_CHIPS);
  if (prefs.isKey("prefix")) prefix = prefs.getUChar("prefix", 3) & 3;
  if (prefs.isKey("max")) maxLevel = prefs.getFloat("max", 1.0f);
  if (prefs.isKey("ssid")) wifiSsid = prefs.getString("ssid", "");
  if (prefs.isKey("pass")) wifiPass = prefs.getString("pass", "");
  if (prefs.isKey("otapass")) otaPass = prefs.getString("otapass", "");
#ifdef WIFI_SSID
  if (!wifiSsid.length()) {
    wifiSsid = WIFI_SSID;
    wifiPass = WIFI_PASS;
  }
#endif
#ifdef OTA_PASS
  if (!otaPass.length()) otaPass = OTA_PASS;
#endif
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char hn[32];
  snprintf(hn, sizeof(hn), "govee-backlight-%02x%02x", mac[4], mac[5]);
  hostname = hn;
  // The DHCP hostname only takes effect when set before the Wi-Fi interface starts.
  WiFi.setHostname(hostname.c_str());

  Serial.printf("%s %s: pin %d, %d chips, max %.2f\n", FW_NAME, fwVersion(), STRIP_PIN, chips, maxLevel);
  if (wifiSsid.length()) connectWifi();
  else startAp();
}

void loop() {
  serialLoop();
  networkLoop();
  handleDdp();
  if (ddpActive && millis() - lastDdp > DDP_TIMEOUT_MS) {
    ddpActive = false;
    fillStatic();
  }
  if (testMode && !ddpActive) runTest();
  if (millis() - lastShow >= FRAME_MS) show();
}
