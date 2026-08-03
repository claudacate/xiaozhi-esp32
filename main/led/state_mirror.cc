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
#include <cstdio>
#include <ctime>
#include <esp_log.h>
#include <esp_random.h>

#define TAG "StateMirror"

namespace {

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

void StateMirror::SetClockEnabled(bool enabled) {
    ESP_LOGI(TAG, "SetClockEnabled(%d)", enabled);
    if (enabled) {
        idle_mode_ = IdleMode::kClock;
        clock_scroll_active_ = false;
        clock_last_scrolled_minute_ = -1;  // force an immediate scroll
    } else if (idle_mode_ == IdleMode::kClock) {
        idle_mode_ = IdleMode::kDark;
    }

    RefreshDisplay();
}

bool StateMirror::CanvasDraw(const std::string& palette, const std::string& grid) {
    if (!canvas_.Draw(palette, grid)) {
        ESP_LOGW(TAG, "CanvasDraw: malformed palette/grid");
        return false;
    }
    ESP_LOGI(TAG, "CanvasDraw: ok");
    idle_mode_ = IdleMode::kCanvas;
    RefreshDisplay();
    return true;
}

bool StateMirror::CanvasSprite(const std::string& name) {
    if (!canvas_.DrawSprite(name)) {
        ESP_LOGW(TAG, "CanvasSprite: unknown sprite '%s'", name.c_str());
        return false;
    }
    ESP_LOGI(TAG, "CanvasSprite: '%s'", name.c_str());
    idle_mode_ = IdleMode::kCanvas;
    RefreshDisplay();
    return true;
}

void StateMirror::CanvasSetPixel(int x, int y, MatrixColor color) {
    canvas_.SetPixel(x, y, color);
    idle_mode_ = IdleMode::kCanvas;
    RefreshDisplay();
}

void StateMirror::CanvasFill(MatrixColor color) {
    ESP_LOGI(TAG, "CanvasFill: %02X%02X%02X", color.red, color.green, color.blue);
    canvas_.Fill(color);
    idle_mode_ = IdleMode::kCanvas;
    RefreshDisplay();
}

void StateMirror::CanvasClear() {
    ESP_LOGI(TAG, "CanvasClear");
    canvas_.Clear();
    if (idle_mode_ == IdleMode::kCanvas) {
        idle_mode_ = IdleMode::kDark;
    }
    RefreshDisplay();
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
    esp_timer_start_periodic(timer_check_timer_, 1000000);

    auto state = Application::GetInstance().GetDeviceState();
    OnDeviceStateChanged(state, state);
}

void StateMirror::CancelTimer() {
    if (!timer_running_) {
        return;
    }
    ESP_LOGI(TAG, "CancelTimer");
    timer_running_ = false;
    esp_timer_stop(timer_check_timer_);

    auto state = Application::GetInstance().GetDeviceState();
    OnDeviceStateChanged(state, state);
}

void StateMirror::TimerCheckCallback(void* arg) {
    static_cast<StateMirror*>(arg)->OnTimerCheck();
}

void StateMirror::OnTimerCheck() {
    if (!timer_running_) {
        return;
    }
    int64_t elapsed_us = esp_timer_get_time() - timer_start_us_;
    if (elapsed_us >= static_cast<int64_t>(timer_total_seconds_) * 1000000) {
        ESP_LOGI(TAG, "Timer complete");
        timer_running_ = false;
        esp_timer_stop(timer_check_timer_);
        const char* message = timer_mode_ == "pomodoro" ? "Pomodoro session complete!" : "Timer's up!";
        Application::GetInstance().Alert("Timer", message, "happy", Lang::Sounds::P3_SUCCESS);

        auto state = Application::GetInstance().GetDeviceState();
        OnDeviceStateChanged(state, state);
    }
}

void StateMirror::ShowFortune(const std::string& answer, const std::string& symbol) {
    ESP_LOGI(TAG, "ShowFortune: answer='%s' symbol='%s'", answer.c_str(), symbol.c_str());

    std::string lower = ToLower(symbol);
    std::string sprite_name = "question";
    if (lower == "yes") {
        sprite_name = "checkmark";
    } else if (lower == "no") {
        sprite_name = "cross";
    }
    const Sprite* sprite = Sprites::Find(sprite_name);

    const int kShakeFrames = 25;   // ~2s at 80ms/tick
    const int kRevealFrames = 25;  // ~2s at 80ms/tick
    StartTransient(kShakeFrames + kRevealFrames, 80, [this, sprite, kShakeFrames]() {
        if (animation_step_ < kShakeFrames) {
            matrix_->FillLocked(MatrixColor());
            int width = matrix_->width();
            int height = matrix_->height();
            for (int i = 0; i < 6; i++) {
                uint32_t r = esp_random();
                int x = r % width;
                int y = (r / width) % height;
                matrix_->SetPixelLocked(x, y, {180, 180, 180});
            }
        } else {
            Sprites::RenderLocked(matrix_, sprite);
        }
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
    const Sprite* sprite = Sprites::Find(sprite_name);

    char buf[8];
    snprintf(buf, sizeof(buf), "%dC", temp_c);
    std::string temp_text = buf;

    const int kIconFrames = 37;  // ~3s at 80ms/tick
    int scroll_frames = Marquee::FrameCount(matrix_, temp_text);
    StartTransient(kIconFrames + scroll_frames, 80, [this, sprite, kIconFrames, temp_text]() {
        if (animation_step_ < kIconFrames) {
            Sprites::RenderLocked(matrix_, sprite);
        } else {
            Marquee::RenderFrame(matrix_, temp_text, animation_step_ - kIconFrames, {255, 255, 255});
        }
        matrix_->ShowLocked();
        animation_step_++;
    });
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
            ESP_LOGI(TAG, "-> rest, clock active");
            matrix_->StartAnimation(100, [this]() { ShowClockFrame(); });
            break;
        case IdleMode::kCanvas:
            ESP_LOGI(TAG, "-> rest, canvas active");
            matrix_->StartAnimation(500, [this]() { ShowCanvasFrame(); });
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

void StateMirror::ShowClockFrame() {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int minute_id = tm_now.tm_hour * 60 + tm_now.tm_min;
    const MatrixColor kClockColor{0, 220, 255};

    if (clock_scroll_active_) {
        Marquee::RenderFrame(matrix_, clock_scroll_text_, animation_step_, kClockColor);
        matrix_->ShowLocked();
        animation_step_++;
        if (animation_step_ >= Marquee::FrameCount(matrix_, clock_scroll_text_)) {
            clock_scroll_active_ = false;
            clock_last_scrolled_minute_ = minute_id;
        }
        return;
    }

    if (minute_id != clock_last_scrolled_minute_) {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        clock_scroll_text_ = buf;
        clock_scroll_active_ = true;
        animation_step_ = 0;
        Marquee::RenderFrame(matrix_, clock_scroll_text_, animation_step_, kClockColor);
        matrix_->ShowLocked();
        animation_step_++;
        return;
    }

    matrix_->FillLocked(MatrixColor());
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
