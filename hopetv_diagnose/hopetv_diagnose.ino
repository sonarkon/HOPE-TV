// HopeTV - Diagnostic v2: manual RESET pulse + cycling through several init variants
// Purpose: screen stays permanently WHITE -> figure out whether that's a
// RESET/CS/SPI wiring problem or the wrong init variant (tab type).
// Open the Serial Monitor at 115200 baud. The sketch runs in an endless
// loop, no reflashing needed to test different things.
//
// Wiring (identical to the main sketch hopetv.ino):
//   VCC   -> 3V3
//   GND   -> GND
//   CS    -> D2 (GPIO4)
//   RESET -> D4 (GPIO2)
//   A0/DC -> D3 (GPIO0)
//   SDA   -> D7 (GPIO13)  Hardware SPI MOSI
//   SCK   -> D5 (GPIO14)  Hardware SPI SCK
//   LED   -> D1 (GPIO5)

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

#define TFT_CS   D2
#define TFT_DC   D3
#define TFT_RST  D4
#define BACKLIGHT_PIN D1

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

void manualReset() {
  pinMode(TFT_RST, OUTPUT);
  Serial.println("    RESET pin: HIGH (2s)...");
  digitalWrite(TFT_RST, HIGH);
  delay(2000);
  Serial.println("    RESET pin: LOW (2s) - display should be held in reset now...");
  digitalWrite(TFT_RST, LOW);
  delay(2000);
  Serial.println("    RESET pin: HIGH again, short pause...");
  digitalWrite(TFT_RST, HIGH);
  delay(200);
}

void testVariant(const char* name, uint8_t tabType, uint16_t color, const char* colorName) {
  Serial.println();
  Serial.print("=== Testing init variant: ");
  Serial.print(name);
  Serial.println(" ===");
  manualReset();
  tft.initR(tabType);
  tft.setRotation(0);
  tft.fillScreen(color);
  Serial.print("    -> Screen should now be ");
  Serial.print(colorName);
  Serial.println(". 5 seconds to take a look...");
  delay(5000);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== HopeTV Diagnostic v2 ===");
  Serial.println("Important: if the screen stays white/blank for ALL variants below,");
  Serial.println("it's a wiring problem (RESET/CS/SCK/MOSI/DC), not a software issue.");
  Serial.println("If any variant shows a real color, it was the wrong init variant.");

  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);
}

void loop() {
  testVariant("INITR_144GREENTAB", INITR_144GREENTAB, ST77XX_RED,    "RED");
  testVariant("INITR_BLACKTAB",    INITR_BLACKTAB,    ST77XX_GREEN,  "GREEN");
  testVariant("INITR_GREENTAB",    INITR_GREENTAB,    ST77XX_BLUE,   "BLUE");
  testVariant("INITR_REDTAB",      INITR_REDTAB,      ST77XX_YELLOW, "YELLOW");

  Serial.println();
  Serial.println(">>> One pass through all variants done. Repeating from the top. <<<");
  delay(1000);
}
