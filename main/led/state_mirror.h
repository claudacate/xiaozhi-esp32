#ifndef _STATE_MIRROR_H_
#define _STATE_MIRROR_H_

#include "rgb_matrix.h"
#include "device_state.h"
#include <string>
#include <utility>
#include <vector>

// Drives the external matrix to reflect what the assistant is currently doing
// (thinking, listening, speaking...), independent of any LLM/voice command.
// Hooks DeviceStateEventManager, so it needs no changes to application.cc.
//
// Also owns idle-time content: when idle and a mood is set (SetMood), the
// mood's ambient animation plays instead of going dark. Active states
// (listening/thinking/speaking/error) always take over from the mood and hand
// back to it once idle again — this is the one place two behaviors currently
// compete for the panel, so it's resolved here directly rather than via a
// general priority stack (see SPEC.md 3), which has nothing else to arbitrate
// yet.
class StateMirror {
public:
    explicit StateMirror(RgbMatrix* matrix);

    void SetEnabled(bool enabled, bool permanent = false);
    bool enabled() const { return enabled_; }

    // Returns false if `mood` is not a name MoodEffects recognizes.
    bool SetMood(const std::string& mood, uint8_t intensity, bool permanent = true);
    void ClearMood(bool permanent = true);

private:
    void OnDeviceStateChanged(DeviceState previous_state, DeviceState current_state);
    void ShowConnectingFrame();
    void ShowListeningFrame();
    void ShowSpeakingFrame();
    void ShowErrorFrame();
    void ShowMoodFrame();
    void ShowMoodPreviewFrame();
    // Idle/boot states: the active mood if one is set, otherwise dark.
    void ShowRest();

    RgbMatrix* matrix_;
    bool enabled_ = true;
    int animation_step_ = 0;
    std::vector<std::pair<int, int>> perimeter_;
    std::string mood_;
    uint8_t mood_intensity_ = 60;
    int mood_preview_frames_ = 0;
};

#endif // _STATE_MIRROR_H_
