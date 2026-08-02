#ifndef _SPRITES_H_
#define _SPRITES_H_

#include "rgb_matrix.h"
#include <string>

struct Sprite {
    const char* name;
    // 8 rows, each byte's bits 7..0 = columns left..right (1 = lit).
    uint8_t rows[8];
    MatrixColor color;
};

// A small built-in library of recognizable 8x8 icons, single color each, so
// common requests (canvas sprites, fortune-teller symbols) render reliably
// without depending on an LLM to draw them freehand.
class Sprites {
public:
    // Returns nullptr if `name` isn't in the library.
    static const Sprite* Find(const std::string& name);
    // Comma-separated list of valid names, for error messages.
    static const char* Names();
    // Draws the sprite into the matrix. Caller holds the lock and calls
    // ShowLocked() itself afterward.
    static void RenderLocked(RgbMatrix* matrix, const Sprite* sprite);
};

#endif // _SPRITES_H_
