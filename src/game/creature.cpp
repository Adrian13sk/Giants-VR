#include "game/creature.h"

#include <cmath>

#include "game/enemy.h"   // rayCapsule
#include "game/player.h"  // angleDelta

namespace gvr {

int CreatureDirector::spawn(const Vec3& pos, float yaw, float scale, int kind) {
    Creature c;
    c.pos = c.home = pos;
    if (groundHeight) c.pos.y = c.home.y = groundHeight(pos.x, pos.z);
    c.yaw = yaw;
    c.scale = std::max(0.2f, scale);
    c.health = params.health * std::min(4.0f, c.scale * c.scale);
    c.kind = kind;
    c.clipTime = rnd() * clipDuration[int(CreatureClip::Idle)];  // desynchronise idles
    creatures.push_back(c);
    return int(creatures.size()) - 1;
}

size_t CreatureDirector::aliveCount() const {
    size_t n = 0;
    for (const auto& c : creatures) n += c.state != CreatureState::Dead;
    return n;
}

void CreatureDirector::setClip(Creature& c, CreatureClip clip, bool loop, float rate) {
    c.loopClip = loop;
    c.rate = rate;
    if (clip == c.clip) return;
    c.prevClip = c.clip;
    c.prevTime = c.clipTime;
    c.clip = clip;
    c.clipTime = 0;
    c.fade = 0;
}

bool CreatureDirector::tryMove(Creature& c, const Vec3& step) {
    if (!groundHeight) { c.pos += step; return true; }
    const float run = std::sqrt(step.x * step.x + step.z * step.z);
    const float rise = groundHeight(c.pos.x + step.x, c.pos.z + step.z) - groundHeight(c.pos.x, c.pos.z);
    if (rise > params.maxStepUp * c.scale || (rise > 0.02f && run > 1e-4f && rise / run > params.maxSlope)) return false;
    c.pos += step;
    return true;
}

bool CreatureDirector::damage(int i, float amount, std::vector<CreatureEvent>* events) {
    if (i < 0 || size_t(i) >= creatures.size()) return false;
    Creature& c = creatures[size_t(i)];
    if (c.state == CreatureState::Dead) return false;
    c.health -= amount;
    if (c.health <= 0) {
        c.state = CreatureState::Dead;
        c.stateTime = 0;
        setClip(c, CreatureClip::Die, false);
        if (events) events->push_back({CreatureEvent::Die, i, 0, c.pos});
        return true;
    }
    // Interrupts everything but a lunge in flight; a hit also makes it notice the player.
    if (c.state != CreatureState::Lunge) {
        c.state = CreatureState::Hit;
        c.stateTime = 0;
        setClip(c, amount >= 20.0f ? CreatureClip::Hit : CreatureClip::HitSmall, false);
        c.clipTime = 0;
        c.fade = std::min(c.fade, params.crossfade * 0.5f);
    }
    if (events) events->push_back({CreatureEvent::Hurt, i, 0, c.pos});
    return false;
}

void CreatureDirector::update(float dt, const Vec3& player, bool playerAlive, std::vector<CreatureEvent>& events) {
    events.clear();
    for (size_t i = 0; i < creatures.size(); ++i) {
        Creature& c = creatures[i];
        c.stateTime += dt;
        c.cooldown = std::max(0.0f, c.cooldown - dt);
        c.biteWait = std::max(0.0f, c.biteWait - dt);
        const float s = c.scale, ss = std::sqrt(s);
        const Vec3 to{player.x - c.pos.x, 0, player.z - c.pos.z};
        const float dist = std::sqrt(to.x * to.x + to.z * to.z);
        const float faceYaw = std::atan2(-to.x, -to.z);
        const float fromHome = std::hypot(c.pos.x - c.home.x, c.pos.z - c.home.z);
        auto turn = [&](float target, float rate) {
            const float d = angleDelta(c.yaw, target), st = rate * dt;
            c.yaw = angleDelta(0.0f, c.yaw + (std::fabs(d) <= st ? d : (d > 0 ? st : -st)));
        };
        const Vec3 fwd{-std::sin(c.yaw), 0, -std::cos(c.yaw)};
        const float reach = params.reach * s;
        const bool sees = playerAlive && dist < params.sightRange * ss;
        const float runDur = clipDuration[int(CreatureClip::Attack)];
        switch (c.state) {
            case CreatureState::Idle:
                setClip(c, CreatureClip::Idle, true);
                if (sees) {
                    c.state = CreatureState::Chase;
                    c.stateTime = 0;
                    events.push_back({CreatureEvent::Alert, int(i), 0, c.pos});
                }
                break;
            case CreatureState::Chase: {
                if (!playerAlive || (fromHome > params.leash && dist > params.sightRange * ss)) {
                    c.state = CreatureState::Return;
                    break;
                }
                if (dist <= reach) {
                    turn(faceYaw, params.turnRate);
                    if (c.biteWait > 0) {  // catching its breath between bites
                        setClip(c, CreatureClip::Idle, true);
                        break;
                    }
                    c.state = CreatureState::Attack;
                    c.stateTime = 0;
                    c.struck = false;
                    setClip(c, CreatureClip::Attack, false);
                    c.clipTime = 0;
                    events.push_back({CreatureEvent::Swing, int(i), 0, c.pos});
                    break;
                }
                turn(faceYaw, params.turnRate);
                const bool facing = std::fabs(angleDelta(c.yaw, faceYaw)) < 0.3f;
                if (facing && c.cooldown <= 0 && dist >= params.lungeMin * s && dist <= params.lungeMax * s &&
                    rnd() < dt * 1.5f) {
                    c.state = CreatureState::Lunge;
                    c.stateTime = 0;
                    c.struck = false;
                    c.cooldown = params.lungeCooldown;
                    setClip(c, CreatureClip::Lunge, false);
                    c.clipTime = 0;
                    events.push_back({CreatureEvent::Lunge, int(i), 0, c.pos});
                    break;
                }
                setClip(c, CreatureClip::Walk, true, params.runPlayback);
                tryMove(c, fwd * (params.runSpeed * ss * dt));
                break;
            }
            case CreatureState::Attack: {
                turn(faceYaw, params.turnRate * 0.5f);
                if (!c.struck && c.clipTime >= runDur * params.biteAt) {
                    c.struck = true;
                    if (playerAlive && dist <= reach + 0.4f * s)
                        events.push_back({CreatureEvent::Strike, int(i), params.biteDamage * ss, c.pos});
                }
                if (c.clipTime >= runDur * 0.98f) {
                    c.state = CreatureState::Chase;
                    c.stateTime = 0;
                    c.biteWait = params.bitePause;
                }
                break;
            }
            case CreatureState::Lunge: {
                // Airborne between 25% and 70% of the clip: travels toward where it aimed.
                const float d = clipDuration[int(CreatureClip::Lunge)];
                const float f = c.clipTime / std::max(d, 1e-3f);
                if (f > 0.25f && f < 0.7f) tryMove(c, fwd * (params.lungeSpeed * ss * dt));
                if (!c.struck && f >= 0.55f) {
                    c.struck = true;
                    if (playerAlive && dist <= reach + 0.6f * s)
                        events.push_back({CreatureEvent::Strike, int(i), params.lungeDamage * ss, c.pos});
                }
                if (f >= 0.98f) {
                    c.state = CreatureState::Chase;
                    c.stateTime = 0;
                }
                break;
            }
            case CreatureState::Hit:
                if (c.stateTime >= clipDuration[int(c.clip)] * 0.95f) {
                    c.state = playerAlive ? CreatureState::Chase : CreatureState::Return;
                    c.stateTime = 0;
                }
                break;
            case CreatureState::Return: {
                if (sees && fromHome < params.leash * 0.8f) {
                    c.state = CreatureState::Chase;
                    break;
                }
                const Vec3 h{c.home.x - c.pos.x, 0, c.home.z - c.pos.z};
                const float hd = std::sqrt(h.x * h.x + h.z * h.z);
                if (hd < 0.5f) {
                    c.state = CreatureState::Idle;
                    break;
                }
                turn(std::atan2(-h.x, -h.z), params.turnRate);
                setClip(c, CreatureClip::Walk, true, 1.0f);
                if (!tryMove(c, fwd * (params.walkSpeed * ss * dt)) && c.stateTime > 6.0f) {
                    c.state = CreatureState::Idle;  // stuck: settle where it is
                    c.home = c.pos;
                }
                break;
            }
            case CreatureState::Dead:
                if (c.stateTime > params.corpseTime) c.sink += dt * 0.25f;
                if (c.sink > params.height * s + 0.5f) c.remove = true;
                break;
        }
        if (c.state != CreatureState::Dead) {
            // Ground too steep to stand on: slide down; buildings push out.
            if (groundHeight) {
                const float h = 0.5f;
                const float gx = (groundHeight(c.pos.x + h, c.pos.z) - groundHeight(c.pos.x - h, c.pos.z)) / (2 * h);
                const float gz = (groundHeight(c.pos.x, c.pos.z + h) - groundHeight(c.pos.x, c.pos.z - h)) / (2 * h);
                const float grade = std::sqrt(gx * gx + gz * gz);
                if (grade > params.maxSlope) c.pos += Vec3{-gx / grade, 0, -gz / grade} * (5.0f * dt);
            }
            if (collision) collision->pushOut(c.pos, params.radius * s, params.height * s, 0.3f * s);
        }
        if (groundHeight) c.pos.y = groundHeight(c.pos.x, c.pos.z) - c.sink;
        c.clipTime += dt * c.rate;
        if (!c.loopClip) c.clipTime = std::min(c.clipTime, clipDuration[int(c.clip)] * 0.999f);
        c.prevTime += dt;
        c.fade += dt;
    }
    // Living creatures don't overlap each other or the player.
    for (size_t a = 0; a < creatures.size(); ++a) {
        Creature& p = creatures[a];
        if (p.state == CreatureState::Dead) continue;
        for (size_t b = a + 1; b < creatures.size(); ++b) {
            Creature& q = creatures[b];
            if (q.state == CreatureState::Dead) continue;
            Vec3 d{q.pos.x - p.pos.x, 0, q.pos.z - p.pos.z};
            const float l = std::sqrt(d.x * d.x + d.z * d.z), minD = params.radius * (p.scale + q.scale) + 0.1f;
            if (l < minD && l > 1e-4f) {
                d = d * ((minD - l) * 0.5f / l);
                p.pos = p.pos - d;
                q.pos += d;
            }
        }
        Vec3 d{p.pos.x - player.x, 0, p.pos.z - player.z};
        const float l = std::sqrt(d.x * d.x + d.z * d.z), minD = params.radius * p.scale + 0.35f;
        if (playerAlive && l < minD && l > 1e-4f) p.pos += d * ((minD - l) / l);
    }
    creatures.erase(std::remove_if(creatures.begin(), creatures.end(), [](const Creature& c) { return c.remove; }),
                    creatures.end());
}

bool CreatureDirector::raycast(const Vec3& o, const Vec3& d, float maxDist, float& t, Vec3& normal, int& index) const {
    bool hit = false;
    float best = maxDist;
    for (size_t i = 0; i < creatures.size(); ++i) {
        const Creature& c = creatures[i];
        if (c.state == CreatureState::Dead) continue;
        const float r = params.radius * c.scale, h = params.height * c.scale;
        const Vec3 a = c.pos + Vec3{0, r, 0}, b = c.pos + Vec3{0, std::max(r, h - r), 0};
        float th;
        if (rayCapsule(o, d, maxDist, a, b, r, th) && th < best) {
            best = th;
            index = int(i);
            const Vec3 p = o + d * th;
            normal = normalize(p - Vec3{c.pos.x, std::max(a.y, std::min(b.y, p.y)), c.pos.z});
            hit = true;
        }
    }
    if (hit) t = best;
    return hit;
}

}  // namespace gvr
