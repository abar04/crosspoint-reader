#pragma once

#include <HalPowerManager.h>

#include <memory>

#include "activities/Activity.h"
#include "network/PhoneLink.h"

// Live list of the paired iPhone's notifications. Up/Down select one and
// Confirm dismisses it on the phone. Stays connected while open.
class PhoneNotificationsActivity final : public Activity {
 public:
  explicit PhoneNotificationsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PhoneNotifications", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int MAX_ITEMS = 8;
  static constexpr unsigned long POLL_MS = 500;
  static constexpr unsigned long CONNECT_TIMEOUT_MS = 20000;

  bool refresh();

  bool started = false;
  unsigned long startedAt = 0;
  unsigned long lastPoll = 0;
  PhoneLink::LiveState state = PhoneLink::LiveState::Connecting;
  bool timedOut = false;
  int count = 0;
  int selected = 0;
  int8_t battery = -1;
  PhoneLink::Notification items[MAX_ITEMS]{};
  PhoneLink::Notification fresh[MAX_ITEMS]{};  // refresh() scratch, kept off the stack
  // BLE needs the full CPU clock; idle power saving would drop it to 10 MHz.
  std::unique_ptr<HalPowerManager::Lock> powerLock;
};
