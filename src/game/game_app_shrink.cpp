// (2026-10-29) Delphi's SHRINK: weapondefs item 29 (IconRSShrinker, GText "Shrink", HUD art st_rp_shrink).
// The data has no effect model for it and no rules (native code); its multiplayer line "%k shrunk %v to
// death" and the hint "You have been SHRUNK!" are what is known. Rules ours:
// - Cast: the casting hand points steadily at a Reaper with its trigger held (as for FOLLOW), then closes -
//   a squeeze of the grip. 30 m, in sight; 14 s between casts.
// - He shrinks to a quarter of his size over half a second and stays so for 15 s, then grows back. Small,
//   he runs from her on short legs, can't fire or punch, and any hit kills him (EnemyDirector).
// - She can tread on him: squashed. A shrunk Reaper killed any way is squashed - no full-size ragdoll.
// Rstory4 and 5 and practice.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/game_app.h"

namespace gvr {

namespace {
constexpr float kShrinkTime = 15.0f;   // s shrunk
constexpr float kShrinkCool = 14.0f;   // s between casts
}  // namespace

bool GameApp::shrinkOwned() const {
    if (hero_ != Hero::Delphi || !assetWorld_) return false;
    const std::string w = toLower(worldName_);
    return practice_ || w == "rstory4" || w == "rstory5" || std::getenv("GVR_SHRINK") != nullptr;  // (test hook)
}

void GameApp::castShrink(uint32_t uid, int hand) {
    const int ei = enemyOfUid(uid);
    if (ei < 0) return;
    Enemy& e = enemies_.enemies[size_t(ei)];
    if (e.state == EnemyState::Dead || e.state == EnemyState::Falling) return;
    spellGestureCast_ = true;  // (the trigger's release is not a fire bolt)
    spellCharge_ = 0;
    followCandUid_ = 0;
    followAimT_ = 0;
    if (shrinkCool_ > 0) {  // not ready: a fizzle
        sfx2D("ph_error", 0.5f);
        haptics_.push_back({hand, 0.2f, 0.05f});
        return;
    }
    endCloak("she cast");
    e.shrinkT = kShrinkTime;
    e.aware = true;
    shrinkCool_ = kShrinkCool;
    const Vec3 mid = e.pos + Vec3{0, 1.0f, 0};
    for (int k = 0; k < 18; ++k) {  // a burst of her blue fire round him, drawing in
        const Vec3 off{frand() - 0.5f, frand() - 0.3f, frand() - 0.5f};
        smokePuff(FxTex::FireBlue, mid + off * 1.6f, off * -1.5f, 0.25f, 0.5f, 0.35f, 0.9f, 0.0f);
    }
    sfx("sr_spfsh", mid, 1.0f, 1.5f);
    sfx("rpman_hit1", mid, 0.9f, 1.9f);  // (his voice going up)
    haptics_.push_back({hand, 0.8f, 0.15f});
    LOGI("Delphi casts SHRINK: Reaper %u, %.0f m away (%.0f s)", e.uid, double(length(e.pos - player_.position)), double(kShrinkTime));
}

void GameApp::updateShrink(float dt) {
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        Enemy& e = enemies_.enemies[i];
        if (e.shrink >= 0.95f || e.remove) continue;
        if (e.state == EnemyState::Dead) {
            // Shrunk to death: squashed, a little splash of him.
            if (!e.gibbed) {
                e.gibbed = true;
                gibCorpse(e.pos, e.yaw, Vec3{0, 1, 0}, true, std::max(0.25f, e.shrink));
                blood_.pool(e.pos, 0.5f);
                sfx("ex_bdys1", e.pos, 0.9f, 1.5f);
                LOGI("SHRINK: Reaper %u squashed", e.uid);
            }
            e.remove = true;
            continue;
        }
        // Trodden on: her feet over him.
        const float flat = length(Vec3{e.pos.x - player_.position.x, 0, e.pos.z - player_.position.z});
        if (e.shrink < 0.6f && flat < 0.55f && std::fabs(player_.position.y - e.pos.y) < 0.6f && playerHealth_ > 0 && !dying_) {
            const int obj = e.objective;
            if (enemies_.damage(int(i), 1e5f) && obj >= 0) completeObjective(obj);
            haptics_.push_back({kLeft, 0.7f, 0.1f});
            haptics_.push_back({kRight, 0.7f, 0.1f});
            LOGI("SHRINK: Delphi treads on Reaper %u", e.uid);
            continue;  // (squashed next frame, above)
        }
        if (e.shrinkT > 0 && frand() < dt * 3.0f) {  // squeaks now and then
            static const char* v[] = {"rpman_hit1", "rpman_hit2"};
            sfx(v[int(frand() * 1.99f)], e.pos, 0.5f, 2.1f + 0.3f * frand());
        }
    }
}

// Test hook GVR_SHRINK_CHASE: once a Reaper is shrunk, she runs straight at the nearest one (to tread on him).
bool GameApp::shrinkChaseInput(Vec2& move, float& viewYaw) {
    static const bool on = std::getenv("GVR_SHRINK_CHASE") != nullptr;
    if (!on || hero_ != Hero::Delphi || !playMode_ || dying_) return false;
    const Vec3 me = player_.position;
    float best = 1e9f;
    Vec3 to;
    for (const auto& e : enemies_.enemies) {
        if (e.state == EnemyState::Dead || e.remove || e.shrink > 0.6f) continue;
        const float d = length(Vec3{e.pos.x - me.x, 0, e.pos.z - me.z});
        if (d < best) { best = d; to = e.pos; }
    }
    if (best > 1e8f) return false;
    viewYaw = std::atan2(-(to.x - me.x), -(to.z - me.z));
    move = Vec2{0, 1};
    return true;
}

}  // namespace gvr
