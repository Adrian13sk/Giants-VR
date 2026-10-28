// (2026-10-28) Baz's portable turret: weapondefs item 11 (IconTurret1, object 60 mc_turret: "max 1", a gadget).
// The data has the item, its model (a gun head on a spike: MC_turret.gbs, nodes center / Turret / gun /
// Ref_handle / Ref_nozzle01) and its icons (st_mec_turret1, i_mecc_turret), but no mission hands it out and nothing says how it
// behaves - all of that is ours: carried in the hand by its Ref_handle, planted with the trigger, it turns to the
// nearest Reaper it can see within 40 m and fires (60 rounds, 12 damage, one every 0.45 s). Spent, it folds away and
// is back in the pack 20 s later, its rounds refilled. A squeeze of the grip with a hand on it takes it back.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "assets/gzp.h"
#include "audio/mixer.h"
#include "assets/skin_anim.h"
#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr float kTurretRange = 40.0f;     // m
constexpr float kTurretInterval = 0.45f;  // s between rounds
constexpr float kTurretDamage = 12.0f;    // about seven to drop a Reaper (the base turrets: 18 every 0.7 s)
constexpr int kTurretRounds = 60;
constexpr float kTurretRecharge = 20.0f;  // s from folding away spent to back in the pack
constexpr float kTurretRise = 0.6f;       // s to drive in / pull out
constexpr float kTurretSink = 0.25f;      // m of the spike in the ground
constexpr float kTurretReach = 6.0f;      // m: the farthest it is planted from Baz
// Game-frame (Z-up, game units) points of the model: its head turns about "center".
Vec3 gCentre{0, 0, 3.0f}, gNozzle{0, -3.3f, 4.6f};
float gBarrel = -1.5708f;  // the barrel's rest heading in the game frame (atan2 of the nozzle's offset)
}  // namespace

void GameApp::initPortTurret(ModelCache* mc) {
    turretHeldModel_ = nullptr;
    turretModel_.reset();
    if (!mc || !assetWorld_) return;
    turretHeldModel_ = mc->get("MC_turret.gbs", "", /*dynamicLit=*/true);
    auto sm = std::make_unique<SkinnedModel>();
    if (!turretHeldModel_ || !sm->load(assetWorld_->fs(), *mc, renderer_, "mc_turret", "mc_turret.anm")) {
        turretHeldModel_ = nullptr;
        LOGW("Turret: mc_turret unavailable");
        return;
    }
    {
        std::vector<uint8_t> b;
        AnmAnimation a;
        if (assetWorld_->fs().read("mc_turret.anm", b) && parseAnm(b.data(), b.size(), a)) {
            std::vector<Mat4> wm;
            a.evaluate(0.0f, wm);
            auto at = [&](const char* node, Vec3& out) {
                auto it = a.byName.find(node);
                if (it != a.byName.end() && size_t(it->second) < wm.size())
                    out = {wm[size_t(it->second)].m[12], wm[size_t(it->second)].m[13], wm[size_t(it->second)].m[14]};
            };
            at("center", gCentre);
            at("ref_nozzle01", gNozzle);
            at("ref_handle", turretHandle_);
            turretMuzzle_ = gNozzle;
        }
    }
    gBarrel = std::atan2(gNozzle.y - gCentre.y, gNozzle.x - gCentre.x);
    // The head (node "center" and what hangs from it) turns about its pivot by turret_.aimYaw (game Z).
    sm->setPoseModifier([this](const AnmAnimation& clip, const std::vector<std::vector<int>>& children, std::vector<Mat4>& world) {
        int c = -1;
        for (size_t i = 0; i < clip.nodes.size(); ++i)
            if (toLower(clip.nodes[i].name) == "center") { c = int(i); break; }
        if (c < 0 || size_t(c) >= world.size()) return;
        const Vec3 pv{world[size_t(c)].m[12], world[size_t(c)].m[13], world[size_t(c)].m[14]};
        const Mat4 rot = Mat4::translation(pv) * Mat4::rotation(Quat::fromAxisAngle({0, 0, 1}, turret_.aimYaw)) * Mat4::translation(pv * -1.0f);
        std::vector<int> stack{c};
        while (!stack.empty()) {
            const int n = stack.back();
            stack.pop_back();
            world[size_t(n)] = rot * world[size_t(n)];
            if (size_t(n) < children.size()) for (int ch : children[size_t(n)]) stack.push_back(ch);
        }
    });
    turretModel_ = std::move(sm);
    LOGI("Turret: model ok; handle (%.2f %.2f %.2f), nozzle (%.2f %.2f %.2f), centre (%.2f %.2f %.2f), barrel at %.0f deg",
         turretHandle_.x, turretHandle_.y, turretHandle_.z, gNozzle.x, gNozzle.y, gNozzle.z, gCentre.x, gCentre.y, gCentre.z,
         double(gBarrel * 57.2958f));
}

void GameApp::resetPortTurret() {
    turret_ = PortTurret{};
    turretRounds_ = kTurretRounds;
    turretReadyT_ = 0;
    turretShots_.clear();
    turretTrigHeld_ = turretTrigBlock_ = false;
    turretGripWas_[0] = turretGripWas_[1] = false;
}

// Back into the pack at once, its rounds kept (left far behind, or Baz back at a checkpoint).
void GameApp::recallTurret(const char* why) {
    if (!turret_.up) return;
    turret_.up = false;
    turret_.folding = false;
    LOGI("Turret: %s - back in the pack (%d rounds)", why, turretRounds_);
}

void GameApp::plantTurret(const Vec3& muzzle, const Vec3& fwd) {
    if (!turretModel_ || !assetWorld_ || turret_.up) return;
    // Where the hand points: the ground it meets within 8 m (a wall: just short of it), else 3 m along it.
    if (!player_.onGround) {
        sfx2D("me_outam", 0.8f);
        timmyLine_ = "TURRET: STAND ON THE GROUND TO PLANT IT";
        timmyLineT_ = 2.0f;
        return;
    }
    Vec3 at = muzzle + fwd * 3.0f;
    float t;
    Vec3 n;
    bool onTop = false;  // (the ray met a floor - a roof, a bridge, a walkway: planted on it, not on the terrain under it)
    if (worldRay_ && worldRay_(muzzle, fwd, 8.0f, t, n)) {
        at = muzzle + fwd * (n.y > 0.5f ? t : std::max(0.0f, t - 0.6f));
        onTop = n.y > 0.5f;
    }
    Vec3 flat{at.x - player_.position.x, 0, at.z - player_.position.z};
    const float d = length(flat);
    if (d > kTurretReach) { at = player_.position + flat * (kTurretReach / d); onTop = false; }
    if (d < 0.8f) {  // (not into his own feet: a step ahead, where he stands)
        const Vec3 ahead{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)};
        at = player_.position + ahead * 1.2f;
        at.y = player_.position.y;
        onTop = true;
    }
    float h = at.y;
    if (!onTop && !assetWorld_->terrain().heightAt(at.x, at.z, h)) h = -1e9f;
    if (h < player_.seaLevel - 0.2f || std::fabs(h - player_.position.y) > 3.0f) {
        sfx2D("me_outam", 0.8f);
        timmyLine_ = "TURRET: NO GROUND TO PLANT IT THERE";
        timmyLineT_ = 2.0f;
        LOGI("Turret: no ground to plant it at (%.1f %.1f)", at.x, at.z);
        return;
    }
    turret_ = PortTurret{};
    turret_.up = true;
    turret_.pos = {at.x, h, at.z};
    // Facing the way the hand pointed: the model's -Z (game -Y) along it, the barrel turned to match.
    const float yaw = std::atan2(-fwd.x, -fwd.z);
    turret_.xf = Mat4::translation(turret_.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, yaw));
    turret_.aimYaw = -1.5708f - gBarrel;
    turret_.fireT = kTurretRise + 0.3f;
    sfx("me_minth", turret_.pos, 1.0f, 0.7f);
    sfx("ph_enter", turret_.pos, 0.7f, 1.3f);
    haptics_.push_back({rig_.turnHand(), 0.7f, 0.12f});
    LOGI("Turret planted %.1f m away (%d rounds)", length(Vec3{at.x - player_.position.x, 0, at.z - player_.position.z}), turretRounds_);
    // Back to what was in hand - not into the bush, and not a grenade cocked by the trigger still held.
    weaponSel_ = weaponOwned(lastWeaponSel_) && lastWeaponSel_ != 2 && lastWeaponSel_ != 8 ? lastWeaponSel_ : 0;
    weaponHudT_ = 0;
    turretTrigBlock_ = true;
}

void GameApp::updatePortTurret(float dt, const InputFrame& in) {
    if (turretReadyT_ > 0) {
        turretReadyT_ -= dt;
        if (turretReadyT_ <= 0) {
            turretReadyT_ = 0;
            turretRounds_ = kTurretRounds;
            if (hasTurret_ && hero_ == Hero::Mecc) {
                sfx2D("ph_selct", 0.6f, 1.2f);
                timmyLine_ = "TURRET READY";
                timmyLineT_ = 2.0f;
                LOGI("Turret: back in the pack (%d rounds)", turretRounds_);
            }
        }
    }
    stepTurretShots(turretShots_, dt, "Baz's turret");
    // The grips' last state, every frame (a squeeze already held when it is planted is not a fresh one).
    bool fresh[2];
    for (int h = 0; h < 2; ++h) {
        const bool sq = in.hand[h].squeeze > 0.8f;
        fresh[h] = sq && !turretGripWas_[h];
        turretGripWas_[h] = sq;
    }
    if (!turret_.up) return;
    if (length(Vec3{player_.position.x - turret_.pos.x, 0, player_.position.z - turret_.pos.z}) > 60.0f) { recallTurret("left behind"); return; }
    if (turret_.folding) {
        turret_.rise -= dt / kTurretRise;
        if (turret_.rise <= 0) {
            turret_.up = false;
            turretReadyT_ = kTurretRecharge;
            LOGI("Turret: folded away (back in %.0f s)", double(kTurretRecharge));
        }
        return;
    }
    turret_.rise = std::min(1.0f, turret_.rise + dt / kTurretRise);
    // Taken back: a squeeze of the grip with the hand on its head (first person) or standing by it.
    const Vec3 head = turret_.pos + Vec3{0, 0.85f - kTurretSink, 0};
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        if (!fresh[h] || !c.gripPoseValid || hero_ != Hero::Mecc || dying_) continue;
        // (third person: the gun hand only - the other's grip is sprint; first person: not a hand already holding
        // something - a stone, a body, the barrel of a gun held in two hands)
        if (!firstPerson_ && h != rig_.turnHand()) continue;
        if (firstPerson_ && (stoneInHand(h) || heldBody_.hand == h || (twoHand_ && h != rig_.turnHand()))) continue;
        const Vec3 hp = rig_.toWorld(c.gripPose).position;
        static const bool trace = std::getenv("GVR_TURRET_TRACE") != nullptr;  // test hook
        if (trace) LOGI("turret trace: grip %d at %.2f m from its head (hand %.1f %.1f %.1f, head %.1f %.1f %.1f, Baz %.1f m off)", h, double(length(hp - head)), hp.x, hp.y, hp.z, head.x, head.y, head.z, double(length(player_.position - turret_.pos)));
        const bool near = firstPerson_ ? length(hp - head) < 0.6f * std::max(1.0f, rig_.comfort.worldScale)
                                       : length(Vec3{player_.position.x - turret_.pos.x, 0, player_.position.z - turret_.pos.z}) < 1.6f;
        if (!near) continue;
        turret_.up = false;
        sfx("me_minth", turret_.pos, 0.8f, 1.2f);
        haptics_.push_back({h, 0.6f, 0.1f});
        LOGI("Turret: taken back (%d rounds left)", turretRounds_);
        if (mgLoop_ && audio_) { audio_->stop(mgLoop_); mgLoop_ = 0; }
        weaponSel_ = 11;
        weaponHudT_ = 0;
        sfx2D("ph_selct", 0.7f);
        turretTrigBlock_ = in.hand[rig_.turnHand()].trigger > 0.6f;  // (a trigger held now doesn't plant it again)
        return;
    }
    // The nearest Reaper it can see.
    const Vec3 eye = turret_.pos + Vec3{0, 0.9f - kTurretSink, 0};
    const Enemy* tgt = nullptr;
    float best = kTurretRange * kTurretRange;
    for (const auto& e : enemies_.enemies) {
        if (e.state == EnemyState::Dead || e.remove) continue;
        const Vec3 v = e.pos + Vec3{0, 1.1f, 0} - eye;
        const float d2 = dot(v, v);
        // (the one it is on keeps it unless another is clearly nearer: no swinging between two)
        const float score = e.uid == turret_.target ? d2 * 0.64f : d2;
        if (d2 >= kTurretRange * kTurretRange || score >= best) continue;
        const float d = std::sqrt(d2);
        float t;
        Vec3 n;
        const Vec3 dirv = v * (1.0f / std::max(d, 1e-3f));
        if (worldRay_ && d > 2.0f && worldRay_(eye + dirv * 0.8f, dirv, d - 1.6f, t, n)) continue;  // behind a wall or a hill
        best = score;
        tgt = &e;
    }
    turret_.fireT = std::max(0.0f, turret_.fireT - dt);
    // The muzzle (game frame: the nozzle turned about the centre by aimYaw), to the world.
    auto muzzleNow = [&]() {
        const float c = std::cos(turret_.aimYaw), s = std::sin(turret_.aimYaw);
        const Vec3 o = gNozzle - gCentre;
        const Vec3 g = gCentre + Vec3{o.x * c - o.y * s, o.x * s + o.y * c, o.z};
        const Vec3 r = giantsToRuntime(g.x, g.y, g.z);
        const Vec4 w = turret_.xf * Vec4{r.x, r.y - kTurretSink, r.z, 1.0f};  // (xf turns about Y only: the sink is local Y)
        return Vec3{w.x, w.y, w.z};
    };
    turret_.muzzle = muzzleNow();
    if (!tgt) { turret_.target = 0; return; }
    if (turret_.target != tgt->uid) {
        turret_.target = tgt->uid;
        LOGI("Turret: on Reaper %u, %.0f m away", tgt->uid, double(length(tgt->pos - turret_.pos)));
    }
    // The heading wanted, in the model's game frame: the target's offset brought back into it.
    const Mat4 inv = affineInverse(turret_.xf);
    const Vec4 lv = inv * Vec4{tgt->pos.x - turret_.pos.x, 0, tgt->pos.z - turret_.pos.z, 0};
    // runtime local (x, y, z) = game (-x, z, y): game x = -lx, game y = lz.
    const float want = std::atan2(lv.z, -lv.x) - gBarrel;
    float diff = want - turret_.aimYaw;
    while (diff > kPi) diff -= 2 * kPi;
    while (diff < -kPi) diff += 2 * kPi;
    turret_.aimYaw += std::max(-3.0f * dt, std::min(3.0f * dt, diff));
    if (std::fabs(diff) < 0.12f && turret_.fireT <= 0 && turret_.rise >= 1.0f && turretRounds_ > 0) {
        turret_.fireT = kTurretInterval;
        const Vec3 mz = muzzleNow();
        const Vec3 aim = tgt->pos + Vec3{0, 1.1f, 0} + Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} * 0.4f;
        const Vec3 dv = normalize(aim - mz);
        static const bool trace = std::getenv("GVR_TURRET_TRACE") != nullptr;  // test hook: the barrel's line against the target's
        if (trace) {
            const Vec3 hd = turret_.pos + Vec3{0, 0.62f - kTurretSink, 0};
            const Vec3 a{mz.x - hd.x, 0, mz.z - hd.z}, b{tgt->pos.x - hd.x, 0, tgt->pos.z - hd.z};
            LOGI("turret trace: barrel %.0f deg off the target", double(std::acos(std::max(-1.0f, std::min(1.0f, dot(normalize(a), normalize(b))))) * 57.2958f));
        }
        turretShots_.push_back({mz + dv * 0.3f, dv * 70.0f, 1.0f, tgt->uid, kTurretDamage});
        addFlash(mz, {2.0f, 1.4f, 0.5f}, 4.0f, 0.06f);
        sfx("me_gtfr1", mz, 0.7f, 1.1f + 0.1f * frand());
        if (--turretRounds_ <= 0) {
            turret_.folding = true;
            sfx("ph_enter", turret_.pos, 0.6f, 0.8f);
            LOGI("Turret: out of rounds - it folds away");
        }
    }
}

void GameApp::renderPortTurret() {
    if (turretModel_ && turret_.up) {
        if (std::fabs(turret_.aimYaw - turret_.posedYaw) > 0.01f) {
            turretModel_->pose(renderer_, 0.0f);
            turret_.posedYaw = turret_.aimYaw;
            static const bool trace = std::getenv("GVR_TURRET_TRACE") != nullptr;  // test hook: the drawn nozzle against ours
            Mat4 nm;
            if (trace && turretModel_->nodeWorld("ref_nozzle01", nm)) {
                const Vec3 r = giantsToRuntime(nm.m[12], nm.m[13], nm.m[14]);
                const Vec4 w = turret_.xf * Vec4{r.x, r.y - kTurretSink, r.z, 1.0f};
                LOGI("turret trace: drawn nozzle %.2f m from the muzzle shots leave", double(length(Vec3{w.x, w.y, w.z} - turret_.muzzle)));
            }
        }
        const float sink = kTurretSink + (1.0f - std::max(0.0f, std::min(1.0f, turret_.rise))) * 0.9f;
        turretModel_->render(renderer_, Mat4::translation({0, -sink, 0}) * turret_.xf);
    }
    drawTurretShots(turretShots_, Vec4{2.4f, 1.5f, 0.5f, 1.0f});
}

}  // namespace gvr
