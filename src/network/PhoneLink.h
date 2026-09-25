#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

// iPhone link over BLE: reads the time from Apple's Current Time Service and
// the notifications currently on the phone from ANCS. The X3 acts as a BLE
// peripheral that a bonded iPhone reconnects to on its own, then as a GATT
// client of the phone. Compiled in only when the build keeps NimBLE.
namespace PhoneLink {

constexpr int MAX_NOTIFICATIONS = 4;
constexpr size_t TITLE_LEN = 32;    // UTF-8 bytes including NUL
constexpr size_t MESSAGE_LEN = 96;  // UTF-8 bytes including NUL

struct Notification {
  char title[TITLE_LEN];
  char message[MESSAGE_LEN];
};

struct SyncResult {
  bool gotTime = false;
  time_t utc = 0;
  bool hasUtcOffset = false;
  int32_t utcOffsetSeconds = 0;  // phone's local time = utc + offset (DST included)
  bool gotNotifications = false;
  uint8_t count = 0;  // newest first
  Notification items[MAX_NOTIFICATIONS];
};

// NimBLE is compiled in and this is an X3.
bool isAvailable();

// An iPhone has been paired with this device.
bool isPaired();

// Blocking: advertises for the paired iPhone, reads its time and current
// notifications, disconnects and shuts BLE down. Returns false when the phone
// did not connect or no data arrived within timeoutMs.
bool sync(SyncResult& out, uint32_t timeoutMs);

enum class PairState : uint8_t { Idle, Advertising, Connected, Paired, Failed };

// Forgets any previous iPhone and advertises for a new one to pair from
// Settings > Bluetooth. Runs until stopPairing().
bool startPairing();
PairState pairState();
void stopPairing();

}  // namespace PhoneLink
