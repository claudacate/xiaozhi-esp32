#ifndef _STATE_MIRROR_H_
#define _STATE_MIRROR_H_

#include "rgb_matrix.h"
#include "canvas.h"
#include "device_state.h"
#include <esp_timer.h>
#include <functional>
#include <string>

// Owns the external matrix's idle-time content - mood, clock and canvas are
// mutually exclusive (IdleMode); a running Pomodoro/timer layers on top and
// always wins while it runs. Fortune and weather are transient: they take
// over the whole panel for a fixed duration regardless of what's showing,
// then hand back to live state (StartTransient).
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
    void SetClockEnabled(bool enabled);

    // Returns false if input is malformed or a sprite name isn't recognized.
    bool CanvasDraw(const std::string& palette, const std::string& grid);
    bool CanvasSprite(const std::string& name);
    void CanvasSetPixel(int x, int y, MatrixColor color);
    void CanvasClear();

    // Pomodoro/timer. Layered on top of idle content while running; the
    // underlying countdown keeps time regardless of what's on screen.
    void StartTimer(int minutes, const std::string& mode);
    void CancelTimer();
    bool timer_running() const { return timer_running_; }

    // Brief shake, then reveals a symbol (yes/no/maybe -> checkmark/cross/
    // question); unrecognized symbols default to question. `answer` is not
    // rendered (the matrix has no font for prose) - Xiaozhi speaks it.
    void ShowFortune(const std::string& answer, const std::string& symbol);
    // Shows a weather icon for `condition` (unrecognized -> generic cloud),
    // then scrolls the temperature.
    void ShowWeather(const std::string& condition, int temp_c);

private:
    enum class IdleMode { kDark, kMood, kClock, kCanvas };

    void OnDeviceStateChanged(DeviceState previous_state, DeviceState current_state);
    // Call after any idle-content mutation (SetMood, SetClockEnabled, Canvas*)
    // to re-render immediately with whatever's live right now.
    void RefreshDisplay();
    void ShowMoodFrame();
    void ShowClockFrame();
    void ShowTimerFrame();
    void ShowCanvasFrame();
    // Idle/boot states: timer overlay if running, else whichever IdleMode
    // says, else dark.
    void ShowRest();

    // Runs frame_fn for total_frames ticks at interval_ms (frame_fn must
    // increment animation_step_ itself and call ShowLocked), then resumes
    // whatever the live device state calls for. Shared by mood preview,
    // fortune and weather.
    void StartTransient(int total_frames, int interval_ms, std::function<void()> frame_fn);
    void ShowTransientFrame();

    static void TimerCheckCallback(void* arg);
    void OnTimerCheck();

    RgbMatrix* matrix_;
    Canvas canvas_;
    bool enabled_ = true;
    int animation_step_ = 0;

    std::string mood_;
    uint8_t mood_intensity_ = 60;
    IdleMode idle_mode_ = IdleMode::kDark;

    bool clock_scroll_active_ = false;
    std::string clock_scroll_text_;
    int clock_last_scrolled_minute_ = -1;

    esp_timer_handle_t timer_check_timer_ = nullptr;
    bool timer_running_ = false;
    int64_t timer_start_us_ = 0;
    int timer_total_seconds_ = 0;
    std::string timer_mode_;

    std::function<void()> transient_frame_fn_ = nullptr;
    int transient_frames_remaining_ = 0;
};

#endif // _STATE_MIRROR_H_
