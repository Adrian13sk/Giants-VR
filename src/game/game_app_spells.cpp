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
    spellReady_ = true;
    LOGI("Spells: fire roll %d, swirl %d, bubble %d", fireRoll_ != nullptr, swirl_ != nullptr, bubbleModel_ != nullptr);
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

void GameApp::castTornado(const Vec3& dir) {
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
    if (!firstPerson_ || !c.gripPoseValid || !in.headValid || c.trigger < 0.6f || spellGestureCast_ || dt <= 0) {
        gestCircle_ = 0;
        gestSweep_ = 0;
        gestT_ = 0;
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
    if (bubbleModel_)
        for (const auto& b : bubbles_) {
            bubbleModel_->pose(renderer_, t);
            const float s = b.caught ? 0.28f : 0.12f;
            bubbleModel_->renderGlow(renderer_, Mat4::translation(b.pos) * Mat4::scale(Vec3{s, s, s}), 1.0f);
        }
}

}  // namespace gvr
