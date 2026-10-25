// GameApp, rework: VR-native handling for the Meccaryn (first person).
// - Holsters on the body: reach to your right hip and squeeze the grip for the handgun; over your
//   right shoulder for a long gun (again for the next one you carry); to your left hip (cross-body)
//   for a grenade. A tick in the controller tells you your hand is at a holster. The right stick
//   still cycles them too.
// - Punches: a hand that isn't holding a gun hits a Reaper it swings into fast enough (the faster,
//   the harder; a hard punch knocks him back).
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "audio/mixer.h"
#include "core/log.h"
#include "render/renderer.h"

namespace gvr {

namespace {
// Holster zones in the player's own frame: x right, y up from the eyes, z back (tracking metres
// at human scale).
const Vec3 kHolster[3] = {
    {0.24f, -0.78f, 0.02f},   // right hip: the handgun
    {0.16f, -0.05f, 0.18f},   // over the right shoulder: long guns
    {-0.20f, -0.72f, -0.06f}, // left hip (front): grenades
};
constexpr float kHolsterRadius = 0.17f;
}  // namespace

void GameApp::updateFlashes(float dt) {
    float best = 0;
    const Flash* pick = nullptr;
    for (auto& f : flashes_) {
        f.t += dt;
        const float k = 1.0f - f.t / f.dur;
        const float w = k * (f.color.x + f.color.y + f.color.z) * f.radius;
        if (k > 0 && w > best) { best = w; pick = &f; }
    }
    EnvironmentParams& env = renderer_.environment();
    const float cur = (env.pointLightColor.x + env.pointLightColor.y + env.pointLightColor.z) * env.pointLightRadius;
    if (pick && best > cur) {
        const float k = 1.0f - pick->t / pick->dur;
        env.pointLightPos = pick->pos;
        env.pointLightColor = pick->color * (k * k);
        env.pointLightRadius = pick->radius;
    } else if (cur <= 0) {
        env.pointLightRadius = 6.0f;
    }
    flashes_.erase(std::remove_if(flashes_.begin(), flashes_.end(), [](const Flash& f) { return f.t >= f.dur; }), flashes_.end());
}

void GameApp::updateHolsters(float dt, const InputFrame& in) {
    (void)dt;
    if (hero_ != Hero::Mecc || !firstPerson_ || !in.headValid || cutsceneActive() || menuOpen_) return;
    const int gh = rig_.turnHand();
    const ControllerState& c = in.hand[gh];
    if (!c.gripPoseValid) return;
    // Zones follow the body: the head's position and yaw (a body turns with the head over time).
    const float yaw = yawOf(in.head.orientation);
    const Quat body = Quat::fromAxisAngle({0, 1, 0}, yaw);
    const Vec3 hand = c.gripPose.position;  // tracking space (human scale)
    int zone = -1;
    for (int z = 0; z < 3; ++z) {
        Vec3 off = kHolster[z];
        if (rig_.comfort.leftHanded) off.x = -off.x;  // mirrored for a left-handed player
        const Vec3 at = in.head.position + body.rotate(off);
        if (length(hand - at) < kHolsterRadius) { zone = z; break; }
    }
    if (zone != holsterZone_) {
        if (zone >= 0) haptics_.push_back({gh, 0.25f, 0.025f});  // you're at a holster
        holsterZone_ = zone;
    }
    const bool grab = c.squeeze > 0.7f;
    if (zone >= 0 && grab && !holsterGrabHeld_) {
        int want = -1;
        if (zone == 0) want = 0;  // handgun
        else if (zone == 2) want = weaponOwned(8) ? 8 : -1;
        else {
            // Long guns in order, starting after the one in hand.
            static const int kLong[6] = {4, 5, 6, 7, 1, -1};
            int at = 0;
            while (at < 5 && kLong[at] != weaponSel_) ++at;
            for (int k = 1; k <= 5; ++k) {
                const int s = kLong[(at + k) % 5];
                if (weaponOwned(s)) { want = s; break; }
            }
        }
        if (want >= 0 && want != weaponSel_) {
            weaponSel_ = want;
            weaponHudT_ = 0;
            if (mgLoop_ && audio_) { audio_->stop(mgLoop_); mgLoop_ = 0; }
            sfx2D("me_drpwp", 0.6f, 1.3f);
            haptics_.push_back({gh, 0.6f, 0.06f});
            LOGI("Holster: drew weapon %d", want);
        }
    }
    holsterGrabHeld_ = grab;
}

void GameApp::updateMelee(float dt, const InputFrame& in) {
    if (hero_ != Hero::Mecc || !playMode_ || dying_ || cutsceneActive() || dt <= 0) return;
    for (int h = 0; h < 2; ++h) {
        punchCool_[h] = std::max(0.0f, punchCool_[h] - dt);
        const ControllerState& c = in.hand[h];
        if (!c.gripPoseValid) { punchHave_[h] = false; continue; }
        const Vec3 p = rig_.toWorld(c.gripPose).position;
        // The hand's own speed: its world motion less the player's.
        Vec3 v{0, 0, 0};
        if (punchHave_[h]) v = (p - punchLast_[h]) * (1.0f / dt) - player_.velocity;
        punchLast_[h] = p;
        punchHave_[h] = true;
        const bool armed = h == rig_.turnHand() && gunValid_;
        if (armed || punchCool_[h] > 0) continue;
        const float speed = length(v);
        static const bool trace = std::getenv("GVR_MELEE_TRACE") != nullptr;
        if (trace && speed > 1.0f && !enemies_.enemies.empty())
            LOGI("melee-trace hand %d speed %.1f at (%.2f %.2f %.2f) enemy (%.2f %.2f %.2f)", h, speed, p.x, p.y, p.z, enemies_.enemies[0].pos.x,
                 enemies_.enemies[0].pos.y, enemies_.enemies[0].pos.z);
        if (speed < 2.6f) continue;
        for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
            const Enemy& e = enemies_.enemies[i];
            if (e.state == EnemyState::Dead) continue;
            const float dy = p.y - e.pos.y;
            if (dy < 0.3f || dy > 2.1f) continue;
            if (length(Vec3{p.x - e.pos.x, 0, p.z - e.pos.z}) > 0.55f) continue;
            const float dmg = std::min(60.0f, 18.0f + (speed - 2.6f) * 12.0f);
            const Vec3 dir = normalize(Vec3{v.x, 0, v.z} + Vec3{1e-4f, 0, 0});
            const bool killed = enemies_.damage(int(i), dmg, speed > 5.0f);
            if (killed && e.objective >= 0) completeObjective(e.objective);
            enemies_.push(int(i), dir * std::min(9.0f, speed * 1.4f));
            bleed(p, dir, killed, 1.0f);
            if (killed) blood_.pool(e.pos, 1.3f);
            haptics_.push_back({h, 1.0f, 0.09f});
            sfx("ex_bdys1", p, 0.9f, 0.8f + 0.1f * frand());
            static const char* hits[] = {"rpman_hit1", "rpman_hit2", "rpman_hit3", "rpman_hit4"};
            sfx(killed ? "rpman_die1" : hits[int(frand() * 3.99f)], p, 0.9f);
            punchCool_[h] = 0.35f;
            LOGI("Punch: %.1f m/s, %.0f damage%s", speed, dmg, killed ? " - down" : "");
            break;
        }
    }
}

}  // namespace gvr
