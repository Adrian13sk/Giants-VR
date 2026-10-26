// GameApp, rework: diving. Delphi is a Sea Reaper - the original lets her swim (and heal) at the
// surface; ours lets her go under: look down and push the stick forward (or pull up through the
// water with both grips, as in the arm-stroke swimming) to dive, look up / push down to rise; she
// drifts back up slowly when you stop. She needs no air. Under the surface the world turns into a
// close blue haze, every sound is muffled and bubbles rise from her. The fish, the piranhas and the
// sea monster are already under there.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "audio/mixer.h"
#include "core/log.h"
#include "game/game_app.h"
#include "scene/asset_world.h"

namespace gvr {

void GameApp::updateDive(float dt, const InputFrame& in, PlayerInput& pi) {
    (void)in;
    const bool can = playMode_ && hero_ == Hero::Delphi && !dying_ && !cutsceneActive() && player_.seaLevel > -1e8f && dt > 0;
    if (!can || (!player_.swimming() && player_.diveDepth <= 0)) {
        player_.diveDepth = 0;
        return;
    }
    // Look down and swim forward: down; look up: up (the stick's forward push along the view's pitch).
    const Vec3 view = rig_.toWorld(lastInput_.head).orientation.rotate(Vec3{0, 0, -1});
    const float pitch = std::asin(std::max(-1.0f, std::min(1.0f, view.y)));
    float rate = 0;
    if (std::fabs(pitch) > 0.25f) rate = -std::sin(pitch) * std::max(0.0f, pi.move.y) * player_.swimSpeed * 1.2f;
    rate += strokeDive_;
    if (std::fabs(rate) < 0.05f && player_.diveDepth > 0) rate = -0.25f;  // (she drifts back up)
    float d = player_.diveDepth + rate * dt;
    // Not through the sea bed: her feet stay above it.
    float bed = -1e9f;
    if (player_.groundHeight) bed = player_.groundHeight(player_.position.x, player_.position.z);
    const float maxDepth = std::max(0.0f, (player_.seaLevel - player_.floatDepth) - (bed + 0.3f));
    d = std::max(0.0f, std::min(std::min(maxDepth, 40.0f), d));
    if (d > 0.05f && player_.diveDepth <= 0.05f) LOGI("Delphi dives (the bed %.1f m below the surface)", player_.seaLevel - bed);
    if (d <= 0.05f && player_.diveDepth > 0.05f) LOGI("Delphi surfaces");
    player_.diveDepth = d;
    // Under, the stick's forward push mostly goes into the dive while looking steeply down.
    if (d > 0 && std::fabs(pitch) > 0.9f) pi.move = Vec2{pi.move.x * 0.6f, pi.move.y * 0.6f};
}

void GameApp::undoUnderwater() {
    if (!uwApplied_) return;
    uwApplied_ = false;
    EnvironmentParams& env = renderer_.environment();
    env.fogNear = uwSavedNear_;
    env.fogFar = uwSavedFar_;
    env.sunColor = uwSaved_[0];
    env.fogColor = uwSaved_[1];
    env.clearColor = uwSaved_[2];
    env.skyHorizon = uwSaved_[3];
}

void GameApp::applyUnderwater(float dt) {
    const Vec3 eye = rig_.toWorld(lastInput_.head).position;
    const bool under = playMode_ && player_.seaLevel > -1e8f && eye.y < player_.seaLevel - 0.05f && !cutsceneActive();
    if (under != underwater_) {
        underwater_ = under;
        LOGI("%s the water", under ? "Under" : "Out of");
        if (under) sfx2D("bi_watr1", 0.6f, 0.7f);
    }
    static const bool trace = std::getenv("GVR_DIVE_TRACE") != nullptr;
    static float tt = 0;
    if (trace && (tt += dt) > 1.0f) {
        tt = 0;
        LOGI("dive trace: eye %.2f m from the surface, dive %.2f, feet %.2f, fog %.1f..%.1f", eye.y - player_.seaLevel, player_.diveDepth,
             player_.position.y - player_.seaLevel, renderer_.environment().fogNear, renderer_.environment().fogFar);
    }
    if (audio_) audio_->setMuffle(under ? 1.0f : 0.0f);
    renderer_.setSkyHaze(!under);
    renderer_.setUnderwater(under);  // (under: the haze is the water's own colour, not the sky's)
    EnvironmentParams& env = renderer_.environment();
    if (under) {
        // A close haze the colour of the deep water, darker the deeper.
        const float depth = player_.seaLevel - eye.y;
        const float k = std::max(0.35f, 1.0f - depth / 30.0f);
        uwSavedNear_ = env.fogNear;
        uwSavedFar_ = env.fogFar;
        uwSaved_[0] = env.sunColor;
        uwSaved_[1] = env.fogColor;
        uwSaved_[2] = env.clearColor;
        uwSaved_[3] = env.skyHorizon;
        uwApplied_ = true;
        const Vec3 deep = env.waterColor * 0.9f + Vec3{0.02f, 0.08f, 0.12f};
        env.fogColor = deep * k;
        env.clearColor = env.fogColor;
        env.skyHorizon = env.fogColor;
        env.fogNear = 0.5f;
        env.fogFar = 18.0f + 10.0f * k;
        static const char* uwFog = std::getenv("GVR_UW_FOG");  // (tests: the haze's reach)
        if (uwFog) env.fogFar = float(std::atof(uwFog));
        env.sunColor = env.sunColor * (0.45f * k);
        // Bubbles from her mouth now and then, rising and wobbling.
        bubbleT_ -= dt;
        if (bubbleT_ <= 0 && airBubbles_.size() < 40) {
            bubbleT_ = 0.6f + 1.4f * frand();
            const Vec3 fwd = rig_.toWorld(lastInput_.head).orientation.rotate(Vec3{0, -0.6f, -1});
            const int n = 2 + int(frand() * 4.0f);
            for (int i = 0; i < n; ++i)
                airBubbles_.push_back({eye + normalize(fwd) * 0.18f + Vec3{(frand() - 0.5f) * 0.04f, -frand() * 0.05f, (frand() - 0.5f) * 0.04f}, 0.0f,
                                    0.006f + 0.012f * frand()});
        }
    }
    for (auto& b : airBubbles_) {
        b.age += dt;
        b.pos = b.pos + Vec3{std::sin(b.age * 9.0f + b.size * 500.0f) * 0.05f * dt, (0.25f + 18.0f * b.size) * dt, 0};
    }
    const float sea = player_.seaLevel;
    airBubbles_.erase(std::remove_if(airBubbles_.begin(), airBubbles_.end(), [sea](const AirBubble& b) { return b.pos.y > sea || b.age > 8.0f; }),
                   airBubbles_.end());
}

void GameApp::renderAirBubbles() {
    if (airBubbles_.empty() || renderer_.inReflectionPass()) return;
    if (!bubbleBatchReady_) { bubbleBatch_.init(renderer_, 64); bubbleBatchReady_ = true; }
    const Quat q = headWorldRot_;
    const Vec3 right = q.rotate({1, 0, 0}), up = q.rotate({0, 1, 0});
    bubbleBatch_.clear();
    for (const auto& b : airBubbles_) {
        const uint8_t c[4] = {200, 235, 255, uint8_t(170 * std::min(1.0f, 3.0f - b.age * 0.35f))};
        bubbleBatch_.addBillboard(b.pos, b.size * 2.0f, right, up, 0.0f, c);
    }
    bubbleBatch_.upload(renderer_);
    DrawParams dp;
    dp.texture = glowTexture();
    dp.unlit = true;
    dp.blend = BlendMode::AlphaBlend;
    dp.depthWrite = false;
    dp.doubleSided = true;
    bubbleBatch_.draw(renderer_, dp);
}

}  // namespace gvr
