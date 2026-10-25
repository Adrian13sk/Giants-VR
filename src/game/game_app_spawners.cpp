// GameApp, rework: barracks that send out soldiers. A standing Reaper barracks (barracks,
// barracks_story2, bw_minibarracks, guard_army_barracks) sends a Reaper or two out of its door every
// 20-35 s while you are within 110 m (more for a giant), at most four of its own about at a time; a
// Ripper barracks (guard_ripperbarracks) breeds medium Rippers the same way from the nest model pool.
// Destroy the building and it stops. (Ours: the original's spawning rules are native code.)
// GVR_SPAWN_TRACE logs it; GVR_SPAWN_FAST=1 makes the first one come at once.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"

namespace gvr {

void GameApp::initSpawners() {
    spawners_.clear();
    if (!assetWorld_ || skirmish_) return;
    const auto& objs = assetWorld_->worldObjects();
    for (size_t i = 0; i < objs.size(); ++i) {
        const auto& o = objs[i];
        const bool rip = o.name == "guard_ripperbarracks";
        const bool rp = o.name == "barracks" || o.name == "barracks_story2" || o.name == "bw_minibarracks" || o.name == "guard_army_barracks";
        if (!rip && !rp) continue;
        Spawner sp;
        sp.object = int(i);
        sp.item = i < objectItem_.size() ? objectItem_[i] : -1;
        sp.rippers = rip;
        sp.pos = Vec3{o.placement.m[12], o.placement.m[13], o.placement.m[14]};
        sp.radius = std::max(2.0f, std::min(12.0f, o.radius * 0.6f));
        sp.timer = std::getenv("GVR_SPAWN_FAST") ? 0.5f : 8.0f + 10.0f * frand();
        spawners_.push_back(sp);
        LOGI("Spawner: %s at game (%.0f, %.0f)", o.name.c_str(), -sp.pos.x / gGameUnitsToMeters, sp.pos.z / gGameUnitsToMeters);
    }
    if (!spawners_.empty()) {
        missionHostiles_ = true;
        LOGI("Spawners: %zu barracks", spawners_.size());
    }
}

void GameApp::updateSpawners(float dt) {
    if (spawners_.empty() || !playMode_ || dying_ || cutsceneActive() || !assetWorld_) return;
    static const bool trace = std::getenv("GVR_SPAWN_TRACE") != nullptr;
    const float range = 110.0f * std::sqrt(std::max(1.0f, heroScale_));
    // Pooled Ripper models in use (as updateNests).
    std::vector<char> used(chars_.size(), 0);
    for (const auto& c : creatures_.creatures)
        if (c.slot >= 0 && size_t(c.slot) < used.size()) used[size_t(c.slot)] = 1;
    for (auto& sp : spawners_) {
        if (sp.item >= 0 && size_t(sp.item) < destruct_.items.size() && !destruct_.items[size_t(sp.item)].alive()) continue;
        const Vec3 to{player_.position.x - sp.pos.x, 0, player_.position.z - sp.pos.z};
        const float d = length(to);
        if (d > range) continue;
        sp.timer -= dt;
        if (sp.timer > 0) continue;
        sp.timer = 20.0f + 15.0f * frand();
        // Its own still about (Reapers by uid; Rippers counted by home).
        int mine = 0;
        if (sp.rippers) {
            for (const auto& c : creatures_.creatures) mine += c.hunter && c.state != CreatureState::Dead && length(c.home - sp.pos) < sp.radius + 3.0f;
        } else {
            sp.sent.erase(std::remove_if(sp.sent.begin(), sp.sent.end(), [&](uint32_t uid) {
                for (const auto& e : enemies_.enemies) if (e.uid == uid && e.state != EnemyState::Dead) return false;
                return true;
            }), sp.sent.end());
            mine = int(sp.sent.size());
        }
        if (mine >= 4) continue;
        // Out of the door: the side facing the player, on the ground.
        const Vec3 dir = d > 1e-3f ? to * (1.0f / d) : Vec3{0, 0, 1};
        const int n = sp.rippers ? 1 : 1 + int(frand() * 1.99f);
        for (int k = 0; k < n && mine < 4; ++k) {
            const Vec3 side{-dir.z, 0, dir.x};
            Vec3 p = sp.pos + dir * (sp.radius + 1.5f) + side * ((frand() - 0.5f) * sp.radius);
            float gy;
            if (!assetWorld_->terrain().heightAt(p.x, p.z, gy) || gy < player_.seaLevel - 0.3f) continue;
            p.y = gy;
            const float yaw = std::atan2(-dir.x, -dir.z);
            if (sp.rippers) {
                bool hatched = false;
                for (size_t c = 0; c < chars_.size() && !hatched; ++c) {
                    CharacterInst& ci = chars_[c];
                    if (!ci.pooled || used[c] || ci.size == CreatureSize::Small) continue;
                    const ObjectType* t = assetWorld_->registry().type(uint32_t(ci.type));
                    const int idx = creatures_.spawnHunter(p, yaw, ci.scale, ci.size, t && t->hitPoints > 0 ? t->hitPoints : 40.0f);
                    creatures_.creatures[size_t(idx)].slot = int(c);
                    creatures_.creatures[size_t(idx)].home = sp.pos;
                    used[c] = 1;
                    ci.objective = -1;
                    sfx("rm_spawn", p, 0.9f);
                    hatched = true;
                }
                if (!hatched) break;
                if (trace) LOGI("spawner: a Ripper out of the Ripper barracks (%.0f m from you)", d);
            } else {
                const int ei = enemies_.spawn(p, yaw);
                if (ei < 0) break;
                Enemy& e = enemies_.enemies[size_t(ei)];
                e.aware = d < 60.0f;
                e.alert = 0.5f;
                e.home = e.patrolTo = sp.pos + dir * (sp.radius + 6.0f);
                sp.sent.push_back(e.uid);
                if (trace) LOGI("spawner: Reaper %u out of the barracks (%.0f m from you)", e.uid, d);
            }
            ++mine;
        }
    }
}

}  // namespace gvr
