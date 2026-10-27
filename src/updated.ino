/* removed the nobe module with the normal tactile buttons
 * ESP32 Bluetooth Speaker - 1.3" OLED (128x64) with 4 Tactile Buttons (No Encoder)
 *
 * Libraries Required (Install via Arduino Library Manager):
 *   - U8g2                      (olikraus)
 *   - ESP32-A2DP                (pschatzmann)
 *   - arduino-audio-tools       (pschatzmann)
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

// ---------------- OLED selection ----------------
#define OLED_IS_SH1106 1
constexpr uint8_t OLED_ADDR_PRIMARY = 0x3C;
constexpr uint8_t OLED_ADDR_FALLBACK = 0x3D;

#if OLED_IS_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
#endif

// ---------------- Pins & Tactile Buttons (Using Internal Pull-Ups) ----------------
constexpr int I2S_BCK = 26, I2S_LRCK = 25, I2S_DATA = 27;
constexpr int OLED_SDA = 21, OLED_SCL = 22;
constexpr int BATTERY_ADC_PIN = 34;

// 4 Tactile Buttons connected to GPIO and GND (Internal Pull-Up enabled)
constexpr int BTN_PLAY_PAUSE = 14;
constexpr int BTN_NEXT       = 12;
constexpr int BTN_PREV       = 13;
constexpr int BTN_PAGE       = 4;

// ---------------- Tunables ----------------
constexpr float BATTERY_DIVIDER_RATIO = 2.0f;
constexpr float BATTERY_CALIBRATION = 1.0f;
constexpr uint32_t BATTERY_SAMPLE_MS = 100;
constexpr uint8_t BATTERY_SAMPLES = 8;
constexpr int VOLUME_STEP = 4;
constexpr uint32_t FRAME_MS_ACTIVE = 40;
constexpr uint32_t FRAME_MS_IDLE = 100;
constexpr uint32_t VOLUME_POPUP_MS = 1300;
constexpr uint32_t DIM_AFTER_MS = 30000;
constexpr uint32_t OFF_AFTER_MS = 120000;
constexpr uint8_t DIM_CONTRAST_PERCENT = 8;
constexpr char BT_NAME[] = "ESP32_BT_Speaker";

// ---------------- Fonts ----------------
#define FONT_TITLE u8g2_font_7x13B_tf
#define FONT_BODY u8g2_font_6x12_tf
#define FONT_SMALL u8g2_font_5x8_tf

// ---------------- State ----------------
Preferences settings;
bool settingsOK = false;
bool prefsDirty = false;
uint32_t lastPrefChange = 0;

I2SStream audioOutput;
BluetoothA2DPSink bluetooth(audioOutput);
bool audioOK = false, oledOK = false;

int volumePercent = 40;
int brightnessPercent = 100;
uint32_t lastLocalVolumeChange = 0;
uint32_t volumePopupUntil = 0;

String notice;
uint32_t noticeUntil = 0;

// Pages: Player -> Lyrics -> Info
enum Page : uint8_t { PAGE_PLAYER = 0, PAGE_LYRICS = 1, PAGE_INFO = 2, PAGE_COUNT = 3 };
uint8_t page = PAGE_PLAYER;

// Metadata
portMUX_TYPE metadataMux = portMUX_INITIALIZER_UNLOCKED;
char pendingTitle[160] = {}, pendingArtist[128] = {}, pendingAlbum[128] = {};
uint32_t pendingDurationMs = 0;
bool metadataChanged = false;

String btTitle, btArtist, btAlbum;
String btLyrics = "Synchronized lyrics will appear here when streaming via supported apps.";
uint32_t durationMs = 0;
uint32_t elapsedMs = 0;

bool wasConnected = false, wasPlaying = false;

// Battery
int batteryPercent = -1;
uint32_t batteryAdcMv = 0, batteryMv = 0;
float filteredBatteryMv = 0;

// Display power
uint32_t lastActivity = 0;
bool displayDimmed = false, displayOff = false;

// Button Debounce Struct
struct DebouncedButton {
  int pin;
  bool stableState;
  bool lastRawState;
  uint32_t lastDebounceTime;
};

DebouncedButton btnPlayPause = {BTN_PLAY_PAUSE, HIGH, HIGH, 0};
DebouncedButton btnNext      = {BTN_NEXT, HIGH, HIGH, 0};
DebouncedButton btnPrev      = {BTN_PREV, HIGH, HIGH, 0};
DebouncedButton btnPage      = {BTN_PAGE, HIGH, HIGH, 0};

// Marquees
struct Marquee { String text; uint32_t start = 0; };
Marquee mqTitle, mqArtist, mqAlbum, mqInfoAlbum, mqLyrics;

// ---------------- Helpers ----------------
bool activeUntil(uint32_t deadline) {
  return deadline && int32_t(deadline - millis()) > 0;
}

void markActivity() {
  lastActivity = millis();
}

uint8_t contrastFromPercent(int pct) {
  return uint8_t(constrain(pct, 1, 100) * 255 / 100);
}

void applyContrast() {
  if (!oledOK) return;
  const int pct = displayDimmed ? min(brightnessPercent, int(DIM_CONTRAST_PERCENT)) : brightnessPercent;
  u8g2.setContrast(contrastFromPercent(pct));
}

void showNotice(const String &text, uint32_t ms = 1300) {
  notice = text;
  noticeUntil = millis() + ms;
  Serial.println(text);
}

void markPrefsDirty() {
  prefsDirty = true;
  lastPrefChange = millis();
}

String displayText(const String &input) {
  String result;
  result.reserve(input.length());
  for (size_t i = 0; i < input.length();) {
    uint32_t c = uint8_t(input[i++]);
    if (c >= 0xC2 && c <= 0xDF && i < input.length()) {
      c = ((c & 0x1F) << 6) | (uint8_t(input[i++]) & 0x3F);
    } else if (c >= 0xE0 && c <= 0xEF && i + 1 < input.length()) {
      const uint8_t a = input[i++], b = input[i++];
      c = ((c & 0x0F) << 12) | ((a & 0x3F) << 6) | (b & 0x3F);
      if (c == 0x2018 || c == 0x2019) c = '\'';
      else if (c == 0x201C || c == 0x201D) c = '"';
      else if (c == 0x2013 || c == 0x2014) c = '-';
      else if (c == 0x2026) c = '.';
    } else if (c >= 0xF0) {
      i = min(size_t(i + 3), size_t(input.length()));
      c = '?';
    }
    if ((c >= 32 && c <= 126) || (c >= 160 && c <= 255)) result += char(c);
    else result += '?';
  }
  return result;
}

void formatTime(uint32_t ms, char *out, size_t len) {
  const uint32_t total = ms / 1000;
  const uint32_t h = total / 3600, m = (total / 60) % 60, s = total % 60;
  if (h) snprintf(out, len, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
  else snprintf(out, len, "%lu:%02lu", (unsigned long)m, (unsigned long)s);
}

// ---------------- Battery ----------------
int estimateBatteryPercent(float millivolts) {
  const uint16_t voltage[] = {3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200};
  const uint8_t percent[] = {0, 5, 10, 20, 35, 50, 60, 70, 85, 95, 100};
  if (millivolts <= voltage[0]) return 0;
  for (size_t i = 1; i < sizeof(voltage) / sizeof(voltage[0]); ++i) {
    if (millivolts <= voltage[i]) {
      const float fraction = (millivolts - voltage[i - 1]) / float(voltage[i] - voltage[i - 1]);
      return int(percent[i - 1] + fraction * (percent[i] - percent[i - 1]) + 0.5f);
    }
  }
  return 100;
}

void updateBattery(bool immediate = false) {
  static uint32_t lastSample = 0, totalMv = 0;
  static uint8_t sampleCount = 0;
  const uint32_t now = millis();
  if (!immediate && now - lastSample < BATTERY_SAMPLE_MS) return;
  lastSample = now;
  totalMv += analogReadMilliVolts(BATTERY_ADC_PIN);
  ++sampleCount;
  if (!immediate && sampleCount < BATTERY_SAMPLES) return;
  batteryAdcMv = (totalMv + sampleCount / 2) / sampleCount;
  totalMv = 0;
  sampleCount = 0;
  const float measured = batteryAdcMv * BATTERY_DIVIDER_RATIO * BATTERY_CALIBRATION;
  if (measured < 2500 || measured > 4350) {
    filteredBatteryMv = 0;
    batteryMv = 0;
    batteryPercent = -1;
  } else {
    filteredBatteryMv = filteredBatteryMv == 0 ? measured : filteredBatteryMv + 0.15f * (measured - filteredBatteryMv);
    batteryMv = uint32_t(filteredBatteryMv + 0.5f);
    const int estimate = estimateBatteryPercent(filteredBatteryMv);
    if (batteryPercent < 0 || abs(estimate - batteryPercent) >= 2 || estimate == 0 || estimate == 100) batteryPercent = estimate;
  }
}

// ---------------- Drawing primitives ----------------
int textWidth(const String &s) { return u8g2.getStrWidth(s.c_str()); }

void drawCentered(int baselineY, const String &s) {
  u8g2.drawStr((128 - textWidth(s)) / 2, baselineY, s.c_str());
}

void drawMarquee(Marquee &m, const String &text, int x, int baselineY, int width, int top, int bottom) {
  if (m.text != text) {
    m.text = text;
    m.start = millis();
  }
  const int tw = textWidth(text);
  if (tw <= width) {
    u8g2.drawStr(x, baselineY, text.c_str());
    return;
  }
  const int gap = 28;
  const int cycle = tw + gap;
  const uint32_t pauseMs = 1500;
  const uint32_t scrollMs = uint32_t(cycle) * 1000UL / 32UL;
  const uint32_t phase = (millis() - m.start) % (pauseMs + scrollMs);
  const int offset = phase < pauseMs ? 0 : int((phase - pauseMs) * 32UL / 1000UL);
  u8g2.setClipWindow(x, top, x + width, bottom);
  u8g2.drawStr(x - offset, baselineY, text.c_str());
  u8g2.drawStr(x - offset + cycle, baselineY, text.c_str());
  u8g2.setMaxClipWindow();
}

void drawPlayIcon(int x, int y) { u8g2.drawTriangle(x, y, x, y + 8, x + 7, y + 4); }
void drawPauseIcon(int x, int y) {
  u8g2.drawBox(x, y, 3, 9);
  u8g2.drawBox(x + 5, y, 3, 9);
}

void drawBattery(int x, int y) {
  u8g2.drawFrame(x, y, 18, 9);
  u8g2.drawBox(x + 18, y + 2, 2, 5);
  if (batteryPercent < 0) {
    u8g2.setFont(FONT_SMALL);
    u8g2.drawStr(x + 6, y + 8, "?");
    return;
  }
  const int bars = constrain((batteryPercent + 12) / 25, 0, 4);
  for (int i = 0; i < bars; ++i) u8g2.drawBox(x + 2 + i * 4, y + 2, 3, 5);
  if (batteryPercent < 10 && ((millis() / 500) & 1)) u8g2.drawBox(x + 2, y + 2, 14, 5);
}

void drawBluetoothGlyph(int x, int y) {
  u8g2.drawLine(x, y + 3, x + 6, y + 8);
  u8g2.drawLine(x, y + 8, x + 6, y + 3);
  u8g2.drawLine(x + 3, y, x + 3, y + 11);
  u8g2.drawLine(x + 3, y, x + 6, y + 3);
  u8g2.drawLine(x + 3, y + 11, x + 6, y + 8);
}

void drawTopBar(bool connected, bool playing) {
  drawBluetoothGlyph(1, 0);
  if (connected) {
    u8g2.drawDisc(14, 5, 2);
  } else {
    const int r = (millis() / 160) % 5;
    u8g2.drawCircle(14, 5, r);
  }
  if (connected) {
    if (playing) drawPlayIcon(28, 1);
    else drawPauseIcon(28, 1);
  }
  u8g2.setFont(FONT_SMALL);
  char vol[12];
  snprintf(vol, sizeof(vol), "VOL %d", volumePercent);
  u8g2.drawStr(50, 8, vol);
  drawBattery(106, 0);
  u8g2.drawHLine(0, 11, 128);
}

void drawEqualizer(int x, int baseY, int maxH, bool animate) {
  const int bars = 8, w = 6, sp = 2;
  const float t = millis();
  for (int i = 0; i < bars; ++i) {
    int h = 1;
    if (animate) {
      h = int(maxH * 0.5f + (maxH * 0.35f) * sinf(t * 0.011f + i * 0.9f) + (maxH * 0.15f) * cosf(t * 0.017f - i * 0.5f));
      h = constrain(h, 1, maxH);
    }
    const int bx = x + i * (w + sp);
    u8g2.drawBox(bx, baseY - h, w, h);
    u8g2.setDrawColor(0);
    for (int y = baseY - 2; y > baseY - h; y -= 3) u8g2.drawHLine(bx, y, w);
    u8g2.setDrawColor(1);
  }
}

void drawProgress(int y) {
  u8g2.drawFrame(0, y, 128, 5);
  if (durationMs > 0) {
    const uint32_t clamped = min(elapsedMs, durationMs);
    const int fill = int(uint64_t(clamped) * 124 / durationMs);
    u8g2.drawBox(2, y + 2, fill, 1);
    u8g2.drawBox(2, y + 1, fill, 3);
  }
}

// ---------------- Pages ----------------
void drawPairing() {
  const int cx = 22, cy = 38;
  const int phase = (millis() / 60) % 24;
  for (int ring = 0; ring < 3; ++ring) {
    const int r = (phase + ring * 8) % 24;
    if (r > 2) u8g2.drawCircle(cx, cy, r / 2 + 2);
  }
  u8g2.drawDisc(cx, cy, 3);

  u8g2.setFont(FONT_TITLE);
  u8g2.drawStr(46, 28, "Ready to");
  u8g2.drawStr(46, 41, "pair");
  const int dots = (millis() / 400) % 4;
  for (int i = 0; i < dots; ++i) u8g2.drawBox(46 + 30 + i * 5, 39, 2, 2);
  u8g2.setFont(FONT_SMALL);
  u8g2.drawStr(46, 53, BT_NAME);
  u8g2.drawStr(46, 62, "Waiting for phone");
}

void drawPlayer(bool connected, bool playing) {
  drawTopBar(connected, playing);
  if (!audioOK) {
    u8g2.setFont(FONT_TITLE);
    drawCentered(34, "Audio Error");
    u8g2.setFont(FONT_BODY);
    drawCentered(50, "Check I2S wiring");
    return;
  }
  if (!connected) {
    drawPairing();
    return;
  }

  String title = btTitle.length() ? btTitle : String("Streaming Audio");
  String artist = btArtist.length() ? btArtist : String(playing ? "Now playing" : "Paused");

  u8g2.setFont(FONT_TITLE);
  drawMarquee(mqTitle, title, 2, 26, 124, 14, 28);
  u8g2.setFont(FONT_BODY);
  drawMarquee(mqArtist, artist, 2, 39, 124, 30, 41);

  drawProgress(44);

  u8g2.setFont(FONT_SMALL);
  char buf[16];
  formatTime(elapsedMs, buf, sizeof(buf));
  u8g2.drawStr(0, 63, buf);
  if (durationMs > 0) {
    formatTime(durationMs, buf, sizeof(buf));
    u8g2.drawStr(128 - u8g2.getStrWidth(buf), 63, buf);
  }
  drawEqualizer(32, 63, 11, playing);
}

void drawLyrics(bool connected, bool playing) {
  drawTopBar(connected, playing);
  u8g2.setFont(FONT_TITLE);
  u8g2.drawStr(0, 26, "Live Lyrics");
  u8g2.setFont(FONT_BODY);
  drawMarquee(mqLyrics, btLyrics, 0, 44, 128, 30, 56);
  u8g2.drawHLine(0, 52, 128);
}

void drawInfo(bool connected, bool playing) {
  drawTopBar(connected, playing);
  u8g2.setFont(FONT_BODY);

  String album = btAlbum.length() ? btAlbum : String("-");
  u8g2.drawStr(0, 23, "Alb:");
  drawMarquee(mqInfoAlbum, album, 26, 23, 102, 13, 25);

  char line[32];
  if (batteryPercent >= 0) snprintf(line, sizeof(line), "Bat: %d%%  %.2fV", batteryPercent, batteryMv / 1000.0f);
  else snprintf(line, sizeof(line), "Bat: no cell");
  u8g2.drawStr(0, 35, line);

  snprintf(line, sizeof(line), "Vol:%d%%  Bri:%d%%", volumePercent, brightnessPercent);
  u8g2.drawStr(0, 47, line);

  const uint32_t up = millis() / 1000;
  snprintf(line, sizeof(line), "Up: %02lu:%02lu:%02lu", (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
  u8g2.drawStr(0, 59, line);
}

void drawVolumePopup() {
  const int x = 8, y = 18, w = 112, h = 30;
  u8g2.setDrawColor(0);
  u8g2.drawRBox(x, y, w, h, 3);
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(x, y, w, h, 3);
  u8g2.setFont(FONT_BODY);
  char buf[20];
  snprintf(buf, sizeof(buf), "Volume  %d%%", volumePercent);
  drawCentered(y + 12, buf);
  u8g2.drawFrame(x + 6, y + 18, w - 12, 7);
  u8g2.drawBox(x + 8, y + 20, (w - 16) * volumePercent / 100, 3);
}

void drawNotice() {
  u8g2.setFont(FONT_TITLE);
  const int tw = textWidth(notice);
  const int w = min(tw + 20, 124);
  const int x = (128 - w) / 2, y = 20, h = 24;
  u8g2.setDrawColor(0);
  u8g2.drawRBox(x, y, w, h, 3);
  u8g2.setDrawColor(1);
  u8g2.drawRFrame(x, y, w, h, 3);
  drawCentered(y + 16, notice);
}

void renderFrame() {
  const bool connected = bluetooth.is_connected();
  const bool playing = connected && bluetooth.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED;
  u8g2.clearBuffer();
  if (page == PAGE_LYRICS) drawLyrics(connected, playing);
  else if (page == PAGE_INFO) drawInfo(connected, playing);
  else drawPlayer(connected, playing);
  if (activeUntil(noticeUntil)) drawNotice();
  else if (activeUntil(volumePopupUntil)) drawVolumePopup();
  u8g2.sendBuffer();
}

// ---------------- Setup helpers ----------------
void setupOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);
  uint8_t address = 0;
  for (uint8_t candidate : {OLED_ADDR_PRIMARY, OLED_ADDR_FALLBACK}) {
    Wire.beginTransmission(candidate);
    if (Wire.endTransmission() == 0) { address = candidate; break; }
  }
  if (!address) {
    Serial.println("OLED not found! Check SDA/SCL wiring and power.");
    return;
  }
  u8g2.setI2CAddress(address << 1);
  oledOK = u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setFontMode(1);
  u8g2.setFontPosBaseline();
  applyContrast();
}

void bootSplash() {
  if (!oledOK) return;
  u8g2.clearBuffer();
  u8g2.setFont(FONT_TITLE);
  drawCentered(28, "ESP32");
  drawCentered(43, "BT Speaker");
  u8g2.setFont(FONT_SMALL);
  drawCentered(58, "starting...");
  u8g2.sendBuffer();
}

void applyVolume(bool fromUser) {
  bluetooth.set_volume((volumePercent * 127 + 50) / 100);
  volumePopupUntil = millis() + VOLUME_POPUP_MS;
  if (fromUser) lastLocalVolumeChange = millis();
  markPrefsDirty();
}

// ---------------- Actions ----------------
void togglePlayback() {
  if (!bluetooth.is_connected()) {
    showNotice("Connect phone");
    return;
  }
  if (bluetooth.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED) {
    bluetooth.pause();
    showNotice("Pause");
  } else {
    bluetooth.play();
    showNotice("Play");
  }
}

void nextTrack() {
  if (!bluetooth.is_connected()) { showNotice("Connect phone"); return; }
  bluetooth.next();
  showNotice("Next >>");
}

void previousTrack() {
  if (!bluetooth.is_connected()) { showNotice("Connect phone"); return; }
  bluetooth.previous();
  showNotice("<< Prev");
}

void switchPage() {
  page = (page + 1) % PAGE_COUNT;
  const char* pageNames[] = {"Now Playing", "Live Lyrics", "Info"};
  showNotice(pageNames[page], 800);
}

// Button debouncing & event handling
bool updateButton(DebouncedButton &btn) {
  const uint32_t now = millis();
  const bool raw = digitalRead(btn.pin);
  if (raw != btn.lastRawState) {
    btn.lastRawState = raw;
    btn.lastDebounceTime = now;
  }
  if (raw != btn.stableState && (now - btn.lastDebounceTime) >= 25) {
    btn.stableState = raw;
    markActivity();
    // Trigger action on press (falling edge because of INPUT_PULLUP to GND)
    if (btn.stableState == LOW) {
      return true;
    }
  }
  return false;
}

void handleButtons() {
  if (updateButton(btnPlayPause)) {
    togglePlayback();
  }
  if (updateButton(btnNext)) {
    nextTrack();
  }
  if (updateButton(btnPrev)) {
    previousTrack();
  }
  if (updateButton(btnPage)) {
    switchPage();
  }
}

// ---------------- Bluetooth metadata ----------------
void onMetadata(uint8_t attribute, const uint8_t *value) {
  if (!value) return;
  portENTER_CRITICAL(&metadataMux);
  switch (attribute) {
    case ESP_AVRC_MD_ATTR_TITLE: strlcpy(pendingTitle, (const char *)value, sizeof(pendingTitle)); break;
    case ESP_AVRC_MD_ATTR_ARTIST: strlcpy(pendingArtist, (const char *)value, sizeof(pendingArtist)); break;
    case ESP_AVRC_MD_ATTR_ALBUM: strlcpy(pendingAlbum, (const char *)value, sizeof(pendingAlbum)); break;
    case ESP_AVRC_MD_ATTR_PLAYING_TIME: pendingDurationMs = strtoul((const char *)value, nullptr, 10); break;
    default: break;
  }
  metadataChanged = true;
  portEXIT_CRITICAL(&metadataMux);
}

void updateMetadata() {
  char title[sizeof(pendingTitle)], artist[sizeof(pendingArtist)], album[sizeof(pendingAlbum)];
  uint32_t duration = 0;
  portENTER_CRITICAL(&metadataMux);
  const bool changed = metadataChanged;
  if (changed) {
    memcpy(title, pendingTitle, sizeof(title));
    memcpy(artist, pendingArtist, sizeof(artist));
    memcpy(album, pendingAlbum, sizeof(album));
    duration = pendingDurationMs;
    metadataChanged = false;
  }
  portEXIT_CRITICAL(&metadataMux);
  if (!changed) return;

  const String newTitle = displayText(String(title));
  const String newArtist = displayText(String(artist));
  if (newTitle != btTitle || newArtist != btArtist) {
    elapsedMs = 0;
    markActivity();
    btLyrics = "Now playing: " + newTitle + " by " + newArtist;
  }
  btTitle = newTitle;
  btArtist = newArtist;
  btAlbum = displayText(String(album));
  durationMs = duration;
}

void clearMetadata() {
  portENTER_CRITICAL(&metadataMux);
  pendingTitle[0] = pendingArtist[0] = pendingAlbum[0] = 0;
  pendingDurationMs = 0;
  metadataChanged = false;
  portEXIT_CRITICAL(&metadataMux);
  btTitle = btArtist = btAlbum = "";
  btLyrics = "Waiting for music playback...";
  durationMs = 0;
  elapsedMs = 0;
}

void updatePlaybackState() {
  static uint32_t lastTick = millis();
  const uint32_t now = millis();
  const bool connected = bluetooth.is_connected();
  const bool playing = connected && bluetooth.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED;

  if (playing) elapsedMs += now - lastTick;
  lastTick = now;
  if (durationMs > 0 && elapsedMs > durationMs) elapsedMs = durationMs;

  if (connected != wasConnected) {
    wasConnected = connected;
    markActivity();
    if (connected) {
      showNotice("Connected");
      bluetooth.set_volume((volumePercent * 127 + 50) / 100);
    } else {
      clearMetadata();
      showNotice("Disconnected");
    }
  }
  if (playing != wasPlaying) {
    wasPlaying = playing;
    markActivity();
  }
}

void updateDisplayPower() {
  if (!oledOK) return;
  const uint32_t idle = millis() - lastActivity;
  const bool playing = wasPlaying;

  const bool wantOff = !playing && idle >= OFF_AFTER_MS;
  const bool wantDim = idle >= DIM_AFTER_MS;

  if (wantOff != displayOff) {
    displayOff = wantOff;
    u8g2.setPowerSave(displayOff ? 1 : 0);
  }
  if (wantDim != displayDimmed) {
    displayDimmed = wantDim;
    applyContrast();
  }
}

void syncRemoteVolume() {
  static uint32_t lastPoll = 0;
  const uint32_t now = millis();
  if (!audioOK || !bluetooth.is_connected() || now - lastPoll < 250) return;
  lastPoll = now;
  if (now - lastLocalVolumeChange < 600) return;
  const int remotePercent = (bluetooth.get_volume() * 100 + 63) / 127;
  if (abs(remotePercent - volumePercent) > 1) {
    volumePercent = remotePercent;
    volumePopupUntil = now + 900;
    markPrefsDirty();
    markActivity();
  }
}

void savePrefsIfNeeded() {
  if (!prefsDirty || !settingsOK || millis() - lastPrefChange < 2000) return;
  settings.putUChar("volume", volumePercent);
  settings.putUChar("bright", brightnessPercent);
  prefsDirty = false;
}

// ---------------- Arduino entry points ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32 Bluetooth Speaker (1.3\" OLED 128x64 - Buttons Edition)");

  settingsOK = settings.begin("bt-speaker", false);
  if (settingsOK) {
    volumePercent = constrain(int(settings.getUChar("volume", 40)), 0, 100);
    brightnessPercent = constrain(int(settings.getUChar("bright", 100)), 5, 100);
  }

  // Initialize all tactile buttons with internal pull-ups
  pinMode(BTN_PLAY_PAUSE, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_PAGE, INPUT_PULLUP);

  pinMode(BATTERY_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  updateBattery(true);

  setupOLED();
  bootSplash();

  auto config = audioOutput.defaultConfig(TX_MODE);
  config.pin_bck = I2S_BCK;
  config.pin_ws = I2S_LRCK;
  config.pin_data = I2S_DATA;
  config.sample_rate = 44100;
  config.bits_per_sample = 16;
  config.channels = 2;
  config.buffer_count = 8;
  config.buffer_size = 512;
  audioOK = audioOutput.begin(config);
  if (!audioOK) showNotice("I2S failed", 3000);

  bluetooth.set_auto_reconnect(true);
  bluetooth.set_avrc_metadata_attribute_mask(
    ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_PLAYING_TIME);
  bluetooth.set_avrc_metadata_callback(onMetadata);
  bluetooth.start(BT_NAME);
  bluetooth.set_volume((volumePercent * 127 + 50) / 100);

  Serial.print("Bluetooth ready as: ");
  Serial.println(BT_NAME);
  delay(900);
  markActivity();
}

void loop() {
  handleButtons();
  updateMetadata();
  updateBattery();
  updatePlaybackState();
  syncRemoteVolume();
  updateDisplayPower();
  savePrefsIfNeeded();

  static uint32_t lastDraw = 0;
  const uint32_t now = millis();
  const uint32_t frameMs = (wasPlaying || activeUntil(noticeUntil) || activeUntil(volumePopupUntil)) ? FRAME_MS_ACTIVE : FRAME_MS_IDLE;
  if (oledOK && !displayOff && now - lastDraw >= frameMs) {
    lastDraw = now;
    renderFrame();
  }
}
