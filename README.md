# HopeTV

A tiny WiFi-connected "TV" built from a Wemos D1 mini (ESP8266) and a 1.44"
128x128 SPI TFT display (ST7735). Shows a color test pattern, classic
black/white TV static, a bouncing-ball animation, a playlist of looping
Motion-JPEG clips and still images, or a dedicated photo slideshow — all
remote-controllable over OSC (or a Serial console, for testing without an
OSC sender).

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
| CS | D4 (GPIO2) | D2 (GPIO4) | deliberately never D8 (boot issues) |
| RESET | D3 (GPIO0) | D4 (GPIO2) | |
| A0 / DC | D2 (GPIO4) | D3 (GPIO0) | |
| SDA / MOSI | D7 (GPIO13) | D7 (GPIO13) | Hardware SPI MOSI, fixed on the ESP8266 |
| SCK | D5 (GPIO14) | D5 (GPIO14) | Hardware SPI SCK, fixed on the ESP8266 |
| LED / Backlight | D1 (GPIO5) | D1 (GPIO5) | PWM-controlled, see Brightness/Power note below |

D0 and D8 are deliberately avoided in both layouts: they have special
boot-strapping functions and can easily cause boot loops or a dead display.

### Brightness/Power

The backlight LED is wired to **D1 (GPIO5)**, not straight to 3V3, so
`/hopetv/brightness` and `/hopetv/power` can dim/switch it via PWM. Per the
module's datasheet, the LED pin is a 3.3V logic control input ("high level
lighting"), not a raw LED — driving it directly from a GPIO without a
transistor is fine.

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
   - `JPEGDEC` (by bitbank2)
4. **WiFi/OSC config**: copy `hopetv/data/config.example.txt` to
   `hopetv/data/config.txt` and fill in your own `ssid`/`password` (and
   `pinout`, if you're on the `prototyp` wiring). `config.txt` is gitignored
   on purpose — never commit your real WiFi credentials.
5. **Upload the data folder** with a LittleFS uploader (IDE 2.x: the
   [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload)
   plugin, run via the command palette — "Upload LittleFS to Pico/ESP8266/ESP32";
   IDE 1.8.x: the "ESP8266 LittleFS Data Upload" menu item). This uploads
   `config.txt`, the clip files, and `slideshow/`.
6. **Flash the sketch** normally.

If `config.txt` is missing, the sketch falls back to the placeholder
defaults hardcoded in the `.ino` file (`YOUR_WIFI_SSID` / `YOUR_WIFI_PASSWORD`),
which will simply fail to connect — always use a real `config.txt` for
actual WiFi use.

WiFi connects in the background: `WiFi.begin()` fires immediately at boot
(non-blocking), while the boot screen draws the clip list etc.; by the time
it actually waits for a result, part or all of the connect time has usually
already passed. Listing found clip names on the boot screen is capped at a
total of 5 seconds regardless of how many there are (a fixed 1s/clip
delay doesn't scale once you have a dozen+ clips).

## Clip playback (Motion-JPEG / stills)

Drop any number of 128x128 clip files into `data/` and upload them with the
filesystem tool — they're auto-detected at boot (sorted alphabetically,
addressable by index 0, 1, 2, ... — see the Serial `clips` command), and
each plays with the decoder matching its extension:

| Extension | Decoder | Notes |
|---|---|---|
| `.mjpg` | custom Motion-JPEG player (see below), via `JPEGDEC` | the only animated clip format |
| `.jpg` / `.jpeg` | `JPEGDEC`, decoded once | a lone still image, just held on screen |

**No animated-GIF support** — it was dropped deliberately. `AnimatedGIF`'s
own global decode-state object alone used ~24KB; combined with `JPEGDEC`
(added for stills/slideshow, ~18KB heap-allocated) that left almost no RAM
headroom on the ESP8266's small 80KB region for global/static variables
(90% used, no room for anything else). Dropping `AnimatedGIF` and
re-encoding old `.gif` clips as `.mjpg` instead brought that down to 60% —
and the two example clips got *smaller* in the process (see below). Old
`.gif` files can be converted the same way as any other source video (just
point ffmpeg at the `.gif` as input instead of a video file).

By default the device boots straight into clip mode and loops `clip2.mjpg`
if present, otherwise the first file found; no cycling through the other
modes unless you turn that on explicitly (`/hopetv/auto 1`) — and no
auto-advancing through the whole clip playlist either, `/hopetv/clip
<index>` (or the Serial `clip` command) just jumps to and loops/holds one
specific clip.

**`.mjpg` is not a standard container** — it's a tiny custom format made for
this project (see `tools/frame_mjpeg.py`): repeated `[4-byte little-endian
frame length][that many JPEG bytes]`, nothing else. That makes on-device
frame reading trivial (a length-prefixed read, no marker-scanning), at the
cost of not being playable by anything other than this firmware.

**Creating a new `.mjpg` clip**, roughly (adjust the crop values to your
source video/GIF — check for letterboxing/aspect ratio first):

```bash
ffmpeg -i input.mov -vf "crop=<w>:<h>:<x>:<y>,scale=128:128:flags=lanczos,fps=12" \
  -c:v mjpeg -huffman default -pix_fmt yuvj420p -q:v 10 -an -f mjpeg raw.mjpeg
python3 tools/frame_mjpeg.py raw.mjpeg clipN.mjpg
```

**`-huffman default -pix_fmt yuvj420p` are required, not optional.** FFmpeg's
mjpeg encoder defaults to per-frame-optimized ("optimal") Huffman tables and
a non-canonical chroma subsampling layout; both are valid JPEG but JPEGDEC
(the decoder this firmware uses) fails to decode the vast majority of frames
encoded that way — the exact symptom is clips/stills that look glitchy
(color noise, torn/shifted rows, wrong colors) on the device, verified by
running every frame of an affected clip through JPEGDEC's own reference
decoder offline. `-huffman default` forces the standard JPEG Huffman tables
and `-pix_fmt yuvj420p` forces canonical 4:2:0 subsampling — together these
reliably produce frames JPEGDEC decodes correctly.

Keep clips short (a few seconds) — file size scales with length; `q:v 10`
at 12fps runs well under 100KB for a several-second clip. LittleFS space is
limited to a few MB total.

## Slideshow

A separate, dedicated mode for cycling through photos, independent of the
clip playlist above: drop `.jpg`/`.jpeg` stills into `data/slideshow/` and
upload them — `/hopetv/slideshow 1` (or Serial `slideshow 1`) switches to
it, each image is shown before auto-advancing to the next, looping forever.
`/hopetv/slideshow 0` returns to clip mode. Default is 4 seconds/image,
adjustable with `/hopetv/slideshow/speed <seconds>` (or Serial `slidespeed
<seconds>`), e.g. `slidespeed 2.5`.

Optionally, each transition can fade to black, swap the image, then fade
back in via backlight PWM dimming, same mechanism as `/hopetv/brightness`.
Off by default; enable/adjust it with `/hopetv/slideshow/fade <ms>` (or
Serial `slidefade <ms>`), where `<ms>` is the total fade duration (0 turns
it off again), e.g. `slidefade 1000` for a 1-second fade. This isn't a true
pixel crossfade between the two images (that would need two full 128x128
framebuffers in RAM at once, ~64KB, more than reliably fits alongside WiFi +
JPEGDEC), but reads as a clean fade transition in practice.

```bash
ffmpeg -i input.jpg -vf "crop=min(iw\,ih):min(iw\,ih):(iw-min(iw\,ih))/2:(ih-min(iw\,ih))/2,\
scale=128:128:flags=lanczos" -huffman default -pix_fmt yuvj420p -update 1 -q:v 5 output.jpg
```

Same `-huffman default -pix_fmt yuvj420p` requirement as clips above (see
note there) — without it, stills decode just as unreliably as clips do.

## OSC commands (default port 9000)

| Address | Type | Meaning |
|---|---|---|
| `/hopetv/mode` | int (0-3) | 0=test pattern, 1=noise, 2=animation, 3=clip; disables auto-cycling |
| `/hopetv/auto` | int (0/1) | auto mode-cycling off/on |
| `/hopetv/debug` | int (0-2) | 0=off, 1=info screen (IP/port/clip+slide count/last command), 2=file list of found clips (paginated every 2s if needed) |
| `/hopetv/power` | int (0/1) | display off/on |
| `/hopetv/brightness` | float (0.0-1.0) | backlight brightness |
| `/hopetv/fps` | float (1-60) | frame rate for noise/animation (MJPEG clips run at their own native pace) |
| `/hopetv/bw` | int (0/1) | black & white filter, applied to test pattern, animation, and all clip/slideshow playback |
| `/hopetv/clip` | int (index) | select a clip by index (0-based, alphabetical — see Serial `clips`), switches to clip mode |
| `/hopetv/clip/nr` | int (number) | select a clip by its filename number prefix (e.g. `3` → `03_mond_1.jpg`), independent of list position; Serial `clipnr 3` |
| `/hopetv/slideshow` | int (0/1) | slideshow mode off/on (see Slideshow section above) |
| `/hopetv/slideshow/speed` | float (seconds) | seconds per slideshow image (default 4) |
| `/hopetv/slideshow/fade` | int (ms) | total fade transition duration; 0 = off (default) |

The address must match exactly (lowercase, one leading slash, no spaces) or
the message is received but ignored. Every received OSC message is echoed
over Serial (`OSC empfangen: <address> <value>`) followed by whether it
matched a known command — and in debug mode 1, the on-screen "Letzter OSC:"
line shows `[OK]` or `[?]` right next to the last command, so a typo in the
sender is easy to spot without a Serial Monitor open.

## Testing without an OSC sender

The exact same set of commands also works by typing into the Serial Monitor
(115200 baud, line ending set to "Newline") — just drop the `/hopetv/`
prefix: `mode 3`, `brightness 0.5`, `clip 1`, `slideshow 1`, `slidespeed
2.5`, `slidefade 1000`, `clips` (lists found clips + slide count), `help`
(full command list).

## Repo layout

```
hopetv/             the sketch: hopetv.ino, data/ (clips, slideshow/, config),
                     videoclips/ (raw source clips/photos, gitignored),
                     3d print/ (enclosure STL files)
tools/               frame_mjpeg.py - converts a raw ffmpeg MJPEG stream into
                     the custom length-prefixed .mjpg format this firmware reads
```
