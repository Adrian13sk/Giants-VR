// The POP-UP BOMB (Meccaryn): from the game's text and data. Reg in story3_popup_bomb: "I've created
// a new bomb, which will destroy the doors to the prison"; "extra light so you'll be able to move
// around easily with it attached to your back"; "if you set off the Pop-up Bomb right by those
// doors you'll be able to get inside". story3_prison_7's briefing: "Only the POP-UP BOMB can
// destroy the GATES" and "Check the Shop for an extra POP-UP BOMB"; 5c's: "Use the POP-UP BOMB
// that your Smarties create for you to destroy the enemy ARMY BARRACKS ... available once your
// base is complete". Model and clips: MC_PopUpBomb (xx_mecc_flick.gzp) with closed / tick / fire.
//
// Ours: how it's used in VR - choose it on the weapon strip (right stick forward), pull the
// trigger to set it down in front of you; it ticks for 5 s, pops up and blows (radius 14 m). The
// timings, radius and damage are ours.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr float kTickSeconds = 5.0f;
constexpr float kRadius = 14.0f;
constexpr float kDamage = 2500.0f;
}  // namespace

bool GameApp::popupOnlyTarget(int item) const {
    // The objects only the bomb can break, in the missions whose briefings say so.
    if (hero_ != Hero::Mecc || !assetWorld_ || item < 0 || size_t(item) >= destruct_.items.size()) return false;
    const int o = destruct_.items[size_t(item)].object;
    if (o < 0 || size_t(o) >= assetWorld_->worldObjects().size()) return false;
    const std::string& name = assetWorld_->worldObjects()[size_t(o)].name;
    const std::string m = toLower(assetWorld_->missionName());
    if (m.rfind("story3_prison_7", 0) == 0) return name == "bw_prison";
    if (m.rfind("5c", 0) == 0) return name == "guard_army_barracks";
    return false;
}

void GameApp::initPopup() {
    popups_.clear();
    popupBombs_ = 0;
    if (!assetWorld_ || hero_ != Hero::Mecc) return;
    const std::string m = toLower(assetWorld_->missionName());
    if (m.rfind("story3_prison_7", 0) == 0) popupBombs_ = 1;  // Reg hands it over in the opening cutscene
    if (const char* f = std::getenv("GVR_POPUP")) popupBombs_ = std::atoi(f);
    if (!popupModel_) {
        popupModel_ = std::make_unique<SkinnedModel>();
        if (popupModel_->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, "MC_PopUpBomb", "MC_Popupbomb_closed.anm")) {
            popupClip_[0] = 0;
            popupClip_[1] = popupModel_->addAnimation(assetWorld_->fs(), "MC_Popupbomb_tick.anm");
            popupClip_[2] = popupModel_->addAnimation(assetWorld_->fs(), "MC_Popupbomb_fire.anm");
        } else {
            popupModel_.reset();
            LOGW("Pop-up bomb model unavailable");
        }
    }
    if (popupBombs_ > 0) LOGI("Pop-up bombs: %d", popupBombs_);
}

void GameApp::givePopupBomb(int n, const char* why) {
    popupBombs_ += n;
    timmyLine_ = std::string(why) + " PICK IT ON THE WEAPON STRIP (RIGHT STICK FORWARD), TRIGGER TO SET IT DOWN.";
    timmyLineT_ = 6.0f;
    weaponHudT_ = 0;
    LOGI("Pop-up bomb given (%d)", popupBombs_);
}

void GameApp::updatePopup(float dt, const InputFrame& in) {
    if (!assetWorld_) return;
    if (weaponSel_ == 3 && popupBombs_ <= 0) weaponSel_ = 0;
    // Test hook: GVR_POPUP_SEL=1 picks the bomb on the first frame of play.
    static bool selDone = false;
    if (!selDone && playMode_ && popupBombs_ > 0 && std::getenv("GVR_POPUP_SEL")) { selDone = true; weaponSel_ = 3; }
    const int gh = rig_.turnHand();
    const bool pull = popupEdge_.pressed(in.hand[gh].trigger > 0.6f);
    if (playMode_ && !dying_ && weaponSel_ == 3 && popupBombs_ > 0 && pull) {
        // Set down at arm's length ahead, on the ground.
        const float yaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : player_.yaw;
        Vec3 at = player_.position + Vec3{-std::sin(yaw) * 1.2f, 0, -std::cos(yaw) * 1.2f};
        float gy;
        if (assetWorld_->terrain().heightAt(at.x, at.z, gy)) at.y = std::max(gy, player_.position.y - 0.5f);
        else at.y = player_.position.y;
        popups_.push_back({at, yaw, 0.0f, false});
        --popupBombs_;
        sfx("ph_selct", at, 1.0f, 0.6f);
        haptics_.push_back({gh, 0.6f, 0.1f});
        timmyLine_ = "POP-UP BOMB SET - GET CLEAR!";
        timmyLineT_ = 3.0f;
        LOGI("Pop-up bomb set at (%.1f %.1f %.1f)", at.x, at.y, at.z);
        if (popupBombs_ <= 0) weaponSel_ = 0;
    }
    for (auto& p : popups_) {
        const float before = p.t;
        p.t += dt;
        // A beep a second, faster at the end.
        const float period = p.t > kTickSeconds - 1.5f ? 0.25f : 1.0f;
        if (p.t < kTickSeconds && std::floor(before / period) != std::floor(p.t / period)) sfx("ph_selct", p.pos, 0.8f, 1.6f);
        if (!p.fired && p.t >= kTickSeconds) {
            sfx("me_mslex", p.pos, 1.0f, 0.7f);
            dustBurst(p.pos, 3.0f, 12);
        }
        if (!p.fired && p.t >= kTickSeconds + 0.6f) {
            p.fired = true;
            popupBlast_ = true;
            explode(p.pos + Vec3{0, 1.5f, 0}, kRadius, kDamage, true);
            popupBlast_ = false;
            dustBurst(p.pos, kRadius * 0.6f, 24);
            for (int h = 0; h < 2; ++h) haptics_.push_back({h, 1.0f, 0.3f});
            LOGI("Pop-up bomb went off at (%.1f %.1f %.1f)", p.pos.x, p.pos.y, p.pos.z);
        }
    }
    popups_.erase(std::remove_if(popups_.begin(), popups_.end(), [](const Popup& p) { return p.fired; }), popups_.end());
}

void GameApp::renderPopup() {
    if (!popupModel_) return;
    for (const auto& p : popups_) {
        const bool firing = p.t >= kTickSeconds;
        const int clip = firing ? popupClip_[2] : popupClip_[1];
        const float d = popupModel_->animDuration(std::max(0, clip));
        const float t = firing ? std::min(d, p.t - kTickSeconds) : std::fmod(p.t, std::max(0.01f, d));
        popupModel_->poseBlend(renderer_, std::max(0, clip), t, 0, 0.0f, 0.0f);
        popupModel_->render(renderer_, Mat4::translation(p.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, p.yaw)));
    }
    // Held in the hand while chosen on the strip.
    if (playMode_ && !dying_ && weaponSel_ == 3 && popupBombs_ > 0 && lastInput_.hand[rig_.turnHand()].gripPoseValid) {
        const Pose hand = rig_.toWorld(lastInput_.hand[rig_.turnHand()].gripPose);
        popupModel_->poseBlend(renderer_, popupClip_[0], 0.0f, 0, 0.0f, 0.0f);
        popupModel_->render(renderer_, Mat4::translation(hand.position + hand.orientation.rotate(Vec3{0, -0.05f, -0.1f})) *
                                           Mat4::rotation(hand.orientation) * Mat4::scale(Vec3{0.5f, 0.5f, 0.5f}));
    }
}

}  // namespace gvr
