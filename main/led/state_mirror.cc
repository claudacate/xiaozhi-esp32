#include "state_mirror.h"
#include "device_state_event.h"
#include "application.h"
#include "settings.h"
#include "mood_effects.h"

namespace {

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
        return false;
    }

    mood_ = mood;
    mood_intensity_ = intensity;
    if (permanent) {
        Settings settings("matrix", true);
        settings.SetString("mood", mood_);
        settings.SetInt("mood_intensity", mood_intensity_);
    }

    auto state = Application::GetInstance().GetDeviceState();
    OnDeviceStateChanged(state, state);
    return true;
}

void StateMirror::ClearMood(bool permanent) {
    mood_.clear();
    if (permanent) {
        Settings settings("matrix", true);
        settings.SetString("mood", "");
    }

    auto state = Application::GetInstance().GetDeviceState();
    OnDeviceStateChanged(state, state);
}

void StateMirror::OnDeviceStateChanged(DeviceState previous_state, DeviceState current_state) {
    if (!enabled_) {
        return;
    }

    animation_step_ = 0;
    switch (current_state) {
        case kDeviceStateConnecting:
            matrix_->StartAnimation(80, [this]() { ShowConnectingFrame(); });
            break;
        case kDeviceStateListening:
        case kDeviceStateAudioTesting:
            matrix_->StartAnimation(40, [this]() { ShowListeningFrame(); });
            break;
        case kDeviceStateSpeaking:
            matrix_->StartAnimation(60, [this]() { ShowSpeakingFrame(); });
            break;
        case kDeviceStateFatalError:
            matrix_->StartAnimation(100, [this]() { ShowErrorFrame(); });
            break;
        default:
            ShowRest();
            break;
    }
}

void StateMirror::ShowRest() {
    if (mood_.empty()) {
        matrix_->StopAnimation();
        matrix_->Clear();
        return;
    }
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
