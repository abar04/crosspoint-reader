#include "PhoneMediaActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "PhoneUi.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PhoneMediaActivity::onEnter() {
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

void PhoneMediaActivity::onExit() {
  if (started) PhoneLink::stopLive();
  started = false;
  powerLock.reset();
  Activity::onExit();
}

void PhoneMediaActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state == PhoneLink::LiveState::Ready) {
    // The screen follows the phone's own update, so a press needs no repaint here.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      PhoneLink::mediaCommand(PhoneLink::MediaCommand::TogglePlayPause);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      PhoneLink::mediaCommand(PhoneLink::MediaCommand::NextTrack);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      PhoneLink::mediaCommand(PhoneLink::MediaCommand::PreviousTrack);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      PhoneLink::mediaCommand(PhoneLink::MediaCommand::VolumeUp);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      PhoneLink::mediaCommand(PhoneLink::MediaCommand::VolumeDown);
    }
  }

  if (millis() - lastPoll < POLL_MS) return;
  lastPoll = millis();
  auto newState = started ? PhoneLink::liveState() : PhoneLink::LiveState::Failed;
  if (newState == PhoneLink::LiveState::Connecting && millis() - startedAt >= CONNECT_TIMEOUT_MS) timedOut = true;
  if (timedOut) newState = PhoneLink::LiveState::Failed;
  PhoneLink::liveMedia(fresh);
  // The media fields are all byte-sized, so the struct compares cleanly.
  if (newState != state || memcmp(&fresh, &media, sizeof(media)) != 0) {
    state = newState;
    media = fresh;
    requestUpdate();
  }
}

void PhoneMediaActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int midY = pageHeight / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PHONE_NOW_PLAYING));

  if (state == PhoneLink::LiveState::Failed) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_PHONE_CONNECT_FAILED), true, EpdFontFamily::BOLD);
    drawWrappedCentered(renderer, UI_10_FONT_ID, midY + 10,
                        timedOut ? "timed out waiting for the iPhone" : PhoneLink::lastError(), pageWidth - 20, 5);
  } else if (state == PhoneLink::LiveState::Connecting && !media.known) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_PHONE_CONNECTING), true, EpdFontFamily::BOLD);
  } else if (media.title[0] == '\0') {
    renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_MEDIA_NOTHING));
  } else {
    const int width = pageWidth - 40;
    const int smallH = renderer.getLineHeight(UI_10_FONT_ID);
    int y = midY - 3 * smallH;
    if (media.player[0] != '\0') {
      renderer.drawCenteredText(UI_10_FONT_ID, y, media.player);
    }
    y += smallH + 8;
    y = drawWrappedCentered(renderer, UI_12_FONT_ID, y, media.title, width, 2) + 4;
    if (media.artist[0] != '\0') y = drawWrappedCentered(renderer, UI_10_FONT_ID, y, media.artist, width, 1);
    if (media.album[0] != '\0') y = drawWrappedCentered(renderer, UI_10_FONT_ID, y, media.album, width, 1);
    y += smallH;
    renderer.drawCenteredText(UI_12_FONT_ID, y,
                              PhoneLink::isPlaying(media) ? tr(STR_MEDIA_PLAYING) : tr(STR_MEDIA_PAUSED), true,
                              EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 4;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_MEDIA_VOLUME_HINT));
  }

  const bool ready = state == PhoneLink::LiveState::Ready;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), ready ? tr(STR_MEDIA_PLAY_PAUSE) : "",
                                            ready ? tr(STR_MEDIA_PREVIOUS) : "", ready ? tr(STR_MEDIA_NEXT) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
