// (2026-10-28) Delphi's FOLLOW spell: weapondefs item 30 (IconRSFollow, GText "Follow", HUD art st_rp_follow) and
// the caught / loop clips the data has for its victims (rpman_followspell_caught: a Reaper lifted off his feet,
// rpman_followspell_loop: hanging limp in the air; Rippers, Vimps, Smarties and Meccaryns have them too). No mission
// hands it out and nothing in the data says how it is cast; the rules are ours (game_app.h).
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "assets/gzp.h"
#include "scene/asset_world.h"

namespace gvr {

namespace {
// The ground the Reapers land on (their own height function).
float groundOf(const EnemyDirector& d, const Vec3& p) { return d.groundHeight ? d.groundHeight(p.x, p.z) : p.y - 10.0f; }
constexpr float kFollowRange = 30.0f;    // m
constexpr float kFollowCone = 0.122f;    // rad (7 degrees) off the hand's aim
constexpr float kFollowPull = 1.4f;      // m/s of the hand toward her (horizontal) that catches
constexpr float kFollowHold = 6.0f;      // s at most
constexpr float kFollowCool = 12.0f;     // s between
constexpr float kFollowMaxSpeed = 22.0f; // m/s he can be moved / thrown at
}  // namespace

bool GameApp::followOwned() const {
    if (hero_ != Hero::Delphi || !assetWorld_) return false;
    return practice_ || toLower(worldName_) == "rstory5";
}

void GameApp::releaseFollow(const char* why) {
    if (!followUid_) return;
    const int ei = enemyOfUid(followUid_);
    followUid_ = 0;
    followCool_ = kFollowCool;
    if (ei < 0) return;
    Enemy& e = enemies_.enemies[size_t(ei)];
    if (e.state != EnemyState::Falling) return;
    Vec3 v = followVel_;
    const float sp = length(v);
    if (sp > kFollowMaxSpeed) v = v * (kFollowMaxSpeed / sp);
    e.vel = v;
    e.fallFrom = e.pos.y;
    e.aware = true;
    enemies_.setBodyClip(ei, EnemyClip::FallCycle, true);
    const float speed = std::min(sp, kFollowMaxSpeed);
    if (speed > 6.0f) followThrown_.push_back({e.uid, speed, 0.0f});
    LOGI("FOLLOW: Reaper %u let go (%s) at %.1f m/s, %.1f m up", e.uid, why, double(speed), double(e.pos.y - groundOf(enemies_, e.pos)));
}

void GameApp::updateFollow(float dt, const InputFrame& in, const Vec3& dir) {
    followCool_ = std::max(0.0f, followCool_ - dt);
    followCandT_ = std::max(0.0f, followCandT_ - dt);
    stepFollowThrown(dt);
    const int mh = rig_.moveHand();
    const ControllerState& c = in.hand[mh];
    const bool held = c.trigger > 0.4f;
    // The hand's speed (tracking space) toward her head, horizontally.
    Vec3 hv{0, 0, 0};
    if (c.gripPoseValid && dt > 0) {
        if (followHandValid_) hv = (c.gripPose.position - followHandPrev_) * (1.0f / dt);
        followHandPrev_ = c.gripPose.position;
        followHandValid_ = true;
    } else {
        followHandValid_ = false;
    }
    if (followUid_) {
        const int ei = enemyOfUid(followUid_);
        if (ei < 0 || enemies_.enemies[size_t(ei)].state != EnemyState::Falling || hero_ != Hero::Delphi) {  // (killed in the air)
            followUid_ = 0;
            followCool_ = kFollowCool;
            LOGI("FOLLOW: the hold is broken");
            return;
        }
        followT_ += dt;
        if (!held) { releaseFollow("the trigger"); return; }
        if (followT_ > kFollowHold || !c.aimPoseValid) { releaseFollow("time"); return; }
        Enemy& e = enemies_.enemies[size_t(ei)];
        // He floats where the hand points, at the distance he was caught at (his middle on the line).
        const Pose aim = rig_.toWorld(c.aimPose);
        const Vec3 ad = aim.orientation.rotate(Vec3{0, 0, -1});
        Vec3 want = aim.position + ad * followDist_ - Vec3{0, 1.1f, 0};
        want.y = std::max(want.y, groundOf(enemies_, want) + 0.5f);  // (not dragged through the ground: that would land him)
        Vec3 step = (want - e.pos) * std::min(1.0f, 8.0f * dt);
        const float sl = length(step), maxStep = kFollowMaxSpeed * dt;
        if (sl > maxStep) step = step * (maxStep / sl);
        const Vec3 v = dt > 0 ? step * (1.0f / dt) : Vec3{0, 0, 0};
        followVel_ = followVel_ * 0.5f + v * 0.5f;
        e.pos = e.pos + step;
        e.vel = Vec3{0, 0, 0};
        e.fallFrom = e.pos.y;
        // Caught, then hanging limp.
        if (e.clip == EnemyClip::SpellCaught && e.clipTime >= enemies_.clipDuration[int(EnemyClip::SpellCaught)] * 0.97f)
            enemies_.setBodyClip(ei, EnemyClip::SpellFloat, true);
        if (frand() < dt * 30.0f) {  // a thread of her blue fire from the hand to him
            const float u = frand();
            smokePuff(FxTex::FireBlue, aim.position + (e.pos + Vec3{0, 1.1f, 0} - aim.position) * u, Vec3{0, 0.2f, 0}, 0.18f, 0.25f, 0.3f, 0.8f, 0.0f);
        }
        if (int(followT_ * 4) != int((followT_ - dt) * 4)) haptics_.push_back({mh, 0.25f, 0.04f});
        return;
    }
    if (!followOwned() || !firstPerson_ || !c.aimPoseValid || !c.gripPoseValid || !in.headValid || !held || delphiBow()) {
        followCandUid_ = 0;
        return;
    }
    // The Reaper she points at (nearest the line within the cone), remembered a moment.
    const Vec3 from = rig_.toWorld(c.aimPose).position;
    float bestA = kFollowCone;
    uint32_t cand = 0;
    float candD = 0;
    for (const auto& e : enemies_.enemies) {
        if (e.state == EnemyState::Dead || e.state == EnemyState::Falling || e.remove) continue;
        const Vec3 to = e.pos + Vec3{0, 1.1f, 0} - from;
        const float d = length(to);
        if (d > kFollowRange || d < 1.5f) continue;
        const float a = std::acos(std::max(-1.0f, std::min(1.0f, dot(to * (1.0f / d), dir))));
        if (a < bestA) { bestA = a; cand = e.uid; candD = d; }
    }
    if (cand) { followCandUid_ = cand; followCandT_ = 0.4f; followDist_ = candD; }
    static const bool ctrace = std::getenv("GVR_SPELL_TRACE") != nullptr;
    if (ctrace && int(time_ * 2) != int((time_ - dt) * 2))
        for (const auto& e : enemies_.enemies) {
            const Vec3 to = e.pos + Vec3{0, 1.1f, 0} - from;
            if (length(to) > kFollowRange * 2) continue;
            const float yawOff = std::atan2(-to.x, -to.z) - std::atan2(-dir.x, -dir.z);
            LOGI("spell trace: FOLLOW Reaper %u %.1f m, %.0f deg off the aim (yaw %+.0f, pitch %+.0f)", e.uid, double(length(to)),
                 double(std::acos(std::max(-1.0f, std::min(1.0f, dot(normalize(to), dir)))) * 57.3f),
                 double(std::remainder(yawOff, 2 * kPi) * 57.3f), double((std::asin(normalize(to).y) - std::asin(dir.y)) * 57.3f));
        }
    // Pulled back toward her: caught.
    const Vec3 toHead{in.head.position.x - c.gripPose.position.x, 0, in.head.position.z - c.gripPose.position.z};
    const float pull = length(toHead) > 1e-3f ? dot(hv, normalize(toHead)) : 0.0f;
    static const bool trace = std::getenv("GVR_SPELL_TRACE") != nullptr;
    if (trace && pull > 0.8f) LOGI("spell trace: FOLLOW pull %.1f m/s, candidate %u (%.1f s left)", double(pull), followCandUid_, double(followCandT_));
    if (pull < kFollowPull || !followCandUid_ || followCandT_ <= 0 || followCool_ > 0) return;
    const int ei = enemyOfUid(followCandUid_);
    if (ei < 0) return;
    Enemy& e = enemies_.enemies[size_t(ei)];
    if (e.state == EnemyState::Dead || e.state == EnemyState::Falling) return;
    endCloak("she cast");
    enemies_.knockOff(ei, Vec3{0, 0, 0});
    e.pos.y += 0.6f;  // (off his feet at once: on the ground the fall would end in a landing next frame)
    enemies_.setBodyClip(ei, EnemyClip::SpellCaught, false);
    e.weaponOn = false;
    followUid_ = e.uid;
    followCandUid_ = 0;
    followT_ = 0;
    followVel_ = Vec3{0, 0, 0};
    followDist_ = std::max(3.0f, std::min(10.0f, followDist_));  // (brought in close, or kept off a little)
    spellGestureCast_ = true;  // (the trigger's release lets him go, not a fire bolt)
    spellCharge_ = 0;
    sfx("sr_spfsh", e.pos + Vec3{0, 1.1f, 0}, 1.0f, 0.6f);
    sfx("rpman_hit1", e.pos, 0.8f, 0.8f);
    haptics_.push_back({mh, 0.9f, 0.2f});
    LOGI("Delphi casts FOLLOW: Reaper %u caught %.0f m away (hand pulled at %.1f m/s)", e.uid, double(followDist_), double(pull));
}

// Thrown: bowls over the Reapers he meets in flight; hurt where he lands, by how hard he was thrown.
void GameApp::stepFollowThrown(float dt) {
    for (auto& t : followThrown_) {
        t.age += dt;
        const int ti = enemyOfUid(t.uid);
        if (ti < 0) { t.age = 99; continue; }
        Enemy& e = enemies_.enemies[size_t(ti)];
        const Vec3 c = e.pos + Vec3{0, 0.9f, 0};
        if (e.state == EnemyState::Falling) {
            for (size_t j = 0; j < enemies_.enemies.size(); ++j) {
                if (int(j) == ti) continue;
                Enemy& o = enemies_.enemies[j];
                if (o.state == EnemyState::Dead || o.state == EnemyState::Falling || length(o.pos + Vec3{0, 0.9f, 0} - c) > 1.3f) continue;
                const bool killed = enemies_.damage(int(j), 3.0f * t.speed);
                if (killed && o.objective >= 0) completeObjective(o.objective);
                if (!killed) enemies_.knockOff(int(j), e.vel * 0.5f + Vec3{0, 2.0f, 0});
                sfx("ex_bdys1", c, 0.9f, 0.9f);
                LOGI("FOLLOW: a thrown Reaper bowls over Reaper %u%s", o.uid, killed ? " (dead)" : "");
                e.vel = e.vel * 0.4f;
                break;
            }
            continue;
        }
        if (e.state != EnemyState::Dead) {
            const int obj = e.objective;
            const bool killed = enemies_.damage(ti, 6.0f * t.speed);
            if (killed && obj >= 0) completeObjective(obj);
            LOGI("FOLLOW: the thrown Reaper lands (%.0f damage%s)", double(6.0f * t.speed), killed ? ", dead" : "");
        }
        bleed(c, Vec3{0, 1, 0}, e.state == EnemyState::Dead, 1.0f);
        sfx("ex_bdys1", e.pos, 1.0f, 0.8f);
        t.age = 99;
    }
    followThrown_.erase(std::remove_if(followThrown_.begin(), followThrown_.end(), [](const FollowThrown& t) { return t.age > 20.0f; }), followThrown_.end());
}

}  // namespace gvr
