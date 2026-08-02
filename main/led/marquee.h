#ifndef _MARQUEE_H_
#define _MARQUEE_H_

#include "rgb_matrix.h"
#include <string>

// A reusable scrolling-text primitive shared by the clock, weather
// temperature, and fortune fallback. Supports digits, ':', '-', 'C' and
// space; other characters render as a blank column rather than failing.
class Marquee {
public:
    // Total animation steps for `text` to fully enter from the right and
    // scroll off to the left. Callers run RenderFrame for this many steps.
    static int FrameCount(RgbMatrix* matrix, const std::string& text);

    // Renders one frame of the scroll, `step` frames in. Caller must hold
    // the matrix's lock (this uses the *Locked drawing calls) and call
    // ShowLocked() itself afterward.
    static void RenderFrame(RgbMatrix* matrix, const std::string& text, int step, MatrixColor color);
};

#endif // _MARQUEE_H_
