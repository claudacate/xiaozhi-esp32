#include "canvas.h"
#include <algorithm>
#include <cctype>

namespace {

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Returns false if `hex` isn't exactly 6 valid hex digits.
bool ParseHexColor(const std::string& hex, MatrixColor* out) {
    if (hex.size() != 6) {
        return false;
    }
    int digits[6];
    for (int i = 0; i < 6; i++) {
        digits[i] = HexDigit(hex[i]);
        if (digits[i] < 0) {
            return false;
        }
    }
    out->red = static_cast<uint8_t>(digits[0] * 16 + digits[1]);
    out->green = static_cast<uint8_t>(digits[2] * 16 + digits[3]);
    out->blue = static_cast<uint8_t>(digits[4] * 16 + digits[5]);
    return true;
}

}  // namespace

Canvas::Canvas(int width, int height) : width_(width), height_(height) {
    pixels_.resize(width_ * height_);
}

bool Canvas::ParseHexColor(const std::string& hex, MatrixColor* out) {
    return ::ParseHexColor(hex, out);
}

void Canvas::Fill(MatrixColor color) {
    std::fill(pixels_.begin(), pixels_.end(), color);
}

void Canvas::RenderLocked(RgbMatrix* matrix) const {
    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            matrix->SetPixelLocked(x, y, pixels_[y * width_ + x]);
        }
    }
}
