// GameApp, rework: VR-native handling for the Meccaryn (first person).
// - Holsters on the body: reach to your right hip and squeeze the grip for the handgun; over your
//   right shoulder for a long gun (again for the next one you carry); to your left hip (cross-body)
//   for a grenade. A tick in the controller tells you your hand is at a holster. The right stick
//   still cycles them too.
// - Punches: a hand that isn't holding a gun hits a Reaper it swings into fast enough (the faster,
//   the harder; a hard punch knocks him back).
#include "game/game_app.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "audio/mixer.h"
#include "core/log.h"
#include "game/arm_ik.h"
#include "render/renderer.h"
#include "scene/asset_world.h"

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

// The top of the player's hit capsule above his feet: his real head in first person (duck behind
// cover and Reaper bolts pass over), the body's height otherwise.
float GameApp::playerHitTop() const {
    const float hs = heroScale_;
    if (firstPerson_ && lastInput_.headValid && hero_ != Hero::Kabuto) {
        const float hy = rig_.toWorld(lastInput_.head).position.y - player_.position.y;
        return std::max(0.7f * hs, std::min(1.85f * hs, hy + 0.1f * hs));
    }
    return 1.65f * hs;
}

// Reaper grenades (rework; ours): a Reaper who knows where you are but has lost sight of you for
// 1.5 s (you ducked behind cover) lobs a grenade at where you are - between 8 and 26 m, each at
// most every 9 s, the squad at most every 3.5 s. A throw sound warns you; it bounces like yours.
void GameApp::updateReaperGrenades(float dt) {
    if (hero_ == Hero::Kabuto || !playMode_ || dying_ || std::getenv("GVR_NO_REAPER_GRENADES")) return;
    rpGrenadeCool_ = std::max(0.0f, rpGrenadeCool_ - dt);
    for (auto& e : enemies_.enemies) {
        e.grenadeCool = std::max(0.0f, e.grenadeCool - dt);
        const bool fighting = e.aware && (e.state == EnemyState::Fire || e.state == EnemyState::Advance) && !e.posted;
        if (!fighting) { e.hiddenT = 0; continue; }
        e.hiddenT = e.los ? 0.0f : e.hiddenT + dt;
        const Vec3 to = player_.position - e.pos;
        const float d = std::sqrt(to.x * to.x + to.z * to.z);
        if (e.hiddenT < 1.5f || e.grenadeCool > 0 || rpGrenadeCool_ > 0 || d < 8.0f || d > 26.0f) continue;
        const Vec3 from = e.pos + Vec3{0, 1.7f, 0};
        const Vec3 aim = player_.position + Vec3{(frand() - 0.5f) * 2.0f, 0.2f, (frand() - 0.5f) * 2.0f};
        const float t = std::max(0.9f, std::min(1.8f, d / 13.0f));
        Vec3 v = (aim - from) * (1.0f / t);
        v.y += 0.5f * 9.8f * t;
        rockets_.push_back({from, v, 0.0f, 6});
        e.grenadeCool = 9.0f;
        rpGrenadeCool_ = 3.5f;
        e.hiddenT = 0;
        sfx("me_grnth", from, 1.0f, 0.8f);
        LOGI("Reaper %u lobs a grenade (%.0f m, you were out of sight)", e.uid, d);
    }
}

// A hand within `r` (human-scale metres) of a point: reach out to take a pickup (rework).
bool GameApp::handReach(const Vec3& p, float r) const {
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = lastInput_.hand[h];
        if (c.gripPoseValid && length(rig_.toWorld(c.gripPose).position - p) < r * rig_.comfort.worldScale) return true;
    }
    return false;
}

// Ambushes (rework; ours - against long empty walks): when nothing has fought the player for a
// while (70-110 s) in a mission with Reapers in it, a squad of 2-3 comes at him from ahead, 40-55 m
// out on dry ground. Not in base missions (they have raids), races, multiplayer or as Kabuto.
void GameApp::updateAmbush(float dt) {
    if (!playMode_ || dying_ || cutsceneActive() || hero_ == Hero::Kabuto || skirmish_ || base_.on || riding_ || enemyPool_.empty() ||
        !assetWorld_ || load_.phase != LoadPhase::Idle || std::getenv("GVR_NO_AMBUSH") || !settings_.ambushes)
        return;
    bool fighting = false;
    for (const auto& e : enemies_.enemies)
        if (e.state != EnemyState::Dead && e.aware && length(e.pos - player_.position) < 70.0f) { fighting = true; break; }
    if (fighting) { ambushT_ = 0; return; }
    if (ambushDue_ <= 0) ambushDue_ = std::getenv("GVR_AMBUSH_DUE") ? float(std::atof(std::getenv("GVR_AMBUSH_DUE"))) : 70.0f + 40.0f * frand();
    if (player_.horizontalSpeed() > 0.5f || !player_.onGround) ambushT_ += dt;  // only while he's on the move
    if (ambushT_ < ambushDue_) return;
    ambushT_ = 0;
    ambushDue_ = 0;
    const float yaw = lastInput_.headValid ? yawOf(rig_.toWorld(lastInput_.head).orientation) : player_.yaw;
    const int n = 2 + int(frand() * 1.99f);
    int placed = 0;
    for (int attempt = 0; attempt < 24 && placed < n; ++attempt) {
        const float a = yaw + (frand() - 0.5f) * 2.1f;
        const float r = 40.0f + 15.0f * frand();
        Vec3 p = player_.position + Vec3{-std::sin(a) * r, 0, -std::cos(a) * r};
        float h;
        if (!assetWorld_->terrain().heightAt(p.x, p.z, h) || h < player_.seaLevel + 0.2f || std::fabs(h - player_.position.y) > 25.0f) continue;
        const Vec3 to = player_.position - p;
        const int ei = enemies_.spawn(Vec3{p.x, h, p.z}, std::atan2(-to.x, -to.z));
        if (ei < 0) continue;
        Enemy& e = enemies_.enemies[size_t(ei)];
        e.aware = true;
        e.alert = 0.3f;
        e.home = e.patrolTo = player_.position;
        ++placed;
    }
    if (placed) {
        timmyLine_ = "REAPERS AHEAD!";
        timmyLineT_ = 2.5f;
        sfx2D("rs_attk2", 0.6f, 0.9f);
        LOGI("Ambush: %d Reapers %s", placed, "ahead of the player");
    }
}

void GameApp::renderAimLaser() {
    if (!laserOn_) return;
    if (!laserMesh_) laserMesh_ = renderer_.uploadMesh(prim::box({0.5f, 0.5f, 0.5f}, std::array<uint8_t, 4>{255, 255, 255, 255}.data()));
    const Vec3 d = laserTo_ - laserFrom_;
    const float len = length(d);
    if (len < 0.05f) return;
    const Quat r = rotationBetween({0, 0, -1}, d * (1.0f / len));
    DrawParams dp;
    dp.unlit = true;
    dp.blend = BlendMode::Additive;
    dp.depthWrite = false;
    dp.noFog = true;
    dp.tint = {1.4f, 0.12f, 0.08f, 1.0f};
    renderer_.draw(laserMesh_, Mat4::translation(laserFrom_ + d * 0.5f) * Mat4::rotation(r) * Mat4::scale({0.0025f, 0.0025f, len}), dp);
    // The dot where it lands (constant angular size).
    const Vec3 eye = renderer_.currentView().eyePosition;
    const float s = std::max(0.02f, 0.006f * length(laserTo_ - eye));
    dp.tint = {2.0f, 0.25f, 0.15f, 1.0f};
    renderer_.draw(laserMesh_, Mat4::translation(laserTo_) * Mat4::rotation(headWorldRot_) * Mat4::scale({s, s, s * 0.2f}), dp);
}

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

// Objective marker (rework; ours): a small gold diamond in the direction of the nearest unfinished
// objective, 6 m out, seen through walls, with its distance - only while it is more than 20 m away.
void GameApp::renderObjectiveMarker() {
    if (!text_.ready() || objectives_.empty() || missionComplete_) return;
    const Vec3 eye = renderer_.currentView().eyePosition;
    float best = 1e18f;
    Vec3 target;
    bool have = false;
    for (size_t i = 0; i < objectives_.size(); ++i) {
        const Objective& o = objectives_[i];
        if (o.done) continue;
        Vec3 p = (o.boundsMin + o.boundsMax) * 0.5f;
        if (o.item >= 0 && size_t(o.item) < destruct_.items.size()) p = destruct_.items[size_t(o.item)].base + Vec3{0, destruct_.items[size_t(o.item)].height * 0.6f, 0};
        for (const auto& e : enemies_.enemies)
            if (e.objective == int(i) && e.state != EnemyState::Dead) { p = e.pos + Vec3{0, 2.0f, 0}; break; }
        const float d2 = dot(p - player_.position, p - player_.position);
        if (d2 < best) { best = d2; target = p; have = true; }
    }
    if (!have) return;
    const float dist = std::sqrt(best);
    if (dist < 20.0f) return;
    const Vec3 dir = normalize(target - eye);
    const Vec3 at = eye + dir * 6.0f;
    const float pulse = 0.85f + 0.15f * std::sin(float(time_) * 3.0f);
    DrawParams dp;
    dp.unlit = true;
    dp.blend = BlendMode::Additive;
    dp.depthTest = false;
    dp.depthWrite = false;
    dp.noFog = true;
    dp.doubleSided = true;
    dp.tint = {1.0f * pulse, 0.75f * pulse, 0.25f * pulse, 1.0f};
    const Quat face = headWorldRot_;
    if (!markerMesh_) {
        MeshData md;
        const Vec3 pts[4] = {{0, 0.12f, 0}, {0.08f, 0, 0}, {0, -0.12f, 0}, {-0.08f, 0, 0}};
        for (const Vec3& q : pts) {
            Vertex v{};
            v.pos[0] = q.x; v.pos[1] = q.y; v.pos[2] = q.z;
            v.normal[2] = 1;
            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
            md.vertices.push_back(v);
        }
        md.indices = {0, 1, 2, 0, 2, 3};
        markerMesh_ = renderer_.uploadMesh(md);
    }
    renderer_.draw(markerMesh_, Mat4::translation(at) * Mat4::rotation(face), dp);
    char s[32];
    std::snprintf(s, sizeof s, "%d M", int(dist + 0.5f));
    float w = 0, h = 0;
    if (const MeshId tm = text_.mesh(s, 0.07f, 1.0f, &w, &h))
        text_.draw(tm, Mat4::translation(at) * Mat4::rotation(face) * Mat4::translation({0, -0.2f, 0}), {1.0f, 0.85f, 0.45f, 0.9f}, false);
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
