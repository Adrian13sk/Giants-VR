// The bush pack (Story 2 on; 2b's briefing: "Use the BUSH PACK disguise to hide from Reaper
// Guards. Note that you can't fly or attack while wearing your bush disguise, and that you can only
// wear it for limited spans of time (it draws power from your jetpack)"). It is the fourth item of
// the weapon strip (st_mec_bush): selected, the Meccaryn crouches into the original MCbush (its in /
// idle / shake / out clips, his mc_bush_* clips in third person) and shuffles along slowly. No
// jetpack, no weapon; it drains the jetpack's tank and drops off when the tank is dry. Reapers that
// haven't seen him walk up to a bush that's close or moving and poke it (rpman_poke_bush,
// rpman_bush.wav) - and find him if he's still there; engaged ones lose him at a distance.
// The original's exact rules are not decoded; these are ours.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr float kBushDrain = 1.0f / 16.0f;  // tanks per second: a full tank lasts 16 s in the bush
constexpr float kBushSpeed = 0.4f;           // stick scale: a slow shuffle (2 m/s)
}  // namespace

bool GameApp::hasBush() const {
    if (hero_ != Hero::Mecc || !assetWorld_) return false;
    const std::string m = toLower(assetWorld_->missionName());
    if (m.empty() || m[0] == '1' || m.rfind("2a", 0) == 0) return false;  // given out in 2b
    return m[0] >= '2' && m[0] <= '5' ? true : m.rfind("story", 0) == 0;
}

void GameApp::initBush() {
    if (bushModel_ || !assetWorld_) return;
    auto m = std::make_unique<SkinnedModel>();
    if (!m->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, "MCbush", "MCbush_idle.anm")) return;
    bushIn_ = std::max(0, m->addAnimation(assetWorld_->fs(), "MCbush_in.anm"));
    bushOut_ = std::max(0, m->addAnimation(assetWorld_->fs(), "MCbush_out.anm"));
    bushShake_ = std::max(0, m->addAnimation(assetWorld_->fs(), "MCbush_shake.anm"));
    m->pose(renderer_, 1.0f);
    Vec3 mn, mx;
    m->posedBounds(mn, mx);
    const float h = mx.y - mn.y;
    // Big enough to hide a crouching Meccaryn (about 1.5 m) whatever its own units.
    bushScale_ = h > 1e-3f ? 1.5f / h : 1.0f;
    LOGI("Bush pack: MCbush %.2f m tall as posed, drawn x%.2f", h, bushScale_);
    bushModel_ = std::move(m);
}

void GameApp::updateBush(float dt, PlayerInput& pi) {
    const bool want = weaponSel_ == 2 && hasBush() && playMode_ && !dying_ && !riding_;
    if (want && !bushOn_) {
        if (player_.fuel() <= 0.02f || !player_.onGround) {
            weaponSel_ = 0;  // no power, or in the air: can't
            sfx2D("ph_selct", 0.5f, 0.6f);
        } else {
            bushOn_ = true;
            bushT_ = 0;
            bushOutT_ = -1;
            sfx2D("me_smton", 0.7f, 0.7f);
            LOGI("Into the bush");
        }
    } else if (!want && bushOn_) {
        bushOn_ = false;
        bushOutT_ = 0;
        LOGI("Out of the bush");
    }
    if (bushOutT_ >= 0) {
        bushOutT_ += dt;
        if (bushOutT_ > 1.0f) bushOutT_ = -1;
    }
    enemies_.playerHidden = false;
    enemies_.playerMoving = false;
    if (!bushOn_) return;
    bushT_ += dt;
    // In it: no flying, a slow shuffle; it drains the jetpack.
    pi.jet = false;
    pi.sprint = false;
    pi.move = Vec2{pi.move.x * kBushSpeed, pi.move.y * kBushSpeed};
    player_.drainFuel(kBushDrain * dt);
    if (player_.fuel() <= 0.0f) {
        weaponSel_ = 0;
        bushOn_ = false;
        bushOutT_ = 0;
        timmyLine_ = "BUSH PACK: OUT OF POWER";
        timmyLineT_ = 2.5f;
        LOGI("The bush pack ran out of power");
    }
    enemies_.playerHidden = bushOn_ && bushT_ > 0.6f;
    enemies_.playerMoving = player_.horizontalSpeed() > 0.6f;
}

void GameApp::bushEvents() {
    static const bool trace = std::getenv("GVR_BUSH_TRACE") != nullptr;  // test hook
    if (trace && int(time_ * 2) != int((time_ - 0.014) * 2))
        for (const auto& e : enemies_.enemies)
            if (length(e.pos - player_.position) < 20.0f)
                LOGI("bush trace: reaper %.1f m, state %d aware %d poke %.1f hidden %d moving %d", length(e.pos - player_.position), int(e.state),
                     int(e.aware), e.poke, int(enemies_.playerHidden), int(enemies_.playerMoving));
    // Called after the Reapers' update: their reactions to the bush.
    if (enemies_.pokeStarted) {
        for (const auto& e : enemies_.enemies)
            if (e.poke >= 0 && e.poke < 0.05f) sfx("rpman_bush", e.pos, 1.0f);
        LOGI("A Reaper goes to poke the bush");
    }
    if (enemies_.pokeFound) {
        sfx2D("rpman_hit1", 0.6f, 1.2f);
        timmyLine_ = "FOUND!";
        timmyLineT_ = 1.5f;
        for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.7f, 0.15f});
        LOGI("A Reaper poked the bush and found the player");
    }
}

bool GameApp::bushBodyClip(int& clip, float& t) const {
    // Third person: the Meccaryn's crouch clips while in (or getting out of) the bush.
    if (!bushOn_ && bushOutT_ < 0) return false;
    if (bushOutT_ >= 0) { clip = bushClip_[3]; t = bushOutT_; return clip > 0; }
    if (bushT_ < 0.6f) { clip = bushClip_[0]; t = bushT_; return clip > 0; }
    clip = player_.horizontalSpeed() > 0.4f ? bushClip_[2] : bushClip_[1];
    t = bushT_;
    return clip > 0;
}

void GameApp::renderBush() {
    if (!bushModel_ || (!bushOn_ && bushOutT_ < 0)) return;
    int clip = 0;
    float t = float(time_);
    float k = 1.0f;
    if (bushOutT_ >= 0) { clip = bushOut_; t = bushOutT_; k = std::max(0.0f, 1.0f - bushOutT_); }
    else if (bushT_ < 2.0f && bushIn_ > 0) { clip = bushIn_; t = bushT_; k = std::min(1.0f, bushT_ / 0.4f); }
    else if (player_.horizontalSpeed() > 0.6f && bushShake_ > 0) clip = bushShake_;
    const float d = bushModel_->animDuration(clip);
    bushModel_->poseBlend(renderer_, clip, clip == 0 || clip == bushShake_ ? std::fmod(t, std::max(0.1f, d)) : std::min(t, d * 0.999f), clip,
                          clip == 0 || clip == bushShake_ ? std::fmod(t, std::max(0.1f, d)) : std::min(t, d * 0.999f), 0.0f);
    const float s = bushScale_ * (0.3f + 0.7f * k);
    bushModel_->render(renderer_, Mat4::translation(player_.position) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, player_.yaw)) *
                                      Mat4::scale(Vec3{s, s, s}));
}

}  // namespace gvr
