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

bool EnemyDirector::tryMove(Enemy& e, const Vec3& step) {
    if (!groundHeight) { e.pos += step; return true; }
    const float run = std::sqrt(step.x * step.x + step.z * step.z);
    const float g0 = groundHeight(e.pos.x, e.pos.z);
    const float g1 = groundHeight(e.pos.x + step.x, e.pos.z + step.z);
    const float rise = g1 - g0;
    if (rise > params.maxStepUp || (rise > 0.02f && run > 1e-4f && rise / run > params.maxSlope)) return false;
    e.pos += step;
    return true;
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
                // Run (no shooting while moving) toward a spot beside the player, then stop and fire.
                if (!sees) { e.state = EnemyState::Idle; break; }
                e.flankTimer -= dt;
                const Vec3 dirTo = to * (1.0f / std::max(dist, 1e-3f));
                const Vec3 side{-dirTo.z, 0, dirTo.x};
                const Vec3 goal{player.x + side.x * e.flank - dirTo.x * params.fireRange - e.pos.x, 0,
                                player.z + side.z * e.flank - dirTo.z * params.fireRange - e.pos.z};
                const float goalDist = std::sqrt(goal.x * goal.x + goal.z * goal.z);
                const bool arrived = goalDist < 1.0f || dist <= params.fireRange;
                // Stop to shoot when in range and the current run has lasted long enough (or arrived).
                if (dist <= params.shootRange && (arrived || e.flankTimer <= 0)) {
                    e.state = EnemyState::Fire;
                    e.stateTime = 0;
                    e.burstLeft = params.burstSize;
                    e.burstsLeft = params.burstsPerStop;
                    e.fireCooldown = 0.35f;
                    break;
                }
                setClip(e, EnemyClip::Run, true);
                // Steer around cliffs: while detouring, the heading is offset from the goal.
                e.detourTime -= dt;
                if (e.detourTime <= 0) e.detour = 0;
                turn(std::atan2(-goal.x, -goal.z) + e.detour);
                const Vec3 fwd{-std::sin(e.yaw), 0, -std::cos(e.yaw)};
                if (tryMove(e, fwd * (params.runSpeed * dt))) {
                    e.blockedTime = 0;
                } else {
                    // Can't climb this: try going around; if the player is in range, shoot instead.
                    e.blockedTime += dt;
                    if (e.blockedTime > 0.3f) {
                        if (dist <= params.shootRange) {
                            e.state = EnemyState::Fire;
                            e.stateTime = 0;
                            e.burstLeft = params.burstSize;
                            e.burstsLeft = params.burstsPerStop;
                            e.fireCooldown = 0.35f;
                        } else {
                            const float side = e.detour != 0 ? -e.detour : ((nextRand() & 1) ? 1.1f : -1.1f);
                            e.detour = side;
                            e.detourTime = 2.0f;
                        }
                        e.blockedTime = 0;
                    }
                }
                break;
            }
            case EnemyState::Fire:
                turn(faceYaw);
                if (!sees) { e.state = EnemyState::Idle; break; }
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
                    if (e.fireCooldown <= 0) {
                        if (--e.burstsLeft > 0) {
                            e.burstLeft = params.burstSize;
                        } else {
                            // Reposition: pick a new flank spot and run there.
                            e.flank = (float(nextRand() % 2001) / 1000.0f - 1.0f) * 10.0f;
                            e.flankTimer = 1.5f + float(nextRand() % 2000) / 1000.0f;
                            e.state = EnemyState::Advance;
                            e.stateTime = 0;
                        }
                    }
                }
                break;
            case EnemyState::Hit:
                if (e.stateTime >= clipDuration[int(e.clip)] * 0.95f) {
                    // After being hit: usually run for cover to a new spot.
                    e.state = EnemyState::Advance;
                    e.stateTime = 0;
                    e.flank = (float(nextRand() % 2001) / 1000.0f - 1.0f) * 10.0f;
                    e.flankTimer = 1.0f + float(nextRand() % 1500) / 1000.0f;
                }
                break;
            case EnemyState::Dead:
                if (e.stateTime > params.corpseTime) e.sink += dt * 0.35f;
                if (e.sink > params.height) e.remove = true;
                break;
        }
        // Ground too steep to stand on: slide downhill (as the player does).
        if (e.state != EnemyState::Dead && groundHeight) {
            const float h = 0.5f;
            const float gx = (groundHeight(e.pos.x + h, e.pos.z) - groundHeight(e.pos.x - h, e.pos.z)) / (2 * h);
            const float gz = (groundHeight(e.pos.x, e.pos.z + h) - groundHeight(e.pos.x, e.pos.z - h)) / (2 * h);
            const float grade = std::sqrt(gx * gx + gz * gz);
            if (grade > params.maxSlope) e.pos += Vec3{-gx / grade, 0, -gz / grade} * (params.slideSpeed * dt);
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
