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
//   "JPEGDEC" (by bitbank2)
// LittleFS, ESP8266WiFi, WiFiUdp are part of the ESP8266 board package.
//
// Clip files: drop any number of .mjpg / .jpg(.jpeg) files (128x128) into
// the /data folder and upload them with the "ESP8266 LittleFS Data Upload"
// plugin - they are auto-detected at boot (sorted alphabetically,
// addressable via index 0,1,2,..., see the serial command "clips"), and each
// plays with the decoder matching its extension: .mjpg via a small custom
// Motion-JPEG player (see below), a lone .jpg/.jpeg is treated as a still
// image and just held on screen. (No animated-GIF support: it was dropped
// deliberately - the AnimatedGIF library's own global decode-state object
// alone used ~24KB, which combined with JPEGDEC left no RAM headroom on the
// ESP8266's small 80KB region for global/static variables. Re-encode old
// .gif clips to .mjpg with tools/frame_mjpeg.py instead - typically smaller
// files too.) Default behavior: the device boots straight into clip mode
// and loops clip2.mjpg if present, otherwise the first file found (no
// auto-cycling through all modes).
//
// .mjpg format: NOT a standard container - a simple custom format made for
// this project (see ../tools/frame_mjpeg.py in the repo root): repeated
// [4-byte little-endian frame length][one JFIF/JPEG frame's bytes], nothing
// else. Makes on-device frame reading trivial (no marker-scanning needed).
// To make one: encode a short 128x128 clip as concatenated JPEG frames with
// ffmpeg, e.g.
//   ffmpeg -i input.mov -vf fps=12 -c:v mjpeg -q:v 10 -an -f mjpeg raw.mjpeg
// then run `python3 tools/frame_mjpeg.py raw.mjpeg output.mjpg`.
//
// Slideshow: separate, dedicated mode - put .jpg/.jpeg stills into
// /data/slideshow/ and upload them; they auto-advance every N seconds
// (default 4, see /hopetv/slideshow/speed), looping forever, independent
// of the main clip playlist above.
//
// Optional: /data/config.txt for SSID/password/OSC port without reflashing.
//
// OSC commands (port from config.txt / default 9000):
//   /hopetv/mode           int   0=Test pattern 1=Noise 2=Animation 3=Clip
//   /hopetv/auto           int   0/1  auto mode-cycling off/on
//   /hopetv/debug          int   0=off 1=info (IP/port/clip+slide count/last command)
//                                 2=file list (found clips, paginated every 2s)
//   /hopetv/power          int   0/1  display off/on
//   /hopetv/brightness     float 0.0-1.0  backlight brightness (see note above)
//   /hopetv/fps            float 1-60  frame rate for noise/animation
//   /hopetv/bw             int   0/1  black & white filter off/on
//   /hopetv/clip           int   index  select a clip by index (0-based, alphabetically
//                                 sorted, see serial "clips"), switches to clip mode
//   /hopetv/slideshow      int   0/1  slideshow mode off/on (see above)
//   /hopetv/slideshow/speed float seconds per slideshow image (default 4)
//
// The same commands also work via the Serial Monitor (115200 baud, line
// ending "Newline"), no OSC sender needed for testing: e.g. "mode 3",
// "brightness 0.5", "clip 1", "slidespeed 2.5". "help" shows the full list.
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
#include <JPEGDEC.h>

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

// Allocated on the heap in setup(), not a global object: JPEGDEC's internal
// decode buffers/Huffman tables are ~18KB, which alone would overflow the
// ESP8266's small (80KB) fixed region for global/static variables.
JPEGDEC *jpeg;

enum ClipTyp { CLIP_MJPG, CLIP_STILL };

#define MAX_CLIPS 16
String clipListe[MAX_CLIPS];
ClipTyp clipTypListe[MAX_CLIPS];
int anzahlClips = 0;
String aktuellerClipPfad = "/01_clip2.mjpg"; // may be overridden by sucheClips()/waehleStandardClip()
ClipTyp aktuellerClipTyp = CLIP_MJPG;

// Motion-JPEG playback (custom .mjpg container, see header comment above)
File mjpgFile;
bool mjpgOffen = false;
uint8_t mjpgFrameBuf[8192]; // generous headroom over the ~3.3KB max frame seen so far
const unsigned long MJPG_FRAME_MS = 83; // ~12 fps, matches how clips are encoded

// Slideshow: separate file list + own mode, independent of the clip playlist
#define MAX_SLIDES 32
String slideListe[MAX_SLIDES];
int anzahlSlides = 0;
int slideIndex = 0;
unsigned long slideStart = 0;
unsigned long slideDauerMs = 4000; // per image, adjustable via /hopetv/slideshow/speed

// Mode name kept as "GIFMODUS" for historical reasons (this used to be
// GIF-only) - it's now the general clip-playlist mode (MJPEG + stills).
enum Mode { TESTBILD, RAUSCHEN, ANIMATION, GIFMODUS, SLIDESHOW, DEBUG };
Mode mode = GIFMODUS; // default: loop the default clip directly, no auto-cycling
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
unsigned long wlanStartZeit = 0; // set by starteWLAN(), used to cap the total connect wait

// State for the ball animation
float ballX = 64, ballY = 64;
float ballVX = 2.3, ballVY = 1.7;
const int ballR = 6;

void setup() {
  Serial.begin(115200);

  ladeConfig(); // mounts LittleFS, reads SSID/password/port/pinout
  waehlePinout();
  sucheClips();
  sucheSlideshow();
  waehleStandardClip();

  tft = new Adafruit_ST7735(pinCS, pinDC, pinRST);
  tft->initR(INITR_144GREENTAB); // correct for 1.44" 128x128 v1.1
  tft->setRotation(1); // +90 degrees (was -90, rotated another 180)
  tft->fillScreen(ST77XX_BLACK);

  pinMode(pinBacklight, OUTPUT);
  analogWriteRange(1023);
  setzeHelligkeit(brightness);

  jpeg = new JPEGDEC();

  // Fire off the WiFi connection now (non-blocking) so it happens in the
  // background while the boot sequence below draws the clip list etc. -
  // by the time we get to actually waiting for it, some/all of the connect
  // time has likely already elapsed.
  starteWLAN();

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

  if (mode == SLIDESHOW) {
    if (millis() - slideStart > slideDauerMs) {
      zeigeSlide(slideIndex + 1);
    }
    return;
  }

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
    if (mode == GIFMODUS) beendeMjpeg();
    mode = (Mode)((mode + 1) % 4); // cycles TESTBILD/RAUSCHEN/ANIMATION/GIFMODUS
    tft->fillScreen(ST77XX_BLACK);
    if (mode == TESTBILD) zeichneTestbild();
    else if (mode == GIFMODUS && aktuellerClipTyp == CLIP_STILL) zeigeStandbild(aktuellerClipPfad);
  }

  if (mode == GIFMODUS) {
    if (aktuellerClipTyp == CLIP_MJPG) {
      if (millis() - lastFrame >= MJPG_FRAME_MS) {
        lastFrame = millis();
        spieleMjpegFrame();
      }
    }
    // CLIP_STILL: nothing to do each frame, already drawn once on selection
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

// Recognizes .mjpg (custom container, see header) and .jpg/.jpeg (single
// still image) - anything else in the root folder is ignored, so e.g.
// config.txt or the slideshow/ subfolder are naturally skipped.
void sucheClips() {
  anzahlClips = 0;
  Dir dir = LittleFS.openDir("/");
  while (dir.next() && anzahlClips < MAX_CLIPS) {
    String name = dir.fileName();
    String nameLower = name;
    nameLower.toLowerCase();
    ClipTyp typ;
    if (nameLower.endsWith(".mjpg")) typ = CLIP_MJPG;
    else if (nameLower.endsWith(".jpg") || nameLower.endsWith(".jpeg")) typ = CLIP_STILL;
    else continue;
    if (!name.startsWith("/")) name = "/" + name;
    clipListe[anzahlClips] = name;
    clipTypListe[anzahlClips] = typ;
    anzahlClips++;
  }
  // simple sort (alphabetical, both arrays in lockstep), lists are small
  for (int i = 0; i < anzahlClips - 1; i++) {
    for (int j = 0; j < anzahlClips - 1 - i; j++) {
      if (clipListe[j] > clipListe[j + 1]) {
        String tauschName = clipListe[j];
        clipListe[j] = clipListe[j + 1];
        clipListe[j + 1] = tauschName;
        ClipTyp tauschTyp = clipTypListe[j];
        clipTypListe[j] = clipTypListe[j + 1];
        clipTypListe[j + 1] = tauschTyp;
      }
    }
  }
  filelistSeitenAnzahl = max(1, (anzahlClips + FILELIST_ZEILEN_PRO_SEITE - 1) / FILELIST_ZEILEN_PRO_SEITE);

  Serial.print(anzahlClips);
  Serial.println(" Clip-Datei(en) gefunden:");
  for (int i = 0; i < anzahlClips; i++) {
    Serial.print("  ");
    Serial.print(i);
    Serial.print(": ");
    Serial.println(clipListe[i]);
  }
}

// Scans /slideshow (separate from the main clip playlist above) for stills.
void sucheSlideshow() {
  anzahlSlides = 0;
  Dir dir = LittleFS.openDir("/slideshow");
  while (dir.next() && anzahlSlides < MAX_SLIDES) {
    String name = dir.fileName();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1); // defensive: keep just the basename
    String nameLower = name;
    nameLower.toLowerCase();
    if (!(nameLower.endsWith(".jpg") || nameLower.endsWith(".jpeg"))) continue;
    slideListe[anzahlSlides++] = "/slideshow/" + name;
  }
  for (int i = 0; i < anzahlSlides - 1; i++) {
    for (int j = 0; j < anzahlSlides - 1 - i; j++) {
      if (slideListe[j] > slideListe[j + 1]) {
        String tausch = slideListe[j];
        slideListe[j] = slideListe[j + 1];
        slideListe[j + 1] = tausch;
      }
    }
  }
  Serial.print(anzahlSlides);
  Serial.println(" Slideshow-Bild(er) gefunden");
}

void waehleStandardClip() {
  for (int i = 0; i < anzahlClips; i++) {
    if (clipListe[i] == "/01_clip2.mjpg" || clipListe[i] == "/clip2.mjpg") {
      aktuellerClipPfad = clipListe[i];
      aktuellerClipTyp = clipTypListe[i];
      return;
    }
  }
  if (anzahlClips > 0) {
    aktuellerClipPfad = clipListe[0];
    aktuellerClipTyp = clipTypListe[0];
  }
}

// Starts the WiFi connection attempt and returns immediately - WiFi.begin()
// itself is non-blocking, the ESP8266 connects in the background. Call
// zeigeWLANStatus() later to wait for/show the result.
void starteWLAN() {
  wlanStartZeit = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  Serial.print("Verbinde mit ");
  Serial.println(config.ssid);
}

// Waits for (and shows) the outcome of the WiFi connection started earlier
// by starteWLAN(). The 15s timeout is measured from wlanStartZeit, not from
// here - if other boot steps already used up several seconds in the
// meantime, only the remaining time is waited (often zero: already connected).
void zeigeWLANStatus(int y) {
  const char* punkte[3] = {".  ", ".. ", "..."};
  int punktIndex = 0;
  const unsigned long GESAMT_TIMEOUT = 15000;

  while (WiFi.status() != WL_CONNECTED && millis() - wlanStartZeit < GESAMT_TIMEOUT) {
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
  if (mode == GIFMODUS) beendeMjpeg();
  mode = (Mode)constrain(m, 0, 3);
  autoCycle = false;
  tft->fillScreen(ST77XX_BLACK);
  if (mode == TESTBILD) zeichneTestbild();
  else if (mode == GIFMODUS && aktuellerClipTyp == CLIP_STILL) zeigeStandbild(aktuellerClipPfad);
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
    if (mode == GIFMODUS) beendeMjpeg();
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
    else if (mode == GIFMODUS && aktuellerClipTyp == CLIP_STILL) zeigeStandbild(aktuellerClipPfad);
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
    Serial.println("Keine Clip-Dateien gefunden (LittleFS-Upload gemacht?)");
    return;
  }
  index = constrain(index, 0, anzahlClips - 1);
  aktuellerClipPfad = clipListe[index];
  aktuellerClipTyp = clipTypListe[index];
  beendeMjpeg(); // closes whichever was open - the next frame opens the new clip
  mode = GIFMODUS;
  autoCycle = false;
  modeStart = millis();
  if (aktuellerClipTyp == CLIP_STILL) zeigeStandbild(aktuellerClipPfad);
}

void setzeSlideshow(bool an) {
  if (an) {
    if (anzahlSlides == 0) {
      Serial.println("Keine Slideshow-Bilder gefunden (data/slideshow/ hochgeladen?)");
      return;
    }
    if (mode == GIFMODUS) beendeMjpeg();
    mode = SLIDESHOW;
    autoCycle = false;
    zeigeSlide(0);
  } else {
    mode = GIFMODUS;
    autoCycle = false;
    tft->fillScreen(ST77XX_BLACK);
    if (aktuellerClipTyp == CLIP_STILL) zeigeStandbild(aktuellerClipPfad);
    modeStart = millis();
  }
}

void setzeSlideSpeed(float sekunden) {
  sekunden = constrain(sekunden, 0.5f, 600.0f);
  slideDauerMs = (unsigned long)(sekunden * 1000.0f);
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

void handleOscSlideshow(OSCMessage &msg) {
  bootSplashActive = false;
  setzeSlideshow(msg.getInt(0) != 0);
}

void handleOscSlideshowSpeed(OSCMessage &msg) {
  bootSplashActive = false;
  setzeSlideSpeed(msg.getFloat(0));
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
    Serial.println("         clip <index> | clips (Liste der gefundenen Clips) |");
    Serial.println("         slideshow <0/1> | slidespeed <seconds>");
    Serial.println("debug: 0=aus 1=Info 2=Dateiliste");
    return;
  }

  if (befehl == "clips") {
    Serial.print(anzahlClips);
    Serial.println(" Clip-Datei(en):");
    for (int i = 0; i < anzahlClips; i++) {
      Serial.print("  ");
      Serial.print(i);
      Serial.print(": ");
      Serial.println(clipListe[i]);
    }
    Serial.print(anzahlSlides);
    Serial.println(" Slideshow-Bild(er)");
    return;
  }

  bootSplashActive = false;

  bool bekannt = (befehl == "mode" || befehl == "auto" || befehl == "debug" ||
                  befehl == "power" || befehl == "brightness" || befehl == "fps" ||
                  befehl == "bw" || befehl == "clip" || befehl == "slideshow" ||
                  befehl == "slidespeed");
  letzterOscBefehl = "serial:" + zeile + (bekannt ? " [OK]" : " [?]");

  if (befehl == "mode") setzeModus(wert.toInt());
  else if (befehl == "auto") setzeAuto(wert.toInt() != 0);
  else if (befehl == "debug") setzeDebugModus(wert.toInt());
  else if (befehl == "power") setzePower(wert.toInt() != 0);
  else if (befehl == "brightness") setzeBrightness(wert.toFloat());
  else if (befehl == "fps") setzeFps(wert.toFloat());
  else if (befehl == "bw") setzeBW(wert.toInt() != 0);
  else if (befehl == "clip") setzeClip(wert.toInt());
  else if (befehl == "slideshow") setzeSlideshow(wert.toInt() != 0);
  else if (befehl == "slidespeed") setzeSlideSpeed(wert.toFloat());
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
  treffer |= msg.dispatch("/hopetv/slideshow/speed", handleOscSlideshowSpeed);
  treffer |= msg.dispatch("/hopetv/slideshow", handleOscSlideshow);

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
  tft->print("Clips: ");
  tft->print(anzahlClips);
  tft->print(" Slides: ");
  tft->println(anzahlSlides);

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

// Debug level 2: list of all found clip files, paginated (advances one page
// every 2s if there are more files than fit on the display).
void zeichneDateiliste() {
  tft->fillScreen(ST77XX_BLACK);
  tft->setTextColor(ST77XX_WHITE);
  tft->setTextSize(1);

  tft->setCursor(4, 4);
  tft->print("Clips (");
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

  bootZeile(y, "Clips: " + String(anzahlClips) + " Slides: " + String(anzahlSlides)); y += zeilenhoehe;

  // List each clip name, but cap the TOTAL time spent here at 5s regardless
  // of how many clips there are (was a flat 1s/clip before - with a dozen+
  // clips that alone delayed the WLAN status past its own point of being useful).
  unsigned long proClipMs = (anzahlClips > 0) ? max(150UL, 5000UL / (unsigned long)anzahlClips) : 0;
  for (int i = 0; i < anzahlClips; i++) {
    tft->fillRect(0, y, 128, 10, ST77XX_BLACK);
    tft->setCursor(4, y);
    tft->println(clipListe[i]);
    delay(proClipMs);
  }
  y += zeilenhoehe;
  bootZeile(y, "SSID: " + config.ssid); y += zeilenhoehe;

  // WiFi was already started in setup() (starteWLAN()) and has been
  // connecting in the background this whole time - this just waits for
  // (and shows) whatever's left of the 15s budget, often nothing at all.
  zeigeWLANStatus(y); y += zeilenhoehe;
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

// Draws one decoded MCU block of a JPEG (still or MJPEG frame) to the display.
int JPEGDraw(JPEGDRAW *pDraw) {
  if (bwFilter) {
    int n = pDraw->iWidth * pDraw->iHeight;
    for (int i = 0; i < n; i++) pDraw->pPixels[i] = wandleFarbe(pDraw->pPixels[i]);
  }
  tft->startWrite();
  tft->setAddrWindow(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);
  tft->writePixels(pDraw->pPixels, pDraw->iWidth * pDraw->iHeight, false, false);
  tft->endWrite();
  return 1;
}

// Decodes and shows a single JPEG still (main clip playlist or slideshow),
// then holds it on screen - nothing more to do per frame after this.
void zeigeStandbild(const String &pfad) {
  File f = LittleFS.open(pfad, "r");
  if (!f) {
    Serial.println("Bild konnte nicht geoeffnet werden (Datei fehlt?)");
    return;
  }
  tft->fillScreen(ST77XX_BLACK);
  if (jpeg->open(f, JPEGDraw)) {
    jpeg->decode(0, 0, 0);
    jpeg->close(); // also closes the underlying File
  } else {
    f.close();
  }
}

const int SLIDE_FADE_SCHRITTE = 10;
const int SLIDE_FADE_SCHRITT_MS = 50; // 10 * 50ms = 500ms je Richtung = 1s gesamt

void zeigeSlide(int index) {
  if (anzahlSlides == 0) return;
  slideIndex = ((index % anzahlSlides) + anzahlSlides) % anzahlSlides;

  // 1s transition via backlight dimming (fade to black, swap image, fade
  // back in) - needs the backlight rewired to D1 (see README), same as
  // /hopetv/brightness. A true pixel crossfade would need two full 128x128
  // framebuffers (~64KB) held in RAM at once, more than reliably fits
  // alongside WiFi + JPEGDEC on the ESP8266.
  for (int i = SLIDE_FADE_SCHRITTE; i >= 0; i--) {
    setzeHelligkeit(brightness * i / SLIDE_FADE_SCHRITTE);
    delay(SLIDE_FADE_SCHRITT_MS);
  }

  zeigeStandbild(slideListe[slideIndex]);

  for (int i = 0; i <= SLIDE_FADE_SCHRITTE; i++) {
    setzeHelligkeit(brightness * i / SLIDE_FADE_SCHRITTE);
    delay(SLIDE_FADE_SCHRITT_MS);
  }

  slideStart = millis();
}

// Reads one frame from the custom .mjpg container (see header comment):
// [4-byte little-endian length][that many JPEG bytes], repeated.
bool naechsterMjpgFrame(uint32_t &frameLen) {
  uint8_t hdr[4];
  if (mjpgFile.read(hdr, 4) != 4) return false; // clean EOF
  frameLen = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
             ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
  if (frameLen == 0 || frameLen > sizeof(mjpgFrameBuf)) return false; // corrupt/oversized
  return mjpgFile.read(mjpgFrameBuf, frameLen) == (int)frameLen;
}

void beendeMjpeg() {
  if (mjpgOffen) {
    mjpgFile.close();
    mjpgOffen = false;
  }
}

void spieleMjpegFrame() {
  if (!mjpgOffen) {
    mjpgFile = LittleFS.open(aktuellerClipPfad, "r");
    if (!mjpgFile) {
      Serial.println("MJPG konnte nicht geoeffnet werden (Datei fehlt?)");
      delay(1000); // avoid hammering in a tight loop if the file is missing
      return;
    }
    mjpgOffen = true;
  }
  uint32_t frameLen;
  if (!naechsterMjpgFrame(frameLen)) {
    mjpgFile.close();
    mjpgOffen = false; // next call reopens it -> infinite loop
    return;
  }
  if (jpeg->openRAM(mjpgFrameBuf, frameLen, JPEGDraw)) {
    jpeg->decode(0, 0, 0);
    jpeg->close();
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

// RANDOM_REG32 (ESP8266 hardware RNG) is already provided by the core (esp8266_peri.h)

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
