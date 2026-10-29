// (2026-10-29) Delphi's SUMMON SHADOW: weapondefs item 26 (IconRSShadow, GText "Summon Shadow") - the data holds
// nothing more (no effect model, no HUD art, no rules). Ours:
// - Cast: in first person with her sword, the right trigger held, the blade driven point-first into the ground (the
//   tip going under at over 1.2 m/s, within 6 m of her); it rises at least 2.5 m out.
// - A shadow rises out of the ground there: a Reaper warrior (the data's rpman_l0 with its stand / run / punch
//   clips) drawn near-black with a faint violet glow, trailing dark smoke. For 20 s it runs at the nearest Reaper
//   (within 30 m of it or of her) and strikes, 40 a blow; with nobody to fight it keeps 2.5 m behind her. Then it
//   sinks back into the ground. The Reapers don't fight it (their AI only knows her). 45 s between.
// Reaper story 5 and practice.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/game_app.h"
#include "scene/asset_world.h"
#include "scene/collision.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr float kShadowLife = 20.0f, kShadowCool = 45.0f, kShadowReach = 1.7f, kShadowBlow = 40.0f;
constexpr float kShadowRun = 6.0f, kShadowSight = 30.0f;
}  // namespace

bool GameApp::shadowOwned() const {
    if (hero_ != Hero::Delphi || !assetWorld_) return false;
    return practice_ || toLower(worldName_) == "rstory5" || std::getenv("GVR_SHADOW") != nullptr;  // (test hook)
}

void GameApp::updateShadowSummon(float dt, const Vec3& tip, const Vec3& tipVel, bool trigger, int hand) {
    shadowCool_ = std::max(0.0f, shadowCool_ - dt);
    if (!trigger || !shadowOwned() || !firstPerson_ || shadowW_.on || shadowCool_ > 0 || impaledUid_ || !assetWorld_) return;
    const float g = enemies_.groundHeight ? enemies_.groundHeight(tip.x, tip.z) : tip.y - 1.0f;
    const float flat = length(Vec3{tip.x - player_.position.x, 0, tip.z - player_.position.z});
    static const bool trace = std::getenv("GVR_SPELL_TRACE") != nullptr;
    if (trace && int(time_ * 8) != int((time_ - dt) * 8))
        LOGI("spell trace: SHADOW sword tip %.2f m over the ground, %.1f m/s down, %.1f m out", double(tip.y - g), double(-tipVel.y), double(flat));
    // Point first, going down into the ground (just under it), in front of her.
    if (tip.y > g || tip.y < g - 0.6f || tipVel.y > -1.2f || flat > 6.0f) return;
    if (player_.seaLevel > -1e8f && g < player_.seaLevel - 1.2f) return;  // (not out in deep water; the shallows will do)
    // (Not inside her view: at least 2.5 m out, along where the blade went in - a body rising in her face filled it.)
    Vec3 at{tip.x, g, tip.z};
    const Vec3 d{tip.x - player_.position.x, 0, tip.z - player_.position.z};
    const Vec3 fwd = Quat::fromAxisAngle({0, 1, 0}, player_.yaw).rotate(Vec3{0, 0, -1});
    const Vec3 dir = flat > 0.2f ? d * (1.0f / flat) : fwd;
    if (flat < 2.5f) {
        at = player_.position + dir * 2.5f;
        at.y = enemies_.groundHeight ? enemies_.groundHeight(at.x, at.z) : g;
    }
    summonShadow(at);
    haptics_.push_back({hand, 1.0f, 0.25f});
}

void GameApp::summonShadow(const Vec3& at) {
    ModelCache* mc = assetWorld_->models();
    if (!shadowModel_ || shadowCache_ != mc) {
        shadowModel_.reset();
        shadowCache_ = mc;
        auto m = std::make_unique<SkinnedModel>();
        if (!m->load(assetWorld_->fs(), *mc, renderer_, "rpman_l0", "rpman_stand_scan.anm")) { LOGW("Shadow: model unavailable"); return; }
        shadowClipIdle_ = 0;
        shadowClipRun_ = std::max(0, m->addAnimation(assetWorld_->fs(), "rpman_bzka_run.anm"));
        shadowClipPunch_ = std::max(0, m->addAnimation(assetWorld_->fs(), "rpman_punch.anm"));
        shadowModel_ = std::move(m);
    }
    shadowW_ = ShadowWarrior{};
    shadowW_.on = true;
    shadowW_.pos = at;
    const Vec3 toHer = player_.position - at;
    shadowW_.yaw = std::atan2(toHer.x, toHer.z);  // (facing away from her, out at the fight: forward is -Z)
    shadowW_.life = kShadowLife;
    shadowCool_ = kShadowCool;
    spellGestureCast_ = true;
    endCloak("she cast");
    for (int k = 0; k < 20; ++k)
        smokePuff(FxTex::SmokeBlack, at + Vec3{frand() - 0.5f, 0.1f, frand() - 0.5f} * 1.2f, Vec3{0, 1.2f + frand(), 0}, 0.4f, 1.6f, 1.2f, 0.7f, 0.0f);
    sfx("sr_spfsh", at + Vec3{0, 1.0f, 0}, 1.0f, 0.55f);
    sfx("rpman_hit1", at + Vec3{0, 1.0f, 0}, 0.8f, 0.6f);  // (a low groan out of the ground)
    LOGI("Delphi summons a SHADOW %.1f m away (%.0f s)", double(length(Vec3{at.x - player_.position.x, 0, at.z - player_.position.z})), double(kShadowLife));
}

void GameApp::updateShadowWarrior(float dt) {
    ShadowWarrior& s = shadowW_;
    if (!s.on || !shadowModel_) return;
    s.life -= dt;
    s.clipT += dt;
    s.atkCool = std::max(0.0f, s.atkCool - dt);
    // Rising out of the ground (1 s), sinking back at the end (1 s).
    s.rise = std::min(1.0f, s.rise + dt);
    if (s.life <= 0) {
        if (s.life < -1.0f) {
            s.on = false;
            LOGI("The shadow sinks back into the ground");
        }
        return;
    }
    auto setClip = [&](int c) { if (s.clip != c) { s.clip = c; s.clipT = 0; } };
    // The nearest Reaper on his feet within its sight (or hers), keeping the one it is on.
    int ti = -1;
    float best = 1e9f;
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const Enemy& e = enemies_.enemies[i];
        if (e.state == EnemyState::Dead || e.state == EnemyState::Falling || e.remove || e.posted) continue;
        const float d = length(Vec3{e.pos.x - s.pos.x, 0, e.pos.z - s.pos.z});
        if (d > kShadowSight && length(e.pos - player_.position) > kShadowSight) continue;
        const float score = e.uid == s.target ? d * 0.7f : d;
        if (score < best) { best = score; ti = int(i); }
    }
    Vec3 goal;
    bool fight = false;
    if (ti >= 0 && s.rise >= 1.0f) {
        const Enemy& e = enemies_.enemies[size_t(ti)];
        if (s.target != e.uid) {
            s.target = e.uid;
            LOGI("The shadow goes for Reaper %u, %.0f m off", e.uid, double(length(e.pos - s.pos)));
        }
        goal = e.pos;
        fight = true;
    } else {
        s.target = 0;
        const Vec3 back = Quat::fromAxisAngle({0, 1, 0}, player_.yaw).rotate(Vec3{0, 0, 2.5f});
        goal = player_.position + back;
    }
    Vec3 to = goal - s.pos;
    to.y = 0;
    const float dist = length(to);
    if (s.rise >= 1.0f) {
        if (dist > (fight ? kShadowReach : 1.0f)) {
            const Vec3 dir = to * (1.0f / dist);
            s.yaw = std::atan2(-dir.x, -dir.z);
            const float sp = fight || dist > 6.0f ? kShadowRun : 3.0f;
            s.pos = s.pos + dir * std::min(dist, sp * dt);
            if (enemies_.collision) enemies_.collision->pushOut(s.pos, 0.45f, 2.2f, 0.45f);
            setClip(shadowClipRun_);
        } else if (fight) {
            s.yaw = std::atan2(-to.x, -to.z);
            if (s.atkCool <= 0) {  // a blow
                s.atkCool = 0.9f;
                setClip(shadowClipPunch_);
                s.clipT = 0;
            }
            // The blow lands a third of the way into the punch.
            const float hitAt = 0.33f * std::max(0.3f, shadowModel_->animDuration(shadowClipPunch_));
            if (s.clip == shadowClipPunch_ && s.clipT >= hitAt && s.clipT - dt < hitAt) {
                Enemy& e = enemies_.enemies[size_t(ti)];
                const int obj = e.objective;
                const Vec3 at = enemyChest(e);
                const bool killed = enemies_.damage(ti, kShadowBlow);
                if (killed && obj >= 0) completeObjective(obj);
                if (!killed) enemies_.enemies[size_t(ti)].vel = normalize(to + Vec3{0, 0.01f, 0}) * 3.0f;
                bleed(at, normalize(to + Vec3{0, 0.01f, 0}), killed, 1.0f);
                if (killed) blood_.pool(e.pos, 1.2f);
                sfx(killed ? "rpman_die2" : "ex_bdys1", at, 0.9f, 0.9f + 0.2f * frand());
                LOGI("The shadow strikes Reaper %u%s", e.uid, killed ? " (dead)" : "");
            }
        } else {
            setClip(shadowClipIdle_);
        }
    }
    if (enemies_.groundHeight) s.pos.y = enemies_.groundHeight(s.pos.x, s.pos.z);
    if (frand() < dt * 10.0f)  // a dark smoke off it
        smokePuff(FxTex::SmokeBlack, s.pos + Vec3{frand() - 0.5f, 0.6f + 1.2f * frand(), frand() - 0.5f} * 0.8f, Vec3{0, 0.5f, 0}, 0.25f, 0.8f, 0.9f, 0.4f, 0.0f);
}

void GameApp::renderShadowWarrior() {
    const ShadowWarrior& s = shadowW_;
    if (!s.on || !shadowModel_) return;
    // Out of the ground and back into it: a whole body's height below at the start and the end.
    const float out = s.life > 0 ? s.rise : std::max(0.0f, 1.0f + s.life);
    const float sink = (1.0f - out * out * (3.0f - 2.0f * out)) * 2.3f;
    const float t = s.clip == shadowClipPunch_ ? std::min(s.clipT, shadowModel_->animDuration(s.clip) * 0.999f) : s.clipT;
    shadowModel_->poseBlend(renderer_, s.clip, t, s.clip, t, 0.0f);
    const Mat4 xf = Mat4::translation(s.pos + Vec3{0, enemyFootOffset_ - sink, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, s.yaw));
    shadowModel_->render(renderer_, xf, Vec4{0.07f, 0.05f, 0.11f, 1.0f});   // near black
    shadowModel_->renderGhost(renderer_, xf, Vec3{0.12f, 0.05f, 0.22f});    // a faint violet glow on it
}

}  // namespace gvr
