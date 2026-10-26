#include "game/villager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "game/player.h"  // angleDelta

namespace gvr {

int VillagerDirector::add(const Vec3& pos, float yaw) {
    Villager v;
    v.pos = v.home = v.target = pos;
    v.yaw = yaw;
    startIdle(v);
    v.stateTime = rand01() * v.duration;  // don't all change at once
    static uint32_t nextUid = 1;
    v.uid = nextUid++;
    villagers.push_back(v);
    return int(villagers.size()) - 1;
}

void VillagerDirector::setClip(Villager& v, VillagerClip c) {
    if (v.clip != c) v.clipTime = 0;
    v.clip = c;
}

void VillagerDirector::face(Villager& v, const Vec3& dir, float dt) {
    if (dir.x * dir.x + dir.z * dir.z < 1e-6f) return;
    const float want = std::atan2(-dir.x, -dir.z);
    const float d = angleDelta(v.yaw, want);
    const float step = params.turnRate * dt;
    v.yaw += std::max(-step, std::min(step, d));
}

void VillagerDirector::startIdle(Villager& v) {
    v.stateTime = 0;
    v.rate = 1;
    const float r = rand01();
    if (r < 0.35f) {
        // A stroll to somewhere near home (not through a house).
        for (int tries = 0; tries < 5; ++tries) {
            const float a = rand01() * 6.2832f, d = 3.0f + rand01() * (params.homeRadius - 3.0f);
            Vec3 t = v.home + Vec3{std::cos(a) * d, 0, std::sin(a) * d};
            if (groundHeight) t.y = groundHeight(t.x, t.z);
            if (blocked && blocked(v.pos, t)) continue;
            v.target = t;
            v.state = VillagerState::Walk;
            v.duration = 30.0f;
            setClip(v, VillagerClip::Walk);
            return;
        }
    }
    static const VillagerClip acts[] = {VillagerClip::Stand, VillagerClip::Scan, VillagerClip::Talk, VillagerClip::Listen,
                                        VillagerClip::Laugh, VillagerClip::Sing, VillagerClip::TaiChi, VillagerClip::Sit,
                                        VillagerClip::Celebrate, VillagerClip::Stand};
    const VillagerClip c = acts[int(rand01() * 9.99f)];
    v.state = VillagerState::Act;
    v.duration = c == VillagerClip::Sit || c == VillagerClip::TaiChi ? 10.0f + 8.0f * rand01() : 4.0f + 5.0f * rand01();
    setClip(v, c);
}

bool VillagerDirector::damage(int i, float amount, const Vec3& from) {
    if (i < 0 || size_t(i) >= villagers.size()) return false;
    Villager& v = villagers[size_t(i)];
    if (v.state == VillagerState::Dead) return false;
    v.hp -= amount;
    v.calm = 0;
    v.fleeFrom = from;
    v.stateTime = 0;
    v.rate = 1;
    if (v.hp <= 0) {
        v.state = VillagerState::Dead;
        setClip(v, VillagerClip::Death);
        v.clipTime = 0;
        return true;
    }
    if (v.state != VillagerState::Held) {
        v.state = VillagerState::Hurt;
        setClip(v, VillagerClip::GetsShot);
        v.clipTime = 0;
    }
    return false;
}

void VillagerDirector::hold(int i, bool held) {
    if (i < 0 || size_t(i) >= villagers.size() || villagers[size_t(i)].state == VillagerState::Dead) return;
    Villager& v = villagers[size_t(i)];
    v.stateTime = 0;
    v.rate = 1;
    v.calm = 0;
    if (held) {
        v.state = VillagerState::Held;
        setClip(v, VillagerClip::Squirm);
    } else {
        v.state = VillagerState::Cower;
        setClip(v, VillagerClip::Cower);
    }
}

void VillagerDirector::handsUp(int i) {
    if (i < 0 || size_t(i) >= villagers.size()) return;
    Villager& v = villagers[size_t(i)];
    if (v.state == VillagerState::Dead || v.state == VillagerState::Held || v.state == VillagerState::Hurt || v.state == VillagerState::HandsUp) return;
    v.state = VillagerState::HandsUp;
    v.stateTime = 0;
    v.duration = 2.5f;
    v.rate = 1;
    setClip(v, VillagerClip::HandsUp);
}

void VillagerDirector::update(float dt, const Vec3& player, const std::vector<Vec3>& threats, const std::vector<Vec3>& scares) {
    for (auto& v : villagers) {
        v.stateTime += dt;
        v.clipTime += dt * v.rate;
        // Rework: the dead lie still (the death clip holds its last frame), then go; the held squirm;
        // the hurt flinch, then run from where it came; hands up hold, then cower.
        const float cd = clipDuration[int(v.clip)];
        if (v.state == VillagerState::Dead) {
            if (cd > 0) v.clipTime = std::min(v.clipTime, cd * 0.999f);
            continue;
        }
        if (v.state == VillagerState::Held) continue;
        static const bool still = std::getenv("GVR_STILL_SMARTIES") != nullptr;  // (test hook: they stand where they are)
        if (still && v.state != VillagerState::Hurt) continue;
        if (v.state == VillagerState::Hurt) {
            if (cd > 0) v.clipTime = std::min(v.clipTime, cd * 0.999f);
            if (v.stateTime < std::max(0.5f, cd)) continue;
            Vec3 away = v.pos - v.fleeFrom;
            away.y = 0;
            away = length(away) > 1e-3f ? normalize(away) : Vec3{1, 0, 0};
            v.target = v.home + away * (params.homeRadius * 0.9f);
            if (groundHeight) v.target.y = groundHeight(v.target.x, v.target.z);
            v.state = VillagerState::Flee;
            v.stateTime = 0;
            v.duration = 5.0f;
            setClip(v, VillagerClip::Run);
        }
        if (v.state == VillagerState::HandsUp) {
            face(v, player - v.pos, dt);
            if (v.stateTime > v.duration) { v.state = VillagerState::Cower; v.stateTime = 0; setClip(v, VillagerClip::Cower); }
            continue;
        }
        v.calm += dt;
        v.waveCooldown = std::max(0.0f, v.waveCooldown - dt);
        // Danger: the nearest threat, or a bang close by.
        Vec3 danger;
        float dd = 1e9f;
        for (const Vec3& t : threats) {
            const float d = length(Vec3{t.x - v.pos.x, 0, t.z - v.pos.z});
            if (d < params.fearRadius && d < dd) { dd = d; danger = t; }
        }
        for (const Vec3& s : scares) {
            const float d = length(Vec3{s.x - v.pos.x, 0, s.z - v.pos.z});
            if (d < params.blastFear && d < dd) { dd = d; danger = s; }
        }
        if (dd < 1e8f) {
            v.calm = 0;
            if (v.state != VillagerState::Flee && v.state != VillagerState::Cower) {
                // Run away from it, toward the far side of home.
                Vec3 away = v.pos - danger;
                away.y = 0;
                const float l = length(away);
                away = l > 1e-3f ? away * (1.0f / l) : Vec3{1, 0, 0};
                Vec3 t = v.home + away * (params.homeRadius * 0.9f);
                if (groundHeight) t.y = groundHeight(t.x, t.z);
                v.target = t;
                v.state = VillagerState::Flee;
                v.stateTime = 0;
                v.duration = 5.0f;
                setClip(v, VillagerClip::Run);
            }
        }
        switch (v.state) {
            case VillagerState::Walk:
            case VillagerState::Flee: {
                const bool flee = v.state == VillagerState::Flee;
                Vec3 to = v.target - v.pos;
                to.y = 0;
                const float dist = length(to);
                const float speed = flee ? params.runSpeed : params.walkSpeed;
                if (dist < 0.4f || v.stateTime > v.duration + (flee ? 0.0f : 25.0f)) {
                    if (flee) {
                        v.state = VillagerState::Cower;
                        v.stateTime = 0;
                        setClip(v, VillagerClip::Cower);
                    } else {
                        startIdle(v);
                    }
                    break;
                }
                face(v, to, dt);
                const Vec3 step = to * (std::min(dist, speed * dt) / dist);
                Vec3 np = v.pos + step;
                if (!flee && blocked && blocked(v.pos, np)) { startIdle(v); break; }
                if (groundHeight) np.y = groundHeight(np.x, np.z);
                v.pos = np;
                v.rate = speed / (flee ? params.runStride : params.walkStride);
                break;
            }
            case VillagerState::Cower:
                v.rate = 1;
                if (v.calm > params.calmAfter) startIdle(v);
                break;
            case VillagerState::Wave: {
                face(v, player - v.pos, dt);
                if (v.stateTime > v.duration) startIdle(v);
                break;
            }
            case VillagerState::Act:
            case VillagerState::Idle: {
                // The Meccaryn close by: a wave (now and then).
                const float dp = length(Vec3{player.x - v.pos.x, 0, player.z - v.pos.z});
                if (dp < params.waveRadius && v.waveCooldown <= 0 && v.calm > params.calmAfter) {
                    v.state = VillagerState::Wave;
                    v.stateTime = 0;
                    v.duration = 2.5f;
                    v.waveCooldown = 20.0f;
                    setClip(v, VillagerClip::Wave);
                    break;
                }
                if (v.stateTime > v.duration) startIdle(v);
                break;
            }
        }
    }
    // Bodies go after a while.
    villagers.erase(std::remove_if(villagers.begin(), villagers.end(),
                                   [&](const Villager& v) { return v.state == VillagerState::Dead && v.stateTime > params.corpseTime; }),
                    villagers.end());
}

int VillagerDirector::waveBack(const Vec3& player, float radius) {
    int n = 0;
    for (auto& v : villagers) {
        if (v.state == VillagerState::Dead || v.state == VillagerState::Held || v.state == VillagerState::Hurt || v.state == VillagerState::Flee || v.state == VillagerState::Cower || v.calm < params.calmAfter) continue;
        if (length(Vec3{player.x - v.pos.x, 0, player.z - v.pos.z}) > radius) continue;
        v.state = VillagerState::Wave;
        v.stateTime = 0;
        v.duration = 2.5f;
        v.waveCooldown = 8.0f;
        setClip(v, VillagerClip::Wave);
        ++n;
    }
    return n;
}

int VillagerDirector::cheer() {
    int n = 0;
    for (auto& v : villagers) {
        if (v.state == VillagerState::Flee || v.state == VillagerState::Dead || v.state == VillagerState::Held || v.state == VillagerState::Hurt) continue;
        v.state = VillagerState::Act;
        v.stateTime = 0;
        v.duration = 3.5f;
        v.calm = params.calmAfter + 1.0f;
        v.waveCooldown = std::max(v.waveCooldown, 5.0f);
        setClip(v, VillagerClip::Celebrate);
        ++n;
    }
    return n;
}

}  // namespace gvr
