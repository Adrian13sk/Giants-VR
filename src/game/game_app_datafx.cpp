// The game's own effects (fx.bin) played by FxPlayer and drawn here: cutscene effects (flick op 0x27
// names fx.bin entries) first; the stand-in particles remain for what the library doesn't name.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/arm_ik.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

void GameApp::initDataFx() {
    if (!assetWorld_) return;
    if (!fxLibLoaded_) {
        fxLibLoaded_ = true;
        std::vector<uint8_t> d;
        std::string err;
        if (!assetWorld_->fs().read("fx.bin", d) || !parseFxLibrary(d.data(), d.size(), fxLib_, &err))
            LOGW("fx.bin: %s - effects use the stand-ins", err.empty() ? "not found" : err.c_str());
        else
            LOGI("Effect library: %zu effects (fx.bin)", fxLib_.effects.size());
    }
    if (fxLib_.effects.empty()) return;
    // Each world has its own model cache: the player fetches models from this one.
    fxPlayer_.init(&fxLib_, &assetWorld_->registry(), assetWorld_->models(), &assetWorld_->fs());
    const ObjectRegistry* reg = &assetWorld_->registry();
    fxPlayer_.soundName = [reg](uint32_t id) { return reg->soundName(id); };
    fxPlayer_.groundAt = [this](float x, float z, float& y) { return assetWorld_ && assetWorld_->terrain().heightAt(x, z, y); };
}

bool GameApp::dataFxOn() const {
    // Options -> EFFECTS: ORIGINAL (TEST) (or GVR_DATA_FX=1); off by default until seen on the headset.
    static const bool forced = std::getenv("GVR_DATA_FX") != nullptr;
    return settings_.originalFx || forced;
}

namespace {
// The trail each kind of rocket carries in the original (fx.bin, op 24 with the projectile's model):
// 0 rocket, 2 SAM (homing), 3 Reaper bazooka, 4 shotgun shell, 5 Millenium bomb, 7 RPG-bow arrow,
// 8 homing arrow. The fire bolt and the grenade keep ours.
const char* rocketFxName(int kind) {
    switch (kind) {
        case 0: case 3: return "FX_rocket";
        case 2: return "FX_homing";
        case 4: return "FX_shotgun_trail";
        case 5: return "FX_Lbomb";
        case 7: return "FXB_bow_RPG";
        case 8: return "FXB_bow_screamer";
        default: return nullptr;
    }
}
}  // namespace

void GameApp::syncRocketFx() {
    const bool on = dataFxOn() && fxPlayer_.ready();
    static int logged = 0;
    for (auto& r : rockets_) {
        const bool gone = r.life >= 1e9f;
        if (gone || !on) {
            if (rocketTrail(r)) fxPlayer_.detach(r.fx);
            r.fx = 0;
            continue;
        }
        const Quat q = rotationBetween({0, 0, -1}, length(r.vel) > 1e-3f ? r.vel : Vec3{0, 0, -1});
        if (r.fx == 0) {
            const char* name = rocketFxName(r.kind);
            r.fx = name ? fxPlayer_.attach(name, r.pos, q) : 0;
            if (r.fx && logged < 6) { ++logged; LOGI("Trail %s (fx.bin) on a kind-%d projectile", name, r.kind); }
            if (!r.fx) r.fx = kNoFx;
        } else if (r.fx != kNoFx && !fxPlayer_.moveAttached(r.fx, r.pos, q)) {
            r.fx = kNoFx;
        }
    }
}

void GameApp::buildStreaks(const Vec3& eye) {
    streakBatch_.clear();
    for (const auto& p : fxPlayer_.particles) {
        if (!p.streak || p.streakPts.empty() || length(p.pos - eye) > 250.0f) continue;
        // Points oldest first, then the emitter itself while it flies.
        const size_t n = p.streakPts.size();
        auto pt = [&](size_t i) { return i < n ? p.streakPts[i].pos : p.pos; };
        auto ageAt = [&](size_t i) { return i < n ? p.streakPts[i].age : 0.0f; };
        const size_t m = n + (p.attached ? 1 : 0);
        for (size_t i = 0; i + 1 < m && !streakBatch_.full(); ++i) {
            const Vec3 a = pt(i), b = pt(i + 1);
            const Vec3 along = b - a;
            if (length(along) < 1e-3f) continue;
            const Vec3 side = normalize(cross(along, eye - a));
            Vec3 c[4];
            uint8_t col[4];
            const float ta = std::min(1.0f, ageAt(i) / p.slife), tb = std::min(1.0f, ageAt(i + 1) / p.slife);
            const float wa = 0.5f * (p.sw0 + (p.sw1 - p.sw0) * ta), wb = 0.5f * (p.sw0 + (p.sw1 - p.sw0) * tb);
            c[0] = a - side * wa;
            c[1] = b - side * wb;
            c[2] = b + side * wb;
            c[3] = a + side * wa;
            // Additive: colour is brightness; fades over its life (one colour per quad, its newer end).
            const float k = (1.0f - tb) * p.sopacity;
            for (int j = 0; j < 3; ++j) col[j] = uint8_t(std::min(1.0f, (p.sc0[j] + (p.sc1[j] - p.sc0[j]) * tb) * k) * 255.0f);
            col[3] = 255;
            const float uv[4][2] = {{0.5f, 0.0f}, {0.5f, 0.0f}, {0.5f, 1.0f}, {0.5f, 1.0f}};
            streakBatch_.add(c, uv, col);
        }
    }
    streakBatch_.upload(renderer_);
}

bool GameApp::playDataFx(const std::string& name, const Vec3& at, const Quat& rot, float scale) {
    if (!dataFxOn()) return false;
    const bool ok = fxPlayer_.ready() && fxPlayer_.play(name, at, rot, scale);
    static int logged = 0;
    if (ok && logged < 6) { ++logged; LOGI("Effect %s (fx.bin)", name.c_str()); }
    return ok;
}

void GameApp::updateDataFx(float dt) {
    if (!fxPlayer_.ready()) return;
    // Test hook: GVR_FX_TEST=<effect> plays it 8 m ahead every 3 s (GVR_FX_TEST_FRAME: once, at that second).
    if (const char* test = std::getenv("GVR_FX_TEST")) {
        static float t = 2.0f;
        t += dt;
        if (playMode_ && t > 3.0f) {
            t = 0;
            const Vec3 f = Quat::fromAxisAngle({0, 1, 0}, player_.yaw).rotate(Vec3{0, 0, -1});
            Vec3 at = player_.position + f * 8.0f;
            float gy;
            if (assetWorld_ && assetWorld_->terrain().heightAt(at.x, at.z, gy)) at.y = gy + 0.5f;
            LOGI("FX test %s: %s", test, fxPlayer_.play(test, at) ? "playing" : "unknown");
        }
    }
    fxPlayer_.update(dt);
    for (const auto& s : fxPlayer_.sounds) sfx(s.name.c_str(), s.pos, 1.0f, 0.95f + 0.1f * frand());
    fxPlayer_.sounds.clear();
}

void GameApp::renderDataFx() {
    if (!fxPlayer_.ready() || fxPlayer_.particles.empty()) return;
    const Vec3 eye = renderer_.currentView().eyePosition;
    for (const auto& p : fxPlayer_.particles) {
        if (p.ref || !p.model || length(p.pos - eye) > 300.0f) continue;
        const float a = p.alpha();
        if (a <= 0.01f) continue;
        // Quads are 1 x 1 game unit (0.2 m): sized to the particle's metres; meshes scale as they are.
        const float k = p.billboard ? p.size() / gGameUnitsToMeters : p.size();
        const Quat q = p.billboard ? headWorldRot_ : p.rot;
        const Mat4 xf = Mat4::translation(p.pos) * Mat4::rotation(q) * Mat4::scale({k, k, k});
        for (const auto& part : p.model->parts) {
            DrawParams dp;
            dp.texture = part.texture;
            dp.unlit = true;
            dp.doubleSided = true;
            dp.depthWrite = false;
            if (p.additive) {
                dp.blend = BlendMode::Additive;
                dp.tint = {a, a, a, 1.0f};
            } else {
                dp.blend = BlendMode::AlphaBlend;
                dp.tint = {1.0f, 1.0f, 1.0f, a};
            }
            renderer_.draw(part.mesh, xf, dp);
        }
    }
}

}  // namespace gvr
