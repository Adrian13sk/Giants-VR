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
        const bool fighting = e.aware && (e.state == EnemyState::Fire || e.state == EnemyState::Advance) && !e.posted &&
                              e.shrinkT <= 0 && e.shrink >= 0.9f;  // (2026-10-29 review: shrunk, he only runs)
        if (!fighting) { e.hiddenT = 0; continue; }
        e.hiddenT = e.los ? 0.0f : e.hiddenT + dt;
        const Vec3 to = player_.position - e.pos;
        const float d = std::sqrt(to.x * to.x + to.z * to.z);
        if (e.hiddenT < 1.5f || e.grenadeCool > 0 || rpGrenadeCool_ > 0 || d < 8.0f || d > 26.0f) continue;
        // From over his head and a little toward you: inside his own body it would burst on him at once.
        const Vec3 from = e.pos + Vec3{to.x / d * 0.9f, 2.2f, to.z / d * 0.9f};
        const Vec3 aim = player_.position + Vec3{(frand() - 0.5f) * 2.0f, 0.2f, (frand() - 0.5f) * 2.0f};
        const float t = std::max(0.9f, std::min(1.8f, d / 13.0f));
        Vec3 v = (aim - from) * (1.0f / t);
        v.y += 0.5f * 9.8f * t;
        rockets_.push_back({from, v, 0.0f, 6});
        enemies_.playAction(int(&e - enemies_.enemies.data()), EnemyClip::Throw);  // (rework: rpman_grenade)
        e.grenadeCool = 9.0f;
        rpGrenadeCool_ = 3.5f;
        e.hiddenT = 0;
        sfx("me_grnth", from, 1.0f, 0.8f);
        LOGI("Reaper %u lobs a grenade (%.0f m, you were out of sight)", e.uid, d);
    }
}

// Near misses (rework): a bolt of yours flying close past a Reaper (within 1.3 m of his body, not
// hitting him) makes him flinch - often a dive aside - and know where you are. Each at most every 2 s.
void GameApp::updateNearMisses(float dt) {
    for (auto& e : enemies_.enemies) e.flinchCool = std::max(0.0f, e.flinchCool - dt);
    if (!playMode_ || hero_ == Hero::Kabuto) return;
    for (const auto& p : weapon_.projectiles()) {
        for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
            Enemy& e = enemies_.enemies[i];
            if (e.state == EnemyState::Dead || e.state == EnemyState::Falling || e.posted || e.flinchCool > 0) continue;
            const Vec3 c = e.pos + Vec3{0, 1.1f, 0};
            const Vec3 d = c - p.pos;
            const float dist = length(d);
            if (dist > 1.3f || dist < 0.45f || dot(d, p.vel) > 0.0f) continue;  // (just past him, not into him)
            e.flinchCool = 2.0f;
            enemies_.alertNear(e.pos, 1.0f);
            if (frand() < 0.6f) {
                Vec3 side = normalize(Vec3{-p.vel.z, 0, p.vel.x});
                if (dot(side, d) < 0) side = side * -1.0f;  // away from the bolt's line
                enemies_.push(int(i), side * 6.0f);
            }
            if (frand() < 0.4f) sfx("rpman_hit1", c, 0.5f, 1.25f);
            static const bool trace = std::getenv("GVR_HIT_TRACE") != nullptr;
            if (trace) LOGI("near miss: Reaper %u flinches (bolt %.1f m off)", e.uid, dist);
        }
    }
}

// Waving (rework): raise a hand above your head and wave it side to side (three turns within a
// couple of seconds) - the Smarties around wave back, one of them calling out. The Meccaryn and Delphi.
void GameApp::updateWaveGesture(float dt) {
    waveCool_ = std::max(0.0f, waveCool_ - dt);
    if (!playMode_ || !firstPerson_ || cutsceneActive() || hero_ == Hero::Kabuto || !lastInput_.headValid) {
        waveValid_[0] = waveValid_[1] = false;
        return;
    }
    const Pose head = rig_.toWorld(lastInput_.head);
    const Vec3 f = head.orientation.rotate(Vec3{0, 0, -1});
    Vec3 right{-f.z, 0, f.x};
    right = length(right) > 1e-3f ? normalize(right) : Vec3{1, 0, 0};
    const float S = rig_.comfort.worldScale;
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = lastInput_.hand[h];
        if (!c.gripPoseValid || c.trigger > 0.5f || c.squeeze > 0.5f) { waveValid_[h] = false; waveFlips_[h] = 0; continue; }
        const Vec3 p = rig_.toWorld(c.gripPose).position;
        const float x = dot(p - head.position, right) / S;
        const bool raised = p.y > head.position.y - 0.15f * S;
        waveFlipT_[h] += dt;
        if (waveValid_[h] && raised && dt > 0) {
            const float v = (x - waveLastX_[h]) / dt;
            const int sgn = v > 0.45f ? 1 : (v < -0.45f ? -1 : 0);
            if (sgn != 0 && sgn != waveSign_[h]) {
                waveFlips_[h] = waveFlipT_[h] < 0.8f ? waveFlips_[h] + 1 : 1;
                waveSign_[h] = sgn;
                waveFlipT_[h] = 0;
            }
        } else if (!raised) {
            waveFlips_[h] = 0;
        }
        waveLastX_[h] = x;
        waveValid_[h] = true;
        if (waveFlips_[h] >= 4 && waveCool_ <= 0) {
            waveFlips_[h] = 0;
            waveCool_ = 4.0f;
            const int n = villagers_.waveBack(player_.position, 30.0f);
            LOGI("Wave gesture (hand %d): %d Smarties wave back", h, n);
            if (n > 0) {
                // The nearest of them calls out.
                float best = 1e9f;
                Vec3 at = player_.position;
                for (const auto& v : villagers_.villagers)
                    if (v.state == VillagerState::Wave && length(v.pos - player_.position) < best) { best = length(v.pos - player_.position); at = v.pos; }
                static const int voices[] = {1, 2, 3, 4, 13, 14};
                char nm[16];
                std::snprintf(nm, sizeof nm, "gsm%02d_%d", voices[int(frand() * 5.99f)], frand() < 0.5f ? 1 : 2);
                sfx(nm, at + Vec3{0, 0.8f, 0}, 1.0f, 1.0f + 0.15f * frand());
                haptics_.push_back({h, 0.25f, 0.06f});
            }
        }
    }
}

// Touch (rework): a hand pushed into scenery or the ground buzzes - a tap on first contact, a light
// hum while it stays in. First person, in play; not for Kabuto's fists (they smash instead).
void GameApp::updateHandTouch(float dt) {
    if (!playMode_ || !firstPerson_ || !assetWorld_ || cutsceneActive() || hero_ == Hero::Kabuto) { touch_[0] = touch_[1] = false; return; }
    const float S = rig_.comfort.worldScale;
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = lastInput_.hand[h];
        touchT_[h] = std::max(0.0f, touchT_[h] - dt);
        if (!c.gripPoseValid) { touch_[h] = false; continue; }
        const Vec3 p = rig_.toWorld(c.gripPose).position;
        bool in = false;
        float gy;
        if (assetWorld_->terrain().heightAt(p.x, p.z, gy) && p.y < gy + 0.02f * S) in = true;
        if (!in) {
            Vec3 f{p.x, p.y - 0.05f * S, p.z};
            const Vec3 f0 = f;
            in = assetWorld_->collision().pushOut(f, 0.04f * S, 0.1f * S, 0.0f) && length(f - f0) > 0.01f * S;
        }
        static const bool trace = std::getenv("GVR_TOUCH_TRACE") != nullptr;
        if (trace && in != touch_[h]) LOGI("touch: hand %d %s at (%.2f %.2f %.2f)", h, in ? "in" : "out", p.x, p.y, p.z);
        if (in && !touch_[h]) haptics_.push_back({h, 0.35f, 0.04f});
        else if (in && touchT_[h] <= 0) { haptics_.push_back({h, 0.12f, 0.05f}); touchT_[h] = 0.1f; }
        touch_[h] = in;
    }
}

// A hand within `r` (human-scale metres) of a point: reach out to take a pickup (rework).
bool GameApp::handReach(const Vec3& p, float r) const {
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = lastInput_.hand[h];
        if (c.gripPoseValid && length(rig_.toWorld(c.gripPose).position - p) < r * gameScale()) return true;  // (KABUTO VIEW: LOWER reaches as far)
    }
    return false;
}

// Ambushes (rework; ours - against long empty walks): when nothing has fought the player for a
// while (70-110 s) in a mission with Reapers in it, a squad of 2-3 comes at him from ahead, 40-55 m
// out on dry ground. Not in base missions (they have raids), races, multiplayer or as Kabuto.
void GameApp::updateAmbush(float dt) {
    // Hover packs: flame and smoke under a Reaper coming down (and their roar).
    for (const auto& e : enemies_.enemies)
        if (e.dropping && e.state != EnemyState::Dead) {
            const Vec3 at = e.pos + Vec3{0, 0.9f, 0} + Quat::fromAxisAngle({0, 1, 0}, e.yaw).rotate(Vec3{0, 0, 0.3f});
            smokePuff(FxTex::Fire, at, Vec3{0, -6.0f, 0}, 0.25f, 0.6f, 0.25f, 0.9f, 0.0f);
            if (frand() < 0.3f) smokePuff(FxTex::SmokeGrey, at + Vec3{0, -0.8f, 0}, Vec3{0, -2.0f, 0}, 0.4f, 1.3f, 0.8f, 0.22f, 0.3f);
            if (int(time_ * 4) != int((time_ - dt) * 4)) sfx("me_jpkth", e.pos, 0.5f, 0.8f);  // (was me_jtlp1: not in the data; the jetpack thrust)
        }
    if (!playMode_ || dying_ || cutsceneActive() || hero_ == Hero::Kabuto || skirmish_ || base_.on || riding_ || enemyPool_.empty() ||
        !assetWorld_ || load_.phase != LoadPhase::Idle || std::getenv("GVR_NO_AMBUSH") || !settings_.ambushes)
        return;
    // Only where the mission has Reapers of its own (Story 1's opening has none: no ambush there).
    if (ambushMission_ != assetWorld_->missionName()) {
        ambushMission_ = assetWorld_->missionName();
        ambushAllowed_ = !enemies_.enemies.empty();
        ambushT_ = ambushDue_ = 0;
    }
    if (!ambushAllowed_) return;
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
    // Half the squads come down from the sky on hover packs (rpman_Hover), closer in (GVR_AMBUSH_AIR=1/0 forces).
    const char* airEnv = std::getenv("GVR_AMBUSH_AIR");
    const bool air = airEnv ? airEnv[0] == '1' : frand() < 0.5f;
    for (int attempt = 0; attempt < 24 && placed < n; ++attempt) {
        const float a = yaw + (frand() - 0.5f) * (air ? 1.4f : 2.1f);
        const float r = air ? 22.0f + 10.0f * frand() : 40.0f + 15.0f * frand();
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
        if (air) {
            e.dropping = true;
            e.pos.y = h + 18.0f + 6.0f * frand();
        }
        ++placed;
    }
    if (placed) {
        timmyLine_ = air ? "REAPERS - ABOVE YOU!" : "REAPERS AHEAD!";
        timmyLineT_ = 2.5f;
        sfx2D("rs_attk2", 0.6f, 0.9f);
        LOGI("Ambush: %d Reapers %s", placed, air ? "dropping from the sky" : "ahead of the player");
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

// A marksman's aim (rework): a thin red line from his gun to you while his shot charges - brighter
// as it comes. (The glow at the muzzle is added with the shots, game_app.cpp.)
void GameApp::renderMarksmanLasers() {
    if (!playMode_ || hero_ == Hero::Kabuto) return;
    if (!laserMesh_) laserMesh_ = renderer_.uploadMesh(prim::box({0.5f, 0.5f, 0.5f}, std::array<uint8_t, 4>{255, 255, 255, 255}.data()));
    const Vec3 target = player_.position + Vec3{0, 0.75f * playerHitTop(), 0};
    for (const auto& e : enemies_.enemies) {
        if (e.role != 1 || e.state != EnemyState::Fire || e.burstLeft <= 0 || e.fireCooldown >= 0.7f || e.weapon == 1) continue;
        const Quat r0 = Quat::fromAxisAngle({0, 1, 0}, e.yaw);
        Vec3 m = e.pos + Vec3{0, 1.5f, 0} + r0.rotate(Vec3{0.3f, 0, -0.6f});
        reaperMuzzle(e, m);
        const Vec3 d = target - m;
        const float len = length(d);
        if (len < 0.5f) continue;
        const float k = 1.0f - std::max(0.0f, e.fireCooldown) / 0.7f;
        static const bool trace = std::getenv("GVR_MK_TRACE") != nullptr;
        if (trace) LOGI("marksman aims: Reaper %u, %.1f m, charge %.2f, t %.2f", e.uid, len, k, time_);
        DrawParams dp;
        dp.unlit = true;
        dp.blend = BlendMode::Additive;
        dp.depthWrite = false;
        dp.noFog = true;
        dp.tint = {1.2f * (0.3f + 0.7f * k), 0.08f, 0.05f, 1.0f};
        const Quat r = rotationBetween({0, 0, -1}, d * (1.0f / len));
        const float w = 0.006f + 0.006f * k;
        renderer_.draw(laserMesh_, Mat4::translation(m + d * 0.5f) * Mat4::rotation(r) * Mat4::scale({w, w, len}), dp);
    }
}

void GameApp::updateMotes(float dt) {
    if (!lastInput_.headValid || !assetWorld_ || std::getenv("GVR_NO_MOTES")) { motes_.clear(); return; }
    const Vec3 c = rig_.toWorld(lastInput_.head).position;
    const float R = 9.0f * rig_.comfort.worldScale;
    if (motes_.empty()) {
        motes_.resize(90);
        for (auto& m : motes_) {
            m.pos = c + Vec3{(frand() - 0.5f) * 2.0f * R, (frand() - 0.4f) * R, (frand() - 0.5f) * 2.0f * R};
            m.vel = Vec3{frand() - 0.5f, (frand() - 0.5f) * 0.3f, frand() - 0.5f} * 0.25f;
            m.size = 0.03f + 0.035f * frand();
            m.phase = frand() * 6.28f;
        }
    }
    const float t = float(time_);
    for (auto& m : motes_) {
        m.pos = m.pos + (m.vel + Vec3{0.12f * std::sin(t * 0.3f + m.phase), 0.05f * std::sin(t * 0.7f + m.phase * 2.0f), 0.08f}) * dt;
        const Vec3 d = m.pos - c;
        if (std::fabs(d.x) > R || std::fabs(d.z) > R || d.y > 0.7f * R || d.y < -0.5f * R) {
            // Back in on the far side of the cube, somewhere new.
            m.pos = c + Vec3{d.x > R ? -R : (d.x < -R ? R : (frand() - 0.5f) * 2.0f * R), (frand() - 0.4f) * R,
                             d.z > R ? -R : (d.z < -R ? R : (frand() - 0.5f) * 2.0f * R)};
            float gy;
            if (assetWorld_->terrain().heightAt(m.pos.x, m.pos.z, gy) && m.pos.y < gy + 0.3f) m.pos.y = gy + 0.3f + frand() * 2.0f;
        }
    }
}

void GameApp::renderMotes() {
    if (motes_.empty() || renderer_.inReflectionPass()) return;
    if (!moteTex_) {
        moteTex_ = glowTexture();
        moteBatch_.init(renderer_, 128);
    }
    const EnvironmentParams& env = renderer_.environment();
    const float night = env.night;
    const Quat q = headWorldRot_;
    const Vec3 right = q.rotate({1, 0, 0}), up = q.rotate({0, 1, 0});
    moteBatch_.clear();
    const float t = float(time_);
    for (const auto& m : motes_) {
        // Day: faint warm dust; night: fireflies that pulse.
        const float pulse = night > 0.3f ? 0.4f + 0.6f * std::max(0.0f, std::sin(t * 2.3f + m.phase * 3.0f)) : 1.0f;
        const Vec3 col = night > 0.3f ? Vec3{0.6f, 1.0f, 0.35f} * (1.4f * pulse) : Vec3{1.0f, 0.88f, 0.66f} * 0.8f;
        const uint8_t rgba[4] = {uint8_t(std::min(255.0f, col.x * 255.0f)), uint8_t(std::min(255.0f, col.y * 255.0f)),
                                 uint8_t(std::min(255.0f, col.z * 255.0f)), 255};
        moteBatch_.addBillboard(m.pos, m.size * rig_.comfort.worldScale * (night > 0.3f ? 1.3f : 1.0f), right, up, 0.0f, rgba);
    }
    moteBatch_.upload(renderer_);
    DrawParams dp;
    dp.texture = moteTex_;
    dp.unlit = true;
    dp.blend = BlendMode::Additive;
    dp.depthWrite = false;
    dp.doubleSided = true;
    moteBatch_.draw(renderer_, dp);
}

void GameApp::updateCreatureFlashes(float dt) {
    for (auto& c : chars_) c.flash = std::max(0.0f, c.flash - dt * 6.0f);
    const auto& cs = creatures_.creatures;
    if (creatureHp_.size() != cs.size()) creatureHp_.assign(cs.size(), -1.0f);
    for (size_t i = 0; i < cs.size(); ++i) {
        if (creatureHp_[i] >= 0 && cs[i].health < creatureHp_[i] - 0.01f && cs[i].slot >= 0 && size_t(cs[i].slot) < chars_.size())
            chars_[size_t(cs[i].slot)].flash = 1.0f;
        creatureHp_[i] = cs[i].health;
    }
}

TextureId GameApp::glowTexture() {
    static TextureId tex = 0;
    if (tex) return tex;
    std::vector<uint8_t> px(32 * 32 * 4);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float dx = (float(x) + 0.5f) / 16.0f - 1.0f, dy = (float(y) + 0.5f) / 16.0f - 1.0f;
            const float a = std::max(0.0f, 1.0f - std::sqrt(dx * dx + dy * dy));
            uint8_t* p = &px[size_t(y * 32 + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = uint8_t(255.0f * a * a);
        }
    tex = renderer_.uploadTexture(32, 32, px.data(), true, false);
    return tex;
}

void GameApp::addFireball(const Vec3& at, float radius) {
    if (fireballs_.size() < 12) fireballs_.push_back({at, radius, 0.0f});
    const int n = std::min(24, int(6 + radius * 3.0f));
    for (int i = 0; i < n && embers_.size() < 160; ++i) {
        const Vec3 d = normalize(Vec3{frand() - 0.5f, 0.3f + frand(), frand() - 0.5f});
        embers_.push_back({at, d * (4.0f + 8.0f * frand()) * std::min(2.0f, 0.4f + radius * 0.25f), 0.0f, 0.6f + 0.8f * frand(), 0.05f + 0.07f * frand()});
    }
}

// Sparks (rework): a shot striking rock or metal throws a few bright, quick sparks off the surface.
void GameApp::addSparks(const Vec3& at, const Vec3& normal, int n) {
    const Vec3 nn = length(normal) > 1e-4f ? normalize(normal) : Vec3{0, 1, 0};
    if (embers_.size() < 220) embers_.push_back({at + nn * 0.06f, Vec3{0, 0, 0}, 0.0f, 0.12f, 0.45f});  // the flash: a soft glow
    for (int i = 0; i < n && embers_.size() < 220; ++i) {
        Vec3 d = normalize(nn * 1.2f + Vec3{frand() - 0.5f, frand() - 0.3f, frand() - 0.5f} * 1.6f);
        embers_.push_back({at + nn * 0.03f, d * (3.0f + 6.0f * frand()), 0.0f, 0.18f + 0.3f * frand(), 0.025f + 0.025f * frand()});
    }
}

void GameApp::updateFireballs(float dt) {
    for (auto& f : fireballs_) f.t += dt;
    fireballs_.erase(std::remove_if(fireballs_.begin(), fireballs_.end(), [](const Fireball& f) { return f.t > 0.55f; }), fireballs_.end());
    for (auto& e : embers_) {
        e.t += dt;
        e.vel.y -= 9.8f * dt;
        e.vel = e.vel * (1.0f - 0.8f * dt);
        e.pos = e.pos + e.vel * dt;
    }
    embers_.erase(std::remove_if(embers_.begin(), embers_.end(), [](const Ember& e) { return e.t > e.life; }), embers_.end());
}

void GameApp::renderFireballs() {
    if ((fireballs_.empty() && embers_.empty()) || renderer_.inReflectionPass()) return;
    static bool init = false;
    if (!init) { init = true; fireBatch_.init(renderer_, 260); }
    const Vec3 right = headWorldRot_.rotate({1, 0, 0}), up = headWorldRot_.rotate({0, 1, 0});
    fireBatch_.clear();
    for (const auto& f : fireballs_) {
        const float k = f.t / 0.55f;
        const float size = f.radius * (0.6f + 1.4f * std::sqrt(k)) * 1.6f;
        float a = (1.0f - k);
        // Never a wall of fire over the eyes: fade one whose billboard would surround the viewer.
        const float eyeD = length(f.pos - renderer_.currentView().eyePosition);
        a *= std::min(1.0f, std::max(0.0f, (eyeD - 0.5f * size) / (0.7f * size)));
        if (a <= 0.01f) continue;
        // A white-hot core turning orange, then dark red as it fades.
        const Vec3 col = Vec3{1.0f, 0.75f - 0.45f * k, 0.45f - 0.4f * k} * (a * 1.0f);
        const uint8_t rgba[4] = {uint8_t(255 * col.x), uint8_t(255 * std::max(0.0f, col.y)), uint8_t(255 * std::max(0.0f, col.z)), 255};
        fireBatch_.addBillboard(f.pos, size, right, up, f.t * 2.0f, rgba);
        if (fxCostView_) fxCover_[kCostFireball] += coverOf(size, eyeD) * (k < 0.4f ? 1.25f : 1.0f);
        const uint8_t core[4] = {uint8_t(255 * a), uint8_t(240 * a), uint8_t(200 * a), 255};
        if (k < 0.4f) fireBatch_.addBillboard(f.pos, size * 0.5f, right, up, 0.0f, core);
    }
    for (const auto& e : embers_) {
        const float a = 1.0f - e.t / e.life;
        const uint8_t rgba[4] = {255, uint8_t(160 * a + 60), uint8_t(60 * a), 255};
        const uint8_t dim[4] = {uint8_t(rgba[0] * a), uint8_t(rgba[1] * a), uint8_t(rgba[2] * a), 255};
        fireBatch_.addBillboard(e.pos, e.size * (0.6f + 0.4f * a), right, up, 0.0f, dim);
        if (fxCostView_) fxCover_[kCostEmbers] += coverOf(e.size * (0.6f + 0.4f * a), length(e.pos - renderer_.currentView().eyePosition));
    }
    fireBatch_.upload(renderer_);
    DrawParams dp;
    dp.texture = glowTexture();
    dp.unlit = true;
    dp.blend = BlendMode::Additive;
    dp.depthWrite = false;
    dp.doubleSided = true;
    fireBatch_.draw(renderer_, dp);
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
    // Zones follow the body: the head's position, and a body yaw that turns with the head only past
    // 35 degrees either way (a neck: looking around doesn't swing your hips).
    const float headYaw = yawOf(in.head.orientation);
    float dy = headYaw - holsterYaw_;
    while (dy > kPi) dy -= 2 * kPi;
    while (dy < -kPi) dy += 2 * kPi;
    if (dy > 0.6f) holsterYaw_ += dy - 0.6f;
    else if (dy < -0.6f) holsterYaw_ += dy + 0.6f;
    const float yaw = holsterYaw_;
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
        {
            const float dmg = std::min(60.0f, 18.0f + (speed - 2.6f) * 12.0f);
            if (skirmishHitWhere(dmg, p - v * 0.05f, [&](const SkPlayer& sp) {
                    const float dy = p.y - sp.body.position.y;
                    return dy > 0.3f && dy < std::max(2.1f, sp.body.height) &&
                           length(Vec3{p.x - sp.body.position.x, 0, p.z - sp.body.position.z}) < 0.25f + sp.body.radius;
                }) > 0) {
                haptics_.push_back({h, 1.0f, 0.09f});
                punchCool_[h] = 0.35f;
                LOGI("Punch: a skirmish player hit (%.1f m/s)", speed);
                continue;
            }
        }
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
