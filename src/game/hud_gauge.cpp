#include "game/hud_gauge.h"

#include <algorithm>

namespace gvr {

std::vector<GaugeSpan> gaugeInterior(const Image& img, int seedX, int seedY, int rowStep, int alphaWall) {
    std::vector<GaugeSpan> out;
    const int w = img.width, h = img.height;
    if (w <= 2 || h <= 2 || seedX <= 0 || seedY <= 0 || seedX >= w - 1 || seedY >= h - 1 || rowStep < 1) return out;
    if (img.rgba.size() < size_t(w) * size_t(h) * 4) return out;
    auto wall = [&](int x, int y) { return img.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] >= alphaWall; };
    if (wall(seedX, seedY)) return out;
    std::vector<uint8_t> in(size_t(w) * size_t(h), 0);
    std::vector<int> stack{seedY * w + seedX};
    in[size_t(seedY * w + seedX)] = 1;
    while (!stack.empty()) {
        const int p = stack.back();
        stack.pop_back();
        const int x = p % w, y = p / w;
        if (x == 0 || y == 0 || x == w - 1 || y == h - 1) return {};  // leaked out of the outline
        const int nb[4] = {p - 1, p + 1, p - w, p + w};
        for (int q : nb) {
            if (in[size_t(q)] || wall(q % w, q / w)) continue;
            in[size_t(q)] = 1;
            stack.push_back(q);
        }
    }
    // Bands of rowStep rows: each run of inside pixels (union over the band's rows) is a span.
    for (int y0 = 0; y0 < h; y0 += rowStep) {
        const int y1 = std::min(h, y0 + rowStep);
        int x = 0;
        while (x < w) {
            auto inside = [&](int xx) {
                for (int y = y0; y < y1; ++y)
                    if (in[size_t(y * w + xx)]) return true;
                return false;
            };
            if (!inside(x)) { ++x; continue; }
            const int start = x;
            while (x < w && inside(x)) ++x;
            // One pixel of overlap under the outline's soft edge.
            out.push_back({float(y0) / float(h), float(y1) / float(h), float(std::max(0, start - 1)) / float(w),
                           float(std::min(w, x + 1)) / float(w)});
        }
    }
    return out;
}

}  // namespace gvr
