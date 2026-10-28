// Delphi's spells beyond her fire bolt, from the Reaper story's briefings: the FIRE WALL ("useful for
// setting groups of Reaper Guards aflame", r1_saveyan), the BUBBLE SPELL ("capture Smarties from a
// distance", r1_turbo) and the TORNADO ("the enemy ARMY BARRACKS can only be destroyed by a TORNADO
// SPELL", the base battles). The visuals are the original effect models (EFX_fire_roll, EFX_swirl,
// EFX_bubble_reflection) and icons (st_rp_firewall / cluster / tornado); how the spells behave in
// the original is native code, so the rules are ours.
// VR controls (ours): the move-hand trigger. A tap casts the fire bolt - or, aimed at a Smartie, a
// bubble that brings it to her. Held and released after 0.7 s: a fire wall; after 1.6 s (in the
// battles, where the tornado is hers): a tornado. A glow in that hand shows the charge.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "game/destruction.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr float kWallCharge = 0.7f, kTornadoCharge = 1.6f;
constexpr int kVimpTargetS = 4000;      // as game_app.cpp's kVimpTarget
constexpr int kCreatureTargetS = 1000;  // as game_app.cpp's kCreatureTarget
}  // namespace

bool GameApp::tornadoOwned() const {
    if (hero_ != Hero::Delphi || !assetWorld_) return false;
    // Her Smarties make it once she has a base: the battles (r4a, rstory2_battle, rstory5_battle) and on.
    const std::string m = toLower(assetWorld_->missionName());
    // r4a builds the base first: "Your Smarties will create the TORNADO SPELL for you once your BASE is complete".
    if (base_.on) return base_.complete;  // r4a and the Reaper battles that build a base
    return m.rfind("r4", 0) == 0 || m.find("battle") != std::string::npos || m.rfind("rstory5", 0) == 0;
}

void GameApp::initSpells() {
    if (spellReady_ || !assetWorld_) return;
    ModelCache* mc = assetWorld_->models();
    fireRoll_ = mc->get("EFX_fire_roll.gbs", "", false);
    swirl_ = mc->get("EFX_swirl.gbs", "", false);
    auto b = std::make_unique<SkinnedModel>();
    if (b->load(assetWorld_->fs(), *mc, renderer_, "EFX_bubble_reflection", "EFX_bubble_reflection.anm")) bubbleModel_ = std::move(b);
    hailModel_ = mc->get("EFX_hail_missile.gbs", "", false);
    slowModel_ = mc->get("EFX_slow_spell.gbs", "", false);
    circleModel_ = mc->get("EFX_circle_spell.gbs", "", false);
    spellReady_ = true;
    LOGI("Spells: fire roll %d, swirl %d, bubble %d, hail %d, slow %d", fireRoll_ != nullptr, swirl_ != nullptr, bubbleModel_ != nullptr,
         hailModel_ != nullptr, slowModel_ != nullptr);
    if (hailModel_) {
        const Vec3 e = hailModel_->boundsMax - hailModel_->boundsMin;
        LOGI("Spells: hail stone %.2f x %.2f x %.2f m", e.x, e.y, e.z);
    }
}

bool GameApp::smartieInAim(const Vec3& from, const Vec3& dir, int& index) const {
    float best = 0.97f;
    index = -1;
    for (size_t i = 0; i < rescue_.smarties.size(); ++i) {
        const RescueSmartie& s = rescue_.smarties[i];
        if (s.state != SmartieState::Idle && s.state != SmartieState::Hanging) continue;
        const Vec3 to = s.pos + Vec3{0, 0.6f, 0} - from;
        const float d = length(to);
        if (d > 45.0f || d < 1.5f) continue;
        const float c = dot(to * (1.0f / d), dir);
        if (c > best) { best = c; index = int(i); }
    }
    return index >= 0;
}

// (Ours) Hail from the third Reaper chapter on, Slow Time from the fourth; both in the battles and in practice.
bool GameApp::hailOwned() const {
    if (hero_ != Hero::Delphi || !assetWorld_) return false;
    if (practice_ || tornadoOwned()) return true;
    const std::string w = toLower(worldName_);
    return w == "rstory3" || w == "rstory4" || w == "rstory5";
}

bool GameApp::slowOwned() const {
    if (hero_ != Hero::Delphi || !assetWorld_) return false;
    if (practice_ || tornadoOwned()) return true;
    const std::string w = toLower(worldName_);
    return w == "rstory4" || w == "rstory5";
}

void GameApp::castHail(const Vec3& from, const Vec3& dir) {
    endCloak("she cast");  // (an attack gives her away)
    // Where she aims: the first ground along the aim within 60 m, else 40 m out.
    Vec3 at = from + dir * 40.0f;
    for (float t = 2.0f; t <= 60.0f; t += 1.0f) {
        const Vec3 p = from + dir * t;
        const float g = enemies_.groundHeight ? enemies_.groundHeight(p.x, p.z) : 0.0f;
        if (p.y <= std::max(g, player_.seaLevel)) { at = p; break; }
    }
    {   // At least 7 m out: not on her own head (nor on the Smarties at her heels).
        Vec3 flatTo{at.x - player_.position.x, 0, at.z - player_.position.z};
        const float d = length(flatTo);
        if (d < 7.0f) at = player_.position + (d > 0.1f ? flatTo * (1.0f / d) : Vec3{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)}) * 7.0f;
    }
    if (enemies_.groundHeight) at.y = std::max(player_.seaLevel, enemies_.groundHeight(at.x, at.z));
    HailStorm s;
    s.centre = at;
    s.left = 16;
    hailStorms_.push_back(s);
    hailCool_ = 10.0f;
    sfx("sr_spfsh", from, 1.0f, 0.5f);
    addFlash(from + Vec3{0, 0.5f, 0}, {0.8f, 1.1f, 1.6f}, 4.0f, 0.3f);
    LOGI("Delphi casts SUMMON HAIL at (%.1f %.1f %.1f), %.0f m away", at.x, at.y, at.z, length(at - player_.position));
}

bool GameApp::cloakOwned() const {
    if (hero_ != Hero::Delphi || !assetWorld_) return false;
    const std::string w = toLower(worldName_);
    return practice_ || w == "rstory4" || w == "rstory5";
}

void GameApp::castCloak() {
    cloakT_ = 12.0f;
    cloakCool_ = 30.0f;
    cloakShots0_ = uint32_t(weapon_.shotsFired());
    cloakHits_ = 0;
    int fighting = 0, lost = 0;
    for (auto& e : enemies_.enemies) {  // gone from their sight: even a Reaper in the middle of a fight loses her
        if (e.state == EnemyState::Dead) continue;
        fighting += e.state == EnemyState::Advance || e.state == EnemyState::Fire;
        lost += e.aware;
        e.aware = false;
        e.alert = 0;
        e.poke = -1;
        e.lost = 0;
        e.los = false;
        if (e.state == EnemyState::Advance || e.state == EnemyState::Fire) { e.state = EnemyState::Idle; e.stateTime = 0; }
    }
    sfx2D("sr_spfsh", 0.9f, 1.6f);
    LOGI("Delphi casts CLOAK (12 s): %d Reapers lose her, %d of them in a fight", lost, fighting);
}

void GameApp::endCloak(const char* why) {
    if (cloakT_ <= 0) return;
    cloakT_ = 0;
    sfx2D("sr_spfsh", 0.6f, 0.7f);
    LOGI("Cloak ends (%s): hit %d times while cloaked", why, cloakHits_);
}

void GameApp::resetSpells() {
    circleT_ = circleCool_ = gestArmsT_ = 0;
    followUid_ = followCandUid_ = 0;
    followCool_ = followT_ = 0;
    followThrown_.clear();
    cloakT_ = cloakCool_ = clusterCool_ = gestFaceT_ = 0;
    hailStones_.clear();
    hailStorms_.clear();
    slowT_ = hailCool_ = slowCool_ = 0;
    gestHighT_ = gestTogether_ = 0;
    gestPrevValid_ = false;
    fireWalls_.clear();
    tornadoes_.clear();
    wheelOpen_ = wheelWasOpen_ = false;
    stickFwdT_ = 0;
}

namespace {
constexpr float kCircleR = 2.0f;      // m: the wall's radius at the ground (the model's foot, scaled)
constexpr float kCircleTime = 8.0f;   // s
}  // namespace

void GameApp::castFireCircle() {
    endCloak("she cast");
    circleAt_ = player_.position;
    circleT_ = kCircleTime;
    circleCool_ = 20.0f;
    circleBurnt_ = 0;
    sfx("sr_spfsh", circleAt_, 1.0f, 0.7f);
    sfx("sr_spfsh", circleAt_, 0.8f, 1.2f);
    addFlash(circleAt_ + Vec3{0, 1.0f, 0}, {0.6f, 0.9f, 2.0f}, 8.0f, 0.4f);
    LOGI("Delphi casts FIRE CIRCLE (%.0f m round, %.0f s)", double(kCircleR), double(kCircleTime));
}

void GameApp::castSlowTime() {
    slowT_ = 8.0f;
    slowCool_ = 25.0f;
    sfx2D("sr_spfsh", 1.0f, 0.4f);
    addFlash(player_.position + Vec3{0, 1.2f, 0}, {0.6f, 0.8f, 1.6f}, 10.0f, 0.5f);
    LOGI("Delphi casts SLOW TIME (8 s)");
}

void GameApp::castTornado(const Vec3& dir) {
    endCloak("she cast");  // (an attack gives her away)
    Tornado t;
    t.pos = player_.position + Vec3{dir.x, 0, dir.z} * 4.0f;
    Vec3 d{dir.x, 0, dir.z};
    t.dir = length(d) > 1e-3f ? normalize(d) : Vec3{0, 0, -1};
    t.age = 0;
    tornadoes_.push_back(t);
    sfx("sr_spfsh", t.pos, 1.0f, 0.6f);
    sfx("ex_woods", t.pos, 0.7f, 0.5f);
    LOGI("Delphi casts the TORNADO");
}

void GameApp::castFireWall(const Vec3& dir) {
    endCloak("she cast");  // (an attack gives her away)
    // A wall of rolling fire across her aim, sweeping forward.
    Vec3 d{dir.x, 0, dir.z};
    d = length(d) > 1e-3f ? normalize(d) : Vec3{0, 0, -1};
    const Vec3 side{-d.z, 0, d.x};
    FireWall w;
    w.dir = d;
    w.age = 0;
    for (int k = -3; k <= 3; ++k) w.seg.push_back(player_.position + d * 3.0f + side * (float(k) * 1.4f));
    fireWalls_.push_back(w);
    sfx("sr_spfsh", player_.position + d * 3.0f, 1.0f, 0.8f);
    LOGI("Delphi casts the FIRE WALL");
}

// Rework: spells by gesture, first person, while the casting hand's trigger is held - a circle drawn
// with that hand above your head calls the tornado (where she has it); a fast sweep of it across
// you, low, throws the fire wall. Holding the trigger to charge them still works. GVR_SPELL_TRACE.
void GameApp::updateSpellGestures(float dt, const InputFrame& in, const Vec3& dir) {
    const int mh = rig_.moveHand();
    const ControllerState& c = in.hand[mh];
    static const bool trace = std::getenv("GVR_SPELL_TRACE") != nullptr;
    // (review 2026-10-28) Not evaluated last frame (dying, riding, the menu): no motion measured across the gap.
    if (time_ - gestLastT_ > 0.1) gestPrevValid_ = false, gestHighT_ = 0, gestTogether_ = 0;
    gestLastT_ = float(time_);
    // With a bow in the casting hand (first person) its trigger lights fire arrows: no gestures there.
    if (!firstPerson_ || !c.gripPoseValid || !in.headValid || c.trigger < 0.6f || spellGestureCast_ || dt <= 0 || delphiBow()) {
        gestCircle_ = 0;
        gestSweep_ = 0;
        gestT_ = 0;
        gestHighT_ = gestTogether_ = 0;
        gestPrevValid_ = false;
        return;
    }
    const Vec3 h = c.gripPose.position, head = in.head.position;
    const Vec3 rel{h.x - head.x, 0, h.z - head.z};
    const float ang = std::atan2(rel.x, rel.z);
    gestT_ += dt;
    if (gestPrevValid_) {
        // Circling overhead: the angle round the head, summed while the hand stays above it.
        if (h.y > head.y + 0.03f && length(rel) > 0.08f) {
            float d = ang - gestPrevAng_;
            while (d > 3.14159f) d -= 6.28318f;
            while (d < -3.14159f) d += 6.28318f;
            gestCircle_ += d;
        } else {
            gestCircle_ *= 0.9f;
        }
        // Sweeping across, low (below the chin): sideways speed relative to the head's facing.
        const Vec3 f = in.head.orientation.rotate(Vec3{0, 0, -1});
        Vec3 side{-f.z, 0, f.x};
        side = length(side) > 1e-3f ? normalize(side) : Vec3{1, 0, 0};
        const float vs = dot(h - gestPrev_, side) / dt;
        if (h.y < head.y - 0.15f && std::fabs(vs) > 2.2f) gestSweep_ += std::fabs(vs) * dt;
        else gestSweep_ = std::max(0.0f, gestSweep_ - 2.0f * dt);
        // Hail: the hand raised high (in the last 0.7 s), then brought down - mostly straight down - at least 0.4 m
        // from its highest point to below the chin, fast (an overhand fire-bolt throw goes forward, not down).
        const float vy = (h.y - gestPrev_.y) / dt;
        const float drop = gestPeakY_ - h.y;
        const float across = length(Vec3{h.x - gestPeak_.x, 0, h.z - gestPeak_.z});
        if (gestHighT_ > 0 && drop > 0.4f && across < 0.6f * drop && vy < -1.5f && h.y < head.y - 0.2f && hailOwned() && hailCool_ <= 0) {
            gestHighT_ = 0;
            // Where she looks (the pitch held between 10 and 35 degrees down), not the chopping hand's aim.
            const Pose eye = rig_.toWorld(in.head);
            Vec3 g = eye.orientation.rotate(Vec3{0, 0, -1});
            Vec3 gh{g.x, 0, g.z};
            gh = length(gh) > 1e-3f ? normalize(gh) : Vec3{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)};
            const float pitch = std::max(-0.61f, std::min(-0.17f, std::asin(std::max(-1.0f, std::min(1.0f, g.y)))));
            castHail(eye.position, gh * std::cos(pitch) + Vec3{0, std::sin(pitch), 0});
            if (trace) LOGI("spell gesture: raised and brought down (%.2f m, %.1f m/s) -> hail", drop, -vy);
            spellGestureCast_ = true;
            spellCharge_ = 0;
            gestCircle_ = gestSweep_ = 0;
            gestPrev_ = h;
            haptics_.push_back({mh, 1.0f, 0.2f});
            return;
        }
    }
    if (h.y > head.y + 0.15f) {
        if (gestHighT_ <= 0 || h.y > gestPeakY_) { gestPeakY_ = h.y; gestPeak_ = h; }
        gestHighT_ = 0.7f;
    } else {
        gestHighT_ = std::max(0.0f, gestHighT_ - dt);
    }
    // Cloak: the casting hand held over the eyes (just before the face) for 0.6 s.
    {
        const Vec3 f = in.head.orientation.rotate(Vec3{0, 0, -1});
        const Vec3 rel = h - in.head.position;
        const bool face = length(rel) < 0.2f && dot(rel, f) > 0.02f && std::fabs(rel.y) < 0.12f;
        gestFaceT_ = face ? gestFaceT_ + dt : 0.0f;
        if (gestFaceT_ > 0.6f && cloakOwned() && cloakCool_ <= 0) {
            castCloak();
            if (trace) LOGI("spell gesture: hand over the eyes -> cloak");
            gestFaceT_ = 0;
            spellGestureCast_ = true;
            spellCharge_ = 0;
            gestCircle_ = gestSweep_ = 0;
            gestPrev_ = h;
            haptics_.push_back({mh, 0.5f, 0.3f});
            return;
        }
    }
    // Slow time: both hands together before the chest for half a second.
    {
        const ControllerState& o = in.hand[1 - mh];
        const bool together = o.gripPoseValid && length(h - o.gripPose.position) < 0.2f && h.y < head.y - 0.1f && h.y > head.y - 0.75f;
        gestTogether_ = together ? gestTogether_ + dt : 0.0f;
        if (gestTogether_ > 0.5f && slowOwned() && slowCool_ <= 0) {
            castSlowTime();
            if (trace) LOGI("spell gesture: hands together -> slow time");
            gestTogether_ = 0;
            spellGestureCast_ = true;
            spellCharge_ = 0;
            gestCircle_ = gestSweep_ = 0;
            gestPrev_ = h;
            haptics_.push_back({kLeft, 0.8f, 0.3f});
            haptics_.push_back({kRight, 0.8f, 0.3f});
            return;
        }
    }
    // Fire circle: both arms stretched out to the sides, between the chest and the hips, for 0.4 s.
    {
        const ControllerState& o = in.hand[1 - mh];
        const Vec3 oh = o.gripPose.position;
        const float apart = length(Vec3{h.x - oh.x, 0, h.z - oh.z});
        const bool spread = o.gripPoseValid && apart > 1.1f && h.y < head.y - 0.2f && oh.y < head.y - 0.2f && h.y > head.y - 1.0f && oh.y > head.y - 1.0f;
        gestArmsT_ = spread ? gestArmsT_ + dt : 0.0f;
        if (gestArmsT_ > 0.4f && slowOwned() && circleCool_ <= 0) {
            castFireCircle();
            if (trace) LOGI("spell gesture: arms out (%.2f m apart) -> fire circle", apart);
            gestArmsT_ = 0;
            spellGestureCast_ = true;
            spellCharge_ = 0;
            gestCircle_ = gestSweep_ = 0;
            gestPrev_ = h;
            haptics_.push_back({kLeft, 0.9f, 0.25f});
            haptics_.push_back({kRight, 0.9f, 0.25f});
            return;
        }
    }
    gestPrev_ = h;
    gestPrevAng_ = ang;
    gestPrevValid_ = true;
    const Vec3 fwd = Vec3{dir.x, 0, dir.z};
    if (std::fabs(gestCircle_) > 6.28318f * 1.1f && tornadoOwned()) {
        castTornado(length(fwd) > 1e-3f ? fwd : Vec3{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)});
        if (trace) LOGI("spell gesture: circle overhead (%.0f deg) -> tornado", std::fabs(gestCircle_) * 57.3f);
    } else if (gestSweep_ > 0.55f) {
        castFireWall(length(fwd) > 1e-3f ? fwd : Vec3{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)});
        if (trace) LOGI("spell gesture: sweep (%.2f m) -> fire wall", gestSweep_);
    } else {
        return;
    }
    spellGestureCast_ = true;
    spellCharge_ = 0;
    gestCircle_ = gestSweep_ = 0;
    haptics_.push_back({mh, 0.9f, 0.15f});
}

// Returns true if it consumed the trigger (a spell was cast or is charging); the caller casts the
// fire bolt on a short tap otherwise.
bool GameApp::updateSpells(float dt, bool trig, const Vec3& from, const Vec3& dir, bool aimOk) {
    initSpells();
    bool cast = false;
    if (spellGestureCast_) {
        // A gesture cast the spell during this pull of the trigger: nothing more until it is let go.
        spellCharge_ = 0;
        if (!trig) spellGestureCast_ = false;
        return true;
    }
    if (trig) {
        spellCharge_ += dt;
        if (spellCharge_ > 0.25f && int(spellCharge_ * 8) != int((spellCharge_ - dt) * 8))
            haptics_.push_back({rig_.moveHand(), spellCharge_ > kTornadoCharge && tornadoOwned() ? 0.5f : (spellCharge_ > kWallCharge ? 0.3f : 0.12f), 0.03f});
    } else if (spellCharge_ > 0) {
        const float c = spellCharge_;
        spellCharge_ = 0;
        if (aimOk && c >= kTornadoCharge && tornadoOwned()) {
            castTornado(dir);
            cast = true;
        } else if (aimOk && c >= kWallCharge) {
            castFireWall(dir);
            cast = true;
        } else if (aimOk && c < kWallCharge) {
            int si;
            if (smartieInAim(from, dir, si)) {
                Bubble b;
                b.pos = from;
                b.target = si;
                b.age = 0;
                b.caught = false;
                bubbles_.push_back(b);
                sfx("sr_spfsh", from, 0.8f, 1.5f);
                LOGI("Delphi casts a bubble at Smartie %d", si);
                cast = true;
            } else {
                return false;  // a tap: the caller's fire bolt
            }
        }
        if (cast) {
            haptics_.push_back({rig_.moveHand(), 0.7f, 0.12f});
            if (!firstPerson_ && player_.onGround) player_.startAction(PlayerAnim::Cast, std::max(0.3f, player_.oneShot[int(PlayerAnim::Cast)] * 0.7f));
        }
        return true;
    }
    return trig && spellCharge_ > 0.25f;
}

void GameApp::stepSpells(float dt) {
    const float t = float(time_);
    circleCool_ = std::max(0.0f, circleCool_ - dt);
    if (circleT_ > 0) {
        const float was = circleT_;
        circleT_ = std::max(0.0f, circleT_ - dt);
        // Its wall: blue flames licking up round the ring.
        for (int k = 0; k < 3; ++k) {
            const float a = frand() * 6.2832f;
            smokePuff(FxTex::FireBlue, circleAt_ + Vec3{std::sin(a) * kCircleR, 0.2f + frand() * 1.5f, std::cos(a) * kCircleR}, Vec3{0, 2.0f, 0}, 0.6f, 0.4f, 0.5f, 0.9f, 0.3f);
        }
        // Bolts that cross it burn up.
        auto& ps = enemyFire_.projectiles();
        for (size_t i = 0; i < ps.size();) {
            const Vec3 d{ps[i].pos.x - circleAt_.x, 0, ps[i].pos.z - circleAt_.z};
            const float r = length(d), y = ps[i].pos.y - circleAt_.y;
            if (std::fabs(r - kCircleR) < 0.6f && y > -0.5f && y < 6.0f) {
                smokePuff(FxTex::FireBlue, ps[i].pos, Vec3{0, 1.0f, 0}, 0.5f, 0.3f, 0.4f, 1.0f, 0.0f);
                ++circleBurnt_;
                ps[i] = ps.back();
                ps.pop_back();
                continue;
            }
            ++i;
        }
        // A Reaper's grenade or bazooka rocket meeting the wall bursts there (not on her).
        for (auto& r : rockets_) {
            if ((r.kind != 3 && r.kind != 6) || r.life >= 1e9f) continue;
            const float rr = length(Vec3{r.pos.x - circleAt_.x, 0, r.pos.z - circleAt_.z}), y = r.pos.y - circleAt_.y;
            if (std::fabs(rr - kCircleR) < 0.6f && y > -0.5f && y < 6.0f) {
                explode(r.pos, 2.5f, 40.0f, false, 0);
                r.life = 1e9f;
                ++circleBurnt_;
                LOGI("Fire circle: a %s bursts in its wall", r.kind == 6 ? "grenade" : "rocket");
            }
        }
        // What stands in the wall burns (half-second ticks: 20 each).
        if (int(circleT_ * 2) != int(was * 2) && weapon_.onHit) {
            for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
                const Enemy& e = enemies_.enemies[i];
                if (e.state == EnemyState::Dead) continue;
                const float r = length(Vec3{e.pos.x - circleAt_.x, 0, e.pos.z - circleAt_.z});
                if (std::fabs(r - kCircleR) < 0.9f && std::fabs(e.pos.y - circleAt_.y) < 3.0f) {
                    hitMul_ = 0.8f;
                    weapon_.onHit(int(i), e.pos + Vec3{0, 1.0f, 0}, normalize(Vec3{e.pos.x - circleAt_.x, 0.1f, e.pos.z - circleAt_.z}));
                    hitMul_ = 1.0f;
                }
            }
            for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
                const Creature& c = creatures_.creatures[i];
                if (c.state == CreatureState::Dead || CreatureDirector::underground(c)) continue;
                const float r = length(Vec3{c.pos.x - circleAt_.x, 0, c.pos.z - circleAt_.z});
                if (std::fabs(r - kCircleR) < 0.9f + 0.4f * c.scale) {
                    hitMul_ = 0.8f;
                    weapon_.onHit(kCreatureTargetS + int(i), c.pos + Vec3{0, 0.5f, 0}, Vec3{0, 1, 0});
                    hitMul_ = 1.0f;
                }
            }
        }
        static const bool ctrace = std::getenv("GVR_SPELL_TRACE") != nullptr;
        if (ctrace && int(circleT_) != int(was)) LOGI("spell trace: fire circle, Delphi %.1f m from its middle", double(length(Vec3{player_.position.x - circleAt_.x, 0, player_.position.z - circleAt_.z})));
        if (circleT_ <= 0) LOGI("Fire circle dies down (%d bolts burnt up)", circleBurnt_);
    }
    hailCool_ = std::max(0.0f, hailCool_ - dt);
    cloakCool_ = std::max(0.0f, cloakCool_ - dt);
    clusterCool_ = std::max(0.0f, clusterCool_ - dt);
    if (cloakT_ > 0) {
        if (cloakT_ - dt <= 0) endCloak("time");
        else if (uint32_t(weapon_.shotsFired()) != cloakShots0_) endCloak("she shot");
        else cloakT_ -= dt;
    }
    slowCool_ = std::max(0.0f, slowCool_ - dt);
    if (slowT_ > 0) {
        slowT_ -= dt;
        if (slowT_ <= 0) { slowT_ = 0; LOGI("Slow time over"); }
    }
    // Hail: each storm drops its stones one by one (~2.4 s), from 30 m up within 7 m of the spot, slanting in.
    for (auto& s : hailStorms_) {
        s.t += dt;
        s.next -= dt;
        while (s.next <= 0 && s.left > 0) {
            s.next += 0.17f;
            --s.left;
            const float a = frand() * 6.2832f, r = 5.0f * std::sqrt(frand());
            HailStone st;
            const Vec3 slant{std::cos(a + 1.0f) * 3.0f, 0, std::sin(a + 1.0f) * 3.0f};
            st.pos = s.centre + Vec3{std::cos(a) * r, 30.0f, std::sin(a) * r} - slant;
            st.vel = slant * (32.0f / 30.0f) + Vec3{0, -32.0f, 0};
            hailStones_.push_back(st);
        }
    }
    hailStorms_.erase(std::remove_if(hailStorms_.begin(), hailStorms_.end(), [](const HailStorm& s) { return s.left <= 0; }), hailStorms_.end());
    for (auto& st : hailStones_) {
        st.age += dt;
        st.pos += st.vel * dt;
        if (frand() < dt * 30.0f) smokePuff(FxTex::Dust, st.pos, Vec3{0, 1.0f, 0}, 0.25f, 0.4f, 0.4f, 0.5f, 0.0f);
        const float g = enemies_.groundHeight ? std::max(player_.seaLevel, enemies_.groundHeight(st.pos.x, st.pos.z)) : 0.0f;
        if (st.pos.y > g && st.age < 4.0f) continue;
        st.pos.y = g;
        blastNoFire_ = true;
        const size_t alive0 = enemies_.aliveCount();
        explode(st.pos, 3.0f, 70.0f, /*fire=*/false, /*hurtsPlayer=*/0);
        blastNoFire_ = false;
        if (enemies_.aliveCount() < alive0) LOGI("Hail: %zu Reaper(s) killed", alive0 - enemies_.aliveCount());
        static const bool trace = std::getenv("GVR_SPELL_TRACE") != nullptr;
        if (trace) LOGI("Hail stone lands at (%.1f %.1f %.1f)", st.pos.x, st.pos.y, st.pos.z);
        for (int k = 0; k < 6; ++k)
            smokePuff(FxTex::Dust, st.pos + Vec3{0, 0.2f, 0}, Vec3{(frand() - 0.5f) * 6.0f, 2.0f + 3.0f * frand(), (frand() - 0.5f) * 6.0f}, 0.5f, 1.2f, 0.8f, 0.6f, 0.6f);
        sfx(frand() < 0.5f ? "ex_stons" : "ex_dirts", st.pos, 0.8f, 1.2f + 0.2f * frand());
        st.age = 99;
    }
    hailStones_.erase(std::remove_if(hailStones_.begin(), hailStones_.end(), [](const HailStone& s) { return s.age >= 99.0f; }), hailStones_.end());
    // Charging glow in the casting hand: orange toward the wall, blue-white toward the tornado.
    if (spellCharge_ > 0.25f && hero_ == Hero::Delphi) {
        Vec3 hand = player_.position + Vec3{0, 1.2f, 0};
        const ControllerState& c = lastInput_.hand[rig_.moveHand()];
        if (firstPerson_ && c.gripPoseValid) hand = rig_.toWorld(c.gripPose).position;
        const bool torn = spellCharge_ >= kTornadoCharge && tornadoOwned();
        smokePuff(torn ? FxTex::FireBlue : FxTex::Fire, hand + Vec3{0, 0.05f, 0}, Vec3{0, 0.3f, 0},
                  0.15f + 0.15f * std::min(1.0f, spellCharge_ / kWallCharge), 0.05f, 0.25f, 1.0f, 0.0f);
    }
    // Fire walls: roll forward 6 m/s for 3 s, burning what they pass.
    for (auto& w : fireWalls_) {
        w.age += dt;
        for (auto& p : w.seg) {
            p += w.dir * (6.0f * dt);
            if (enemies_.groundHeight) p.y = enemies_.groundHeight(p.x, p.z);
            if (frand() < 0.6f) smokePuff(FxTex::Fire, p + Vec3{(frand() - 0.5f) * 0.8f, 0.4f + frand(), (frand() - 0.5f) * 0.8f}, Vec3{0, 1.5f, 0}, 1.0f, 0.3f, 0.5f, 1.0f, 0.5f);
            if (frand() < dt * 3.0f) smokePuff(FxTex::SmokeBlack, p + Vec3{0, 2.0f, 0}, Vec3{0, 1.0f, 0}, 0.8f, 3.0f, 2.0f, 0.4f, 0.4f);
        }
        // Burn: 40 per second to anything within 1.6 m of a segment (half-second ticks).
        if (int(w.age * 2) != int((w.age - dt) * 2) && weapon_.onHit) {
            for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
                const Enemy& e = enemies_.enemies[i];
                if (e.state == EnemyState::Dead) continue;
                for (const auto& p : w.seg)
                    if (length(Vec3{e.pos.x - p.x, 0, e.pos.z - p.z}) < 1.6f) { hitMul_ = 0.8f; weapon_.onHit(int(i), e.pos + Vec3{0, 1.0f, 0}, w.dir); hitMul_ = 1.0f; break; }
            }
            for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
                const Creature& c = creatures_.creatures[i];
                if (c.state == CreatureState::Dead || CreatureDirector::underground(c)) continue;
                for (const auto& p : w.seg)
                    if (length(Vec3{c.pos.x - p.x, 0, c.pos.z - p.z}) < 1.6f + 0.4f * c.scale) { hitMul_ = 0.8f; weapon_.onHit(kCreatureTargetS + int(i), c.pos + Vec3{0, 0.5f, 0}, w.dir); hitMul_ = 1.0f; break; }
            }
            for (size_t i = 0; i < destruct_.items.size(); ++i) {
                const Destructible& d = destruct_.items[i];
                if (!d.alive() || d.kind != DestructKind::Tree) continue;
                for (const auto& p : w.seg)
                    if (length(Vec3{d.base.x - p.x, 0, d.base.z - p.z}) < 1.6f + d.radius) { damageObject(int(i), 30.0f, true, p - w.dir); break; }
            }
        }
    }
    fireWalls_.erase(std::remove_if(fireWalls_.begin(), fireWalls_.end(), [](const FireWall& w) { return w.age > 3.0f; }), fireWalls_.end());
    // Tornadoes: drift along their heading for 7 s (weaving), lifting and throwing what they touch;
    // buildings in their path take heavy damage (the army barracks only from these).
    for (auto& tn : tornadoes_) {
        tn.age += dt;
        const Vec3 side{-tn.dir.z, 0, tn.dir.x};
        tn.pos += (tn.dir * 7.0f + side * (3.0f * std::sin(tn.age * 1.7f))) * dt;
        if (enemies_.groundHeight) tn.pos.y = enemies_.groundHeight(tn.pos.x, tn.pos.z);
        for (int k = 0; k < 2; ++k) {
            const float a = frand() * 6.28f, r = 1.5f + 2.5f * frand();
            smokePuff(FxTex::Dust, tn.pos + Vec3{std::cos(a) * r, 0.3f, std::sin(a) * r}, Vec3{-std::sin(a) * 6.0f, 2.0f + 3.0f * frand(), std::cos(a) * 6.0f}, 1.0f, 3.5f, 1.5f, 0.55f, 1.0f);
        }
        tn.tick -= dt;
        if (tn.tick > 0) continue;
        tn.tick = 0.4f;
        for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
            const Enemy& e = enemies_.enemies[i];
            const Vec3 r{e.pos.x - tn.pos.x, 0, e.pos.z - tn.pos.z};
            if (e.state == EnemyState::Dead || length(r) > 4.5f) continue;
            const bool killed = enemies_.damage(int(i), 35.0f, /*blast=*/true);
            if (killed && enemies_.enemies[i].objective >= 0) completeObjective(enemies_.enemies[i].objective);
            enemies_.push(int(i), normalize(r + Vec3{0, 0.01f, 0}) * 8.0f + Vec3{0, 6.0f, 0});
        }
        for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
            const Creature& c = creatures_.creatures[i];
            if (c.state == CreatureState::Dead || CreatureDirector::underground(c) || length(Vec3{c.pos.x - tn.pos.x, 0, c.pos.z - tn.pos.z}) > 4.5f) continue;
            if (weapon_.onHit) { hitMul_ = 1.6f; weapon_.onHit(kCreatureTargetS + int(i), c.pos, tn.dir); hitMul_ = 1.0f; }
        }
        for (size_t i = 0; i < vimps_.vimps.size(); ++i) {
            const Vimp& v = vimps_.vimps[i];
            if (v.state == VimpState::Dead || length(Vec3{v.pos.x - tn.pos.x, 0, v.pos.z - tn.pos.z}) > 5.0f) continue;
            if (weapon_.onHit) { hitMul_ = 1.6f; weapon_.onHit(kVimpTargetS + int(i), v.pos + Vec3{0, 2, 0}, tn.dir); hitMul_ = 1.0f; }
        }
        tornadoHitting_ = true;
        for (size_t i = 0; i < destruct_.items.size(); ++i) {
            const Destructible& d = destruct_.items[i];
            if (!d.alive() || length(Vec3{d.base.x - tn.pos.x, 0, d.base.z - tn.pos.z}) > 4.5f + d.radius) continue;
            damageObject(int(i), 90.0f, true, tn.pos);
        }
        tornadoHitting_ = false;
    }
    tornadoes_.erase(std::remove_if(tornadoes_.begin(), tornadoes_.end(), [](const Tornado& tn) { return tn.age > 7.0f; }), tornadoes_.end());
    // Bubbles: fly to their Smartie (14 m/s), enclose it, float it over to Delphi and set it down
    // within her reach.
    for (auto& b : bubbles_) {
        b.age += dt;
        if (b.target < 0 || size_t(b.target) >= rescue_.smarties.size()) { b.age = 99; continue; }
        RescueSmartie& s = rescue_.smarties[size_t(b.target)];
        const Vec3 centre = s.pos + Vec3{0, 0.6f, 0};
        if (!b.caught) {
            const Vec3 to = centre - b.pos;
            const float d = length(to);
            if (d < 0.6f) {
                b.caught = true;
                b.age = 0;
                s.state = SmartieState::Idle;  // let go of whatever it hung from
                sfx("me_smton", s.pos, 0.8f, 1.4f);
                LOGI("Bubble caught Smartie %d", b.target);
            } else {
                b.pos += to * (std::min(d, 14.0f * dt) / d);
            }
            if (b.age > 5.0f) b.age = 99;
            continue;
        }
        // Floating to her: ends in front of her at hand height.
        const Vec3 fwd{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)};
        const Vec3 goal = player_.position + fwd * 0.9f;
        Vec3 to = goal - s.pos;
        to.y = 0;
        const float d = length(to);
        const float lift = std::min(1.0f, b.age) * (d > 0.5f ? 1.2f : 1.2f * d / 0.5f);
        if (d > 0.05f) s.pos += to * (std::min(d, 6.0f * dt) / d);
        float g = 0;
        if (enemies_.groundHeight) g = enemies_.groundHeight(s.pos.x, s.pos.z);
        s.pos.y = g + lift;
        b.pos = s.pos + Vec3{0, 0.6f, 0};
        if (d < 0.1f && b.age > 1.0f) {
            s.pos.y = g;
            b.age = 99;
            sfx("me_smtof", s.pos, 0.7f, 1.3f);
            LOGI("Bubble set Smartie %d down by Delphi", b.target);
        }
        if (b.age > 12.0f) { s.pos.y = g; b.age = 99; }
    }
    bubbles_.erase(std::remove_if(bubbles_.begin(), bubbles_.end(), [](const Bubble& b) { return b.age >= 99.0f; }), bubbles_.end());
    (void)t;
}

void GameApp::renderSpells() {
    const float t = float(time_);
    if (fireRoll_)
        for (const auto& w : fireWalls_) {
            const float k = std::min(1.0f, (3.0f - w.age) / 0.5f);
            for (size_t i = 0; i < w.seg.size(); ++i)
                drawModel(fireRoll_, Mat4::translation(w.seg[i] + Vec3{0, 0.8f, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, t * 3.0f + float(i))) *
                                         Mat4::scale(Vec3{1, 1, 1} * (7.0f + std::sin(t * 9.0f + float(i)))), true, 0.9f * k);
        }
    if (swirl_)
        for (const auto& tn : tornadoes_) {
            const float k = std::min(1.0f, std::min(tn.age / 0.5f, (7.0f - tn.age) / 0.7f));
            for (int l = 0; l < 6; ++l) {
                // Stacked swirls overlapping into a funnel, wider up, each spinning (and swaying).
                const float s = 0.55f + 0.42f * float(l);
                const Vec3 sway{0.6f * std::sin(t * 2.0f + float(l) * 0.7f), 0, 0.6f * std::cos(t * 1.7f + float(l))};
                drawModel(swirl_, Mat4::translation(tn.pos + sway * (float(l) / 5.0f) + Vec3{0, 2.2f * float(l), 0}) *
                                      Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, -t * (7.0f - float(l)))) * Mat4::scale(Vec3{s, 1.25f, s}),
                          true, 0.75f * k);
            }
        }
    if (hailModel_)
        for (const auto& st : hailStones_) {
            // Along its fall (the model's long axis taken as its local y), spinning.
            const Vec3 d = normalize(st.vel);
            const Vec3 up{0, 1, 0};
            const Vec3 ax = cross(up, d * -1.0f);
            const float ang = std::acos(std::max(-1.0f, std::min(1.0f, dot(up, d * -1.0f))));
            const Quat q = length(ax) > 1e-4f ? Quat::fromAxisAngle(normalize(ax), ang) : Quat{};
            drawModel(hailModel_, Mat4::translation(st.pos) * Mat4::rotation(q * Quat::fromAxisAngle(up, t * 6.0f)) * Mat4::scale(Vec3{0.55f, 0.55f, 0.55f}), true, 0.95f);  // (the model is ~3 m long: a ~1.7 m block)
        }
    if (slowModel_ && slowT_ > 0) {
        // Round her while time runs slow: fading in and out, slowly turning.
        const float k = std::min(1.0f, std::min((8.0f - slowT_) / 0.4f, slowT_ / 0.8f));
        drawModel(slowModel_, Mat4::translation(player_.position + Vec3{0, 0.1f, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, t * 0.4f)) *
                                  Mat4::scale(Vec3{3.0f, 3.0f, 3.0f}), true, (0.16f + 0.29f * std::max(0.0f, std::min(1.0f, (slowT_ - 6.3f) / 0.5f))) * k);  // (bright as it is cast, faint after)
    }
    if (circleModel_ && circleT_ > 0) {
        // The flared column of blue fire, turning, fading in and out (its foot ~1.1 m round, scaled to the ring).
        const float k = std::min(1.0f, std::min((kCircleTime - circleT_) / 0.4f, circleT_ / 0.8f));
        drawModel(circleModel_, Mat4::translation(circleAt_) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, t * 0.8f)) * Mat4::scale(Vec3{1.8f, 1.0f, 1.8f}),
                  true, 0.45f * k);
    }
    if (bubbleModel_)
        for (const auto& b : bubbles_) {
            bubbleModel_->pose(renderer_, t);
            const float s = b.caught ? 0.28f : 0.12f;
            bubbleModel_->renderGlow(renderer_, Mat4::translation(b.pos) * Mat4::scale(Vec3{s, s, s}), 1.0f);
        }
}

// (2026-10-28) Fire arrows. A flame within r of p: a fire wall, a burning hut or tree, a fireball, the fire
// particles of anything burning (Delphi's own charging hand among them).
bool GameApp::fireNear(const Vec3& p, float r) const {
    for (const auto& w : fireWalls_)
        for (const auto& s : w.seg)
            if (length(Vec3{s.x - p.x, 0, s.z - p.z}) < r + 0.8f && p.y > s.y - 0.3f && p.y < s.y + 2.2f) return true;
    for (const auto& f : fireballs_)
        if (length(f.pos - p) < r + f.radius) return true;
    for (const auto& b : blazes_)
        if (length(Vec3{b.pos.x - p.x, 0, b.pos.z - p.z}) < r + 0.8f && p.y > b.pos.y - 0.3f && p.y < b.pos.y + 1.8f) return true;
    for (const auto& q : fx_.parts)
        if ((q.tex == FxTex::Fire || q.tex == FxTex::FireBlue) && q.opacity() > 0.2f && length(q.pos - p) < r * 0.6f + 0.5f * q.size()) return true;
    if (assetWorld_) {
        const auto& objs = assetWorld_->worldObjects();
        for (const auto& d : destruct_.items) {
            if (d.object < 0 || size_t(d.object) >= objs.size() || objs[size_t(d.object)].glow < 0.15f) continue;
            if (length(Vec3{d.base.x - p.x, 0, d.base.z - p.z}) < d.radius + r && p.y < d.base.y + d.height + 0.5f) return true;
        }
    }
    return false;
}

void GameApp::startBlaze(const Vec3& at) {
    if (blazes_.size() >= 12) blazes_.erase(blazes_.begin());
    Vec3 p = at;
    if (enemies_.groundHeight) p.y = std::min(p.y, enemies_.groundHeight(p.x, p.z) + 1.5f);
    blazes_.push_back({p, 0.0f, 0.0f});
    addFlash(p, {2.2f, 1.2f, 0.4f}, 5.0f, 0.3f);
    sfx("sr_spfsh", p, 0.8f, 0.9f);
    LOGI("Fire arrow: a blaze at (%.1f %.1f %.1f)", p.x, p.y, p.z);
}

// A blaze burns 5 s where a fire arrow struck: flames and smoke; every half second 25 to anything within 1.8 m
// (Reapers, creatures) and to huts, trees and wooden things it stands in - they catch and burn on.
void GameApp::updateBlazes(float dt) {
    for (const auto& pr : weapon_.projectiles())  // (a fire arrow in flight trails flame)
        if (pr.style == 4 && frand() < dt * 40.0f) smokePuff(FxTex::Fire, pr.pos, Vec3{0, 0.3f, 0}, 0.12f, 0.05f, 0.25f, 1.0f, 0.2f);
    for (auto& b : blazes_) {
        b.age += dt;
        const float k = std::max(0.0f, 1.0f - b.age / 5.0f);
        if (frand() < dt * 30.0f * k)
            smokePuff(FxTex::Fire, b.pos + Vec3{(frand() - 0.5f) * 0.8f, 0.2f + 0.4f * frand(), (frand() - 0.5f) * 0.8f}, Vec3{0, 1.6f, 0}, 0.6f, 0.2f, 0.45f, 1.0f, 0.6f);
        if (frand() < dt * 3.0f * k) smokePuff(FxTex::SmokeBlack, b.pos + Vec3{0, 1.2f, 0}, Vec3{0, 1.2f, 0}, 0.5f, 2.2f, 2.0f, 0.4f, 0.4f);
        b.tick -= dt;
        if (b.tick > 0 || !weapon_.onHit) continue;
        b.tick = 0.5f;
        for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
            const Enemy& e = enemies_.enemies[i];
            if (e.state == EnemyState::Dead || length(Vec3{e.pos.x - b.pos.x, 0, e.pos.z - b.pos.z}) > 1.8f) continue;
            hitMul_ = 0.6f;
            weapon_.onHit(int(i), e.pos + Vec3{0, 1.0f, 0}, Vec3{0, 1, 0});
            hitMul_ = 1.0f;
            LOGI("Fire arrow: Reaper %u burns", e.uid);
        }
        for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
            const Creature& c = creatures_.creatures[i];
            if (c.state == CreatureState::Dead || CreatureDirector::underground(c) || length(Vec3{c.pos.x - b.pos.x, 0, c.pos.z - b.pos.z}) > 1.8f + 0.4f * c.scale) continue;
            hitMul_ = 0.6f;
            weapon_.onHit(kCreatureTargetS + int(i), c.pos + Vec3{0, 0.5f, 0}, Vec3{0, 1, 0});
            hitMul_ = 1.0f;
        }
        for (size_t i = 0; i < destruct_.items.size(); ++i) {
            const Destructible& d = destruct_.items[i];
            if (!d.alive() || d.kind == DestructKind::Rock || length(Vec3{d.base.x - b.pos.x, 0, d.base.z - b.pos.z}) > 1.5f + d.radius) continue;
            damageObject(int(i), 25.0f, true, b.pos);
            static int logged = 0;
            if (logged++ < 8) LOGI("Fire arrow: object %d catches fire (%.0f hp left)", int(i), destruct_.items[i].hp);
        }
    }
    blazes_.erase(std::remove_if(blazes_.begin(), blazes_.end(), [](const Blaze& b) { return b.age > 5.0f; }), blazes_.end());
}

}  // namespace gvr
