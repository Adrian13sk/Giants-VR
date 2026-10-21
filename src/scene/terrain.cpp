#include "scene/terrain.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
constexpr uint32_t kChunk = 32;          // cells per chunk side
constexpr float kTexCellsPerRepeat = 1;  // ground texture repeats once per cell (provisional, unverified)
constexpr float kWallSlopeY = 0.6f;      // normal.y below this uses the wall/rock texture
}  // namespace

bool Terrain::build(const GtiTerrain& t, Renderer& r, TextureId ground, TextureId wall) {
    t_ = t;
    bmin_ = {1e30f, 1e30f, 1e30f};
    bmax_ = {-1e30f, -1e30f, -1e30f};
    tris_ = 0;
    auto vert = [&](uint32_t x, uint32_t y) {
        Vertex v{};
        Vec3 p = giantsToRuntime(t.worldX(x), t.worldY(y), t.h(x, y));
        v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
        v.uv[0] = float(x) / kTexCellsPerRepeat;
        v.uv[1] = float(y) / kTexCellsPerRepeat;
        const uint8_t* l = &t.light[(size_t(y) * t.gridW + x) * 3];
        if (t.flags & 2) {
            // ASSUMPTION (unverified): lightmap is applied like D3D MODULATE2X (128 = neutral);
            // shipped lightmaps average ~70/255. A small floor avoids pitch-black levels whose
            // lightmap is near zero (e.g. M_Reaper_L1) until the original rule is confirmed.
            // Raised from 40+2x to 56+2.4x after device test 1 reported the scene a bit dark.
            for (int k = 0; k < 3; ++k) v.color[k] = uint8_t(std::min(255, 56 + int(2.4f * l[k])));
        }
        else { v.color[0] = v.color[1] = v.color[2] = 255; }
        v.color[3] = 255;
        bmin_ = {std::min(bmin_.x, p.x), std::min(bmin_.y, p.y), std::min(bmin_.z, p.z)};
        bmax_ = {std::max(bmax_.x, p.x), std::max(bmax_.y, p.y), std::max(bmax_.z, p.z)};
        return v;
    };

    for (uint32_t cy = 0; cy + 1 < t.gridH; cy += kChunk) {
        for (uint32_t cx = 0; cx + 1 < t.gridW; cx += kChunk) {
            MeshData meshes[2];  // 0 = ground, 1 = wall
            for (uint32_t y = cy; y < std::min(cy + kChunk, t.gridH - 1); ++y) {
                for (uint32_t x = cx; x < std::min(cx + kChunk, t.gridW - 1); ++x) {
                    if (!t.valid(x, y) || !t.valid(x + 1, y) || !t.valid(x, y + 1) || !t.valid(x + 1, y + 1))
                        continue;
                    // Two triangles, counter-clockwise when viewed from above in game space.
                    // Diagonal choice from the cell's triangle code bit 2 (provisional).
                    const bool altDiag = (t.triangles[size_t(y) * t.gridW + x] & 4) != 0;
                    uint32_t q[2][3][2];
                    if (altDiag) {
                        uint32_t a[2][3][2] = {{{x, y}, {x + 1, y}, {x + 1, y + 1}}, {{x, y}, {x + 1, y + 1}, {x, y + 1}}};
                        std::copy(&a[0][0][0], &a[0][0][0] + 12, &q[0][0][0]);
                    } else {
                        uint32_t a[2][3][2] = {{{x, y}, {x + 1, y}, {x, y + 1}}, {{x + 1, y}, {x + 1, y + 1}, {x, y + 1}}};
                        std::copy(&a[0][0][0], &a[0][0][0] + 12, &q[0][0][0]);
                    }
                    for (auto& tri : q) {
                        Vertex v[3];
                        for (int k = 0; k < 3; ++k) v[k] = vert(tri[k][0], tri[k][1]);
                        Vec3 a{v[0].pos[0], v[0].pos[1], v[0].pos[2]};
                        Vec3 b{v[1].pos[0], v[1].pos[1], v[1].pos[2]};
                        Vec3 c{v[2].pos[0], v[2].pos[1], v[2].pos[2]};
                        Vec3 n = normalize(cross(b - a, c - a));
                        MeshData& m = meshes[n.y < kWallSlopeY ? 1 : 0];
                        uint32_t base = uint32_t(m.vertices.size());
                        for (auto& vx : v) {
                            vx.normal[0] = n.x; vx.normal[1] = n.y; vx.normal[2] = n.z;
                            m.vertices.push_back(vx);
                        }
                        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
                        ++tris_;
                    }
                }
            }
            for (int i = 0; i < 2; ++i) {
                if (meshes[i].indices.empty()) continue;
                Chunk c;
                c.mesh = r.uploadMesh(meshes[i]);
                c.texture = i == 0 ? ground : (wall ? wall : ground);
                chunks_.push_back(c);
            }
        }
    }
    LOGI("Terrain %s: %ux%u cells, %u triangles, %zu chunks", t.name.c_str(), t.tilesX, t.tilesY, tris_,
         chunks_.size());
    return !chunks_.empty();
}

void Terrain::render(Renderer& r) const {
    for (const Chunk& c : chunks_) {
        DrawParams dp;
        dp.texture = c.texture;
        dp.unlit = true;  // the per-cell lightmap is the lighting, as in the original
        r.draw(c.mesh, Mat4::identity(), dp);
    }
}

bool Terrain::heightAt(float rx, float rz, float& outY) const {
    if (t_.gridW == 0) return false;
    const float gx = -rx / gGameUnitsToMeters;
    const float gy = rz / gGameUnitsToMeters;
    const float fx = (gx - t_.minX) / t_.stretch + 1.0f;
    const float fy = (gy - t_.minY) / t_.stretch + 1.0f;
    if (fx < 0 || fy < 0) return false;
    const uint32_t x = uint32_t(fx), y = uint32_t(fy);
    if (!t_.valid(x, y) || !t_.valid(x + 1, y) || !t_.valid(x, y + 1) || !t_.valid(x + 1, y + 1)) return false;
    const float u = fx - x, v = fy - y;
    const float h = (1 - u) * (1 - v) * t_.h(x, y) + u * (1 - v) * t_.h(x + 1, y) +
                    (1 - u) * v * t_.h(x, y + 1) + u * v * t_.h(x + 1, y + 1);
    outY = h * gGameUnitsToMeters;
    return true;
}

Vec3 Terrain::suggestSpawn() const {
    // Land cell (height well above sea level) with the flattest neighbourhood closest to the
    // centroid of all land cells.
    double sx = 0, sy = 0;
    int n = 0;
    for (uint32_t y = 1; y + 1 < t_.gridH; ++y)
        for (uint32_t x = 1; x + 1 < t_.gridW; ++x)
            if (t_.valid(x, y) && t_.h(x, y) > 5.0f) { sx += x; sy += y; ++n; }
    if (n == 0) return {0, 0, 0};
    const float cx = float(sx / n), cy = float(sy / n);
    float best = 1e30f;
    uint32_t bx = 0, by = 0;
    for (uint32_t y = 2; y + 2 < t_.gridH; ++y)
        for (uint32_t x = 2; x + 2 < t_.gridW; ++x) {
            if (!t_.valid(x, y) || t_.h(x, y) <= 5.0f) continue;
            float slope = 0;
            bool ok = true;
            for (int dy = -1; dy <= 1 && ok; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!t_.valid(x + dx, y + dy)) { ok = false; break; }
                    slope = std::max(slope, std::fabs(t_.h(x + dx, y + dy) - t_.h(x, y)));
                }
            if (!ok || slope > t_.stretch * 0.3f) continue;
            const float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d < best) { best = d; bx = x; by = y; }
        }
    if (best == 1e30f) return {0, 0, 0};
    return giantsToRuntime(t_.worldX(bx), t_.worldY(by), t_.h(bx, by));
}

}  // namespace gvr
