#ifndef _STATE_MIRROR_H_
#define _STATE_MIRROR_H_

#include "rgb_matrix.h"
#include "device_state.h"
#include <utility>
#include <vector>

// Drives the external matrix to reflect what the assistant is currently doing
// (thinking, listening, speaking...), independent of any LLM/voice command.
// Hooks DeviceStateEventManager, so it needs no changes to application.cc.
class StateMirror {
public:
    explicit StateMirror(RgbMatrix* matrix);

    void SetEnabled(bool enabled, bool permanent = false);
    bool enabled() const { return enabled_; }

private:
    void OnDeviceStateChanged(DeviceState previous_state, DeviceState current_state);
    void ShowConnectingFrame();
    void ShowListeningFrame();
    void ShowSpeakingFrame();
    void ShowErrorFrame();

    RgbMatrix* matrix_;
    bool enabled_ = true;
    int animation_step_ = 0;
    std::vector<std::pair<int, int>> perimeter_;
};

#endif // _STATE_MIRROR_H_
