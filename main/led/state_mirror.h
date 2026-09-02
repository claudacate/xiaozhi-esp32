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
    enum class IdleMode { kDark, kMood, kClock, kCanvas, kWeather };

    void OnDeviceStateChanged(DeviceState previous_state, DeviceState current_state);
    // Call after any idle-content mutation (SetMood, SetClockEnabled, Canvas*)
    // to re-render immediately with whatever's live right now.
    void RefreshDisplay();
    void ShowMoodFrame();
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

    esp_timer_handle_t timer_check_timer_ = nullptr;
    bool timer_running_ = false;
    int64_t timer_start_us_ = 0;
    int timer_total_seconds_ = 0;
    std::string timer_mode_;

    std::function<void()> transient_frame_fn_ = nullptr;
    int transient_frames_remaining_ = 0;
};

#endif // _STATE_MIRROR_H_
