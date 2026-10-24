// The map (the original's C key), made for VR: Y opens a holographic table model of the island
// floating at waist height in front of the player - the real terrain in miniature (heights
// exaggerated), coloured sea / sand / grass / rock with a faint holo tint, and live: the player (blue,
// with a pointer for his facing), Rippers and Reapers (red / orange), Smarties (yellow), nests,
// the shop, and the open objectives as dots circling their place with a beam rising from it. Like
// the original's map it starts dark: ground shows only once the player has been near it (fog of
// war, kept per world for the session). The map is aligned with the world (what lies ahead of you
// lies ahead of your marker) and moves along with the player while open; Y again closes it.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "assets/gtext.h"
#include "core/log.h"
#include "scene/asset_world.h"

namespace gvr {

namespace {
constexpr int kMapN = 97;              // vertices per side
constexpr float kMapSize = 0.9f;       // metres across the table
constexpr float kMapLift = 2.5f;       // height exaggeration
constexpr float kExploreRadius = 90.0f;
}  // namespace

void GameApp::mapReset() {
    if (mapMesh_) renderer_.destroyMesh(mapMesh_);
    mapMesh_ = 0;
    mapVerts_.clear();
    mapBase_.clear();
    explored_.clear();
    mapWorld_.clear();
    mapOpen_ = false;
}

bool GameApp::buildMapMesh() {
    if (!assetWorld_ || !assetWorld_->loaded()) return false;
    const Terrain& t = assetWorld_->terrain();
    // The table shows the land (and a margin of sea round it), not the whole sea of the terrain.
    Vec3 mn = t.boundsMax(), mx = t.boundsMin();
    {
        const Vec3 a = t.boundsMin(), b = t.boundsMax();
        for (float z = a.z; z <= b.z; z += 8.0f)
            for (float x = a.x; x <= b.x; x += 8.0f) {
                float h;
                if (!t.heightAt(x, z, h) || h < -0.3f) continue;
                mn = Vec3{std::min(mn.x, x), 0, std::min(mn.z, z)};
                mx = Vec3{std::max(mx.x, x), 0, std::max(mx.z, z)};
            }
        if (mx.x <= mn.x) { mn = a; mx = b; }
        mn = Vec3{std::max(a.x, mn.x - 60.0f), 0, std::max(a.z, mn.z - 60.0f)};
        mx = Vec3{std::min(b.x, mx.x + 60.0f), 0, std::min(b.z, mx.z + 60.0f)};
    }
    const float w = mx.x - mn.x, d = mx.z - mn.z;
    if (w < 10.0f || d < 10.0f) return false;
    mapMin_ = mn;
    mapMax_ = mx;
    mapScale_ = kMapSize / std::max(w, d);
    MeshData md;
    md.vertices.resize(size_t(kMapN) * kMapN);
    mapBase_.assign(md.vertices.size(), Vec3{0, 0, 0});
    std::vector<float> hs(md.vertices.size(), -30.0f);
    for (int j = 0; j < kMapN; ++j)
        for (int i = 0; i < kMapN; ++i) {
            const float x = mn.x + w * float(i) / float(kMapN - 1), z = mn.z + d * float(j) / float(kMapN - 1);
            float h;
            if (!t.heightAt(x, z, h)) h = -30.0f;
            hs[size_t(j) * kMapN + size_t(i)] = h;
        }
    for (int j = 0; j < kMapN; ++j)
        for (int i = 0; i < kMapN; ++i) {
            const size_t k = size_t(j) * kMapN + size_t(i);
            const float h = hs[k];
            const float x = mn.x + w * float(i) / float(kMapN - 1), z = mn.z + d * float(j) / float(kMapN - 1);
            Vertex& v = md.vertices[k];
            const float shown = std::max(h, -1.5f);  // the sea bed flattened just under the surface
            v.pos[0] = (x - (mn.x + mx.x) * 0.5f) * mapScale_;
            v.pos[1] = shown * mapScale_ * kMapLift;
            v.pos[2] = (z - (mn.z + mx.z) * 0.5f) * mapScale_;
            v.normal[0] = 0; v.normal[1] = 1; v.normal[2] = 0;
            v.uv[0] = v.uv[1] = 0;
            // Slope from the neighbours (rock where steep).
            const float hl = hs[k - (i > 0 ? 1 : 0)], hr = hs[k + (i < kMapN - 1 ? 1 : 0)];
            const float hd = hs[k - (j > 0 ? size_t(kMapN) : 0)], hu = hs[k + (j < kMapN - 1 ? size_t(kMapN) : 0)];
            const float cell = w / float(kMapN - 1);
            const float slope = std::sqrt((hr - hl) * (hr - hl) + (hu - hd) * (hu - hd)) / (2.0f * cell);
            Vec3 c;
            if (h < -0.3f) {
                const float k2 = std::min(1.0f, -h / 15.0f);
                c = Vec3{0.08f, 0.28f, 0.38f} * (1.0f - k2) + Vec3{0.02f, 0.08f, 0.18f} * k2;
            } else if (h < 1.0f) {
                c = Vec3{0.80f, 0.74f, 0.50f};
            } else if (slope > 0.9f) {
                c = Vec3{0.46f, 0.42f, 0.36f};
            } else {
                const float hk = std::min(1.0f, h / 80.0f);
                c = Vec3{0.24f, 0.52f, 0.22f} * (1.0f - hk) + Vec3{0.50f, 0.50f, 0.40f} * hk;
            }
            mapBase_[k] = c * 0.85f + Vec3{0.30f, 0.85f, 1.0f} * 0.15f;  // the holo tint
        }
    for (int j = 0; j + 1 < kMapN; ++j)
        for (int i = 0; i + 1 < kMapN; ++i) {
            const uint32_t a = uint32_t(j * kMapN + i), b = a + 1, c = a + uint32_t(kMapN), e = c + 1;
            md.indices.insert(md.indices.end(), {a, c, b, b, c, e});
        }
    if (explored_.size() != md.vertices.size()) explored_.assign(md.vertices.size(), std::getenv("GVR_MAP_ALL") ? 1 : 0);  // test hook: all explored
    mapVerts_ = md.vertices;
    recolourMap();
    md.vertices = mapVerts_;
    if (mapMesh_) renderer_.destroyMesh(mapMesh_);
    mapMesh_ = renderer_.uploadMesh(md);
    mapWorld_ = worldName_;
    if (!dotTex_) {
        // A soft round dot for the markers.
        const int n = 32;
        std::vector<uint8_t> px(size_t(n) * n * 4);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const float dx = (x + 0.5f) / n * 2 - 1, dy = (y + 0.5f) / n * 2 - 1;
                const float r = std::sqrt(dx * dx + dy * dy);
                const float a = r < 0.6f ? 1.0f : std::max(0.0f, 1.0f - (r - 0.6f) / 0.4f);
                uint8_t* p = &px[(size_t(y) * n + size_t(x)) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = uint8_t(a * 255.0f);
            }
        dotTex_ = renderer_.uploadTexture(n, n, px.data(), true, false);
        mapDots_.init(renderer_, 256);
    }
    LOGI("Map: %d x %d over %.0f x %.0f m (1:%.0f)", kMapN, kMapN, w, d, 1.0f / mapScale_);
    return mapMesh_ != 0;
}

void GameApp::recolourMap() {
    for (size_t k = 0; k < mapVerts_.size() && k < mapBase_.size(); ++k) {
        // Unexplored: a dim holographic ghost of the shape (land a little brighter than sea).
        const float lum = 0.3f * mapBase_[k].x + 0.6f * mapBase_[k].y + 0.1f * mapBase_[k].z;
        const Vec3 c = explored_[k] ? mapBase_[k] : Vec3{0.02f, 0.07f, 0.10f} + Vec3{0.05f, 0.16f, 0.22f} * lum;
        Vertex& v = mapVerts_[k];
        // The colours above are picked as seen; the eye buffers are sRGB, the shader works linear.
        v.color[0] = uint8_t(std::pow(std::min(1.0f, c.x), 2.2f) * 255.0f);
        v.color[1] = uint8_t(std::pow(std::min(1.0f, c.y), 2.2f) * 255.0f);
        v.color[2] = uint8_t(std::pow(std::min(1.0f, c.z), 2.2f) * 255.0f);
        v.color[3] = 255;
    }
}

void GameApp::updateMap(float dt, const InputFrame& in) {
    if (worldName_ != mapWorld_ && (mapMesh_ || !explored_.empty())) mapReset();
    // Exploring: the ground round the player shows on the map from now on.
    exploreT_ -= dt;
    if (exploreT_ <= 0 && playMode_ && assetWorld_ && assetWorld_->loaded()) {
        exploreT_ = 0.25f;
        if (explored_.empty() && buildMapMesh()) mapDirty_ = true;
        if (!explored_.empty()) {
            const float w = mapMax_.x - mapMin_.x, d = mapMax_.z - mapMin_.z;
            const float ci = (player_.position.x - mapMin_.x) / w * float(kMapN - 1), cj = (player_.position.z - mapMin_.z) / d * float(kMapN - 1);
            const int ri = int(kExploreRadius / w * float(kMapN - 1)) + 1, rj = int(kExploreRadius / d * float(kMapN - 1)) + 1;
            for (int j = std::max(0, int(cj) - rj); j <= std::min(kMapN - 1, int(cj) + rj); ++j)
                for (int i = std::max(0, int(ci) - ri); i <= std::min(kMapN - 1, int(ci) + ri); ++i) {
                    const float x = mapMin_.x + w * float(i) / float(kMapN - 1), z = mapMin_.z + d * float(j) / float(kMapN - 1);
                    const float dx = x - player_.position.x, dz = z - player_.position.z;
                    uint8_t& e = explored_[size_t(j) * kMapN + size_t(i)];
                    if (!e && dx * dx + dz * dz < kExploreRadius * kExploreRadius) { e = 1; mapDirty_ = true; }
                }
        }
    }
    // Y (the move hand's upper button) opens / closes it - on foot, not on Kabuto (his roar).
    const bool can = playMode_ && !riding_ && hero_ != Hero::Kabuto && !menuOpen_ && !cutsceneActive() && load_.phase == LoadPhase::Idle;
    const bool press = mapEdge_.pressed(in.hand[rig_.moveHand()].secondary);
    if (!can) { mapOpen_ = false; return; }
    if (press) {
        mapOpen_ = !mapOpen_ && mapMesh_ != 0;
        sfx2D("ph_selct", 0.6f, mapOpen_ ? 1.2f : 0.9f);
        haptics_.push_back({rig_.moveHand(), 0.35f, 0.05f});
        if (mapOpen_) {
            const Pose head = rig_.toWorld(in.head);
            const float yaw = yawOf(head.orientation);
            // In front, below the eyes, relative to the player (it moves with him).
            mapOffset_ = Vec3{head.position.x - player_.position.x, head.position.y - player_.position.y - 0.55f,
                              head.position.z - player_.position.z} +
                         Quat::fromAxisAngle({0, 1, 0}, yaw).rotate(Vec3{0, 0, -0.62f});
            mapOpenT_ = 0;
            LOGI("Map opened");
        }
    }
    if (mapOpen_) mapOpenT_ += dt;
}

Mat4 GameApp::mapTransform() const {
    const float grow = std::min(1.0f, mapOpenT_ / 0.25f);  // unfolds
    const float g = 0.2f + 0.8f * grow * grow * (3.0f - 2.0f * grow);
    return Mat4::translation(player_.position + mapOffset_) * Mat4::scale(Vec3{g, g, g});
}

Vec3 GameApp::mapPoint(const Vec3& world, float lift) const {
    float h = world.y;
    if (assetWorld_) {
        float gy;
        if (assetWorld_->terrain().heightAt(world.x, world.z, gy)) h = std::max(gy, -1.5f);
    }
    const Vec3 local{(world.x - (mapMin_.x + mapMax_.x) * 0.5f) * mapScale_, h * mapScale_ * kMapLift + lift,
                     (world.z - (mapMin_.z + mapMax_.z) * 0.5f) * mapScale_};
    const Vec4 p = mapTransform() * Vec4{local.x, local.y, local.z, 1.0f};
    return {p.x, p.y, p.z};
}

void GameApp::buildMapBatch() {
    mapDots_.clear();
    if (!mapOpen_ || !mapMesh_ || !dotTex_) { mapDots_.upload(renderer_); return; }
    if (mapDirty_) {
        recolourMap();
        renderer_.updateMeshVertices(mapMesh_, mapVerts_);
        mapDirty_ = false;
    }
    const Vec3 right = headWorldRot_.rotate(Vec3{1, 0, 0}), up = headWorldRot_.rotate(Vec3{0, 1, 0});
    const float t = float(time_);
    const float g = std::min(1.0f, mapOpenT_ / 0.25f);
    auto seen = [&](const Vec3& p) {
        const float w = mapMax_.x - mapMin_.x, d = mapMax_.z - mapMin_.z;
        const int i = int((p.x - mapMin_.x) / w * float(kMapN - 1) + 0.5f), j = int((p.z - mapMin_.z) / d * float(kMapN - 1) + 0.5f);
        return i >= 0 && j >= 0 && i < kMapN && j < kMapN && explored_[size_t(j) * kMapN + size_t(i)];
    };
    auto dot = [&](const Vec3& world, float size, uint8_t r, uint8_t gg, uint8_t b, float lift = 0.008f) {
        if (mapDots_.full()) return;
        const uint8_t c[4] = {r, gg, b, 255};
        mapDots_.addBillboard(mapPoint(world, lift), size * g, right, up, 0.0f, c);
    };
    // Objectives still open: three dots circling the place and a beam of dots rising from it.
    for (const auto& o : objectives_) {
        if (o.done) continue;
        const Vec3 c = (o.boundsMin + o.boundsMax) * 0.5f;
        for (int k = 0; k < 3; ++k) {
            const float a = t * 2.4f + 2.094f * float(k);
            const float r = 0.022f / std::max(1e-3f, mapScale_);  // world metres for 2.2 cm on the table
            dot(c + Vec3{std::cos(a) * r, 0, std::sin(a) * r}, 0.012f, 255, 230, 60, 0.012f);
        }
        for (int k = 0; k < 5; ++k) {
            const float h = 0.012f + 0.014f * float(k) + 0.01f * std::fmod(t * 2.0f, 1.0f);
            dot(c, 0.006f, 255, 220, 80, h);
        }
    }
    // Creatures and people, where explored.
    for (const auto& c : creatures_.creatures)
        if (c.state != CreatureState::Dead && !CreatureDirector::underground(c) && seen(c.pos)) dot(c.pos, c.size == CreatureSize::Large ? 0.013f : 0.009f, 255, 60, 50);
    for (const auto& e : enemies_.enemies)
        if (e.state != EnemyState::Dead && seen(e.pos)) dot(e.pos, 0.010f, 255, 140, 40);
    for (const auto& n : nests_)
        if (n.alive && seen(n.pos)) dot(n.pos, 0.012f, 190, 70, 220);
    for (const auto& s : rescue_.smarties)
        if (s.state != SmartieState::Gone && s.state != SmartieState::Saved && seen(s.pos)) dot(s.pos, 0.010f, 255, 210, 40);
    for (const auto& v : villagers_.villagers)
        if (seen(v.pos)) dot(v.pos, 0.007f, 255, 210, 40);
    for (const auto& ci : chars_)
        if (ci.boss && ci.hp > 0 && seen(ci.pos)) dot(ci.pos, 0.016f, 255, 90, 20);
    if (shopOn_) dot(shopCentre_, 0.012f, 80, 255, 120);
    for (const auto& p : healthPickups_) if (seen(p.pos)) dot(p.pos, 0.007f, 80, 255, 120);
    // The player: a blue dot, pulsing, with a pointer for where he faces.
    const float pulse = 0.014f + 0.003f * std::sin(t * 6.0f);
    dot(player_.position, pulse, 70, 170, 255, 0.012f);
    const Vec3 fwd{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)};
    for (int k = 1; k <= 3; ++k) dot(player_.position + fwd * (float(k) * 0.008f / mapScale_), 0.006f, 140, 210, 255, 0.012f);
    mapDots_.upload(renderer_);
}

void GameApp::renderMap() {
    if (!mapOpen_ || !mapMesh_) return;
    DrawParams dp;
    dp.unlit = true;
    dp.noFog = true;
    dp.doubleSided = true;
    renderer_.draw(mapMesh_, mapTransform(), dp);
    DrawParams dd;
    dd.texture = dotTex_;
    dd.blend = BlendMode::AlphaBlend;
    dd.unlit = true;
    dd.noFog = true;
    dd.depthWrite = false;
    dd.doubleSided = true;
    mapDots_.draw(renderer_, dd);
    // The title: the mission's name over the table.
    if (text_.ready() && gameText_ && assetWorld_) {
        const std::string title = gameText_->get(assetWorld_->missionName());
        if (!title.empty()) {
            float w = 0, h = 0;
            const MeshId m = text_.mesh(title, 0.05f, 2.0f, &w, &h);
            if (m) {
                const Pose head = rig_.toWorld(lastInput_.head);
                const Vec3 at = player_.position + mapOffset_ + Vec3{0, 0.22f, 0};
                const Mat4 xf = Mat4::translation(at) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, yawOf(head.orientation)));
                text_.drawPanel(xf, w + 0.04f, h + 0.02f, {0, 0.05f, 0.1f, 0.55f}, true);
                text_.draw(m, xf * Mat4::translation({0, 0, 0.002f}), {0.6f, 0.95f, 1.0f, 1.0f}, true);
            }
        }
    }
}

}  // namespace gvr
