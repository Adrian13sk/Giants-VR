// GameApp, rework: grass tufts on the ground near the player (atmosphere and depth up close in VR).
// The original ground is one flat texture; here crossed blade cards stand on gentle, dry, grassy
// ground within ~16 m, coloured from the world's own ground texture. Positions come from a hash of
// a 0.9 m grid, so they stay put as you move; the batch is rebuilt only when you've moved 2 m.
// Not as Kabuto (too small to matter) or on worlds whose ground isn't green. GVR_NO_GRASS=1: off.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "assets/image.h"
#include "scene/terrain.h"
#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
constexpr float kCell = 0.9f;
constexpr float kRadius = 16.0f;
constexpr size_t kMaxQuads = 2600;
uint32_t hash2(int x, int z) {
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(z) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
float h01(uint32_t h, int k) { return float((h >> (k * 4)) & 0xFFFu) / 4095.0f; }
}  // namespace

void GameApp::initGrass() {
    grassOn_ = false;
    grassCentre_ = Vec3{1e9f, 0, 1e9f};
    if (!assetWorld_ || std::getenv("GVR_NO_GRASS")) return;
    // The world's ground colour: green enough for grass?
    Image img;
    const std::string g = assetWorld_->world().groundTexture;
    if (g.empty() || !assetWorld_->models()->image(g, img) || img.rgba.empty()) return;
    double r = 0, gg = 0, b = 0;
    const size_t n = img.rgba.size() / 4;
    for (size_t i = 0; i < n; ++i) { r += img.rgba[i * 4]; gg += img.rgba[i * 4 + 1]; b += img.rgba[i * 4 + 2]; }
    r /= double(n); gg /= double(n); b /= double(n);
    grassColor_ = Vec3{float(r), float(gg), float(b)} * (1.0f / 255.0f);
    // (The ground textures are olive/yellow: the shader's lush tint and the lightmap make them green.
    // Vegetation: green well above blue and not far below red; red earth and sand fail.)
    const bool green = gg > 2.5 * b + 0.02 && gg > 0.6 * r && gg > 0.08;
    LOGI("Grass: ground %s average (%.2f %.2f %.2f) -> %s", g.c_str(), grassColor_.x, grassColor_.y, grassColor_.z, green ? "grass" : "none");
    if (!green) return;
    if (!grassTex_) {
        // Blades: tapered, darker at the root, alpha-tested.
        const int W = 64, H = 128;
        std::vector<uint8_t> px(size_t(W) * H * 4, 0);
        uint32_t seed = 12345u;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float((seed >> 8) & 0xFFFF) / 65535.0f; };
        for (int k = 0; k < 18; ++k) {
            const float x0 = 6.0f + rnd() * float(W - 12), lean = (rnd() - 0.5f) * 22.0f, hgt = float(H) * (0.45f + 0.55f * rnd());
            const float w0 = 1.6f + rnd() * 1.8f, shade = 0.75f + 0.5f * rnd();
            for (int y = 0; y < int(hgt); ++y) {
                const float t = float(y) / hgt;
                const float cx = x0 + lean * t * t, hw = w0 * (1.0f - t);
                for (int x = int(cx - hw - 1); x <= int(cx + hw + 1); ++x) {
                    if (x < 0 || x >= W) continue;
                    const float d = std::fabs(float(x) + 0.5f - cx);
                    if (d > hw + 0.5f) continue;
                    uint8_t* p = &px[size_t((H - 1 - y) * W + x) * 4];
                    const float v = std::min(1.0f, (0.55f + 0.6f * t) * shade);
                    p[0] = uint8_t(255.0f * v * 0.92f);
                    p[1] = uint8_t(255.0f * v);
                    p[2] = uint8_t(255.0f * v * 0.78f);
                    p[3] = 255;
                }
            }
        }
        grassTex_ = renderer_.uploadTexture(W, H, px.data(), true, false);
        grassBatch_.init(renderer_, kMaxQuads);
    }
    grassOn_ = true;
}

void GameApp::updateGrass() {
    if (assetWorld_.get() != grassWorld_) {  // a new world: decide again
        grassWorld_ = assetWorld_.get();
        initGrass();
    }
    if (!grassOn_ || hero_ == Hero::Kabuto || !assetWorld_) return;
    const Vec3 c = player_.position;
    if (length(Vec3{c.x - grassCentre_.x, 0, c.z - grassCentre_.z}) < 2.0f) return;
    grassCentre_ = c;
    grassBatch_.clear();
    const Terrain& terr = assetWorld_->terrain();
    const int r = int(kRadius / kCell);
    const int cx = int(std::floor(c.x / kCell)), cz = int(std::floor(c.z / kCell));
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx) {
            if (grassBatch_.count() + 2 > kMaxQuads) break;
            const float dd = std::sqrt(float(dx * dx + dz * dz)) * kCell;
            if (dd > kRadius) continue;
            const uint32_t h = hash2(cx + dx, cz + dz);
            if (h01(h, 0) > 0.8f) continue;  // density
            const float x = (float(cx + dx) + h01(h, 1)) * kCell, z = (float(cz + dz) + h01(h, 2)) * kCell;
            float y, yx, yz;
            if (!terr.heightAt(x, z, y) || !terr.heightAt(x + 0.5f, z, yx) || !terr.heightAt(x, z + 0.5f, yz)) continue;
            if (y < renderer_.environment().seaLevel + 1.2f) continue;  // no grass on the beach or under water
            const float slope = std::sqrt((yx - y) * (yx - y) + (yz - y) * (yz - y)) / 0.5f;
            if (slope > 0.55f) continue;  // cliffs and steep banks: rock
            // Smaller toward the edge of the patch (no hard line where it ends).
            const float edge = 1.0f - std::max(0.0f, (dd - kRadius * 0.7f) / (kRadius * 0.3f));
            const float size = (0.22f + 0.36f * h01(h, 3) * h01(h, 3)) * (0.4f + 0.6f * edge);
            const float a = h01(h, 4) * 3.14159f;
            const Vec3 base{x, y - 0.03f, z};
            // The ground's own colour there (texture average x baked light), a little lusher, varied.
            const float tone = 0.8f + 0.45f * h01(h, 5);
            const Vec3 li = terr.lightAt(x, z);
            const Vec3 col = Vec3{grassColor_.x * li.x * 0.95f, grassColor_.y * li.y * 1.08f, grassColor_.z * li.z * 0.9f} * (tone * 1.2f);
            const uint8_t rgba[4] = {uint8_t(std::min(255.0f, col.x * 255.0f)), uint8_t(std::min(255.0f, col.y * 255.0f)),
                                     uint8_t(std::min(255.0f, col.z * 255.0f)), 255};
            for (int k = 0; k < 2; ++k) {  // two crossed cards
                const float aa = a + float(k) * 1.5708f;
                const Vec3 side{std::cos(aa) * size * 0.5f, 0, std::sin(aa) * size * 0.5f};
                const Vec3 up{0, size, 0};
                const Vec3 q[4] = {base - side, base + side, base + side + up, base - side + up};
                static const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
                grassBatch_.add(q, uv, rgba, Vec3{0, 1, 0});  // lit like the ground
            }
        }
    grassBatch_.upload(renderer_);
}

void GameApp::renderGrass() {
    // Only with the rework look at full quality (classic look and the adaptive fallback: none).
    if (!grassOn_ || hero_ == Hero::Kabuto || grassBatch_.count() == 0 || renderer_.inReflectionPass() || renderer_.look() != 1) return;
    DrawParams dp;
    dp.texture = grassTex_;
    dp.blend = BlendMode::AlphaTest;
    dp.doubleSided = true;
    grassBatch_.draw(renderer_, dp);
}

}  // namespace gvr
