// (2026-10-28) Baz's shield: weapondefs item 17 (IconShield, object 902 mc_shield: a pack with an antenna worn on
// the back, Ref_back / Ref_shield), its HUD icon st_mec_shield and the EFX_shield1 / 2 textures. No mission hands it out
// and nothing says how it works; ours: from Story 4 on and in practice, chosen on the weapon wheel it raises a bubble
// round him (the EFX_prx_sphere drawn with EFX_shield1) for 12 s that takes up to 150 of the shots, bites and blasts
// meant for him (not falls or the sea); 40 s before it can be raised again. The gun in hand stays.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
constexpr float kShieldTime = 12.0f;
constexpr float kShieldHp = 150.0f;
constexpr float kShieldCool = 40.0f;
constexpr float kShieldRadius = 1.25f;  // m
}  // namespace

void GameApp::initShield(ModelCache* mc) {
    shieldSphere_ = nullptr;
    shieldTex_ = 0;
    if (!mc) return;
    shieldSphere_ = mc->get("EFX_prx_sphere.gbs", "", false);
    // EFX_shield1 (a blue-violet web of energy). Not EFX_shield itself: that one is an artist's leftover placeholder
    // (plain red with a scrawled note), never meant to be seen.
    shieldTex_ = mc->texture("EFX_shield1");
    if (shieldSphere_) {
        const Vec3 e = shieldSphere_->boundsMax - shieldSphere_->boundsMin;
        shieldScale_ = 2.0f * kShieldRadius / std::max(0.1f, std::max(e.x, std::max(e.y, e.z)));
        LOGI("Shield: sphere %.1f m across (drawn x%.2f), texture %s", double(e.x), double(shieldScale_), shieldTex_ ? "EFX_shield1" : "its own");
    }
}

void GameApp::raiseShield() {
    if (!hasShield_ || shieldT_ > 0 || shieldCool_ > 0) return;
    shieldT_ = kShieldTime;
    shieldHp_ = kShieldHp;
    shieldHitT_ = 0.5f;
    sfx2D("me_sld1", 0.9f);  // (the data's shield sounds: me_sld1 up, me_slden down)
    for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.5f, 0.2f});
    timmyLine_ = "SHIELD UP";
    timmyLineT_ = 2.0f;
    LOGI("Shield up (%.0f s, takes %.0f)", double(kShieldTime), double(kShieldHp));
}

// What the shield takes of a hit (the rest gets through); called from damagePlayer.
float GameApp::shieldAbsorb(float amount, DeathCause cause) {
    if (shieldT_ <= 0 || amount <= 0 || cause == DeathCause::Drowned || cause == DeathCause::Fall) return amount;
    const float took = std::min(amount, shieldHp_);
    shieldHp_ -= took;
    shieldTaken_ += took;
    shieldHitT_ = 0.25f;
    if (frand() < 0.5f) sfx2D("me_minlk", 0.5f, 1.6f);
    if (shieldHp_ <= 0.5f) {
        shieldT_ = 0;
        shieldCool_ = kShieldCool;
        sfx2D("me_slden", 0.9f, 0.8f);
        timmyLine_ = "SHIELD DOWN";
        timmyLineT_ = 2.0f;
        LOGI("Shield broken (it took %.0f)", double(shieldTaken_));
        shieldTaken_ = 0;
    }
    return amount - took;
}

void GameApp::updateShield(float dt) {
    shieldHitT_ = std::max(0.0f, shieldHitT_ - dt);
    static const char* autoAt = std::getenv("GVR_RAISE_SHIELD");  // test hook: raised this many seconds into the mission (as the wheel would)
    shieldClock_ += dt;
    if (autoAt && hasShield_ && shieldClock_ >= float(std::atof(autoAt)) && shieldClock_ - dt < float(std::atof(autoAt))) raiseShield();
    if (shieldT_ > 0) {
        shieldT_ -= dt;
        if (shieldT_ <= 0) {
            shieldT_ = 0;
            shieldCool_ = kShieldCool;
            sfx2D("me_slden", 0.7f);
            LOGI("Shield down (time; it took %.0f)", double(shieldTaken_));
            shieldTaken_ = 0;
        }
    } else if (shieldCool_ > 0) {
        shieldCool_ -= dt;
        if (shieldCool_ <= 0) {
            shieldCool_ = 0;
            if (hasShield_ && hero_ == Hero::Mecc) {
                timmyLine_ = "SHIELD READY";
                timmyLineT_ = 1.5f;
                LOGI("Shield ready");
            }
        }
    }
}

void GameApp::renderShield() {
    if (!shieldSphere_ || shieldT_ <= 0 || hero_ != Hero::Mecc) return;
    const float t = float(time_);
    const float fade = std::min(1.0f, std::min((kShieldTime - shieldT_) / 0.3f, shieldT_ / 0.6f));
    // Faint, flaring where a hit lands; flickering in its last seconds.
    float a = 0.10f + 0.45f * (shieldHitT_ / 0.5f);
    if (shieldT_ < 2.5f) a *= 0.7f + 0.3f * std::sin(t * 9.4f);  // (a slow 1.5 Hz pulse as it runs out - no fast flicker filling the view)
    const Mat4 xf = Mat4::translation(player_.position + Vec3{0, 0.5f * player_.height, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, t * 0.05f)) *
                    Mat4::scale(Vec3{shieldScale_, shieldScale_ * 1.1f, shieldScale_});
    for (const auto& part : shieldSphere_->parts) {
        DrawParams dp;
        dp.texture = shieldTex_ ? shieldTex_ : part.texture;
        dp.blend = BlendMode::Additive;
        dp.unlit = true;
        dp.depthWrite = false;
        dp.doubleSided = true;
        const float k = std::max(0.0f, a * fade);  // (additive: the colour carries the strength)
        dp.tint = Vec4{0.8f * k, 1.0f * k, 1.3f * k, 1.0f};
        renderer_.draw(part.mesh, xf, dp);
    }
}

}  // namespace gvr
