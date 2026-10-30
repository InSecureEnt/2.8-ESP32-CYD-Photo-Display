/*
  CYD (ESP32-2432S028R) Pixel-Art Photo Frame
  - Cycles through .jpg/.jpeg/.bmp files on the SD card
  - Fades between images, auto-dims with the onboard light sensor
  - BOOT button (GPIO0) = next image

  Libraries (Library Manager):
    TFT_eSPI       (configure it with the build flags / User_Setup below)
    TJpg_Decoder   (by Bodmer)

  Pixel art tip: save BMPs (24-bit) or JPEGs at exactly 320x240,
  upscaled with NEAREST-NEIGHBOR so pixels stay crisp.
  JPEG compression blurs pixel art edges, so BMP looks best.
*/

#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <vector>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// ---------- Upload web page (served in WiFi upload mode) ----------
// Resizes photos in the browser before sending, so phone photos arrive as small
// baseline JPEGs the ESP32 can decode.
const char UPLOAD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Photo Frame Uploader</title>
<style>
:root{--bg:#fafafa;--fg:#1a1a1a;--card:#fff;--line:#ddd;--acc:#2563eb;--mut:#666}
@media(prefers-color-scheme:dark){:root{--bg:#121212;--fg:#eee;--card:#1e1e1e;--line:#333;--acc:#60a5fa;--mut:#999}}
body{margin:0;font:16px system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:640px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:0 0 12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:14px}
label.opt{display:block;margin:6px 0}
small{color:var(--mut)}
#drop{border:2px dashed var(--line);text-align:center;cursor:pointer;padding:28px 12px}
#drop.over{border-color:var(--acc)}
button{font:inherit;padding:2px 10px;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--fg)}
.row{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:6px 0;border-top:1px solid var(--line)}
.row span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
progress{width:100%;margin-bottom:8px}
#log div{font-size:14px;margin:2px 0}
.err{color:#dc2626}
</style></head><body><main>
<h1>Photo Frame</h1>

<div class="card">
<label class="opt"><input type="radio" name="mode" value="photo" checked> Photos: convert to JPEG and resize <small>(best for pictures)</small></label>
<label class="opt"><input type="radio" name="mode" value="pixel"> Pixel art: convert to BMP, no smoothing <small>(keeps edges crisp)</small></label>
<label class="opt"><input type="radio" name="mode" value="raw"> Upload as-is <small>(.jpg or .bmp only; baseline JPEG with no EXIF rotation)</small></label>
</div>

<div id="drop" class="card">Tap to choose images<br><small>or drop them here</small></div>
<input id="pick" type="file" accept="image/*,.bmp" multiple hidden>
<progress id="bar" value="0" max="1" hidden></progress>
<div id="log"></div>

<div class="card"><b>On the frame</b> <small id="info"></small><div id="files"></div></div>

<script>
const $=id=>document.getElementById(id);
const FIT=640;                         // display-ready size used for photos
let LIMIT={bytes:1536000,dim:1024};    // replaced by the frame's real limits on load
const base=n=>n.replace(/\.[^.]*$/,'');
const log=(t,c)=>{const d=document.createElement('div');d.textContent=t;if(c)d.className=c;$('log').prepend(d);return d;};

async function loadImg(file){
  if(window.createImageBitmap){
    try{return await createImageBitmap(file,{imageOrientation:'from-image'});}catch(e){}
  }
  return new Promise((ok,bad)=>{
    const i=new Image();
    i.onload=()=>ok(i);
    i.onerror=()=>bad(new Error('Not a readable image'));
    i.src=URL.createObjectURL(file);
  });
}
function paint(src,smooth,maxSide){
  const sw=src.naturalWidth||src.width, sh=src.naturalHeight||src.height;
  const s=Math.min(1,maxSide/Math.max(sw,sh));
  const w=Math.max(1,Math.round(sw*s)), h=Math.max(1,Math.round(sh*s));
  const c=document.createElement('canvas');c.width=w;c.height=h;
  const g=c.getContext('2d');
  g.fillStyle='#000';g.fillRect(0,0,w,h);
  g.imageSmoothingEnabled=smooth;
  if(smooth)g.imageSmoothingQuality='high';
  g.drawImage(src,0,0,w,h);
  return {c,g,w,h};
}
async function toJpeg(file){
  const src=await loadImg(file);
  // Shrink the quality, then the size, until it fits under the frame's limit
  for(let side=FIT;side>=160;side=Math.round(side*0.8)){
    const {c}=paint(src,true,side);
    for(const q of [0.9,0.8,0.7]){
      const b=await new Promise(r=>c.toBlob(r,'image/jpeg',q));
      if(b&&b.size<=LIMIT.bytes)return b;
    }
  }
  throw new Error('could not shrink it under the size limit');
}
async function toBmp(file){
  const src=await loadImg(file);
  let side=FIT, p=paint(src,false,side);
  while(54+((p.w*3+3)&~3)*p.h>LIMIT.bytes&&side>64){side=Math.round(side*0.8);p=paint(src,false,side);}
  const {g,w,h}=p;
  const px=g.getImageData(0,0,w,h).data;
  const rowSize=(w*3+3)&~3, size=54+rowSize*h;
  if(size>LIMIT.bytes)throw new Error('could not shrink it under the size limit');
  const buf=new ArrayBuffer(size), dv=new DataView(buf), u8=new Uint8Array(buf);
  dv.setUint16(0,0x4D42,true); dv.setUint32(2,size,true); dv.setUint32(10,54,true);
  dv.setUint32(14,40,true); dv.setInt32(18,w,true); dv.setInt32(22,h,true);
  dv.setUint16(26,1,true); dv.setUint16(28,24,true); dv.setUint32(34,rowSize*h,true);
  let o=54;
  for(let y=h-1;y>=0;y--){
    let p=y*w*4;
    for(let x=0;x<w;x++){u8[o++]=px[p+2];u8[o++]=px[p+1];u8[o++]=px[p];p+=4;}
    o+=rowSize-w*3;
  }
  return new Blob([buf],{type:'image/bmp'});
}

function send(blob,name,onprog){
  return new Promise((ok,bad)=>{
    const fd=new FormData();fd.append('file',blob,name);
    const x=new XMLHttpRequest();x.open('POST','/upload');
    x.upload.onprogress=e=>{if(e.lengthComputable)onprog(e.loaded/e.total);};
    x.onload=()=>x.status===200?ok():bad(new Error(x.responseText||'the frame rejected it'));
    x.onerror=()=>bad(new Error('connection lost'));
    x.send(fd);
  });
}

async function handle(files){
  const mode=document.querySelector('input[name=mode]:checked').value;
  $('bar').hidden=false;
  for(const f of files){
    const line=log('Preparing '+f.name+'...');
    try{
      let blob=f,name=f.name;
      if(mode==='raw'&&f.size>LIMIT.bytes)throw new Error('too large (max '+Math.round(LIMIT.bytes/1024)+' KB)');
      if(mode==='photo'){blob=await toJpeg(f);name=base(f.name)+'.jpg';}
      else if(mode==='pixel'){blob=await toBmp(f);name=base(f.name)+'.bmp';}
      line.textContent='Sending '+name+'...';
      await send(blob,name,p=>{$('bar').value=p;});
      line.textContent='Saved '+name+' ('+Math.round(blob.size/1024)+' KB)';
    }catch(e){line.textContent='Failed '+f.name+': '+e.message;line.className='err';}
  }
  $('bar').hidden=true;$('bar').value=0;
  refresh();
}

async function refresh(){
  try{
    const r=await (await fetch('/list')).json();
    if(r.maxBytes)LIMIT={bytes:r.maxBytes,dim:r.maxDim};
    $('info').textContent=r.files.length+' images, '+r.card+' MB card, max '+Math.round(LIMIT.bytes/1024)+' KB per image';
    const box=$('files');box.textContent='';
    r.files.sort((a,b)=>a.n.localeCompare(b.n)).forEach(f=>{
      const row=document.createElement('div');row.className='row';
      const s=document.createElement('span');s.textContent=f.n+'  ('+Math.round(f.s/1024)+' KB)';
      const b=document.createElement('button');b.textContent='Delete';
      b.onclick=async()=>{
        if(!confirm('Delete '+f.n+'?'))return;
        await fetch('/delete?name='+encodeURIComponent(f.n),{method:'POST'});
        refresh();
      };
      row.append(s,b);box.append(row);
    });
  }catch(e){$('info').textContent='(cannot reach the frame)';}
}

const drop=$('drop'),pick=$('pick');
drop.onclick=()=>pick.click();
pick.onchange=()=>{handle([...pick.files]);pick.value='';};
['dragenter','dragover'].forEach(t=>drop.addEventListener(t,e=>{e.preventDefault();drop.classList.add('over');}));
['dragleave','drop'].forEach(t=>drop.addEventListener(t,e=>{e.preventDefault();drop.classList.remove('over');}));
drop.addEventListener('drop',e=>handle([...e.dataTransfer.files]));
refresh();
</script></main></body></html>)rawliteral";

// ---------- Settings ----------
const char*    IMAGE_DIR       = "/";       // folder on SD card, e.g. "/frames"
const uint32_t SLIDE_INTERVAL  = 15000;     // ms between images
const bool     SHUFFLE         = true;
const bool     AUTO_DIM        = false;     // set true once LDR readings look sane
const bool     LDR_DEBUG       = true;      // print raw LDR values to Serial
const bool     LDR_LOW_IS_BRIGHT = true;    // flip if dimming works backwards
const uint8_t  MAX_BRIGHTNESS  = 110;   // raise gradually once power is stable
const uint8_t  MIN_BRIGHTNESS  = 60;

// Touch zones: left 30% = previous, right 30% = next, middle 40% = rotate
const float    ZONE_EDGE       = 0.30f;
const bool     TOUCH_INVERT_X  = false;     // set true if left/right zones are swapped
const bool     TOUCH_DEBUG     = true;      // print raw touch values to Serial

// WiFi upload mode: hold the BOOT button this long to start/stop it
const uint32_t LONG_PRESS_MS   = 1500;
const uint32_t UPLOAD_TIMEOUT  = 15UL * 60UL * 1000UL;   // auto-exit after 15 min with no web activity
const uint32_t NO_CLIENT_TIMEOUT = 5UL * 60UL * 1000UL;  // WiFi off if no device is connected for 5 min

// Upload limits: anything beyond these is rejected so every image displays properly
const uint32_t MAX_UPLOAD_BYTES = 1500UL * 1024UL;   // per file
const uint16_t MAX_IMAGE_DIM    = 1024;              // longest side, in pixels

// ---------- Pins (CYD) ----------
#define PIN_BL      21
#define PIN_LDR     34
#define PIN_BUTTON   0
#define PIN_TOUCH_IRQ 36   // XPT2046 PENIRQ: LOW while the screen is touched
#define T_CLK        25    // XPT2046 pins (read by bit-banging so the SD's SPI bus is untouched)
#define T_MOSI       32
#define T_MISO       39
#define T_CS         33
#define SD_CS        5
#define SD_SCK      18
#define SD_MISO     19
#define SD_MOSI     23

TFT_eSPI tft = TFT_eSPI();
std::vector<String> files;
int currentIndex = -1;
uint32_t lastChange = 0;
uint32_t lastLdr = 0;
uint8_t  brightness = MAX_BRIGHTNESS;
uint8_t  rotation = 1;      // 0..3; 1 and 3 are landscape, 0 and 2 are portrait
Preferences prefs;          // saves the rotation across reboots

WebServer server(80);
bool     sdOk = false;
bool     uploadMode = false;
uint32_t lastUploadActivity = 0;
uint32_t lastClientSeen = 0;      // last time at least one device was joined to the hotspot
String   apSsid, apPass;
File     uploadFile;
String   uploadTmpPath, uploadFinalPath;
bool     uploadOk = false;
int      uploadedCount = 0;
String   uploadReason;            // why the last upload was refused (sent back to the web page)
uint32_t uploadBytes = 0;

// ---------- Backlight ----------
void setBacklight(uint8_t v) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  analogWrite(PIN_BL, v);
#else
  ledcWrite(0, v);
#endif
}

void backlightInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  pinMode(PIN_BL, OUTPUT);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BL, 0);
#endif
  setBacklight(0);
}

void fadeTo(uint8_t from, uint8_t to) {
  int step = (to > from) ? 5 : -5;
  int v = from;
  while ((step > 0 && v < to) || (step < 0 && v > to)) {
    v += step;
    setBacklight(constrain(v, 0, 255));
    delay(6);
  }
  setBacklight(to);
}

void updateAutoBrightness() {
  if (millis() - lastLdr < 500) return;
  lastLdr = millis();
  int raw = analogRead(PIN_LDR);  // 0..4095
  if (LDR_DEBUG) Serial.printf("LDR raw: %d\n", raw);
  if (!AUTO_DIM) return;
  int target = LDR_LOW_IS_BRIGHT
      ? map(raw, 200, 3000, MAX_BRIGHTNESS, MIN_BRIGHTNESS)
      : map(raw, 200, 3000, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
  target = constrain(target, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
  // smooth it so it doesn't flicker
  brightness = (brightness * 7 + target) / 8;
  setBacklight(brightness);
}

// ---------- JPEG ----------
// "Cover" scaling: the picture is scaled until it fills the WHOLE screen, then
// center-cropped (like CSS object-fit: cover). Works in every rotation.
struct CoverMap {
  float k;            // scale from decoded JPEG pixels to screen pixels
  float offX, offY;   // crop offsets, in screen pixels
  int   dW, dH;       // screen size
  bool  fast;         // k == 1: no resampling needed
};
CoverMap cm;

bool tftOutput(int16_t bx, int16_t by, uint16_t bw, uint16_t bh, uint16_t* bitmap) {
  // Blocks arrive top-to-bottom, so once we are below the screen we can stop.
  if (by * cm.k - cm.offY >= cm.dH) return false;

  if (cm.fast) {
    // Image already matches the screen scale: just shift by the crop offset.
    tft.pushImage(bx - (int)cm.offX, by - (int)cm.offY, bw, bh, bitmap);
    return true;
  }

  // Nearest-neighbour resample of this block into screen pixels.
  int dx0 = max(0,      (int)ceilf(bx * cm.k - cm.offX));
  int dx1 = min(cm.dW,  (int)ceilf((bx + bw) * cm.k - cm.offX));   // exclusive
  int dy0 = max(0,      (int)ceilf(by * cm.k - cm.offY));
  int dy1 = min(cm.dH,  (int)ceilf((by + bh) * cm.k - cm.offY));   // exclusive
  if (dx1 <= dx0 || dy1 <= dy0) return true;

  static uint16_t line[320];
  for (int dy = dy0; dy < dy1; dy++) {
    int sy = constrain((int)((dy + cm.offY) / cm.k) - by, 0, (int)bh - 1);
    for (int dx = dx0; dx < dx1; dx++) {
      int sx = constrain((int)((dx + cm.offX) / cm.k) - bx, 0, (int)bw - 1);
      line[dx - dx0] = bitmap[sy * bw + sx];
    }
    tft.pushImage(dx0, dy, dx1 - dx0, 1, line);
  }
  return true;
}

bool drawJpg(const String& path) {
  uint16_t w = 0, h = 0;
  int sizeErr = (int)TJpgDec.getSdJpgSize(&w, &h, path.c_str());
  if (sizeErr != 0) {
    Serial.printf("JPEG header error %d (8 = progressive/unsupported) in %s\n",
                  sizeErr, path.c_str());
    return false;
  }
  Serial.printf("JPEG %dx%d\n", w, h);

  int dW = tft.width(), dH = tft.height();

  // Let the decoder shrink by 1/2, 1/4 or 1/8 (fast), but never below screen size.
  uint8_t d = 1;
  for (uint8_t s2 = 2; s2 <= 8; s2 *= 2)
    if (w / s2 >= dW && h / s2 >= dH) d = s2;
  TJpgDec.setJpgScale(d);

  float W = (float)((w + d - 1) / d);
  float H = (float)((h + d - 1) / d);
  cm.k    = fmaxf(dW / W, dH / H);          // scale needed to cover the screen
  cm.offX = (W * cm.k - dW) / 2.0f;         // center-crop
  cm.offY = (H * cm.k - dH) / 2.0f;
  cm.dW   = dW;
  cm.dH   = dH;
  cm.fast = fabsf(cm.k - 1.0f) < 0.001f;

  int err = (int)TJpgDec.drawSdJpg(0, 0, path.c_str());
  // 0 = OK, 1 = decode stopped early because the rest is off-screen (fine)
  if (err > 1) {
    Serial.printf("JPEG decode error %d in %s\n", err, path.c_str());
    return false;
  }
  return true;
}

// ---------- BMP (24-bit, uncompressed) ----------
uint32_t read32(File& f) { uint32_t v; f.read((uint8_t*)&v, 4); return v; }
uint16_t read16(File& f) { uint16_t v; f.read((uint8_t*)&v, 2); return v; }

// Same "cover" behaviour, using nearest-neighbour so pixel art stays crisp.
bool drawBmp(const String& path) {
  File f = SD.open(path);
  if (!f) return false;

  if (read16(f) != 0x4D42) { f.close(); return false; }  // "BM"
  f.seek(10); uint32_t dataOffset = read32(f);
  f.seek(18); int32_t  w = (int32_t)read32(f);
              int32_t  h = (int32_t)read32(f);
  read16(f);                    // planes
  uint16_t bpp  = read16(f);
  uint32_t comp = read32(f);

  if (bpp != 24 || comp != 0 || w <= 0 || h == 0) { f.close(); return false; }

  bool bottomUp = h > 0;
  if (h < 0) h = -h;
  uint32_t rowSize = ((w * 3) + 3) & ~3;

  int   dW = tft.width(), dH = tft.height();
  float k    = fmaxf((float)dW / w, (float)dH / h);   // scale to cover the screen
  float offX = (w * k - dW) / 2.0f;                   // center-crop offsets
  float offY = (h * k - dH) / 2.0f;

  // Only the visible slice of each source row needs to be read.
  int sxMin = constrain((int)(offX / k), 0, (int)w - 1);
  int sxMax = constrain((int)((dW - 1 + offX) / k), 0, (int)w - 1);
  int segW  = sxMax - sxMin + 1;

  uint8_t* seg = (uint8_t*)malloc(segW * 3);
  if (!seg) { f.close(); return false; }

  static int16_t  xmap[320];
  static uint16_t pxBuf[320];
  for (int dx = 0; dx < dW; dx++)
    xmap[dx] = constrain((int)((dx + offX) / k), sxMin, sxMax) - sxMin;

  tft.setSwapBytes(false);       // color565 already gives the right byte order
  int lastSrcRow = -1;
  for (int dy = 0; dy < dH; dy++) {
    int srcRow = constrain((int)((dy + offY) / k), 0, (int)h - 1);
    if (srcRow != lastSrcRow) {  // upscaled rows repeat; only re-read when it changes
      int fileRow = bottomUp ? ((int)h - 1 - srcRow) : srcRow;
      f.seek(dataOffset + (uint32_t)fileRow * rowSize + (uint32_t)sxMin * 3);
      f.read(seg, segW * 3);
      lastSrcRow = srcRow;
    }
    for (int dx = 0; dx < dW; dx++) {
      const uint8_t* px = seg + xmap[dx] * 3;
      pxBuf[dx] = tft.color565(px[2], px[1], px[0]);   // BMP is BGR
    }
    tft.pushImage(0, dy, dW, 1, pxBuf);
  }
  tft.setSwapBytes(true);
  free(seg);
  f.close();
  return true;
}

// ---------- File handling ----------
bool hasExt(String n, const char* ext) {
  n.toLowerCase();
  return n.endsWith(ext);
}

void scanImages() {
  files.clear();
  File dir = SD.open(IMAGE_DIR);
  if (!dir || !dir.isDirectory()) return;

  while (File e = dir.openNextFile()) {
    if (!e.isDirectory()) {
      String name = e.name();
      // skip hidden/macOS resource files like ._photo.jpg
      if (!name.startsWith(".") &&
          (hasExt(name, ".jpg") || hasExt(name, ".jpeg") || hasExt(name, ".bmp"))) {
        String full = String(IMAGE_DIR);
        if (!full.endsWith("/")) full += "/";
        files.push_back(full + name);
      }
    }
    e.close();
  }
  dir.close();

  if (SHUFFLE) {
    for (int i = files.size() - 1; i > 0; i--) {
      int j = random(i + 1);
      std::swap(files[i], files[j]);
    }
  }
}

void message(const char* line1, const char* line2 = "") {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line1, tft.width() / 2, tft.height() / 2 - 10, 2);
  tft.drawString(line2, tft.width() / 2, tft.height() / 2 + 14, 2);
}

void showImage(int dir) {
  if (files.empty()) return;
  int n = (int)files.size();

  for (int tries = 0; tries < n; tries++) {
    currentIndex = (currentIndex + dir + n) % n;
    const String& p = files[currentIndex];

    fadeTo(brightness, 0);
    bool ok = hasExt(p, ".bmp") ? drawBmp(p) : drawJpg(p);
    if (ok) {
      Serial.println("Showing " + p);
      fadeTo(0, brightness);
      lastChange = millis();
      return;
    }
    Serial.println("Failed to draw " + p);
  }
  message("No readable images", "Use 24-bit BMP or baseline JPEG");
  fadeTo(0, brightness);
  lastChange = millis();
}

void showNext() { showImage(1); }
void showPrev() { showImage(-1); }

// Redraw the current image (without advancing) after a rotation change
void redrawCurrent() {
  if (files.empty() || currentIndex < 0) return;
  const String& p = files[currentIndex];

  fadeTo(brightness, 0);
  tft.setRotation(rotation);
  bool ok = hasExt(p, ".bmp") ? drawBmp(p) : drawJpg(p);
  if (!ok) Serial.println("Failed to redraw " + p);
  fadeTo(0, brightness);
  lastChange = millis();   // restart the slideshow timer
}

// ---------- Button ----------
// 0 = nothing, 1 = short press (fires on release), 2 = long press (fires while held)
int buttonEvent() {
  static bool down = false, longFired = false;
  static uint32_t downAt = 0;
  bool now = (digitalRead(PIN_BUTTON) == LOW);
  if (now && !down) {
    down = true; longFired = false; downAt = millis();
  } else if (now && down && !longFired && millis() - downAt >= LONG_PRESS_MS) {
    longFired = true;
    return 2;
  } else if (!now && down) {
    down = false;
    if (!longFired && millis() - downAt > 30) return 1;
  }
  return 0;
}

// Bit-banged XPT2046 read: cmd 0xD0 = X position, 0x90 = Y position (12-bit)
uint16_t xptRead(uint8_t cmd) {
  digitalWrite(T_CS, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(T_CLK, LOW);
    digitalWrite(T_MOSI, (cmd >> i) & 1);
    digitalWrite(T_CLK, HIGH);
  }
  delayMicroseconds(6);
  digitalWrite(T_CLK, LOW);       // busy clock
  delayMicroseconds(2);
  digitalWrite(T_CLK, HIGH);
  digitalWrite(T_CLK, LOW);
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    v <<= 1;
    digitalWrite(T_CLK, HIGH);
    digitalWrite(T_CLK, LOW);
    if (digitalRead(T_MISO)) v |= 1;
  }
  digitalWrite(T_CS, HIGH);
  return v >> 4;
}

// Convert raw touch values to 0..1 across the screen's CURRENT horizontal axis
float touchFractionX(int rawX, int rawY) {
  float f;
  switch (rotation) {
    case 1:  f = (rawX - 200) / 3500.0f;        break;  // landscape
    case 3:  f = 1.0f - (rawX - 200) / 3500.0f; break;  // landscape, flipped
    case 0:  f = 1.0f - (rawY - 240) / 3560.0f; break;  // portrait
    default: f = (rawY - 240) / 3560.0f;        break;  // portrait, flipped
  }
  if (TOUCH_INVERT_X) f = 1.0f - f;
  return constrain(f, 0.0f, 1.0f);
}

// Fires once per touch (on touch-down), with a short debounce.
// On a tap, fx is set to the horizontal position (0 = left edge, 1 = right edge).
bool tapped(float &fx) {
  static bool wasDown = false, fired = false;
  static uint32_t downSince = 0;
  bool down = (digitalRead(PIN_TOUCH_IRQ) == LOW);
  if (down && !wasDown) { downSince = millis(); fired = false; }
  wasDown = down;

  if (down && !fired && millis() - downSince > 40) {
    fired = true;
    uint32_t sx = 0, sy = 0;
    int n = 0;
    for (int i = 0; i < 6; i++) {
      uint16_t rx = xptRead(0xD0), ry = xptRead(0x90);
      if (i >= 1) { sx += rx; sy += ry; n++; }   // drop the first sample
    }
    if (digitalRead(PIN_TOUCH_IRQ) != LOW) return false;  // released mid-read
    int rawX = sx / n, rawY = sy / n;
    fx = touchFractionX(rawX, rawY);
    if (TOUCH_DEBUG) Serial.printf("Touch raw X=%d Y=%d -> fraction %.2f\n", rawX, rawY, fx);
    return true;
  }
  return false;
}

// ---------- WiFi upload mode ----------
// Hold BOOT for 1.5 s to start/stop. While active, the frame runs its own WiFi
// hotspot plus a small web page for adding/deleting images on the SD card.

String jsonEscape(const String& in) {
  String o;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}

// Keep only safe filename characters and strip any folder part
String safeName(String n) {
  int slash = max(n.lastIndexOf('/'), n.lastIndexOf('\\'));
  if (slash >= 0) n = n.substring(slash + 1);
  String out;
  for (size_t i = 0; i < n.length() && out.length() < 60; i++) {
    char c = n[i];
    out += (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') ? c : '_';
  }
  while (out.startsWith(".")) out.remove(0, 1);   // no hidden files
  return out;
}

bool allowedExt(const String& n) {
  return hasExt(n, ".jpg") || hasExt(n, ".jpeg") || hasExt(n, ".bmp");
}

String imagePath(const String& name) {
  String d = IMAGE_DIR;
  if (!d.endsWith("/")) d += "/";
  return d + name;
}

// Reads the EXIF orientation (1..8) from the data of a JPEG APP1 segment.
// Returns 0 if the segment has no EXIF orientation. 1 = upright, 2..8 = needs rotating/flipping.
uint8_t exifOrientation(const uint8_t* d, size_t n) {
  if (n < 14 || memcmp(d, "Exif\0\0", 6) != 0) return 0;
  const uint8_t* t = d + 6;                    // start of the TIFF header
  size_t tn = n - 6;
  bool le;
  if (t[0] == 'I' && t[1] == 'I')      le = true;
  else if (t[0] == 'M' && t[1] == 'M') le = false;
  else return 0;

  auto rd16 = [&](size_t o) -> uint16_t {
    return le ? (uint16_t)(t[o] | (t[o + 1] << 8)) : (uint16_t)((t[o] << 8) | t[o + 1]);
  };
  auto rd32 = [&](size_t o) -> uint32_t {
    return le ? ((uint32_t)rd16(o) | ((uint32_t)rd16(o + 2) << 16))
              : (((uint32_t)rd16(o) << 16) | rd16(o + 2));
  };

  uint32_t ifd = rd32(4);                      // offset of the first directory
  if (ifd > tn || tn - ifd < 2) return 0;
  uint16_t count = rd16(ifd);
  for (uint16_t i = 0; i < count; i++) {
    size_t e = (size_t)ifd + 2 + (size_t)i * 12;
    if (e + 12 > tn) break;
    if (rd16(e) == 0x0112) {                   // Orientation tag
      uint16_t v = rd16(e + 8);
      return (v >= 1 && v <= 8) ? (uint8_t)v : 0;
    }
  }
  return 0;
}

// Returns "" if the file is a supported, screen-friendly image, otherwise the reason it isn't.
// Checks type, size and dimensions by reading only the file header.
String validateImage(const String& path, bool isBmp) {
  File f = SD.open(path);
  if (!f) return "cannot read the saved file";
  if (f.size() > MAX_UPLOAD_BYTES) { f.close(); return "file too large"; }

  String why = "";
  if (isBmp) {
    if (read16(f) != 0x4D42) {
      why = "not a valid BMP file";
    } else {
      f.seek(18);
      int32_t w = (int32_t)read32(f);
      int32_t h = (int32_t)read32(f);
      read16(f);                               // planes
      uint16_t bpp  = read16(f);
      uint32_t comp = read32(f);
      int32_t ah = (h < 0) ? -h : h;
      if (bpp != 24 || comp != 0)                          why = "BMP must be 24-bit and uncompressed";
      else if (w <= 0 || ah <= 0)                          why = "not a valid BMP file";
      else if (w > MAX_IMAGE_DIM || ah > MAX_IMAGE_DIM)    why = "image is too large (max " + String(MAX_IMAGE_DIM) + " px)";
    }
  } else {
    // JPEG: walk the markers until the frame header (SOFn)
    why = "not a valid JPEG file";
    uint8_t b[2];
    if (f.read(b, 2) == 2 && b[0] == 0xFF && b[1] == 0xD8) {
      while (f.read(b, 2) == 2 && b[0] == 0xFF) {
        uint8_t m = b[1];
        while (m == 0xFF && f.read(&m, 1) == 1) {}                    // skip fill bytes
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;  // markers with no length
        if (m == 0xD9 || m == 0xDA) break;                            // end / start of scan
        if (f.read(b, 2) != 2) break;
        uint16_t len = ((uint16_t)b[0] << 8) | b[1];
        if (len < 2) break;
        size_t segStart = f.position();           // start of this segment's data

        if (m == 0xE1 && len >= 16) {             // APP1: may hold EXIF
          uint8_t buf[512];
          size_t want = min((size_t)(len - 2), sizeof(buf));
          size_t got = f.read(buf, want);
          uint8_t o = exifOrientation(buf, got);
          if (o > 1) {
            why = "JPEG has an EXIF rotation tag (orientation " + String(o) +
                  "), so it would show sideways. Upload it in Photos mode instead";
            break;
          }
        }
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
          if (m != 0xC0 && m != 0xC1) { why = "progressive JPEGs are not supported"; break; }
          uint8_t sof[5];
          if (f.read(sof, 5) != 5) break;
          uint16_t h = ((uint16_t)sof[1] << 8) | sof[2];
          uint16_t w = ((uint16_t)sof[3] << 8) | sof[4];
          if (w == 0 || h == 0)                                   why = "not a valid JPEG file";
          else if (w > MAX_IMAGE_DIM || h > MAX_IMAGE_DIM)        why = "image is too large (max " + String(MAX_IMAGE_DIM) + " px)";
          else                                                    why = "";
          break;
        }
        if (!f.seek(segStart + len - 2)) break;
      }
    }
  }
  f.close();
  return why;
}

void handleRoot() {
  lastUploadActivity = millis();
  server.send_P(200, "text/html", UPLOAD_HTML);
}

void handleList() {
  lastUploadActivity = millis();
  String json = "{\"files\":[";
  bool first = true;
  File dir = SD.open(IMAGE_DIR);
  if (dir && dir.isDirectory()) {
    while (File e = dir.openNextFile()) {
      String name = e.name();
      if (!e.isDirectory() && !name.startsWith(".") && allowedExt(name)) {
        if (!first) json += ",";
        first = false;
        json += "{\"n\":\"" + jsonEscape(name) + "\",\"s\":" + String((unsigned long)e.size()) + "}";
      }
      e.close();
    }
    dir.close();
  }
  json += "],\"card\":" + String((unsigned long)(SD.cardSize() / (1024ULL * 1024ULL))) +
          ",\"maxBytes\":" + String((unsigned long)MAX_UPLOAD_BYTES) +
          ",\"maxDim\":" + String((unsigned long)MAX_IMAGE_DIM) + "}";
  server.send(200, "application/json", json);
}

void handleDelete() {
  lastUploadActivity = millis();
  String name = safeName(server.arg("name"));
  String path = imagePath(name);
  if (name.length() && allowedExt(name) && SD.exists(path) && SD.remove(path)) {
    Serial.println("Deleted " + path);
    server.send(200, "text/plain", "deleted");
  } else {
    server.send(404, "text/plain", "not found");
  }
}

// Called repeatedly while a file is being received. Data goes to a ".part" file
// first and is renamed only after it passes validation, so a dropped connection or
// an unsuitable image never ends up in the slideshow.
void handleUploadData() {
  HTTPUpload& up = server.upload();
  lastUploadActivity = millis();

  if (up.status == UPLOAD_FILE_START) {
    uploadOk = false;
    uploadFinalPath = "";
    uploadTmpPath = "";
    uploadBytes = 0;
    uploadReason = "only .jpg and .bmp images are accepted";
    String name = safeName(up.filename);
    if (!name.length() || !allowedExt(name)) {
      Serial.println("Rejected upload: " + up.filename);
      return;
    }
    uploadFinalPath = imagePath(name);
    uploadTmpPath = uploadFinalPath + ".part";
    SD.remove(uploadTmpPath);
    uploadFile = SD.open(uploadTmpPath, FILE_WRITE);
    uploadOk = (bool)uploadFile;
    if (!uploadOk) uploadReason = "cannot write to the SD card";
    Serial.println("Receiving " + uploadFinalPath);

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploadOk || !uploadFile) return;
    uploadBytes += up.currentSize;
    if (uploadBytes > MAX_UPLOAD_BYTES) {            // too big: stop saving it
      uploadOk = false;
      uploadReason = "file too large (max " + String(MAX_UPLOAD_BYTES / 1024) + " KB)";
      uploadFile.close();
      SD.remove(uploadTmpPath);
    } else if (uploadFile.write(up.buf, up.currentSize) != up.currentSize) {
      uploadOk = false;
      uploadReason = "SD card full or write error";
      uploadFile.close();
      SD.remove(uploadTmpPath);
    }

  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    if (uploadOk) {
      String why = validateImage(uploadTmpPath, hasExt(uploadFinalPath, ".bmp"));
      if (why.length()) { uploadOk = false; uploadReason = why; }
    }
    if (uploadOk) {
      SD.remove(uploadFinalPath);
      uploadOk = SD.rename(uploadTmpPath, uploadFinalPath);
      if (!uploadOk) uploadReason = "could not save the file";
    }
    if (uploadOk) {
      uploadedCount++;
      Serial.printf("Saved %s (%u bytes)\n", uploadFinalPath.c_str(), (unsigned)uploadBytes);
    } else {
      if (uploadTmpPath.length()) SD.remove(uploadTmpPath);
      Serial.println("Upload failed: " + uploadReason);
    }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    if (uploadTmpPath.length()) SD.remove(uploadTmpPath);
    uploadOk = false;
    uploadReason = "upload interrupted";
  }
}

void handleUploadDone() {
  lastUploadActivity = millis();
  if (uploadOk) server.send(200, "text/plain", "ok");
  else          server.send(400, "text/plain", uploadReason);
}

void setupUploadRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/list", HTTP_GET, handleList);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
}

void drawUploadScreen() {
  tft.setRotation(rotation);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  int cx = tft.width() / 2, y = 8;

  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("UPLOAD MODE", cx, y, 2);
  y += 38;

  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("1. Join this WiFi network:", cx, y, 2);  y += 18;
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(apSsid, cx, y, 2);                        y += 20;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Password:", cx, y, 2);                   y += 18;

  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(apPass, cx, y, 2);                        y += 38;

  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("2. Open in a browser:", cx, y, 2);       y += 18;
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("192.168.4.1", cx, y, 2);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("WiFi turns off if nobody joins in 5 min", cx, tft.height() - 38, 1);
  tft.drawString("Hold BOOT to finish", cx, tft.height() - 22, 2);
  tft.setTextDatum(TL_DATUM);
}

void startUploadMode() {
  Serial.println("Upload mode ON");
  uploadMode = true;
  uploadedCount = 0;
  if (!SD.exists(IMAGE_DIR)) SD.mkdir(IMAGE_DIR);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  WiFi.setTxPower(WIFI_POWER_15dBm);          // gentler on weak USB power supplies
  delay(100);
  MDNS.begin("photoframe");                   // http://photoframe.local on most devices
  server.begin();
  lastUploadActivity = millis();
  lastClientSeen = millis();
  drawUploadScreen();
  Serial.printf("WiFi: %s  password: %s  http://%s\n",
                apSsid.c_str(), apPass.c_str(), WiFi.softAPIP().toString().c_str());
}

void stopUploadMode() {
  Serial.printf("Upload mode OFF (%d files received)\n", uploadedCount);
  server.stop();
  MDNS.end();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  uploadMode = false;

  scanImages();                               // pick up anything that was added or deleted
  currentIndex = -1;
  if (files.empty()) {
    message("No images found", "Hold BOOT to upload some");
    return;
  }
  showNext();
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  Serial.printf("\nBoot. Reset reason: %d (1=power-on, 3=software, 4=panic, 15=brownout)\n",
                (int)esp_reset_reason());
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LDR, INPUT);
  pinMode(PIN_TOUCH_IRQ, INPUT);   // GPIO36 is input-only; board has an external pull-up
  pinMode(T_CS, OUTPUT);   digitalWrite(T_CS, HIGH);
  pinMode(T_CLK, OUTPUT);  digitalWrite(T_CLK, LOW);
  pinMode(T_MOSI, OUTPUT);
  pinMode(T_MISO, INPUT);
  randomSeed(esp_random());

  prefs.begin("frame", false);
  rotation = prefs.getUChar("rot", 1) & 3;   // restore saved orientation

  // Per-device hotspot name, and a random 8-digit password made once and remembered
  char ssidBuf[24];
  snprintf(ssidBuf, sizeof(ssidBuf), "PhotoFrame-%04X", (unsigned)((ESP.getEfuseMac() >> 32) & 0xFFFF));
  apSsid = ssidBuf;
  apPass = prefs.getString("appass", "");
  if (apPass.length() != 8) {
    char pw[9];
    snprintf(pw, sizeof(pw), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    apPass = pw;
    prefs.putString("appass", apPass);
  }

  backlightInit();

  tft.init();
  tft.setRotation(rotation);     // starts landscape; tap the screen to rotate
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);

  TJpgDec.setCallback(tftOutput);
  TJpgDec.setSwapBytes(false);   // TJpgDec handles byte order; keep tft setting separate

  setBacklight(MAX_BRIGHTNESS);

  // SD card is on its own SPI bus on the CYD
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 20000000)) {
    message("SD card not found", "Check card (FAT32)");
    return;
  }

  sdOk = true;
  setupUploadRoutes();
  scanImages();
  if (files.empty()) {
    message("No images found", "Hold BOOT to upload some");
    return;
  }
  Serial.printf("Found %d images\n", (int)files.size());
  brightness = MAX_BRIGHTNESS;
  showNext();
}

void loop() {
  int btn = buttonEvent();

  // Hold BOOT to start/stop WiFi upload mode
  if (btn == 2 && sdOk) {
    if (uploadMode) stopUploadMode(); else startUploadMode();
    return;
  }

  if (uploadMode) {
    server.handleClient();

    // Check once a second whether any device is joined to the hotspot
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 1000) {
      lastCheck = millis();
      if (WiFi.softAPgetStationNum() > 0) lastClientSeen = millis();
    }

    if (millis() - lastClientSeen > NO_CLIENT_TIMEOUT) {
      Serial.println("No device connected for 5 minutes: turning WiFi off");
      stopUploadMode();
    } else if (millis() - lastUploadActivity > UPLOAD_TIMEOUT) {
      Serial.println("No activity for 15 minutes: turning WiFi off");
      stopUploadMode();
    }
    delay(2);
    return;
  }

  updateAutoBrightness();

  float fx = 0.5f;
  if (tapped(fx)) {
    if (fx < ZONE_EDGE) {
      Serial.println("Tap: previous");
      showPrev();
    } else if (fx > 1.0f - ZONE_EDGE) {
      Serial.println("Tap: next");
      showNext();
    } else {
      rotation = (rotation + 1) % 4;   // use (rotation + 3) % 4 to rotate the other way
      prefs.putUChar("rot", rotation); // remember across reboots
      Serial.printf("Tap: rotation %d\n", rotation);
      redrawCurrent();
    }
  }

  if (!files.empty() && (millis() - lastChange > SLIDE_INTERVAL || btn == 1)) {
    showNext();
  }
}
