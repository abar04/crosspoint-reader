#include "ClockSleepScreen.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace ClockSleepScreen {
namespace {

constexpr uint32_t FACE_MAGIC = 0xC10CFACE;
constexpr uint64_t US_PER_SECOND = 1000000;
// The RTC reports whole seconds, so a wake computed from it lands 0-1 s past
// the minute boundary. The margin keeps sleep-timer drift (it runs from the
// RC slow clock) from waking just before the boundary.
constexpr uint64_t WAKE_MARGIN_US = 500000;
// After this many missed phone syncs in a row, only try every 5th minute.
constexpr uint8_t PHONE_BACKOFF_AFTER = 3;
constexpr int PHONE_BACKOFF_MINUTES = 5;
// Seconds of RTC drift from the phone's time before the RTC is rewritten.
constexpr int PHONE_TIME_TOLERANCE_S = 2;
constexpr int MESSAGE_LINES = 2;

// The face on the glass. RTC_DATA_ATTR memory survives deep sleep and is
// re-initialized (magic cleared) by any other reset.
struct Face {
  uint32_t magic;
  uint8_t hour;  // 0-23
  uint8_t minute;
  bool use12h;
  char posixTz[64];
  bool phoneSync;         // a paired iPhone is synced after each repaint
  uint8_t phoneFailures;  // consecutive syncs the phone missed
  uint8_t notificationCount;
  PhoneLink::Notification notifications[PhoneLink::MAX_NOTIFICATIONS];
};
RTC_DATA_ATTR Face face;

// Seven-segment bits: a top, b upper right, c lower right, d bottom,
// e lower left, f upper left, g middle.
constexpr uint8_t SEG_A = 1 << 0;
constexpr uint8_t SEG_B = 1 << 1;
constexpr uint8_t SEG_C = 1 << 2;
constexpr uint8_t SEG_D = 1 << 3;
constexpr uint8_t SEG_E = 1 << 4;
constexpr uint8_t SEG_F = 1 << 5;
constexpr uint8_t SEG_G = 1 << 6;
constexpr uint8_t DIGIT_SEGMENTS[10] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,          // 0
    SEG_B | SEG_C,                                          // 1
    SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,                  // 2
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,                  // 3
    SEG_B | SEG_C | SEG_F | SEG_G,                          // 4
    SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,                  // 5
    SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,          // 6
    SEG_A | SEG_B | SEG_C,                                  // 7
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,  // 8
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,          // 9
};

struct Metrics {
  int digitW;
  int digitH;
  int half;     // segment half-thickness
  int gap;      // clearance between neighbouring segment tips
  int pairGap;  // between the two digits of a pair
  int colonW;   // colon slot, including its side spacing
};

// Sized for four digits so the face keeps its size when a 12-hour clock goes
// from 9:59 to 10:00. The widest face is 5 digit widths.
Metrics metricsFor(const GfxRenderer& renderer) {
  const int screenW = renderer.getScreenWidth();
  const int margin = screenW / 16;
  Metrics m{};
  m.digitW = (screenW - 2 * margin) / 5;
  m.digitH = m.digitW * 2;
  m.half = m.digitW / 8;
  m.gap = std::max(1, m.digitW / 40);
  m.pairGap = m.digitW / 5;
  m.colonW = m.digitW * 3 / 5;
  return m;
}

// Bar with 45-degree pointed tips at x0 and x1, centred on row y.
void drawHorizontalSegment(const GfxRenderer& renderer, const int x0, const int x1, const int y, const int half) {
  for (int dy = -half; dy <= half; dy++) {
    const int inset = std::abs(dy);
    renderer.fillRect(x0 + inset, y + dy, x1 - x0 + 1 - 2 * inset, 1);
  }
}

// Bar with 45-degree pointed tips at y0 and y1, centred on column x.
void drawVerticalSegment(const GfxRenderer& renderer, const int x, const int y0, const int y1, const int half) {
  for (int dx = -half; dx <= half; dx++) {
    const int inset = std::abs(dx);
    renderer.fillRect(x + dx, y0 + inset, 1, y1 - y0 + 1 - 2 * inset);
  }
}

void drawSegments(const GfxRenderer& renderer, const Metrics& m, const int x, const int y, const uint8_t segments) {
  const int left = x + m.half;
  const int right = x + m.digitW - 1 - m.half;
  const int top = y + m.half;
  const int middle = y + m.digitH / 2;
  const int bottom = y + m.digitH - 1 - m.half;

  if (segments & SEG_A) drawHorizontalSegment(renderer, left + m.gap, right - m.gap, top, m.half);
  if (segments & SEG_G) drawHorizontalSegment(renderer, left + m.gap, right - m.gap, middle, m.half);
  if (segments & SEG_D) drawHorizontalSegment(renderer, left + m.gap, right - m.gap, bottom, m.half);
  if (segments & SEG_F) drawVerticalSegment(renderer, left, top + m.gap, middle - m.gap, m.half);
  if (segments & SEG_B) drawVerticalSegment(renderer, right, top + m.gap, middle - m.gap, m.half);
  if (segments & SEG_E) drawVerticalSegment(renderer, left, middle + m.gap, bottom - m.gap, m.half);
  if (segments & SEG_C) drawVerticalSegment(renderer, right, middle + m.gap, bottom - m.gap, m.half);
}

void drawDigit(const GfxRenderer& renderer, const Metrics& m, const int x, const int y, const int digit) {
  drawSegments(renderer, m, x, y, DIGIT_SEGMENTS[digit]);
}

void drawColon(const GfxRenderer& renderer, const Metrics& m, const int x, const int y) {
  const int dot = 2 * m.half + 1;
  const int dotX = x + (m.colonW - dot) / 2;
  const int middle = y + m.digitH / 2;
  renderer.fillRect(dotX, middle - m.digitH / 5 - m.half, dot, dot);
  renderer.fillRect(dotX, middle + m.digitH / 5 - m.half, dot, dot);
}

// Copies the longest prefix of `text` that fits `maxWidth` into `out` and
// returns where the rest starts. Wrapping breaks at a space when it can; with
// `ellipsize` an overflowing line ends in "..." and the rest is dropped.
const char* fitLine(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style, const char* text,
                    const int maxWidth, const bool ellipsize, char* out, const size_t outSize) {
  while (*text == ' ') text++;
  size_t n = strnlen(text, outSize - 4);
  memcpy(out, text, n);
  out[n] = '\0';
  if (text[n] == '\0' && renderer.getTextWidth(fontId, out, style) <= maxWidth) return text + n;

  const auto prevBoundary = [text](size_t k) {
    while (k > 0 && (static_cast<uint8_t>(text[k]) & 0xC0) == 0x80) k--;
    return k;
  };
  size_t k = prevBoundary(n);
  while (k > 0) {
    memcpy(out, text, k);
    if (ellipsize) {
      memcpy(out + k, "...", 4);
    } else {
      out[k] = '\0';
    }
    if (renderer.getTextWidth(fontId, out, style) <= maxWidth) break;
    k = prevBoundary(k - 1);
  }
  if (ellipsize) return text + strlen(text);

  // Prefer a word break in the back half of the line.
  for (size_t i = k; i > k / 2; i--) {
    if (text[i] == ' ') {
      k = i;
      out[k] = '\0';
      break;
    }
  }
  return text + k;
}

void drawNotifications(const GfxRenderer& renderer, const Face& f, int y) {
  const int screenW = renderer.getScreenWidth();
  const int margin = screenW / 16;
  const int width = screenW - 2 * margin;
  const int bottom = renderer.getScreenHeight() - margin;
  const int titleH = renderer.getLineHeight(UI_12_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_10_FONT_ID);
  char line[PhoneLink::MESSAGE_LEN + 4];

  for (int i = 0; i < f.notificationCount && y + 8 + titleH <= bottom; i++) {
    const PhoneLink::Notification& n = f.notifications[i];
    renderer.fillRect(margin, y, width, 1);
    y += 8;
    if (n.title[0] != '\0') {
      fitLine(renderer, UI_12_FONT_ID, EpdFontFamily::BOLD, n.title, width, true, line, sizeof(line));
      renderer.drawText(UI_12_FONT_ID, margin, y, line, true, EpdFontFamily::BOLD);
      y += titleH;
    }
    const char* rest = n.message;
    for (int l = 0; l < MESSAGE_LINES && *rest != '\0' && y + bodyH <= bottom; l++) {
      rest = fitLine(renderer, UI_10_FONT_ID, EpdFontFamily::REGULAR, rest, width, l == MESSAGE_LINES - 1, line,
                     sizeof(line));
      renderer.drawText(UI_10_FONT_ID, margin, y, line);
      y += bodyH;
    }
    y += 8;
  }
}

// Deterministic for a given face: update() redraws the previous minute to
// rebuild the controller baseline, so the pixels must match the earlier paint.
// With notifications the clock moves to the top and the list fills the rest.
void drawFace(const GfxRenderer& renderer, const Face& f) {
  const Metrics m = metricsFor(renderer);

  int hour = f.hour;
  if (f.use12h) {
    hour %= 12;
    if (hour == 0) hour = 12;
  }
  // 24-hour time keeps its leading zero; 12-hour time drops it.
  const bool twoHourDigits = !f.use12h || hour >= 10;
  const int hourW = twoHourDigits ? 2 * m.digitW + m.pairGap : m.digitW;
  const int faceW = hourW + m.colonW + 2 * m.digitW + m.pairGap;

  int x = (renderer.getScreenWidth() - faceW) / 2;
  const int y = f.notificationCount > 0 ? renderer.getScreenHeight() / 12 : (renderer.getScreenHeight() - m.digitH) / 2;
  int belowClock = y + m.digitH;

  if (twoHourDigits) {
    drawDigit(renderer, m, x, y, hour / 10);
    x += m.digitW + m.pairGap;
  }
  drawDigit(renderer, m, x, y, hour % 10);
  x += m.digitW;

  drawColon(renderer, m, x, y);
  x += m.colonW;

  drawDigit(renderer, m, x, y, f.minute / 10);
  x += m.digitW + m.pairGap;
  drawDigit(renderer, m, x, y, f.minute % 10);

  if (f.use12h) {
    const char* marker = HalClock::meridiem(f.hour);
    const int markerX = x + m.digitW - renderer.getTextWidth(UI_12_FONT_ID, marker, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, markerX, y + m.digitH + m.digitH / 10, marker, true, EpdFontFamily::BOLD);
    belowClock += m.digitH / 10 + renderer.getLineHeight(UI_12_FONT_ID);
  }
  drawNotifications(renderer, f, belowClock + m.digitH / 6);
}

// "--:--" with a note, for an RTC that has no valid time (never set, or its
// backup supply ran out).
void drawUnsetFace(const GfxRenderer& renderer) {
  const Metrics m = metricsFor(renderer);
  const int pairW = 2 * m.digitW + m.pairGap;
  int x = (renderer.getScreenWidth() - (2 * pairW + m.colonW)) / 2;
  const int y = (renderer.getScreenHeight() - m.digitH) / 2;
  for (int pair = 0; pair < 2; pair++) {
    drawSegments(renderer, m, x, y, SEG_G);
    drawSegments(renderer, m, x + m.digitW + m.pairGap, y, SEG_G);
    x += pairW;
    if (pair == 0) {
      drawColon(renderer, m, x, y);
      x += m.colonW;
    }
  }
  char note[64];
  snprintf(note, sizeof(note), "%s: %s", tr(STR_CLOCK), tr(STR_NOT_SET));
  renderer.drawCenteredText(UI_12_FONT_ID, y + m.digitH + m.digitH / 5, note, true, EpdFontFamily::BOLD);
}

}  // namespace

bool isSupported() { return gpio.deviceIsX3() && halClock.isAvailable(); }

bool render(GfxRenderer& renderer) {
  if (!isSupported()) return false;

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();

  struct tm now;
  if (!halClock.localTime(now, /*fresh=*/true)) {
    // No wake timer: there is no time to keep updated.
    LOG_ERR("CLK", "Clock sleep screen: RTC has no valid time");
    drawUnsetFace(renderer);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return true;
  }

  face.hour = static_cast<uint8_t>(now.tm_hour);
  face.minute = static_cast<uint8_t>(now.tm_min);
  face.use12h = SETTINGS.clockFormat == 1;
  const char* tz = getenv("TZ");
  snprintf(face.posixTz, sizeof(face.posixTz), "%s", tz ? tz : "");
  face.phoneSync = PhoneLink::isAvailable() && PhoneLink::isPaired();
  face.phoneFailures = 0;
  face.notificationCount = 0;

  drawFace(renderer, face);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  face.magic = FACE_MAGIC;
  return true;
}

bool isActive() { return face.magic == FACE_MAGIC; }

void deactivate() { face.magic = 0; }

void restoreTimezone() { halClock.setTimezone(face.posixTz); }

bool needsRepaint(const struct tm& now) { return now.tm_hour != face.hour || now.tm_min != face.minute; }

void update(GfxRenderer& renderer, const struct tm& now) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // A clean refresh once an hour keeps differential repaints from building up ghosting.
  const bool cleanRefresh = now.tm_hour != face.hour;
  if (!cleanRefresh) {
    // display.begin() cleared the controller RAM. Reload it with the face on
    // the glass so the differential waveform drives only the changed segments.
    renderer.clearScreen();
    drawFace(renderer, face);
    renderer.cleanupGrayscaleWithFrameBuffer();
  }

  face.hour = static_cast<uint8_t>(now.tm_hour);
  face.minute = static_cast<uint8_t>(now.tm_min);
  renderer.clearScreen();
  drawFace(renderer, face);
  if (cleanRefresh) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  }
}

bool phoneSyncDue(const struct tm& now) {
  return face.phoneSync && (face.phoneFailures < PHONE_BACKOFF_AFTER || now.tm_min % PHONE_BACKOFF_MINUTES == 0);
}

void applyPhoneSync(GfxRenderer& renderer, const bool ok, const PhoneLink::SyncResult& result) {
  if (!ok) {
    if (face.phoneFailures < UINT8_MAX) face.phoneFailures++;
    return;
  }
  face.phoneFailures = 0;

  if (result.gotTime) {
    if (result.hasUtcOffset) {
      // The phone's zone wins while asleep, so travel and DST changes follow
      // it. POSIX offsets count west of UTC as positive.
      const int32_t offset = result.utcOffsetSeconds;
      const int32_t absOffset = offset < 0 ? -offset : offset;
      snprintf(face.posixTz, sizeof(face.posixTz), "UTC%c%d:%02d", offset < 0 ? '+' : '-',
               static_cast<int>(absOffset / 3600), static_cast<int>(absOffset % 3600 / 60));
      halClock.setTimezone(face.posixTz);
    }
    halClock.adjustTo(result.utc, PHONE_TIME_TOLERANCE_S);
  }

  bool changed = false;
  if (result.gotNotifications &&
      (result.count != face.notificationCount ||
       memcmp(result.items, face.notifications, sizeof(PhoneLink::Notification) * result.count) != 0)) {
    face.notificationCount = result.count;
    memcpy(face.notifications, result.items, sizeof(PhoneLink::Notification) * result.count);
    changed = true;
  }
  struct tm now;
  if (halClock.localTime(now, /*fresh=*/true) && needsRepaint(now)) {
    face.hour = static_cast<uint8_t>(now.tm_hour);
    face.minute = static_cast<uint8_t>(now.tm_min);
    changed = true;
  }
  if (!changed) return;

  // update() already left the controller holding the face on the glass.
  renderer.clearScreen();
  drawFace(renderer, face);
  renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
}

bool armWakeTimer() {
  if (!isActive()) return false;

  struct tm now;
  if (!halClock.localTime(now, /*fresh=*/true)) {
    LOG_ERR("CLK", "Clock sleep screen: RTC read failed, not arming wake timer");
    return false;
  }
  const uint64_t untilNextMinuteUs = needsRepaint(now) ? 0 : static_cast<uint64_t>(60 - now.tm_sec) * US_PER_SECOND;
  const uint64_t delayUs = untilNextMinuteUs + WAKE_MARGIN_US;
  esp_sleep_enable_timer_wakeup(delayUs);
  LOG_DBG("CLK", "Clock sleep screen: next wake in %lu ms", static_cast<unsigned long>(delayUs / 1000));
  return true;
}

}  // namespace ClockSleepScreen
