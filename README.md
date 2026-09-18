# HopeTV

A tiny WiFi-connected "TV" built from a Wemos D1 mini (ESP8266) and a 1.44"
128x128 SPI TFT display (ST7735). Shows a color test pattern, classic
black/white TV static, a bouncing-ball animation, or looping GIF clips —
all remote-controllable over OSC (or a Serial console, for testing without
an OSC sender).

## Hardware

- Wemos D1 mini (ESP8266 / ESP-12)
- 1.44" SPI TFT, 128x128, ST7735 driver (commonly sold as "1.44 TFT LCD
  Display Module ST7735 128x128")
- USB power (5V) or a regulated 3.0–3.6V source wired directly to the 3V3 pin

## Pinout

One firmware, two supported wiring layouts — which one is active is chosen
at runtime via the `pinout` key in `config.txt` (`v2` or `prototyp`, see
below), no code changes needed. `v2` is the current/default board with a
re-laid-out, shorter-wire layout; `prototyp` is the original wiring.

| Display pin | `v2` (default) | `prototyp` (original) | Note |
|---|---|---|---|
| VCC | 3V3 | 3V3 | |
| GND | GND | GND | |
| LED / Backlight | D1 (GPIO5) | D1 (GPIO5) | see rewiring note below |
| A0 / DC | D2 (GPIO4) | D3 (GPIO0) | |
| RESET | D3 (GPIO0) | D4 (GPIO2) | |
| CS | D4 (GPIO2) | D2 (GPIO4) | deliberately never D8 (boot issues) |
| SCK | D5 (GPIO14) | D5 (GPIO14) | Hardware SPI SCK, fixed on the ESP8266 |
| SDA / MOSI | D7 (GPIO13) | D7 (GPIO13) | Hardware SPI MOSI, fixed on the ESP8266 |

D0 and D8 are deliberately avoided in both layouts: they have special
boot-strapping functions and can easily cause boot loops or a dead display.

### Backlight rewiring (required for brightness/power control)

The backlight LED must be wired to **D1 (GPIO5)**, not straight to 3V3, for
`/hopetv/brightness` and `/hopetv/power` to have any effect. According to the
module's datasheet, the LED pin is a 3.3V logic control input ("high level
lighting"), not a raw LED — wiring it directly to a GPIO without a transistor
is fine. Without this rewiring the sketch still runs, but brightness/power
commands only blank the drawn image, not the actual backlight.

## Setup

1. **Board package**: add `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
   under Additional Board Manager URLs, then install the ESP8266 package and
   select board **"LOLIN(WEMOS) D1 R2 & mini"**.
2. **Flash size**: Tools → Flash Size → a scheme with at least ~2MB of
   filesystem space (e.g. "4MB (FS:3MB OTA:~512KB)").
3. **Libraries** (via Library Manager):
   - `Adafruit GFX Library`
   - `Adafruit ST7735 and ST7789 Library` (a version with `enableDisplay()`)
   - `OSC` (by CNMAT / Adrian Freed)
   - `AnimatedGIF` (by bitbank2)
4. **WiFi/OSC config**: copy `hopetv/data/config.example.txt` to
   `hopetv/data/config.txt` and fill in your own `ssid`/`password` (and
   `pinout`, if you're on the `prototyp` wiring). `config.txt` is gitignored
   on purpose — never commit your real WiFi credentials.
5. **Upload the data folder** with a LittleFS uploader (IDE 2.x: the
   [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload)
   plugin, run via the command palette — "Upload LittleFS to Pico/ESP8266/ESP32";
   IDE 1.8.x: the "ESP8266 LittleFS Data Upload" menu item). This uploads
   `config.txt` and any `.gif` files.
6. **Flash the sketch** normally.

If `config.txt` is missing, the sketch falls back to the placeholder
defaults hardcoded in the `.ino` file (`YOUR_WIFI_SSID` / `YOUR_WIFI_PASSWORD`),
which will simply fail to connect — always use a real `config.txt` for
actual WiFi use.

## GIF playback

Drop any number of 128x128 `.gif` files into `data/` and upload them with
the filesystem tool — they're auto-detected at boot (sorted alphabetically,
addressable by index 0, 1, 2, ... — see the Serial `clips` command). By
default the device boots straight into GIF mode and loops `clip2.gif` if
present, otherwise the first file found; no cycling through the other modes
unless you turn that on explicitly (`/hopetv/auto 1`).

Playback decodes and draws the GIF one row at a time via the `AnimatedGIF`
library, so it stays RAM-friendly regardless of clip length. `data/` already
ships with two example clips; the source videos are in `hopetv/video/`.

**Creating a new clip**, roughly (adjust the crop values to your source
video — check for letterboxing/aspect ratio first):

```bash
ffmpeg -i input.mov -vf "crop=<w>:<h>:<x>:<y>,scale=128:128:flags=lanczos,\
fps=12,split[s0][s1];[s0]palettegen=max_colors=64[p];\
[s1][p]paletteuse=dither=bayer" -loop 0 clipN.gif
```

Keep clips short (a few seconds) and the palette small (32–128 colors, or
add `hue=s=0` before `split` to desaturate) — file size scales with both
length and color complexity, and LittleFS space is limited (a few MB).

## OSC commands (default port 9000)

| Address | Type | Meaning |
|---|---|---|
| `/hopetv/mode` | int (0-3) | 0=test pattern, 1=noise, 2=animation, 3=GIF; disables auto-cycling |
| `/hopetv/auto` | int (0/1) | auto mode-cycling off/on |
| `/hopetv/debug` | int (0-2) | 0=off, 1=info screen (IP/port/GIF count/last command), 2=file list of found GIFs (paginated every 2s if needed) |
| `/hopetv/power` | int (0/1) | display off/on |
| `/hopetv/brightness` | float (0.0-1.0) | backlight brightness (needs the backlight rewiring above) |
| `/hopetv/fps` | float (1-60) | frame rate for noise/animation (GIF runs at its own native pace) |
| `/hopetv/bw` | int (0/1) | black & white filter, applied to test pattern, animation and GIF playback |
| `/hopetv/clip` | int (index) | select a GIF by index (0-based, alphabetical — see Serial `clips`), switches to GIF mode |

The address must match exactly (lowercase, one leading slash, no spaces) or
the message is received but ignored. Every received OSC message is echoed
over Serial (`OSC empfangen: <address> <value>`) followed by whether it
matched a known command — and in debug mode 1, the on-screen "Letzter OSC:"
line shows `[OK]` or `[?]` right next to the last command, so a typo in the
sender is easy to spot without a Serial Monitor open.

## Testing without an OSC sender

The exact same set of commands also works by typing into the Serial Monitor
(115200 baud, line ending set to "Newline") — just drop the `/hopetv/`
prefix: `mode 3`, `brightness 0.5`, `clip 1`, `clips` (lists found GIFs),
`help` (full command list).

## Repo layout

```
hopetv/             the sketch: hopetv.ino, data/ (GIFs + config),
                     video/ (source clips, .mov gitignored), 3d print/ (enclosure STL files)
```
