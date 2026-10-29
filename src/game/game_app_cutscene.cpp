// GameApp: Cutscenes in the world: starting, following the shot cameras, skipping, subtitles. Same class; see game_app.h.
// (Split out of game_app.cpp, 2026-10-28: moved as it was.)
#include "game/game_app.h"
#include "game/hero_clips.h"
#include "core/log.h"
#include "core/prof.h"
#include "scene/asset_world.h"
#include "audio/mixer.h"
#include "audio/music.h"
#include "assets/gtext.h"
#include "assets/skin_anim.h"
#include "game/cutscene.h"
#include "game/arm_ik.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"
#include <algorithm>
#include <fstream>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>

namespace gvr {
bool GameApp::cutsceneActive() const { return cutscene_ && cutscene_->active(); }
size_t GameApp::cutsceneShots() const { return cutscene_ ? cutscene_->shotsPlayed() : 0; }
bool GameApp::cutsceneInfo(std::string& shot, float& t, float& black) const {
    if (!cutscene_ || !cutscene_->active()) return false;
    shot = cutscene_->shotName();
    t = cutscene_->shotTime();
    black = cutscene_->blackout();
    return true;
}

void GameApp::startCutscene(const std::string& first) {
    if (!cutscene_ || first.empty()) return;
    if (!cutscene_->start(first)) return;
    stopVehicleLoops();  // (loops the cutscene would otherwise play under)
    LOGI("Cutscene %s started", first.c_str());
    skipArmed_ = false;  // a trigger held when the cutscene starts must not skip it
}

void GameApp::followHead(float dt, const InputFrame& in) {
    // Head-locked panels (subtitles, the mission briefing): yaw and pitch of the head, no roll,
    // eased toward the head so small head motions don't shake the text.
    if (!in.headValid) return;
    const Pose head = rig_.toWorld(in.head);
    const Vec3 f = head.orientation.rotate(Vec3{0, 0, -1});
    const float yaw = std::atan2(-f.x, -f.z), pitch = std::asin(std::max(-1.0f, std::min(1.0f, f.y)));
    const Quat want = Quat::fromAxisAngle({0, 1, 0}, yaw) * Quat::fromAxisAngle({1, 0, 0}, pitch);
    const float k = subInit_ ? std::min(1.0f, dt * 10.0f) : 1.0f;
    Quat q = subRot_;
    if (q.x * want.x + q.y * want.y + q.z * want.z + q.w * want.w < 0) q = Quat{-q.x, -q.y, -q.z, -q.w};
    subRot_ = normalize(Quat{q.x + (want.x - q.x) * k, q.y + (want.y - q.y) * k, q.z + (want.z - q.z) * k, q.w + (want.w - q.w) * k});
    subHead_ = subInit_ ? subHead_ + (head.position - subHead_) * std::min(1.0f, dt * 20.0f) : head.position;
    subInit_ = true;
}

void GameApp::updateCutscene(float dt, const InputFrame& in) {
    cutscene_->update(dt);
    const std::string stream = cutscene_->takeStream();
    if (!stream.empty()) playStream(stream);
    for (const auto& c : cutscene_->takeCues()) playStream(c, true);
    for (const auto& e : cutscene_->takeEffects()) {
        static const bool trace = std::getenv("GVR_CUT_TRACE") != nullptr;
        if (trace) LOGI("cut-fx %s at (%.1f %.1f %.1f)", e.name.c_str(), e.pos.x, e.pos.y, e.pos.z);
        cutsceneEffect(e.name, e.pos);
    }
    fx_.update(dt);  // the main loop's updateFx is paused while a cutscene plays
    updateDataFx(dt);
    if (in.headValid) headWorldRot_ = rig_.toWorld(in.head).orientation;
    // Skip with either trigger.
    bool skip = false;
    if (in.hand[0].trigger < 0.3f && in.hand[1].trigger < 0.3f) skipArmed_ = true;
    else if (skipArmed_) skip = in.hand[0].trigger > 0.6f || in.hand[1].trigger > 0.6f;
    if (skip) {
        cutscene_->stop();
        if (audio_ && streamVoice_) audio_->stop(streamVoice_);
        if (audio_)
            for (uint32_t v : cueVoices_) audio_->stop(v);
        cueVoices_.clear();
        streamVoice_ = 0;
        streamLeft_ = 0;
        LOGI("Cutscene skipped");
    }
    Vec3 eye;
    float yaw;
    // Keep the viewpoint above the ground: a few shot cameras sit at or below our terrain
    // surface (e.g. story1_jetpack_2's low angle), which would put the viewer inside a hill.
    auto aboveGround = [&](Vec3 p) {
        float h;
        if (assetWorld_ && assetWorld_->terrain().heightAt(p.x, p.z, h)) p.y = std::max(p.y, h + 0.7f);
        // Not inside scenery either (rework): seen from what the shot looks at, anything solid in
        // between brings the camera in front of it (story1_save_sm_2a's camera stood inside the rock
        // pillars it films).
        if (worldRay_ && cutscene_) {
            const Vec3 tgt = cutscene_->targetNow();
            const Vec3 d = p - tgt;
            const float len = length(d);
            float t;
            Vec3 n;
            if (len > 1.0f && worldRay_(tgt, d * (1.0f / len), len, t, n) && t > 1.0f) {
                const Vec3 q = tgt + d * ((t - 0.5f) / len);
                static const bool trace = std::getenv("GVR_CUT_TRACE") != nullptr;
                if (trace) LOGI("cut camera pulled out of scenery: %.1f m nearer its target", len - (t - 0.5f));
                p = q;
            }
        }
        return p;
    };
    if (cutscene_->active()) cutBlack_ = cutscene_->blackout();
    if (cutscene_->active() && cutscene_->takeNewShot(eye, yaw)) {
        // Rework: the view never pitches by itself (comfort), so a shot looking steeply down on its
        // actors (rstory2_out_of_portal_2 from 60 m up) showed the horizon and no one in it. Such a
        // camera swings down (or up) round what it films, same distance and side, to at most 28
        // degrees from level, facing it; a moving camera keeps that offset.
        cutOffset_ = Vec3{0, 0, 0};
        if (renderer_.look() != 0) {
            const Vec3 tgt = cutscene_->targetNow();
            const Vec3 d = eye - tgt;
            const float h = std::sqrt(d.x * d.x + d.z * d.z), L = length(d);
            const float kMax = 0.4887f;  // 28 degrees
            // (Only near shots of characters: a flyover's look-at point km away is no one to frame - intro_island_1.)
            if (L > 2.0f && L < 60.0f && std::fabs(d.y) > h * std::tan(kMax)) {
                Vec3 side = h > 0.05f ? Vec3{d.x / h, 0, d.z / h} : Vec3{std::sin(yaw), 0, std::cos(yaw)};
                const Vec3 moved = tgt + side * (L * std::cos(kMax)) + Vec3{0, (d.y > 0 ? 1.0f : -1.0f) * L * std::sin(kMax), 0};
                if (std::getenv("GVR_CUT_TRACE"))
                    LOGI("cut camera swung to 28 deg: %.0f deg %s its target %.1f m away", std::atan2(std::fabs(d.y), h) * 57.3f, d.y > 0 ? "down on" : "up at", L);
                cutOffset_ = moved - eye;
                eye = moved;
                yaw = std::atan2(-(tgt.x - eye.x), -(tgt.z - eye.z));
            }
        }
        eye = aboveGround(eye);
        // Put the viewer's eyes at the shot camera, facing the scene; head look stays free.
        cutEye_ = eye;
        cutYaw_ = yaw;
        const float headYaw = in.headValid ? yawOf(in.head.orientation) : 0.0f;
        const Quat q = Quat::fromAxisAngle({0, 1, 0}, yaw - headYaw);
        const Vec3 headT = in.headValid ? in.head.position * rig_.comfort.worldScale : Vec3{0, 1.6f, 0};
        rig_.worldFromTracking.orientation = q;
        rig_.worldFromTracking.position = eye - q.rotate(headT);
        subInit_ = false;  // the subtitles jump with the cut
        if (std::getenv("GVR_CUT_TRACE")) {
            const Vec3 got = rig_.toWorld(in.head).position;
            float gh = -1e9f;
            if (assetWorld_) assetWorld_->terrain().heightAt(eye.x, eye.z, gh);
            const Vec3 tg = cutscene_->targetNow();
            float t1 = -1, t2 = -1;
            Vec3 nn;
            const Vec3 dd = normalize(tg - eye);
            if (worldRay_) {
                if (!worldRay_(eye, dd, length(tg - eye), t1, nn)) t1 = -1;
                if (!worldRay_(tg, dd * -1.0f, length(tg - eye), t2, nn)) t2 = -1;
            }
            LOGI("cut target (%.1f %.1f %.1f), %.1f m: ray eye->target hit %.1f, target->eye hit %.1f", tg.x, tg.y, tg.z, length(tg - eye), t1, t2);
            LOGI("cut eye: shot (%.1f %.1f %.1f) ground %.1f head (%.2f %.2f %.2f) scale %.2f -> (%.1f %.1f %.1f)", eye.x, eye.y, eye.z, gh,
                 in.head.position.x, in.head.position.y, in.head.position.z, rig_.comfort.worldScale, got.x, got.y, got.z);
        }
    } else if (cutscene_->active()) {
        // Travel with a moving shot camera (translation only; the view never rotates by itself).
        const Vec3 now = aboveGround(cutscene_->eyeNow() + cutOffset_);
        rig_.worldFromTracking.position = rig_.worldFromTracking.position + (now - cutEye_);
        cutEye_ = now;
    }
    followHead(dt, in);
    if (!cutscene_->active()) {
        // Back to the game where the player stands.
        if (playMode_) rig_.teleportHead(in, player_.position, player_.yaw);
        // A cutscene that faded out (its last shot's script) fades back in on the game.
        respawnFade_ = std::max(respawnFade_, cutBlack_);
        cutBlack_ = 0;
        LOGI("Cutscene finished");
    }
}

void GameApp::renderCutscene(bool actors) {
    if (actors) cutscene_->render();
    const Vec3 eye = renderer_.currentView().eyePosition;
    // Subtitles: a panel below the line of sight, fixed for the shot.
    if (gameText_ && text_.ready() && settings_.subtitles) {
        std::string lines;
        for (const auto& key : cutscene_->subtitles()) {
            const std::string t = gameText_->get(key);
            if (t.empty()) continue;
            if (!lines.empty()) lines += "\n";
            lines += t;
        }
        if (!lines.empty()) {
            float w = 0, h = 0;
            const MeshId m = text_.mesh(lines, 0.052f, 1.3f, &w, &h);
            // Head-locked (user request after device test 19): the panel follows the headset,
            // a little below the line of sight, smoothed so it doesn't jitter with the head.
            const Quat r = subRot_;
            const Vec3 at = subHead_ + r.rotate(Vec3{0, -0.34f, -1.4f});
            // (cinema: under the screen, as on a cinema's subtitle line)
            const Mat4 xf = !actors && cinemaSubValid_ ? cinemaSubXf_ : Mat4::translation(at) * Mat4::rotation(r * Quat::fromAxisAngle({1, 0, 0}, 0.24f));
            text_.drawPanel(xf, w + 0.08f, h + 0.05f, {0, 0, 0, 0.55f});
            text_.draw(m, xf * Mat4::translation({0, 0, 0.003f}), {1.0f, 0.92f, 0.7f, 1.0f});
        }
    }
    const float b = cutscene_->blackout();
    if (b > 0.01f && text_.ready() && actors) {  // (cinema: the picture fades, not the room - renderCinemaRoom)
        const Vec3 fwd = headWorldRot_.rotate(Vec3{0, 0, -1});
        text_.drawPanel(Mat4::translation(eye + fwd * 0.1f) * Mat4::rotation(headWorldRot_), 1.0f, 1.0f, {0, 0, 0, b});
    }
}

}  // namespace gvr
