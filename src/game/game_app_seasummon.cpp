// (2026-10-29) Delphi's SUMMON SEA MONSTER: weapondefs item 32 (IconRSSeaMonster, GText "Summon Sea Monster"); in the
// original the Reapers' base builds it as its special ("Sea Monster Spell"), and the serpent's weapon is 48
// (fx_seamonster_fireball, 150 damage, a wide burst) - "%v was blasted by %k's sea monster!", and "%v was killed by her
// own monster!". The serpent itself is the data's RM_seamonster (game_app_seamonster.cpp). Rules ours:
// - Cast: the casting hand points at deep water (over 5 m deep, 12-70 m out) with the trigger held, steadily for
//   0.4 s; then it is lifted sharply from below the chest to above the head - raising it from the sea.
// - It surfaces where she pointed, comes in toward the nearest Reapers and Rippers by the water (never into
//   water under 6 m deep), rears and spits a fireball at one five times, then dives. Not at enemies within 9 m of
//   her - but a burst is a burst. 45 s between. Her battles (where the tornado is hers) and practice.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/game_app.h"
#include "scene/asset_world.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr float kSeaCool = 45.0f;
constexpr float kSeaReach = 70.0f;  // m from the serpent it spits at
}  // namespace

bool GameApp::seaSummonOwned() const {
    if (hero_ != Hero::Delphi || !assetWorld_ || !seaMonModel_) return false;
    return practice_ || tornadoOwned() || std::getenv("GVR_SEA_SUMMON") != nullptr;  // (test hook)
}

// The nearest Reaper or Ripper above water within its reach, not right by Delphi.
bool GameApp::seaMonsterTarget(Vec3& at) const {
    const Vec3 m = seaMon_.pos;
    float best = kSeaReach;
    bool got = false;
    auto consider = [&](const Vec3& p) {
        const float d = length(Vec3{p.x - m.x, 0, p.z - m.z});
        if (d >= best || length(Vec3{p.x - player_.position.x, 0, p.z - player_.position.z}) < 9.0f) return;
        best = d;
        at = p;
        got = true;
    };
    for (const auto& e : enemies_.enemies)
        if (e.state != EnemyState::Dead && !e.remove) consider(e.pos + Vec3{0, 0.5f, 0});
    for (const auto& c : creatures_.creatures)
        if (c.state != CreatureState::Dead && !c.remove && !CreatureDirector::underground(c)) consider(c.pos + Vec3{0, 0.3f, 0});
    return got;
}

void GameApp::summonSeaMonster(const Vec3& at) {
    SeaMonster& s = seaMon_;
    // Heading in: toward the nearest enemy near the water, else toward her.
    Vec3 toward = player_.position;
    float best = 1e9f;
    for (const auto& e : enemies_.enemies)
        if (e.state != EnemyState::Dead && length(e.pos - at) < best) { best = length(e.pos - at); toward = e.pos; }
    Vec3 dir = toward - at;
    dir.y = 0;
    dir = length(dir) > 1.0f ? normalize(dir) : Vec3{0, 0, -1};
    s.summoned = true;
    s.shots = 0;
    s.idleT = 0;
    s.fired = false;
    s.goal = toward;
    s.dir = dir;
    s.yaw = std::atan2(-dir.x, -dir.z);
    s.pos = Vec3{at.x, player_.seaLevel - 10.0f, at.z};
    s.phase = 1;  // (surfacing - game_app_seamonster.cpp's kSurface)
    s.phaseT = 0;
    s.splashes = 0;
    waterSplash(Vec3{at.x, player_.seaLevel, at.z}, 3.5f, 0);
    const float d = std::max(2.0f, length(at - player_.position));
    sfx("se_first", at + Vec3{0, 6.0f, 0}, std::min(30.0f, d / 6.0f), 1.0f);
    LOGI("Delphi summons the SEA MONSTER %.0f m out", double(length(at - player_.position)));
}

void GameApp::updateSeaSummon(float dt, const InputFrame& in) {
    seaCool_ = std::max(0.0f, seaCool_ - dt);
    const int mh = rig_.moveHand();
    const ControllerState& c = in.hand[mh];
    const bool held = c.trigger > 0.4f;
    auto reset = [&]() { seaAimT_ = seaLift_ = 0; seaArmLow_ = false; seaHandValid_ = false; };
    if (!seaSummonOwned() || !firstPerson_ || !held || !c.aimPoseValid || !c.gripPoseValid || !in.headValid || delphiBow() ||
        spellGestureCast_ || seaCool_ > 0 || followUid_ || player_.seaLevel < -1e8f || (seaMon_.summoned && seaMon_.phase != 0)) {
        reset();
        return;
    }
    static const bool trace = std::getenv("GVR_SPELL_TRACE") != nullptr;
    if (trace && int(time_ * 2) != int((time_ - dt) * 2)) {
        // Where deep water is from here (for a test's aim): the nearest bearing with water over 5 m deep 20-50 m out.
        for (int a = 0; a < 36; ++a) {
            const float yaw = float(a) * 10.0f * kPi / 180.0f;
            const Vec3 d{-std::sin(yaw), 0, -std::cos(yaw)};
            int deep = 0;
            for (float r = 20.0f; r <= 50.0f; r += 10.0f) {
                float g;
                const Vec3 p = player_.position + d * r;
                deep += !assetWorld_->terrain().heightAt(p.x, p.z, g) || g < player_.seaLevel - 5.0f;
            }
            if (deep == 4) {
                const Vec3 p = player_.position + d * 35.0f;
                LOGI("spell trace: SEA MONSTER deep water at yaw %d deg, 35 m out at game (%.0f %.0f)", a * 10, double(-p.x / 0.2f), double(p.z / 0.2f));
                break;
            }
        }
    }
    // The hand's height against the head (tracking space: ducking isn't lifting).
    const float rel = c.gripPose.position.y - in.head.position.y;
    const float vy = seaHandValid_ && dt > 0 ? (rel - seaHandPrevY_) / dt : 0.0f;
    seaHandPrevY_ = rel;
    seaHandValid_ = true;
    // Aimed at deep water? (before the lift: once lifting, the aim sweeps up off the sea)
    if (seaLift_ <= 0) {
        const Pose aim = rig_.toWorld(c.aimPose);
        const Vec3 d = aim.orientation.rotate(Vec3{0, 0, -1});
        const float sea = player_.seaLevel;
        bool onSea = false;
        Vec3 p;
        if (d.y < -0.03f && aim.position.y > sea) {
            const float t = (aim.position.y - sea) / -d.y;
            p = aim.position + d * t;
            float g = sea;
            const bool deep = !assetWorld_->terrain().heightAt(p.x, p.z, g) || g < sea - 5.0f;
            float th;
            Vec3 n;
            const bool blocked = worldRay_ && worldRay_(aim.position, d, t - 0.5f, th, n);
            const float flat = length(Vec3{p.x - player_.position.x, 0, p.z - player_.position.z});
            onSea = deep && !blocked && flat > 12.0f && flat < 70.0f;
            if (trace && int(time_ * 4) != int((time_ - dt) * 4))
                LOGI("spell trace: SEA MONSTER aim %.0f m out (yaw %.0f deg), %.1f m deep%s, hand %.2f m under the head", double(flat),
                     double(std::atan2(-d.x, -d.z) * 57.3f), double(sea - g), blocked ? ", blocked" : "", double(-rel));
        } else if (trace && int(time_ * 4) != int((time_ - dt) * 4)) {
            LOGI("spell trace: SEA MONSTER aim not down at the sea (dir y %.2f)", double(d.y));
        }
        if (onSea && (seaAimT_ <= 0 || length(p - seaAimP_) < 5.0f)) {
            if (seaAimT_ <= 0) seaAimP_ = p;
            seaAimP_ = seaAimP_ * 0.9f + p * 0.1f;
            seaAimT_ += dt;
            seaArmLow_ = rel < -0.25f;  // (the hand down, pointing out to sea)
        } else {
            seaAimT_ = 0;
        }
    }
    // Then the lift: fast, from below the chest, over 0.2 m and more.
    if (trace && vy > 1.0f) LOGI("spell trace: SEA MONSTER hand rising %.1f m/s (aimed %.2f s, low %d, lifted %.2f m)", double(vy), double(seaAimT_), int(seaArmLow_), double(seaLift_));
    if (seaAimT_ >= 0.4f && seaArmLow_ && vy > 1.4f) seaLift_ += vy * dt;
    else if (vy < 0.4f && seaLift_ > 0) { if (trace) LOGI("spell trace: SEA MONSTER lift stopped at %.2f m", double(seaLift_)); seaLift_ = 0; }
    if (seaLift_ > 0.2f && rel > -0.05f) {
        spellGestureCast_ = true;  // (the trigger's release is not a fire bolt)
        spellCharge_ = 0;
        seaCool_ = kSeaCool;
        endCloak("she cast");
        summonSeaMonster(seaAimP_);
        haptics_.push_back({mh, 1.0f, 0.3f});
        reset();
    }
}

}  // namespace gvr
