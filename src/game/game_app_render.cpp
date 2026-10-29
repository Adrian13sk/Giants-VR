// GameApp: Rendering: sun and shadows, reflections, sun rays, the eye views and the world pass. Same class; see game_app.h.
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
void GameApp::updateSun(float dt) {
    EnvironmentParams& env = renderer_.environment();
    env.time = float(time_);
    undoUnderwater();
    undoWeatherLight();  // (last frame's rain light and lightning: never into a newly captured base)
    // The world's own (daylight) colours, captured once per loaded world.
    if (envBaseWorld_ != worldName_ + "#" + std::to_string(reinterpret_cast<uintptr_t>(assetWorld_.get())) ||
        envBaseSea_ != env.seaLevel) {
        envBaseWorld_ = worldName_ + "#" + std::to_string(reinterpret_cast<uintptr_t>(assetWorld_.get()));
        envBaseSea_ = env.seaLevel;
        envBase_ = env;
    }
    sunAzimuth_ += dt * 2.0f * kPi / 1200.0f;
    if (!settings_.dayNight || title_) {
        // Always day: the sun circles once in 20 minutes between 30 and 62 degrees up.
        const float el = (46.0f + 16.0f * std::sin(sunAzimuth_ * 2.0f)) * kPi / 180.0f;
        env.sunDirection = Vec3{std::cos(el) * std::sin(sunAzimuth_), std::sin(el), std::cos(el) * std::cos(sunAzimuth_)} * -1.0f;
        env.sunColor = envBase_.sunColor; env.ambientColor = envBase_.ambientColor; env.fogColor = envBase_.fogColor;
        env.clearColor = envBase_.clearColor; env.skyHorizon = envBase_.skyHorizon; env.skyZenith = envBase_.skyZenith;
        env.waterColor = envBase_.waterColor;
        env.skyTint = {1, 1, 1}; env.night = 0; env.bakedLight = {1, 1, 1};
        return;
    }
    // Day and night (ours; the original's light is fixed per level): one cycle in 20 minutes,
    // about 12 of daylight, a warm dusk and dawn, ~5 minutes of moonlit night with stars.
    if (const char* p = std::getenv("GVR_DAY_PHASE")) { dayPhase_ = float(std::atof(p)); dt = 0; }  // test hook
    dayPhase_ += dt / 1200.0f;
    if (dayPhase_ >= 1.0f) dayPhase_ -= 1.0f;
    const float sinEl = std::sin(2.0f * kPi * dayPhase_) * 0.78f + 0.22f;  // sun height (sine of elevation)
    const float el = std::asin(std::max(-1.0f, std::min(1.0f, sinEl))) ;
    const float az = sunAzimuth_;
    const float d = std::max(0.0f, std::min(1.0f, (sinEl + 0.1f) / 0.3f));            // daylight 0..1
    const float day = d * d * (3.0f - 2.0f * d);
    const float dusk = std::max(0.0f, 1.0f - std::fabs(sinEl - 0.03f) / 0.28f);      // low sun: warm light
    auto mix3 = [](const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; };
    auto mul = [](const Vec3& a, const Vec3& b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; };
    const Vec3 warm{1.0f, 0.62f, 0.42f}, nightBlue{0.13f, 0.17f, 0.3f};
    Vec3 toLight;
    if (sinEl > -0.08f) {
        const float e = std::max(el, 0.03f);
        toLight = {std::cos(e) * std::sin(az), std::sin(e), std::cos(e) * std::cos(az)};
    } else {  // the moon, high on the other side
        const float e = 0.75f;
        toLight = {std::cos(e) * std::sin(az + kPi), std::sin(e), std::cos(e) * std::cos(az + kPi)};
    }
    env.sunDirection = toLight * -1.0f;
    env.sunColor = mix3(Vec3{0.22f, 0.27f, 0.42f}, mix3(envBase_.sunColor, mul(envBase_.sunColor, warm), dusk), day);
    env.ambientColor = mix3(mul(envBase_.ambientColor, Vec3{0.32f, 0.38f, 0.55f}), envBase_.ambientColor, day);
    const Vec3 skyT = mix3(Vec3{0.09f, 0.11f, 0.22f}, mix3(Vec3{1, 1, 1}, Vec3{1.15f, 0.72f, 0.55f}, dusk * 0.8f), day);
    env.skyTint = skyT;
    env.night = 1.0f - day;
    env.fogColor = mul(envBase_.fogColor, mix3(nightBlue, mix3(Vec3{1, 1, 1}, Vec3{1.2f, 0.78f, 0.6f}, dusk * 0.7f), day));
    env.clearColor = env.fogColor;
    env.skyHorizon = env.fogColor;
    env.skyZenith = mul(envBase_.skyZenith, mix3(Vec3{0.12f, 0.14f, 0.24f}, Vec3{1, 1, 1}, day));
    env.waterColor = envBase_.waterColor * (0.35f + 0.65f * day);
    env.bakedLight = mix3(Vec3{0.34f, 0.4f, 0.58f}, mix3(Vec3{1, 1, 1}, Vec3{1.08f, 0.86f, 0.72f}, dusk * 0.6f), day);
}

void GameApp::renderShadows() {
    renderer_.setTerrainDetail(quality_ < 2);  // the adaptive fallback drops the extra terrain layers too
    {  // rework: clouds' shadows drifting over the land (not at the reduced levels, nor in the classic look)
        static const char* ce = std::getenv("GVR_CLOUDS");
        const bool on = quality_ < 2 && renderer_.look() != 0 && !settings_.classicLook && !(ce && ce[0] == '0');
        renderer_.setCloudShadow(on ? (ce ? float(std::atof(ce)) : 0.5f) : 0.0f);
    }
    // ...and the rework's per-pixel relief and highlights (keeping its tone and haze).
    if (renderer_.look() != 0 && !settings_.classicLook) renderer_.setLook(quality_ >= 2 ? 2 : 1);
    // Level 1: the rework look's lighter step (relief, highlights and detail octaves off: 3 ms on the headset).
    renderer_.setLookLite(quality_ == 1);
    buildBatches();
    pushUnscaledRig(lastInput_.head, lastInput_.headValid);  // HUD at human scale (Kabuto)
    buildHud();
    popUnscaledRig();
    if (!weaponsTicked_) dropHeldWeaponState();  // no weapon update ran since the last frame
    weaponsTicked_ = false;
    renderShadowMap();
    renderer_.updateHaze();  // (the sky haze cube for this frame's views)
    renderReflection();
    renderScope();
    renderCinema();  // rework: a cutscene's picture for the cinema screen
    buildBowLines();
}

void GameApp::renderShadows(const EyeRenderInfo& left, const EyeRenderInfo& right) {
    const EyeRenderInfo* e[2] = {&left, &right};
    for (int k = 0; k < 2; ++k) {
        eyeFov_[k][0] = e[k]->fovLeft; eyeFov_[k][1] = e[k]->fovRight; eyeFov_[k][2] = e[k]->fovUp; eyeFov_[k][3] = e[k]->fovDown;
    }
    haveFov_ = true;
    renderShadows();
}

void GameApp::renderShadowMap() {
    if (quality_ >= 3) { renderer_.setShadowsActive(false); return; }
    if (!shadowsOn_ || !assetWorld_ || !assetWorld_->loaded()) { renderer_.setShadowsActive(false); shadowFresh_ = false; return; }
    // (2026-10-28) At the reduced levels the map is drawn every other frame and reused in between (sampled
    // with the matrix it was drawn with, so it stays put; moving shadows lag one frame). GVR_SHADOW_EVERY=n.
    static const int every = std::getenv("GVR_SHADOW_EVERY") ? std::max(1, std::atoi(std::getenv("GVR_SHADOW_EVERY"))) : 0;
    const int n = every > 0 ? every : (quality_ >= 1 ? 2 : 1);
    if (++shadowFrame_ % uint32_t(n) != 0 && shadowFresh_ && !cutsceneActive()) return;
    shadowFresh_ = true;
    const Vec3 centre = playMode_ ? player_.position : rig_.worldFromTracking.position;
    // 48 m square around the Meccaryn (~2.3 cm texels at 2048); wider for a bigger hero (Kabuto:
    // ~140 m, so the trees a few of his steps away still cast shadows; texel size cost nothing on the Quest).
    const float half = 24.0f * std::sqrt(std::max(1.0f, heroScale_));
    renderer_.beginShadowPass(renderer_.sunShadowMatrix(centre, half));
    assetWorld_->renderShadowCasters(renderer_, centre, half * 1.5f);
    const bool cut = cutsceneActive();
    for (const auto& c : chars_) {
        if (!c.visible || length(c.pos - centre) > half * 1.4f) continue;
        c.model->renderShadow(renderer_, c.xf());
    }
    renderVillagers(true, centre, half * 1.4f);
    renderVimps(true, centre, half * 1.4f);
    renderJetski(true, centre);
    for (const auto& n : nests_)
        if (n.alive && n.model && length(n.pos - centre) < half * 1.4f)
            for (const auto& part : n.model->parts) renderer_.drawShadow(part.mesh, n.xf * Mat4::scale(Vec3{1, 1, 1} * n.shown));
    for (const auto& e : enemies_.enemies)
        if (e.slot >= 0 && length(e.pos - centre) < half * 1.4f)
            enemyPool_[size_t(e.slot)]->renderShadow(renderer_, enemyXf(e));
    renderReaperWeapons(true, centre, half * 1.4f);
    renderSkirmish(true, centre);
    // The player's own body (whole, also in first person) and Timmy on his back.
    if (playMode_ && playerModel_ && !cut && !playerGibbed_) playerModel_->renderShadowFull(renderer_, bodyXf_);
    if (playMode_ && timmyOn_ && !cut) timmy_->renderShadow(renderer_, timmyXf_);
    if (playMode_ && gyroModel_ && gyro_.on && !cut && length(gyro_.pos - centre) < half * 1.4f) gyroModel_->renderShadow(renderer_, gyroXf());  // (the gyrocopter)
    renderer_.endShadowPass();
    renderer_.setShadowsActive(true);
}

// Inverse of an affine matrix (rotation/scale/mirror + translation).
Mat4 affineInverse(const Mat4& m) {
    const float a = m.m[0], b = m.m[4], c = m.m[8], d = m.m[1], e = m.m[5], f = m.m[9], g = m.m[2], h = m.m[6], i = m.m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    Mat4 r = Mat4::identity();
    if (std::fabs(det) < 1e-12f) return r;
    const float k = 1.0f / det;
    r.m[0] = (e * i - f * h) * k;  r.m[4] = (c * h - b * i) * k;  r.m[8] = (b * f - c * e) * k;
    r.m[1] = (f * g - d * i) * k;  r.m[5] = (a * i - c * g) * k;  r.m[9] = (c * d - a * f) * k;
    r.m[2] = (d * h - e * g) * k;  r.m[6] = (b * g - a * h) * k;  r.m[10] = (a * e - b * d) * k;
    r.m[12] = -(r.m[0] * m.m[12] + r.m[4] * m.m[13] + r.m[8] * m.m[14]);
    r.m[13] = -(r.m[1] * m.m[12] + r.m[5] * m.m[13] + r.m[9] * m.m[14]);
    r.m[14] = -(r.m[2] * m.m[12] + r.m[6] * m.m[13] + r.m[10] * m.m[14]);
    return r;
}

void GameApp::renderReflection() {
    ++frameNo_;
    // Every other frame (every third at quality 1); the last one is reused in between.
    const uint32_t every = quality_ >= 1 ? 3u : 2u;
    if (quality_ >= 2) { renderer_.setReflectionActive(false); return; }
    if (frameNo_ % every != 0 && reflFresh_) return;
    reflFresh_ = false;
    // (2026-10-28) Half size at quality 1 (GVR_REFL_HALF=0/1 forces it either way, to compare).
    static const char* rh = std::getenv("GVR_REFL_HALF");
    renderer_.setReflectionHalf(rh ? rh[0] == '1' : quality_ >= 1);
    renderer_.setReflectionActive(false);
    if (!reflOn_ || !haveFov_ || !assetWorld_ || !assetWorld_->loaded() || !lastInput_.headValid) return;
    if (load_.phase != LoadPhase::Idle && load_.phase != LoadPhase::FadeOut) return;
    const float sea = renderer_.environment().seaLevel;
    const Pose head = rig_.toWorld(lastInput_.head);
    if (head.position.y < sea + 0.05f) return;  // under water: no mirror
    // Mirror about the sea surface: M = T(sea) S(1,-1,1) T(-sea).
    Mat4 mirror = Mat4::identity();
    mirror.m[5] = -1.0f;
    mirror.m[13] = 2.0f * sea;
    ViewParams v;
    v.view = Mat4::fromPose(head.inverse()) * mirror;
    v.eyePosition = Vec3{head.position.x, 2.0f * sea - head.position.y, head.position.z};
    // Wide enough for both eyes, plus a margin for the wave distortion.
    const float m = 0.08f;
    const float l = std::min(eyeFov_[0][0], eyeFov_[1][0]) - m, rr = std::max(eyeFov_[0][1], eyeFov_[1][1]) + m;
    const float u = std::max(eyeFov_[0][2], eyeFov_[1][2]) + m, d = std::min(eyeFov_[0][3], eyeFov_[1][3]) - m;
    const float nearZ = 0.1f;
    // No water in view (looking up at the sky): skip the pass.
    bool seesDown = false;
    for (int c = 0; c < 4 && !seesDown; ++c) {
        const Vec3 ray = head.orientation.rotate(normalize(Vec3{std::tan((c & 1) ? rr : l), std::tan((c & 2) ? u : d), -1.0f}));
        seesDown = ray.y < -0.01f;
    }
    if (!seesDown) return;
    v.projection = Mat4::projectionFov(l, rr, u, d, nearZ, 0.0f);
    const uint32_t calls0 = renderer_.drawCallsThisFrame();
    renderer_.beginReflectionPass(v, nearZ);
    const bool cut = cutsceneActive();
    std::vector<int> cutTypes;
    if (cut) cutTypes = cutscene_->actorTypes();
    assetWorld_->setHiddenTypes(cutTypes);
    assetWorld_->render(renderer_, true);
    for (const auto& n : nests_)
        if (n.alive && n.shown >= 0.02f) drawModel(n.model, n.xf * Mat4::scale(Vec3{1, 1, 1} * n.shown), false);
    const Vec3 eye = head.position;
    if (playMode_ && playerModel_ && !cut && !playerGibbed_) playerModel_->renderFull(renderer_, bodyXf_);
    if (playMode_ && timmyOn_ && !cut) timmy_->render(renderer_, timmyXf_);
    if (playMode_ && gunValid_ && !cut) drawModel(heldModel(), gunXf_, false);
    for (const auto& c : chars_) {
        if (!c.visible || length(c.pos - eye) > (c.flyer ? 260.0f : 90.0f)) continue;
        if (cut && std::find(cutTypes.begin(), cutTypes.end(), c.type) != cutTypes.end()) continue;
        c.model->render(renderer_, c.xf(), Vec4{1.0f + 2.2f * c.flash, 1.0f + 0.9f * c.flash, 1.0f + 0.5f * c.flash, 1.0f});
    }
    for (const auto& e : enemies_.enemies)
        if (e.slot >= 0 && length(e.pos - eye) < 90.0f)
            enemyPool_[size_t(e.slot)]->render(renderer_, enemyXf(e));
    if (cut) cutscene_->render();
    renderer_.endReflectionPass();
    renderer_.setReflectionActive(true);
    reflFresh_ = true;
    static int logged = 0;
    if (logged < 3) { ++logged; LOGI("Reflection pass: %u draw calls", renderer_.drawCallsThisFrame() - calls0); }
}

void GameApp::renderSunRays() {
    // Shafts of light around the sun: a slowly turning fan of faint additive rays far out on the
    // sky in the sun's direction. Terrain and objects in front hide them (depth test), so they read
    // as rays breaking past ridges and trees. (A cheap stand-in for screen-space god rays.)
    if (!sunRaysMesh_) {
        MeshData m;
        const int n = 14;
        for (int i = 0; i < n; ++i) {
            const float a = float(i) * 2.0f * kPi / float(n) + 0.17f * float(i % 3);
            const float len = 0.55f + 0.25f * float((i * 7) % 5) / 4.0f, w = 0.035f + 0.02f * float(i % 2);
            const uint32_t b = uint32_t(m.vertices.size());
            const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(a + kPi * 0.5f), sb = std::sin(a + kPi * 0.5f);
            const float pts[3][3] = {{cb * w * 0.3f, sb * w * 0.3f, 255}, {-cb * w * 0.3f, -sb * w * 0.3f, 255}, {ca * len, sa * len, 0}};
            for (const auto& p : pts) {
                Vertex v{};
                v.pos[0] = p[0];
                v.pos[1] = p[1];
                v.normal[2] = 1;
                v.color[0] = v.color[1] = v.color[2] = 255;
                v.color[3] = uint8_t(p[2]);
                m.vertices.push_back(v);
            }
            m.indices.insert(m.indices.end(), {b, b + 1, b + 2});
            (void)cb;
        }
        sunRaysMesh_ = renderer_.uploadMesh(m);
    }
    const EnvironmentParams& env = renderer_.environment();
    const Vec3 toSun = normalize(env.sunDirection * -1.0f);
    const Vec3 eye = renderer_.currentView().eyePosition;
    const float dist = 900.0f;
    // Billboard facing the viewer at the sun's position, rotating slowly.
    const Vec3 f = toSun * -1.0f;
    const Vec3 upHint = std::fabs(f.y) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 r = normalize(cross(upHint, f)), u = cross(f, r);
    Mat4 basis = Mat4::identity();
    basis.m[0] = r.x; basis.m[1] = r.y; basis.m[2] = r.z;
    basis.m[4] = u.x; basis.m[5] = u.y; basis.m[6] = u.z;
    basis.m[8] = -f.x; basis.m[9] = -f.y; basis.m[10] = -f.z;
    DrawParams dp;
    dp.blend = BlendMode::Additive;
    dp.unlit = true;
    dp.noFog = true;
    dp.doubleSided = true;
    dp.depthWrite = false;
    const float k = 0.10f + 0.03f * std::sin(float(time_) * 0.7f);
    dp.tint = {env.sunColor.x * k, env.sunColor.y * k, env.sunColor.z * k * 0.85f, 1.0f};
    renderer_.draw(sunRaysMesh_, Mat4::translation(eye + toSun * dist) * basis * Mat4::rotation(Quat::fromAxisAngle({0, 0, 1}, float(time_) * 0.02f)) *
                                     Mat4::scale({dist, dist, dist}), dp);
}

void GameApp::drawModel(const RenderModel* m, const Mat4& xf, bool additive, float alpha) {
    if (!m) return;
    for (const auto& part : m->parts) {
        DrawParams dp;
        dp.texture = part.texture;
        if (additive) {
            dp.blend = BlendMode::Additive;
            dp.unlit = true;
            dp.depthWrite = false;
            dp.doubleSided = true;
            dp.tint = {alpha, alpha, alpha, 1.0f};
        } else {
            dp.blend = part.blend;
            if (part.glass) { dp.blend = BlendMode::AlphaBlend; dp.tint.w *= 0.3f; }
            dp.unlit = !part.lit;
        }
        renderer_.draw(part.mesh, xf, dp);
    }
}

void GameApp::renderEye(const EyeRenderInfo& eye) { renderViews(&eye, 1); }

void GameApp::renderStereo(const EyeRenderInfo& left, const EyeRenderInfo& right) {
    const EyeRenderInfo eyes[2] = {left, right};
    renderViews(eyes, 2);
}

void GameApp::beginEyeViews(const EyeRenderInfo* eyes, int n, const Pose* worldEyeOverride, bool clear) {
    ViewParams vs[2];
    for (int k = 0; k < n; ++k) {
        const Pose ew = worldEyeOverride ? *worldEyeOverride : rig_.toWorld(eyes[k].trackingPose);
        vs[k].view = Mat4::fromPose(ew.inverse());
        vs[k].projection = Mat4::projectionFov(eyes[k].fovLeft, eyes[k].fovRight, eyes[k].fovUp, eyes[k].fovDown, 0.05f, 0.0f);
        vs[k].eyePosition = ew.position;
    }
    if (n == 2) renderer_.beginStereoView(vs, eyes[0].width, eyes[0].height, clear);
    else renderer_.beginView(vs[0], eyes[0].width, eyes[0].height, clear);
}

// Everything of the world as seen from `v` (the eyes, or the cinema screen's camera).
void GameApp::renderWorld(const ViewParams& v, bool cut, const std::vector<int>& cutTypes) {
    if (assetWorld_) assetWorld_->setHiddenTypes(cutTypes);
    if (assetWorld_ && assetWorld_->loaded()) assetWorld_->render(renderer_);
    for (const auto& n : nests_) {
        if (!n.alive || n.shown < 0.02f) continue;
        drawModel(n.model, n.xf * Mat4::scale(Vec3{1, 1, 1} * n.shown), false);
    }
    if (assetWorld_ && assetWorld_->loaded()) renderSunRays();
    if (playMode_ && playerModel_ && !cut && !playerGibbed_) {
        // First person: nothing of the body within reach of the eyes (an idle clip's lean brought
        // Delphi's shoulder and face into view).
        const bool cutNear = firstPerson_ && !dying_ && !renderer_.inReflectionPass();
        if (cutNear) renderer_.setNearCut(renderer_.currentView().eyePosition, 0.14f * player_.height);
        if (cloakT_ > 0) playerModel_->renderGhost(renderer_, bodyXf_, Vec3{0.35f, 0.55f, 0.8f});  // (cloaked: see-through)
        else playerModel_->render(renderer_, bodyXf_);
        if (cutNear) renderer_.setNearCut(Vec3{0, 0, 0}, 0.0f);
    }
    if (playMode_ && !cut && !dying_) renderJetFx(firstPerson_);
    if (playMode_ && !cut && !dying_) renderBush();
    if (playMode_ && swordValid_ && swordModel_ && !cut && !dying_) drawModel(swordModel_, swordXf_, false);
    if (!cut) renderBow();
    if (playMode_ && !cut && !dying_) renderRepairBeam();
    if (playMode_ && !cut) renderPortTurret();
    if (playMode_ && !cut) renderGyro();
    if (playMode_ && !cut && !dying_) renderShield();
    // Timmy on the back: seen in third person (and in reflections and shadows). In first person he
    // sits right behind your head and the body turns with it, so he'd only ever flicker at the edge
    // of view: not drawn there.
    if (playMode_ && timmyOn_ && !cut && !firstPerson_) timmy_->render(renderer_, timmyXf_);
    if (playMode_ && gunValid_ && !cut && !dying_) {
        drawModel(heldModel(), gunDrawXf(), false);
        drawScopeLens();
        renderAimLaser();
    }
    if (playMode_ && !cut && !dying_) renderMarksmanLasers();
    if (playMode_ && !cut && !dying_ && !playerGibbed_) {  // the other weapon, holstered on the body
        if (holsterGun_) drawModel(gunModel_, holsterGunXf_, false);
        if (holsterLauncher_ && !firstPerson_) drawModel(launcherModel_, holsterLauncherXf_, false);
    }
    if (rocketModel_)
        for (const auto& r : rockets_) {
            if (r.kind == 1 || r.kind == 11) continue;  // Delphi's bolt and the sea monster's fireball are all fire (particles)
            if (r.kind == 6) {
                if (grenadeModel_) {
                    const float k = 0.45f;
                    drawModel(grenadeModel_, Mat4::translation(r.pos) * Mat4::rotation(Quat::fromAxisAngle(normalize(Vec3{0.3f, 1, 0.2f}), r.life * 7.0f)) *
                                                 Mat4::scale({k, k, k}) * Mat4::translation((grenadeModel_->boundsMin + grenadeModel_->boundsMax) * -0.5f), false);
                }
                continue;
            }
            if (r.kind == 5 && lbombModel_) {  // the Millenium bomb, tumbling
                const float k = 0.35f;
                drawModel(lbombModel_, Mat4::translation(r.pos) * Mat4::rotation(Quat::fromAxisAngle(normalize(Vec3{1, 0.3f, 0.2f}), r.life * 9.0f)) *
                                           Mat4::scale({k, k, k}) * Mat4::translation((lbombModel_->boundsMin + lbombModel_->boundsMax) * -0.5f), false);
                continue;
            }
            if (r.kind == 7 || r.kind == 8) {  // an RPG-bow / homing arrow: the streak (rparrow) along its flight
                if (arrowModel_) {
                    const Vec3 f = normalize(r.vel);
                    const Quat yawQ = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
                    const Quat look = yawQ * Quat::fromAxisAngle({1, 0, 0}, std::asin(std::max(-1.0f, std::min(1.0f, f.y))));
                    drawModel(arrowModel_, Mat4::translation(r.pos) * Mat4::rotation(look) * Mat4::scale({0.25f, 0.25f, 0.25f}), true, 0.9f);
                }
                continue;
            }
            if (r.kind == 4) {  // shotgun shell (MC_shotgun_shell, nose first) with a hot glow
                if (shellModel_) drawModel(shellModel_, Mat4::translation(r.pos) * Mat4::rotation(rotationBetween({0, 0, -1}, r.vel)) * Mat4::scale({0.6f, 0.6f, 0.6f}), false);
                if (boltModel_) drawModel(boltModel_, Mat4::translation(r.pos) * Mat4::scale({1.0f, 1.0f, 1.0f}), true, 0.7f);
                continue;
            }
            // The rocket model points along -Y (game) = -Z (runtime): orient it along its flight.
            const Vec3 f = normalize(r.vel);
            const Quat yawQ = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
            const Quat look = yawQ * Quat::fromAxisAngle({1, 0, 0}, std::asin(std::max(-1.0f, std::min(1.0f, f.y))));
            drawModel(r.kind == 10 && proxRocketModel_ ? proxRocketModel_ : rocketModel_, Mat4::translation(r.pos) * Mat4::rotation(look) * Mat4::scale({0.6f, 0.6f, 0.6f}), false);
        }
    for (const auto& c : chars_) {
        if (!c.visible || length(c.pos - v.eyePosition) > 160.0f || (c.rider && firstPerson_)) continue;
        if (cut && std::find(cutTypes.begin(), cutTypes.end(), c.type) != cutTypes.end()) continue;
        c.model->render(renderer_, c.xf(), Vec4{1.0f + 2.2f * c.flash, 1.0f + 0.9f * c.flash, 1.0f + 0.5f * c.flash, 1.0f});
    }
    renderBurrows(v.eyePosition);
    renderShop();
    renderBase(v.eyePosition);
    renderPopup();
    renderGrenades();
    renderMines();
    renderSkirmish(false, v.eyePosition);
    renderSpells();
    renderKabutoPowers(v.eyePosition);
    renderThrows();
    renderKabutoCarry();
    renderMap();
    for (const auto& sp : creatures_.spits) {
        const int k = int(sp.size);
        if (k > 0 && spitBall_[k])
            spitBall_[k]->renderGlow(renderer_, Mat4::translation(sp.pos - spitCentre_[k]) *
                                                    Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, sp.age * 3.0f)), 1.3f);
    }
    for (const auto& b : blasts_) {
        // Burst: the flash plus the splash ring (ripspit / ripspitbig) growing and fading.
        const float f = b.age / 0.6f;
        const int k = b.size <= 2 ? b.size : 0;  // 3 = a Ripper bursting: flash only, no spit splash ring
        const float big = b.size >= 2 ? 1.5f : 1.0f;
        if (impactModel_) {
            // (2026-10-28) Faded out as it would surround the viewer (a Ripper bursting at your feet), as the fireballs.
            const float size = 0.3f * big * (4.0f + 8.0f * f), eyeD = length(b.pos - v.eyePosition);
            const float near = std::min(1.0f, std::max(0.0f, (eyeD - 0.5f * size) / (0.7f * size)));
            if (near > 0.01f) {
                drawModel(impactModel_, Mat4::translation(b.pos) * Mat4::rotation(headWorldRot_) *
                                            Mat4::scale(Vec3{1, 1, 1} * big * (4.0f + 8.0f * f)), true, (1.0f - f) * near);
                if (fxCostView_) fxCover_[kCostFlash] += coverOf(size, eyeD);
            }
        }
        if (k > 0 && spitRing_[k])
            drawModel(spitRing_[k], Mat4::translation(b.pos + Vec3{0, 0.1f, 0}) * Mat4::scale(Vec3{1, 1, 1} * (2.0f + 10.0f * f) * big),
                      true, 1.0f - f);
    }
    float waterCover = 0;  // (2026-10-28) at most ~2.5 screens of crowns and rings a frame: newest left out first
    for (const auto& f : waterFx_) {
        SkinnedModel* m = f.kind == 0 ? splashFx_.get() : ringFx_.get();
        if (!m) continue;
        if (f.kind == 0) {
            // The splash crown (its clip doesn't move it): opens out, leaps up and falls back in.
            const float k = f.age / 0.9f;
            if (k >= 1.0f) continue;
            const float wide = f.scale * (0.45f + 0.85f * std::sqrt(k));
            const float tall = f.scale * 1.25f * std::sin(3.14159f * std::min(1.0f, k * 1.1f));
            if (tall < 0.01f) continue;
            // Not with the eye inside the crown (a full view of translucent sheets: the jet-ski spray).
            if (length(Vec3{f.pos.x - v.eyePosition.x, 0, f.pos.z - v.eyePosition.z}) < wide * 1.3f && v.eyePosition.y < f.pos.y + tall * 1.5f) continue;
            // (2026-10-28) Nor right at the viewer's feet (piranhas at your legs, rounds into the water beside you):
            // seen from above, its sheets filled the view in blue streaks; the drops and spray still show it.
            if (length(Vec3{f.pos.x - v.eyePosition.x, 0, f.pos.z - v.eyePosition.z}) < 1.6f * rig_.comfort.worldScale + wide) continue;
            const float cw = coverOf(2.0f * std::max(wide, tall), length(f.pos - v.eyePosition));
            if (waterCover + cw > 2.5f && cw > 0.05f) continue;
            waterCover += cw;
            m->pose(renderer_, 0.0f);
            if (fxCostView_) fxCover_[kCostSpell] += cw;
            const float spin = std::fmod(f.pos.x * 7.3f + f.pos.z * 3.1f, 6.28f);
            m->render(renderer_, Mat4::translation(f.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, spin + k * 0.6f)) *
                                     Mat4::scale(Vec3{wide, tall, wide}));
            continue;
        }
        m->pose(renderer_, std::min(f.age, m->animDuration() * 0.999f));
        if (length(f.pos - v.eyePosition) < 0.6f * f.scale) continue;  // (a ring seen from inside its own spray)
        const float cr = coverOf(1.5f * f.scale, length(f.pos - v.eyePosition));
        if (waterCover + cr > 2.5f && cr > 0.05f) continue;
        waterCover += cr;
        if (fxCostView_) fxCover_[kCostSpell] += cr;
        m->render(renderer_, Mat4::translation(f.pos) * Mat4::scale(Vec3{1, 1, 1} * f.scale));
    }
    // (the swimming piranhas: drawn under the sea, AssetWorld::underwater -> renderPiranhas)
    renderGore();
    for (const auto& c : collapses_)
        if (c.model) c.model->render(renderer_, c.xf);
    renderPickups();
    renderHealthBars();
    renderVillagers(false, renderer_.currentView().eyePosition, 90.0f);
    renderVimps(false, renderer_.currentView().eyePosition, 150.0f);
    renderBirds(renderer_.currentView().eyePosition);
    renderHitchers(renderer_.currentView().eyePosition);
    renderJetski(false, renderer_.currentView().eyePosition);
    renderCasings();
    for (const auto& g : gibs_)
        drawModel(g.model, Mat4::translation(g.pos) * Mat4::rotation(Quat::fromAxisAngle(g.axis, g.angle)) * g.base *
                               Mat4::scale({g.scale, g.scale, g.scale}),
                  false);
    for (const auto& e : enemies_.enemies)
        if (e.slot >= 0) {
            // A hit flashes the body hot for a moment (rework: you see every hit land).
            const float f = e.hitFlash;
            enemyPool_[size_t(e.slot)]->render(renderer_, enemyXf(e),
                                               Vec4{1.0f + 2.2f * f, 1.0f + 0.9f * f, 1.0f + 0.5f * f, 1.0f});
        }
    renderReaperWeapons(false, renderer_.currentView().eyePosition, 150.0f);
    // Reaper bolts: the spark in its own white (as the original's sea Reapermen fire).
    for (const auto& p : enemyFire_.projectiles()) {
        const Quat r = rotationBetween({0, 0, -1}, p.vel);
        for (const auto& part : boltModel_ ? boltModel_->parts : std::vector<RenderModel::Part>{}) {
            DrawParams dp;
            dp.texture = part.texture;
            dp.blend = BlendMode::Additive;
            dp.unlit = true;
            dp.depthWrite = false;
            dp.doubleSided = true;
            dp.tint = {2.2f, 2.2f, 2.2f, 1.0f};
            renderer_.draw(part.mesh, Mat4::translation(p.pos) * Mat4::rotation(r) * Mat4::scale({0.6f, 0.6f, 0.6f}), dp);
        }
    }
    for (const auto& e : enemyFire_.effects())
        if (e.kind == WeaponEffect::Impact && renderer_.look() != 1)
            drawModel(impactModel_, Mat4::translation(e.pos) * Mat4::rotation(headWorldRot_) *
                                        Mat4::scale(Vec3{1, 1, 1} * (2.0f + 3.0f * e.progress())), true, 1.0f - e.progress());
    // The Meccaryn's pistol: small pink bolts (the original's), the spark quads facing along the
    // flight with a short fading trail.
    for (const auto& p : weapon_.projectiles()) {
        const Quat r = rotationBetween({0, 0, -1}, p.vel);
        if (p.style == 2 && sniperShotModel_) {  // the sniper's round: its own model, stretched along its flight
            drawModel(sniperShotModel_, Mat4::translation(p.pos) * Mat4::rotation(r) * Mat4::scale({0.6f, 0.6f, 2.5f}), true, 1.0f);
            continue;
        }
        if ((p.style == 3 || p.style == 4) && arrowModel_) {  // an arrow: the rparrow streak (4: burning)
            drawModel(arrowModel_, Mat4::translation(p.pos) * Mat4::rotation(r) * Mat4::scale({0.2f, 0.2f, 0.2f}), true, 0.9f);
            continue;
        }
        const Vec3 back = normalize(p.vel) * -0.22f;
        const bool mg = p.style == 1;  // machine-gun tracers: smaller and hotter
        for (int i = 0; i < 3; ++i) {
            const float s = (0.32f - 0.07f * float(i)) * (mg ? 0.7f : 1.0f);
            if (p.travelled < 0.22f * float(i)) break;  // no trail behind the muzzle
            const float a = 2.6f - 0.8f * float(i);
            for (const auto& part : boltModel_ ? boltModel_->parts : std::vector<RenderModel::Part>{}) {
                DrawParams dp;
                dp.texture = part.texture;
                dp.blend = BlendMode::Additive;
                dp.unlit = true;
                dp.depthWrite = false;
                dp.doubleSided = true;
                dp.tint = mg ? Vec4{1.0f * a, 0.7f * a, 0.25f * a, 1.0f} : Vec4{1.0f * a, 0.35f * a, 0.75f * a, 1.0f};
                renderer_.draw(part.mesh, Mat4::translation(p.pos + back * float(i)) * Mat4::rotation(r) * Mat4::scale({s, s, s}), dp);
            }
        }
    }
    // Third-person crosshair: original bracket texture at the aim point, constant angular size.
    if (playMode_ && !firstPerson_ && crosshairTex_ && impactModel_ && lastInput_.hand[rig_.turnHand()].aimPoseValid) {
        const Vec3 eye = renderer_.currentView().eyePosition;
        const Vec3 toEye = eye - aimTarget_;
        const float dist = length(toEye);
        const float s = std::max(0.45f, dist * 0.33f);  // impact quad is 0.2 m: ~3.8 deg
        const Vec3 p = aimTarget_ + toEye * (std::min(0.5f, dist * 0.05f) / std::max(dist, 1e-3f));
        for (const auto& part : impactModel_->parts) {
            DrawParams dp;
            dp.texture = crosshairTex_;
            dp.blend = BlendMode::AlphaBlend;
            dp.unlit = true;
            dp.depthWrite = false;
            dp.doubleSided = true;
            dp.noFog = true;
            renderer_.draw(part.mesh, Mat4::translation(p) * Mat4::rotation(headWorldRot_) * Mat4::scale({s, s, s}), dp);
        }
    }
    for (const auto& e : weapon_.effects()) {
        const float k = e.progress();
        if (fxCostView_ && (e.kind == WeaponEffect::Muzzle || renderer_.look() != 1))
            fxCover_[kCostFlash] += coverOf(e.kind == WeaponEffect::Muzzle ? 0.6f * (0.25f + 0.15f * k) : 1.2f * (3.0f + 4.0f * k), length(e.pos - v.eyePosition));
        if (e.kind == WeaponEffect::Muzzle && e.style == 2 && sniperFlashModel_)
            drawModel(sniperFlashModel_, Mat4::translation(e.pos) * Mat4::rotation(rotationBetween({0, 0, -1}, e.dir)) *
                                             Mat4::scale(Vec3{1, 1, 1} * (0.5f + 0.4f * k)), true, 1.0f - k);
        else if (e.kind == WeaponEffect::Muzzle)
            drawModel(muzzleModel_, Mat4::translation(e.pos) * Mat4::rotation(rotationBetween({0, 0, -1}, e.dir)) *
                                        Mat4::scale(Vec3{1, 1, 1} * (0.25f + 0.15f * k)), true, 1.0f - k);
        else if (renderer_.look() != 1)  // impact flash: billboard toward the viewer (rework: a soft glow and sparks, addSparks)
            drawModel(impactModel_, Mat4::translation(e.pos) * Mat4::rotation(headWorldRot_) *
                                        Mat4::scale(Vec3{1, 1, 1} * (3.0f + 4.0f * k)), true, 1.0f - k);
    }
    renderFx();
    renderDataFx();
    if (playMode_) renderGrass();
    if (playMode_ && !cut) renderMotes();
    if (!cut) renderWeather();
    if (!cut) renderAirBubbles();
    if (!cut) renderPlankton();
    renderFireballs();
    renderSwordTrail();
}

// n = 1: one eye into the bound framebuffer; n = 2: both eyes at once into a two-layer (multiview)
// framebuffer. The game's own distance checks then use the point between the eyes.
void GameApp::logFxCost(float dt) {
    // Called once a frame after the views: keeps the interval's worst frame per kind, then starts over.
    static const bool on = std::getenv("GVR_FX_COST") != nullptr;
    for (int k = 0; k < kCostCount; ++k) { fxCoverMax_[k] = std::max(fxCoverMax_[k], fxCover_[k]); fxCover_[k] = 0; }
    fxCoverT_ += dt;
    if (fxCoverT_ < 0.5f) return;
    fxCoverT_ = 0;
    float tot = 0;
    for (float c : fxCoverMax_) tot += c;
    if (on && tot > 0.05f)
        LOGI("fx cost (screens, worst frame of 0.5 s): smoke %.2f blood %.2f fireball %.2f embers %.2f streaks %.2f flash %.2f water %.2f, total %.2f",
             fxCoverMax_[0], fxCoverMax_[1], fxCoverMax_[2], fxCoverMax_[3], fxCoverMax_[4], fxCoverMax_[5], fxCoverMax_[6], tot);
    for (float& c : fxCoverMax_) c = 0;
}

void GameApp::renderViews(const EyeRenderInfo* eyes, int n) {
    for (int i = 0; i < n; ++i) {
        // Remember the eyes' fields of view (the reflection camera covers both).
        const EyeRenderInfo& eye = eyes[i];
        const int k = eye.fovLeft < -eye.fovRight ? 0 : 1;
        eyeFov_[k][0] = eye.fovLeft; eyeFov_[k][1] = eye.fovRight; eyeFov_[k][2] = eye.fovUp; eyeFov_[k][3] = eye.fovDown;
        if (k == 1) haveFov_ = true;
        if (!haveFov_) { eyeFov_[1][0] = eye.fovLeft; eyeFov_[1][1] = eye.fovRight; eyeFov_[1][2] = eye.fovUp; eyeFov_[1][3] = eye.fovDown; }
    }
    if (load_.phase == LoadPhase::Brief && load_.t > 0.3f && load_.t < 5.0f && assetWorld_ && assetWorld_->loaded() && !warmedThisFrame_) {
        // Warm-up behind the briefing (first eye only): draw the new level from where the player
        // starts, a different direction each frame, so the driver's first-use work on its textures
        // and shaders happens now - on the Quest the first 3-4 s of play ran at 33-45 fps without it.
        const float yaw = float(warmFrame_ % 8) * 0.785398f;
        Pose wp;
        wp.position = assetWorld_->spawnPoint() + Vec3{0, 1.7f * heroScale_, 0};
        wp.orientation = Quat::fromAxisAngle({0, 1, 0}, yaw) * Quat::fromAxisAngle({1, 0, 0}, (warmFrame_ / 8) % 2 ? -0.35f : 0.0f);
        beginEyeViews(eyes, n, &wp, true);
        assetWorld_->render(renderer_);
        for (const auto& c : chars_)
            if (c.visible && c.model && length(c.pos - wp.position) < 160.0f) c.model->render(renderer_, c.xf());
        renderer_.endView();
        warmedThisFrame_ = true;
        if (++warmFrame_ == 16) LOGI("Warm-up: the level drawn from the start in 16 directions");
    }
    if (load_.phase != LoadPhase::Brief) warmFrame_ = 0;
    {
        // The cinema room is dark (the world is on its screen).
        const bool room = cinemaActive() && load_.phase == LoadPhase::Idle;
        const Vec3 cc = renderer_.environment().clearColor;
        if (room) renderer_.environment().clearColor = Vec3{0.012f, 0.012f, 0.016f};
        beginEyeViews(eyes, n, nullptr, true);
        if (room) renderer_.environment().clearColor = cc;
    }
    const ViewParams v = renderer_.currentView();  // (stereo: the point between the eyes)
    if (load_.phase != LoadPhase::Idle && load_.phase != LoadPhase::FadeOut) {
        renderLoading();  // the briefing on black, nothing else
        renderer_.endView();
        return;
    }
    const bool cut = cutsceneActive();
    std::vector<int> cutTypes;
    if (cut) cutTypes = cutscene_->actorTypes();
    const bool cinema = cut && cinemaActive();
    if (cinema) renderCinemaRoom(v);  // the world is on the screen (renderCinema, before the eyes)
    else renderWorld(v, cut, cutTypes);
    if (title_) renderTitle();
    beginUiFrame(eyes, n);  // the HUD, menu and hands at human scale (Kabuto)
    if (cut) renderCutscene(!cinema);
    else if (playMode_) renderHud();
    if (playMode_ && !cut) renderTags();
    if (playMode_ && !cut && !title_) renderObjectiveMarker();
    if (playMode_ && !cut) renderTimmyLine();
    if (playMode_ && !cut && !title_) renderWristPanel();
    if (playMode_ && !cut && !title_) renderGunCounter();
    if (playMode_ && !cut && !title_) renderDelphiWheel();
    if (playMode_ && !cut && !title_) renderTeleportArc();
    if (playMode_ && !cut) renderSkirmishHud();
    if (playMode_ && !cut) renderRaceHud();
    if (briefHeld_ && !cut && playMode_) renderBriefOverlay();
    if (menuOpen_ && !cut) renderMenu();
    if (showTestScene_ || !hasOriginalAssets()) testScene_.render(renderer_, rig_, lastInput_);
    else if (menuOpen_ || title_ || !playMode_ || briefHeld_) testScene_.renderPointers(renderer_, rig_, lastInput_);  // (in play: your hero's own hands)
    if (load_.phase == LoadPhase::FadeOut) renderLoading();
    renderVignette();
    endUiFrame();
    fxCostView_ = false;
    // Death: fade to black before the respawn, and back in after it.
    const float black = std::max(menuOpen_ ? 0.0f : telFade_, dying_ ? std::min(1.0f, std::max(0.0f, (deathTime_ - 4.3f) / 1.0f)) : respawnFade_);
    if (black > 0.01f && text_.ready()) {
        const Vec3 fwd = headWorldRot_.rotate(Vec3{0, 0, -1});
        text_.drawPanel(Mat4::translation(v.eyePosition + fwd * 0.1f) * Mat4::rotation(headWorldRot_), 1.0f, 1.0f, {0, 0, 0, black});
    }
    renderer_.endView();
}

void GameApp::renderPiranhas() {
    const ViewParams& v = renderer_.currentView();
    if (pirSwim_) {
        // Circling just under the surface; one now and then leaps (pir_swim_jump) at the prey.
        if (pirPosed_ != time_) { pirPosed_ = time_; pirSwim_->pose(renderer_, float(time_)); }  // (once a frame: this runs in every world pass)
        for (const auto& sc : schools_) {
            if (sc.vis <= 0.01f || length(sc.centre - v.eyePosition) > 120.0f) continue;
            for (int i = 0; i < kPiranhas; ++i) {
                const Piranha& f = sc.fish[i];
                const float r = (sc.target < 0 ? 1.2f : 0.8f) + 0.35f * float(i % 3);
                const Vec3 at = sc.centre + Vec3{std::cos(f.angle) * r, 0, std::sin(f.angle) * r};
                const float yaw = std::atan2(std::sin(f.angle), -std::cos(f.angle));  // swimming along the circle
                const float depth = -0.25f - (1.0f - sc.vis) * 1.5f;
                if (f.jump >= 0 && pirJump_) {
                    pirJump_->pose(renderer_, f.jump);
                    const Vec3 to = sc.centre - at;
                    pirJump_->render(renderer_, Mat4::translation(Vec3{at.x, depth, at.z}) *
                                                    Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, std::atan2(-to.x, -to.z))));
                } else {
                    pirSwim_->render(renderer_, Mat4::translation(Vec3{at.x, depth, at.z}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, yaw)));
                }
            }
        }
    }
}

}  // namespace gvr
