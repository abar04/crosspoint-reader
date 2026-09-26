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
constexpr size_t APP_LEN = 24;      // app display name, UTF-8 bytes including NUL

struct Notification {
  uint32_t uid;             // ANCS id, used to dismiss it
  uint32_t arrivedMinutes;  // phone's local time in minutes since 1970; 0 = unknown
  uint8_t category;         // ANCS CategoryID
  char app[APP_LEN];        // empty when the phone did not name the app
  char title[TITLE_LEN];
  char message[MESSAGE_LEN];
};

// Sender & app mode: one notification without its message text.
constexpr int MAX_SENDERS = 8;
struct Sender {
  uint32_t arrivedMinutes;  // as Notification::arrivedMinutes
  char app[APP_LEN];        // empty when the phone did not name the app
  char title[TITLE_LEN];    // the sender, for messages and mail
};

struct SyncResult {
  bool gotTime = false;
  time_t utc = 0;
  bool hasUtcOffset = false;
  int32_t utcOffsetSeconds = 0;  // phone's local time = utc + offset (DST included)
  bool gotNotifications = false;
  uint8_t count = 0;  // newest first
  Notification items[MAX_NOTIFICATIONS];
  int8_t phoneBattery = -1;  // percent; -1 when the phone offers no Battery Service
  // Sender & app mode: items stays empty and these describe the notifications.
  bool summary = false;
  uint16_t total = 0;       // notifications on the phone that pass the filter
  uint8_t senderCount = 0;  // newest first
  Sender senders[MAX_SENDERS];
};

// How much of each notification Clock sleep fetches and shows: the full text,
// or the count plus each notification's sender and app without the message.
enum class Detail : uint8_t { Full, SenderAndApp, Count };
Detail detail();
void setDetail(Detail d);

// Which notification categories are fetched from the phone.
enum class Filter : uint8_t { All, Messages, MessagesAndCalendar, Count };
Filter filter();
void setFilter(Filter f);

// Hours when Clock sleep skips the phone sync (the clock keeps running).
// Index 0 is Off; the rest are fixed windows, see quietHoursLabel().
constexpr uint8_t QUIET_HOURS_COUNT = 4;
uint8_t quietHours();
void setQuietHours(uint8_t index);
const char* quietHoursLabel(uint8_t index);  // "22:00-07:00"; nullptr for Off
bool inQuietHours(uint8_t index, int hour);

// Local wall-clock time as minutes since 1970, the unit of arrivedMinutes.
uint32_t localMinutes(const struct tm& wallClock);

// Compact age ("now", "5m", "2h", "3d"); empty when the arrival time is unknown.
void formatAge(char* buf, size_t size, uint32_t arrivedMinutes, uint32_t nowMinutes);

// NimBLE is compiled in and this is an X3.
bool isAvailable();

// The BLE controller memory survived boot. Arduino releases it unless the link
// was turned on with enableBluetooth() (or a phone is paired) before this boot.
bool bluetoothReady();

// Keeps the BLE memory from the next boot on; takes effect after a restart.
void enableBluetooth();

// Human-readable reason for the last failure, for on-screen diagnosis.
const char* lastError();

// An iPhone has been paired with this device.
bool isPaired();

// Blocking: advertises for the paired iPhone, reads its time and current
// notifications, disconnects and shuts BLE down. Returns false when the phone
// did not connect or no data arrived within timeoutMs.
bool sync(SyncResult& out, uint32_t timeoutMs);

// Live session for the notifications screen: stays connected, keeps the list
// current and can dismiss notifications on the phone.
enum class LiveState : uint8_t { Connecting, Ready, Failed };
bool startLive();
LiveState liveState();
// Copies up to `max` notifications, newest first. `battery` gets the phone's
// level or -1.
int liveSnapshot(Notification* out, int max, int8_t& battery);
// Clears the notification on the iPhone (the ANCS negative action).
bool dismiss(uint32_t uid);
void stopLive();

enum class PairState : uint8_t { Idle, Advertising, Connected, Paired, Failed };

// Forgets any previous iPhone and advertises for a new one to pair from
// Settings > Bluetooth. Runs until stopPairing().
bool startPairing();
PairState pairState();
void stopPairing();

}  // namespace PhoneLink
