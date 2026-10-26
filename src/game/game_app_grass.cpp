// GameApp, rework: grass tufts on the ground near the player (atmosphere and depth up close in VR).
// The original ground is one flat texture; here crossed blade cards stand on gentle, grassy ground
// within ~15 m. Rework 2 (user: "Grass looks awful"): the tufts were bright, uniform sprites on
// darker ground. Now each blade takes the colour of the ground it grows from - the grass material
// (renderer Material::Grass) runs the terrain's own colour function at the tuft's ground texture
// coordinate, lit by the ground's baked light there - with darker roots and lighter, varied tips;
// tufts grow in clumps and patches (low-frequency noise) with bare ground between, three cards each
// so they read from above; thin blades with alpha-to-coverage on the headset's MSAA. Positions come
// from a hash of a 0.55 m grid, so they stay put as you move; the batch is rebuilt only when you've
// moved 2 m. Not as Kabuto (too small to matter) or on worlds whose ground isn't green.
// GVR_NO_GRASS=1: off.
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
constexpr float kCell = 0.55f;
constexpr float kRadius = 15.0f;
constexpr size_t kMaxQuads = 3600;
uint32_t hash2(int x, int z) {
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(z) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
float h01(uint32_t h, int k) { return float((h >> (k * 4)) & 0xFFFu) / 4095.0f; }
// Smooth value noise (0..1) on a grid of `cell` metres: where the grass grows thick and where thin.
float vnoise(float x, float z, float cell, int salt) {
    const float fx = x / cell, fz = z / cell;
    const int ix = int(std::floor(fx)), iz = int(std::floor(fz));
    float u = fx - float(ix), v = fz - float(iz);
    u = u * u * (3 - 2 * u);
    v = v * v * (3 - 2 * v);
    auto n = [&](int a, int b) { return h01(hash2(a * 31 + salt, b * 17 - salt), 0); };
    return (n(ix, iz) * (1 - u) + n(ix + 1, iz) * u) * (1 - v) + (n(ix, iz + 1) * (1 - u) + n(ix + 1, iz + 1) * u) * v;
}
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
    LOGI("Grass: ground %s average (%.2f %.2f %.2f) -> %s", g.c_str(), grassColor_.x, grassColor_.y, grassColor_.z, green ? "grass" : "dry tufts unless snow");
    // Rework 2: other ground (desert sand, mud, ash) gets sparse dry tufts instead - the blades take that
    // ground's colour, so they read as dry grass and scrub; snow and ice (bright, blue) get none.
    const double lum = 0.3 * r + 0.59 * gg + 0.11 * b;
    grassDry_ = !green;
    if (!green && (b > gg * 1.05 || lum > 190.0)) return;
    if (!grassTex_) {
        // Blades: a tuft of ~34 thin, tapering, curving blades leaning out from the middle; each its
        // own shade and hue (fresh green, yellowed, a few dry); dark at the root (the ground's shade),
        // lighter toward the tip. rgb around 0.4-0.85: the shader scales it by ~1.25-1.75 over the
        // ground colour. Coverage-antialiased edges (alpha), so mip levels stay smooth.
        const int W = 128, H = 128;
        std::vector<float> acc(size_t(W) * H * 4, 0.0f);
        uint32_t seed = 4711u;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float((seed >> 8) & 0xFFFF) / 65535.0f; };
        for (int k = 0; k < 34; ++k) {
            const float side = rnd() * 2.0f - 1.0f;                      // -1 .. 1 across the tuft
            const float x0 = float(W) * (0.5f + 0.3f * side);
            const float lean = side * float(W) * (0.12f + 0.2f * rnd()) + (rnd() - 0.5f) * 14.0f;
            const float hgt = float(H) * (0.5f + 0.48f * rnd()) * (1.0f - 0.3f * std::fabs(side));
            const float w0 = 1.3f + rnd() * 1.4f;
            const float shade = 0.82f + 0.3f * rnd();
            const float kind = rnd();
            float hr = 0.9f, hg = 1.0f, hb = 0.62f;                     // fresh green (on the ground's colour)
            if (kind > 0.72f) { hr = 1.08f; hg = 1.0f; hb = 0.5f; }     // yellowed
            if (kind > 0.92f) { hr = 1.2f; hg = 0.96f; hb = 0.62f; }    // dry
            for (int y = 0; y < int(hgt); ++y) {
                const float t = float(y) / hgt;
                const float cx = x0 + lean * t * t * (1.4f - 0.4f * t);
                const float hw = w0 * (1.0f - t * t) + 0.25f;
                for (int x = int(cx - hw - 2); x <= int(cx + hw + 2); ++x) {
                    if (x < 0 || x >= W) continue;
                    const float d = std::fabs(float(x) + 0.5f - cx);
                    const float cov = std::max(0.0f, std::min(1.0f, hw + 0.5f - d));
                    if (cov <= 0) continue;
                    float* p = &acc[size_t((H - 1 - y) * W + x) * 4];
                    const float v = (0.42f + 0.43f * std::pow(t, 0.7f)) * shade;
                    // Blades in front cover the ones behind (later ones drawn over).
                    p[0] = p[0] * (1 - cov) + v * hr * 0.75f * cov;
                    p[1] = p[1] * (1 - cov) + v * hg * 0.75f * cov;
                    p[2] = p[2] * (1 - cov) + v * hb * 0.75f * cov;
                    p[3] = std::max(p[3], cov);
                }
            }
        }
        // Colour where uncovered = the blades' mean, so the smaller mip levels don't darken them.
        double mr = 0, mg = 0, mb = 0, mw = 0;
        for (size_t i = 0; i < acc.size(); i += 4)
            if (acc[i + 3] > 0.5f) { mr += acc[i] / acc[i + 3]; mg += acc[i + 1] / acc[i + 3]; mb += acc[i + 2] / acc[i + 3]; mw += 1; }
        if (mw > 0) { mr /= mw; mg /= mw; mb /= mw; }
        std::vector<uint8_t> px(acc.size());
        for (size_t i = 0; i < acc.size(); i += 4) {
            const float a = acc[i + 3];
            const double c[3] = {a > 0.02f ? acc[i] / a : mr, a > 0.02f ? acc[i + 1] / a : mg, a > 0.02f ? acc[i + 2] / a : mb};
            for (int k = 0; k < 3; ++k) px[i + size_t(k)] = uint8_t(std::min(255.0, c[k] * 255.0));
            px[i + 3] = uint8_t(std::min(255.0f, a * 255.0f));
        }
        grassTex_ = renderer_.uploadTexture(W, H, px.data(), true, false);
        grassBatch_.init(renderer_, kMaxQuads);
    }
    renderer_.setGrassTexture(grassTex_);
    grassOn_ = true;
}

void GameApp::updateGrass() {
    if (assetWorld_.get() != grassWorld_) {  // a new world: decide again
        grassWorld_ = assetWorld_.get();
        initGrass();
    }
    if (hero_ == Hero::Kabuto || !assetWorld_ || std::getenv("GVR_NO_GRASS")) return;
    const Vec3 c = player_.position;
    // The adaptive quality fallback's first step (level 1) keeps a smaller patch (42 % of the cards).
    const float radius = kRadius * (quality_ >= 1 ? 0.65f : 1.0f);
    if (length(Vec3{c.x - grassCentre_.x, 0, c.z - grassCentre_.z}) < 2.0f && radius == grassRadius_) return;
    grassRadius_ = radius;
    grassCentre_ = c;
    grassBatch_.clear();
    if (!grassOn_) {  // (snow and ice: no tufts, but stones)
        grassBatch_.upload(renderer_);
        buildPebbles(c, radius);
        return;
    }
    const Terrain& terr = assetWorld_->terrain();
    const int r = int(radius / kCell);
    const int cx = int(std::floor(c.x / kCell)), cz = int(std::floor(c.z / kCell));
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx) {
            if (grassBatch_.count() + 3 > kMaxQuads) break;
            const float dd = std::sqrt(float(dx * dx + dz * dz)) * kCell;
            if (dd > radius) continue;
            const uint32_t h = hash2(cx + dx, cz + dz);
            const float x = (float(cx + dx) + h01(h, 1)) * kCell, z = (float(cz + dz) + h01(h, 2)) * kCell;
            // Clumps (~3 m) within patches (~14 m): thick in places, bare ground between.
            const float dens = 0.15f + 1.1f * vnoise(x, z, 14.0f, 7) * vnoise(x, z, 3.2f, 3);
            if (h01(h, 0) > (grassDry_ ? dens * 0.3f : dens)) continue;
            float y, yx, yz;
            if (!terr.heightAt(x, z, y) || !terr.heightAt(x + 0.5f, z, yx) || !terr.heightAt(x, z + 0.5f, yz)) continue;
            if (y < renderer_.environment().seaLevel + 1.2f) continue;  // no grass on the beach or under water
            const float slope = std::sqrt((yx - y) * (yx - y) + (yz - y) * (yz - y)) / 0.5f;
            if (slope > 0.5f) continue;  // cliffs and steep banks: rock
            // Smaller toward the edge of the patch (no hard line where it ends), and in thin places.
            const float edge = 1.0f - std::max(0.0f, (dd - radius * 0.7f) / (radius * 0.3f));
            const float size = (0.2f + 0.3f * h01(h, 3) * h01(h, 3) + 0.14f * std::min(1.0f, dens)) * (0.35f + 0.65f * edge);
            const float a = h01(h, 4) * 3.14159f;
            const Vec3 base{x, y - 0.02f, z};
            const Vec2 guv = terr.uvAt(x, z);
            const Vec3 li = terr.lightAt(x, z);
            const float tone = h01(h, 5);
            const uint8_t lc[4] = {uint8_t(std::min(255.0f, li.x * 255.0f)), uint8_t(std::min(255.0f, li.y * 255.0f)),
                                   uint8_t(std::min(255.0f, li.z * 255.0f)), 255};
            for (int k = 0; k < 3; ++k) {  // three cards, 60 degrees apart
                const float aa = a + float(k) * 1.0472f;
                const Vec3 side{std::cos(aa) * size * 0.55f, 0, std::sin(aa) * size * 0.55f};
                const Vec3 up{0, size, 0};
                const Vec3 q[4] = {base - side, base + side, base + side + up, base - side + up};
                static const float bc[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                Vertex v[4];
                for (int c = 0; c < 4; ++c) {
                    v[c] = Vertex{};
                    v[c].pos[0] = q[c].x; v[c].pos[1] = q[c].y; v[c].pos[2] = q[c].z;
                    v[c].normal[0] = bc[c][0]; v[c].normal[1] = bc[c][1]; v[c].normal[2] = tone;
                    v[c].uv[0] = guv.x; v[c].uv[1] = guv.y;
                    for (int i = 0; i < 4; ++i) v[c].color[i] = lc[i];
                }
                grassBatch_.addVertices(v);
            }
        }
    grassBatch_.upload(renderer_);
    buildPebbles(c, radius);
}

void GameApp::renderGrass() {
    renderPebbles();
    // Only with the rework look at full quality (classic look and the adaptive fallback: none).
    if (!grassOn_ || hero_ == Hero::Kabuto || grassBatch_.count() == 0 || renderer_.inReflectionPass() || renderer_.look() != 1) return;
    DrawParams dp;
    dp.texture = assetWorld_ ? assetWorld_->terrain().groundTexture() : 0;  // (the ground's colour; the blades: setGrassTexture)
    dp.material = DrawParams::Material::Grass;
    dp.unlit = true;  // the ground's baked light, as the terrain
    dp.blend = BlendMode::AlphaTest;
    dp.alphaCoverage = true;
    dp.doubleSided = true;
    dp.sway = 0.07f;
    grassBatch_.draw(renderer_, dp);
}


// --- Stones (rework graphics pass) -----------------------------------------------------------------
namespace {
constexpr size_t kMaxPebbles = 260;
constexpr int kPebbleTris = 20;  // a squashed, jittered icosahedron
// The unit icosahedron.
void icosa(std::vector<Vec3>& v, std::vector<int>& f) {
    const float t = 1.618034f;
    v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (auto& p : v) p = normalize(p);
    f = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
         3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
}
}  // namespace

void GameApp::buildPebbles(const Vec3& c, float radius) {
    if (!assetWorld_) return;
    if (!pebbleMesh_) {
        pebbleVerts_ = kMaxPebbles * kPebbleTris * 3;
        MeshData md;
        md.vertices.assign(pebbleVerts_, Vertex{});
        for (size_t i = 0; i < pebbleVerts_; ++i) md.indices.push_back(uint32_t(i));
        pebbleMesh_ = renderer_.uploadMesh(md);
        pebbleBuf_.assign(pebbleVerts_, Vertex{});
    }
    static std::vector<Vec3> iv;
    static std::vector<int> ifc;
    if (iv.empty()) icosa(iv, ifc);
    const Terrain& terr = assetWorld_->terrain();
    size_t n = 0;
    const float cell = 1.3f;
    const int r = int(radius / cell);
    const int cx = int(std::floor(c.x / cell)), cz = int(std::floor(c.z / cell));
    for (int dz = -r; dz <= r && n < kMaxPebbles; ++dz)
        for (int dx = -r; dx <= r && n < kMaxPebbles; ++dx) {
            const float dd = std::sqrt(float(dx * dx + dz * dz)) * cell;
            if (dd > radius) continue;
            const uint32_t h = hash2((cx + dx) * 7 + 3, (cz + dz) * 13 - 5);
            // Stony patches (~9 m) - more on bare ground, a few anywhere.
            const float x = (float(cx + dx) + h01(h, 1)) * cell, z = (float(cz + dz) + h01(h, 2)) * cell;
            const float patch = vnoise(x, z, 9.0f, 21);
            if (h01(h, 0) > 0.06f + 0.5f * patch * patch) continue;
            float y, yx, yz;
            if (!terr.heightAt(x, z, y) || !terr.heightAt(x + 0.4f, z, yx) || !terr.heightAt(x, z + 0.4f, yz)) continue;
            if (y < renderer_.environment().seaLevel - 0.3f) continue;
            const float slope = std::sqrt((yx - y) * (yx - y) + (yz - y) * (yz - y)) / 0.4f;
            if (slope > 0.9f) continue;
            const float edge = 1.0f - std::max(0.0f, (dd - radius * 0.75f) / (radius * 0.25f));
            float size = (0.12f + 0.3f * h01(h, 3) * h01(h, 3)) * (h01(h, 4) > 0.93f ? 2.4f : 1.0f) * (0.3f + 0.7f * edge);
            const float a = h01(h, 5) * 6.2832f;
            const float ca = std::cos(a), sa = std::sin(a);
            const Vec3 sc{size * (0.8f + 0.5f * h01(h, 6)), size * (0.6f + 0.4f * h01(h, 7)), size * (0.8f + 0.5f * h01(h, 4))};
            const Vec3 base{x, y + 0.2f * sc.y, z};  // (a fifth sunk into the ground)
            const Vec2 guv = terr.uvAt(x, z);
            const Vec3 li = terr.lightAt(x, z);
            const uint8_t lc[4] = {uint8_t(std::min(255.0f, li.x * 235.0f)), uint8_t(std::min(255.0f, li.y * 235.0f)),
                                   uint8_t(std::min(255.0f, li.z * 235.0f)), 255};
            // Jittered corners (per stone), then flat-shaded faces.
            Vec3 cv[12];
            for (int k = 0; k < 12; ++k) {
                const float j = 0.75f + 0.5f * h01(hash2(int(h & 0xFFFF) + k, int(h >> 16)), 0);
                const Vec3 p = iv[size_t(k)] * j;
                const Vec3 q{p.x * sc.x, p.y * sc.y, p.z * sc.z};
                cv[k] = base + Vec3{q.x * ca - q.z * sa, q.y, q.x * sa + q.z * ca};
            }
            Vertex* out = &pebbleBuf_[n * kPebbleTris * 3];
            for (int t = 0; t < kPebbleTris; ++t) {
                Vec3 A = cv[ifc[size_t(t * 3)]], B = cv[ifc[size_t(t * 3 + 1)]], C = cv[ifc[size_t(t * 3 + 2)]];
                Vec3 nrm = normalize(cross(B - A, C - A));
                if (dot(nrm, (A + B + C) * (1.0f / 3.0f) - base) < 0) { std::swap(B, C); nrm = nrm * -1.0f; }  // every face outward
                const Vec3 P[3] = {A, B, C};
                for (int k = 0; k < 3; ++k) {
                    Vertex& v = out[t * 3 + k];
                    v.pos[0] = P[k].x; v.pos[1] = P[k].y; v.pos[2] = P[k].z;
                    v.normal[0] = nrm.x; v.normal[1] = nrm.y; v.normal[2] = nrm.z;
                    v.uv[0] = guv.x; v.uv[1] = guv.y;
                    for (int i = 0; i < 4; ++i) v.color[i] = lc[i];
                }
            }
            ++n;
        }
    // Unused stones collapse to nothing.
    const size_t used = n * kPebbleTris * 3;
    for (size_t i = used; i < std::max(used, pebbleUsed_); ++i) pebbleBuf_[i] = Vertex{};
    const size_t upto = std::max(used, pebbleUsed_);
    if (upto) renderer_.updateMeshVertexRange(pebbleMesh_, 0, pebbleBuf_.data(), upto);
    pebbleUsed_ = used;
    static int logged = 0;
    if (logged < 2) { ++logged; LOGI("Stones: %zu near the player", n); }
}

void GameApp::renderPebbles() {
    if (!pebbleMesh_ || pebbleUsed_ == 0 || hero_ == Hero::Kabuto || renderer_.inReflectionPass() || renderer_.look() == 0) return;  // (one draw: also at the fallback quality)
    DrawParams dp;
    dp.texture = assetWorld_ ? assetWorld_->terrain().groundTexture() : 0;
    dp.material = DrawParams::Material::Stone;
    dp.unlit = true;  // baked like the ground
    renderer_.draw(pebbleMesh_, Mat4::identity(), dp);
}

}  // namespace gvr
