// The Meccaryn's jetpack, seen and heard: the original thrust flame (mcthrust2, or MCthrust_nitro on
// turbo) and blue glow star (mcthrust) at the pack's Ref_Thrust node, blue plasma streaming down
// from it, its light on the ground below (b_thrust) and dust blown up near the ground. In first
// person the body isn't drawn, so the plasma stream and the ground light are what shows it.
// Burnt dry and held in the air it sputters (PlayerController::sputtering): the flame flickers on and
// off, grey smoke coughs out and it makes a struggling noise.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/cutscene.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

void GameApp::initJetFx() {
    if (!assetWorld_ || thrustFlame_) return;
    ModelCache* mc = assetWorld_->models();
    auto load = [&](const char* base, const char* anim) -> std::unique_ptr<SkinnedModel> {
        auto m = std::make_unique<SkinnedModel>();
        if (!m->load(assetWorld_->fs(), *mc, renderer_, base, anim)) return nullptr;
        return m;
    };
    thrustFlame_ = load("mcthrust2", "mcthrust2.anm");
    thrustNitro_ = load("MCthrust_nitro", "mcthrust2.anm");
    thrustStar_ = load("mcthrust", "mcthrust.anm");
    jetGlowTex_ = mc->texture("b_thrust");
    jetGlowBatch_.init(renderer_, 2);
    LOGI("Jetpack effects: flame %d, nitro %d, star %d, glow texture %u", thrustFlame_ != nullptr, thrustNitro_ != nullptr,
         thrustStar_ != nullptr, jetGlowTex_);
}

void GameApp::updateJetFx(float dt) {
    jetFxOn_ = false;
    jetGlow_ = 0;
    if (hero_ != Hero::Mecc || !playerModel_ || !player_.jetpackAvailable) return;
    const bool thr = player_.thrusting(), sput = player_.sputtering();
    // Sputtering: the flame catches and dies at random (on ~40% of the time).
    if (sput) {
        jetFlick_ -= dt;
        if (jetFlick_ <= 0) {
            jetFlickOn_ = frand() < 0.4f;
            jetFlick_ = jetFlickOn_ ? 0.04f + 0.08f * frand() : 0.06f + 0.18f * frand();
            if (jetFlickOn_) {
                // The struggling noise: a choked start or the dry cough, never the steady loop.
                sfx2D(frand() < 0.55f ? "me_jpkst" : "me_jpkno", 0.3f + 0.25f * frand(), 0.7f + 0.35f * frand());
                haptics_.push_back({rig_.turnHand(), 0.25f + 0.2f * frand(), 0.05f});
            }
        }
    } else {
        jetFlickOn_ = true;
        jetFlick_ = 0;
    }
    const bool lit = thr || (sput && jetFlickOn_);
    if (!thr && !sput) return;
    // The nozzle: Ref_Thrust on the pack (body frame), else behind the shoulders.
    Mat4 ref;
    if (playerModel_->nodeWorld("ref_thrust", ref)) {
        nozzleXf_ = bodyXf_ * gameMatrixToRuntime(ref);
    } else {
        const Quat body = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
        nozzleXf_ = Mat4::translation(player_.position + Vec3{0, 1.3f, 0} + body.rotate(Vec3{0, 0, 0.3f})) *
                    Mat4::rotation(body);
    }
    const Vec3 nozzle{nozzleXf_.m[12], nozzleXf_.m[13], nozzleXf_.m[14]};
    jetFxOn_ = lit;
    if (lit) {
        // Its blue light on the body, the hands and the ground (the renderer's point light):
        // what shows it in first person, where the flame is behind the eyes.
        EnvironmentParams& env = renderer_.environment();
        const float shimmer = 0.85f + 0.3f * frand();
        env.pointLightPos = nozzle + Vec3{0, -0.5f, 0};
        static const float boost = std::getenv("GVR_JETLIGHT") ? float(std::atof(std::getenv("GVR_JETLIGHT"))) : 1.0f;  // test hook
        env.pointLightColor = Vec3{0.45f, 0.75f, 1.7f} * (boost * shimmer * (sput ? 0.8f : 1.0f) * (player_.turbo() ? 1.3f : 1.0f));
        env.pointLightRadius = 7.0f;
    }
    jetNitro_ = player_.turbo();
    // Plasma streaming down from the nozzle (blue, white-hot core); coughs of grey smoke when
    // sputtering.
    jetPuffT_ -= dt;
    if (jetPuffT_ <= 0) {
        jetPuffT_ = 1.0f / 50.0f;
        const Vec3 carry = player_.velocity * 0.8f;
        if (lit) {
            const Vec3 down{(frand() - 0.5f) * 1.2f, -7.0f - 3.0f * frand(), (frand() - 0.5f) * 1.2f};
            smokePuff(FxTex::FireBlue, nozzle + Vec3{0, -0.15f, 0}, carry + down, jetNitro_ ? 0.95f : 0.75f, 0.25f, 0.45f, 1.0f, 0.0f);
            smokePuff(FxTex::FireBlue, nozzle, carry + down * 0.35f, 0.55f, 0.2f, 0.12f, 1.0f, 0.0f);  // the hot core
            // First person: the stream is behind and below the eyes; some of it fans out round
            // the legs so looking down shows it.
            if (firstPerson_ && frand() < 0.6f) {
                const Quat body = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
                const float side = frand() < 0.5f ? -1.0f : 1.0f;
                const Vec3 at = player_.position + Vec3{0, 0.9f, 0} + body.rotate(Vec3{side * 0.35f, 0, 0.15f});
                smokePuff(FxTex::FireBlue, at, carry + body.rotate(Vec3{side * 0.8f, -6.0f - 2.0f * frand(), 0.4f}), 0.45f, 0.15f, 0.4f, 0.9f, 0.0f);
            }
        }
        if (sput && frand() < 0.25f)
            smokePuff(FxTex::SmokeGrey, nozzle + Vec3{0, -0.2f, 0}, carry + Vec3{0, -1.5f, 0}, 0.25f, 1.1f, 1.2f, 0.45f, 0.3f);
    }
    // Its light on the ground below, and dust blown out near the ground.
    const float g = enemies_.groundHeight ? std::max(enemies_.groundHeight(player_.position.x, player_.position.z), -0.05f) : 0.0f;
    const float h = player_.position.y - g;
    if (lit && h < 9.0f) {
        jetGlow_ = (1.0f - h / 9.0f) * (sput ? 0.6f : 1.0f);
        jetGlowAt_ = Vec3{player_.position.x, g, player_.position.z};
        jetDustT_ -= dt;
        if (h < 4.0f && jetDustT_ <= 0 && g > -0.04f) {
            jetDustT_ = 0.09f;
            const float a = frand() * 6.28f;
            const Vec3 out{std::cos(a), 0.15f, std::sin(a)};
            smokePuff(FxTex::Dust, jetGlowAt_ + out * (0.4f + 0.4f * frand()) + Vec3{0, 0.15f, 0}, out * (3.0f + 2.0f * frand()), 0.4f, 1.8f, 1.0f,
                      0.45f * (1.0f - h / 4.0f), 0.05f);
        }
    }
}

void GameApp::renderJetFx(bool firstPersonView) {
    // The flame and star at the nozzle (third person; in first person they'd sit behind the eyes).
    if (!jetFxOn_ || firstPersonView) return;
    SkinnedModel* flame = jetNitro_ && thrustNitro_ ? thrustNitro_.get() : thrustFlame_.get();
    // The flame models are tiny in the original's units (0.24 m); drawn larger so they read in VR.
    const Mat4 xf = nozzleXf_ * Mat4::scale(Vec3{1, 1, 1} * 2.2f);
    if (flame) {
        flame->pose(renderer_, float(time_));
        flame->renderGlow(renderer_, xf, 1.4f);
    }
    if (thrustStar_) {
        thrustStar_->pose(renderer_, float(time_));
        thrustStar_->renderGlow(renderer_, xf * Mat4::scale(Vec3{1, 1, 1} * 1.6f), 1.2f);
    }
}

}  // namespace gvr
