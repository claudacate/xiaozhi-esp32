#ifndef _CANVAS_H_
#define _CANVAS_H_

#include "rgb_matrix.h"
#include <string>
#include <vector>

// A persistent pixel buffer for the voice-to-pixel-art feature. Holds
// whatever was last drawn (a sprite or a freehand palette+grid) so it can be
// redrawn as idle content until cleared or replaced.
class Canvas {
public:
    Canvas(int width, int height);

    // `palette` is comma-separated 6-digit hex colors (e.g. "FF0000,00FF00"),
    // up to 16 entries. `grid` is width*height hex-nibble characters
    // (row-major, top-to-bottom) indexing into the palette. Returns false and
    // leaves the canvas unchanged if either is malformed.
    bool Draw(const std::string& palette, const std::string& grid);
    // Returns false if `name` isn't in the sprite library.
    bool DrawSprite(const std::string& name);
    void SetPixel(int x, int y, MatrixColor color);
    void Clear();

    // Pushes the current buffer to the matrix. Caller holds the lock and
    // calls ShowLocked() itself afterward.
    void RenderLocked(RgbMatrix* matrix) const;

private:
    int width_;
    int height_;
    std::vector<MatrixColor> pixels_;
};

#endif // _CANVAS_H_
