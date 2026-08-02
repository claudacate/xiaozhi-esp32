#include "marquee.h"
#include <cctype>

namespace {

// 3 columns x 5 rows per glyph. Each byte's bits 2,1,0 are the left, middle
// and right column of that row (1 = lit). Unrecognized characters fall
// through to an all-blank glyph rather than failing.
const uint8_t* GetGlyph(char c) {
    static const uint8_t kBlank[5]  = {0b000, 0b000, 0b000, 0b000, 0b000};
    static const uint8_t k0[5]      = {0b111, 0b101, 0b101, 0b101, 0b111};
    static const uint8_t k1[5]      = {0b010, 0b110, 0b010, 0b010, 0b111};
    static const uint8_t k2[5]      = {0b111, 0b001, 0b111, 0b100, 0b111};
    static const uint8_t k3[5]      = {0b111, 0b001, 0b111, 0b001, 0b111};
    static const uint8_t k4[5]      = {0b101, 0b101, 0b111, 0b001, 0b001};
    static const uint8_t k5[5]      = {0b111, 0b100, 0b111, 0b001, 0b111};
    static const uint8_t k6[5]      = {0b111, 0b100, 0b111, 0b101, 0b111};
    static const uint8_t k7[5]      = {0b111, 0b001, 0b001, 0b001, 0b001};
    static const uint8_t k8[5]      = {0b111, 0b101, 0b111, 0b101, 0b111};
    static const uint8_t k9[5]      = {0b111, 0b101, 0b111, 0b001, 0b111};
    static const uint8_t kColon[5]  = {0b000, 0b010, 0b000, 0b010, 0b000};
    static const uint8_t kDash[5]   = {0b000, 0b000, 0b111, 0b000, 0b000};
    static const uint8_t kC[5]      = {0b111, 0b100, 0b100, 0b100, 0b111};

    switch (c) {
        case '0': return k0;
        case '1': return k1;
        case '2': return k2;
        case '3': return k3;
        case '4': return k4;
        case '5': return k5;
        case '6': return k6;
        case '7': return k7;
        case '8': return k8;
        case '9': return k9;
        case ':': return kColon;
        case '-': return kDash;
        case 'C': case 'c': return kC;
        default: return kBlank;
    }
}

const int kGlyphWidth = 3;
const int kGlyphAdvance = 4;  // glyph width + 1px spacing
const int kGlyphHeight = 5;

}  // namespace

int Marquee::FrameCount(RgbMatrix* matrix, const std::string& text) {
    return matrix->width() + static_cast<int>(text.size()) * kGlyphAdvance;
}

void Marquee::RenderFrame(RgbMatrix* matrix, const std::string& text, int step, MatrixColor color) {
    int width = matrix->width();
    int height = matrix->height();
    int top_margin = (height - kGlyphHeight) / 2;
    int text_start_x = width - step;

    matrix->FillLocked(MatrixColor());
    for (int x = 0; x < width; x++) {
        int virtual_x = x - text_start_x;
        if (virtual_x < 0) {
            continue;
        }
        int char_index = virtual_x / kGlyphAdvance;
        int col_in_char = virtual_x % kGlyphAdvance;
        if (char_index >= static_cast<int>(text.size()) || col_in_char >= kGlyphWidth) {
            continue;
        }
        const uint8_t* glyph = GetGlyph(text[char_index]);
        for (int row = 0; row < kGlyphHeight; row++) {
            int y = row + top_margin;
            if (y < 0 || y >= height) {
                continue;
            }
            bool lit = (glyph[row] >> (kGlyphWidth - 1 - col_in_char)) & 1;
            if (lit) {
                matrix->SetPixelLocked(x, y, color);
            }
        }
    }
}
