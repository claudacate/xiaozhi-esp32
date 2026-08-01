#ifndef _MOOD_EFFECTS_H_
#define _MOOD_EFFECTS_H_

#include "rgb_matrix.h"
#include <string>

// Procedural ambient animations for idle time. Firmware owns the visual
// quality; the LLM only ever picks a name and an intensity, so results are
// always reliable regardless of what the model does.
class MoodEffects {
public:
    // Renders one frame of `mood` at animation tick `step` and `intensity`
    // (0-100, effect-specific meaning) into `matrix`. Returns false if `mood`
    // is not a recognized name, in which case nothing is drawn.
    static bool RenderFrame(RgbMatrix* matrix, const std::string& mood, int step, uint8_t intensity);

    // True if `mood` is a name RenderFrame recognizes.
    static bool IsValidMood(const std::string& mood);

    // Comma-separated list of valid mood names, for error messages.
    static const char* ValidMoods();

private:
    static void Fireplace(RgbMatrix* matrix, int step, uint8_t intensity);
    static void Rain(RgbMatrix* matrix, int step, uint8_t intensity);
    static void Waves(RgbMatrix* matrix, int step, uint8_t intensity);
    static void Breathing(RgbMatrix* matrix, int step, uint8_t intensity);
    static void Starfield(RgbMatrix* matrix, int step, uint8_t intensity);
    static void Aurora(RgbMatrix* matrix, int step, uint8_t intensity);
};

#endif // _MOOD_EFFECTS_H_
