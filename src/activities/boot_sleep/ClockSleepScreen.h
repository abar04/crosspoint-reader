#pragma once

#include <ctime>

class GfxRenderer;

// "Clock" sleep screen: a large HH:MM face that a deep-sleep timer wake
// repaints once per minute without mounting SD or booting the UI. The face
// that is on the glass is kept in RTC memory, which survives deep sleep.
namespace ClockSleepScreen {

// X3 only: needs the RTC and a deep sleep that keeps the MCU on battery. X4
// deep sleep is a battery power-off, so a wake timer could never fire.
bool isSupported();

// Paints the current time with a clean refresh and starts a clock sleep cycle.
// Returns false (nothing painted) when unsupported or the RTC can't be read.
bool render(GfxRenderer& renderer);

// True while the panel shows a face painted by render() or update().
bool isActive();

// Ends the clock sleep cycle; call before painting any other sleep screen.
void deactivate();

// Restores the timezone captured by render(), for timer wakes that skip
// loading settings from SD.
void restoreTimezone();

// True when `now` shows a different minute than the face on the glass.
bool needsRepaint(const struct tm& now);

// Timer-wake repaint. Requires display.begin(true) and renderer.begin().
// Rebuilds the controller baseline from the previous face and repaints only
// the change; the first paint of each hour is a clean refresh instead.
void update(GfxRenderer& renderer, const struct tm& now);

// Arms the deep-sleep wake timer for just after the next minute boundary, or
// right away if the minute already turned. No-op when not active.
void armWakeTimer();

}  // namespace ClockSleepScreen
