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
// The terrain or a roof / ledge under (x, z) no higher than maxY (what a falling body would land on).
float floorUnder(const EnemyDirector& d, const Vec3& p, float maxY) {
    float g = groundOf(d, p), r;
    if (d.collision && d.collision->groundAt(p.x, p.z, maxY, r) && r > g) g = r;
    return g;
}
constexpr float kFollowRange = 30.0f;    // m
constexpr float kFollowCone = 0.122f;    // rad (7 degrees) off the hand's aim
constexpr float kFollowPull = 1.2f;      // m/s of the hand toward her (horizontal, relative to the head) that catches - over 12 cm
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
    e.wallHit = 0;
    e.fallFrom = e.pos.y;
    e.aware = true;
    enemies_.setBodyClip(ei, EnemyClip::FallCycle, true);
    const float speed = std::min(sp, kFollowMaxSpeed);
    followThrown_.push_back({e.uid, speed, 0.0f});  // (watched to the ground: hurt if thrown hard, drowned in the sea)
    LOGI("FOLLOW: Reaper %u let go (%s) at %.1f m/s, %.1f m up", e.uid, why, double(speed), double(e.pos.y - groundOf(enemies_, e.pos)));
}

void GameApp::updateFollow(float dt, const InputFrame& in, const Vec3& dir) {
    followCool_ = std::max(0.0f, followCool_ - dt);
    followCandT_ = std::max(0.0f, followCandT_ - dt);
    shrinkCool_ = std::max(0.0f, shrinkCool_ - dt);
    const int mh = rig_.moveHand();
    const ControllerState& c = in.hand[mh];
    const bool held = c.trigger > 0.4f;
    // SHRINK (game_app_shrink.cpp): the same steady aim, then the hand closes (a grip squeeze).
    const bool gripEdge = c.squeeze > 0.8f && !shrinkGripHeld_;
    if (c.squeeze > 0.8f) shrinkGripHeld_ = true;
    else if (c.squeeze < 0.4f) shrinkGripHeld_ = false;
    // (review 2026-10-28) Not run last frame (dying, riding, the menu, a cutscene): no hand speed across the gap.
    if (time_ - followLastT_ > 0.1) { followHandValid_ = false; followPullDist_ = 0; followAimT_ = 0; }
    followLastT_ = time_;
    // The hand's speed toward her head (horizontally), relative to the head - stepping back is not a pull.
    Vec3 hv{0, 0, 0};
    if (c.gripPoseValid && in.headValid && dt > 0) {
        const Vec3 rel = c.gripPose.position - in.head.position;
        if (followHandValid_) hv = (rel - followHandPrev_) * (1.0f / dt);
        followHandPrev_ = rel;
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
        // (review: kept on every frame of the hold - a trigger easing between 0.4 and 0.6 must not let the other
        // gestures, a fire bolt or a charged fire wall / tornado through when he is let go)
        spellGestureCast_ = true;
        spellCharge_ = 0;
        if (!held) { releaseFollow("the trigger"); return; }
        if (followT_ > kFollowHold || !c.aimPoseValid) { releaseFollow("time"); return; }
        if (!firstPerson_ || delphiBow()) { releaseFollow("the view / a bow"); return; }
        Enemy& e = enemies_.enemies[size_t(ei)];
        // He floats where the hand points, at the distance he was caught at (his middle on the line).
        const Pose aim = rig_.toWorld(c.aimPose);
        const Vec3 ad = aim.orientation.rotate(Vec3{0, 0, -1});
        Vec3 want = aim.position + ad * followDist_ - Vec3{0, 1.1f, 0};
        want.y = std::max(want.y, floorUnder(enemies_, want, e.pos.y + 0.3f) + 0.5f);  // (not dragged through the ground: that would land him)
        Vec3 step = (want - e.pos) * std::min(1.0f, 8.0f * dt);
        const float sl = length(step), maxStep = kFollowMaxSpeed * dt;
        if (sl > maxStep) step = step * (maxStep / sl);
        // Not dragged through walls or rock: stopped short of what is in the way.
        {
            const Vec3 c0 = e.pos + Vec3{0, 1.1f, 0};
            const float l = length(step);
            float t;
            Vec3 n;
            if (worldRay_ && l > 1e-3f && worldRay_(c0, step * (1.0f / l), l + 0.5f, t, n)) step = step * (std::max(0.0f, t - 0.5f) / l);
        }
        const Vec3 v = dt > 0 ? step * (1.0f / dt) : Vec3{0, 0, 0};
        followVel_ = followVel_ * 0.5f + v * 0.5f;
        e.pos = e.pos + step;
        e.pos.y = std::max(e.pos.y, floorUnder(enemies_, e.pos, e.pos.y + 0.3f) + 0.3f);  // (trailing up a slope or over a roof: kept off it)
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
    rtCool_ = std::max(0.0f, rtCool_ - dt);
    if (!(followOwned() || shrinkOwned() || rsTeleportOwned()) || !firstPerson_ || !c.aimPoseValid || !c.gripPoseValid || !in.headValid || !held || delphiBow() || spellGestureCast_) {
        followCandUid_ = 0;
        followAimT_ = followPullDist_ = 0;
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
        if (d > kFollowRange || d < 1.5f || e.dropping) continue;
        const float a = std::acos(std::max(-1.0f, std::min(1.0f, dot(to * (1.0f / d), dir))));
        if (a >= bestA) continue;
        float t;
        Vec3 n;
        if (worldRay_ && worldRay_(from, to * (1.0f / d), d - 1.0f, t, n)) continue;  // (behind a wall or a hill)
        bestA = a; cand = e.uid; candD = d;
    }
    // Pointed at steadily (0.2 s on the same one) before the pull counts: the wind-up of a thrown fire bolt doesn't.
    if (cand && cand == followCandUid_ && followCandT_ > 0) followAimT_ += dt;
    else if (cand) followAimT_ = 0;
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
    // The hand closed on him (trigger held, pointed at steadily): SHRINK. (2026-10-29 review) The same grip runs
    // (turbo) with the stick pushed: only standing - the stick near centre - does closing the hand cast; on
    // cooldown it does nothing (a charged spell stays charged).
    const bool stickIdle = c.thumbstick.x * c.thumbstick.x + c.thumbstick.y * c.thumbstick.y < 0.09f;
    if (gripEdge && !stickIdle && ctrace && shrinkOwned() && followCandUid_) LOGI("spell trace: SHRINK not cast - the grip runs (stick pushed)");
    if (gripEdge && stickIdle && shrinkCool_ <= 0 && shrinkOwned() && followCandUid_ && followCandT_ > 0 && followAimT_ >= 0.2f) {
        castShrink(followCandUid_, mh);
        return;
    }
    // Pulled back toward her: caught.
    const Vec3 toHead{in.head.position.x - c.gripPose.position.x, 0, in.head.position.z - c.gripPose.position.z};
    const float pull = length(toHead) > 1e-3f ? dot(hv, normalize(toHead)) : 0.0f;
    // The pull: fast, and over at least 12 cm.
    if (pull > 1.0f) followPullDist_ += pull * dt;
    else if (pull < 0.5f) followPullDist_ = 0;
    // No Reaper in the aim: the same pull on open ground is TELEPORT (game_app_rsteleport.cpp).
    if (!(followCandUid_ && followCandT_ > 0) && updateRsTeleport(dt, pull, followPullDist_, from, dir, mh)) {
        followPullDist_ = 0;
        return;
    }
    if (!followOwned()) return;
    static const bool trace = std::getenv("GVR_SPELL_TRACE") != nullptr;
    if (trace && pull > 0.8f) LOGI("spell trace: FOLLOW pull %.1f m/s (%.2f m), candidate %u (%.1f s left, aimed %.2f s)", double(pull), double(followPullDist_), followCandUid_, double(followCandT_), double(followAimT_));
    if (pull < kFollowPull || followPullDist_ < 0.12f || !followCandUid_ || followCandT_ <= 0 || followAimT_ < 0.2f || followCool_ > 0) return;
    const int ei = enemyOfUid(followCandUid_);
    if (ei < 0) return;
    Enemy& e = enemies_.enemies[size_t(ei)];
    if (e.state == EnemyState::Dead || e.state == EnemyState::Falling) return;
    endCloak("she cast");
    enemies_.knockOff(ei, Vec3{0, 0, 0});
    e.pos.y += 0.6f;  // (off his feet at once: on the ground the fall would end in a landing next frame)
    enemies_.setBodyClip(ei, EnemyClip::SpellCaught, false);
    followUid_ = e.uid;
    followCandUid_ = 0;
    followAimT_ = followPullDist_ = 0;
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
        if (e.state == EnemyState::Falling && e.wallHit > 0) {
            // Slammed into a wall or rock (EnemyDirector stops him there): it hurts like the landing.
            const float hit = e.wallHit;
            e.wallHit = 0;
            t.speed = std::min(t.speed, length(e.vel));  // (2026-10-29 review: stopped by the wall - he only drops; not hurt twice)
            if (hit > 6.0f) {
                const int obj = e.objective;
                const bool killed = enemies_.damage(ti, 5.0f * hit);
                if (killed && obj >= 0) completeObjective(obj);
                bleed(c, Vec3{0, 1, 0}, killed, 0.8f);
                sfx("ex_bdys1", c, 1.0f, 0.85f);
                LOGI("FOLLOW: the thrown Reaper hits a wall at %.1f m/s (%.0f damage%s)", double(hit), double(5.0f * hit), killed ? ", dead" : "");
            }
        }
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
        t.age = 99;
        // Into the sea (the Reapers' floor there is an invisible shelf under the surface): drowned.
        if (e.pos.y < player_.seaLevel - 1.0f) {  // (deep: at the shelf - the shallows only wet him)
            if (e.state != EnemyState::Dead) {
                const int obj = e.objective;
                if (enemies_.damage(ti, 1e5f) && obj >= 0) completeObjective(obj);
            }
            waterSplash(Vec3{e.pos.x, player_.seaLevel, e.pos.z}, 1.2f, 2);
            LOGI("FOLLOW: the Reaper she let go drowns");
            continue;
        }
        if (t.speed <= 6.0f) continue;  // (just let go: an ordinary fall)
        if (e.state != EnemyState::Dead) {
            const int obj = e.objective;
            const bool killed = enemies_.damage(ti, 6.0f * t.speed);
            if (killed && obj >= 0) completeObjective(obj);
            LOGI("FOLLOW: the thrown Reaper lands (%.0f damage%s)", double(6.0f * t.speed), killed ? ", dead" : "");
        }
        bleed(c, Vec3{0, 1, 0}, e.state == EnemyState::Dead, 1.0f);
        sfx("ex_bdys1", e.pos, 1.0f, 0.8f);
    }
    followThrown_.erase(std::remove_if(followThrown_.begin(), followThrown_.end(), [](const FollowThrown& t) { return t.age > 20.0f; }), followThrown_.end());
}

}  // namespace gvr
