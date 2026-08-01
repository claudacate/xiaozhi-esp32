#include "state_mirror.h"
#include "device_state_event.h"
#include "application.h"
#include "settings.h"
#include "mood_effects.h"
#include <esp_log.h>

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

// Walks the panel's outer ring, starting top-left, clockwise. Computed from
// actual dimensions rather than hardcoded for 8x8, so it stays correct if the
// panel size in config.h ever changes.
std::vector<std::pair<int, int>> BuildPerimeter(int width, int height) {
    std::vector<std::pair<int, int>> ring;
    if (width <= 0 || height <= 0) {
        return ring;
    }
    for (int x = 0; x < width; x++) {
        ring.push_back({x, 0});
    }
    for (int y = 1; y < height; y++) {
        ring.push_back({width - 1, y});
    }
    if (height > 1) {
        for (int x = width - 2; x >= 0; x--) {
            ring.push_back({x, height - 1});
        }
    }
    if (width > 1) {
        for (int y = height - 2; y >= 1; y--) {
            ring.push_back({0, y});
        }
    }
    return ring;
}

// True for the states OnDeviceStateChanged's switch below gives their own
// animation (i.e. everything that isn't the idle/rest bucket). Kept in sync
// with that switch's case list.
bool IsActiveMirrorState(DeviceState state) {
    switch (state) {
        case kDeviceStateConnecting:
        case kDeviceStateListening:
        case kDeviceStateAudioTesting:
        case kDeviceStateSpeaking:
        case kDeviceStateFatalError:
            return true;
        default:
            return false;
    }
}

MatrixColor Scale(MatrixColor color, int level_0_255) {
    return {
        static_cast<uint8_t>(color.red * level_0_255 / 255),
        static_cast<uint8_t>(color.green * level_0_255 / 255),
        static_cast<uint8_t>(color.blue * level_0_255 / 255),
    };
}

// Symmetric triangle wave, 0 to 255 and back, over 2 * half_period_steps.
int TriangleLevel(int step, int half_period_steps) {
    int phase = step % (half_period_steps * 2);
    if (phase < half_period_steps) {
        return phase * 255 / half_period_steps;
    }
    return (half_period_steps * 2 - phase) * 255 / half_period_steps;
}

// ~1.5s at the mood tick rate (100ms/frame).
constexpr int kMoodPreviewFrames = 15;

}  // namespace

StateMirror::StateMirror(RgbMatrix* matrix) : matrix_(matrix) {
    perimeter_ = BuildPerimeter(matrix_->width(), matrix_->height());

    Settings settings("matrix");
    enabled_ = settings.GetInt("mirror_enabled", 1) != 0;
    mood_ = settings.GetString("mood", "");
    mood_intensity_ = static_cast<uint8_t>(settings.GetInt("mood_intensity", 60));

    DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
        [this](DeviceState previous_state, DeviceState current_state) {
            OnDeviceStateChanged(previous_state, current_state);
        });
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
    // Re-sync to whatever the assistant is doing right now.
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
    if (permanent) {
        Settings settings("matrix", true);
        settings.SetString("mood", mood_);
        settings.SetInt("mood_intensity", mood_intensity_);
    }

    auto state = Application::GetInstance().GetDeviceState();
    if (enabled_ && IsActiveMirrorState(state)) {
        // Idle is still owned by whatever's actually happening (listening,
        // thinking...), so the mood wouldn't be seen until that ends - which
        // could be a while. Give a brief preview now so the visual lands
        // together with the voice confirmation, then hand back to the live
        // conversation state.
        ESP_LOGI(TAG, "Previewing mood '%s' during %s", mood_.c_str(), StateName(state));
        animation_step_ = 0;
        mood_preview_frames_ = kMoodPreviewFrames;
        matrix_->StartAnimation(100, [this]() { ShowMoodPreviewFrame(); });
    } else {
        OnDeviceStateChanged(state, state);
    }
    return true;
}

void StateMirror::ClearMood(bool permanent) {
    ESP_LOGI(TAG, "ClearMood");
    mood_.clear();
    if (permanent) {
        Settings settings("matrix", true);
        settings.SetString("mood", "");
    }

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

    animation_step_ = 0;
    switch (current_state) {
        case kDeviceStateConnecting:
            ESP_LOGI(TAG, "-> connecting animation (flash + comet)");
            matrix_->StartAnimation(80, [this]() { ShowConnectingFrame(); });
            break;
        case kDeviceStateListening:
        case kDeviceStateAudioTesting:
            ESP_LOGI(TAG, "-> listening animation (breathing)");
            matrix_->StartAnimation(40, [this]() { ShowListeningFrame(); });
            break;
        case kDeviceStateSpeaking:
            ESP_LOGI(TAG, "-> speaking animation (pulse)");
            matrix_->StartAnimation(60, [this]() { ShowSpeakingFrame(); });
            break;
        case kDeviceStateFatalError:
            ESP_LOGI(TAG, "-> error animation (red pulse)");
            matrix_->StartAnimation(100, [this]() { ShowErrorFrame(); });
            break;
        default:
            ShowRest();
            break;
    }
}

void StateMirror::ShowRest() {
    if (mood_.empty()) {
        ESP_LOGI(TAG, "-> rest, no mood set: dark");
        matrix_->StopAnimation();
        matrix_->Clear();
        return;
    }
    ESP_LOGI(TAG, "-> rest, mood '%s' active", mood_.c_str());
    matrix_->StartAnimation(100, [this]() { ShowMoodFrame(); });
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

void StateMirror::ShowMoodPreviewFrame() {
    if (mood_preview_frames_ > 0) {
        MoodEffects::RenderFrame(matrix_, mood_, animation_step_, mood_intensity_);
        animation_step_++;
        mood_preview_frames_--;
        return;
    }
    // Preview's over - hand back to whatever the assistant is actually doing
    // now (it may have moved on since the preview started).
    ESP_LOGI(TAG, "Mood preview done, resuming live state");
    auto state = Application::GetInstance().GetDeviceState();
    OnDeviceStateChanged(state, state);
}

void StateMirror::ShowConnectingFrame() {
    // A brief flash acknowledges the wake word, then a comet chases the
    // panel's border while the assistant is thinking.
    const MatrixColor kThinkColor{160, 40, 255};  // violet, distinct from onboard's blue

    if (animation_step_ < 3) {
        matrix_->FillLocked({255, 255, 255});
    } else if (!perimeter_.empty()) {
        matrix_->FillLocked(MatrixColor());
        int len = static_cast<int>(perimeter_.size());
        int head = (animation_step_ - 3) % len;
        for (int trail = 0; trail < 3; trail++) {
            int index = (head - trail + len) % len;
            int level = 255 - trail * 90;
            auto [x, y] = perimeter_[index];
            matrix_->SetPixelLocked(x, y, Scale(kThinkColor, level));
        }
    }
    matrix_->ShowLocked();
    animation_step_++;
}

void StateMirror::ShowListeningFrame() {
    const MatrixColor kListenColor{0, 140, 255};  // cyan-blue, distinct from onboard's red
    int level = TriangleLevel(animation_step_, 50);  // ~2s breathing period at 40ms/tick
    matrix_->FillLocked(Scale(kListenColor, level));
    matrix_->ShowLocked();
    animation_step_++;
}

void StateMirror::ShowSpeakingFrame() {
    const MatrixColor kSpeakColor{255, 150, 0};  // warm amber, distinct from onboard's green
    const int kPeriodSteps = 12;  // ~720ms at 60ms/tick
    int phase = animation_step_ % kPeriodSteps;
    int level = 255 - phase * 255 / kPeriodSteps;
    matrix_->FillLocked(Scale(kSpeakColor, level));
    matrix_->ShowLocked();
    animation_step_++;
}

void StateMirror::ShowErrorFrame() {
    const MatrixColor kErrorColor{255, 0, 0};
    int level = TriangleLevel(animation_step_, 15);  // ~3s slow pulse period at 100ms/tick
    matrix_->FillLocked(Scale(kErrorColor, level));
    matrix_->ShowLocked();
    animation_step_++;
}
