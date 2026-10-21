// GTI terrain heightfield (magic 0x9496500N). Verified against Story1.gti from the GOG data:
// the cell stream is consumed to within 4 bytes of the end of file and the decoded lightmap
// shows the level's islands (see docs/formats/GTI.md).
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gvr {

struct GtiTerrain {
    float minX = 0, minY = 0;          // game units; corner of interior cell (1,1)
    float minHeight = 0, maxHeight = 0;
    uint32_t tilesX = 0, tilesY = 0;   // interior cells; grid is (tilesX+2) x (tilesY+2)
    float stretch = 0;                 // world size of one cell (game units)
    uint8_t flags = 0;                 // bit1: per-cell RGB lightmap, bit2: extra byte
    std::string name;
    uint32_t gridW = 0, gridH = 0;
    std::vector<float> height;         // NaN where the cell is absent
    std::vector<uint8_t> triangles;    // per-cell triangle/diagonal code (low 3 bits)
    std::vector<uint8_t> light;        // RGB per cell (zeros if no lightmap)
    size_t bytesConsumed = 0;

    bool valid(uint32_t x, uint32_t y) const {
        return x < gridW && y < gridH && !std::isnan(height[y * gridW + x]);
    }
    float h(uint32_t x, uint32_t y) const { return height[y * gridW + x]; }
    // Game-space XY of grid vertex (x, y).
    float worldX(uint32_t x) const { return minX + (float(x) - 1.0f) * stretch; }
    float worldY(uint32_t y) const { return minY + (float(y) - 1.0f) * stretch; }
};

bool parseGti(const uint8_t* data, size_t size, GtiTerrain& out, std::string* error = nullptr);

}  // namespace gvr
