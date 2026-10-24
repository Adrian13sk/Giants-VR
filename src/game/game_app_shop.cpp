// The Shop (guard_shop, object class 102) in Story 1's last level (1d_1_sneakpast). In the original
// you walk in and buy weapons and supplies; here (ours) walking into it opens a holographic counter:
// the goods float in a ring round you - the rocket launcher, health injections - and touching one
// (walking into it, or reaching for it in first person) takes it, each once per level. On the floor
// in the middle, a health pool heals while you stand in it, its store draining as it does until
// it is spent (it does not refill).
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
constexpr float kShopRadius = 4.5f;     // m round the shop's centre: "inside"
constexpr float kPoolRadius = 1.4f;     // m
constexpr float kPoolStore = 150.0f;    // health points in the pool
constexpr float kPoolRate = 25.0f;      // per second while standing in it
}  // namespace

void GameApp::initShop() {
    shopOn_ = false;
    shopItems_.clear();
    shopInside_ = false;
    shopPool_ = 0;
    shopCentres_.clear();
    if (!assetWorld_) return;
    // Every guard_shop of the level (4a has two); one counter per level, at whichever is entered first.
    for (const auto& o : assetWorld_->worldObjects()) {
        if (o.name != "guard_shop") continue;
        Vec3 c = o.centre;
        float gy;
        if (assetWorld_->terrain().heightAt(c.x, c.z, gy)) c.y = gy;
        shopCentres_.push_back(c);
    }
    shopOn_ = !shopCentres_.empty();
    if (shopOn_) shopAt_ = shopCentre_ = shopCentres_.front();
    if (!shopOn_) return;
    shopPool_ = kPoolStore;
    shopAnchored_ = false;
    // The goods (item 0 the launcher, if not owned yet); placed when the player first walks in.
    const int kinds[3] = {hasLauncher_ ? 1 : 0, 1, 2};  // launcher (or an injection), an injection, flares
    for (int i = 0; i < 3; ++i) shopItems_.push_back({kinds[i], shopAt_, false});
    LOGI("Shop at (%.1f %.1f %.1f): %zu items, health pool %.0f", shopAt_.x, shopAt_.y, shopAt_.z, shopItems_.size(), shopPool_);
}

void GameApp::updateShop(float dt, const InputFrame& in) {
    if (!shopOn_ || !playMode_ || dying_) return;
    bool inside = false;
    for (const Vec3& c : shopCentres_) {
        if (shopAnchored_ && length(c - shopCentre_) > 0.1f) continue;  // the counter is open at another one
        const Vec3 d = player_.position - c;
        if (std::sqrt(d.x * d.x + d.z * d.z) < kShopRadius && std::fabs(d.y) < 4.0f) { inside = true; shopCentre_ = c; }
    }
    if (inside && !shopAnchored_) {
        // The interior is cramped: the counter opens where the player stands on first walking
        // in - the goods in an arc at arm's length ahead, the pool under his feet.
        shopAnchored_ = true;
        shopAt_ = Vec3{player_.position.x, player_.position.y, player_.position.z};
        const float yaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : player_.yaw;  // where he looks
        for (size_t i = 0; i < shopItems_.size(); ++i) {
            const float a = yaw + (float(i) - 1.0f) * 0.75f;
            shopItems_[i].pos = shopAt_ + Vec3{-std::sin(a) * 1.5f, 1.1f, -std::cos(a) * 1.5f};
        }
    }
    if (inside && !shopInside_) {
        timmyLine_ = "SHOP: TAKE WHAT YOU NEED - ONCE EACH. THE POOL HEALS.";
        timmyLineT_ = 4.0f;
        sfx2D("ph_selct", 0.8f, 0.8f);
        LOGI("Entered the shop");
    }
    shopInside_ = inside;
    if (!inside) return;
    // Taking an item: walking into it, or a hand reaching it (first person).
    Vec3 hands[2];
    int nh = 0;
    for (int h = 0; h < 2; ++h)
        if (firstPerson_ && in.hand[h].gripPoseValid) hands[nh++] = rig_.toWorld(in.hand[h].gripPose).position;
    for (auto& it : shopItems_) {
        if (it.taken) continue;
        const Vec3 f = it.pos - player_.position;
        bool touch = f.x * f.x + f.z * f.z < 0.8f * 0.8f;
        for (int k = 0; k < nh; ++k) touch = touch || length(hands[k] - it.pos) < 0.45f;
        if (!touch) continue;
        if (it.kind == 0) {
            hasLauncher_ = true;
            weaponHudT_ = 0;
            LOGI("Shop: took the rocket launcher");
        } else if (it.kind == 2) {
            if (flares_ >= kMaxFlares) continue;
            flares_ = std::min(kMaxFlares, flares_ + 3);
            LOGI("Shop: took flares (%d)", flares_);
        } else {
            if (syringes_ >= kMaxSyringes) continue;  // no room: it stays for later
            ++syringes_;
            LOGI("Shop: took a health injection (%d)", syringes_);
        }
        it.taken = true;
        pickupFlash_ = 1.0f;
        sfx2D(it.kind == 0 ? "ph_selct" : "pu_hlth", 1.0f);
        for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.5f, 0.1f});
        for (int k = 0; k < 10; ++k)
            smokePuff(FxTex::FireGreen, it.pos, Vec3{(frand() - 0.5f) * 2, 1.0f + frand(), (frand() - 0.5f) * 2}, 0.4f, 0.1f, 0.6f, 1.0f, 0.0f);
    }
    // The health pool.
    const Vec3 dp = player_.position - shopAt_;
    const bool inPool = std::sqrt(dp.x * dp.x + dp.z * dp.z) < kPoolRadius && shopAnchored_ && shopPool_ > 0 && playerHealth_ < 100.0f && playerHealth_ > 0;
    if (inPool) {
        const float h = std::min({kPoolRate * dt, shopPool_, 100.0f - playerHealth_});
        playerHealth_ += h;
        shopPool_ -= h;
        healFlash_ = std::max(healFlash_, 0.35f);
        shopPoolSnd_ -= dt;
        if (shopPoolSnd_ <= 0) { shopPoolSnd_ = 0.6f; sfx2D("pu_hlth", 0.35f, 1.3f); }
        if (shopPool_ <= 0) LOGI("Shop: the health pool is spent");
    }
    // Sparkles off the pool (fewer as it drains), its surface rippling while it lasts.
    shopFxT_ -= dt;
    if (shopFxT_ <= 0) {
        shopFxT_ = 0.08f;
        const float k = shopAnchored_ ? shopPool_ / kPoolStore : 0.0f;
        shopRingT_ -= 0.08f;
        if (k > 0 && shopRingT_ <= 0 && waterFx_.size() < 56) {
            shopRingT_ = 0.9f;
            waterFx_.push_back({shopAt_ + Vec3{0, 0.05f, 0}, 0.0f, 1, 1.6f * kPoolRadius * (0.5f + 0.5f * k)});
        }
        if (k > 0 && frand() < 0.3f + 0.7f * k) {
            const float a = frand() * 6.2832f, r = kPoolRadius * std::sqrt(frand());
            smokePuff(FxTex::FireGreen, shopAt_ + Vec3{std::cos(a) * r, 0.1f, std::sin(a) * r}, Vec3{0, 0.8f + 0.8f * k, 0}, 0.35f, 0.1f, 0.9f, 0.8f, 0.2f);
        }
    }
}

void GameApp::renderShop() {
    if (!shopOn_ || !playMode_) return;
    const Pose head = rig_.toWorld(lastInput_.head);
    if (length(head.position - shopAt_) > 30.0f) return;
    const float t = float(time_);
    const bool near = shopInside_;
    if (!shopAnchored_) return;
    for (const auto& it : shopItems_) {
        if (it.taken) continue;
        if (it.kind == 2) {
            // Flares: the HUD's flare icon as a glowing card, facing the player.
            const Mat4 xf = Mat4::translation(it.pos + Vec3{0, 0.06f * std::sin(t * 2.0f + it.pos.x), 0}) *
                            Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, yawOf(head.orientation)));
            text_.drawImage(xf, 0.32f, 0.32f, hudTex_[kHudFlare], {1, 1, 1, 1}, true);
        }
        const RenderModel* m = it.kind == 0 ? launcherModel_ : (it.kind == 2 ? nullptr : syringeModel_);
        const float bob = 0.06f * std::sin(t * 2.0f + it.pos.x);
        const float s = it.kind == 0 ? 1.0f : 2.0f;  // the syringe is small
        if (m)
            drawModel(m, Mat4::translation(it.pos + Vec3{0, bob, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, t * 0.8f)) *
                             Mat4::scale(Vec3{s, s, s}),
                      it.kind == 2);  // the flare glows
        if (!near || !text_.ready()) continue;
        const std::string label = it.kind == 0 ? "ROCKET LAUNCHER"
                                  : it.kind == 2 ? (flares_ >= kMaxFlares ? "FLARES (FULL)" : "3 FLARES")
                                                 : (syringes_ >= kMaxSyringes ? "INJECTION (FULL)" : "HEALTH INJECTION");
        float w = 0, h = 0;
        const MeshId tm = text_.mesh(label, 0.1f, 20.0f, &w, &h);
        if (!tm) continue;
        const Vec3 at = it.pos + Vec3{0, 0.55f, 0};
        const Quat face = Quat::fromAxisAngle({0, 1, 0}, yawOf(head.orientation));
        const Mat4 xf = Mat4::translation(at) * Mat4::rotation(face);
        text_.drawPanel(xf, w + 0.06f, h + 0.03f, {0, 0, 0, 0.45f}, true);
        text_.draw(tm, xf * Mat4::translation({0, 0, 0.002f}), {0.6f, 1.0f, 0.6f, 1.0f}, true);
    }
    if (near && text_.ready()) {
        char lab[48];
        if (shopPool_ > 0) std::snprintf(lab, sizeof lab, "HEALTH POOL %d%%", int(std::ceil(100.0f * shopPool_ / kPoolStore)));
        else std::snprintf(lab, sizeof lab, "HEALTH POOL EMPTY");
        float w = 0, h = 0;
        const MeshId tm = text_.mesh(lab, 0.12f, 20.0f, &w, &h);
        if (tm) {
            const Quat face = Quat::fromAxisAngle({0, 1, 0}, yawOf(head.orientation));
            const Mat4 xf = Mat4::translation(shopAt_ + Vec3{0, 2.4f, 0}) * Mat4::rotation(face);
            text_.drawPanel(xf, w + 0.08f, h + 0.04f, {0, 0, 0, 0.45f}, true);
            text_.draw(tm, xf * Mat4::translation({0, 0, 0.002f}), shopPool_ > 0 ? Vec4{0.5f, 1.0f, 0.6f, 1.0f} : Vec4{0.7f, 0.7f, 0.7f, 1.0f}, true);
        }
    }
}

}  // namespace gvr
