// The inside of an outlined HUD gauge (the Meccaryn's heart-shaped health gauge, the jetpack's
// arrow-shaped fuel gauge): found by flood-filling the transparent pixels from a seed point inside
// the outline, then cut into horizontal spans. The game fills such a gauge from the bottom up; we
// draw the spans below the fill level with the fill texture.
#pragma once

#include <vector>

#include "assets/image.h"

namespace gvr {

struct GaugeSpan {
    float v0, v1;  // rows covered, texture v (0 = top of the image)
    float x0, x1;  // horizontal extent, texture u
};

// rowStep: image rows per span band. alphaWall: alpha at or above which a pixel is outline.
// Empty if the fill leaks to the image border (seed outside the outline) or the seed is a wall.
std::vector<GaugeSpan> gaugeInterior(const Image& img, int seedX, int seedY, int rowStep = 2, int alphaWall = 60);

}  // namespace gvr
