#ifndef _STATE_MIRROR_H_
#define _STATE_MIRROR_H_

#include "rgb_matrix.h"
#include "canvas.h"
#include "device_state.h"
#include <esp_timer.h>
#include <functional>
#include <string>

// Owns the external matrix's idle-time content - mood, clock, canvas and
// weather are mutually exclusive (IdleMode); a running Pomodoro/timer layers
// on top and always wins while it runs. Fortune is transient: it takes over
// the whole panel for a fixed duration regardless of what's showing, then
// hands back to live state (StartTransient).
//
// Hooks DeviceStateEventManager so idle content responds to the device
// actually going idle, but does NOT animate in response to listening/
// thinking/speaking/error - that reaction was removed per user feedback
// (it interrupted moods and read as the panel "freezing"); see DECISIONS.md.
// SetEnabled is a master on/off for everything this class shows.
//
// This hand-rolled priority resolution - rather than a fully generic stack -
// is deliberate: it covers exactly the features that exist, see SPEC.md 3 for
// why a general mechanism was deferred.
class StateMirror {
public:
    explicit StateMirror(RgbMatrix* matrix);
    ~StateMirror();

    void SetEnabled(bool enabled, bool permanent = false);
    bool enabled() const { return enabled_; }

    // Idle content - mutually exclusive. Setting one replaces whichever was
    // showing before; clearing one goes dark rather than restoring a prior one.
    bool SetMood(const std::string& mood, uint8_t intensity, bool permanent = true);
    void ClearMood(bool permanent = true);
    // analogue: false draws the scrolling HH:MM digital clock (default),
    // true draws an hour/minute hand face instead.
    void SetClockEnabled(bool enabled, bool analogue = false);

    // Whole panel one solid colour, as idle content (replaces mood/clock).
    // The only surviving canvas entry point - lamp mode (SPEC.md 4.0).
    void CanvasFill(MatrixColor color);

    // Sunrise alarm (SPEC.md 4.7). `time_hhmm` is local "HH:MM"; the alarm
    // targets the next occurrence of it. The ramp PEAKS kLightLeadMinutes
    // before that time and holds, so the light gets a silent solo window
    // before the clip sounds at the time itself. Arming engages quiet mode -
    // the device stops answering until the alarm finishes.
    // Returns false and fills *error if the clock isn't set yet or HH:MM is
    // malformed; both are reported to the user rather than guessed at.
    bool SetSunriseAlarm(const std::string& time_hhmm, int ramp_minutes, std::string* error);
    // Cancels an armed or sounding alarm and lifts quiet mode, from any state.
    // The BOOT long-press path - and the only cancel route there is, since
    // quiet mode makes a voice cancel unreachable by construction.
    void CancelSunriseAlarm();
    bool quiet_active() const { return quiet_active_; }
    // Brief visible flash confirming a long-press cancel. Needed because the
    // cancel happens while the device is muted and the OLED is off, so this is
    // the only feedback available.
    void ShowCancelAck();
    // Board hook, called when quiet mode engages/lifts. The board blanks its
    // own display from here; the panel handle lives there, not in this class.
    void SetQuietModeCallback(std::function<void(bool)> cb) { quiet_cb_ = std::move(cb); }

    // Pomodoro/timer. Layered on top of idle content while running; the
    // underlying countdown keeps time regardless of what's on screen.
    void StartTimer(int minutes, const std::string& mode);
    void CancelTimer();
    bool timer_running() const { return timer_running_; }

    // Idle content: cycles weather icon (unrecognized condition -> generic
    // cloud) -> scrolling temperature -> scrolling clock, repeating until
    // replaced by other idle content. The temperature is a snapshot - only
    // the LLM can refresh it - but the clock is re-read every cycle.
    void ShowWeather(const std::string& condition, int temp_c);

private:
    enum class IdleMode { kDark, kMood, kClock, kCanvas, kWeather, kSunrise };

    void OnDeviceStateChanged(DeviceState previous_state, DeviceState current_state);
    // Call after any idle-content mutation (SetMood, SetClockEnabled, Canvas*)
    // to re-render immediately with whatever's live right now.
    void RefreshDisplay();
    void ShowMoodFrame();
    // Reads the wall clock every frame rather than tracking elapsed ticks, so
    // a reboot mid-ramp resumes at the correct point on the curve for free.
    void ShowSunriseFrame();
    void EvaluateAlarm();
    void FireAlarm();
    void ReleaseAlarm(const char* why);
    void EnterQuiet(bool quiet);
    void SaveAlarm();
    // The 1 Hz check is shared by the countdown and the alarm; it runs while
    // either needs it and stops when neither does.
    void UpdateCheckTimer();
    void ShowClockFrame();
    void ShowAnalogueClockFrame();
    void ShowWeatherFrame();
    void ShowTimerFrame();
    void ShowCanvasFrame();
    // "HH:MM" for right now, shared by the clock and weather modes.
    std::string CurrentClockText() const;
    // Idle/boot states: timer overlay if running, else whichever IdleMode
    // says, else dark.
    void ShowRest();

    // Runs frame_fn for total_frames ticks at interval_ms (frame_fn must
    // increment animation_step_ itself and call ShowLocked), then resumes
    // whatever the live device state calls for. Shared by mood preview,
    // fortune, and the timer-completion alarm flash. Note this also means
    // any real device state change (e.g. a wake word) cuts a transient short,
    // since OnDeviceStateChanged always wins the animation slot.
    void StartTransient(int total_frames, int interval_ms, std::function<void()> frame_fn);
    void ShowTransientFrame();

    // Red/blue flash, ~10s, shown when a timer/Pomodoro completes.
    void ShowAlarmFlash();

    static void TimerCheckCallback(void* arg);
    void OnTimerCheck();

    RgbMatrix* matrix_;
    Canvas canvas_;
    bool enabled_ = true;
    int animation_step_ = 0;

    std::string mood_;
    uint8_t mood_intensity_ = 60;
    IdleMode idle_mode_ = IdleMode::kDark;

    // Weather idle content. Deliberately not persisted to NVS like mood is -
    // a temperature restored from before a reboot would be stale and wrong.
    const struct Sprite* weather_sprite_ = nullptr;
    std::string weather_temp_text_;
    std::string weather_clock_text_;

    bool clock_analogue_ = false;
    bool clock_scroll_active_ = false;
    std::string clock_scroll_text_;
    // Start of the last scroll, for the repeat interval. 0 = never scrolled,
    // which forces one immediately.
    int64_t clock_last_scroll_us_ = 0;

    // Sunrise alarm. alarm_target_ is an absolute local epoch (0 = disarmed),
    // persisted as a decimal string - Settings::SetInt is 32-bit and a Unix
    // epoch stops fitting in 2038.
    time_t alarm_target_ = 0;
    int alarm_ramp_minutes_ = 30;
    bool alarm_sounding_ = false;
    bool alarm_flash_active_ = false;
    bool quiet_active_ = false;
    bool alarm_no_clock_warned_ = false;
    IdleMode idle_mode_saved_ = IdleMode::kDark;
    std::function<void(bool)> quiet_cb_;

    esp_timer_handle_t timer_check_timer_ = nullptr;
    bool timer_running_ = false;
    int64_t timer_start_us_ = 0;
    int timer_total_seconds_ = 0;
    std::string timer_mode_;

    std::function<void()> transient_frame_fn_ = nullptr;
    int transient_frames_remaining_ = 0;
};

#endif // _STATE_MIRROR_H_
