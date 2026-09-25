#include "PhoneNotificationsActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "PhoneUi.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PhoneNotificationsActivity::onEnter() {
  Activity::onEnter();
  powerLock = makeUniqueNoThrow<HalPowerManager::Lock>();
  // Font caches rebuild on demand; drop them so NimBLE's buffers fit.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
    fcm->clearCache();
  }
  started = PhoneLink::startLive();
  state = started ? PhoneLink::LiveState::Connecting : PhoneLink::LiveState::Failed;
  startedAt = millis();
  requestUpdate();
}

void PhoneNotificationsActivity::onExit() {
  if (started) PhoneLink::stopLive();
  started = false;
  powerLock.reset();
  Activity::onExit();
}

// Returns true when anything shown changed.
bool PhoneNotificationsActivity::refresh() {
  auto newState = started ? PhoneLink::liveState() : PhoneLink::LiveState::Failed;
  if (newState == PhoneLink::LiveState::Connecting && millis() - startedAt >= CONNECT_TIMEOUT_MS) timedOut = true;
  if (timedOut) newState = PhoneLink::LiveState::Failed;

  int8_t freshBattery = -1;
  const int freshCount = started ? PhoneLink::liveSnapshot(fresh, MAX_ITEMS, freshBattery) : 0;

  bool changed = newState != state || freshCount != count || freshBattery != battery;
  for (int i = 0; i < freshCount && !changed; i++) {
    changed = fresh[i].uid != items[i].uid || strcmp(fresh[i].app, items[i].app) != 0;
  }
  state = newState;
  count = freshCount;
  battery = freshBattery;
  memcpy(items, fresh, sizeof(PhoneLink::Notification) * freshCount);
  if (selected >= count) selected = count > 0 ? count - 1 : 0;
  return changed;
}

void PhoneNotificationsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state == PhoneLink::LiveState::Ready && count > 0) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      selected = (selected + 1) % count;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
               mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      selected = (selected + count - 1) % count;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // The list updates when the phone reports the notification removed.
      if (!PhoneLink::dismiss(items[selected].uid)) LOG_ERR("BLE", "Could not send dismiss");
    }
  }

  if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    if (refresh()) requestUpdate();
  }
}

void PhoneNotificationsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int margin = 16;
  const int width = pageWidth - 2 * margin;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PHONE_NOTIFICATIONS));

  const int midY = pageHeight / 2;
  if (state == PhoneLink::LiveState::Failed) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_PHONE_CONNECT_FAILED), true, EpdFontFamily::BOLD);
    drawWrappedCentered(renderer, UI_10_FONT_ID, midY + 10,
                        timedOut ? "timed out waiting for the iPhone" : PhoneLink::lastError(), pageWidth - 20, 5);
  } else if (state == PhoneLink::LiveState::Connecting) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_PHONE_CONNECTING), true, EpdFontFamily::BOLD);
  } else {
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int smallH = renderer.getLineHeight(UI_10_FONT_ID);
    const int titleH = renderer.getLineHeight(UI_12_FONT_ID);
    if (battery >= 0) {
      char text[32];
      snprintf(text, sizeof(text), tr(STR_PHONE_BATTERY), static_cast<int>(battery));
      renderer.drawText(UI_10_FONT_ID, margin, y, text);
      y += smallH + 6;
    }
    if (count == 0) {
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_PHONE_NO_NOTIFICATIONS));
    }

    struct tm now;
    const uint32_t nowMinutes = halClock.localTime(now) ? PhoneLink::localMinutes(now) : 0;
    const int bottom = pageHeight - metrics.buttonHintsHeight - 4;
    const int rowH = 2 * smallH + titleH + 12;
    // Keep the selection on screen by starting the page at it when needed.
    const int perPage = std::max(1, (bottom - y) / rowH);
    const int first = selected >= perPage ? selected - perPage + 1 : 0;
    for (int i = first; i < count && y + rowH <= bottom; i++) {
      const PhoneLink::Notification& n = items[i];
      const bool isSelected = i == selected;
      if (isSelected) renderer.fillRect(margin - 6, y, width + 12, rowH - 4);
      const bool ink = !isSelected;
      int ty = y + 4;
      char age[16];
      PhoneLink::formatAge(age, sizeof(age), n.arrivedMinutes, nowMinutes);
      char meta[PhoneLink::APP_LEN + sizeof(age) + 8];
      snprintf(meta, sizeof(meta), "%s%s%s", n.app, n.app[0] != '\0' && age[0] != '\0' ? " \xC2\xB7 " : "", age);
      renderer.drawText(UI_10_FONT_ID, margin, ty, meta, ink);
      ty += smallH;
      renderer.drawText(UI_12_FONT_ID, margin, ty, n.title, ink, EpdFontFamily::BOLD);
      ty += titleH;
      // One line of the message, cut at the width.
      char body[PhoneLink::MESSAGE_LEN];
      snprintf(body, sizeof(body), "%s", n.message);
      size_t len = strlen(body);
      while (len > 0 && renderer.getTextWidth(UI_10_FONT_ID, body) > width) {
        do {
          len--;
        } while (len > 0 && (static_cast<uint8_t>(body[len]) & 0xC0) == 0x80);
        body[len] = '\0';
      }
      renderer.drawText(UI_10_FONT_ID, margin, ty, body, ink);
      y += rowH;
    }
  }

  const bool canDismiss = state == PhoneLink::LiveState::Ready && count > 0;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), canDismiss ? tr(STR_PHONE_DISMISS) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
