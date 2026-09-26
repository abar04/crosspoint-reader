#include "PhoneLink.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#if __has_include(<host/ble_hs.h>)
#define PHONE_LINK_ENABLED 1
#else
#define PHONE_LINK_ENABLED 0
#endif

#if PHONE_LINK_ENABLED
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <nvs.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

extern "C" void ble_store_config_init(void);

namespace {
constexpr char NVS_NAMESPACE[] = "cpphone";
constexpr char NVS_PAIRED_KEY[] = "paired";
constexpr char NVS_ENABLED_KEY[] = "enabled";
constexpr char NVS_FILTER_KEY[] = "filter";
constexpr char NVS_QUIET_KEY[] = "quiet";
constexpr char NVS_DETAIL_KEY[] = "detail";
bool bleMemoryKept = false;

uint8_t readFlag(const char* key) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
  uint8_t value = 0;
  nvs_get_u8(h, key, &value);
  nvs_close(h);
  return value;
}

void writeFlag(const char* key, const uint8_t value) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, key, value);
  nvs_commit(h);
  nvs_close(h);
}
}  // namespace

// Arduino's initArduino() releases the BLE controller memory (~36 KB) at boot
// unless this returns true, after which the controller can never start. Keep
// it only on devices where the iPhone link has been turned on.
extern "C" bool bleInUse(void) {
  bleMemoryKept = readFlag(NVS_ENABLED_KEY) != 0 || readFlag(NVS_PAIRED_KEY) != 0;
  return bleMemoryKept;
}
#endif

namespace PhoneLink {

#if !PHONE_LINK_ENABLED

bool isAvailable() { return false; }
bool bluetoothReady() { return false; }
void enableBluetooth() {}
const char* lastError() { return "Bluetooth not in this build"; }
bool isPaired() { return false; }
bool sync(SyncResult&, uint32_t) { return false; }
bool startPairing() { return false; }
PairState pairState() { return PairState::Idle; }
void stopPairing() {}
Filter filter() { return Filter::All; }
void setFilter(Filter) {}
Detail detail() { return Detail::Full; }
void setDetail(Detail) {}
uint8_t quietHours() { return 0; }
void setQuietHours(uint8_t) {}
bool startLive() { return false; }
LiveState liveState() { return LiveState::Failed; }
int liveSnapshot(Notification*, int, int8_t& battery) {
  battery = -1;
  return 0;
}
bool dismiss(uint32_t) { return false; }
void stopLive() {}

#else

namespace {

constexpr char DEVICE_NAME[] = "CrossPoint";
// A sync ends once no new notification has been announced for this long and
// every requested one has been answered.
constexpr uint32_t NOTIFICATIONS_SETTLE_MS = 600;
constexpr uint32_t DISCONNECT_WAIT_MS = 1000;
// Candidates fetched per sync; the newest MAX_NOTIFICATIONS are kept.
constexpr int MAX_TRACKED = 8;
// App & count mode only asks for each notification's app, so it can follow
// more of them for the per-app breakdown.
constexpr int MAX_TRACKED_SUMMARY = 20;
constexpr int MAX_ENTRIES = MAX_TRACKED_SUMMARY;

// Current Time Service (Bluetooth SIG).
const ble_uuid16_t CTS_SERVICE = BLE_UUID16_INIT(0x1805);
constexpr uint16_t CTS_CURRENT_TIME = 0x2A2B;
constexpr uint16_t CTS_LOCAL_TIME_INFO = 0x2A0F;
constexpr uint16_t CCCD_UUID = 0x2902;

// Battery Service (Bluetooth SIG); iPhones usually offer it to paired accessories.
const ble_uuid16_t BATTERY_SERVICE = BLE_UUID16_INIT(0x180F);
constexpr uint16_t BATTERY_LEVEL = 0x2A19;

// Apple Notification Center Service.
const ble_uuid128_t ANCS_SERVICE =
    BLE_UUID128_INIT(0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
const ble_uuid128_t ANCS_NOTIFICATION_SOURCE =
    BLE_UUID128_INIT(0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, 0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
const ble_uuid128_t ANCS_CONTROL_POINT =
    BLE_UUID128_INIT(0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, 0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
const ble_uuid128_t ANCS_DATA_SOURCE =
    BLE_UUID128_INIT(0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, 0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);

constexpr uint8_t ANCS_EVENT_ADDED = 0;
constexpr uint8_t ANCS_EVENT_REMOVED = 2;
constexpr uint8_t ANCS_CMD_GET_NOTIFICATION_ATTRIBUTES = 0;
constexpr uint8_t ANCS_CMD_GET_APP_ATTRIBUTES = 1;
constexpr uint8_t ANCS_CMD_PERFORM_ACTION = 2;
constexpr uint8_t ANCS_ACTION_NEGATIVE = 1;  // "Clear" for most notifications
constexpr uint8_t ANCS_ATTR_APP_ID = 0;
constexpr uint8_t ANCS_APP_ATTR_DISPLAY_NAME = 0;
constexpr uint8_t ANCS_ATTR_TITLE = 1;
constexpr uint8_t ANCS_ATTR_MESSAGE = 3;
constexpr uint8_t ANCS_ATTR_DATE = 5;
constexpr size_t ANCS_DATE_LEN = 16;  // "yyyyMMdd'T'HHmmSS" + NUL
constexpr size_t APP_ID_LEN = 48;     // bundle id, e.g. "com.apple.MobileSMS"

// ANCS CategoryIDs kept by each Filter.
constexpr uint16_t CATEGORY_MESSAGES = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 6);
constexpr uint16_t CATEGORY_CALENDAR = 1u << 5;

enum class Mode : uint8_t { Sync, Pair, Live };

enum class EntryState : uint8_t { Pending, InFlight, Done };

struct Entry {
  EntryState state;
  char appId[APP_ID_LEN];
  char date[ANCS_DATE_LEN];
  Notification n;
};

// App display names, looked up once per app per session.
struct AppName {
  EntryState state;
  char appId[APP_ID_LEN];
  char name[APP_LEN];
};

struct Chr {
  uint16_t defHandle;
  uint16_t valHandle;
};

// One BLE session. GATT/GAP callbacks run on the NimBLE host task; the
// calling task polls the volatile progress fields and reads the results only
// after the host has stopped.
struct Session {
  Mode mode;
  SemaphoreHandle_t lock;
  uint8_t ownAddrType;
  volatile uint16_t conn;
  volatile bool connected;
  volatile bool disconnected;
  volatile bool failed;
  volatile bool stopping;
  volatile bool timeDone;
  volatile bool subscribed;
  volatile uint32_t lastEventMs;
  volatile PairState pairState;

  uint16_t svcStart, svcEnd;
  uint16_t ctsTime, ctsLocalInfo;
  Chr ancsNs, ancsDs, ancsCp;
  uint16_t nsCccd, dsCccd;

  uint8_t localTime[10];
  bool hasLocalTime;

  uint16_t batteryLevel;
  volatile int8_t phoneBattery;
  uint16_t categoryMask;
  bool summaryOnly;  // App & count mode: fetch only each notification's app
  int trackLimit;
  uint8_t categoryCounts[16];  // latest ANCS CategoryCount per category

  Entry entries[MAX_ENTRIES];
  int entryCount;
  AppName apps[MAX_ENTRIES];
  int appCount;
  uint32_t inFlightUid;
  int inFlightApp;  // index into apps, or -1 for a notification request
  bool requestInFlight;
  uint8_t dsBuf[320];
  uint16_t dsLen;

  SyncResult* out;
};

Session* session = nullptr;
TaskHandle_t hostTask = nullptr;

class SessionLock {
 public:
  SessionLock() { xSemaphoreTakeRecursive(session->lock, portMAX_DELAY); }
  ~SessionLock() { xSemaphoreGiveRecursive(session->lock); }
};

void setPairedFlag(const bool paired) { writeFlag(NVS_PAIRED_KEY, paired ? 1 : 0); }

// Last failure, shown on the pairing screen for diagnosis.
char lastErrorText[160] = "";

// ESP-IDF reports most controller init failures as ESP_ERR_NO_MEM; its own
// error log line carries the real reason, so keep the last one seen.
char idfErrorLine[72] = "";
vprintf_like_t previousLogger = nullptr;

int captureIdfErrors(const char* fmt, va_list args) {
  char line[96];
  va_list copy;
  va_copy(copy, args);
  vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);
  if (const char* e = strstr(line, "E (")) {
    snprintf(idfErrorLine, sizeof(idfErrorLine), "%s", e);
    for (char* p = idfErrorLine; *p; p++) {
      if (*p == '\n' || *p == '\r' || *p == '\033') *p = ' ';
    }
  }
  return previousLogger ? previousLogger(fmt, args) : 0;
}

void setError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(lastErrorText, sizeof(lastErrorText), fmt, args);
  va_end(args);
  LOG_ERR("BLE", "Phone link: %s", lastErrorText);
}

void fail(const char* why, const int rc = 0) {
  if (rc != 0) {
    setError("%s (rc %d)", why, rc);
  } else {
    setError("%s", why);
  }
  session->failed = true;
  if (session->mode == Mode::Pair) session->pairState = PairState::Failed;
  if (session->conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(session->conn, BLE_ERR_REM_USER_CONN_TERM);
}

// ANCS trouble after the bond is in place. A sync ends; pairing still counts,
// since the time works and notification access is retried on every sync.
void ancsUnavailable(const char* why) {
  if (session->mode != Mode::Pair) {
    fail(why);
    return;
  }
  setError("%s", why);
  session->pairState = PairState::Paired;
}

// Days-from-civil (Howard Hinnant); CTS carries a calendar date.
time_t epochFromCivil(int y, const unsigned m, const unsigned d, const int hh, const int mm, const int ss) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long days = static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
  return static_cast<time_t>(days) * 86400 + hh * 3600L + mm * 60L + ss;
}

void finishTime(const int8_t tzQuarters, const uint8_t dstQuarters, const bool hasLocalInfo) {
  const uint8_t* t = session->localTime;
  if (session->hasLocalTime && session->out) {
    const int year = t[0] | (t[1] << 8);
    if (year >= 2020 && t[2] >= 1 && t[2] <= 12 && t[3] >= 1 && t[3] <= 31) {
      const time_t local = epochFromCivil(year, t[2], t[3], t[4], t[5], t[6]);
      auto& out = *session->out;
      if (hasLocalInfo && tzQuarters != -128 && dstQuarters != 255) {
        out.utcOffsetSeconds = (tzQuarters + dstQuarters) * 15 * 60;
        out.hasUtcOffset = true;
        out.utc = local - out.utcOffsetSeconds;
      } else {
        // No zone from the phone: interpret its local time under our TZ rule.
        struct tm tmLocal{};
        gmtime_r(&local, &tmLocal);
        tmLocal.tm_isdst = -1;
        out.utc = mktime(&tmLocal);
      }
      out.gotTime = true;
    }
  }
  session->timeDone = true;
}

void startBatteryDiscovery(uint16_t conn);

int onLocalInfoRead(uint16_t, const ble_gatt_error* error, ble_gatt_attr* attr, void*) {
  uint8_t info[2] = {0x80, 0xFF};
  const bool ok = error->status == 0 && attr && OS_MBUF_PKTLEN(attr->om) >= 2 &&
                  os_mbuf_copydata(attr->om, 0, sizeof(info), info) == 0;
  finishTime(static_cast<int8_t>(info[0]), info[1], ok);
  startBatteryDiscovery(session->conn);
  return 0;
}

int onCurrentTimeRead(uint16_t conn, const ble_gatt_error* error, ble_gatt_attr* attr, void*) {
  session->hasLocalTime = error->status == 0 && attr && OS_MBUF_PKTLEN(attr->om) >= 7 &&
                          os_mbuf_copydata(attr->om, 0, 7, session->localTime) == 0;
  if (session->ctsLocalInfo && ble_gattc_read(conn, session->ctsLocalInfo, onLocalInfoRead, nullptr) == 0) {
    return 0;
  }
  finishTime(0, 0, false);
  startBatteryDiscovery(conn);
  return 0;
}

int onCtsChr(uint16_t conn, const ble_gatt_error* error, const ble_gatt_chr* chr, void*) {
  if (error->status == 0 && chr) {
    if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
      if (chr->uuid.u16.value == CTS_CURRENT_TIME) session->ctsTime = chr->val_handle;
      if (chr->uuid.u16.value == CTS_LOCAL_TIME_INFO) session->ctsLocalInfo = chr->val_handle;
    }
    return 0;
  }
  if (session->ctsTime && ble_gattc_read(conn, session->ctsTime, onCurrentTimeRead, nullptr) == 0) return 0;
  finishTime(0, 0, false);
  startBatteryDiscovery(conn);
  return 0;
}

int onCtsService(uint16_t conn, const ble_gatt_error* error, const ble_gatt_svc* svc, void*) {
  if (error->status == 0 && svc) {
    session->svcStart = svc->start_handle;
    session->svcEnd = svc->end_handle;
    return 0;
  }
  if (session->svcStart && ble_gattc_disc_all_chrs(conn, session->svcStart, session->svcEnd, onCtsChr, nullptr) == 0) {
    return 0;
  }
  LOG_INF("BLE", "Phone link: no Current Time Service");
  finishTime(0, 0, false);
  startBatteryDiscovery(conn);
  return 0;
}

// ---- Battery ----

void startAncsDiscovery(uint16_t conn);

int onBatteryRead(uint16_t conn, const ble_gatt_error* error, ble_gatt_attr* attr, void*) {
  uint8_t level = 0;
  if (error->status == 0 && attr && OS_MBUF_PKTLEN(attr->om) >= 1 && os_mbuf_copydata(attr->om, 0, 1, &level) == 0 &&
      level <= 100) {
    session->phoneBattery = static_cast<int8_t>(level);
    if (session->out) session->out->phoneBattery = static_cast<int8_t>(level);
  }
  startAncsDiscovery(conn);
  return 0;
}

int onBatteryChr(uint16_t conn, const ble_gatt_error* error, const ble_gatt_chr* chr, void*) {
  if (error->status == 0 && chr) {
    if (chr->uuid.u.type == BLE_UUID_TYPE_16 && chr->uuid.u16.value == BATTERY_LEVEL) {
      session->batteryLevel = chr->val_handle;
    }
    return 0;
  }
  if (session->batteryLevel && ble_gattc_read(conn, session->batteryLevel, onBatteryRead, nullptr) == 0) return 0;
  startAncsDiscovery(conn);
  return 0;
}

int onBatteryService(uint16_t conn, const ble_gatt_error* error, const ble_gatt_svc* svc, void*) {
  if (error->status == 0 && svc) {
    session->svcStart = svc->start_handle;
    session->svcEnd = svc->end_handle;
    return 0;
  }
  if (session->svcStart &&
      ble_gattc_disc_all_chrs(conn, session->svcStart, session->svcEnd, onBatteryChr, nullptr) == 0) {
    return 0;
  }
  LOG_INF("BLE", "Phone link: no Battery Service");
  startAncsDiscovery(conn);
  return 0;
}

void startBatteryDiscovery(const uint16_t conn) {
  session->svcStart = session->svcEnd = 0;
  if (ble_gattc_disc_svc_by_uuid(conn, &BATTERY_SERVICE.u, onBatteryService, nullptr) != 0) {
    startAncsDiscovery(conn);
  }
}

// ---- ANCS ----

uint16_t chrEnd(const Chr& c) {
  // A characteristic's descriptors run up to the next declaration.
  uint16_t end = session->svcEnd;
  for (const Chr* other : {&session->ancsNs, &session->ancsDs, &session->ancsCp}) {
    if (other->defHandle > c.valHandle && other->defHandle - 1 < end) end = other->defHandle - 1;
  }
  return end;
}

void pumpRequests(uint16_t conn);

int onRequestWritten(uint16_t conn, const ble_gatt_error* error, ble_gatt_attr*, void*) {
  if (error->status == 0) return 0;
  // The notification went away before we asked (ANCS error 0xA2), or the
  // phone has no name for the app.
  SessionLock lock;
  if (session->inFlightApp >= 0) {
    session->apps[session->inFlightApp].state = EntryState::Done;
  } else {
    for (int i = 0; i < session->entryCount; i++) {
      if (session->entries[i].n.uid == session->inFlightUid) session->entries[i].state = EntryState::Done;
    }
  }
  session->requestInFlight = false;
  session->dsLen = 0;
  pumpRequests(conn);
  return 0;
}

// Caller holds the session lock. One request is in flight at a time;
// notification details go first, then the names of the apps they came from.
void pumpRequests(uint16_t conn) {
  if (session->requestInFlight) return;
  // Newest first, so a long backlog still yields the latest notifications.
  for (int i = session->entryCount - 1; i >= 0; i--) {
    Entry& e = session->entries[i];
    if (e.state != EntryState::Pending) continue;
    const uint32_t uid = e.n.uid;
    const uint8_t summaryCmd[] = {ANCS_CMD_GET_NOTIFICATION_ATTRIBUTES, static_cast<uint8_t>(uid),
                                  static_cast<uint8_t>(uid >> 8),       static_cast<uint8_t>(uid >> 16),
                                  static_cast<uint8_t>(uid >> 24),      ANCS_ATTR_APP_ID};
    const uint8_t cmd[] = {ANCS_CMD_GET_NOTIFICATION_ATTRIBUTES,
                           static_cast<uint8_t>(uid),
                           static_cast<uint8_t>(uid >> 8),
                           static_cast<uint8_t>(uid >> 16),
                           static_cast<uint8_t>(uid >> 24),
                           ANCS_ATTR_APP_ID,
                           ANCS_ATTR_TITLE,
                           static_cast<uint8_t>(TITLE_LEN - 1),
                           0,
                           ANCS_ATTR_MESSAGE,
                           static_cast<uint8_t>(MESSAGE_LEN - 1),
                           0,
                           ANCS_ATTR_DATE};
    const uint8_t* request = session->summaryOnly ? summaryCmd : cmd;
    const size_t requestLen = session->summaryOnly ? sizeof(summaryCmd) : sizeof(cmd);
    if (ble_gattc_write_flat(conn, session->ancsCp.valHandle, request, requestLen, onRequestWritten, nullptr) != 0) {
      e.state = EntryState::Done;
      continue;
    }
    e.state = EntryState::InFlight;
    session->inFlightUid = uid;
    session->inFlightApp = -1;
    session->requestInFlight = true;
    session->dsLen = 0;
    return;
  }
  for (int i = 0; i < session->appCount; i++) {
    AppName& a = session->apps[i];
    if (a.state != EntryState::Pending) continue;
    uint8_t cmd[APP_ID_LEN + 2];
    const size_t idLen = strlen(a.appId);
    cmd[0] = ANCS_CMD_GET_APP_ATTRIBUTES;
    memcpy(cmd + 1, a.appId, idLen + 1);  // NUL-terminated identifier
    cmd[idLen + 2] = ANCS_APP_ATTR_DISPLAY_NAME;
    if (ble_gattc_write_flat(conn, session->ancsCp.valHandle, cmd, idLen + 3, onRequestWritten, nullptr) != 0) {
      a.state = EntryState::Done;
      continue;
    }
    a.state = EntryState::InFlight;
    session->inFlightApp = i;
    session->requestInFlight = true;
    session->dsLen = 0;
    return;
  }
}

void copyAttribute(char* dst, const size_t dstSize, const uint8_t* src, const uint16_t len) {
  const size_t n = len < dstSize - 1 ? len : dstSize - 1;
  for (size_t i = 0; i < n; i++) {
    const char c = static_cast<char>(src[i]);
    dst[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
  }
  dst[n] = '\0';
}

// ANCS dates are the phone's local time, "yyyyMMdd'T'HHmmSS".
uint32_t minutesFromAncsDate(const uint8_t* d, const uint16_t len) {
  if (len < 13 || d[8] != 'T') return 0;
  int v[6];
  const int offsets[6] = {0, 4, 6, 9, 11, 13};
  const int widths[6] = {4, 2, 2, 2, 2, 2};
  for (int f = 0; f < 5; f++) {
    v[f] = 0;
    for (int i = 0; i < widths[f]; i++) {
      const uint8_t c = d[offsets[f] + i];
      if (c < '0' || c > '9') return 0;
      v[f] = v[f] * 10 + (c - '0');
    }
  }
  if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31) return 0;
  return static_cast<uint32_t>(epochFromCivil(v[0], v[1], v[2], v[3], v[4], 0) / 60);
}

// Caller holds the session lock.
void registerApp(const char* appId) {
  if (appId[0] == '\0') return;
  for (int i = 0; i < session->appCount; i++) {
    if (strcmp(session->apps[i].appId, appId) == 0) return;
  }
  if (session->appCount == MAX_ENTRIES) return;
  AppName& a = session->apps[session->appCount++];
  memset(&a, 0, sizeof(a));
  snprintf(a.appId, sizeof(a.appId), "%s", appId);
  a.state = EntryState::Pending;
}

// Caller holds the session lock. Parses the reply to the request in flight
// once all of it has arrived (it can span several Data Source notifications).
void parseDataSource(uint16_t conn) {
  const uint8_t* b = session->dsBuf;
  const uint16_t len = session->dsLen;
  if (len == 0) return;

  if (b[0] == ANCS_CMD_GET_NOTIFICATION_ATTRIBUTES) {
    if (len < 5) return;
    uint32_t uid;
    memcpy(&uid, b + 1, sizeof(uid));
    const uint8_t* attrs[4] = {};  // app id, title, message, date
    uint16_t lens[4] = {};
    uint16_t pos = 5;
    const int requested = session->summaryOnly ? 1 : 4;
    for (int i = 0; i < requested; i++) {
      if (pos + 3 > len) return;  // wait for the next fragment
      const uint16_t attrLen = b[pos + 1] | (b[pos + 2] << 8);
      if (pos + 3 + attrLen > len) return;
      const uint8_t id = b[pos];
      const int slot = id == ANCS_ATTR_APP_ID    ? 0
                       : id == ANCS_ATTR_TITLE   ? 1
                       : id == ANCS_ATTR_MESSAGE ? 2
                       : id == ANCS_ATTR_DATE    ? 3
                                                 : -1;
      if (slot >= 0) {
        attrs[slot] = b + pos + 3;
        lens[slot] = attrLen;
      }
      pos += 3 + attrLen;
    }
    for (int i = 0; i < session->entryCount; i++) {
      Entry& e = session->entries[i];
      if (e.n.uid != uid) continue;
      copyAttribute(e.appId, sizeof(e.appId), attrs[0], lens[0]);
      copyAttribute(e.n.title, sizeof(e.n.title), attrs[1], lens[1]);
      copyAttribute(e.n.message, sizeof(e.n.message), attrs[2], lens[2]);
      copyAttribute(e.date, sizeof(e.date), attrs[3], lens[3]);
      e.n.arrivedMinutes = attrs[3] ? minutesFromAncsDate(attrs[3], lens[3]) : 0;
      e.state = EntryState::Done;
      registerApp(e.appId);
    }
  } else if (b[0] == ANCS_CMD_GET_APP_ATTRIBUTES) {
    const auto* nul = static_cast<const uint8_t*>(memchr(b + 1, 0, len - 1));
    if (!nul) return;
    const uint16_t pos = static_cast<uint16_t>(nul - b + 1);
    if (pos + 3 > len) return;
    const uint16_t attrLen = b[pos + 1] | (b[pos + 2] << 8);
    if (pos + 3 + attrLen > len) return;
    const int idx = session->inFlightApp;
    if (idx >= 0 && strcmp(session->apps[idx].appId, reinterpret_cast<const char*>(b + 1)) == 0) {
      copyAttribute(session->apps[idx].name, sizeof(session->apps[idx].name), b + pos + 3, attrLen);
      session->apps[idx].state = EntryState::Done;
    }
  }
  session->dsLen = 0;
  session->requestInFlight = false;
  session->lastEventMs = millis();
  pumpRequests(conn);
}

void onNotificationSource(uint16_t conn, const uint8_t* ns) {
  const uint8_t event = ns[0];
  const uint8_t category = ns[2];
  uint32_t uid;
  memcpy(&uid, ns + 4, sizeof(uid));
  SessionLock lock;
  session->lastEventMs = millis();
  if (category < 16) session->categoryCounts[category] = ns[3];
  int found = -1;
  for (int i = 0; i < session->entryCount; i++) {
    if (session->entries[i].n.uid == uid) found = i;
  }
  if (event == ANCS_EVENT_REMOVED && found >= 0) {
    memmove(&session->entries[found], &session->entries[found + 1], sizeof(Entry) * (session->entryCount - found - 1));
    session->entryCount--;
  } else if (event == ANCS_EVENT_ADDED && found < 0 && category < 16 &&
             (session->categoryMask & (1u << category)) != 0) {
    if (session->entryCount == session->trackLimit) {
      // Keep the newest: drop the oldest candidate.
      memmove(&session->entries[0], &session->entries[1], sizeof(Entry) * (session->trackLimit - 1));
      session->entryCount--;
    }
    Entry& e = session->entries[session->entryCount++];
    memset(&e, 0, sizeof(e));
    e.n.uid = uid;
    e.n.category = category;
    e.state = EntryState::Pending;
  }
  pumpRequests(conn);
}

int onSubscribed(uint16_t conn, const ble_gatt_error* error, ble_gatt_attr*, void* arg);

void subscribe(uint16_t conn, const uint16_t cccd, const uintptr_t step) {
  static const uint8_t enable[2] = {0x01, 0x00};
  if (!cccd || ble_gattc_write_flat(conn, cccd, enable, sizeof(enable), onSubscribed, reinterpret_cast<void*>(step))) {
    ancsUnavailable("ANCS subscribe failed");
  }
}

int onSubscribed(uint16_t conn, const ble_gatt_error* error, ble_gatt_attr*, void* arg) {
  if (error->status != 0) {
    LOG_ERR("BLE", "Phone link: ANCS subscribe status %d (notifications not allowed?)", error->status);
    ancsUnavailable("ANCS subscribe refused");
    return 0;
  }
  // Apple asks for the Data Source subscription before the Notification Source one.
  if (reinterpret_cast<uintptr_t>(arg) == 0) {
    subscribe(conn, session->nsCccd, 1);
    return 0;
  }
  session->lastEventMs = millis();
  session->subscribed = true;
  if (session->mode == Mode::Pair) session->pairState = PairState::Paired;
  return 0;
}

int onNsDsc(uint16_t conn, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* dsc, void*) {
  if (error->status == 0 && dsc) {
    if (dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == CCCD_UUID) session->nsCccd = dsc->handle;
    return 0;
  }
  subscribe(conn, session->dsCccd, 0);
  return 0;
}

int onDsDsc(uint16_t conn, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* dsc, void*) {
  if (error->status == 0 && dsc) {
    if (dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == CCCD_UUID) session->dsCccd = dsc->handle;
    return 0;
  }
  if (ble_gattc_disc_all_dscs(conn, session->ancsNs.valHandle, chrEnd(session->ancsNs), onNsDsc, nullptr) != 0) {
    ancsUnavailable("ANCS descriptor discovery failed");
  }
  return 0;
}

int onAncsChr(uint16_t conn, const ble_gatt_error* error, const ble_gatt_chr* chr, void*) {
  if (error->status == 0 && chr) {
    const Chr c{chr->def_handle, chr->val_handle};
    if (ble_uuid_cmp(&chr->uuid.u, &ANCS_NOTIFICATION_SOURCE.u) == 0) session->ancsNs = c;
    if (ble_uuid_cmp(&chr->uuid.u, &ANCS_DATA_SOURCE.u) == 0) session->ancsDs = c;
    if (ble_uuid_cmp(&chr->uuid.u, &ANCS_CONTROL_POINT.u) == 0) session->ancsCp = c;
    return 0;
  }
  if (!session->ancsNs.valHandle || !session->ancsDs.valHandle || !session->ancsCp.valHandle ||
      ble_gattc_disc_all_dscs(conn, session->ancsDs.valHandle, chrEnd(session->ancsDs), onDsDsc, nullptr) != 0) {
    ancsUnavailable("ANCS characteristics missing");
  }
  return 0;
}

int onAncsService(uint16_t conn, const ble_gatt_error* error, const ble_gatt_svc* svc, void*) {
  if (error->status == 0 && svc) {
    session->svcStart = svc->start_handle;
    session->svcEnd = svc->end_handle;
    return 0;
  }
  if (!session->svcStart ||
      ble_gattc_disc_all_chrs(conn, session->svcStart, session->svcEnd, onAncsChr, nullptr) != 0) {
    ancsUnavailable("ANCS not found");
  }
  return 0;
}

void startAncsDiscovery(const uint16_t conn) {
  session->svcStart = session->svcEnd = 0;
  if (ble_gattc_disc_svc_by_uuid(conn, &ANCS_SERVICE.u, onAncsService, nullptr) != 0) {
    ancsUnavailable("ANCS discovery failed");
  }
}

// ---- GAP ----

int onGapEvent(ble_gap_event* event, void*);

void advertise() {
  // Soliciting ANCS lists the device in the iPhone's Bluetooth settings.
  uint8_t adv[3 + 2 + 16];
  adv[0] = 2;
  adv[1] = BLE_HS_ADV_TYPE_FLAGS;
  adv[2] = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  adv[3] = 17;
  adv[4] = 0x15;  // List of 128-bit Service Solicitation UUIDs
  memcpy(adv + 5, ANCS_SERVICE.value, 16);
  ble_gap_adv_set_data(adv, sizeof(adv));

  ble_hs_adv_fields rsp{};
  rsp.name = reinterpret_cast<const uint8_t*>(DEVICE_NAME);
  rsp.name_len = sizeof(DEVICE_NAME) - 1;
  rsp.name_is_complete = 1;
  ble_gap_adv_rsp_set_fields(&rsp);

  ble_gap_adv_params params{};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = BLE_GAP_ADV_ITVL_MS(20);
  params.itvl_max = BLE_GAP_ADV_ITVL_MS(30);
  const int rc = ble_gap_adv_start(session->ownAddrType, nullptr, BLE_HS_FOREVER, &params, onGapEvent, nullptr);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    fail("advertising failed", rc);
    return;
  }
  if (session->mode == Mode::Pair && session->pairState == PairState::Idle) session->pairState = PairState::Advertising;
}

int onGapEvent(ble_gap_event* event, void*) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status != 0) {
        advertise();
        return 0;
      }
      session->conn = event->connect.conn_handle;
      session->connected = true;
      if (session->mode == Mode::Pair) session->pairState = PairState::Connected;
      // iOS encrypts with the stored bond, or shows its pairing prompt.
      ble_gap_security_initiate(event->connect.conn_handle);
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
      session->conn = BLE_HS_CONN_HANDLE_NONE;
      session->disconnected = true;
      session->subscribed = false;
      if (session->stopping || session->failed) return 0;
      // Let the phone come back if the link dropped.
      if (session->mode != Mode::Pair) {
        advertise();
      } else if (session->pairState != PairState::Paired) {
        session->pairState = PairState::Advertising;
        advertise();
      }
      return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
      if (event->enc_change.status != 0) {
        if (session->mode != Mode::Pair) {
          fail("encryption failed", event->enc_change.status);
        } else {
          // The phone may retry, e.g. after the pairing prompt timed out.
          setError("encryption failed (status %d)", event->enc_change.status);
        }
        return 0;
      }
      ble_gap_conn_desc desc;
      const bool bonded = ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 && desc.sec_state.bonded;
      if (!bonded) {
        fail(session->mode != Mode::Pair ? "connection from an unpaired phone" : "phone did not bond");
        return 0;
      }
      if (session->mode == Mode::Pair) {
        setPairedFlag(true);
        LOG_INF("BLE", "Phone link: iPhone bonded");
      }
      session->svcStart = session->svcEnd = 0;
      if (ble_gattc_disc_svc_by_uuid(event->enc_change.conn_handle, &CTS_SERVICE.u, onCtsService, nullptr) != 0) {
        finishTime(0, 0, false);
        startBatteryDiscovery(event->enc_change.conn_handle);
      }
      return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // The iPhone forgot us and paired again: replace the old bond.
      ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
        ble_store_util_delete_peer(&desc.peer_id_addr);
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
      os_mbuf* om = event->notify_rx.om;
      const uint16_t len = OS_MBUF_PKTLEN(om);
      if (event->notify_rx.attr_handle == session->ancsNs.valHandle && len >= 8) {
        uint8_t ns[8];
        if (os_mbuf_copydata(om, 0, sizeof(ns), ns) == 0) onNotificationSource(event->notify_rx.conn_handle, ns);
      } else if (event->notify_rx.attr_handle == session->ancsDs.valHandle) {
        SessionLock lock;
        if (session->dsLen + len > sizeof(session->dsBuf)) {
          session->dsLen = 0;  // oversized reply; drop it and move on
          session->requestInFlight = false;
          pumpRequests(event->notify_rx.conn_handle);
          return 0;
        }
        os_mbuf_copydata(om, 0, len, session->dsBuf + session->dsLen);
        session->dsLen += len;
        parseDataSource(event->notify_rx.conn_handle);
      }
      return 0;
    }

    default:
      return 0;
  }
}

void onReset(int reason) { LOG_ERR("BLE", "Phone link: host reset, reason %d", reason); }

void onSync() {
  ble_hs_util_ensure_addr(0);
  const int rc = ble_hs_id_infer_auto(0, &session->ownAddrType);
  if (rc != 0) {
    fail("no BLE address", rc);
    return;
  }
  advertise();
}

void hostTaskMain(void*) {
  nimble_port_run();  // returns after nimble_port_stop()
  nimble_port_freertos_deinit();
}

bool begin(const Mode mode, SyncResult* out) {
  if (session) {
    setError("session already running");
    return false;
  }
  lastErrorText[0] = '\0';
  if (!bleMemoryKept) {
    setError("BLE memory was released at boot; restart needed");
    return false;
  }
  session = new (std::nothrow) Session();
  if (!session) {
    setError("out of memory for session (free heap %u)", static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }
  // Recursive: a failed GATT write can call back into the lock holder.
  session->lock = xSemaphoreCreateRecursiveMutex();
  if (!session->lock) {
    setError("could not create session lock");
    delete session;
    session = nullptr;
    return false;
  }
  session->mode = mode;
  session->out = out;
  session->conn = BLE_HS_CONN_HANDLE_NONE;
  session->pairState = PairState::Idle;
  session->phoneBattery = -1;
  session->inFlightApp = -1;
  // The live screen always shows full text.
  session->summaryOnly = mode == Mode::Sync && detail() == Detail::AppAndCount;
  session->trackLimit = session->summaryOnly ? MAX_TRACKED_SUMMARY : MAX_TRACKED;
  switch (filter()) {
    case Filter::Messages:
      session->categoryMask = CATEGORY_MESSAGES;
      break;
    case Filter::MessagesAndCalendar:
      session->categoryMask = CATEGORY_MESSAGES | CATEGORY_CALENDAR;
      break;
    default:
      session->categoryMask = 0xFFFF;
      break;
  }

  idfErrorLine[0] = '\0';
  previousLogger = esp_log_set_vprintf(captureIdfErrors);
  const esp_err_t err = nimble_port_init();
  esp_log_set_vprintf(previousLogger);
  if (err != ESP_OK) {
    setError("NimBLE init failed: %s (0x%x), heap free %u, largest %u", esp_err_to_name(err),
             static_cast<unsigned>(err), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    if (idfErrorLine[0] != '\0') {
      const size_t used = strlen(lastErrorText);
      snprintf(lastErrorText + used, sizeof(lastErrorText) - used, " | %s", idfErrorLine);
      LOG_ERR("BLE", "Phone link: %s", idfErrorLine);
    }
    vSemaphoreDelete(session->lock);
    delete session;
    session = nullptr;
    return false;
  }
  ble_hs_cfg.reset_cb = onReset;
  ble_hs_cfg.sync_cb = onSync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set(DEVICE_NAME);
  ble_store_config_init();
  nimble_port_freertos_init(hostTaskMain);
  return true;
}

void end() {
  if (!session) return;
  session->stopping = true;
  if (session->conn != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(session->conn, BLE_ERR_REM_USER_CONN_TERM);
    const uint32_t start = millis();
    while (session->conn != BLE_HS_CONN_HANDLE_NONE && millis() - start < DISCONNECT_WAIT_MS) delay(10);
  }
  ble_gap_adv_stop();
  nimble_port_stop();
  nimble_port_deinit();
  vSemaphoreDelete(session->lock);
  delete session;
  session = nullptr;
}

// Caller holds the session lock. Newest first by ANCS date
// ("yyyyMMdd'T'HHmmSS" sorts lexically), with app names filled in.
int collectNewest(Notification* out, const int max) {
  int count = 0;
  bool used[MAX_ENTRIES] = {};
  while (count < max) {
    int best = -1;
    for (int i = 0; i < session->entryCount; i++) {
      const Entry& e = session->entries[i];
      if (used[i] || e.state != EntryState::Done || (e.n.title[0] == '\0' && e.n.message[0] == '\0')) continue;
      if (best < 0 || strcmp(e.date, session->entries[best].date) >= 0) best = i;
    }
    if (best < 0) break;
    used[best] = true;
    const Entry& e = session->entries[best];
    out[count] = e.n;
    for (int a = 0; a < session->appCount; a++) {
      if (strcmp(session->apps[a].appId, e.appId) == 0) {
        snprintf(out[count].app, sizeof(out[count].app), "%s", session->apps[a].name);
      }
    }
    count++;
  }
  return count;
}

// Caller holds the session lock. Groups the notifications by app, most first.
void collectSummary(SyncResult& out) {
  out.summary = true;
  out.appCountN = 0;
  int tracked = 0;
  for (int i = 0; i < session->entryCount; i++) {
    const Entry& e = session->entries[i];
    if (e.state != EntryState::Done) continue;
    tracked++;
    const char* name = "";
    for (int a = 0; a < session->appCount; a++) {
      if (strcmp(session->apps[a].appId, e.appId) == 0) name = session->apps[a].name;
    }
    int slot = -1;
    for (int k = 0; k < out.appCountN; k++) {
      if (strcmp(out.appCounts[k].app, name) == 0) slot = k;
    }
    if (slot < 0) {
      if (out.appCountN == MAX_APP_COUNTS) continue;  // counted in the total only
      slot = out.appCountN++;
      snprintf(out.appCounts[slot].app, sizeof(out.appCounts[slot].app), "%s", name);
      out.appCounts[slot].count = 0;
    }
    if (out.appCounts[slot].count < UINT8_MAX) out.appCounts[slot].count++;
  }
  // Stable sort, most notifications first.
  for (int i = 1; i < out.appCountN; i++) {
    const AppCount v = out.appCounts[i];
    int j = i - 1;
    while (j >= 0 && out.appCounts[j].count < v.count) {
      out.appCounts[j + 1] = out.appCounts[j];
      j--;
    }
    out.appCounts[j + 1] = v;
  }
  // The phone's per-category counts cover notifications beyond those tracked.
  int total = 0;
  for (int c = 0; c < 16; c++) {
    if (session->categoryMask & (1u << c)) total += session->categoryCounts[c];
  }
  out.total = static_cast<uint16_t>(total > tracked ? total : tracked);
}

// Caller holds the session lock.
bool requestsPending() {
  if (session->requestInFlight) return true;
  for (int i = 0; i < session->entryCount; i++) {
    if (session->entries[i].state != EntryState::Done) return true;
  }
  for (int i = 0; i < session->appCount; i++) {
    if (session->apps[i].state != EntryState::Done) return true;
  }
  return false;
}

int onActionWritten(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void*) {
  if (error->status != 0) LOG_ERR("BLE", "Phone link: dismiss refused, status %d", error->status);
  return 0;
}

}  // namespace

bool isAvailable() { return gpio.deviceIsX3(); }

bool bluetoothReady() { return bleMemoryKept; }

void enableBluetooth() { writeFlag(NVS_ENABLED_KEY, 1); }

const char* lastError() { return lastErrorText[0] != '\0' ? lastErrorText : "no details recorded"; }

bool isPaired() { return readFlag(NVS_PAIRED_KEY) != 0; }

bool sync(SyncResult& out, const uint32_t timeoutMs) {
  out = SyncResult{};
  if (!isAvailable() || !begin(Mode::Sync, &out)) return false;

  const uint32_t start = millis();
  bool done = false;
  while (!session->failed && millis() - start < timeoutMs) {
    if (session->subscribed) {
      SessionLock lock;
      if (!requestsPending() && millis() - session->lastEventMs >= NOTIFICATIONS_SETTLE_MS) {
        if (session->summaryOnly) {
          collectSummary(out);
        } else {
          out.count = static_cast<uint8_t>(collectNewest(out.items, MAX_NOTIFICATIONS));
        }
        out.gotNotifications = true;
        done = true;
        break;
      }
    }
    delay(20);
  }
  const bool gotTime = out.gotTime;
  LOG_INF("BLE", "Phone link: sync %s in %lu ms (time %s, %u notifications, battery %d)", done ? "done" : "incomplete",
          static_cast<unsigned long>(millis() - start), gotTime ? "yes" : "no", out.count, out.phoneBattery);
  end();
  return done || gotTime;
}

bool startPairing() {
  if (!isAvailable()) return false;
  return begin(Mode::Pair, nullptr);
}

PairState pairState() { return session ? session->pairState : PairState::Idle; }

void stopPairing() { end(); }

Filter filter() {
  const uint8_t f = readFlag(NVS_FILTER_KEY);
  return f < static_cast<uint8_t>(Filter::Count) ? static_cast<Filter>(f) : Filter::All;
}

void setFilter(const Filter f) { writeFlag(NVS_FILTER_KEY, static_cast<uint8_t>(f)); }

Detail detail() {
  const uint8_t d = readFlag(NVS_DETAIL_KEY);
  return d < static_cast<uint8_t>(Detail::Count) ? static_cast<Detail>(d) : Detail::Full;
}

void setDetail(const Detail d) { writeFlag(NVS_DETAIL_KEY, static_cast<uint8_t>(d)); }

uint8_t quietHours() {
  const uint8_t q = readFlag(NVS_QUIET_KEY);
  return q < QUIET_HOURS_COUNT ? q : 0;
}

void setQuietHours(const uint8_t index) { writeFlag(NVS_QUIET_KEY, index < QUIET_HOURS_COUNT ? index : 0); }

bool startLive() {
  if (!isAvailable()) return false;
  return begin(Mode::Live, nullptr);
}

LiveState liveState() {
  if (!session || session->failed) return LiveState::Failed;
  return session->subscribed ? LiveState::Ready : LiveState::Connecting;
}

int liveSnapshot(Notification* out, const int max, int8_t& battery) {
  battery = -1;
  if (!session) return 0;
  SessionLock lock;
  battery = session->phoneBattery;
  return collectNewest(out, max);
}

bool dismiss(const uint32_t uid) {
  if (!session || session->conn == BLE_HS_CONN_HANDLE_NONE || !session->ancsCp.valHandle) return false;
  const uint8_t cmd[] = {ANCS_CMD_PERFORM_ACTION,         static_cast<uint8_t>(uid),
                         static_cast<uint8_t>(uid >> 8),  static_cast<uint8_t>(uid >> 16),
                         static_cast<uint8_t>(uid >> 24), ANCS_ACTION_NEGATIVE};
  return ble_gattc_write_flat(session->conn, session->ancsCp.valHandle, cmd, sizeof(cmd), onActionWritten, nullptr) ==
         0;
}

void stopLive() { end(); }

#endif

namespace {
struct QuietWindow {
  uint8_t start;  // first quiet hour
  uint8_t end;    // first hour syncing resumes
  const char* label;
};
constexpr QuietWindow QUIET_WINDOWS[QUIET_HOURS_COUNT] = {
    {0, 0, nullptr}, {22, 7, "22:00-07:00"}, {23, 7, "23:00-07:00"}, {0, 6, "00:00-06:00"}};
}  // namespace

const char* quietHoursLabel(const uint8_t index) {
  return index < QUIET_HOURS_COUNT ? QUIET_WINDOWS[index].label : nullptr;
}

bool inQuietHours(const uint8_t index, const int hour) {
  if (index == 0 || index >= QUIET_HOURS_COUNT) return false;
  const QuietWindow& w = QUIET_WINDOWS[index];
  return w.start < w.end ? (hour >= w.start && hour < w.end) : (hour >= w.start || hour < w.end);
}

uint32_t localMinutes(const struct tm& wallClock) {
  // Days-from-civil (Howard Hinnant), in minutes.
  int y = wallClock.tm_year + 1900;
  const unsigned m = static_cast<unsigned>(wallClock.tm_mon + 1);
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(wallClock.tm_mday) - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long days = static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
  return static_cast<uint32_t>(days * 1440L + wallClock.tm_hour * 60L + wallClock.tm_min);
}

void formatAge(char* buf, const size_t size, const uint32_t arrivedMinutes, const uint32_t nowMinutes) {
  if (arrivedMinutes == 0 || nowMinutes == 0) {
    buf[0] = '\0';
    return;
  }
  const uint32_t age = nowMinutes > arrivedMinutes ? nowMinutes - arrivedMinutes : 0;
  if (age < 1) {
    snprintf(buf, size, "%s", tr(STR_AGE_NOW));
  } else if (age < 60) {
    snprintf(buf, size, tr(STR_AGE_MINUTES), static_cast<int>(age));
  } else if (age < 48 * 60) {
    snprintf(buf, size, tr(STR_AGE_HOURS), static_cast<int>(age / 60));
  } else {
    snprintf(buf, size, tr(STR_AGE_DAYS), static_cast<int>(age / 1440));
  }
}

}  // namespace PhoneLink
