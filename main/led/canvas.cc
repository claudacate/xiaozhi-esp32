#include "canvas.h"
#include "sprites.h"
#include <algorithm>
#include <cctype>
#include <sstream>

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

bool Canvas::Draw(const std::string& palette, const std::string& grid) {
    std::vector<MatrixColor> parsed_palette;
    std::stringstream ss(palette);
    std::string entry;
    while (std::getline(ss, entry, ',')) {
        MatrixColor color;
        if (!ParseHexColor(entry, &color)) {
            return false;
        }
        parsed_palette.push_back(color);
        if (parsed_palette.size() > 16) {
            return false;
        }
    }
    if (parsed_palette.empty()) {
        return false;
    }

    if (static_cast<int>(grid.size()) != width_ * height_) {
        return false;
    }
    std::vector<MatrixColor> parsed_pixels(width_ * height_);
    for (int i = 0; i < width_ * height_; i++) {
        int index = HexDigit(grid[i]);
        if (index < 0 || index >= static_cast<int>(parsed_palette.size())) {
            return false;
        }
        parsed_pixels[i] = parsed_palette[index];
    }

    pixels_ = std::move(parsed_pixels);
    return true;
}

bool Canvas::DrawSprite(const std::string& name) {
    const Sprite* sprite = Sprites::Find(name);
    if (sprite == nullptr) {
        return false;
    }
    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            bool lit = y < 8 && x < 8 && ((sprite->rows[y] >> (7 - x)) & 1);
            pixels_[y * width_ + x] = lit ? sprite->color : MatrixColor();
        }
    }
    return true;
}

void Canvas::SetPixel(int x, int y, MatrixColor color) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) {
        return;
    }
    pixels_[y * width_ + x] = color;
}

void Canvas::Fill(MatrixColor color) {
    std::fill(pixels_.begin(), pixels_.end(), color);
}

void Canvas::Clear() {
    std::fill(pixels_.begin(), pixels_.end(), MatrixColor());
}

void Canvas::RenderLocked(RgbMatrix* matrix) const {
    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            matrix->SetPixelLocked(x, y, pixels_[y * width_ + x]);
        }
    }
}
