#include "game/particles.h"

#include <algorithm>
#include <cmath>

namespace gvr {

float Particle::size() const {
    const float k = std::min(1.0f, age / std::max(1e-3f, life));
    return size0 + (size1 - size0) * (1.0f - (1.0f - k) * (1.0f - k));
}

float Particle::opacity() const {
    const float k = age / std::max(1e-3f, life);
    const float in = std::min(1.0f, k / 0.08f);
    const float out = k < 0.5f ? 1.0f : std::max(0.0f, 1.0f - (k - 0.5f) / 0.5f);
    return alpha * in * out;
}

void ParticleSystem::emit(const Particle& p) {
    if (parts.size() >= maxParticles) parts.erase(parts.begin());  // the oldest goes
    parts.push_back(p);
    parts.back().seed = (nextSeed++) * 2654435761u;  // (Knuth's multiplicative hash: evenly spread)
}

void ParticleSystem::update(float dt) {
    for (auto& p : parts) {
        p.age += dt;
        p.vel.y += p.rise * dt;
        p.vel = p.vel * std::max(0.0f, 1.0f - p.drag * dt);
        p.pos += p.vel * dt;
        p.angle += p.spin * dt;
    }
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Particle& p) { return p.age >= p.life; }), parts.end());
}

}  // namespace gvr
