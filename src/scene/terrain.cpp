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

    // Smooth vertex normals from the heightfield (central differences), so the slope-based
    // texture blend (rock on slopes) is smooth across triangles instead of faceted.
    auto smoothNormal = [&](uint32_t x, uint32_t y) {
        auto hp = [&](int xx, int yy) {
            xx = std::max(0, std::min(int(t.gridW) - 1, xx));
            yy = std::max(0, std::min(int(t.gridH) - 1, yy));
            if (!t.valid(uint32_t(xx), uint32_t(yy))) return giantsToRuntime(t.worldX(x), t.worldY(y), t.h(x, y));
            return giantsToRuntime(t.worldX(uint32_t(xx)), t.worldY(uint32_t(yy)), t.h(uint32_t(xx), uint32_t(yy)));
        };
        const Vec3 dx = hp(int(x) + 1, int(y)) - hp(int(x) - 1, int(y));
        const Vec3 dy = hp(int(x), int(y) + 1) - hp(int(x), int(y) - 1);
        Vec3 n = normalize(cross(dx, dy));
        if (n.y < 0) n = n * -1.0f;
        return n;
    };
    for (uint32_t cy = 0; cy + 1 < t.gridH; cy += kChunk) {
        for (uint32_t cx = 0; cx + 1 < t.gridW; cx += kChunk) {
            MeshData mesh;
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
                        const uint32_t base = uint32_t(mesh.vertices.size());
                        for (int k = 0; k < 3; ++k) {
                            Vertex v = vert(tri[k][0], tri[k][1]);
                            const Vec3 n = smoothNormal(tri[k][0], tri[k][1]);
                            v.normal[0] = n.x; v.normal[1] = n.y; v.normal[2] = n.z;
                            mesh.vertices.push_back(v);
                        }
                        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
                        ++tris_;
                    }
                }
            }
            if (mesh.indices.empty()) continue;
            Chunk c;
            c.mesh = r.uploadMesh(mesh);
            c.texture = ground ? ground : wall;
            chunks_.push_back(c);
        }
    }
    (void)kWallSlopeY;
    LOGI("Terrain %s: %ux%u cells, %u triangles, %zu chunks", t.name.c_str(), t.tilesX, t.tilesY, tris_,
         chunks_.size());
    return !chunks_.empty();
}

void Terrain::render(Renderer& r) const {
    for (const Chunk& c : chunks_) {
        DrawParams dp;
        dp.texture = c.texture;
        dp.unlit = true;  // the per-cell lightmap is the lighting, as in the original
        dp.material = DrawParams::Material::Terrain;
        r.draw(c.mesh, Mat4::identity(), dp);
    }
}

void Terrain::release(Renderer& r) {
    for (const Chunk& c : chunks_) r.destroyMesh(c.mesh);
    chunks_.clear();
    t_ = GtiTerrain{};
    tris_ = 0;
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
    // Same two triangles as the rendered mesh (diagonal from the cell's triangle code), so the
    // ground height matches the visible surface exactly (bilinear was metres off on cliffs).
    const float u = fx - x, v = fy - y;
    const float h00 = t_.h(x, y), h10 = t_.h(x + 1, y), h01 = t_.h(x, y + 1), h11 = t_.h(x + 1, y + 1);
    const bool altDiag = (t_.triangles[size_t(y) * t_.gridW + x] & 4) != 0;
    float h;
    if (altDiag) {  // diagonal (0,0)-(1,1): triangles (00,10,11) and (00,11,01)
        h = u >= v ? h00 + u * (h10 - h00) + v * (h11 - h10) : h00 + v * (h01 - h00) + u * (h11 - h01);
    } else {        // diagonal (1,0)-(0,1): triangles (00,10,01) and (10,11,01)
        h = u + v <= 1.0f ? h00 + u * (h10 - h00) + v * (h01 - h00)
                          : h11 + (1.0f - u) * (h01 - h11) + (1.0f - v) * (h10 - h11);
    }
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

// Tileable noise for the terrain blends (our own): R = fractal value noise (patches), G = finer
// grain; size x size RGBA.
std::vector<uint8_t> makeTerrainNoise(int size, uint32_t seed) {
    auto lattice = [&](int i, int j, int cells, uint32_t sd) {
        i = ((i % cells) + cells) % cells;
        j = ((j % cells) + cells) % cells;
        uint32_t v = uint32_t(i) * 374761393u + uint32_t(j) * 668265263u + sd * 2246822519u;
        v = (v ^ (v >> 13)) * 1274126177u;
        return float((v ^ (v >> 16)) & 0xffffu) / 65535.0f;
    };
    auto noise = [&](float u, float v, int cells, uint32_t sd) {
        const float fx = u * float(cells), fy = v * float(cells);
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        float tx = fx - float(ix), ty = fy - float(iy);
        tx = tx * tx * (3 - 2 * tx);
        ty = ty * ty * (3 - 2 * ty);
        const float a = lattice(ix, iy, cells, sd), b = lattice(ix + 1, iy, cells, sd);
        const float c = lattice(ix, iy + 1, cells, sd), d = lattice(ix + 1, iy + 1, cells, sd);
        return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
    };
    std::vector<uint8_t> out(size_t(size) * size_t(size) * 4, 255);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float u = float(x) / float(size), v = float(y) / float(size);
            const float r = 0.5f * noise(u, v, 4, seed) + 0.3f * noise(u, v, 8, seed + 1) + 0.2f * noise(u, v, 16, seed + 2);
            const float g = 0.6f * noise(u, v, 32, seed + 3) + 0.4f * noise(u, v, 64, seed + 4);
            const size_t o = (size_t(y) * size_t(size) + size_t(x)) * 4;
            out[o + 0] = uint8_t(std::min(1.0f, std::max(0.0f, r)) * 255.0f);
            out[o + 1] = uint8_t(std::min(1.0f, std::max(0.0f, g)) * 255.0f);
            out[o + 2] = 128;
        }
    return out;
}

}  // namespace gvr
