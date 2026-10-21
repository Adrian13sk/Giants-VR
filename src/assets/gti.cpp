#include "assets/gti.h"

#include <limits>

#include "assets/binreader.h"

namespace gvr {

bool parseGti(const uint8_t* data, size_t size, GtiTerrain& t, std::string* error) {
    BinReader r(data, size);
    auto fail = [&](const std::string& m) {
        if (error) *error = "GTI: " + m + " (offset " + std::to_string(r.pos()) + ")";
        return false;
    };
    t = GtiTerrain{};
    const uint32_t magic = r.u32();
    if ((magic & 0xFFFFFF00u) != 0x94965000u) return fail("bad magic");
    if ((magic & 0xFF) >= 2) {
        const uint32_t chunks = r.u32();
        if (chunks > 4096) return fail("bad chunk count");
        r.skip(size_t(chunks) * 0x34);
    }
    t.minX = r.f32();
    t.minY = r.f32();
    t.minHeight = r.f32();
    t.maxHeight = r.f32();
    t.tilesX = r.u32();
    t.tilesY = r.u32();
    t.stretch = r.f32();
    r.skip(4 * 4);  // 0.5, 0.5, colour1, colour2
    t.flags = r.u8();
    r.skip(3);      // material bytes + pad
    r.skip(4 + 4);  // u32 0, f32 1.0
    t.name = r.fixedString(32);
    if (!r.ok() || t.tilesX == 0 || t.tilesY == 0 || t.tilesX > 4096 || t.tilesY > 4096 || !(t.stretch > 0))
        return fail("bad header");
    t.gridW = t.tilesX + 2;
    t.gridH = t.tilesY + 2;
    const size_t cells = size_t(t.gridW) * t.gridH;
    t.height.assign(cells, std::numeric_limits<float>::quiet_NaN());
    t.triangles.assign(cells, 0);
    t.light.assign(cells * 3, 0);

    // Run-length cell stream over interior rows 1..tilesY, wrapping columns at tilesX+1.
    uint32_t x = 1, y = 1;
    while (y < t.tilesY + 1) {
        if (r.remaining() == 0) return fail("cell stream truncated");
        const int8_t run = static_cast<int8_t>(r.u8());
        if (run < 0) {
            x += static_cast<uint32_t>(-run);
            while (x >= t.tilesX + 1) {
                x = x + 2 - t.gridW;
                ++y;
            }
        } else {
            for (int i = 0; i <= run; ++i) {
                const float hgt = r.f32();
                const uint8_t tri = r.u8();
                uint8_t rgb[3] = {0, 0, 0};
                if (t.flags & 2) { rgb[0] = r.u8(); rgb[1] = r.u8(); rgb[2] = r.u8(); }
                if (t.flags & 4) r.u8();
                if (!r.ok()) return fail("cell data truncated");
                if (y < t.gridH && x < t.gridW) {
                    const size_t idx = size_t(y) * t.gridW + x;
                    t.height[idx] = hgt;
                    t.triangles[idx] = tri;
                    t.light[idx * 3 + 0] = rgb[0];
                    t.light[idx * 3 + 1] = rgb[1];
                    t.light[idx * 3 + 2] = rgb[2];
                }
                if (++x >= t.tilesX + 1) {
                    x = 1;
                    ++y;
                }
            }
        }
    }
    t.bytesConsumed = r.pos();
    return true;
}

}  // namespace gvr
