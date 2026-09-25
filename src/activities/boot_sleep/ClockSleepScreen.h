#pragma once

#include <ctime>

#include "network/PhoneLink.h"

class GfxRenderer;

// "Clock" sleep screen: a large HH:MM face that a deep-sleep timer wake
// repaints once per minute without mounting SD or booting the UI. The face
// that is on the glass is kept in RTC memory, which survives deep sleep.
namespace ClockSleepScreen {

// X3 only: needs the RTC and a deep sleep that keeps the MCU on battery. X4
// deep sleep is a battery power-off, so a wake timer could never fire.
bool isSupported();

// Paints the current time with a clean refresh and starts a clock sleep cycle.
// Without a valid RTC time it paints "--:--" and a "not set" note instead,
// with no cycle. Returns false (nothing painted) when unsupported.
bool render(GfxRenderer& renderer);

// True while the panel shows a face painted by render() or update().
bool isActive();

// Ends the clock sleep cycle; call before painting any other sleep screen.
void deactivate();

// Restores the timezone and UI language captured by render(), for timer
// wakes that skip loading settings from SD.
void restoreLocale();

// True when `now` shows a different minute than the face on the glass.
bool needsRepaint(const struct tm& now);

// Timer-wake repaint. Requires display.begin(true) and renderer.begin().
// Rebuilds the controller baseline from the previous face and repaints only
// the change; the first paint of each hour is a clean refresh instead.
void update(GfxRenderer& renderer, const struct tm& now);

// True when this timer wake should sync with the paired iPhone. After repeated
// misses it only tries every few minutes to spare the battery.
bool phoneSyncDue(const struct tm& now);

// Applies an iPhone sync after update(): corrects the RTC and timezone from the
// phone and repaints if the notifications (or the minute) changed.
void applyPhoneSync(GfxRenderer& renderer, bool ok, const PhoneLink::SyncResult& result);

// Arms the deep-sleep wake timer for just after the next minute boundary, or
// right away if the minute already turned. Returns true when armed; the
// caller must then keep battery power through sleep.
bool armWakeTimer();

}  // namespace ClockSleepScreen
