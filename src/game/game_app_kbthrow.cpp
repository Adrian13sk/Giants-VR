// Kabuto's throws, from his briefings (rules ours - the original's are native code):
// - kstory3_2: "Find the pillar of VOLCANIC ROCK ... and smash it! Grab a BOULDER from the broken pillar
//   and throw it at the nearby BARRACKS - watch the shrapnel fly! ... the pillar of VOLCANIC ROCK will
//   keep giving you more." The pillar is k_rock_gen; its top (k_rock_gen_top, the boulder) is not
//   placed by the mission data, so we sit one on each pillar. A blow at the top knocks it off; a new
//   one rises after a few seconds.
// - k2a: "Try THROWING your enemies at each other - it's like a high-speed car collision without the
//   cars." A Reaper grabbed with X and held (rather than tapped, which eats it) is thrown on release.
// Holding X carries what he grabbed (a boulder or a Reaper); letting go throws it. First person: with
// the swing of the grabbing hand (a slow release just drops it). Third person: kb_throw at whatever is
// nearest in front (a building, a Reaper), lobbed to land on it.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/destruction.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
constexpr float kGravity = 14.0f;       // as everything else (m/s^2)
constexpr float kRegrow = 4.0f;          // s until a pillar has a new top
constexpr float kTapTime = 0.35f;        // X released sooner than this: eat (a Reaper)
constexpr int kMaxLoose = 3;             // loose boulders per pillar
}  // namespace

void GameApp::initThrows() {
    kbRocks_.clear();
    kbBoulders_.clear();
    kbShrapnel_.clear();
    kbThrown_.clear();
    kbHold_ = HoldNone;
    kbHeld_ = -1;
    kbHeldUid_ = 0;
    if (hero_ != Hero::Kabuto || !assetWorld_) return;
    for (const auto& o : assetWorld_->worldObjects()) {
        if (o.name != "k_rock_gen" || !o.model) continue;
        RockGen r;
        r.base = o.centre;
        {
            const Vec4 t = o.transform * Vec4{0, o.model->boundsMax.y, 0, 1};
            r.top = Vec3{o.centre.x, t.y, o.centre.z};
            r.base = Vec3{o.centre.x, (o.transform * Vec4{0, o.model->boundsMin.y, 0, 1}).y, o.centre.z};
        }
        r.radius = o.radius;
        kbRocks_.push_back(r);
    }
    if (!kbRocks_.empty() && !boulderModel_) {
        boulderModel_ = assetWorld_->models()->get("k_rock_gen_top.gbs", "", true);
        if (boulderModel_) {
            const Vec3 e = boulderModel_->boundsMax - boulderModel_->boundsMin;
            boulderSize_ = std::max(0.1f, std::max(e.x, std::max(e.y, e.z)));
            LOGI("Boulder (k_rock_gen_top): %.1f x %.1f x %.1f m", e.x, e.y, e.z);
        }
    }
    for (const auto& r : kbRocks_)
        LOGI("Volcanic rock pillar at game (%.0f %.0f), top %.1f m up", -r.base.x / 0.2f, r.base.z / 0.2f, r.top.y - r.base.y);
}

float GameApp::boulderRadius() const { return 0.14f * player_.height; }  // about 4 m across: a two-handed rock for him

bool GameApp::boulderInReach() const {
    const float reach = 0.6f * player_.height;
    for (const auto& b : kbBoulders_) {
        if (b.state != Boulder::Resting && b.state != Boulder::Loose) continue;
        const Vec3 d = b.pos - player_.position;
        if (d.x * d.x + d.z * d.z < reach * reach) return true;
    }
    for (const auto& r : kbRocks_) {
        const Vec3 d = r.top - player_.position;
        const float l = 0.5f * player_.height + r.radius;
        if (r.hasTop && r.grow >= 1.0f && d.x * d.x + d.z * d.z < l * l && r.top.y < player_.position.y + 1.3f * player_.height) return true;
    }
    return false;
}

void GameApp::rockHit(const Vec3& at, float radius) {
    // A blow near a pillar's top knocks the boulder off it.
    for (auto& r : kbRocks_) {
        if (!r.hasTop) continue;
        Vec3 d = at - r.top;
        const float reach = radius + std::max(0.5f * r.radius, 2.0f * boulderRadius());
        if (at.y < r.base.y || std::sqrt(d.x * d.x + d.z * d.z) > reach || std::fabs(d.y) > reach + r.top.y - r.base.y) continue;
        int loose = 0;
        for (const auto& b : kbBoulders_) loose += b.state == Boulder::Resting || b.state == Boulder::Loose;
        if (loose >= kMaxLoose) return;
        r.hasTop = false;
        r.regrow = kRegrow;
        Boulder b;
        b.pos = r.top + Vec3{0, boulderRadius(), 0};
        Vec3 away = r.top - player_.position;
        away.y = 0;
        away = length(away) > 1e-3f ? normalize(away) : Vec3{1, 0, 0};
        const Vec3 side{-away.z, 0, away.x};
        // Topples off towards him and to one side, to land at his feet.
        b.vel = away * -4.0f + side * ((frand() - 0.5f) * 6.0f) + Vec3{0, 5.0f, 0};
        b.state = Boulder::Loose;
        b.spin = Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f};
        kbBoulders_.push_back(b);
        dustBurst(r.top, 2.0f * boulderRadius(), 12);
        sfx("ex_stons", r.top, 1.0f, 0.7f);
        LOGI("A boulder breaks off the volcanic rock");
        return;
    }
}

bool GameApp::throwAimAt(const Vec3& from, Vec3& target) const {
    // Third person: the nearest building or Reaper in front, within 12 heights.
    const Vec3 fwd{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)};
    const float maxD = 12.0f * player_.height;
    float best = 1e9f;
    auto consider = [&](const Vec3& p, float bias) {
        Vec3 d = p - from;
        d.y = 0;
        const float l = length(d);
        if (l < 0.2f * player_.height || l > maxD) return;
        const float c = dot(d * (1.0f / l), fwd);
        if (c < 0.9f) return;  // within ~25 degrees
        const float score = l * (2.0f - c) * bias;
        if (score < best) { best = score; target = p; }
    };
    for (const auto& it : destruct_.items)
        if (it.alive() && it.kind != DestructKind::Tree) consider(it.base + Vec3{0, std::min(it.height * 0.4f, 0.3f * player_.height), 0}, 0.8f);
    for (const auto& e : enemies_.enemies)
        if (e.state != EnemyState::Dead && e.state != EnemyState::Falling) consider(e.pos + Vec3{0, 0.9f, 0}, 1.0f);
    return best < 1e8f;
}

Vec3 GameApp::lobTo(const Vec3& from, const Vec3& to, float elev) const {
    // Speed at elevation `elev` that lands on `to` (falls back to a flatter, faster throw).
    Vec3 d = to - from;
    const float dy = d.y;
    d.y = 0;
    const float x = std::max(1.0f, length(d));
    const Vec3 h = d * (1.0f / x);
    for (float a = elev; a > 0.05f; a -= 0.1f) {
        const float c = std::cos(a), den = 2.0f * c * c * (x * std::tan(a) - dy);
        if (den <= 0) continue;
        const float v = std::sqrt(kGravity * x * x / den);
        if (v < 120.0f) return h * (v * c) + Vec3{0, v * std::sin(a), 0};
    }
    return h * 60.0f + Vec3{0, 15.0f, 0};
}

void GameApp::updateThrows(float dt, const InputFrame& in) {
    if (hero_ != Hero::Kabuto) return;
    const float H = player_.height;
    const int mh = rig_.moveHand();
    const Vec3 fwd{-std::sin(player_.yaw), 0, -std::cos(player_.yaw)};
    auto ground = [&](const Vec3& p) { return player_.groundHeight ? player_.groundHeight(p.x, p.z) : 0.0f; };

    static int hook = std::getenv("GVR_BOULDER") != nullptr ? 10 : -1;  // test hook: a boulder at his feet
    static int hookR = std::getenv("GVR_REAPER_NEAR") != nullptr ? 10 : -1;  // test hook: two Reapers in front of him
    if (hookR > 0 && playMode_ && --hookR == 0) {
        int n = 0;
        for (auto& e : enemies_.enemies)
            if (e.state != EnemyState::Dead && !e.posted && n < 2) {
                e.pos = player_.position + fwd * (n == 0 ? 0.3f * H : 1.2f * H);  // one to grab, one to hit
                ++n;
            }
        LOGI("Test hook: %d Reapers placed in front of Kabuto", n);
    }
    if (hook > 0 && playMode_ && --hook == 0) {
        Boulder b;
        b.pos = player_.position + Vec3{0, boulderRadius(), 0};
        kbBoulders_.push_back(b);
    }
    // Pillars grow a new top.
    for (auto& r : kbRocks_)
        if (!r.hasTop && (r.regrow -= dt) <= 0) {
            r.hasTop = true;
            r.grow = 0;
            sfx("ka_grnd1", r.top, 0.6f, 0.6f);
        }
    for (auto& r : kbRocks_) r.grow = std::min(1.0f, r.grow + dt * 1.5f);

    // Rework, VR grab: in first person either hand's grip closes on what is in reach of that hand.
    updateKabutoHandGrab(in);
    // Where his grabbing hand is (the hand that grabbed; else the move hand, X's).
    const ControllerState& c = in.hand[mh];
    const ControllerState& hc = in.hand[kbGrabHand_ >= 0 ? kbGrabHand_ : mh];
    Vec3 hand;
    if (firstPerson_ && hc.gripPoseValid) {
        const Pose g = rig_.toWorld(hc.gripPose);
        hand = g.position + g.orientation.rotate(Vec3{0, 0.02f * H, -0.09f * H});  // in front of the fingers
    } else {
        hand = player_.position + fwd * (0.15f * H) + Vec3{0, 0.95f * H, 0};  // raised over his head
    }
    // Hand velocity (first person throws), smoothed.
    if (dt > 0) {
        const Vec3 v = kbHandValid_ ? (hand - kbHandPrev_) * (1.0f / dt) : Vec3{0, 0, 0};
        kbHandVel_ = kbHandVel_ * 0.5f + v * 0.5f;
    }
    kbHandPrev_ = hand;
    kbHandValid_ = true;

    // X: grab (the eating in updateKabuto handles taps on Smarties, Vimps and creatures).
    const bool x = c.primary && playMode_ && !dying_;
    if (x && !kbXWas_ && kbHold_ == HoldNone && kb_.eatT < 0) {
        kbGrabHand_ = -1;
        // The nearest loose boulder within reach, else a Reaper in front.
        float best = 0.6f * H;
        int bi = -1;
        for (size_t i = 0; i < kbBoulders_.size(); ++i) {
            const Boulder& b = kbBoulders_[i];
            if (b.state != Boulder::Resting && b.state != Boulder::Loose) continue;
            Vec3 d = b.pos - player_.position;
            d.y = 0;
            if (length(d) < best) { best = length(d); bi = int(i); }
        }
        // A pillar's top is in reach too (he can pull it off).
        if (bi < 0)
            for (auto& r : kbRocks_) {
                Vec3 d = r.top - player_.position;
                d.y = 0;
                if (r.hasTop && r.grow >= 1.0f && length(d) < 0.5f * H + r.radius && r.top.y < player_.position.y + 1.3f * H) {
                    r.hasTop = false;
                    r.regrow = kRegrow;
                    Boulder b;
                    b.pos = r.top;
                    b.state = Boulder::Loose;
                    // Pulled tops add up (dropped ones stay): past a dozen lying about, the oldest is reused.
                    int loose = 0, oldest = -1;
                    for (size_t j = 0; j < kbBoulders_.size(); ++j)
                        if (kbBoulders_[j].state == Boulder::Loose) { ++loose; if (oldest < 0) oldest = int(j); }
                    if (loose >= 12 && oldest >= 0) {
                        kbBoulders_[size_t(oldest)] = b;
                        bi = oldest;
                    } else {
                        kbBoulders_.push_back(b);
                        bi = int(kbBoulders_.size()) - 1;
                    }
                    LOGI("Kabuto pulls a boulder off the volcanic rock");
                    break;
                }
            }
        if (bi >= 0) {
            kbHold_ = HoldBoulder;
            kbHeld_ = bi;
            kbBoulders_[size_t(bi)].state = Boulder::Held;
            sfx2D("ka_grab", 0.9f);
            haptics_.push_back({mh, 0.6f, 0.1f});
            LOGI("Kabuto picks up a boulder");
        } else {
            float bd = 0.6f * H;
            int ei = -1;
            for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
                const Enemy& e = enemies_.enemies[i];
                if (e.state == EnemyState::Dead || e.state == EnemyState::Falling) continue;
                Vec3 d = e.pos - player_.position;
                d.y = 0;
                const float l = length(d);
                if (l < bd && (l < 0.1f * H || dot(d * (1.0f / l), fwd) > 0.2f)) { bd = l; ei = int(i); }
            }
            if (ei >= 0) {
                kbHold_ = HoldReaper;
                kbHeld_ = ei;
                kbHeldUid_ = enemies_.enemies[size_t(ei)].uid;
                enemies_.knockOff(ei, Vec3{0, 0, 0});
                    sfx2D("ka_grab", 0.9f);
                sfx("rpman_hit1", enemies_.enemies[size_t(ei)].pos, 0.8f, 1.1f);
                haptics_.push_back({mh, 0.6f, 0.1f});
                LOGI("Kabuto grabs Reaper %d", ei);
            }
        }
        kbHoldT_ = 0;
    }
    // Carrying.
    if (kbHold_ != HoldNone) {
        kbHoldT_ += dt;
        if (kbHold_ == HoldBoulder) {
            if (kbHeld_ < 0 || size_t(kbHeld_) >= kbBoulders_.size()) { kbHold_ = HoldNone; }
            else { kbBoulders_[size_t(kbHeld_)].pos = hand; kbBoulders_[size_t(kbHeld_)].vel = Vec3{0, 0, 0}; }
        } else if (kbHold_ == HoldReaper) {
            kbHeld_ = enemyOfUid(kbHeldUid_);  // (the list shrinks as the dead are cleared)
            if (kbHeld_ < 0 || size_t(kbHeld_) >= enemies_.enemies.size() || enemies_.enemies[size_t(kbHeld_)].state != EnemyState::Falling) {
                kbHold_ = HoldNone;
            } else {
                Enemy& e = enemies_.enemies[size_t(kbHeld_)];
                e.pos = hand - Vec3{0, 0.9f, 0};
                e.vel = Vec3{0, 0, 0};
                e.fallFrom = e.pos.y;
            }
        }
    }
    // A Reaper held in the hand and brought up to his mouth: eaten.
    static const bool mouthTrace = std::getenv("GVR_GRAB_TRACE") != nullptr;
    if (mouthTrace && kbGrabHand_ >= 0 && in.headValid)
        LOGI("grab trace: held %.1f m from the mouth (limit %.1f)", length(hand - rig_.toWorld(in.head).position - Vec3{0, 0.06f * H, 0}), 0.22f * H);
    if (kbGrabHand_ >= 0 && kbHold_ == HoldReaper && in.headValid &&
        length(hand - (rig_.toWorld(in.head).position - Vec3{0, 0.06f * H, 0})) < 0.22f * H && kb_.eatT < 0) {
        LOGI("Kabuto: a Reaper brought to his mouth");
        enemies_.enemies[size_t(kbHeld_)].state = EnemyState::Hit;
        kabutoEat(kbHeld_);
        haptics_.push_back({kbGrabHand_, 0.8f, 0.2f});
        kbHold_ = HoldNone;
        kbHeld_ = -1;
        kbHeldUid_ = 0;
        kbGrabHand_ = -1;
    }
    // Let go: throw (or, a quick tap on a Reaper, eat it). A hand grab lets go when the grip opens.
    const bool byHand = kbGrabHand_ >= 0;
    const bool tap = !byHand && kbHold_ == HoldReaper && kbHoldT_ < kTapTime;
    const bool letGo = byHand ? in.hand[kbGrabHand_].squeeze < 0.35f : (!x && kbXWas_);
    if (letGo && (kbHold_ == HoldBoulder || kbHold_ == HoldReaper)) {  // (trees and small folk: game_app_kbcarry.cpp)
        Vec3 vel;
        bool thrown = true;
        if (tap) {
            // (eaten below)
        } else if (firstPerson_ && hc.gripPoseValid) {
            const float S = std::max(1.0f, rig_.comfort.worldScale);
            const float real = length(kbHandVel_) / S;  // the user's own hand speed (m/s)
            if (real < 1.2f) thrown = false;             // just let go: it drops
            vel = kbHandVel_ * 1.5f;
            const float l = length(vel);
            if (l > 90.0f) vel = vel * (90.0f / l);
            // A little help: a target close to the throw's line gets the lob that lands on it.
            Vec3 t;
            if (thrown && throwAimAt(hand, t)) {
                const Vec3 lob = lobTo(hand, t, 0.35f);
                Vec3 a = normalize(Vec3{vel.x, 0, vel.z}), b = normalize(Vec3{lob.x, 0, lob.z});
                if (dot(a, b) > 0.94f) vel = lob;
            }
        } else {
            Vec3 t;
            if (throwAimAt(hand, t)) {
                vel = lobTo(hand, t, 0.35f);
                LOGI("Kabuto throws at game (%.0f %.0f), %.0f m away", -t.x / 0.2f, t.z / 0.2f, length(t - hand));
            } else {
                vel = fwd * 55.0f + Vec3{0, 18.0f, 0};
            }
            player_.startAction(PlayerAnim::Throw, std::max(0.4f, player_.oneShot[int(PlayerAnim::Throw)]));
        }
        if (tap) {
            // A tap: eat it (as before throwing existed).
            enemies_.enemies[size_t(kbHeld_)].state = EnemyState::Hit;
            kabutoEat(kbHeld_);
        } else if (kbHold_ == HoldBoulder) {
            Boulder& b = kbBoulders_[size_t(kbHeld_)];
            b.vel = thrown ? vel : Vec3{0, 0, 0};
            b.state = thrown ? Boulder::Thrown : Boulder::Loose;
            b.spin = Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} * 4.0f;
            if (thrown) { sfx2D("ka_throw", 1.0f); haptics_.push_back({byHand ? kbGrabHand_ : mh, 1.0f, 0.15f}); }
            LOGI("Kabuto %s a boulder (%.0f m/s)", thrown ? "throws" : "drops", length(b.vel));
        } else {
            Enemy& e = enemies_.enemies[size_t(kbHeld_)];
            e.vel = thrown ? vel : Vec3{0, 0, 0};
            e.fallFrom = e.pos.y + (thrown ? 100.0f : 0.0f);  // a thrown one doesn't get up again
            if (thrown) {
                kbThrown_.push_back({e.uid, 0.0f});
                sfx2D("ka_throw", 1.0f);
                sfx("rpman_die1", e.pos, 1.0f, 1.2f);
                haptics_.push_back({mh, 1.0f, 0.15f});
            }
            LOGI("Kabuto %s Reaper %d (%.0f m/s)", thrown ? "throws" : "drops", kbHeld_, length(e.vel));
        }
        kbHold_ = HoldNone;
        kbHeld_ = -1;
        kbHeldUid_ = 0;
        kbGrabHand_ = -1;
    }
    if (kbHold_ == HoldNone) kbGrabHand_ = -1;
    if (kbHold_ != HoldTree && kbHold_ != HoldVictim) kbXWas_ = x;

    // Boulders in flight.
    for (auto& b : kbBoulders_) {
        if (b.state != Boulder::Thrown && b.state != Boulder::Loose) continue;
        b.age += dt;
        b.vel.y -= kGravity * dt;
        b.rot = b.rot + b.spin * dt;
        const Vec3 step = b.vel * dt;
        const float sl = length(step);
        float t = 0;
        Vec3 n;
        int target = -1;
        bool hit = false;
        if (b.state == Boulder::Thrown && sl > 1e-4f && weapon_.raycast) {
            hit = weapon_.raycast(b.pos, step * (1.0f / sl), sl + boulderRadius(), t, n, target);
            if (hit) b.pos = b.pos + step * (std::max(0.0f, t - boulderRadius()) / sl);
        }
        if (!hit) b.pos = b.pos + step;
        const float g = ground(b.pos);
        if (!hit && b.pos.y < g + boulderRadius()) {
            b.pos.y = g + boulderRadius();
            hit = true;
        }
        if (!hit) continue;
        if (b.state == Boulder::Loose) {  // knocked or dropped: it settles
            b.state = Boulder::Resting;
            b.vel = Vec3{0, 0, 0};
            sfx("ka_stomp", b.pos, 0.5f, 1.3f);
            LOGI("A boulder comes to rest at game (%.0f %.0f), %.1f m from Kabuto", -b.pos.x / 0.2f, b.pos.z / 0.2f, length(b.pos - player_.position));
            continue;
        }
        // Thrown: it bursts - "watch the shrapnel fly".
        const float R = 0.3f * H;
        if (target >= kObjectTarget) damageObject(target - kObjectTarget, 250.0f, true, b.pos);
        explode(b.pos, R, 300.0f, /*fire=*/false);
        dustBurst(b.pos, R * 0.7f, 26);
        sfx("ex_stons", b.pos, 1.0f, 0.7f);
        sfx("ka_stomp", b.pos, 1.0f, 1.1f);
        for (int k = 0; k < 10; ++k) {
            Shrapnel s;
            s.pos = b.pos + Vec3{0, 0.5f * boulderRadius(), 0};
            s.vel = Vec3{(frand() - 0.5f) * 30.0f, 8.0f + frand() * 18.0f, (frand() - 0.5f) * 30.0f};
            s.spin = Vec3{frand(), frand(), frand()} * 8.0f;
            s.size = 0.15f + 0.2f * frand();
            kbShrapnel_.push_back(s);
        }
        b.state = Boulder::Gone;
        if (target >= kObjectTarget && size_t(target - kObjectTarget) < destruct_.items.size())
            LOGI("A boulder bursts on an object: %.0f / %.0f hp left", destruct_.items[size_t(target - kObjectTarget)].hp,
                 destruct_.items[size_t(target - kObjectTarget)].maxHp);
        else
            LOGI("A boulder bursts at (%.1f %.1f %.1f)", b.pos.x, b.pos.y, b.pos.z);
        for (const auto& it : destruct_.items)  // what it hurt
            if (it.hp < it.maxHp && length(Vec3{it.base.x, b.pos.y, it.base.z} - b.pos) < R + it.radius)
                LOGI("  a building in reach: %.0f / %.0f hp", std::max(0.0f, it.hp), it.maxHp);
    }
    kbBoulders_.erase(std::remove_if(kbBoulders_.begin(), kbBoulders_.end(), [](const Boulder& b) { return b.state == Boulder::Gone; }),
                      kbBoulders_.end());
    if (kbHold_ == HoldBoulder) {  // indices moved: find the held one again
        kbHeld_ = -1;
        for (size_t i = 0; i < kbBoulders_.size(); ++i)
            if (kbBoulders_[i].state == Boulder::Held) kbHeld_ = int(i);
        if (kbHeld_ < 0) kbHold_ = HoldNone;
    }
    for (auto& s : kbShrapnel_) {
        s.age += dt;
        s.vel.y -= kGravity * dt;
        s.pos = s.pos + s.vel * dt;
        s.rot = s.rot + s.spin * dt;
        const float g = ground(s.pos);
        if (s.pos.y < g) { s.pos.y = g; s.vel = s.vel * 0.3f; s.vel.y = std::fabs(s.vel.y); s.spin = s.spin * 0.5f; }
    }
    kbShrapnel_.erase(std::remove_if(kbShrapnel_.begin(), kbShrapnel_.end(), [](const Shrapnel& s) { return s.age > 3.0f; }), kbShrapnel_.end());

    // Thrown Reapers: bowl over the ones they meet, and burst on landing.
    for (auto& t : kbThrown_) {
        t.age += dt;
        const int ti = enemyOfUid(t.uid);
        if (ti < 0) { t.age = 99; continue; }
        Enemy& e = enemies_.enemies[size_t(ti)];
        const Vec3 c = e.pos + Vec3{0, 0.9f, 0};
        if (e.state == EnemyState::Falling) {
            for (size_t j = 0; j < enemies_.enemies.size(); ++j) {
                if (int(j) == ti) continue;
                Enemy& o = enemies_.enemies[j];
                if (o.state == EnemyState::Dead || length(o.pos + Vec3{0, 0.9f, 0} - c) > 1.6f) continue;
                LOGI("A thrown Reaper hits Reaper %zu", j);
                explode(c, 3.5f, 150.0f, false);
                e.vel = e.vel * 0.3f;
                break;
            }
            continue;
        }
        // Down: whatever is under it is crushed; it doesn't get up.
        explode(c, 3.0f, 120.0f, false);
        if (e.state != EnemyState::Dead) {
            const int obj = e.objective;
            if (enemies_.damage(ti, 1e5f, true) && obj >= 0) completeObjective(obj);
        }
        bleed(c, Vec3{0, 1, 0}, true, 1.2f);
        sfx("ka_stomp", e.pos, 0.7f, 1.4f);
        t.age = 99;
    }
    kbThrown_.erase(std::remove_if(kbThrown_.begin(), kbThrown_.end(), [](const Thrown& t) { return t.age > 30.0f; }), kbThrown_.end());
}

void GameApp::renderThrows() {
    if (!boulderModel_ || hero_ != Hero::Kabuto) return;
    const Vec3 c = (boulderModel_->boundsMin + boulderModel_->boundsMax) * 0.5f;
    const float k = 2.0f * boulderRadius() / boulderSize_;
    auto draw = [&](const Vec3& at, const Vec3& rot, float s) {
        const Quat q = Quat::fromAxisAngle({1, 0, 0}, rot.x) * Quat::fromAxisAngle({0, 1, 0}, rot.y) * Quat::fromAxisAngle({0, 0, 1}, rot.z);
        drawModel(boulderModel_, Mat4::translation(at) * Mat4::rotation(q) * Mat4::scale(Vec3{s, s, s}) * Mat4::translation(c * -1.0f), false);
    };
    for (const auto& r : kbRocks_)  // the pillars' tops, seated in the crater (rising when new)
        if (r.hasTop) {
            const float hgt = (boulderModel_->boundsMax.y - boulderModel_->boundsMin.y) * k;
            drawModel(boulderModel_, Mat4::translation(r.top + Vec3{0, (r.grow - 1.0f) * hgt - 0.3f * hgt, 0}) * Mat4::scale(Vec3{k, k, k}) *
                                         Mat4::translation(Vec3{-c.x, -boulderModel_->boundsMin.y, -c.z}), false);
        }
    for (const auto& b : kbBoulders_) draw(b.pos, b.rot, k);
    for (const auto& s : kbShrapnel_) draw(s.pos, s.rot, k * s.size);
}

}  // namespace gvr
