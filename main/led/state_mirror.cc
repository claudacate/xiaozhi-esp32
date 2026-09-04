#include "state_mirror.h"
#include "device_state_event.h"
#include "application.h"
#include "settings.h"
#include "mood_effects.h"
#include "marquee.h"
#include "sprites.h"
#include "assets/lang_config.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <esp_log.h>
#include <esp_random.h>

#define TAG "StateMirror"

namespace {

// Clock: how long between the start of one scroll of the time and the next.
const int64_t kClockRepeatUs = 10 * 1000000;
// Milliseconds per scrolled column. "HH:MM" is 28 frames, so a pass takes
// about 5.6s.
const int kClockScrollIntervalMs = 200;

// Weather cycles icon -> temperature -> clock, forever. 104ms is the original
// 80ms slowed by 30% per user feedback that it went by too fast to read.
const int kWeatherFrameIntervalMs = 104;
// Icon hold, in ticks: ~3.8s.
const int kWeatherIconFrames = 37;
// The clock half scrolls at 2 ticks/column (~208ms) rather than 1, staying
// close to kClockScrollIntervalMs - a single tick per column here would be
// ~104ms, the same "too quick to catch" speed the clock was tuned away from.
const int kWeatherClockTicksPerColumn = 2;

// Sunrise alarm (SPEC.md 4.7.4/4.7.6). The ramp peaks kLightLeadMinutes BEFORE
// the set time and holds, buying the light a silent solo window - otherwise
// light and sound land together and the chime does all the waking.
const int kLightLeadMinutes = 5;
const int kHoldMinutes = 10;
// A reboot later than this past the target means the alarm was missed; fire
// within it, clear silently beyond it.
const int kCatchUpGraceMinutes = 15;
const int kSunriseFrameMs = 1000;
// Perceptual correction. WS2812 output is ~linear in PWM duty but perceived
// lightness goes as luminance^(1/3), so a linear drive ramp is front-loaded -
// half the apparent brightness arrives while the sleeper is deepest, then it
// appears to stall. ^2.2 flattens that out.
const float kRampGamma = 2.2f;
// Chroma path at UNIT intensity - deliberately separate from the envelope so
// colour and brightness stay independently tunable (SPEC.md 4.7.4).
const float kDawnFrom[3] = {1.00f, 0.00f, 0.00f};   // deep red
const float kDawnTo[3]   = {1.00f, 0.706f, 0.431f}; // warm white (255,180,110)
// One dim pixel while armed but not yet ramping: the panel is otherwise dark
// all night, and once quiet mode engages this is the only confirmation the
// alarm is set that the user can still get.
const MatrixColor kArmedDot{12, 4, 0};

const char* StateName(DeviceState state) {
    switch (state) {
        case kDeviceStateUnknown: return "unknown";
        case kDeviceStateStarting: return "starting";
        case kDeviceStateWifiConfiguring: return "wifi_configuring";
        case kDeviceStateIdle: return "idle";
        case kDeviceStateConnecting: return "connecting";
        case kDeviceStateListening: return "listening";
        case kDeviceStateSpeaking: return "speaking";
        case kDeviceStateUpgrading: return "upgrading";
        case kDeviceStateActivating: return "activating";
        case kDeviceStateAudioTesting: return "audio_testing";
        case kDeviceStateFatalError: return "fatal_error";
        default: return "invalid";
    }
}

// Green -> amber -> red as fraction (0-1, elapsed/total) increases.
MatrixColor TimerColor(float fraction) {
    const MatrixColor kGreen{0, 200, 60};
    const MatrixColor kAmber{255, 170, 0};
    const MatrixColor kRed{255, 30, 0};
    if (fraction < 0.5f) {
        float t = fraction / 0.5f;
        return {
            static_cast<uint8_t>(kGreen.red + (kAmber.red - kGreen.red) * t),
            static_cast<uint8_t>(kGreen.green + (kAmber.green - kGreen.green) * t),
            static_cast<uint8_t>(kGreen.blue + (kAmber.blue - kGreen.blue) * t),
        };
    }
    float t = (fraction - 0.5f) / 0.5f;
    return {
        static_cast<uint8_t>(kAmber.red + (kRed.red - kAmber.red) * t),
        static_cast<uint8_t>(kAmber.green + (kRed.green - kAmber.green) * t),
        static_cast<uint8_t>(kAmber.blue + (kRed.blue - kAmber.blue) * t),
    };
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

}  // namespace

StateMirror::StateMirror(RgbMatrix* matrix)
    : matrix_(matrix), canvas_(matrix->width(), matrix->height()) {
    Settings settings("matrix");
    enabled_ = settings.GetInt("mirror_enabled", 1) != 0;
    mood_ = settings.GetString("mood", "");
    mood_intensity_ = static_cast<uint8_t>(settings.GetInt("mood_intensity", 60));
    // Clock and canvas are session conveniences, not persisted preferences;
    // only mood carries across a reboot.
    idle_mode_ = mood_.empty() ? IdleMode::kDark : IdleMode::kMood;

    // Restore a pending alarm. Deliberately does NOT touch Application or
    // Board here: this runs inside the board's own constructor, so calling
    // Board::GetInstance() would recurse into a half-built object. Quiet mode
    // is re-applied by Application::Start() once the board is complete and
    // before the protocol or the boot chime - see SPEC.md 4.7.5a items 1-2.
    alarm_target_ = static_cast<time_t>(
        strtoll(settings.GetString("alarm_target", "0").c_str(), nullptr, 10));
    alarm_ramp_minutes_ = settings.GetInt("alarm_ramp", 30);
    quiet_active_ = settings.GetInt("quiet", 0) != 0;
    if (alarm_target_ != 0) {
        ESP_LOGI(TAG, "Restored sunrise alarm: target=%lld ramp=%dmin quiet=%d",
            static_cast<long long>(alarm_target_), alarm_ramp_minutes_, quiet_active_);
        idle_mode_saved_ = idle_mode_;
        idle_mode_ = IdleMode::kSunrise;
        // Re-assert the wake-light brightness override across a reboot; the
        // user's stored level (matrix/brightness) is untouched and restored by
        // ReleaseAlarm. Non-permanent, so it never reaches NVS.
        matrix_->SetBrightness(100, false);
    }

    esp_timer_create_args_t timer_check_args = {
        .callback = &StateMirror::TimerCheckCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "timer_check",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_check_args, &timer_check_timer_));

    DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
        [this](DeviceState previous_state, DeviceState current_state) {
            OnDeviceStateChanged(previous_state, current_state);
        });
}

StateMirror::~StateMirror() {
    esp_timer_stop(timer_check_timer_);
    esp_timer_delete(timer_check_timer_);
}

void StateMirror::SetEnabled(bool enabled, bool permanent) {
    ESP_LOGI(TAG, "SetEnabled(%d, permanent=%d)", enabled, permanent);
    enabled_ = enabled;
    if (permanent) {
        Settings settings("matrix", true);
        settings.SetInt("mirror_enabled", enabled ? 1 : 0);
    }
    if (!enabled_) {
        matrix_->StopAnimation();
        matrix_->Clear();
        return;
    }
    auto state = Application::GetInstance().GetDeviceState();
    OnDeviceStateChanged(state, state);
}

bool StateMirror::SetMood(const std::string& mood, uint8_t intensity, bool permanent) {
    if (!MoodEffects::IsValidMood(mood)) {
        ESP_LOGW(TAG, "SetMood: rejected unknown mood '%s'", mood.c_str());
        return false;
    }
    ESP_LOGI(TAG, "SetMood: '%s' intensity=%d", mood.c_str(), intensity);

    mood_ = mood;
    mood_intensity_ = intensity;
    idle_mode_ = IdleMode::kMood;
    if (permanent) {
        Settings settings("matrix", true);
        settings.SetString("mood", mood_);
        settings.SetInt("mood_intensity", mood_intensity_);
    }

    RefreshDisplay();
    return true;
}

void StateMirror::ClearMood(bool permanent) {
    ESP_LOGI(TAG, "ClearMood");
    mood_.clear();
    if (idle_mode_ == IdleMode::kMood) {
        idle_mode_ = IdleMode::kDark;
    }
    if (permanent) {
        Settings settings("matrix", true);
        settings.SetString("mood", "");
    }

    RefreshDisplay();
}

void StateMirror::SetClockEnabled(bool enabled, bool analogue) {
    ESP_LOGI(TAG, "SetClockEnabled(%d, analogue=%d)", enabled, analogue);
    if (enabled) {
        idle_mode_ = IdleMode::kClock;
        clock_analogue_ = analogue;
        clock_scroll_active_ = false;
        clock_last_scroll_us_ = 0;  // force an immediate scroll
    } else if (idle_mode_ == IdleMode::kClock) {
        idle_mode_ = IdleMode::kDark;
    }

    RefreshDisplay();
}

void StateMirror::CanvasFill(MatrixColor color) {
    ESP_LOGI(TAG, "CanvasFill: %02X%02X%02X", color.red, color.green, color.blue);
    canvas_.Fill(color);
    idle_mode_ = IdleMode::kCanvas;
    RefreshDisplay();
}

bool StateMirror::SetSunriseAlarm(const std::string& time_hhmm, int ramp_minutes,
                                  std::string* error) {
    // No clock means no computable release time, i.e. a permanent mute. Refuse
    // rather than arm something that cannot end (SPEC.md 4.7.7 item 3).
    if (!Application::GetInstance().has_server_time()) {
        *error = "I don't know what time it is yet - the clock hasn't synced. Try again in a moment.";
        return false;
    }

    int hh = -1, mm = -1;
    if (sscanf(time_hhmm.c_str(), "%d:%d", &hh, &mm) != 2 ||
        hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        *error = "I couldn't read '" + time_hhmm + "' as a time. Use HH:MM, like 06:30.";
        return false;
    }

    if (ramp_minutes < 5) ramp_minutes = 5;
    if (ramp_minutes > 60) ramp_minutes = 60;

    time_t now = time(nullptr);
    struct tm tm_target;
    localtime_r(&now, &tm_target);
    tm_target.tm_hour = hh;
    tm_target.tm_min = mm;
    tm_target.tm_sec = 0;
    time_t target = mktime(&tm_target);
    if (target <= now) {
        target += 24 * 60 * 60;   // already gone today -> tomorrow
    }

    // Both mean "the device will interrupt you later"; having two armed at
    // once is a bug, not a feature (SPEC.md 4.7.5a item 6).
    if (timer_running_) {
        ESP_LOGI(TAG, "Sunrise armed - cancelling the running timer/Pomodoro");
        timer_running_ = false;
    }

    alarm_target_ = target;
    alarm_ramp_minutes_ = ramp_minutes;
    alarm_sounding_ = false;
    alarm_no_clock_warned_ = false;
    if (idle_mode_ != IdleMode::kSunrise) {
        idle_mode_saved_ = idle_mode_;
    }
    idle_mode_ = IdleMode::kSunrise;
    // Counterpart to turn_off, exactly as turn_on needs: without this a prior
    // turn_off leaves the mirror disabled and the alarm silently shows nothing.
    enabled_ = true;
    ESP_LOGI(TAG, "SetSunriseAlarm: %02d:%02d in %lld s, ramp %d min, peak %d min early",
        hh, mm, static_cast<long long>(target - now), ramp_minutes, kLightLeadMinutes);

    SaveAlarm();
    EnterQuiet(true);
    UpdateCheckTimer();
    // Swap the panel to the sunrise frame BEFORE raising brightness. The
    // reverse order flashes: the previous idle animation keeps rendering at
    // its own frame rate across SaveAlarm()/EnterQuiet()'s two NVS commits, so
    // a mood set to 1% suddenly draws several frames at 100% before the
    // sunrise takes over. This way the only frames at 100% are sunrise frames.
    RefreshDisplay();
    // Drive the wake light to full for the ramp; the mA budget in ShowLocked()
    // still caps the peak frame. Non-permanent: the user's stored brightness
    // (matrix/brightness) is left intact and restored by ReleaseAlarm.
    matrix_->SetBrightness(100, false);
    return true;
}

void StateMirror::CancelSunriseAlarm() {
    if (alarm_target_ == 0 && !alarm_sounding_ && !quiet_active_) {
        return;
    }
    ReleaseAlarm("cancelled");
}

void StateMirror::SaveAlarm() {
    Settings settings("matrix", true);
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(alarm_target_));
    settings.SetString("alarm_target", buf);
    settings.SetInt("alarm_ramp", alarm_ramp_minutes_);
}

void StateMirror::EnterQuiet(bool quiet) {
    if (quiet_active_ == quiet) {
        return;
    }
    quiet_active_ = quiet;
    Application::GetInstance().SetQuietMode(quiet);
    if (quiet_cb_) {
        quiet_cb_(quiet);
    }
    Settings settings("matrix", true);
    settings.SetInt("quiet", quiet ? 1 : 0);
}

void StateMirror::ReleaseAlarm(const char* why) {
    ESP_LOGI(TAG, "Sunrise alarm released: %s", why);
    // Undo the ramp's full-brightness override, back to the user's stored level.
    // A no-op if nothing was overridden this session (e.g. missed alarm caught
    // on a fresh boot).
    Settings settings("matrix");
    matrix_->SetBrightness(
        settings.GetInt("brightness", RgbMatrix::kDefaultBrightness), false);
    alarm_target_ = 0;
    alarm_sounding_ = false;
    alarm_no_clock_warned_ = false;
    SaveAlarm();
    EnterQuiet(false);
    if (idle_mode_ == IdleMode::kSunrise) {
        idle_mode_ = idle_mode_saved_;
    }
    UpdateCheckTimer();
    RefreshDisplay();
}

void StateMirror::FireAlarm() {
    ESP_LOGI(TAG, "Sunrise alarm firing");
    alarm_sounding_ = true;
    Application::GetInstance().PlaySound(Lang::Sounds::P3_ALARM);
}

void StateMirror::EvaluateAlarm() {
    if (alarm_target_ == 0 && !alarm_sounding_) {
        return;
    }

    auto& app = Application::GetInstance();
    if (!app.has_server_time()) {
        // Hold armed but do not evaluate: an unsynced clock reads as 1970 and
        // "now >= target" would fire instantly, at full brightness, at night.
        if (!alarm_no_clock_warned_) {
            ESP_LOGW(TAG, "Sunrise alarm armed but the clock is not set; holding");
            alarm_no_clock_warned_ = true;
        }
        return;
    }

    time_t now = time(nullptr);

    if (alarm_sounding_) {
        if (now >= alarm_target_ + kHoldMinutes * 60) {
            ReleaseAlarm("hold expired");
            return;
        }
        // Re-arm on idle rather than on a period: PlaySound queues, so a fixed
        // interval would grow the decode queue without bound.
        if (app.GetAudioService().IsIdle()) {
            app.PlaySound(Lang::Sounds::P3_ALARM);
        }
        return;
    }

    if (now >= alarm_target_) {
        if (now > alarm_target_ + kCatchUpGraceMinutes * 60) {
            // Missed - most likely the device was off. Clear it rather than
            // firing hours late, and log it: a silently missed alarm is the
            // thing you most want to find afterwards.
            ESP_LOGW(TAG, "Sunrise alarm missed by %lld s - clearing without firing",
                static_cast<long long>(now - alarm_target_));
            ReleaseAlarm("missed");
        } else {
            FireAlarm();
        }
    }
}

void StateMirror::ShowSunriseFrame() {
    matrix_->FillLocked(MatrixColor());

    time_t now = time(nullptr);
    time_t peak = alarm_target_ - kLightLeadMinutes * 60;
    time_t start = peak - static_cast<time_t>(alarm_ramp_minutes_) * 60;

    if (alarm_target_ == 0 && !alarm_sounding_) {
        // Shouldn't happen (ReleaseAlarm restores the previous idle mode), but
        // fail dark rather than lighting the whole panel warm white, which is
        // both wrong and the power worst case.
        matrix_->ShowLocked();
        return;
    }

    float f;
    if (alarm_sounding_ || now >= peak) {
        f = 1.0f;
    } else if (now <= start) {
        f = 0.0f;
    } else {
        f = static_cast<float>(now - start) / static_cast<float>(peak - start);
    }

    if (f <= 0.0f) {
        // Armed, ramp not started: one dim pixel, bottom-left.
        matrix_->SetPixelLocked(0, matrix_->height() - 1, kArmedDot);
        matrix_->ShowLocked();
        return;
    }

    float env = powf(f, kRampGamma);
    float r = kDawnFrom[0] + (kDawnTo[0] - kDawnFrom[0]) * f;
    float g = kDawnFrom[1] + (kDawnTo[1] - kDawnFrom[1]) * f;
    float b = kDawnFrom[2] + (kDawnTo[2] - kDawnFrom[2]) * f;

    int width = matrix_->width();
    int height = matrix_->height();
    for (int y = 0; y < height; y++) {
        // Bottom-up horizon: the lowest row lights first. Also halves average
        // current through the first half of the ramp.
        float row = f * height - static_cast<float>(height - 1 - y);
        if (row <= 0.0f) continue;
        if (row > 1.0f) row = 1.0f;
        float v = env * row * 255.0f;
        MatrixColor color{
            static_cast<uint8_t>(r * v),
            static_cast<uint8_t>(g * v),
            static_cast<uint8_t>(b * v)};
        for (int x = 0; x < width; x++) {
            matrix_->SetPixelLocked(x, y, color);
        }
    }
    matrix_->ShowLocked();
}

void StateMirror::RefreshDisplay() {
    auto state = Application::GetInstance().GetDeviceState();
    OnDeviceStateChanged(state, state);
}

void StateMirror::StartTimer(int minutes, const std::string& mode) {
    ESP_LOGI(TAG, "StartTimer: %d minutes, mode '%s'", minutes, mode.c_str());
    timer_total_seconds_ = minutes * 60;
    timer_start_us_ = esp_timer_get_time();
    timer_mode_ = mode;
    timer_running_ = true;
    UpdateCheckTimer();

    auto state = Application::GetInstance().GetDeviceState();
    OnDeviceStateChanged(state, state);
}

void StateMirror::CancelTimer() {
    if (!timer_running_) {
        return;
    }
    ESP_LOGI(TAG, "CancelTimer");
    timer_running_ = false;
    UpdateCheckTimer();

    auto state = Application::GetInstance().GetDeviceState();
    OnDeviceStateChanged(state, state);
}

void StateMirror::TimerCheckCallback(void* arg) {
    static_cast<StateMirror*>(arg)->OnTimerCheck();
}

void StateMirror::OnTimerCheck() {
    if (timer_running_) {
        int64_t elapsed_us = esp_timer_get_time() - timer_start_us_;
        if (elapsed_us >= static_cast<int64_t>(timer_total_seconds_) * 1000000) {
            ESP_LOGI(TAG, "Timer complete");
            timer_running_ = false;
            UpdateCheckTimer();
            const char* message = timer_mode_ == "pomodoro" ? "Pomodoro session complete!" : "Timer's up!";
            // Same clip as the sunrise - one alarm sound for the device.
            Application::GetInstance().Alert("Timer", message, "happy", Lang::Sounds::P3_ALARM);

            ShowAlarmFlash();
        }
    }

    // The countdown flash repeats the clip for its own duration only; it is
    // not under quiet mode, so an unattended forever-alarm would be a nuisance
    // rather than a safeguard. Re-armed on IsIdle() because PlaySound QUEUES
    // rather than restarts - a fixed interval would grow the decode queue.
    if (transient_frames_remaining_ > 0 && alarm_flash_active_ &&
        Application::GetInstance().GetAudioService().IsIdle()) {
        Application::GetInstance().PlaySound(Lang::Sounds::P3_ALARM);
    }

    EvaluateAlarm();
}

void StateMirror::UpdateCheckTimer() {
    bool want = timer_running_ || alarm_target_ != 0 || alarm_sounding_;
    esp_timer_stop(timer_check_timer_);
    if (want) {
        esp_timer_start_periodic(timer_check_timer_, 1000000);
    }
}

void StateMirror::ShowAlarmFlash() {
    const MatrixColor kRedHalf{128, 0, 0};
    const MatrixColor kBlueHalf{0, 0, 128};
    const int kFlashIntervalMs = 250;
    const int kFlashFrames = 10000 / kFlashIntervalMs;  // ~10s, or until a real state change (e.g. wake word) preempts it

    alarm_flash_active_ = true;
    StartTransient(kFlashFrames, kFlashIntervalMs, [this, kRedHalf, kBlueHalf]() {
        matrix_->FillLocked(animation_step_ % 2 == 0 ? kRedHalf : kBlueHalf);
        matrix_->ShowLocked();
        animation_step_++;
    });
}

void StateMirror::ShowCancelAck() {
    const MatrixColor kAck{60, 60, 60};
    StartTransient(6, 120, [this, kAck]() {
        matrix_->FillLocked(animation_step_ % 2 == 0 ? kAck : MatrixColor());
        matrix_->ShowLocked();
        animation_step_++;
    });
}

void StateMirror::ShowWeather(const std::string& condition, int temp_c) {
    ESP_LOGI(TAG, "ShowWeather: condition='%s' temp=%dC", condition.c_str(), temp_c);

    std::string lower = ToLower(condition);
    std::string sprite_name = "cloudy";  // generic fallback for unrecognized conditions
    if (lower.find("sun") != std::string::npos || lower.find("clear") != std::string::npos) {
        sprite_name = "sunny";
    } else if (lower.find("snow") != std::string::npos) {
        sprite_name = "snowy";
    } else if (lower.find("storm") != std::string::npos || lower.find("thunder") != std::string::npos ||
               lower.find("lightning") != std::string::npos) {
        sprite_name = "thunderstorm";
    } else if (lower.find("rain") != std::string::npos || lower.find("drizzle") != std::string::npos) {
        sprite_name = "rainy";
    }
    weather_sprite_ = Sprites::Find(sprite_name);

    char buf[8];
    snprintf(buf, sizeof(buf), "%dC", temp_c);
    weather_temp_text_ = buf;
    weather_clock_text_ = CurrentClockText();

    animation_step_ = 0;
    idle_mode_ = IdleMode::kWeather;
    RefreshDisplay();
}

void StateMirror::StartTransient(int total_frames, int interval_ms, std::function<void()> frame_fn) {
    ESP_LOGI(TAG, "StartTransient: %d frames @ %dms", total_frames, interval_ms);
    animation_step_ = 0;
    transient_frames_remaining_ = total_frames;
    transient_frame_fn_ = frame_fn;
    matrix_->StartAnimation(interval_ms, [this]() { ShowTransientFrame(); });
}

void StateMirror::ShowTransientFrame() {
    if (transient_frames_remaining_ > 0) {
        transient_frame_fn_();
        transient_frames_remaining_--;
        return;
    }
    alarm_flash_active_ = false;
    ESP_LOGI(TAG, "Transient done, resuming live state");
    auto state = Application::GetInstance().GetDeviceState();
    OnDeviceStateChanged(state, state);
}

void StateMirror::OnDeviceStateChanged(DeviceState previous_state, DeviceState current_state) {
    ESP_LOGI(TAG, "Device state: %s -> %s (mirror %s, mood '%s')",
        StateName(previous_state), StateName(current_state),
        enabled_ ? "enabled" : "disabled", mood_.c_str());

    if (!enabled_) {
        return;
    }

    // Idle content (mood/clock/canvas/timer) shows regardless of what the
    // assistant is doing - it no longer reacts to listening/thinking/
    // speaking/error, see the class comment in state_mirror.h.
    animation_step_ = 0;
    ShowRest();
}

void StateMirror::ShowRest() {
    // Sunrise outranks the countdown overlay: a forgotten Pomodoro must not
    // cover the alarm.
    if (idle_mode_ == IdleMode::kSunrise) {
        ESP_LOGI(TAG, "-> rest, sunrise alarm active");
        matrix_->StartAnimation(kSunriseFrameMs, [this]() { ShowSunriseFrame(); });
        return;
    }
    if (timer_running_) {
        ESP_LOGI(TAG, "-> rest, timer/pomodoro active");
        matrix_->StartAnimation(200, [this]() { ShowTimerFrame(); });
        return;
    }

    switch (idle_mode_) {
        case IdleMode::kMood:
            ESP_LOGI(TAG, "-> rest, mood '%s' active", mood_.c_str());
            matrix_->StartAnimation(100, [this]() { ShowMoodFrame(); });
            break;
        case IdleMode::kClock:
            if (clock_analogue_) {
                ESP_LOGI(TAG, "-> rest, analogue clock active");
                matrix_->StartAnimation(1000, [this]() { ShowAnalogueClockFrame(); });
            } else {
                ESP_LOGI(TAG, "-> rest, clock active");
                matrix_->StartAnimation(kClockScrollIntervalMs, [this]() { ShowClockFrame(); });
            }
            break;
        case IdleMode::kCanvas:
            ESP_LOGI(TAG, "-> rest, canvas active");
            matrix_->StartAnimation(500, [this]() { ShowCanvasFrame(); });
            break;
        case IdleMode::kWeather:
            ESP_LOGI(TAG, "-> rest, weather active");
            matrix_->StartAnimation(kWeatherFrameIntervalMs, [this]() { ShowWeatherFrame(); });
            break;
        case IdleMode::kDark:
        default:
            ESP_LOGI(TAG, "-> rest, dark");
            matrix_->StopAnimation();
            matrix_->Clear();
            break;
    }
}

void StateMirror::ShowMoodFrame() {
    if (!MoodEffects::RenderFrame(matrix_, mood_, animation_step_, mood_intensity_)) {
        // Shouldn't happen (SetMood validates), but fail safe rather than spin
        // on an unrecognized name.
        matrix_->StopAnimation();
        matrix_->Clear();
        return;
    }
    animation_step_++;
}

std::string StateMirror::CurrentClockText() const {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    return buf;
}

// One cycle is icon -> temperature -> clock; it then restarts, so weather
// stays up until other idle content replaces it.
void StateMirror::ShowWeatherFrame() {
    const MatrixColor kTempColor{255, 255, 255};
    const MatrixColor kClockColor{0, 220, 255};

    int temp_cols = Marquee::FrameCount(matrix_, weather_temp_text_);
    int clock_ticks = Marquee::FrameCount(matrix_, weather_clock_text_) * kWeatherClockTicksPerColumn;
    int icon_end = kWeatherIconFrames;
    int temp_end = icon_end + temp_cols;
    int cycle_ticks = temp_end + clock_ticks;

    int step = animation_step_ % cycle_ticks;
    if (step == 0) {
        // Re-read at the top of each cycle so the displayed time stays live.
        weather_clock_text_ = CurrentClockText();
    }

    if (step < icon_end) {
        Sprites::RenderLocked(matrix_, weather_sprite_);
    } else if (step < temp_end) {
        Marquee::RenderFrame(matrix_, weather_temp_text_, step - icon_end, kTempColor);
    } else {
        Marquee::RenderFrame(matrix_, weather_clock_text_,
                             (step - temp_end) / kWeatherClockTicksPerColumn, kClockColor);
    }
    matrix_->ShowLocked();
    animation_step_++;
}

void StateMirror::ShowClockFrame() {
    const MatrixColor kClockColor{0, 220, 255};

    if (clock_scroll_active_) {
        Marquee::RenderFrame(matrix_, clock_scroll_text_, animation_step_, kClockColor);
        matrix_->ShowLocked();
        animation_step_++;
        if (animation_step_ >= Marquee::FrameCount(matrix_, clock_scroll_text_)) {
            clock_scroll_active_ = false;
        }
        return;
    }

    // Repeat on a fixed interval rather than once per minute: a single
    // pass per minute was too easy to miss. Measured from the start of the
    // previous scroll, so the period is the full cycle.
    int64_t now_us = esp_timer_get_time();
    if (clock_last_scroll_us_ == 0 || now_us - clock_last_scroll_us_ >= kClockRepeatUs) {
        clock_scroll_text_ = CurrentClockText();
        clock_scroll_active_ = true;
        clock_last_scroll_us_ = now_us;
        animation_step_ = 0;
        Marquee::RenderFrame(matrix_, clock_scroll_text_, animation_step_, kClockColor);
        matrix_->ShowLocked();
        animation_step_++;
        return;
    }

    matrix_->FillLocked(MatrixColor());
    matrix_->ShowLocked();
}

void StateMirror::ShowAnalogueClockFrame() {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    const float kTwoPi = 6.28318530718f;
    const float kCenter = (matrix_->width() - 1) / 2.0f;
    const MatrixColor kHourColor{255, 170, 0};
    const MatrixColor kMinuteColor{0, 220, 255};

    auto draw_hand = [this, kCenter](float angle, float length, MatrixColor color) {
        for (float d = 1.0f; d <= length; d += 1.0f) {
            int x = static_cast<int>(std::lround(kCenter + std::sin(angle) * d));
            int y = static_cast<int>(std::lround(kCenter - std::cos(angle) * d));
            matrix_->SetPixelLocked(x, y, color);
        }
    };

    float minute_frac = tm_now.tm_min / 60.0f;
    float hour_angle = ((tm_now.tm_hour % 12) + minute_frac) / 12.0f * kTwoPi;
    float minute_angle = minute_frac * kTwoPi;

    matrix_->FillLocked(MatrixColor());
    draw_hand(hour_angle, 2.0f, kHourColor);
    draw_hand(minute_angle, 3.5f, kMinuteColor);
    matrix_->ShowLocked();
}

void StateMirror::ShowTimerFrame() {
    int64_t elapsed_us = esp_timer_get_time() - timer_start_us_;
    float fraction = 1.0f;
    if (timer_total_seconds_ > 0) {
        fraction = static_cast<float>(elapsed_us) / (static_cast<float>(timer_total_seconds_) * 1000000.0f);
        fraction = std::min(1.0f, std::max(0.0f, fraction));
    }

    int width = matrix_->width();
    int height = matrix_->height();
    int lit = static_cast<int>(fraction * width * height);
    MatrixColor color = TimerColor(fraction);

    matrix_->FillLocked(MatrixColor());
    for (int i = 0; i < lit; i++) {
        matrix_->SetPixelLocked(i % width, i / width, color);
    }
    matrix_->ShowLocked();
}

void StateMirror::ShowCanvasFrame() {
    canvas_.RenderLocked(matrix_);
    matrix_->ShowLocked();
}
