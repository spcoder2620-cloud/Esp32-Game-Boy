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
#define MAX_ROMS 20
String romList[MAX_ROMS];
int romCount = 0;
int selectedIndex = 0;

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
// ROM browser
// ------------------------------------------------------------

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
        romList[romCount++] = name;
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

  for (int i = 0; i < romCount; i++) {
    tft.setCursor(0, 12 * (i + 1));
    if (i == selectedIndex) {
      tft.setTextColor(0x0000, 0xFFFF);
    } else {
      tft.setTextColor(0xFFFF, 0x0000);
    }
    tft.println(romList[i]);
  }
}

// ------------------------------------------------------------
// ROM loading + emulator start/stop
// ------------------------------------------------------------

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

  tft.fillScreen(0x0000);
  appState = STATE_PLAYING;
}

void exitToMenu() {
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
