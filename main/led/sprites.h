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

// The weather icons (SPEC.md 4.2), single color each, as firmware sprites
// rather than LLM-drawn freehand. The general art library that also lived here
// went with the canvas and fortune features (SPEC.md 4.1, 4.5).
class Sprites {
public:
    // Returns nullptr if `name` isn't in the library.
    static const Sprite* Find(const std::string& name);
    // Draws the sprite into the matrix. Caller holds the lock and calls
    // ShowLocked() itself afterward.
    static void RenderLocked(RgbMatrix* matrix, const Sprite* sprite);
};

#endif // _SPRITES_H_
