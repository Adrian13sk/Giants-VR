#include "game/gore.h"

#include <algorithm>
#include <cmath>

namespace gvr {

float BloodDecal::alpha() const {
    const float in = std::min(1.0f, age / 0.15f);
    const float out = std::min(1.0f, std::max(0.0f, (life - age) / (0.25f * life)));
    return in * out;
}

float BloodDecal::scale() const {
    // Splats land at full size; pools spread out over `grow` seconds.
    const float k = grow > 0 ? std::min(1.0f, age / grow) : 1.0f;
    return size * (0.35f + 0.65f * (1.0f - (1.0f - k) * (1.0f - k)));
}

float BloodSystem::rnd() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return float(rng_ >> 8) / 16777216.0f;
}

void BloodSystem::addDecal(const BloodDecal& d) {
    if (decals.size() >= maxDecals) decals.erase(decals.begin());  // the oldest goes
    decals.push_back(d);
}

void BloodSystem::burst(const Vec3& at, const Vec3& dirIn, int count, float speed, float size) {
    Vec3 dir = dirIn;
    if (length(dir) < 1e-4f) dir = {0, 1, 0};
    dir = normalize(dir);
    for (int i = 0; i < count && drops.size() < maxDrops; ++i) {
        BloodDrop d;
        d.pos = at;
        const Vec3 jitter{rnd() - 0.5f, rnd() * 0.8f, rnd() - 0.5f};
        d.vel = normalize(dir + jitter * 1.4f) * (speed * (0.4f + 0.8f * rnd())) + Vec3{0, 1.2f * rnd(), 0};
        d.size = size * (0.5f + 0.8f * rnd());
        drops.push_back(d);
    }
    if (sprays.size() < maxSprays) {
        BloodSpray s;
        s.pos = at;
        s.vel = dir * (0.6f * speed * 0.3f);
        s.size = size * 3.0f;
        sprays.push_back(s);
    }
}

void BloodSystem::pool(const Vec3& at, float size) {
    BloodDecal d;
    const bool water = at.y < seaLevel - 0.1f;
    const float g = groundHeight ? groundHeight(at.x, at.z) : at.y;
    d.water = water || g < seaLevel - 0.1f;
    d.pos = d.water ? Vec3{at.x, seaLevel + 0.02f, at.z} : Vec3{at.x, g, at.z};
    d.size = size;
    d.grow = d.water ? 1.5f : 6.0f;
    d.angle = rnd() * 6.2832f;
    d.texture = 2;
    d.life = d.water ? 6.0f : 60.0f;
    addDecal(d);
}

void BloodSystem::update(float dt) {
    for (auto& d : drops) {
        d.life += dt;
        d.vel.y -= 9.8f * dt;
        d.pos += d.vel * dt;
        const float g = groundHeight ? groundHeight(d.pos.x, d.pos.z) : -1e9f;
        const bool water = seaLevel > g && d.pos.y <= seaLevel;
        if (d.pos.y <= g || water || d.life > 3.0f) {
            if (d.life <= 3.0f) {
                BloodDecal s;
                s.water = water;
                s.pos = water ? Vec3{d.pos.x, seaLevel + 0.02f, d.pos.z} : Vec3{d.pos.x, g, d.pos.z};
                if (!water && groundHeight) {
                    const float h = 0.3f;
                    s.normal = normalize(Vec3{groundHeight(d.pos.x - h, d.pos.z) - groundHeight(d.pos.x + h, d.pos.z), 2 * h,
                                              groundHeight(d.pos.x, d.pos.z - h) - groundHeight(d.pos.x, d.pos.z + h)});
                }
                s.size = d.size * (water ? 4.0f : 4.5f);
                s.grow = water ? 1.0f : 0.0f;
                s.angle = rnd() * 6.2832f;
                s.texture = rnd() < 0.5f ? 0 : 1;
                s.life = water ? 4.0f : 30.0f + 15.0f * rnd();
                addDecal(s);
            }
            d.life = 1e9f;
        }
    }
    drops.erase(std::remove_if(drops.begin(), drops.end(), [](const BloodDrop& d) { return d.life >= 1e8f; }), drops.end());
    for (auto& d : decals) d.age += dt;
    decals.erase(std::remove_if(decals.begin(), decals.end(), [](const BloodDecal& d) { return d.age >= d.life; }), decals.end());
    for (auto& s : sprays) {
        s.age += dt;
        s.pos += s.vel * dt;
        s.vel = s.vel * std::max(0.0f, 1.0f - 3.0f * dt);
    }
    sprays.erase(std::remove_if(sprays.begin(), sprays.end(), [](const BloodSpray& s) { return s.age >= s.life; }), sprays.end());
}

// ---------------------------------------------------------------------------------------------
float CleanerDirector::rnd() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return float(rng_ >> 8) / 16777216.0f;
}

bool CleanerDirector::busyWith(uint32_t id) const {
    for (const auto& c : cleaners)
        if (c.target == id && c.state != CleanerState::Leave) return true;
    return false;
}

void CleanerDirector::call(uint32_t id, const Vec3& at, int count, float fromYaw) {
    for (int i = 0; i < count; ++i) {
        Cleaner c;
        c.target = id;
        c.targetPos = at;
        const float a = fromYaw + (rnd() - 0.5f) * 1.6f;
        const float r = spawnDistance * (0.8f + 1.0f * rnd());  // a swarm arriving over a few seconds
        c.pos = at + Vec3{-std::sin(a) * r, 0, -std::cos(a) * r};
        if (groundHeight) c.pos.y = groundHeight(c.pos.x, c.pos.z);
        c.offset = float(i) * (6.2832f / float(std::max(1, count))) + 0.5f * rnd();
        c.ring = 0.45f + 0.4f * rnd();
        c.share = 1.0f / float(std::max(1, count));
        c.anim = rnd() * 2.0f;
        cleaners.push_back(c);
    }
}

void CleanerDirector::update(float dt, const std::function<bool(uint32_t, Vec3&)>& corpse,
                             std::vector<std::pair<uint32_t, float>>& eaten, std::vector<CleanerEvent>& events) {
    for (auto& c : cleaners) {
        c.t += dt;
        c.anim += dt;
        Vec3 body;
        const bool there = corpse && corpse(c.target, body);
        if (there) c.targetPos = body;
        if (!there && c.state != CleanerState::Leave) {
            c.state = CleanerState::Leave;
            c.t = 0;
            c.yaw += 3.1416f * (0.7f + 0.6f * rnd());  // turn away
        }
        switch (c.state) {
            case CleanerState::Approach: {
                // To a spot at the body's side (each Cleaner its own), scuttling straight there.
                const Vec3 spot = c.targetPos + Vec3{std::cos(c.offset) * c.ring, 0, std::sin(c.offset) * c.ring};
                Vec3 to{spot.x - c.pos.x, 0, spot.z - c.pos.z};
                const float d = std::sqrt(to.x * to.x + to.z * to.z);
                if (d < 0.15f || c.t > 25.0f) {
                    c.state = CleanerState::Eat;
                    c.t = 0;
                    const Vec3 face{c.targetPos.x - c.pos.x, 0, c.targetPos.z - c.pos.z};
                    if (face.x * face.x + face.z * face.z > 1e-4f) c.yaw = std::atan2(-face.x, -face.z);
                    bool first = true;
                    for (const auto& o : cleaners)
                        if (&o != &c && o.target == c.target && o.state == CleanerState::Eat) first = false;
                    events.push_back({first ? CleanerEvent::StartEat : CleanerEvent::Arrive, c.target, c.pos});
                    break;
                }
                c.yaw = std::atan2(-to.x, -to.z);
                c.speed = walkSpeed;
                c.pos += to * (std::min(d, walkSpeed * dt) / d);
                break;
            }
            case CleanerState::Eat:
                c.speed = 0;
                eaten.push_back({c.target, dt * c.share / eatTime});
                break;
            case CleanerState::Leave: {
                c.speed = walkSpeed * 1.2f;
                c.pos += Vec3{-std::sin(c.yaw), 0, -std::cos(c.yaw)} * (c.speed * dt);
                if (c.t > 3.5f) c.sink += dt * 0.6f;  // burrows away
                if (c.sink > 0.6f) {
                    c.remove = true;
                    events.push_back({CleanerEvent::Gone, c.target, c.pos});
                }
                break;
            }
        }
        if (groundHeight) c.pos.y = groundHeight(c.pos.x, c.pos.z) - c.sink;
    }
    cleaners.erase(std::remove_if(cleaners.begin(), cleaners.end(), [](const Cleaner& c) { return c.remove; }), cleaners.end());
}

}  // namespace gvr
