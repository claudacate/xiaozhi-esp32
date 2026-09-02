#ifndef _CANVAS_H_
#define _CANVAS_H_

#include "rgb_matrix.h"
#include <string>
#include <vector>

// A persistent pixel buffer held as idle content. The voice-to-pixel-art
// feature it was built for was removed (SPEC.md 4.5); it survives because
// self.led_matrix.turn_on (lamp mode) is built on Fill + RenderLocked.
class Canvas {
public:
    Canvas(int width, int height);

    // Sets every pixel to `color` (whole-panel solid fill).
    void Fill(MatrixColor color);

    // Pushes the current buffer to the matrix. Caller holds the lock and
    // calls ShowLocked() itself afterward.
    void RenderLocked(RgbMatrix* matrix) const;

    // Returns false if `hex` isn't exactly 6 valid hex digits (e.g. "FF8800").
    static bool ParseHexColor(const std::string& hex, MatrixColor* out);

private:
    int width_;
    int height_;
    std::vector<MatrixColor> pixels_;
};

#endif // _CANVAS_H_
