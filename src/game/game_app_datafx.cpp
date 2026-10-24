// The game's own effects (fx.bin) played by FxPlayer and drawn here: cutscene effects (flick op 0x27
// names fx.bin entries) first; the stand-in particles remain for what the library doesn't name.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
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

bool GameApp::playDataFx(const std::string& name, const Vec3& at, const Quat& rot, float scale) {
    // Options -> EFFECTS: ORIGINAL (TEST) (or GVR_DATA_FX=1); off by default until seen on the headset.
    static const bool forced = std::getenv("GVR_DATA_FX") != nullptr;
    if (!settings_.originalFx && !forced) return false;
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
