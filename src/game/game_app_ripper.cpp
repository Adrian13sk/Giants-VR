// Burrowing Rippers, the game side: the dirt thrown up ahead of one tunnelling at the player, the
// hole where one bursts out of the ground or digs in (the original Mound model: a low dirt rim
// round a dark hole, b_mound.tga, opened by its Mound_open clip) with a spray of dirt (Dirt1), small Rippers riding the player
// (rip_attack_mecc), and the spit a dead Ripper leaves lying (orange: medium, blue: large) that the
// player walks over to heal. The AI is game/creature.cpp; what the original did in detail beyond its
// clips and models is not known, the behaviour follows the user's description of it.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr float kHoleLife = 90.0f;      // s a hole stays (fading the last 10)
constexpr float kSpitLife = 90.0f;      // s a spit pickup lies
constexpr float kSpitHeal[2] = {15.0f, 40.0f};  // orange, blue
constexpr size_t kMaxHoles = 40;
}  // namespace

void GameApp::initBurrows(AssetWorld* w, ModelCache* mc) {
    holes_.clear();
    openMounds_.clear();
    spitDrops_.clear();
    burrowTick_ = 0;
    if (!mound_) {
        auto m = std::make_unique<SkinnedModel>();
        if (m->load(w->fs(), *mc, renderer_, "Mound", "mound_idle.anm")) {
            const int k = m->addAnimation(w->fs(), "Mound_open.anm");
            moundOpen_ = k >= 0 ? k : 0;
            mound_ = std::move(m);
        }
        dirtChunk_ = mc->get("Dirt1.gbs", "", /*dynamicLit=*/true);
        LOGI("Burrows: mound %d (open clip %d), dirt %d, hole texture %u", mound_ != nullptr, moundOpen_, dirtChunk_ != nullptr, holeTex_);
    }
}

void GameApp::addHole(const Vec3& at, float size) {
    if (holes_.size() >= kMaxHoles) holes_.erase(holes_.begin());
    GroundHole h;
    h.pos = at;
    h.normal = Vec3{0, 1, 0};
    if (enemies_.groundHeight) {
        const float e = 0.6f;
        const float gx = enemies_.groundHeight(at.x + e, at.z) - enemies_.groundHeight(at.x - e, at.z);
        const float gz = enemies_.groundHeight(at.x, at.z + e) - enemies_.groundHeight(at.x, at.z - e);
        h.normal = normalize(Vec3{-gx, 2.0f * e, -gz});
        h.pos.y = enemies_.groundHeight(at.x, at.z);
    }
    h.size = size;
    h.angle = frand() * 6.28f;
    h.age = 0;
    holes_.push_back(h);
}

void GameApp::burrowEvent(const CreatureEvent& e) {
    const float s = (e.index >= 0 && size_t(e.index) < creatures_.creatures.size()) ? creatures_.creatures[size_t(e.index)].scale : 1.0f;
    const bool big = e.size == CreatureSize::Large;
    switch (e.kind) {
        case CreatureEvent::Emerge: {
            // Out of the ground: the ground bursts open, dirt flies, a hole is left.
            addHole(e.pos, 2.6f * s);
            openMounds_.push_back({e.pos, s, frand() * 6.28f, 0.0f});
            dustBurst(e.pos + Vec3{0, 0.2f, 0}, 1.5f * s, big ? 10 : 6);
            if (dirtChunk_) {
                const size_t before = gibs_.size();
                throwDebris({dirtChunk_}, e.pos, big ? 9 : 6, 5.0f, 0.5f * s);
                for (size_t k = before; k < gibs_.size(); ++k) gibs_[k].life = 12.0f;  // gone after ~8 s
            }
            sfx(big ? "ex_dirtl" : "ex_dirts", e.pos, 1.0f, 0.9f + 0.2f * frand());
            LOGI("A Ripper bursts out of the ground at (%.1f %.1f), %.1f m from the player", e.pos.x, e.pos.z, length(e.pos - player_.position));
            if (length(e.pos - player_.position) < 6.0f)
                for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.5f, 0.12f});
            break;
        }
        case CreatureEvent::DiveIn:
            openMounds_.push_back({e.pos, s * 0.9f, frand() * 6.28f, 0.0f});
            dustBurst(e.pos + Vec3{0, 0.2f, 0}, 1.0f * s, 4);
            sfx("ex_dirts", e.pos, 0.8f, 1.0f + 0.15f * frand());
            break;
        case CreatureEvent::LatchOn:
            sfx("rs_chitt", e.pos, 1.0f, 1.1f);
            for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.6f, 0.2f});
            LOGI("A small Ripper latches on (t=%.1f s)", time_);
            break;
        case CreatureEvent::LatchOff:
            sfx("rs_attk2", e.pos, 0.8f, 1.15f);
            LOGI("The Ripper jumps off (t=%.1f s)", time_);
            break;
        default:
            break;
    }
}

void GameApp::updateBurrows(float dt) {
    static const bool trace = std::getenv("GVR_CREATURE_TRACE") != nullptr;  // test hook
    if (trace && int(time_) != int(time_ - dt)) {
        for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
            const Creature& c = creatures_.creatures[i];
            LOGI("t=%.0f creature %zu state %d at (%.1f %.1f %.1f) %.1f m, scale %.2f", time_, i, int(c.state), c.pos.x, c.pos.y, c.pos.z,
                 length(c.pos - player_.position), c.scale);
        }
    }
    // Dirt kicked up ahead of each Ripper tunnelling (where it is heading), a rumble near the player.
    burrowTick_ += dt;
    const bool puff = burrowTick_ >= 0.07f;
    if (puff) burrowTick_ = 0;
    for (auto& c : creatures_.creatures) {
        if (c.state != CreatureState::Tunnel) continue;
        const float d = length(c.pos - player_.position);
        if (d > 90.0f) continue;
        const Vec3 fwd{-std::sin(c.yaw), 0, -std::cos(c.yaw)};
        const float s = c.scale;
        if (puff) {
            const Vec3 at = c.pos + fwd * (0.6f * s) + Vec3{(frand() - 0.5f) * 0.6f * s, 0.15f, (frand() - 0.5f) * 0.6f * s};
            smokePuff(FxTex::Dust, at, fwd * (1.5f + 1.5f * frand()) + Vec3{0, 1.2f + frand(), 0}, 0.35f * s + 0.2f, 1.6f * s + 0.6f, 1.4f, 0.75f,
                      0.1f);
            if (dirtChunk_ && frand() < 0.3f && gibs_.size() < 160) {
                const size_t before = gibs_.size();
                throwDebris({dirtChunk_}, c.pos + fwd * (0.5f * s), 1, 3.0f, 0.3f * s);
                for (size_t k = before; k < gibs_.size(); ++k) {
                    gibs_[k].vel += fwd * 2.0f;
                    gibs_[k].life = 15.0f;
                }
            }
        }
        // The rumble: the dirt sound now and then (the struggling ground).
        c.patrolWait -= dt;  // (unused underground: the rumble timer)
        if (c.patrolWait <= 0 && d < 45.0f) {
            c.patrolWait = 0.5f + 0.3f * frand();
            sfx("bi_dirt1", c.pos, 0.7f, (c.size == CreatureSize::Large ? 0.7f : 1.0f) + 0.1f * frand());
        }
    }
    for (auto& h : holes_) h.age += dt;
    holes_.erase(std::remove_if(holes_.begin(), holes_.end(), [](const GroundHole& h) { return h.age > kHoleLife; }), holes_.end());
    for (auto& m : openMounds_) m.age += dt;
    const float openDur = mound_ ? mound_->animDuration(moundOpen_) : 1.0f;
    openMounds_.erase(std::remove_if(openMounds_.begin(), openMounds_.end(),
                                     [&](const OpenMound& m) { return m.age > std::max(openDur, 0.5f) + kHoleLife; }),
                      openMounds_.end());

    // Dead Rippers leave their spit lying: orange (medium, some small ones), blue (large).
    for (auto& c : creatures_.creatures) {
        if (c.state != CreatureState::Dead || c.dropped) continue;
        c.dropped = true;
        int big = -1;
        if (c.size == CreatureSize::Large) big = 1;
        else if (c.size == CreatureSize::Medium || frand() < 0.4f) big = 0;
        if (big < 0) continue;
        Vec3 p = c.pos + Vec3{(frand() - 0.5f) * 0.8f, 0, (frand() - 0.5f) * 0.8f};
        if (enemies_.groundHeight) p.y = enemies_.groundHeight(p.x, p.z);
        if (p.y < -0.3f) continue;  // sank in the water
        spitDrops_.push_back({p, big, 0.0f});
    }
    for (auto& sd : spitDrops_) {
        const float before = sd.age;
        sd.age += dt;
        const Vec3 d = sd.pos - player_.position;
        // A glow rising off it, to be found.
        if (std::floor(sd.age * 4.0f) != std::floor(before * 4.0f) && length(d) < 70.0f)
            smokePuff(sd.big ? FxTex::FireBlue : FxTex::Fire, sd.pos + Vec3{0, 0.4f, 0}, Vec3{0, 0.5f, 0}, 0.45f, 0.15f, 0.6f, 0.5f, 0.3f);
        if (!playMode_ || playerHealth_ <= 0 || dying_ || hero_ == Hero::Kabuto || playerHealth_ >= 99.5f) continue;
        if (d.x * d.x + d.z * d.z < 1.3f * 1.3f && std::fabs(d.y) < 2.0f) {
            const float heal = kSpitHeal[sd.big];
            playerHealth_ = std::min(100.0f, playerHealth_ + heal);
            healFlash_ = 1.0f;
            sd.age = 1e9f;
            sfx2D("pu_hlth", 1.0f, sd.big ? 0.85f : 1.1f);
            haptics_.push_back({rig_.moveHand(), 0.5f, 0.1f});
            LOGI("Picked up %s Ripper spit: +%.0f health (%.0f)", sd.big ? "blue" : "orange", heal, playerHealth_);
        }
    }
    spitDrops_.erase(std::remove_if(spitDrops_.begin(), spitDrops_.end(), [](const SpitDrop& s) { return s.age > kSpitLife; }),
                     spitDrops_.end());
}

void GameApp::renderBurrows(const Vec3& eye) {
    // A buried Ripper shows nothing (an ambush; the dirt flies once it moves). The holes left
    // where one came out or dug in: the Mound opening, then lying there.
    if (mound_) {
        const float dur = mound_->animDuration(moundOpen_);
        for (const auto& m : openMounds_) {
            if (length(m.pos - eye) > 90.0f) continue;
            mound_->poseBlend(renderer_, moundOpen_, std::min(m.age, dur * 0.999f), moundOpen_, std::min(m.age, dur * 0.999f), 0.0f);
            const float left = dur + kHoleLife - m.age;
            const float sink = left < 6.0f ? (6.0f - left) * 0.06f * m.scale : 0.0f;  // settles away at the end
            mound_->render(renderer_, Mat4::translation(m.pos - Vec3{0, sink, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, m.yaw)) *
                                          Mat4::scale(Vec3{1, 1, 1} * m.scale));
        }
    }
    // Spit lying on the ground: the original spit ball, slowly turning, pulsing.
    for (const auto& sd : spitDrops_) {
        const int k = sd.big ? 2 : 1;
        if (!spitBall_[k] || length(sd.pos - eye) > 90.0f) continue;
        const float fade = std::min(1.0f, (kSpitLife - sd.age) / 5.0f);
        const float pulse = 0.9f + 0.1f * std::sin(sd.age * 4.0f);
        spitBall_[k]->renderGlow(renderer_,
                                 Mat4::translation(sd.pos + Vec3{0, 0.35f + 0.05f * std::sin(sd.age * 2.0f), 0}) *
                                     Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, sd.age * 1.5f)) * Mat4::scale(Vec3{1, 1, 1} * (0.55f * fade * pulse)) *
                                     Mat4::translation(spitCentre_[k] * -1.0f),
                                 1.2f * fade);
    }
}

}  // namespace gvr
