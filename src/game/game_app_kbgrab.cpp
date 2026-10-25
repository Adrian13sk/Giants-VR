// GameApp, rework: Kabuto's hands in first person. Reach down and squeeze a grip on something at
// your feet: a Reaper or a boulder stays in that hand (swing and open the hand to throw it, or bring
// it up to your mouth to eat it); a Smartie, a Vimp or a small creature is scooped up and eaten.
// (X and the old grab still work; game_app_kbthrow.cpp carries and throws.)
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"

namespace gvr {

namespace {
constexpr int kCreatureTargetG = 1000;  // as game_app_kabuto.cpp's target ranges
constexpr int kVillagerTargetG = 5000;
constexpr int kVimpTargetG = 6000;
}  // namespace

void GameApp::updateKabutoHandGrab(const InputFrame& in) {
    const float H = player_.height;
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        const bool g = c.squeeze > 0.7f;
        const bool edge = g && !kbGripWas_[h];
        kbGripWas_[h] = g;
        static const bool trace = std::getenv("GVR_GRAB_TRACE") != nullptr;
        if (trace && edge) LOGI("grab trace: hand %d fp %d play %d valid %d hold %d eatT %.2f", h, firstPerson_, playMode_, c.gripPoseValid, kbHold_, kb_.eatT);
        if (!edge || !firstPerson_ || !playMode_ || dying_ || !c.gripPoseValid || kbHold_ != HoldNone || kb_.eatT >= 0) continue;
        const Pose gp = rig_.toWorld(c.gripPose);
        const Vec3 hand = gp.position + gp.orientation.rotate(Vec3{0, 0.02f * H, -0.09f * H});
        const float gy = player_.groundHeight ? player_.groundHeight(hand.x, hand.z) : player_.position.y;
        if (trace) {
            LOGI("grab trace: hand at (%.1f %.1f %.1f), %.1f above ground (limit %.1f); feet (%.1f %.1f %.1f)", hand.x, hand.y, hand.z, hand.y - gy, 0.32f * H,
                 player_.position.x, player_.position.y, player_.position.z);
            for (const auto& e : enemies_.enemies)
                if (e.state != EnemyState::Dead) LOGI("grab trace:   Reaper at (%.1f %.1f %.1f) %.1f m flat", e.pos.x, e.pos.y, e.pos.z, std::sqrt((e.pos.x - hand.x) * (e.pos.x - hand.x) + (e.pos.z - hand.z) * (e.pos.z - hand.z)));
        }
        if (hand.y - gy > 0.32f * H) continue;  // reach down for it (his hands hang at about half his height)
        const float reach = 0.24f * H;  // (his hand is about a tenth of his height across; a Reaper a doll)
        auto flat = [&](const Vec3& p) { return std::sqrt((p.x - hand.x) * (p.x - hand.x) + (p.z - hand.z) * (p.z - hand.z)); };
        // The nearest of everything in reach of this hand.
        float best = reach;
        int kind = 0, idx = -1;  // kind 1 boulder, 2 Reaper, 3 something to eat (idx: its eat target)
        for (size_t i = 0; i < kbBoulders_.size(); ++i) {
            const Boulder& b = kbBoulders_[i];
            if (b.state != Boulder::Resting && b.state != Boulder::Loose) continue;
            const float d = flat(b.pos) - boulderRadius();
            if (d < best) { best = d; kind = 1; idx = int(i); }
        }
        for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
            const Enemy& e = enemies_.enemies[i];
            if (e.state == EnemyState::Dead || e.state == EnemyState::Falling) continue;
            const float d = flat(e.pos);
            if (d < best) { best = d; kind = 2; idx = int(i); }
        }
        for (size_t i = 0; i < villagers_.villagers.size(); ++i) {
            const float d = flat(villagers_.villagers[i].pos);
            if (d < best) { best = d; kind = 3; idx = kVillagerTargetG + int(i); }
        }
        for (size_t i = 0; i < vimps_.vimps.size(); ++i) {
            if (vimps_.vimps[i].state == VimpState::Dead) continue;
            const float d = flat(vimps_.vimps[i].pos);
            if (d < best) { best = d; kind = 3; idx = kVimpTargetG + int(i); }
        }
        for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
            const Creature& cr = creatures_.creatures[i];
            if (cr.state == CreatureState::Dead || cr.scale >= 2.5f || CreatureDirector::underground(cr)) continue;
            const float d = flat(cr.pos);
            if (d < best) { best = d; kind = 3; idx = kCreatureTargetG + int(i); }
        }
        if (kind == 0) {
            haptics_.push_back({h, 0.15f, 0.04f});  // a brush of the ground: nothing there
            continue;
        }
        haptics_.push_back({h, 0.7f, 0.1f});
        sfx2D("ka_grab", 0.9f);
        kbHoldT_ = 0;
        kbHandValid_ = false;  // (the throw speed starts from this hand)
        if (kind == 1) {
            kbHold_ = HoldBoulder;
            kbHeld_ = idx;
            kbGrabHand_ = h;
            kbBoulders_[size_t(idx)].state = Boulder::Held;
            LOGI("Kabuto: hand %d picks up a boulder", h);
        } else if (kind == 2) {
            kbHold_ = HoldReaper;
            kbHeld_ = idx;
            kbHeldUid_ = enemies_.enemies[size_t(idx)].uid;
            kbGrabHand_ = h;
            enemies_.knockOff(idx, Vec3{0, 0, 0});
            sfx("rpman_hit1", enemies_.enemies[size_t(idx)].pos, 0.8f, 1.1f);
            LOGI("Kabuto: hand %d grabs Reaper %d", h, idx);
        } else {
            kabutoEat(idx);
            LOGI("Kabuto: hand %d scoops up target %d to eat", h, idx);
        }
        break;
    }
}

}  // namespace gvr
