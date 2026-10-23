// GameApp: the HUD (original Meccaryn HUD textures) and the health injections. Split from
// game_app.cpp; same class.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/arm_ik.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

// ---------------------------------------------------------------------------------------------
// HUD

void GameApp::initHud(ModelCache* mc) {
    if (hudReady_) return;
    // Layer -> texture (drawn in layer order, over everything).
    const char* names[kHudLayers] = {nullptr,          "st_mec_hbar",   "st_mec_fuel",     "st_mec_energy",
                                     "st_mec_jetpack", "st_mec_wpslot", "st_mec_gun",      "st_mec_launcher",
                                     "st_mec_syringe", "st_mec_numbers", nullptr};
    const size_t caps[kHudLayers] = {8, 160, 160, 4, 4, 8, 8, 8, 8, 16, 48};
    for (int k = 0; k < kHudLayers; ++k) {
        hudTex_[k] = names[k] ? mc->texture(names[k]) : whiteTex_;
        if (!hudTex_[k]) hudTex_[k] = whiteTex_;
        hudBatch_[k].init(renderer_, caps[k]);
    }
    // The gauges' insides, measured from the outline art: the heart's lobes join below the cusp
    // (seed in the middle), the jetpack arrow's head opens into its stem (seed in the stem).
    Image img;
    if (mc->image("st_mec_energy", img)) heartShape_ = gaugeInterior(img, img.width / 2, img.height / 2);
    if (mc->image("st_mec_jetpack", img)) jetShape_ = gaugeInterior(img, img.width * 31 / 64, img.height / 2);
    LOGI("HUD: heart gauge %zu spans, jetpack gauge %zu spans", heartShape_.size(), jetShape_.size());
    hudReady_ = true;
}

void GameApp::hudRect(int layer, const HudFrame& f, float x, float y, float w, float h, const uint8_t rgba[4], float u0, float v0,
                      float u1, float v1) {
    const Vec3 c = f.o + f.r * x + f.u * y, hr = f.r * (0.5f * w), hu = f.u * (0.5f * h);
    const Vec3 q[4] = {c - hr - hu, c + hr - hu, c + hr + hu, c - hr + hu};
    const float uv[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
    hudBatch_[layer].add(q, uv, rgba);
}

// A gauge filled from the bottom to `level` (0..1): the inside spans below the level with the
// fill texture's lit half, then the outline over it.
void GameApp::hudGauge(int fillLayer, int outlineLayer, const std::vector<GaugeSpan>& shape, const HudFrame& f, float x, float y,
                       float w, float h, float level, const uint8_t fill[4], const uint8_t outline[4]) {
    if (!shape.empty() && level > 0.0f) {
        float top = 1.0f, bottom = 0.0f;
        for (const auto& s : shape) { top = std::min(top, s.v0); bottom = std::max(bottom, s.v1); }
        const float cut = bottom - std::min(1.0f, level) * (bottom - top);
        for (const auto& s : shape) {
            if (s.v1 <= cut) continue;
            const float v0 = std::max(s.v0, cut), v1 = s.v1;
            const float cy = y + (0.5f - 0.5f * (v0 + v1)) * h, cx = x + (0.5f * (s.x0 + s.x1) - 0.5f) * w;
            // The fill textures: transparent / dark top half, the coloured gradient below.
            const float t0 = (v0 - top) / (bottom - top), t1 = (v1 - top) / (bottom - top);
            hudRect(fillLayer, f, cx, cy, (s.x1 - s.x0) * w, (v1 - v0) * h, fill, 0.2f, 0.52f + 0.46f * t0, 0.8f,
                    0.52f + 0.46f * t1);
        }
    }
    hudRect(outlineLayer, f, x, y, w, h, outline);
}

void GameApp::hudDigits(const HudFrame& f, float x, float y, float h, int value, const uint8_t rgba[4]) {
    // st_mec_numbers: "0123456789" in ten equal cells across the strip (left-aligned at x).
    const std::string s = std::to_string(std::max(0, value));
    const float w = h * 0.55f;
    for (size_t i = 0; i < s.size(); ++i) {
        const int d = s[i] - '0';
        hudRect(kHudNumbers, f, x + w * (float(i) + 0.5f), y, w, h, rgba, float(d) / 10.0f, 0.0f, float(d + 1) / 10.0f, 1.0f);
    }
}

// The status panel, laid out in metres (FP size; TP scales the frame).
void GameApp::hudStatus(const HudFrame& f) {
    const float t = float(time_);
    const float hp = std::max(0.0f, playerHealth_) / 100.0f;
    const uint8_t plate[4] = {0, 0, 0, 80};
    hudRect(kHudPlate, f, 0.006f, 0.006f, 0.118f, 0.122f, plate);
    // Health: the heart, pulsing red when low, flashing green while an injection heals.
    const bool low = hp < 0.3f;
    const float pulse = low ? 0.5f + 0.5f * std::sin(t * 9.0f) : 0.0f;
    uint8_t fill[4] = {255, 255, 255, 235};
    if (healFlash_ > 0) { fill[0] = uint8_t(255 - 180 * std::min(1.0f, healFlash_)); fill[2] = fill[0]; }
    uint8_t heart[4] = {255, uint8_t(255 - 150 * pulse), uint8_t(255 - 150 * pulse), 255};
    hudGauge(kHudHealthFill, kHudEnergy, heartShape_, f, -0.034f, 0.0f, 0.030f, 0.084f, hp, fill, heart);
    // Jetpack fuel (once he has it): flashes red while overheated.
    if (player_.jetpackAvailable) {
        uint8_t fuel[4] = {255, 255, 255, 235};
        if (player_.overheated()) { const bool on = std::fmod(t, 0.4f) < 0.2f; fuel[1] = fuel[2] = on ? 60 : 255; }
        const uint8_t outline[4] = {255, 255, 255, 255};
        hudGauge(kHudFuelFill, kHudJetGauge, jetShape_, f, -0.002f, 0.0f, 0.030f, 0.084f, player_.fuel(), fuel, outline);
    }
    // Injections carried: the syringe icon and the count (pops when one is picked up).
    const float pop = 1.0f + 0.5f * std::max(0.0f, pickupFlash_);
    const uint8_t on[4] = {255, 255, 255, 255}, dim[4] = {255, 255, 255, 90};
    hudRect(kHudSyringe, f, 0.032f, 0.018f, 0.030f * pop, 0.030f * pop, syringes_ > 0 ? on : dim);
    const uint8_t num[4] = {255, 255, 255, uint8_t(syringes_ > 0 ? 255 : 110)};
    hudDigits(f, 0.046f, 0.012f, 0.018f, syringes_, num);
    // The weapon in hand.
    const bool launcher = weaponSel_ == 1 && launcherModel_;
    hudRect(launcher ? kHudLauncher : kHudGun, f, 0.040f, -0.024f, 0.036f, 0.036f, on);
    // Mission objectives along the top: one pip each, gold when done.
    const size_t n = objectives_.size();
    for (size_t i = 0; i < n && n <= 16; ++i) {
        const float px = 0.006f + (float(i) - 0.5f * float(n - 1)) * 0.012f;
        const uint8_t back[4] = {0, 0, 0, 150};
        hudRect(kHudMarks, f, px, 0.058f, 0.009f, 0.009f, back);
        const uint8_t gold[4] = {255, 205, 25, 245}, grey[4] = {128, 128, 128, 160};
        hudRect(kHudMarks, f, px, 0.058f, 0.006f, 0.006f, objectives_[i].done ? gold : grey);
    }
}

// The weapon strip (st_mec_wpslot, four slots) that drops in at the top of the view when the
// weapon is switched, the chosen slot popping, then fades: as the original's weapon select.
void GameApp::hudWeaponStrip() {
    const float t = weaponHudT_;
    if (t > 1.9f || !subInit_) return;
    const float in = std::min(1.0f, t / 0.15f), out = t > 1.4f ? std::max(0.0f, 1.0f - (t - 1.4f) / 0.5f) : 1.0f;
    const float a = in * out;
    const float drop = 0.035f * (1.0f - in) * (1.0f - in);
    HudFrame f;
    f.o = rig_.toWorld(lastInput_.head).position + subRot_.rotate(Vec3{0, 0.17f + drop, -0.9f});
    f.r = subRot_.rotate(Vec3{1, 0, 0});
    f.u = subRot_.rotate(Vec3{0, 1, 0});
    const float W = 0.30f * settings_.hudScale(), H = W / 4.0f;
    const uint8_t back[4] = {0, 0, 0, uint8_t(120 * a)}, strip[4] = {255, 255, 255, uint8_t(255 * a)};
    hudRect(kHudPlate, f, 0, 0, W + 0.01f, H + 0.01f, back);
    hudRect(kHudWpSlot, f, 0, 0, W, H, strip);
    const int sel = weaponSel_ == 1 && launcherModel_ ? 1 : 0;
    const float popK = 1.0f + 0.35f * std::max(0.0f, 1.0f - t / 0.3f);
    for (int i = 0; i < 4; ++i) {
        const float cx = -0.5f * W + (float(i) + 0.5f) * (W / 4.0f);
        int layer = -1;
        bool owned = false;
        if (i == 0) { layer = kHudGun; owned = true; }
        if (i == 1) { layer = kHudLauncher; owned = hasLauncher_; }
        if (i == 2) { layer = kHudSyringe; owned = syringes_ > 0; }
        if (layer < 0 || !owned) continue;
        const bool chosen = i == sel;
        if (chosen) {
            const uint8_t glow[4] = {140, 255, 140, uint8_t(110 * a)};
            hudRect(kHudMarks, f, cx, 0, W / 4.0f - 0.006f, H - 0.006f, glow);
        }
        const float s = H * 0.82f * (chosen ? popK : 0.85f);
        const uint8_t c[4] = {255, 255, 255, uint8_t((chosen ? 255 : 130) * a)};
        hudRect(layer, f, cx + 0.004f, 0.004f, s, s, c);
        if (i == 2) {
            const uint8_t nc[4] = {255, 255, 255, uint8_t(255 * a)};
            hudDigits(f, cx - W / 8.0f + 0.006f, -H * 0.33f, H * 0.3f, syringes_, nc);
        }
    }
}

// A red wedge on the side the last hit came from, around the middle of the view.
void GameApp::hudHitMarker() {
    if (hitDirTimer_ <= 0 || !lastInput_.headValid) return;
    const Pose head = rig_.toWorld(lastInput_.head);
    const Vec3 fwd = head.orientation.rotate(Vec3{0, 0, -1}), right = head.orientation.rotate(Vec3{1, 0, 0}),
               up = head.orientation.rotate(Vec3{0, 1, 0});
    const Vec3 d = hitFrom_ - head.position;
    const float ang = std::atan2(dot(d, right), dot(d, fwd));  // 0 = ahead, +pi/2 = right
    const Vec3 dir = right * std::sin(ang) + up * std::cos(ang), side = right * std::cos(ang) - up * std::sin(ang);
    const Vec3 c = head.position + fwd * 0.6f;
    const float r0 = 0.10f, r1 = 0.15f, w0 = 0.010f, w1 = 0.032f;
    const Vec3 q[4] = {c + dir * r0 - side * w0, c + dir * r0 + side * w0, c + dir * r1 + side * w1, c + dir * r1 - side * w1};
    const uint8_t red[4] = {255, 20, 0, uint8_t(250 * std::min(1.0f, hitDirTimer_))};
    hudBatch_[kHudMarks].add(q, red);
}

void GameApp::buildHud() {
    if (!hudReady_) return;
    for (auto& b : hudBatch_) b.clear();
    const bool show = playMode_ && !cutsceneActive() && !dying_ && load_.phase == LoadPhase::Idle && !menuOpen_;
    if (show) {
        const ControllerState& lc = lastInput_.hand[rig_.moveHand()];
        if (firstPerson_ && lc.gripPoseValid) {
            // Floating over the move-hand forearm (behind the hand, so the fingers don't cover
            // it), facing the eyes: raise the wrist to read it.
            const Pose g = rig_.toWorld(lc.gripPose);
            HudFrame f;
            f.o = g.position + g.orientation.rotate(Vec3{0, 0.02f, 0.07f}) + Vec3{0, 0.10f, 0};
            f.r = headWorldRot_.rotate(Vec3{settings_.hudScale(), 0, 0});
            f.u = headWorldRot_.rotate(Vec3{0, settings_.hudScale(), 0});
            hudStatus(f);
        } else if (!firstPerson_ && subInit_) {
            // Third person: head-locked at the lower left of the view.
            const float k = 2.2f * settings_.hudScale();
            HudFrame f;
            f.o = rig_.toWorld(lastInput_.head).position + subRot_.rotate(Vec3{-0.33f, -0.24f, -0.9f});
            f.r = subRot_.rotate(Vec3{k, 0, 0});
            f.u = subRot_.rotate(Vec3{0, k, 0});
            hudStatus(f);
        }
        hudWeaponStrip();
        hudHitMarker();
    }
    if (settings_.showFps && subInit_ && lastInput_.headValid) {
        // Frame rate (smoothed), small, at the top left of the view.
        HudFrame f;
        f.o = rig_.toWorld(lastInput_.head).position + subRot_.rotate(Vec3{-0.3f, 0.26f, -0.9f});
        f.r = subRot_.rotate(Vec3{1, 0, 0});
        f.u = subRot_.rotate(Vec3{0, 1, 0});
        const int fps = int(fpsSmooth_ + 0.5f);
        const uint8_t col[4] = {uint8_t(fps >= 70 ? 120 : 255), uint8_t(fps >= 70 ? 255 : (fps >= 60 ? 220 : 80)), 120, 230};
        const uint8_t back[4] = {0, 0, 0, 150};
        hudRect(kHudPlate, f, 0.035f, 0, 0.085f, 0.06f, back);
        hudDigits(f, 0, 0, 0.05f, fps, col);
    }
    for (auto& b : hudBatch_) b.upload(renderer_);
}

// Draws the HUD batches (built once per frame in buildHud) and the full-view flashes.
void GameApp::renderHud() {
    if (hudReady_) {
        DrawParams dp;
        dp.blend = BlendMode::AlphaBlend;
        dp.unlit = true;
        dp.noFog = true;
        dp.depthWrite = false;
        dp.depthTest = false;
        dp.doubleSided = true;
        for (int k = 0; k < kHudLayers; ++k) {
            dp.texture = hudTex_[k];
            hudBatch_[k].draw(renderer_, dp);
        }
    }
    if (!impactModel_ || impactModel_->parts.empty() || !whiteTex_) return;
    const MeshId quad = impactModel_->parts[0].mesh;  // 0.2 m square in the XY plane
    auto flash = [&](const Vec4& color) {
        DrawParams dp;
        dp.texture = whiteTex_;
        dp.blend = BlendMode::AlphaBlend;
        dp.unlit = true;
        dp.noFog = true;
        dp.depthWrite = false;
        dp.depthTest = false;
        dp.doubleSided = true;
        dp.tint = color;
        const Vec3 fwd = headWorldRot_.rotate(Vec3{0, 0, -1});
        const Vec3 eye = renderer_.currentView().eyePosition;
        renderer_.draw(quad, Mat4::translation(eye + fwd * 0.12f) * Mat4::rotation(headWorldRot_) * Mat4::scale({4, 4, 1}), dp);
    };
    if (damageFlash_ > 0) flash({1.0f, 0.05f, 0.0f, std::min(0.5f, 0.35f * damageFlash_)});
    if (healFlash_ > 0) flash({0.2f, 1.0f, 0.3f, std::min(0.2f, 0.18f * healFlash_)});
}

// ---------------------------------------------------------------------------------------------
// Health injections

void GameApp::initPickups(ModelCache* mc) {
    healthPickups_.clear();
    enemyDead_.clear();
    syringeOut_ = false;
    injectT_ = -1;
    healPending_ = 0;
    pickupModel_ = mc->get("MC_pickup_health.gbs", "", /*dynamicLit=*/true);
    syringeModel_ = mc->get("mc_syringe.gbs", "", /*dynamicLit=*/true);
    if (pickupModel_) {
        const Vec3 s = pickupModel_->boundsMax - pickupModel_->boundsMin;
        const float big = std::max(s.x, std::max(s.y, s.z));
        pickupScale_ = big > 1e-3f ? 0.7f / big : 1.0f;  // about 70 cm: easy to spot
        LOGI("Health pickup model %.2f x %.2f x %.2f m", s.x, s.y, s.z);
    }
    if (syringeModel_) {
        // Its long axis is the needle's; the needle end is the thinner one (fewer vertices there
        // is not known here, so the direction is fixed by the measured model: see the log).
        const Vec3 mn = syringeModel_->boundsMin, mx = syringeModel_->boundsMax, s = mx - mn;
        syringeCentre_ = (mn + mx) * 0.5f;
        if (s.x >= s.y && s.x >= s.z) syringeAxis_ = {1, 0, 0};
        else if (s.y >= s.z) syringeAxis_ = {0, 1, 0};
        else syringeAxis_ = {0, 0, 1};
        const float len = std::max(s.x, std::max(s.y, s.z));
        syringeScale_ = len > 1e-3f ? 0.2f / len : 1.0f;  // a 20 cm syringe in the hand
        LOGI("Syringe model %.2f x %.2f x %.2f m", s.x, s.y, s.z);
    }
    if (!assetWorld_) return;
    auto& objs = assetWorld_->worldObjects();
    if (const char* dump = std::getenv("GVR_DUMP_OBJECTS"))  // test hook: list objects whose name contains this
        for (const auto& o : objs)
            if (o.name.find(dump) != std::string::npos)
                LOGI("object %s at game (%.0f, %.0f, %.0f) r %.1f m yaw-ish %.2f", o.name.c_str(), -o.placement.m[12] / gGameUnitsToMeters,
                     o.placement.m[14] / gGameUnitsToMeters, o.placement.m[13] / gGameUnitsToMeters, o.radius,
                     std::atan2(o.placement.m[8], o.placement.m[10]));
    // Placed health pickups (object class 37, mc_pickup_health) become live pickups.
    const ObjectRegistry& reg = assetWorld_->registry();
    for (auto& o : objs) {
        const ObjectType* t = reg.type(o.type);
        const bool health = o.name.find("pickup_health") != std::string::npos || (t && t->objectClass == 37);
        if (!health || o.hidden) continue;
        o.hidden = true;
        if (o.collision >= 0) assetWorld_->collisionMutable().setEnabled(o.collision, false);
        Vec3 at{o.placement.m[12], o.placement.m[13], o.placement.m[14]};
        float gy;
        if (assetWorld_->terrain().heightAt(at.x, at.z, gy)) at.y = std::max(at.y, gy);
        healthPickups_.push_back({at, 0});
        LOGI("Health pickup at game (%.0f, %.0f)", -at.x / gGameUnitsToMeters, at.z / gGameUnitsToMeters);
    }
    if (const char* n = std::getenv("GVR_SYRINGES")) syringes_ = std::max(0, std::min(kMaxSyringes, std::atoi(n)));  // test hook
    LOGI("Health pickups on the map: %zu", healthPickups_.size());
}

void GameApp::dropHealth(const Vec3& at) {
    Vec3 p = at;
    float gy;
    if (assetWorld_ && assetWorld_->terrain().heightAt(p.x, p.z, gy)) p.y = std::max(p.y, gy);
    healthPickups_.push_back({p, 0});
}

void GameApp::injectHealth() {
    if (syringes_ <= 0) return;
    --syringes_;
    healPending_ += kSyringeHeal;
    healFlash_ = 1.0f;
    sfx2D("me_syrng", 1.0f);
    for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.7f, 0.15f});
    LOGI("Health injection: %d left, healing %.0f", syringes_, kSyringeHeal);
}

void GameApp::updatePickups(float dt, const InputFrame& in) {
    pickupFlash_ = std::max(0.0f, pickupFlash_ - dt * 3.0f);
    healFlash_ = std::max(0.0f, healFlash_ - dt * 1.5f);
    hitDirTimer_ = std::max(0.0f, hitDirTimer_ - dt);
    weaponHudT_ += dt;
    // An injection heals over a second.
    if (healPending_ > 0 && playerHealth_ > 0) {
        const float h = std::min(healPending_, kSyringeHeal * dt);
        healPending_ -= h;
        playerHealth_ = std::min(100.0f, playerHealth_ + h);
        if (playerHealth_ >= 100.0f) healPending_ = 0;
    }
    // Some of the Reapers killed drop an injection.
    if (enemyDead_.size() < enemies_.enemies.size()) enemyDead_.resize(enemies_.enemies.size(), 0);
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const bool dead = enemies_.enemies[i].state == EnemyState::Dead;
        if (dead && !enemyDead_[i]) {
            ++recentKills_;
            if (frand() < 0.35f) dropHealth(enemies_.enemies[i].pos + Vec3{0.6f, 0, 0.3f});
        }
        enemyDead_[i] = dead;
    }
    // Walk over one to pick it up (if there's room in the inventory).
    for (auto& p : healthPickups_) {
        const float before = p.t;
        p.t += dt;
        // A green shimmer rising from it, to be seen from afar.
        if (std::floor(p.t * 5.0f) != std::floor(before * 5.0f) && length(p.pos - player_.position) < 80.0f)
            smokePuff(FxTex::FireGreen, p.pos + Vec3{0, 0.35f, 0}, Vec3{0, 0.6f, 0}, 0.7f, 0.25f, 0.7f, 0.55f, 0.3f);
        const Vec3 d = p.pos - player_.position;
        if (syringes_ < kMaxSyringes && d.x * d.x + d.z * d.z < 1.4f * 1.4f && std::fabs(d.y) < 2.2f) {
            ++syringes_;
            p.t = -1e9f;
            pickupFlash_ = 1.0f;
            sfx2D("pu_hlth", 1.0f);
            haptics_.push_back({rig_.moveHand(), 0.5f, 0.08f});
            LOGI("Picked up a health injection (%d)", syringes_);
        }
    }
    healthPickups_.erase(std::remove_if(healthPickups_.begin(), healthPickups_.end(), [](const HealthPickup& p) { return p.t < -1e8f; }),
                         healthPickups_.end());
    // Using one: X (the move hand's lower face button).
    const int mh = rig_.moveHand();
    const bool press = injectEdge_.pressed(in.hand[mh].primary);
    syringeValid_ = false;
    if (firstPerson_) {
        injectT_ = -1;
        if (press && (syringeOut_ || syringes_ > 0)) {
            syringeOut_ = !syringeOut_;
            syringeOutT_ = 0;
            haptics_.push_back({mh, 0.3f, 0.05f});
        }
        if (syringes_ <= 0) syringeOut_ = false;
        const ControllerState& c = in.hand[mh];
        if (syringeOut_ && syringeModel_ && c.gripPoseValid) {
            syringeOutT_ += dt;
            // In the hand, needle along the controller's pointing direction.
            const Pose aim = c.aimPoseValid ? rig_.toWorld(c.aimPose) : rig_.toWorld(c.gripPose);
            const Pose grip = rig_.toWorld(c.gripPose);
            const Quat orient = aim.orientation * rotationBetween(syringeAxis_, Vec3{0, 0, -1});
            const float k = syringeScale_;
            syringeXf_ = Mat4::translation(grip.position + aim.orientation.rotate(Vec3{0, 0, -0.03f})) * Mat4::rotation(orient) *
                         Mat4::scale({k, k, k}) * Mat4::translation(syringeCentre_ * -1.0f);
            syringeValid_ = true;
            // Push it into the body: the needle tip reaches the chest / belly (below the head,
            // inside the torso's radius).
            const Vec3 tip = grip.position + aim.orientation.rotate(Vec3{0, 0, -0.03f - 0.085f});
            if (in.headValid && syringeOutT_ > 0.25f) {
                const Pose head = rig_.toWorld(in.head);
                const Vec3 hf = head.orientation.rotate(Vec3{0, 0, -1});
                Vec3 flat{hf.x, 0, hf.z};
                flat = length(flat) > 1e-3f ? normalize(flat) : Vec3{0, 0, -1};
                const Vec3 chestTop = head.position + Vec3{0, -0.22f, 0} + flat * 0.04f;
                const float drop = chestTop.y - tip.y;
                const Vec3 dxz{tip.x - chestTop.x, 0, tip.z - chestTop.z};
                if (drop > 0.0f && drop < 0.55f && length(dxz) < 0.17f) {
                    injectHealth();
                    syringeOut_ = false;
                }
            }
        }
    } else {
        syringeOut_ = false;
        // Third person: the Meccaryn's injection animation; the dose goes in half way through.
        if (press && syringes_ > 0 && injectT_ < 0 && player_.onGround && !player_.swimming() && playerModel_) {
            const float dur = std::max(0.8f, playerModel_->animDuration(clip_[int(PlayerAnim::Inject)]));
            player_.startAction(PlayerAnim::Inject, dur);
            LOGI("Injection animation (%.2f s)", dur);
            injectT_ = 0;
            injectHealed_ = false;
        }
        if (injectT_ >= 0) {
            injectT_ += dt;
            if (!player_.inAction()) {
                injectT_ = -1;
            } else {
                if (!injectHealed_ && injectT_ > 0.55f * std::max(0.8f, playerModel_->animDuration(clip_[int(PlayerAnim::Inject)]))) {
                    injectHealed_ = true;
                    injectHealth();
                }
                // The syringe in his left hand meanwhile.
                Mat4 ref;
                if (syringeModel_ && playerModel_->nodeWorld("ref_lhand", ref)) {
                    const Vec4 hp = bodyXf_ * Vec4{-ref.m[12] * gGameUnitsToMeters, ref.m[14] * gGameUnitsToMeters,
                                                   ref.m[13] * gGameUnitsToMeters, 1.0f};
                    const Quat body = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
                    const float k = syringeScale_ * 1.3f;
                    syringeXf_ = Mat4::translation({hp.x, hp.y, hp.z}) * Mat4::rotation(body * rotationBetween(syringeAxis_, Vec3{0, -0.6f, -0.8f})) *
                                 Mat4::scale({k, k, k}) * Mat4::translation(syringeCentre_ * -1.0f);
                    syringeValid_ = true;
                }
            }
        }
    }
}

void GameApp::renderPickups() {
    if (!playMode_) return;
    if (pickupModel_)
        for (const auto& p : healthPickups_) {
            const float bob = 0.45f + 0.07f * std::sin(p.t * 2.5f);
            const Mat4 at = Mat4::translation(p.pos + Vec3{0, bob, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, p.t * 1.6f));
            const Mat4 centre = Mat4::translation((pickupModel_->boundsMin + pickupModel_->boundsMax) * -0.5f);
            const float k = pickupScale_;
            drawModel(pickupModel_, at * Mat4::scale({k, k, k}) * centre, false);
            // A soft green-white glow shell so it can be spotted from a distance.
            drawModel(pickupModel_, at * Mat4::scale(Vec3{k, k, k} * 1.2f) * centre, true, 0.25f + 0.1f * std::sin(p.t * 5.0f));
        }
    if (syringeValid_ && syringeModel_ && !dying_) drawModel(syringeModel_, syringeXf_, false);
}

}  // namespace gvr
