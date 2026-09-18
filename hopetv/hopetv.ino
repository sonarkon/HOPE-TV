// D1 mini (ESP8266/ESP-12) + 1.44" SPI TFT 128x128 (ST7735, "v1.1" China clone module)
//
// Wiring (Hardware SPI):
//   LED/Backlight -> see note below (Brightness/Power)
//   SCK           -> D5 (GPIO14)
//   SDA/MOSI      -> D7 (GPIO13)
//   A0/DC         -> D3 (GPIO0)
//   RESET         -> D4 (GPIO2)
//   CS            -> D2 (GPIO4)
//   VCC           -> 3V3
//   GND           -> GND
//
// Brightness/Power (IMPORTANT):
//   For real brightness control and a true "off" state, the backlight LED
//   must NOT be wired directly to 3V3 anymore, but to D1 (GPIO5). According
//   to the manufacturer datasheet, the LED pin is a logic control input
//   (3.3V TTL, "high level lighting"), not a raw LED pin - wiring it
//   directly to D1 without a transistor is fine.
//   Without this rewiring, /hopetv/brightness and /hopetv/power have no
//   visible effect on the backlight (only the drawn image itself goes
//   dark/blank, the backlight itself stays on).
//
// Libraries (install via the Library Manager):
//   "Adafruit GFX Library"
//   "Adafruit ST7735 and ST7789 Library" (version with enableDisplay()!)
//   "OSC" (by CNMAT / Adrian Freed)
//   "AnimatedGIF" (by bitbank2)
// LittleFS, ESP8266WiFi, WiFiUdp are part of the ESP8266 board package.
//
// GIF files: drop any number of .gif files (128x128) into the /data folder
// and upload them with the "ESP8266 LittleFS Data Upload" plugin - they are
// auto-detected at boot (sorted alphabetically, addressable via index
// 0,1,2,..., see the serial command "clips"). Default behavior: the device
// boots straight into GIF mode and loops clip2.gif if present, otherwise the
// first file found (no auto-cycling through all modes). Optional:
// /data/config.txt for SSID/password/OSC port without reflashing.
//
// OSC commands (port from config.txt / default 9000):
//   /hopetv/mode        int   0=Test pattern 1=Noise 2=Animation 3=GIF
//   /hopetv/auto        int   0/1  auto mode-cycling off/on
//   /hopetv/debug       int   0=off 1=info (IP/port/GIF count/last command)
//                              2=file list (found GIFs, paginated every 2s)
//   /hopetv/power       int   0/1  display off/on
//   /hopetv/brightness  float 0.0-1.0  backlight brightness (see note above)
//   /hopetv/fps         float 1-60  frame rate for noise/animation
//   /hopetv/bw          int   0/1  black & white filter off/on
//   /hopetv/clip        int   index  select GIF by index (0-based, alphabetically
//                              sorted, see serial "clips"), switches to GIF mode
//
// The same commands also work via the Serial Monitor (115200 baud, line
// ending "Newline"), no OSC sender needed for testing: e.g. "mode 3",
// "brightness 0.5", "clip 1", "clips" (list files). "help" shows the full list.
// IMPORTANT: serial commands need NO "/hopetv/" prefix (just "mode 3"), but
// real OSC messages DO (full "/hopetv/mode 3", exact, lowercase, one leading
// slash) - otherwise the message is received/displayed but no action is
// executed. In debug mode 1, "Last OSC:" shows [OK] (address recognized) or
// [?] (no matching address) right after the last command.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <OSCMessage.h>
#include <AnimatedGIF.h>

Adafruit_ST7735 *tft;
int8_t pinCS, pinDC, pinRST, pinBacklight;

struct Config {
  String ssid = "YOUR_WIFI_SSID";
  String password = "YOUR_WIFI_PASSWORD";
  uint16_t oscPort = 9000;
  String pinout = "v2"; // "v2" (default, new board) or "prototyp" (old pinout)
} config;

// Pinout "prototyp": CS=D2 DC=D3 RST=D4 LED=D1 (hardware SPI SCK=D5/MOSI=D7 always fixed)
// Pinout "v2":       CS=D4 DC=D2 RST=D3 LED=D1
void waehlePinout() {
  if (config.pinout == "prototyp") {
    pinCS = D2;
    pinDC = D3;
    pinRST = D4;
    pinBacklight = D1;
  } else {
    pinCS = D4;
    pinDC = D2;
    pinRST = D3;
    pinBacklight = D1;
  }
  Serial.print("Pinout: ");
  Serial.println(config.pinout);
}

WiFiUDP udp;

AnimatedGIF gif;
File gifFile;
bool gifOffen = false;
String GIF_DATEI = "/clip2.gif"; // may be overridden by sucheGifs()/waehleStandardClip()

#define MAX_CLIPS 16
String clipListe[MAX_CLIPS];
int anzahlClips = 0;

enum Mode { TESTBILD, RAUSCHEN, ANIMATION, GIFMODUS, DEBUG };
Mode mode = GIFMODUS; // default: loop clip2.gif directly, no auto-cycling
bool autoCycle = false;
bool powerOn = true;
bool bwFilter = false;
float brightness = 1.0f;
unsigned long modeStart = 0;
const unsigned long MODE_DAUER = 6000; // ms per mode

unsigned long frameIntervalMs = 33; // ~30 fps initial default
unsigned long lastFrame = 0;
unsigned long lastDebugDraw = 0;
int debugModus = 0; // 0=off, 1=info, 2=file list
Mode modusVorDebug = GIFMODUS; // saved on entering debug mode, restored on exit
bool autoCycleVorDebug = false;
int filelistSeite = 0;
int filelistSeitenAnzahl = 1;
const int FILELIST_ZEILEN_PRO_SEITE = 8;
int letzteHelligkeitProzent = -1;
int letzteFps = -1;
String letzterAngezeigterOsc = "";
String letzterOscBefehl = "-";

bool bootSplashActive = true;
unsigned long bootSplashStart = 0;
const unsigned long BOOT_SPLASH_DAUER = 10000; // 10s IP display after boot

// State for the ball animation
float ballX = 64, ballY = 64;
float ballVX = 2.3, ballVY = 1.7;
const int ballR = 6;

void setup() {
  Serial.begin(115200);

  ladeConfig(); // mounts LittleFS, reads SSID/password/port/pinout
  waehlePinout();
  sucheGifs();
  waehleStandardClip();

  tft = new Adafruit_ST7735(pinCS, pinDC, pinRST);
  tft->initR(INITR_144GREENTAB); // correct for 1.44" 128x128 v1.1
  tft->setRotation(1); // +90 degrees (was -90, rotated another 180)
  tft->fillScreen(ST77XX_BLACK);

  pinMode(pinBacklight, OUTPUT);
  analogWriteRange(1023);
  setzeHelligkeit(brightness);

  gif.begin(LITTLE_ENDIAN_PIXELS);

  bootSplashActive = true;
  bootSplashStart = millis();
  zeichneBootSequenz();
}

void loop() {
  pruefeOSC();
  pruefeSerialBefehle();

  if (bootSplashActive) {
    if (millis() - bootSplashStart > BOOT_SPLASH_DAUER) {
      bootSplashActive = false;
      tft->fillScreen(ST77XX_BLACK);
      if (mode == TESTBILD) zeichneTestbild();
      modeStart = millis();
    }
    return;
  }

  if (!powerOn) return;

  if (mode == DEBUG) {
    if (debugModus == 2) {
      // only redraw at all when there are multiple pages (to page through) -
      // if everything fits on one page it was already drawn on entry
      if (filelistSeitenAnzahl > 1 && millis() - lastDebugDraw > 2000) {
        zeichneDateiliste();
        lastDebugDraw = millis();
      }
    } else if (millis() - lastDebugDraw > 1000) {
      zeichneDebugDynamisch();
      lastDebugDraw = millis();
    }
    return;
  }

  if (autoCycle && millis() - modeStart > MODE_DAUER) {
    modeStart = millis();
    if (mode == GIFMODUS) beendeGif();
    mode = (Mode)((mode + 1) % 4); // cycles TESTBILD/RAUSCHEN/ANIMATION/GIFMODUS
    tft->fillScreen(ST77XX_BLACK);
    if (mode == TESTBILD) zeichneTestbild();
  }

  if (mode == GIFMODUS) {
    spieleGifFrame(); // times itself via the GIF's own frame delays
  } else if (millis() - lastFrame >= frameIntervalMs) {
    lastFrame = millis();
    switch (mode) {
      case TESTBILD:
        // already drawn, nothing to do
        break;
      case RAUSCHEN:
        zeichneRauschen();
        break;
      case ANIMATION:
        animiereBall();
        break;
      default:
        break;
    }
  }
}

void ladeConfig() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS-Mount fehlgeschlagen, nutze Standardwerte");
    return;
  }
  if (!LittleFS.exists("/config.txt")) {
    Serial.println("Keine config.txt gefunden, nutze Standardwerte");
    return;
  }
  File f = LittleFS.open("/config.txt", "r");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();
    if (key == "ssid") config.ssid = val;
    else if (key == "password") config.password = val;
    else if (key == "oscport") config.oscPort = (uint16_t)val.toInt();
    else if (key == "pinout") config.pinout = val;
  }
  f.close();
  Serial.println("config.txt geladen");
}

void sucheGifs() {
  anzahlClips = 0;
  Dir dir = LittleFS.openDir("/");
  while (dir.next() && anzahlClips < MAX_CLIPS) {
    String name = dir.fileName();
    String nameLower = name;
    nameLower.toLowerCase();
    if (!nameLower.endsWith(".gif")) continue;
    if (!name.startsWith("/")) name = "/" + name;
    clipListe[anzahlClips++] = name;
  }
  // simple sort (alphabetical), lists are small
  for (int i = 0; i < anzahlClips - 1; i++) {
    for (int j = 0; j < anzahlClips - 1 - i; j++) {
      if (clipListe[j] > clipListe[j + 1]) {
        String tausch = clipListe[j];
        clipListe[j] = clipListe[j + 1];
        clipListe[j + 1] = tausch;
      }
    }
  }
  filelistSeitenAnzahl = max(1, (anzahlClips + FILELIST_ZEILEN_PRO_SEITE - 1) / FILELIST_ZEILEN_PRO_SEITE);

  Serial.print(anzahlClips);
  Serial.println(" GIF-Datei(en) gefunden:");
  for (int i = 0; i < anzahlClips; i++) {
    Serial.print("  ");
    Serial.print(i);
    Serial.print(": ");
    Serial.println(clipListe[i]);
  }
}

void waehleStandardClip() {
  for (int i = 0; i < anzahlClips; i++) {
    if (clipListe[i] == "/clip2.gif") {
      GIF_DATEI = clipListe[i];
      return;
    }
  }
  if (anzahlClips > 0) GIF_DATEI = clipListe[0];
}

void verbindeWLAN(int y) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  Serial.print("Verbinde mit ");
  Serial.println(config.ssid);

  const char* punkte[3] = {".  ", ".. ", "..."};
  int punktIndex = 0;

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");

    String status;
    switch (WiFi.status()) {
      case WL_NO_SSID_AVAIL:   status = "SSID fehlt"; break;
      case WL_CONNECT_FAILED:  status = "Fehlgeschlagen"; break;
      case WL_CONNECTION_LOST: status = "Verbindung weg"; break;
      default:                 status = "Verbinde"; break;
    }

    tft->fillRect(0, y, 128, 10, ST77XX_BLACK);
    tft->setCursor(4, y);
    tft->print(status + punkte[punktIndex % 3]);
    punktIndex++;
  }

  tft->fillRect(0, y, 128, 10, ST77XX_BLACK);
  tft->setCursor(4, y);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Verbunden, IP: ");
    Serial.println(WiFi.localIP());
    udp.begin(config.oscPort);
    Serial.print("OSC-Empfang auf Port ");
    Serial.println(config.oscPort);
    tft->print("WLAN verbunden");
  } else {
    Serial.println();
    Serial.println("WLAN-Verbindung fehlgeschlagen - laeuft offline weiter");
    tft->print("WLAN fehlgeschlagen");
    delay(2000); // leave it up longer, easy to read
  }
}

void setzeHelligkeit(float b) {
  b = constrain(b, 0.0f, 1.0f);
  analogWrite(pinBacklight, (int)(b * 1023));
}

// Core functions: shared by both OSC handlers AND serial test commands

void setzeModus(int m) {
  if (mode == GIFMODUS) beendeGif();
  mode = (Mode)constrain(m, 0, 3);
  autoCycle = false;
  tft->fillScreen(ST77XX_BLACK);
  if (mode == TESTBILD) zeichneTestbild();
  modeStart = millis();
}

void setzeAuto(bool an) {
  autoCycle = an;
  modeStart = millis();
}

void setzeDebugModus(int stufe) {
  int neueStufe = constrain(stufe, 0, 2);

  if (neueStufe > 0 && debugModus == 0) {
    // first entry into debug mode: remember the current state so it can be
    // restored on exit (debug 0) instead of always jumping back to the test pattern
    modusVorDebug = mode;
    autoCycleVorDebug = autoCycle;
  }
  debugModus = neueStufe;

  if (debugModus > 0) {
    if (mode == GIFMODUS) beendeGif();
    mode = DEBUG;
    autoCycle = false;
    filelistSeite = 0;
    if (debugModus == 2) {
      zeichneDateiliste(); // show the first page immediately
    } else {
      zeichneDebugStatisch("Debug"); // title/SSID/IP/port drawn once, no flicker afterwards
      letzteHelligkeitProzent = -1; // forces an immediate draw on the first call
      letzteFps = -1;
      letzterAngezeigterOsc = "";
      zeichneDebugDynamisch();
    }
    lastDebugDraw = millis();
  } else {
    mode = modusVorDebug;
    autoCycle = autoCycleVorDebug;
    tft->fillScreen(ST77XX_BLACK);
    if (mode == TESTBILD) zeichneTestbild();
    modeStart = millis();
  }
}

void setzePower(bool an) {
  powerOn = an;
  if (powerOn) {
    tft->enableDisplay(true);
    setzeHelligkeit(brightness);
  } else {
    setzeHelligkeit(0);
    tft->enableDisplay(false);
  }
}

void setzeBrightness(float b) {
  brightness = constrain(b, 0.0f, 1.0f);
  if (powerOn) setzeHelligkeit(brightness);
}

void setzeFps(float fps) {
  fps = constrain(fps, 1.0f, 60.0f);
  frameIntervalMs = (unsigned long)(1000.0f / fps);
}

void setzeBW(bool an) {
  bwFilter = an;
}

void setzeClip(int index) {
  if (anzahlClips == 0) {
    Serial.println("Keine GIF-Dateien gefunden (LittleFS-Upload gemacht?)");
    return;
  }
  index = constrain(index, 0, anzahlClips - 1);
  GIF_DATEI = clipListe[index];
  beendeGif(); // closes any open file, the next frame opens the new one
  mode = GIFMODUS;
  autoCycle = false;
  modeStart = millis();
}

// OSC handlers: thin wrappers around the core functions above

void handleOscMode(OSCMessage &msg) {
  bootSplashActive = false;
  setzeModus(msg.getInt(0));
}

void handleOscAuto(OSCMessage &msg) {
  bootSplashActive = false;
  setzeAuto(msg.getInt(0) != 0);
}

void handleOscDebug(OSCMessage &msg) {
  bootSplashActive = false;
  setzeDebugModus(msg.getInt(0));
}

void handleOscPower(OSCMessage &msg) {
  bootSplashActive = false;
  setzePower(msg.getInt(0) != 0);
}

void handleOscBrightness(OSCMessage &msg) {
  bootSplashActive = false;
  setzeBrightness(msg.getFloat(0));
}

void handleOscFps(OSCMessage &msg) {
  bootSplashActive = false;
  setzeFps(msg.getFloat(0));
}

void handleOscBW(OSCMessage &msg) {
  bootSplashActive = false;
  setzeBW(msg.getInt(0) != 0);
}

void handleOscClip(OSCMessage &msg) {
  bootSplashActive = false;
  setzeClip(msg.getInt(0));
}

// Serial test commands, e.g. "mode 3", "brightness 0.5", "clip 1" - "help" for the list.
// Set the Serial Monitor's line ending to "Newline".
void pruefeSerialBefehle() {
  if (!Serial.available()) return;
  String zeile = Serial.readStringUntil('\n');
  zeile.trim();
  if (zeile.length() == 0) return;

  int leer = zeile.indexOf(' ');
  String befehl = (leer < 0) ? zeile : zeile.substring(0, leer);
  String wert = (leer < 0) ? "" : zeile.substring(leer + 1);
  befehl.trim();
  wert.trim();
  befehl.toLowerCase();

  if (befehl == "help" || befehl == "?") {
    Serial.println("Befehle: mode <0-3> | auto <0/1> | debug <0-2> | power <0/1> |");
    Serial.println("         brightness <0.0-1.0> | fps <1-60> | bw <0/1> |");
    Serial.println("         clip <index> | clips (Liste der gefundenen GIFs)");
    Serial.println("debug: 0=aus 1=Info 2=Dateiliste");
    return;
  }

  if (befehl == "clips") {
    Serial.print(anzahlClips);
    Serial.println(" GIF-Datei(en):");
    for (int i = 0; i < anzahlClips; i++) {
      Serial.print("  ");
      Serial.print(i);
      Serial.print(": ");
      Serial.println(clipListe[i]);
    }
    return;
  }

  bootSplashActive = false;

  bool bekannt = (befehl == "mode" || befehl == "auto" || befehl == "debug" ||
                  befehl == "power" || befehl == "brightness" || befehl == "fps" ||
                  befehl == "bw" || befehl == "clip");
  letzterOscBefehl = "serial:" + zeile + (bekannt ? " [OK]" : " [?]");

  if (befehl == "mode") setzeModus(wert.toInt());
  else if (befehl == "auto") setzeAuto(wert.toInt() != 0);
  else if (befehl == "debug") setzeDebugModus(wert.toInt());
  else if (befehl == "power") setzePower(wert.toInt() != 0);
  else if (befehl == "brightness") setzeBrightness(wert.toFloat());
  else if (befehl == "fps") setzeFps(wert.toFloat());
  else if (befehl == "bw") setzeBW(wert.toInt() != 0);
  else if (befehl == "clip") setzeClip(wert.toInt());
  else Serial.println("Unbekannter Befehl. 'help' fuer Liste.");
}

void pruefeOSC() {
  int size = udp.parsePacket();
  if (size <= 0) return;

  OSCMessage msg;
  while (size--) msg.fill(udp.read());

  if (msg.hasError()) {
    Serial.print("OSC-Empfangsfehler, Code: ");
    Serial.println(msg.getError());
    return;
  }

  char addr[64];
  msg.getAddress(addr, 0);
  String info = String(addr);
  if (msg.size() > 0) {
    if (msg.isInt(0)) info += " " + String(msg.getInt(0));
    else if (msg.isFloat(0)) info += " " + String(msg.getFloat(0), 2);
  }

  Serial.print("OSC empfangen: ");
  Serial.print(info);

  bool treffer = false;
  treffer |= msg.dispatch("/hopetv/mode", handleOscMode);
  treffer |= msg.dispatch("/hopetv/auto", handleOscAuto);
  treffer |= msg.dispatch("/hopetv/debug", handleOscDebug);
  treffer |= msg.dispatch("/hopetv/power", handleOscPower);
  treffer |= msg.dispatch("/hopetv/brightness", handleOscBrightness);
  treffer |= msg.dispatch("/hopetv/fps", handleOscFps);
  treffer |= msg.dispatch("/hopetv/bw", handleOscBW);
  treffer |= msg.dispatch("/hopetv/clip", handleOscClip);

  // [OK] or [?] directly visible on the debug screen (see zeichneDebugDynamisch)
  letzterOscBefehl = info + (treffer ? " [OK]" : " [?]");

  if (treffer) {
    Serial.println("  -> ausgefuehrt");
  } else {
    Serial.println("  -> KEINE passende Adresse! Exakt \"/hopetv/...\" (Kleinschreibung, ein fuehrender Slash, kein Leerzeichen) erwartet.");
  }
}

// Static parts: draw only ONCE on entering debug mode, so there isn't a
// visible full-screen refresh every second.
void zeichneDebugStatisch(const char* titel) {
  tft->fillScreen(ST77XX_BLACK);
  tft->setTextColor(ST77XX_WHITE);
  tft->setTextSize(1);

  tft->setCursor(4, 4);
  tft->println(titel);

  tft->setCursor(4, 20);
  tft->print("SSID: ");
  tft->println(config.ssid);

  tft->setCursor(4, 32);
  if (WiFi.status() == WL_CONNECTED) {
    tft->print("IP: ");
    tft->println(WiFi.localIP());
  } else {
    tft->println("IP: -- offline --");
  }

  tft->setCursor(4, 44);
  tft->print("Port: ");
  tft->println(config.oscPort);

  tft->setCursor(4, 56);
  tft->print("GIFs: ");
  tft->println(anzahlClips);

  tft->setCursor(4, 96);
  tft->println("Letzter OSC:");
}

// Dynamic parts: only redraw the lines whose value has actually changed
// since the last call - no unnecessary flicker.
void zeichneDebugDynamisch() {
  int helligkeitProzent = (int)(brightness * 100);
  if (helligkeitProzent != letzteHelligkeitProzent) {
    tft->fillRect(0, 68, 128, 12, ST77XX_BLACK);
    tft->setCursor(4, 68);
    tft->print("Hell.: ");
    tft->print(helligkeitProzent);
    tft->println("%");
    letzteHelligkeitProzent = helligkeitProzent;
  }

  int fpsWert = (int)(1000.0f / frameIntervalMs);
  if (fpsWert != letzteFps) {
    tft->fillRect(0, 80, 128, 12, ST77XX_BLACK);
    tft->setCursor(4, 80);
    tft->print("FPS: ");
    tft->println(fpsWert);
    letzteFps = fpsWert;
  }

  if (letzterOscBefehl != letzterAngezeigterOsc) {
    tft->fillRect(0, 108, 128, 16, ST77XX_BLACK);
    tft->setCursor(4, 108);
    tft->println(letzterOscBefehl);
    letzterAngezeigterOsc = letzterOscBefehl;
  }
}

// Debug level 2: list of all found GIF files, paginated (advances one page
// every 2s if there are more files than fit on the display).
void zeichneDateiliste() {
  tft->fillScreen(ST77XX_BLACK);
  tft->setTextColor(ST77XX_WHITE);
  tft->setTextSize(1);

  tft->setCursor(4, 4);
  tft->print("GIFs (");
  tft->print(anzahlClips);
  tft->println("):");

  const int zeilenhoehe = 12;
  if (filelistSeite >= filelistSeitenAnzahl) filelistSeite = 0;
  int start = filelistSeite * FILELIST_ZEILEN_PRO_SEITE;

  int y = 18;
  for (int i = start; i < min(anzahlClips, start + FILELIST_ZEILEN_PRO_SEITE); i++) {
    tft->setCursor(4, y);
    tft->println(clipListe[i]);
    y += zeilenhoehe;
  }

  if (anzahlClips == 0) {
    tft->setCursor(4, 18);
    tft->println("(keine gefunden)");
  }

  if (filelistSeitenAnzahl > 1) {
    tft->setCursor(4, 118);
    tft->print("Seite ");
    tft->print(filelistSeite + 1);
    tft->print("/");
    tft->println(filelistSeitenAnzahl);
  }

  filelistSeite++;
}

void bootZeile(int y, const String &text) {
  tft->setCursor(4, y);
  tft->println(text);
  delay(250);
}

void zeichneBootSequenz() {
  tft->fillScreen(ST77XX_BLACK);
  tft->setTextColor(ST77XX_WHITE);
  tft->setTextSize(1);

  int y = 4;
  const int zeilenhoehe = 12;

  bootZeile(y, "HopeTV Boot..."); y += zeilenhoehe;

  bootZeile(y, "GIFs: " + String(anzahlClips) + " gefunden"); y += zeilenhoehe;
  for (int i = 0; i < anzahlClips; i++) {
    tft->fillRect(0, y, 128, 10, ST77XX_BLACK);
    tft->setCursor(4, y);
    tft->println(clipListe[i]);
    delay(1000);
  }
  y += zeilenhoehe;
  bootZeile(y, "SSID: " + config.ssid); y += zeilenhoehe;

  verbindeWLAN(y); y += zeilenhoehe;
  bootSplashStart = millis(); // the full display time only starts counting from here

  if (WiFi.status() == WL_CONNECTED) {
    bootZeile(y, "IP: " + WiFi.localIP().toString());
  } else {
    bootZeile(y, "IP: -- offline --");
  }
  y += zeilenhoehe;
  bootZeile(y, "Port: " + String(config.oscPort)); y += zeilenhoehe;
  bootZeile(y, "Hell.: " + String((int)(brightness * 100)) + "%"); y += zeilenhoehe;
  bootZeile(y, "FPS: " + String((int)(1000.0f / frameIntervalMs))); y += zeilenhoehe;
  bootZeile(y, "Status: bereit");
}

uint16_t wandleFarbe(uint16_t farbe) {
  if (!bwFilter) return farbe;
  uint8_t r = (farbe >> 11) & 0x1F;
  uint8_t g = (farbe >> 5) & 0x3F;
  uint8_t b = farbe & 0x1F;
  uint8_t r8 = (r * 255) / 31;
  uint8_t g8 = (g * 255) / 63;
  uint8_t b8 = (b * 255) / 31;
  // Integer-only math (the ESP8266 has no hardware FPU, float multiplication
  // is very slow) - fixed-point weights 77/150/29 (sum 256) instead of 0.299/0.587/0.114
  uint8_t y = (uint8_t)((77u * r8 + 150u * g8 + 29u * b8) >> 8);
  return tft->color565(y, y, y);
}

void *GIFOpenFile(const char *fname, int32_t *pSize) {
  gifFile = LittleFS.open(fname, "r");
  if (gifFile) {
    *pSize = gifFile.size();
    return (void *)&gifFile;
  }
  return NULL;
}

void GIFCloseFile(void *pHandle) {
  File *f = static_cast<File *>(pHandle);
  if (f != NULL) f->close();
}

int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead = iLen;
  File *f = static_cast<File *>(pFile->fHandle);
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos - 1; // as in the library's own example
  if (iBytesRead <= 0) return 0;
  iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
  pFile->iPos = f->position();
  return iBytesRead;
}

int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) {
  File *f = static_cast<File *>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// Draws one image row of the GIF directly to the display (RAM-friendly)
void GIFDraw(GIFDRAW *pDraw) {
  uint8_t *s;
  uint16_t *d, *usPalette, usTemp[128];
  int x, y, iWidth;

  iWidth = pDraw->iWidth;
  if (iWidth + pDraw->iX > 128) iWidth = 128 - pDraw->iX;
  usPalette = pDraw->pPalette;
  y = pDraw->iY + pDraw->y;
  if (y >= 128 || pDraw->iX >= 128 || iWidth < 1) return;
  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) { // reset to background color
    for (x = 0; x < iWidth; x++) {
      if (s[x] == pDraw->ucTransparent) s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }

  if (pDraw->ucHasTransparency) {
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    int iCount;
    pEnd = s + iWidth;
    x = 0;
    iCount = 0;
    while (x < iWidth) {
      c = ucTransparent - 1;
      d = usTemp;
      while (c != ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent) {
          s--;
        } else {
          *d++ = wandleFarbe(usPalette[c]);
          iCount++;
        }
      }
      if (iCount) {
        tft->startWrite();
        tft->setAddrWindow(pDraw->iX + x, y, iCount, 1);
        tft->writePixels(usTemp, iCount, false, false);
        tft->endWrite();
        x += iCount;
        iCount = 0;
      }
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent) iCount++;
        else s--;
      }
      if (iCount) {
        x += iCount;
        iCount = 0;
      }
    }
  } else {
    s = pDraw->pPixels;
    for (x = 0; x < iWidth; x++) usTemp[x] = wandleFarbe(usPalette[*s++]);
    tft->startWrite();
    tft->setAddrWindow(pDraw->iX, y, iWidth, 1);
    tft->writePixels(usTemp, iWidth, false, false);
    tft->endWrite();
  }
}

void beendeGif() {
  if (gifOffen) {
    gif.close();
    gifOffen = false;
  }
}

void spieleGifFrame() {
  if (!gifOffen) {
    if (gif.open(GIF_DATEI.c_str(), GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
      gifOffen = true;
    } else {
      Serial.println("GIF konnte nicht geoeffnet werden (Datei fehlt?)");
      delay(1000); // avoid hammering in a tight loop if the file is missing
      return;
    }
  }
  if (!gif.playFrame(true, NULL)) {
    gif.close();
    gifOffen = false; // next call reopens it -> infinite loop
  }
}

void zeichneTestbild() {
  // Color bars
  uint16_t farben[] = {
    ST77XX_WHITE, ST77XX_YELLOW, ST77XX_CYAN, ST77XX_GREEN,
    ST77XX_MAGENTA, ST77XX_RED, ST77XX_BLUE, ST77XX_BLACK
  };
  int balkenBreite = 128 / 8;
  for (int i = 0; i < 8; i++) {
    tft->fillRect(i * balkenBreite, 0, balkenBreite, 90, wandleFarbe(farben[i]));
  }

  // Grayscale strip below
  for (int i = 0; i < 8; i++) {
    uint8_t g = i * 32;
    uint16_t grau = tft->color565(g, g, g);
    tft->fillRect(i * balkenBreite, 90, balkenBreite, 20, grau);
  }

  tft->drawRect(0, 0, 128, 128, ST77XX_WHITE);
  tft->setTextColor(ST77XX_WHITE);
  tft->setTextSize(1);
  tft->setCursor(4, 114);
  tft->print("HOPE TV TEST");
}

#define RANDOM_REG32 (*(volatile uint32_t *)0x3FF20E44) // ESP8266 hardware RNG

void zeichneRauschen() {
  // full-screen black/white bit noise + darkened scanlines, like classic
  // analog TV static (not just scattered dots)
  static uint16_t zeile[128];
  const uint16_t GRAU_DUNKEL = tft->color565(70, 70, 70);

  tft->startWrite();
  tft->setAddrWindow(0, 0, 128, 128);
  for (int y = 0; y < 128; y++) {
    bool scanline = (y % 3 == 0);
    for (int x = 0; x < 128; x += 32) {
      uint32_t bits = RANDOM_REG32;
      for (int b = 0; b < 32; b++) {
        bool weiss = bits & (1UL << b);
        zeile[x + b] = weiss ? (scanline ? GRAU_DUNKEL : ST77XX_WHITE) : ST77XX_BLACK;
      }
    }
    tft->writePixels(zeile, 128);
  }
  tft->endWrite();
}

void animiereBall() {
  static uint16_t bgColor = ST77XX_BLACK;
  // erase the old ball
  tft->fillCircle((int)ballX, (int)ballY, ballR, bgColor);

  ballX += ballVX;
  ballY += ballVY;
  if (ballX <= ballR || ballX >= 128 - ballR) ballVX = -ballVX;
  if (ballY <= ballR || ballY >= 128 - ballR) ballVY = -ballVY;

  tft->fillCircle((int)ballX, (int)ballY, ballR, wandleFarbe(ST77XX_ORANGE));
}
