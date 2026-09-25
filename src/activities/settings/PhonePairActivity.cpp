#include "PhonePairActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PhonePairActivity::onEnter() {
  Activity::onEnter();
  powerLock = makeUniqueNoThrow<HalPowerManager::Lock>();
  started = PhoneLink::startPairing();
  if (!started) LOG_ERR("BLE", "Could not start iPhone pairing");
  timedOut = false;
  startedAt = millis();
  shownState = started ? PhoneLink::pairState() : PhoneLink::PairState::Failed;
  requestUpdate();
}

void PhonePairActivity::onExit() {
  if (started) PhoneLink::stopPairing();
  started = false;
  powerLock.reset();
  Activity::onExit();
}

void PhonePairActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  auto state = started ? PhoneLink::pairState() : PhoneLink::PairState::Failed;
  if (state != PhoneLink::PairState::Paired && state != PhoneLink::PairState::Failed &&
      millis() - startedAt >= PAIR_TIMEOUT_MS) {
    timedOut = true;
  }
  if (timedOut) state = PhoneLink::PairState::Failed;
  if (state != shownState) {
    shownState = state;
    requestUpdate();
  }
}

void PhonePairActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PHONE_PAIR));

  const int midY = pageHeight / 2;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  switch (shownState) {
    case PhoneLink::PairState::Paired:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_PHONE_PAIR_DONE), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_PHONE_PAIR_DONE_HINT));
      break;
    case PhoneLink::PairState::Failed:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_PHONE_PAIR_FAILED), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CHECK_SERIAL_OUTPUT));
      break;
    default: {
      int y = midY - 3 * lineH;
      for (const char* step :
           {tr(STR_PHONE_PAIR_STEP1), tr(STR_PHONE_PAIR_STEP2), tr(STR_PHONE_PAIR_STEP3), tr(STR_PHONE_PAIR_STEP4)}) {
        renderer.drawCenteredText(UI_10_FONT_ID, y, step);
        y += lineH;
      }
      y += lineH;
      renderer.drawCenteredText(
          UI_12_FONT_ID, y,
          shownState == PhoneLink::PairState::Connected ? tr(STR_PHONE_PAIR_CONNECTED) : tr(STR_PHONE_PAIR_WAITING),
          true, EpdFontFamily::BOLD);
      break;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
