#include "game/vimp.h"

#include <algorithm>
#include <cmath>

#include "game/player.h"  // angleDelta

namespace gvr {

int VimpDirector::addHerd(const Vec3& centre) {
    VimpHerd h;
    h.centre = centre;
    herds.push_back(h);
    return int(herds.size()) - 1;
}

int VimpDirector::add(int herd, const Vec3& pos, float yaw) {
    Vimp v;
    v.pos = v.target = pos;
    v.yaw = yaw;
    v.herd = herd;
    v.hp = 20;
    v.state = VimpState::Graze;
    v.duration = 3.0f + 6.0f * rand01();
    v.stateTime = rand01() * v.duration;
    setClip(v, VimpClip::Graze);
    v.clipTime = rand01() * 2.0f;
    vimps.push_back(v);
    return int(vimps.size()) - 1;
}

void VimpDirector::setClip(Vimp& v, VimpClip c) {
    if (v.clip != c) v.clipTime = 0;
    v.clip = c;
}

void VimpDirector::pickGraze(Vimp& v) {
    v.stateTime = 0;
    v.rate = 1;
    const VimpHerd& h = herds[size_t(v.herd)];
    if (rand01() < 0.45f) {
        // Amble to another patch of grass near the herd.
        for (int tries = 0; tries < 6; ++tries) {
            const float a = rand01() * 6.2832f, d = rand01() * params.herdRadius;
            Vec3 t = h.centre + Vec3{std::cos(a) * d, 0, std::sin(a) * d};
            if (walkable && !walkable(t.x, t.z)) continue;
            if (groundHeight) t.y = groundHeight(t.x, t.z);
            v.target = t;
            v.state = VimpState::Walk;
            v.duration = 25.0f;
            setClip(v, VimpClip::Walk);
            return;
        }
    }
    v.state = VimpState::Graze;
    v.duration = 4.0f + 8.0f * rand01();
    setClip(v, rand01() < 0.8f ? VimpClip::Graze : VimpClip::Idle);
}

void VimpDirector::stampede(int herd, const Vec3& from) {
    VimpHerd& h = herds[size_t(herd)];
    h.calm = 0;
    Vec3 away = h.centre - from;
    away.y = 0;
    const float l = length(away);
    away = l > 1e-3f ? away * (1.0f / l) : Vec3{1, 0, 0};
    // Run away from it; if that is sea or cliff, veer off to the side.
    Vec3 to = h.centre + away * params.fleeDistance;
    for (int k = 0; k < 6 && walkable && !walkable(to.x, to.z); ++k) {
        const float a = (k % 2 ? 1.0f : -1.0f) * 0.5f * float(k / 2 + 1);
        const Vec3 d{away.x * std::cos(a) - away.z * std::sin(a), 0, away.x * std::sin(a) + away.z * std::cos(a)};
        to = h.centre + d * params.fleeDistance;
    }
    h.fleeTo = to;
    for (auto& v : vimps) {
        if (v.herd != herd || v.state == VimpState::Dead) continue;
        // Each runs to its own spot round the new grazing place.
        v.target = to + Vec3{(v.pos.x - h.centre.x) * 0.5f, 0, (v.pos.z - h.centre.z) * 0.5f};
        if (groundHeight) v.target.y = groundHeight(v.target.x, v.target.z);
        if (v.state != VimpState::Hit) {
            v.state = VimpState::Flee;
            v.stateTime = 0;
            v.duration = 14.0f;
            setClip(v, VimpClip::Run);
        }
    }
    h.centre = to;
}

void VimpDirector::update(float dt, const std::vector<Vec3>& threats, const std::vector<Vec3>& scares) {
    for (size_t k = 0; k < herds.size(); ++k) {
        VimpHerd& h = herds[k];
        h.calm += dt;
        // Startled by a threat close to any of its beasts, or by shots / blasts nearby.
        bool startled = false;
        Vec3 from;
        for (const auto& v : vimps) {
            if (v.herd != int(k) || v.state == VimpState::Dead) continue;
            for (const Vec3& t : threats)
                if (!startled && length(Vec3{t.x - v.pos.x, 0, t.z - v.pos.z}) < params.fearRadius) { startled = true; from = t; }
            for (const Vec3& s : scares)
                if (!startled && length(Vec3{s.x - v.pos.x, 0, s.z - v.pos.z}) < params.shotFear) { startled = true; from = s; }
        }
        if (startled && h.calm > 1.5f) stampede(int(k), from);
    }
    for (auto& v : vimps) {
        v.stateTime += dt;
        v.clipTime += dt * v.rate;
        switch (v.state) {
            case VimpState::Dead:
                if (v.stateTime > params.corpseTime) v.remove = true;
                break;
            case VimpState::Hit:
                if (v.stateTime > 0.8f) {
                    v.state = VimpState::Flee;
                    v.stateTime = 0;
                    setClip(v, VimpClip::Run);
                }
                break;
            case VimpState::Graze:
                if (v.stateTime > v.duration) pickGraze(v);
                break;
            case VimpState::Walk:
            case VimpState::Flee: {
                const bool flee = v.state == VimpState::Flee;
                Vec3 to = v.target - v.pos;
                to.y = 0;
                const float dist = length(to);
                if (dist < 1.0f || v.stateTime > v.duration) {
                    if (flee && herds[size_t(v.herd)].calm < params.calmAfter) {
                        // Mill about, uneasy, until calm.
                        v.state = VimpState::Graze;
                        v.stateTime = 0;
                        v.duration = 2.0f;
                        setClip(v, VimpClip::Scared);
                    } else {
                        pickGraze(v);
                    }
                    break;
                }
                const float speed = flee ? params.runSpeed : params.walkSpeed;
                const float want = std::atan2(-to.x, -to.z);
                const float d = angleDelta(v.yaw, want);
                const float step = params.turnRate * (flee ? 1.6f : 1.0f) * dt;
                v.yaw += std::max(-step, std::min(step, d));
                const Vec3 fwd{-std::sin(v.yaw), 0, -std::cos(v.yaw)};
                Vec3 np = v.pos + fwd * (std::min(dist, speed * dt));
                if (walkable && !walkable(np.x, np.z)) { pickGraze(v); break; }
                if (groundHeight) np.y = groundHeight(np.x, np.z);
                v.pos = np;
                v.rate = speed / (flee ? params.runStride : params.walkStride);
                break;
            }
        }
    }
    vimps.erase(std::remove_if(vimps.begin(), vimps.end(), [](const Vimp& v) { return v.remove; }), vimps.end());
}

bool VimpDirector::damage(int i, float amount, const Vec3& from) {
    if (i < 0 || size_t(i) >= vimps.size()) return false;
    Vimp& v = vimps[size_t(i)];
    if (v.state == VimpState::Dead) return false;
    v.hp -= amount;
    if (v.hp <= 0) {
        v.state = VimpState::Dead;
        v.stateTime = 0;
        setClip(v, VimpClip::Die);
        v.rate = 1;
        return true;
    }
    v.state = VimpState::Hit;
    v.stateTime = 0;
    setClip(v, VimpClip::Hit);
    v.rate = 1;
    stampede(v.herd, from);
    v.state = VimpState::Hit;  // finish the flinch first
    setClip(v, VimpClip::Hit);
    return false;
}

size_t VimpDirector::aliveCount() const {
    size_t n = 0;
    for (const auto& v : vimps) n += v.state != VimpState::Dead;
    return n;
}

}  // namespace gvr
