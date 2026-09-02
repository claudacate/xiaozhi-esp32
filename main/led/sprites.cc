#include "sprites.h"
#include <cstring>
#include <esp_log.h>

#define TAG "Sprites"

namespace {

// clang-format off
const Sprite kSprites[] = {
    // Weather icons, used by StateMirror::ShowWeather. These are the whole
    // library now; the art/fortune icons went with SPEC.md 4.1 and 4.5.
    {"sunny", {
        0b01000010,
        0b00111100,
        0b10111101,
        0b01111110,
        0b01111110,
        0b10111101,
        0b00111100,
        0b01000010,
    }, {255, 180, 0}},

    {"cloudy", {
        0b00000000,
        0b00111000,
        0b01111100,
        0b11111110,
        0b11111111,
        0b01111110,
        0b00000000,
        0b00000000,
    }, {190, 195, 205}},

    {"rainy", {
        0b00000000,
        0b00111000,
        0b01111100,
        0b11111110,
        0b11111111,
        0b00000000,
        0b01010100,
        0b10101010,
    }, {70, 130, 220}},

    {"snowy", {
        0b00000000,
        0b00111000,
        0b01111100,
        0b11111110,
        0b11111111,
        0b00000000,
        0b10001000,
        0b00100010,
    }, {225, 240, 255}},

    {"thunderstorm", {
        0b00000000,
        0b00111000,
        0b01111100,
        0b11111110,
        0b11111111,
        0b00011000,
        0b00110000,
        0b00100000,
    }, {255, 215, 0}},
};
// clang-format on

const int kSpriteCount = sizeof(kSprites) / sizeof(kSprites[0]);

}  // namespace

const Sprite* Sprites::Find(const std::string& name) {
    for (int i = 0; i < kSpriteCount; i++) {
        if (name == kSprites[i].name) {
            return &kSprites[i];
        }
    }
    ESP_LOGW(TAG, "Find: no sprite named '%s'", name.c_str());
    return nullptr;
}

void Sprites::RenderLocked(RgbMatrix* matrix, const Sprite* sprite) {
    matrix->FillLocked(MatrixColor());
    // Callers resolve names via Find(), which returns nullptr for an unknown
    // one and logs it there. Rendering runs in the esp_timer callback, so
    // dereferencing that nullptr panics the whole device, not just the matrix.
    if (sprite == nullptr) {
        return;
    }
    int width = matrix->width();
    int height = matrix->height();
    for (int y = 0; y < 8 && y < height; y++) {
        for (int x = 0; x < 8 && x < width; x++) {
            bool lit = (sprite->rows[y] >> (7 - x)) & 1;
            if (lit) {
                matrix->SetPixelLocked(x, y, sprite->color);
            }
        }
    }
}
