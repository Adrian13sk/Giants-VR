// Mines (weapondefs item 9 IconMine: object 805 mc_mine, 5 carried, 5 at a time; weapon 13: a blast of
// 55 over (.., 30, 50) - near the grenade's). No mission's list hands them out; the Shops sell them
// (ours, after the Giftshop's "full of weapons"). From fx.bin: FXB_Flasher - the set mine's red light
// every 0.5 s with me_minlk (it locks) - and FX_EXPLOSION_MINE (me_minex).
// Ours: pick MINES on the strip, trigger sets one down at arm's length; it arms after 2 s and blows
// when a Reaper, a Ripper or a Vimp comes within reach. Your own don't go off under you.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
constexpr float kArmSeconds = 2.0f, kReach = 2.0f, kBlastRadius = 3.6f, kBlastDamage = 160.0f;  // ours: a foe that sets it off dies
}

void GameApp::initMines() {
    placedMines_.clear();
    if (!assetWorld_) return;
    mineModel_ = assetWorld_->models()->get("mc_mine.gbs", "mc_mine_set.anm", /*dynamicLit=*/true);
    if (const ItemDef* it = weaponDefs_.item(9)) {
        if (it->maxAmmo > 0) minesMax_ = it->maxAmmo;
        if (it->ammoPerPickup > 0) minesGive_ = it->ammoPerPickup;
    }
    if (std::getenv("GVR_ARSENAL") && mineModel_) mines_ = minesMax_;
}

void GameApp::updateMines(float dt, const InputFrame& in) {
    const int gh = rig_.turnHand();
    const bool trig = in.hand[gh].trigger > 0.6f;
    const bool pull = trig && !mineTrigWas_;
    mineTrigWas_ = trig;
    if (weaponSel_ == 9 && mines_ <= 0) weaponSel_ = 0;
    if (playMode_ && !dying_ && hero_ == Hero::Mecc && weaponSel_ == 9 && mines_ > 0 && pull && mineModel_ && assetWorld_) {
        const float yaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : player_.yaw;
        Vec3 at = player_.position + Vec3{-std::sin(yaw) * 1.3f, 0, -std::cos(yaw) * 1.3f};
        float gy;
        if (assetWorld_->terrain().heightAt(at.x, at.z, gy)) at.y = std::max(gy, player_.position.y - 0.6f);
        else at.y = player_.position.y;
        placedMines_.push_back({at, yaw, 0.0f, false});
        --mines_;
        sfx("me_minth", at, 0.9f);
        haptics_.push_back({gh, 0.5f, 0.08f});
        LOGI("Mine set at (%.1f %.1f %.1f), %d left", at.x, at.y, at.z, mines_);
        if (mines_ <= 0) weaponSel_ = 0;
    }
    for (auto& m : placedMines_) {
        const float before = m.t;
        m.t += dt;
        if (!m.armed && m.t >= kArmSeconds) {
            m.armed = true;
            sfx("me_minlk", m.pos, 0.7f);
        }
        if (!m.armed) continue;
        bool go = false;
        for (const auto& e : enemies_.enemies)
            go = go || (e.state != EnemyState::Dead && length(e.pos - m.pos) < kReach);
        for (const auto& c : creatures_.creatures)
            go = go || (c.state != CreatureState::Dead && length(c.pos - m.pos) < kReach + 0.5f * c.scale);
        for (const auto& v : vimps_.vimps)
            go = go || (v.state != VimpState::Dead && length(Vec3{v.pos.x - m.pos.x, 0, v.pos.z - m.pos.z}) < kReach + 1.0f);
        if (go) {
            m.t = -1e9f;
            blastFx_ = "FX_EXPLOSION_MINE";
            explode(m.pos + Vec3{0, 0.3f, 0}, kBlastRadius, kBlastDamage, true);
            sfx("me_minex", m.pos, 1.0f, 0.95f + 0.1f * frand());
            LOGI("Mine went off at (%.1f %.1f %.1f)", m.pos.x, m.pos.y, m.pos.z);
        } else if (std::floor(before * 2.0f) != std::floor(m.t * 2.0f) && length(m.pos - player_.position) < 25.0f) {
            sfx("me_minml", m.pos, 0.25f, 1.0f);  // a soft tick with the light (FXB_Flasher's sound)
        }
    }
    placedMines_.erase(std::remove_if(placedMines_.begin(), placedMines_.end(), [](const PlacedMine& m) { return m.t < -1e8f; }),
                       placedMines_.end());
}

void GameApp::renderMines() {
    if (!mineModel_ || !playMode_) return;
    const float k = 0.55f;
    for (const auto& m : placedMines_) {
        drawModel(mineModel_, Mat4::translation(m.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, m.yaw)) * Mat4::scale({k, k, k}), false);
        // The red light (FXB_redlight every 0.5 s once armed).
        if (m.armed && std::fmod(m.t, 0.5f) < 0.18f && boltModel_) {
            for (const auto& part : boltModel_->parts) {
                DrawParams dp;
                dp.texture = part.texture;
                dp.blend = BlendMode::Additive;
                dp.unlit = true;
                dp.depthWrite = false;
                dp.doubleSided = true;
                dp.tint = {2.0f, 0.2f, 0.1f, 1.0f};
                renderer_.draw(part.mesh, Mat4::translation(m.pos + Vec3{0, 0.45f, 0}) * Mat4::rotation(headWorldRot_) * Mat4::scale({0.8f, 0.8f, 0.8f}), dp);
            }
        }
    }
    // One in the hand while chosen (first person).
    if (weaponSel_ == 9 && mines_ > 0 && firstPerson_ && !dying_ && lastInput_.hand[rig_.turnHand()].gripPoseValid) {
        const Pose hand = rig_.toWorld(lastInput_.hand[rig_.turnHand()].gripPose);
        const float h = 0.3f;
        drawModel(mineModel_, Mat4::translation(hand.position + hand.orientation.rotate(Vec3{0, -0.04f, -0.1f})) * Mat4::rotation(hand.orientation) *
                                  Mat4::scale({h, h, h}) * Mat4::translation((mineModel_->boundsMin + mineModel_->boundsMax) * -0.5f),
                  false);
    }
}

}  // namespace gvr
