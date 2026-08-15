#include "sprites.h"
#include <cstring>
#include <esp_log.h>

#define TAG "Sprites"

namespace {

// clang-format off
const Sprite kSprites[] = {
    {"heart", {
        0b01100110,
        0b11111111,
        0b11111111,
        0b11111111,
        0b01111110,
        0b00111100,
        0b00011000,
        0b00000000,
    }, {255, 0, 40}},

    {"star", {
        0b00011000,
        0b00011000,
        0b00011000,
        0b11111111,
        0b11111111,
        0b00011000,
        0b00011000,
        0b00011000,
    }, {255, 200, 0}},

    {"smiley", {
        0b00111100,
        0b01000010,
        0b10100101,
        0b10000001,
        0b10000001,
        0b10111101,
        0b01000010,
        0b00111100,
    }, {255, 220, 0}},

    {"ghost", {
        0b00111100,
        0b01111110,
        0b11111111,
        0b11111111,
        0b11111111,
        0b11111111,
        0b11011011,
        0b10110110,
    }, {230, 230, 255}},

    {"coffee", {
        0b00100100,
        0b00010010,
        0b00100100,
        0b01111100,
        0b01111101,
        0b01111101,
        0b01111100,
        0b00111000,
    }, {160, 90, 30}},

    {"invader", {
        0b00100100,
        0b00010010,
        0b00111110,
        0b01101011,
        0b11111111,
        0b10111101,
        0b10100101,
        0b00011000,
    }, {0, 220, 90}},

    {"checkmark", {
        0b00000001,
        0b00000010,
        0b00000100,
        0b10001000,
        0b01010000,
        0b00100000,
        0b00000000,
        0b00000000,
    }, {0, 220, 90}},

    {"cross", {
        0b10000001,
        0b01000010,
        0b00100100,
        0b00011000,
        0b00011000,
        0b00100100,
        0b01000010,
        0b10000001,
    }, {255, 40, 40}},

    {"question", {
        0b01111100,
        0b11000110,
        0b00000110,
        0b00001100,
        0b00011000,
        0b00011000,
        0b00000000,
        0b00011000,
    }, {0, 180, 255}},

    // Weather icons, used by StateMirror::ShowWeather (not exposed via Names(),
    // which is for canvas.sprite's freehand-icon list).
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

const char* Sprites::Names() {
    return "heart, star, smiley, ghost, coffee, invader, checkmark, cross, question";
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
