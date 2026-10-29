// (2026-10-29) The Guards' lift: guard_lift_strut, the gantry on the clifftop in story3_prison_6 ("Get to the LIFT and
// ride it up to the GIFT SHOP", GText mo_story3_prison_6). Without it the mission's place to reach - a plateau 80 m
// above the canyon - could not be reached (the reach-objective survey found it). The strut model's rope
// (lift_strut03) hangs 16 m out from its origin and 122 m down; the car is the data's Guard_lift (its floor, the
// guard_spiral, 1.5 m above its origin, ~2 m round). Ours (no rule for it in the data we read): the car waits at the
// canyon floor; stand on it for a second and it rises at 4 m/s to the clifftop; stand on it at the top and it goes
// down. It runs again only after you have stepped off.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/terrain.h"

namespace gvr {

namespace {
constexpr float kRopeOut = 16.0f;     // m: the rope from the strut's origin (lift_strut03 at y -80 game units)
constexpr float kFloorAbove = 1.52f;  // m: the car's floor (guard_spiral, z 7.6) above its origin
constexpr float kCarRadius = 2.1f;    // m: standing room on the floor
constexpr float kSpeed = 4.0f;        // m/s
}  // namespace

void GameApp::initLifts() {
    lifts_.clear();
    player_.platformOn = false;
    if (!assetWorld_) return;
    for (const auto& o : assetWorld_->worldObjects()) {
        if (o.name != "guard_lift_strut") continue;
        Lift l;
        const Vec4 r = o.placement * Vec4{0, 0, -kRopeOut, 1};
        l.rope = Vec3{r.x, 0, r.z};
        l.topY = o.placement.m[13];
        float g = l.topY - 80.0f;
        assetWorld_->terrain().heightAt(l.rope.x, l.rope.z, g);
        l.bottomY = std::max(g, player_.seaLevel) + 0.05f;
        if (l.topY - l.bottomY < 3.0f) continue;
        l.floorY = l.bottomY;
        l.rot = o.placement;
        l.rot.m[12] = l.rot.m[13] = l.rot.m[14] = 0;
        l.car = assetWorld_->models()->get("guard_lift.gbs", "guard_lift.anm");
        lifts_.push_back(l);
        LOGI("Lift: rope at (%.1f %.1f), %.1f m up from the ground at %.1f; car %s", l.rope.x, l.rope.z, l.topY - l.bottomY, l.bottomY,
             l.car ? "ok" : "missing (drawn as nothing)");
    }
}

void GameApp::updateLifts(float dt) {
    player_.platformOn = false;
    for (auto& l : lifts_) {
        const Vec3 p = player_.position;
        const float d = std::sqrt((p.x - l.rope.x) * (p.x - l.rope.x) + (p.z - l.rope.z) * (p.z - l.rope.z));
        const bool on = d < kCarRadius && std::fabs(p.y - l.floorY) < 0.35f && !dying_;
        if (l.moving) {
            const float target = l.goingUp ? l.topY : l.bottomY;
            const float step = kSpeed * dt;
            if (std::fabs(target - l.floorY) <= step) {
                l.floorY = target;
                l.moving = false;
                l.needLeave = on;
                sfx("me_gylnd", Vec3{l.rope.x, l.floorY, l.rope.z}, 0.8f, 0.7f);
                LOGI("Lift: arrived at the %s (%.1f m)%s", l.goingUp ? "top" : "bottom", l.floorY, on ? ", with the player" : "");
            } else {
                l.floorY += l.goingUp ? step : -step;
            }
        } else if (!on) {
            l.needLeave = false;
            l.waitT = 0;
        } else if (!l.needLeave && (l.waitT += dt) > 1.0f) {
            l.moving = true;
            l.goingUp = l.floorY < 0.5f * (l.topY + l.bottomY);
            sfx("ph_enter", Vec3{l.rope.x, l.floorY, l.rope.z}, 1.0f, 0.6f);
            haptics_.push_back({kLeft, 0.4f, 0.15f});
            haptics_.push_back({kRight, 0.4f, 0.15f});
            LOGI("Lift: going %s with the player", l.goingUp ? "up" : "down");
        }
        if (d < kCarRadius + 3.0f) {
            player_.platformOn = true;
            player_.platformCentre = l.rope;
            player_.platformRadius = kCarRadius;
            player_.platformY = l.floorY;
        }
    }
}

void GameApp::renderLifts() {
    for (const auto& l : lifts_)
        if (l.car) drawModel(l.car, Mat4::translation(Vec3{l.rope.x, l.floorY - kFloorAbove, l.rope.z}) * l.rot, false);
}

}  // namespace gvr
