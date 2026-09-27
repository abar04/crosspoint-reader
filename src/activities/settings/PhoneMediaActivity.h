#pragma once

#include <HalPowerManager.h>

#include <memory>

#include "activities/Activity.h"
#include "network/PhoneLink.h"

// Now playing on the paired iPhone, with playback controls: Confirm plays or
// pauses, Left/Right skip tracks and Up/Down change the volume. Stays
// connected while open.
class PhoneMediaActivity final : public Activity {
 public:
  explicit PhoneMediaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PhoneMedia", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr unsigned long POLL_MS = 500;
  static constexpr unsigned long CONNECT_TIMEOUT_MS = 20000;

  bool started = false;
  bool timedOut = false;
  unsigned long startedAt = 0;
  unsigned long lastPoll = 0;
  PhoneLink::LiveState state = PhoneLink::LiveState::Connecting;
  PhoneLink::Media media{};
  PhoneLink::Media fresh{};  // poll scratch, kept off the stack
  // BLE needs the full CPU clock; idle power saving would drop it to 10 MHz.
  std::unique_ptr<HalPowerManager::Lock> powerLock;
};
