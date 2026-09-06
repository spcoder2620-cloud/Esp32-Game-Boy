// ============================================================
// Game Boy Emulator - Milestone 3: ROM browser + in-game exit
// ============================================================
// Requires peanut_gb.h (from github.com/deltabeard/Peanut-GB)
// in the same folder as this .ino.
// ============================================================

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <SPI.h>
#include <SD.h>

#define ENABLE_SOUND 0
#include "peanut_gb.h"
#include "boot_logo.h"

// ---------- Screen pins ----------
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST  14
#define TFT_SCLK 12
#define TFT_MOSI 11

// ---------- SD card pins ----------
#define SD_CS     8
#define SD_MISO  13

// ---------- Buttons ----------
#define BTN_UP     4
#define BTN_DOWN   5
#define BTN_LEFT   6
#define BTN_RIGHT  7
#define BTN_A     15
#define BTN_B     16
#define BTN_START 17
#define BTN_SELECT 18

Adafruit_SSD1351 tft(128, 128, &SPI, TFT_CS, TFT_DC, TFT_RST);

// ---------- App state ----------
enum AppState { STATE_MENU, STATE_PLAYING };
AppState appState = STATE_MENU;

// ---------- ROM browser state ----------
#define MAX_ROMS 40
#define VISIBLE_ROWS 9
String romList[MAX_ROMS];
String romTitles[MAX_ROMS];
int romCount = 0;
int selectedIndex = 0;
int scrollOffset = 0;

// ---------- Game Boy core state ----------
struct gb_s gb;
uint8_t *rom_data = nullptr;
uint32_t rom_size = 0;

#define CART_RAM_SIZE (32 * 1024)
uint8_t cart_ram[CART_RAM_SIZE];

#define GB_W 160
#define GB_H 144
uint8_t gb_frame[GB_H][GB_W];

const uint16_t palette[4] = {
  0xFFFF,
  0xAD55,
  0x52AA,
  0x0000
};

#define OUT_W 128
#define OUT_H 115
uint16_t out_buf[OUT_H][OUT_W];

// Precomputed nearest-neighbour lookup tables - avoids a multiply+divide
// per pixel, per frame (14,720 pixels/frame otherwise doing that math live)
int rowMap[OUT_H];
int colMap[OUT_W];

void buildScaleTables() {
  for (int oy = 0; oy < OUT_H; oy++) rowMap[oy] = (oy * GB_H) / OUT_H;
  for (int ox = 0; ox < OUT_W; ox++) colMap[ox] = (ox * GB_W) / OUT_W;
}

// FPS instrumentation
uint32_t frameCount = 0;
uint32_t fpsWindowStart = 0;

// ---------- Save game state ----------
String currentSavePath = "";
uint32_t currentSaveSize = 0;
uint32_t lastAutosaveTime = 0;
#define AUTOSAVE_INTERVAL_MS 30000

// ---------- Debounce ----------
uint32_t lastPressTime[40] = {0};

bool wasPressed(int pin) {
  if (digitalRead(pin) == LOW && millis() - lastPressTime[pin] > 200) {
    lastPressTime[pin] = millis();
    return true;
  }
  return false;
}

// ------------------------------------------------------------
// Peanut-GB callbacks
// ------------------------------------------------------------

uint8_t gb_rom_read(struct gb_s *gb_ptr, const uint_fast32_t addr) {
  return rom_data[addr];
}

uint8_t gb_cart_ram_read(struct gb_s *gb_ptr, const uint_fast32_t addr) {
  return cart_ram[addr % CART_RAM_SIZE];
}

void gb_cart_ram_write(struct gb_s *gb_ptr, const uint_fast32_t addr, const uint8_t val) {
  cart_ram[addr % CART_RAM_SIZE] = val;
}

void gb_error(struct gb_s *gb_ptr, const enum gb_error_e gb_err, const uint16_t val) {
  Serial.print("Peanut-GB error: ");
  Serial.println(gb_err);
}

void lcd_draw_line(struct gb_s *gb_ptr, const uint8_t *pixels, const uint_fast8_t line) {
  if (line >= GB_H) return;
  for (int x = 0; x < GB_W; x++) {
    gb_frame[line][x] = pixels[x] & 0x03;
  }
}

// ------------------------------------------------------------
// Boot logo animation - mimics the classic scroll-down-then-
// settle effect of the original Game Boy boot screen
// ------------------------------------------------------------

void playBootLogo() {
  tft.fillScreen(0x0000);

  int targetY = (128 - LOGO_H) / 2;
  int startY = -LOGO_H;
  int x = (128 - LOGO_W) / 2;

  // Scroll the logo down from off-screen to its resting position
  for (int y = startY; y <= targetY; y += 3) {
    tft.fillScreen(0x0000);
    tft.drawRGBBitmap(x, y, logo_bitmap, LOGO_W, LOGO_H);
    delay(20);
  }

  // Hold on screen briefly, like the real boot sequence pause
  delay(900);

  tft.fillScreen(0x0000);
}

// ------------------------------------------------------------
// ROM browser
// ------------------------------------------------------------

// Reads the game's real title straight from the ROM header (offset 0x134,
// 16 bytes) without needing to load the whole ROM into memory
String readRomTitle(const char *path) {
  File f = SD.open(path);
  if (!f) return String("");
  if (f.size() < 0x144) {
    f.close();
    return String("");
  }

  f.seek(0x134);
  char titleBuf[17];
  int n = f.read((uint8_t *)titleBuf, 16);
  f.close();

  if (n <= 0) return String("");

  String title = "";
  for (int i = 0; i < n; i++) {
    char c = titleBuf[i];
    if (c == 0) break;
    if (c < 32 || c > 126) break; // stop at non-printable bytes (e.g. CGB flag)
    title += c;
  }
  title.trim();
  return title;
}

void loadRomList() {
  romCount = 0;
  File root = SD.open("/");
  if (!root) {
    Serial.println("Failed to open root directory");
    return;
  }

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;

    String name = entry.name();
    if (!entry.isDirectory() && name.endsWith(".gb")) {
      if (romCount < MAX_ROMS) {
        String path = "/" + name;
        romList[romCount] = name;
        romTitles[romCount] = readRomTitle(path.c_str());
        romCount++;
      }
    }
    entry.close();
  }
  root.close();

  Serial.print("Found ");
  Serial.print(romCount);
  Serial.println(" ROM(s)");
}

void drawMenu() {
  // Keep the selected item scrolled into view
  if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
  if (selectedIndex >= scrollOffset + VISIBLE_ROWS) {
    scrollOffset = selectedIndex - VISIBLE_ROWS + 1;
  }

  tft.fillScreen(0x0000);
  tft.setCursor(0, 0);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(1);
  tft.println("Select ROM:");

  if (romCount == 0) {
    tft.setCursor(0, 20);
    tft.println("No .gb files");
    tft.println("found on SD");
    return;
  }

  int endIndex = min(romCount, scrollOffset + VISIBLE_ROWS);
  for (int i = scrollOffset; i < endIndex; i++) {
    int row = i - scrollOffset;
    tft.setCursor(0, 12 * (row + 1));
    if (i == selectedIndex) {
      tft.setTextColor(0x0000, 0xFFFF);
    } else {
      tft.setTextColor(0xFFFF, 0x0000);
    }

    String label = romTitles[i].length() > 0 ? romTitles[i] : romList[i];
    if (label.length() > 20) label = label.substring(0, 20);
    tft.println(label);
  }
}

// ------------------------------------------------------------
// ROM loading + emulator start/stop
// ------------------------------------------------------------

// ------------------------------------------------------------
// Save game persistence - one .sav file per ROM, holding cart RAM
// ------------------------------------------------------------

String getSavePath(const String &filename) {
  String base = filename;
  int dot = base.lastIndexOf('.');
  if (dot >= 0) base = base.substring(0, dot);
  return "/" + base + ".sav";
}

void loadSaveFile(const String &path, uint32_t size) {
  memset(cart_ram, 0, CART_RAM_SIZE);
  if (size == 0) return;

  File f = SD.open(path.c_str());
  if (!f) {
    Serial.println("No existing save file - starting fresh");
    return;
  }

  uint32_t toRead = (uint32_t)f.size();
  if (toRead > size) toRead = size;
  f.read(cart_ram, toRead);
  f.close();
  Serial.println("Save file loaded");
}

void writeSaveFile(const String &path, uint32_t size) {
  if (size == 0) return;

  SD.remove(path.c_str()); // ensure a clean overwrite, not an append
  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) {
    Serial.println("Failed to open save file for writing");
    return;
  }

  f.write(cart_ram, size);
  f.close();
  Serial.println("Save written");
}

bool loadRomFromSD(const char *path) {
  File f = SD.open(path);
  if (!f) {
    Serial.println("Failed to open ROM file");
    return false;
  }

  rom_size = f.size();
  Serial.print("ROM size: ");
  Serial.println(rom_size);

  // Try internal SRAM first - the CPU fetches from ROM on every single
  // instruction, so this is a hot path. Internal SRAM has no wait states;
  // PSRAM does, so keeping the ROM off PSRAM matters a lot for speed.
  // Most GB ROMs (32KB-1MB) fit fine; only fall back to PSRAM if internal
  // RAM genuinely doesn't have room.
  rom_data = (uint8_t *)malloc(rom_size);
  if (rom_data) {
    Serial.println("ROM placed in internal SRAM (fast path)");
  } else {
    rom_data = (uint8_t *)ps_malloc(rom_size);
    if (rom_data) {
      Serial.println("ROM placed in PSRAM (fallback - internal RAM full)");
    }
  }

  if (!rom_data) {
    Serial.println("Failed to allocate ROM buffer");
    f.close();
    return false;
  }

  f.read(rom_data, rom_size);
  f.close();
  return true;
}

void startGame(const String &filename) {
  String path = "/" + filename;

  tft.fillScreen(0x0000);
  tft.setCursor(0, 0);
  tft.setTextColor(0xFFFF);
  tft.println("Loading:");
  tft.println(filename);

  if (!loadRomFromSD(path.c_str())) {
    tft.println("ROM load failed");
    delay(2000);
    drawMenu();
    return;
  }

  memset(cart_ram, 0, CART_RAM_SIZE);

  enum gb_init_error_e ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                                      &gb_cart_ram_write, &gb_error, NULL);
  if (ret != GB_INIT_NO_ERROR) {
    Serial.print("gb_init failed: ");
    Serial.println(ret);
    tft.println("gb_init failed");
    delay(2000);
    free(rom_data);
    rom_data = nullptr;
    drawMenu();
    return;
  }

  gb_init_lcd(&gb, &lcd_draw_line);

  // Determine how much cart RAM this game actually uses, then load
  // its save file (if one exists) into that RAM
  uint32_t saveSize = (uint32_t)gb_get_save_size(&gb);
  if (saveSize > CART_RAM_SIZE) saveSize = CART_RAM_SIZE; // safety cap

  currentSaveSize = saveSize;
  currentSavePath = getSavePath(filename);
  loadSaveFile(currentSavePath, currentSaveSize);
  lastAutosaveTime = millis();

  tft.fillScreen(0x0000);
  appState = STATE_PLAYING;
}

void exitToMenu() {
  if (currentSaveSize > 0 && currentSavePath.length() > 0) {
    writeSaveFile(currentSavePath, currentSaveSize);
  }
  currentSaveSize = 0;
  currentSavePath = "";

  if (rom_data) {
    free(rom_data);
    rom_data = nullptr;
  }
  appState = STATE_MENU;
  drawMenu();
}

// ------------------------------------------------------------
// Emulator frame handling
// ------------------------------------------------------------

void updateJoypad() {
  uint8_t joypad = 0xFF;

  if (digitalRead(BTN_UP)     == LOW) joypad &= ~(1 << 6);
  if (digitalRead(BTN_DOWN)   == LOW) joypad &= ~(1 << 7);
  if (digitalRead(BTN_LEFT)   == LOW) joypad &= ~(1 << 5);
  if (digitalRead(BTN_RIGHT)  == LOW) joypad &= ~(1 << 4);
  if (digitalRead(BTN_A)      == LOW) joypad &= ~(1 << 0);
  if (digitalRead(BTN_B)      == LOW) joypad &= ~(1 << 1);
  if (digitalRead(BTN_START)  == LOW) joypad &= ~(1 << 3);
  if (digitalRead(BTN_SELECT) == LOW) joypad &= ~(1 << 2);

  gb.direct.joypad = joypad;
}

void blitFrameToOLED() {
  for (int oy = 0; oy < OUT_H; oy++) {
    int gy = rowMap[oy];
    uint8_t *srcRow = gb_frame[gy];
    uint16_t *dstRow = out_buf[oy];
    for (int ox = 0; ox < OUT_W; ox++) {
      dstRow[ox] = palette[srcRow[colMap[ox]]];
    }
  }

  int yOffset = (128 - OUT_H) / 2;
  tft.drawRGBBitmap(0, yOffset, (uint16_t *)out_buf, OUT_W, OUT_H);
}

// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("GB Emulator - Milestone 3 (optimized)");

  setCpuFrequencyMhz(240); // ensure we're at the S3's max clock, not a lower default

  buildScaleTables();
  fpsWindowStart = millis();

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);
  pinMode(BTN_START, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  SPI.begin(TFT_SCLK, SD_MISO, TFT_MOSI, TFT_CS);

  tft.begin();
  tft.fillScreen(0x0000);
  tft.setTextColor(0xFFFF);
  tft.setCursor(0, 0);
  tft.println("Booting...");

  if (!SD.begin(SD_CS)) {
    tft.println("SD init failed");
    Serial.println("SD init failed");
    while (1) delay(1000);
  }

  playBootLogo();

  loadRomList();
  drawMenu();
}

void loop() {
  if (appState == STATE_MENU) {
    if (romCount == 0) return;

    if (wasPressed(BTN_UP)) {
      selectedIndex = (selectedIndex - 1 + romCount) % romCount;
      drawMenu();
    }
    if (wasPressed(BTN_DOWN)) {
      selectedIndex = (selectedIndex + 1) % romCount;
      drawMenu();
    }
    if (wasPressed(BTN_A)) {
      startGame(romList[selectedIndex]);
    }
  } else { // STATE_PLAYING
    // Start+Select together = exit to menu
    if (digitalRead(BTN_START) == LOW && digitalRead(BTN_SELECT) == LOW) {
      exitToMenu();
      return;
    }

    updateJoypad();
    gb_run_frame(&gb);
    blitFrameToOLED();
    yield(); // let the watchdog/background tasks breathe between frames

    // Autosave periodically so a crash/power loss doesn't lose progress
    if (currentSaveSize > 0) {
      uint32_t now2 = millis();
      if (now2 - lastAutosaveTime > AUTOSAVE_INTERVAL_MS) {
        writeSaveFile(currentSavePath, currentSaveSize);
        lastAutosaveTime = now2;
      }
    }

    // FPS counter - prints actual achieved frame rate every ~2 seconds
    frameCount++;
    uint32_t now = millis();
    if (now - fpsWindowStart >= 2000) {
      float fps = frameCount / ((now - fpsWindowStart) / 1000.0f);
      Serial.print("FPS: ");
      Serial.println(fps);
      frameCount = 0;
      fpsWindowStart = now;
    }
  }
}
