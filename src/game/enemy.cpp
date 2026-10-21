#include "game/enemy.h"

#include <algorithm>
#include <cmath>

#include "game/player.h"  // angleDelta

namespace gvr {

bool rayCapsule(const Vec3& o, const Vec3& d, float maxDist, const Vec3& a, const Vec3& b, float r, float& t) {
    // Sample-free: closest approach between the ray segment and the axis segment.
    const Vec3 u = d * maxDist, v = b - a, w = o - a;
    const float A = dot(u, u), B = dot(u, v), C = dot(v, v), D = dot(u, w), E = dot(v, w);
    const float den = A * C - B * B;
    float s = den > 1e-9f ? std::max(0.0f, std::min(1.0f, (B * E - C * D) / den)) : 0.0f;
    float q = C > 1e-9f ? std::max(0.0f, std::min(1.0f, (B * s + E) / C)) : 0.0f;
    s = A > 1e-9f ? std::max(0.0f, std::min(1.0f, (B * q - D) / A)) : 0.0f;
    const Vec3 pr = o + u * s, pa = a + v * q;
    const float dist = length(pr - pa);
    if (dist > r) return false;
    // Back up from the closest point to the surface entry along the ray.
    const float back = std::sqrt(std::max(0.0f, r * r - dist * dist));
    t = std::max(0.0f, s * maxDist - back);
    return true;
}

int EnemyDirector::spawn(const Vec3& pos, float yaw) {
    Enemy e;
    e.pos = pos;
    if (groundHeight) e.pos.y = groundHeight(pos.x, pos.z);
    e.yaw = yaw;
    e.stateTime = float(nextRand() % 1000) * 0.001f;  // desynchronise idle clips
    e.clipTime = e.stateTime;
    enemies.push_back(e);
    return int(enemies.size()) - 1;
}

size_t EnemyDirector::aliveCount() const {
    size_t n = 0;
    for (const auto& e : enemies) n += e.state != EnemyState::Dead;
    return n;
}

void EnemyDirector::setClip(Enemy& e, EnemyClip c, bool loop) {
    e.loopClip = loop;
    if (c == e.clip) return;
    e.prevClip = e.clip;
    e.prevTime = e.clipTime;
    e.clip = c;
    e.clipTime = 0;
    e.fade = 0;
}

bool EnemyDirector::damage(int i, float amount) {
    if (i < 0 || size_t(i) >= enemies.size()) return false;
    Enemy& e = enemies[size_t(i)];
    if (e.state == EnemyState::Dead) return false;
    e.health -= amount;
    if (e.health <= 0) {
        e.state = EnemyState::Dead;
        e.stateTime = 0;
        setClip(e, EnemyClip(int(EnemyClip::Die1) + int(nextRand() % 3)), false);
        return true;
    }
    e.state = EnemyState::Hit;
    e.stateTime = 0;
    const EnemyClip hc = (nextRand() & 1) ? EnemyClip::Hit1 : EnemyClip::Hit2;
    setClip(e, hc, false);
    e.clipTime = 0;  // restart the reaction even if already playing
    e.fade = std::min(e.fade, params.crossfade * 0.5f);
    return false;
}

void EnemyDirector::update(float dt, const Vec3& player, bool playerAlive, std::vector<int>& shooters) {
    shooters.clear();
    for (size_t i = 0; i < enemies.size(); ++i) {
        Enemy& e = enemies[i];
        e.stateTime += dt;
        const Vec3 to{player.x - e.pos.x, 0, player.z - e.pos.z};
        const float dist = std::sqrt(to.x * to.x + to.z * to.z);
        const float faceYaw = std::atan2(-to.x, -to.z);
        auto turn = [&](float target) {
            const float d = angleDelta(e.yaw, target), step = params.turnRate * dt;
            e.yaw = angleDelta(0.0f, e.yaw + (std::fabs(d) <= step ? d : (d > 0 ? step : -step)));
        };
        const bool sees = playerAlive && dist < params.sightRange;
        switch (e.state) {
            case EnemyState::Idle:
                setClip(e, EnemyClip::Idle, true);
                if (sees) { e.state = EnemyState::Advance; e.stateTime = 0; }
                break;
            case EnemyState::Advance: {
                if (!sees) { e.state = EnemyState::Idle; break; }
                if (dist <= params.fireRange) {
                    e.state = EnemyState::Fire;
                    e.stateTime = 0;
                    e.burstLeft = params.burstSize;
                    e.fireCooldown = 0.3f;
                    break;
                }
                // Approach a point beside the player (each enemy has its own flank offset that
                // changes every few seconds), so a group spreads out and circles in.
                e.flankTimer -= dt;
                if (e.flankTimer <= 0) {
                    e.flank = (float(nextRand() % 2001) / 1000.0f - 1.0f) * 9.0f;
                    e.flankTimer = 3.0f + float(nextRand() % 3000) / 1000.0f;
                }
                const Vec3 dirTo = to * (1.0f / std::max(dist, 1e-3f));
                const Vec3 side{-dirTo.z, 0, dirTo.x};
                const float lateral = e.flank * std::min(1.0f, dist / 25.0f);
                const Vec3 goal{player.x + side.x * lateral - e.pos.x, 0, player.z + side.z * lateral - e.pos.z};
                const bool run = dist > params.runRange;
                setClip(e, run ? EnemyClip::Run : EnemyClip::Walk, true);
                turn(std::atan2(-goal.x, -goal.z));
                const Vec3 fwd{-std::sin(e.yaw), 0, -std::cos(e.yaw)};
                e.pos += fwd * ((run ? params.runSpeed : params.walkSpeed) * dt);
                // Walking Reapers shoot as they come (the walk clip is a walk-and-fire cycle).
                e.fireCooldown -= dt;
                if (!run && dist <= params.shootRange && e.fireCooldown <= 0 &&
                    std::fabs(angleDelta(e.yaw, faceYaw)) < 0.6f) {
                    shooters.push_back(int(i));
                    e.fireCooldown = params.fireInterval * 2.5f + float(nextRand() % 1000) * 0.0012f;
                }
                break;
            }
            case EnemyState::Fire:
                turn(faceYaw);
                if (!sees) { e.state = EnemyState::Idle; break; }
                if (dist > params.fireRange * 1.5f) { e.state = EnemyState::Advance; break; }
                e.fireCooldown -= dt;
                if (e.burstLeft > 0) {
                    setClip(e, EnemyClip::Fire, true);
                    const bool facing = std::fabs(angleDelta(e.yaw, faceYaw)) < 0.35f;
                    if (e.fireCooldown <= 0 && facing) {
                        shooters.push_back(int(i));
                        --e.burstLeft;
                        e.fireCooldown = e.burstLeft > 0 ? params.fireInterval : params.burstPause;
                    }
                } else {
                    setClip(e, EnemyClip::Idle, true);
                    if (e.fireCooldown <= 0) { e.burstLeft = params.burstSize; e.fireCooldown = 0; }
                }
                break;
            case EnemyState::Hit:
                if (e.stateTime >= clipDuration[int(e.clip)] * 0.95f) {
                    e.state = dist <= params.fireRange ? EnemyState::Fire : EnemyState::Advance;
                    e.stateTime = 0;
                    e.burstLeft = params.burstSize;
                    e.fireCooldown = 0.4f;
                }
                break;
            case EnemyState::Dead:
                if (e.stateTime > params.corpseTime) e.sink += dt * 0.35f;
                if (e.sink > params.height) e.remove = true;
                break;
        }
        // Ground and buildings.
        if (e.state != EnemyState::Dead && collision) collision->pushOut(e.pos, params.radius, params.height, 0.45f);
        if (groundHeight) e.pos.y = groundHeight(e.pos.x, e.pos.z) - e.sink;
        // Animation clocks (one-shot clips hold their last frame).
        e.clipTime += dt;
        if (!e.loopClip) e.clipTime = std::min(e.clipTime, clipDuration[int(e.clip)] * 0.999f);
        e.prevTime += dt;
        e.fade += dt;
    }
    // Keep living enemies from overlapping each other.
    for (size_t a = 0; a < enemies.size(); ++a)
        for (size_t b = a + 1; b < enemies.size(); ++b) {
            Enemy &p = enemies[a], &q = enemies[b];
            if (p.state == EnemyState::Dead || q.state == EnemyState::Dead) continue;
            Vec3 d{q.pos.x - p.pos.x, 0, q.pos.z - p.pos.z};
            const float l = std::sqrt(d.x * d.x + d.z * d.z), minD = 2.0f * params.radius + 0.2f;
            if (l < minD && l > 1e-4f) {
                d = d * ((minD - l) * 0.5f / l);
                p.pos = p.pos - d;
                q.pos += d;
            }
        }
    enemies.erase(std::remove_if(enemies.begin(), enemies.end(), [](const Enemy& e) { return e.remove; }), enemies.end());
}

bool EnemyDirector::raycast(const Vec3& o, const Vec3& d, float maxDist, float& t, Vec3& normal, int& index) const {
    bool hit = false;
    float best = maxDist;
    for (size_t i = 0; i < enemies.size(); ++i) {
        const Enemy& e = enemies[i];
        if (e.state == EnemyState::Dead) continue;  // shots pass over corpses
        float th;
        const Vec3 a = e.pos + Vec3{0, params.radius, 0}, b = e.pos + Vec3{0, params.height - params.radius, 0};
        if (rayCapsule(o, d, maxDist, a, b, params.radius, th) && th < best) {
            best = th;
            index = int(i);
            const Vec3 p = o + d * th;
            const Vec3 axis{e.pos.x, std::max(a.y, std::min(b.y, p.y)), e.pos.z};
            normal = normalize(p - axis);
            hit = true;
        }
    }
    if (hit) t = best;
    return hit;
}

}  // namespace gvr
