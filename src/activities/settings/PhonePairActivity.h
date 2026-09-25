#pragma once

#include <HalPowerManager.h>

#include <memory>

#include "activities/Activity.h"
#include "network/PhoneLink.h"

// Pairs an iPhone for the Clock sleep screen's time and notification sync.
// Advertises until the phone pairs from its Bluetooth settings and allows
// notifications, or the user backs out.
class PhonePairActivity final : public Activity {
 public:
  explicit PhonePairActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PhonePair", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr unsigned long PAIR_TIMEOUT_MS = 120000;

  PhoneLink::PairState shownState = PhoneLink::PairState::Idle;
  bool started = false;
  bool timedOut = false;
  unsigned long startedAt = 0;
  // BLE needs the full CPU clock; idle power saving would drop it to 10 MHz.
  std::unique_ptr<HalPowerManager::Lock> powerLock;
};
