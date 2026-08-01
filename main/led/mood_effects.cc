#include "mood_effects.h"
#include <esp_random.h>
#include <algorithm>
#include <cmath>

namespace {

MatrixColor Scale(MatrixColor color, int level_0_255) {
    if (level_0_255 < 0) level_0_255 = 0;
    if (level_0_255 > 255) level_0_255 = 255;
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

// Black -> red -> orange -> yellow -> white, classic fire palette.
MatrixColor HeatColor(uint8_t heat) {
    uint8_t t192 = static_cast<uint8_t>((heat * 191) / 255);
    uint8_t ramp = (t192 & 0x3F) << 2;
    if (t192 & 0x80) {
        return {255, 255, ramp};
    } else if (t192 & 0x40) {
        return {255, ramp, 0};
    } else {
        return {ramp, 0, 0};
    }
}

// hue in degrees [0,360), saturation/value in [0,1].
MatrixColor HsvToRgb(float hue, float saturation, float value) {
    float c = value * saturation;
    float x = c * (1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f));
    float m = value - c;
    float r, g, b;
    if (hue < 60) { r = c; g = x; b = 0; }
    else if (hue < 120) { r = x; g = c; b = 0; }
    else if (hue < 180) { r = 0; g = c; b = x; }
    else if (hue < 240) { r = 0; g = x; b = c; }
    else if (hue < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    return {
        static_cast<uint8_t>((r + m) * 255),
        static_cast<uint8_t>((g + m) * 255),
        static_cast<uint8_t>((b + m) * 255),
    };
}

}  // namespace

bool MoodEffects::IsValidMood(const std::string& mood) {
    return mood == "fireplace" || mood == "rain" || mood == "waves" ||
           mood == "breathing" || mood == "starfield" || mood == "aurora";
}

bool MoodEffects::RenderFrame(RgbMatrix* matrix, const std::string& mood, int step, uint8_t intensity) {
    if (mood == "fireplace") {
        Fireplace(matrix, step, intensity);
    } else if (mood == "rain") {
        Rain(matrix, step, intensity);
    } else if (mood == "waves") {
        Waves(matrix, step, intensity);
    } else if (mood == "breathing") {
        Breathing(matrix, step, intensity);
    } else if (mood == "starfield") {
        Starfield(matrix, step, intensity);
    } else if (mood == "aurora") {
        Aurora(matrix, step, intensity);
    } else {
        return false;
    }
    matrix->ShowLocked();
    return true;
}

const char* MoodEffects::ValidMoods() {
    return "fireplace, rain, waves, breathing, starfield, aurora";
}

// Hottest at the bottom row, fading toward the top, with per-pixel flicker.
void MoodEffects::Fireplace(RgbMatrix* matrix, int step, uint8_t intensity) {
    int w = matrix->width();
    int h = matrix->height();
    int jitter = 30 + intensity;  // 30..130
    for (int y = 0; y < h; y++) {
        int base = h > 1 ? (255 * y) / (h - 1) : 255;
        for (int x = 0; x < w; x++) {
            int noise = static_cast<int>(esp_random() % (jitter * 2 + 1)) - jitter;
            matrix->SetPixelLocked(x, y, HeatColor(static_cast<uint8_t>(std::max(0, std::min(255, base + noise)))));
        }
    }
}

// Cool blue drops falling down each column, staggered so they don't fall in
// lockstep, with a short fading tail.
void MoodEffects::Rain(RgbMatrix* matrix, int step, uint8_t intensity) {
    int w = matrix->width();
    int h = matrix->height();
    matrix->FillLocked(MatrixColor());

    const int kTrail = 3;
    int speed = 1 + intensity / 50;  // 1..3
    for (int x = 0; x < w; x++) {
        int offset = (x * 53 + x * x * 17) % (h * 4);
        int phase = (step * speed + offset) % (h + kTrail);
        int head_y = phase - kTrail;
        for (int t = 0; t < kTrail; t++) {
            int y = head_y - t;
            if (y < 0 || y >= h) continue;
            int level = 255 - t * 255 / kTrail;
            matrix->SetPixelLocked(x, y, Scale({80, 160, 255}, level));
        }
    }
}

// A brightness wave scrolling across the columns.
void MoodEffects::Waves(RgbMatrix* matrix, int step, uint8_t intensity) {
    int w = matrix->width();
    int h = matrix->height();
    float amplitude = 0.3f + (intensity / 100.0f) * 0.7f;
    float phase = step * 0.15f;
    for (int x = 0; x < w; x++) {
        float wave = sinf(phase + x * 0.8f);
        float norm = (wave + 1.0f) * 0.5f;
        int level = static_cast<int>(norm * amplitude * 255.0f);
        MatrixColor c = Scale({0, 200, 220}, level);
        for (int y = 0; y < h; y++) {
            matrix->SetPixelLocked(x, y, c);
        }
    }
}

// Slow whole-panel breathing fade. intensity sets the peak brightness.
void MoodEffects::Breathing(RgbMatrix* matrix, int step, uint8_t intensity) {
    int peak = 60 + intensity * 195 / 100;  // 60..255
    int level = TriangleLevel(step, 60);    // ~4.8s period at 100ms/tick
    matrix->FillLocked(Scale({180, 200, 255}, level * peak / 255));
}

// Sparse twinkling stars on a dark background.
void MoodEffects::Starfield(RgbMatrix* matrix, int step, uint8_t intensity) {
    int w = matrix->width();
    int h = matrix->height();
    matrix->FillLocked(MatrixColor());

    int density = 1 + intensity / 20;  // 1..6 concurrent stars
    int bucket = step / 8;             // each star's position holds for ~8 ticks
    int local_phase = step % 8;
    int level = local_phase < 4 ? local_phase * 255 / 4 : (8 - local_phase) * 255 / 4;
    for (int i = 0; i < density; i++) {
        uint32_t hash = static_cast<uint32_t>(bucket) * 2654435761u + static_cast<uint32_t>(i) * 40503u;
        int x = hash % w;
        int y = (hash / w) % h;
        matrix->SetPixelLocked(x, y, Scale({255, 255, 255}, level));
    }
}

// Hue drifts through green -> cyan -> blue -> purple/magenta and back, at a
// slightly different phase per row, for a shimmering band effect closer to a
// real aurora than a flat two-color blend.
void MoodEffects::Aurora(RgbMatrix* matrix, int step, uint8_t intensity) {
    int w = matrix->width();
    int h = matrix->height();
    float value = 0.5f + (intensity / 100.0f) * 0.5f;
    for (int y = 0; y < h; y++) {
        float phase = step * 0.03f + y * 0.5f;
        float t = (sinf(phase) + 1.0f) * 0.5f;  // 0..1
        float hue = 120.0f + t * 180.0f;        // green(120) .. purple/magenta(300)
        MatrixColor c = HsvToRgb(hue, 0.9f, value);
        for (int x = 0; x < w; x++) {
            matrix->SetPixelLocked(x, y, c);
        }
    }
}
