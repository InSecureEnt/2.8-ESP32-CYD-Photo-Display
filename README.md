# 2.8-ESP32-CYD-Photo-Display

CYD Photo Frame: project summary
A firmware for the ESP32-2432S028 "Cheap Yellow Display" (dual USB, ST7789) that turns the board into an SD-card photo frame. You can load images by SD card or over WiFi, and install the firmware from a web page.

## What it does

**Slideshow**
- Cycles through `.jpg`, `.jpeg` and `.bmp` files on a FAT32 microSD card, shuffled, every 15 seconds with a fade between images.
- Every image fills the whole screen (scaled and center-cropped, no black bars) in all four rotations. BMPs use nearest-neighbor scaling so pixel art stays crisp.

**Controls**
- Tap the left 30% of the screen for the previous image, the right 30% for the next, and the middle 40% to rotate 90 degrees. The rotation is remembered across reboots.
- The BOOT button skips to the next image on a short press and starts or stops WiFi upload mode on a 1.5-second hold.
- Optional auto-dimming from the onboard light sensor is built in but switched off by default.

**WiFi upload mode**
- The frame creates its own hotspot (named `PhotoFrame-XXXX`, with a random per-device 8-digit password shown on screen) and serves a web page at `192.168.4.1`.
- The page lets you add and delete images. It resizes photos in your browser and converts them to a baseline JPEG, or to a BMP for pixel art, before sending.
- Uploads are capped at 1500 KB and 1024 px per file. The frame also rejects progressive JPEGs, JPEGs with an EXIF rotation tag, and BMPs that aren't 24-bit and uncompressed, and it tells you why.
- The WiFi turns off by itself after 5 minutes with no device connected (or 15 minutes with no activity), and the slideshow resumes with the new images.

## Files

```
cyd_photo_frame/cyd_photo_frame.ino      the whole firmware, with the upload page inside it
tft_config/User_Setup.h                  TFT_eSPI display settings for this board
web_flasher/index.html + manifest.json   the browser-based installer page
.github/workflows/build-and-deploy.yml   automatic build and publish
```

## Build and distribution

- To build locally, install the TFT_eSPI and TJpg_Decoder libraries, copy `User_Setup.h` into the TFT_eSPI folder, and compile as **ESP32 Dev Module** with DIO flash mode and the Huge APP partition scheme.
- The GitHub Actions workflow does the same on every push, then publishes `firmware.bin` and the flasher page to GitHub Pages.
- Anyone with the same board can open the flasher page in Chrome or Edge on a desktop, plug in the board, and click Install.

## Things we fixed along the way

- Duplicate `.ino` files in the sketch folder caused redefinition errors.
- The image flash-and-black symptom was a brownout from too much current at full backlight, so the default brightness is now lower.
- Early JPEG failures came from oversized and progressive phone photos, which the upload page and the validation checks now handle.

## Caveats

- I couldn't compile or flash for the ESP32, so everything was tested only through the problems you reported. The first GitHub Actions run is the first real test of the pipeline.
- The touch left/right direction may need `TOUCH_INVERT_X = true` on some boards.
- The ESP32 can't read EXIF rotation, so as-is JPEGs that rely on it are rejected, and Photos mode applies the rotation before sending.

## Ideas not built yet

- Remember the last image shown across reboots.
- A fit-versus-fill toggle for images that you'd rather see whole.
- A QR code on the upload screen for joining the hotspot.
- GIF or animation support.
- Automatic GitHub Releases with downloadable builds.
