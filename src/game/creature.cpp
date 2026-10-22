#include "game/creature.h"

#include <cmath>

#include "game/enemy.h"   // rayCapsule
#include "game/player.h"  // angleDelta

namespace gvr {

namespace {
constexpr float kTwoPi = 6.2831853f;
Vec3 headingDir(float yaw) { return Vec3{-std::sin(yaw), 0, -std::cos(yaw)}; }
float headingTo(const Vec3& d) { return std::atan2(-d.x, -d.z); }
}  // namespace

int CreatureDirector::spawn(const Vec3& pos, float yaw, float scale, CreatureSize size, float hitPoints, bool canSpit) {
    Creature c;
    c.pos = c.home = pos;
    if (groundHeight) c.pos.y = c.home.y = groundHeight(pos.x, pos.z);
    c.yaw = yaw;
    c.scale = std::max(0.2f, scale);
    c.health = c.maxHealth = std::max(1.0f, hitPoints);
    c.size = size;
    c.canSpit = canSpit && size != CreatureSize::Small;
    c.surround = rnd() * kTwoPi;
    c.spitWait = 1.0f + rnd() * 2.0f;
    c.clipTime = rnd() * clipDuration[int(CreatureClip::Idle)];  // desynchronise idles
    creatures.push_back(c);
    return int(creatures.size()) - 1;
}

int CreatureDirector::spawnHunter(const Vec3& pos, float yaw, float scale, CreatureSize size, float hitPoints) {
    const int i = spawn(pos, yaw, scale, size, hitPoints, false);  // they charge, they don't stop to spit
    Creature& c = creatures[size_t(i)];
    c.hunter = true;
    c.state = CreatureState::Chase;
    c.spitWait = 2.0f + rnd() * 2.0f;
    return i;
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

// True if the creature could run `dist` metres along `heading` without meeting a building
// (knee-height ray) or ground too steep to climb.
bool CreatureDirector::pathClear(const Creature& c, float heading, float dist) const {
    const Vec3 d = headingDir(heading);
    if (collision) {
        float t;
        Vec3 n;
        if (collision->raycast(c.pos + Vec3{0, 0.4f * c.scale, 0}, d, dist, t, n)) return false;
    }
    if (groundHeight) {
        float prev = groundHeight(c.pos.x, c.pos.z);
        for (float s = 0.5f; s <= dist + 1e-3f; s += 0.5f) {
            const Vec3 p = c.pos + d * s;
            const float g = groundHeight(p.x, p.z);
            if (g - prev > params.maxSlope * 0.5f) return false;  // rise over 0.5 m steeper than the limit
            prev = g;
        }
    }
    return true;
}

bool CreatureDirector::damage(int i, float amount, const Vec3& dir, std::vector<CreatureEvent>* events) {
    if (i < 0 || size_t(i) >= creatures.size()) return false;
    Creature& c = creatures[size_t(i)];
    if (c.state == CreatureState::Dead) return false;
    c.health -= amount;
    if (c.health <= 0) {
        c.state = CreatureState::Dead;
        c.stateTime = 0;
        c.speed = 0;
        c.exploded = rnd() < params.burstChance;
        setClip(c, CreatureClip::Die, false);
        if (events) {
            CreatureEvent e{CreatureEvent::Die, i, 0, c.pos, c.size, c.exploded, dir};
            events->push_back(e);
        }
        return true;
    }
    // A hit interrupts everything but a lunge in flight.
    if (c.state != CreatureState::Lunge) {
        c.state = CreatureState::Hit;
        c.stateTime = 0;
        c.speed = 0;
        setClip(c, amount >= 0.5f * c.maxHealth ? CreatureClip::Hit : CreatureClip::HitSmall, false);
        c.clipTime = 0;
        c.fade = std::min(c.fade, params.crossfade * 0.5f);
    }
    if (events) events->push_back({CreatureEvent::Hurt, i, 0, c.pos, c.size});
    return false;
}

void CreatureDirector::launchSpit(int i, const Creature& c, const Vec3& target, std::vector<CreatureEvent>& events) {
    Vec3 o = c.pos + Vec3{0, 0.6f * c.scale, 0} + headingDir(c.yaw) * (0.8f * c.scale);
    if (mouth) mouth(i, o);
    Vec3 aim = target;
    const float spread = 0.15f + 0.02f * length(target - o);
    aim.x += (rnd() - 0.5f) * 2.0f * spread;
    aim.z += (rnd() - 0.5f) * 2.0f * spread;
    const Vec3 h{aim.x - o.x, 0, aim.z - o.z};
    const float d = std::max(0.5f, std::sqrt(h.x * h.x + h.z * h.z));
    const float tFlight = std::max(0.3f, d / params.spitSpeed);
    const float vy = (aim.y - o.y) / tFlight + 0.5f * params.spitGravity * tFlight;
    spits.push_back({o, h * (params.spitSpeed / d) + Vec3{0, vy, 0}, c.size, 0.0f});
    events.push_back({CreatureEvent::SpitLaunch, i, 0, o, c.size});
}

void CreatureDirector::updateSpits(float dt, const Vec3& player, bool playerAlive, std::vector<CreatureEvent>& events) {
    const Vec3 pa = player + Vec3{0, 0.3f, 0}, pb = player + Vec3{0, 1.6f, 0};
    for (auto& s : spits) {
        s.age += dt;
        const Vec3 step = s.vel * dt + Vec3{0, -0.5f * params.spitGravity * dt * dt, 0};
        s.vel.y -= params.spitGravity * dt;
        const float len = length(step);
        bool burst = s.age > 4.0f;
        Vec3 at = s.pos + step;
        if (len > 1e-5f) {
            const Vec3 dir = step * (1.0f / len);
            float t;
            Vec3 n;
            if (collision && collision->raycast(s.pos, dir, len, t, n)) { burst = true; at = s.pos + dir * t; }
            if (playerAlive && rayCapsule(s.pos, dir, len, pa, pb, 0.45f, t)) { burst = true; at = s.pos + dir * t; }
        }
        if (!burst && groundHeight && at.y <= groundHeight(at.x, at.z)) {
            burst = true;
            at.y = groundHeight(at.x, at.z);
        }
        s.pos = at;
        if (!burst) continue;
        // Splash damage falls off from the blast centre to the player's body axis.
        const int k = int(s.size);
        const Vec3 axis{player.x, std::max(pa.y, std::min(pb.y, at.y)), player.z};
        const float d = length(at - axis);
        const float r = params.spitRadius[k];
        const float dmg = playerAlive && r > 0 && d < r ? params.spitDamage[k] * (1.0f - 0.7f * d / r) : 0.0f;
        events.push_back({CreatureEvent::SpitBurst, -1, dmg, at, s.size});
        s.age = 1e9f;  // remove
    }
    spits.erase(std::remove_if(spits.begin(), spits.end(), [](const CreatureSpit& s) { return s.age >= 1e8f; }),
                spits.end());
}

void CreatureDirector::update(float dt, const Vec3& player, bool playerAlive, std::vector<CreatureEvent>& events) {
    events.clear();
    for (size_t i = 0; i < creatures.size(); ++i) {
        Creature& c = creatures[i];
        c.stateTime += dt;
        c.lungeWait = std::max(0.0f, c.lungeWait - dt);
        c.biteWait = std::max(0.0f, c.biteWait - dt);
        c.spitWait = std::max(0.0f, c.spitWait - dt);
        c.detourTime -= dt;
        if (c.detourTime <= 0) c.detour = 0;
        const float s = c.scale, ss = std::sqrt(s);
        const Vec3 to{player.x - c.pos.x, 0, player.z - c.pos.z};
        const float dist = std::sqrt(to.x * to.x + to.z * to.z);
        const float faceYaw = headingTo(to);
        const float fromHome = std::hypot(c.pos.x - c.home.x, c.pos.z - c.home.z);
        auto turn = [&](float target, float rate) {
            const float d = angleDelta(c.yaw, target), st = rate * dt;
            c.yaw = angleDelta(0.0f, c.yaw + (std::fabs(d) <= st ? d : (d > 0 ? st : -st)));
        };
        // Steering toward a goal: avoids buildings and cliffs, turns before running, eases speed.
        auto steer = [&](const Vec3& goal, float topSpeed) {
            const Vec3 g{goal.x - c.pos.x, 0, goal.z - c.pos.z};
            float heading = headingTo(g);
            if (c.detourTime > 0) {
                heading += c.detour;
            } else if (!pathClear(c, heading, std::min(2.5f * s, std::sqrt(g.x * g.x + g.z * g.z)))) {
                // Probe alternatives, preferring this creature's side, and keep the choice a moment.
                const float side = std::sin(c.surround) >= 0 ? 1.0f : -1.0f;
                for (float off : {0.6f, 1.2f, 1.8f, 2.4f}) {
                    for (float sg : {side, -side}) {
                        if (pathClear(c, heading + sg * off, 2.0f * s)) {
                            c.detour = sg * off;
                            c.detourTime = 0.8f;
                            break;
                        }
                    }
                    if (c.detourTime > 0) break;
                }
                heading += c.detour;
            }
            turn(heading, params.turnRate);
            const float err = std::fabs(angleDelta(c.yaw, heading));
            const float facing = std::max(0.0f, std::cos(err));
            const float want = topSpeed * facing * facing;  // nearly stops to turn sharply
            c.speed += std::max(-params.accel * dt, std::min(params.accel * dt, want - c.speed));
            if (c.speed > 0.01f) {
                if (tryMove(c, headingDir(c.yaw) * (c.speed * dt))) {
                    c.blockedTime = 0;
                } else {
                    c.speed = 0;
                    c.blockedTime += dt;
                    if (c.blockedTime > 0.25f) {  // cliff or wall right ahead: go around
                        c.detour = (c.detour > 0 ? -1.2f : 1.2f);
                        c.detourTime = 1.2f;
                    }
                }
            }
            const float runRef = params.runSpeed * ss;
            setClip(c, CreatureClip::Walk, true, std::max(0.8f, params.runPlayback * c.speed / runRef));
        };
        const float reach = params.reach * s;
        const bool sees = playerAlive && dist < params.sightRange * ss;
        switch (c.state) {
            case CreatureState::Idle: {
                setClip(c, CreatureClip::Idle, true);
                c.speed = 0;
                if (c.alertIn >= 0) c.alertIn -= dt;
                const bool called = c.alertIn >= 0 && c.alertIn <= dt && playerAlive;
                if (sees || called || (c.hunter && playerAlive)) {
                    c.state = CreatureState::Chase;
                    c.stateTime = 0;
                    c.alertIn = -1;
                    events.push_back({CreatureEvent::Alert, int(i), 0, c.pos, c.size});
                    // The pack joins in.
                    for (auto& o : creatures)
                        if (&o != &c && o.state == CreatureState::Idle && o.alertIn < 0 &&
                            length(o.pos - c.pos) < params.packRange)
                            o.alertIn = 0.2f + 0.6f * rnd();
                }
                break;
            }
            case CreatureState::Chase: {
                if (!playerAlive || (!c.hunter && fromHome > params.leash && dist > params.sightRange * ss)) {
                    c.state = CreatureState::Return;
                    break;
                }
                if (dist <= reach) {
                    turn(faceYaw, params.turnRate);
                    c.speed = 0;
                    if (c.biteWait > 0) {  // catching its breath between bites
                        setClip(c, CreatureClip::Idle, true);
                        break;
                    }
                    c.state = CreatureState::Attack;
                    c.stateTime = 0;
                    c.struck = false;
                    setClip(c, CreatureClip::Attack, false);
                    c.clipTime = 0;
                    events.push_back({CreatureEvent::Swing, int(i), 0, c.pos, c.size});
                    break;
                }
                const bool facing = std::fabs(angleDelta(c.yaw, faceYaw)) < 0.35f;
                // Spit from mid-range (or when it can't find a way through), with a clear line.
                const bool stuck = c.blockedTime > 1.5f;
                if (c.canSpit && c.spitWait <= 0 && facing && dist <= params.spitMax * ss &&
                    (dist >= params.spitMin * ss || stuck)) {
                    float t;
                    Vec3 n;
                    const Vec3 eye = c.pos + Vec3{0, 0.6f * s, 0};
                    const Vec3 tgt = player + Vec3{0, 1.2f, 0};
                    const bool blocked = collision && collision->raycast(eye, normalize(tgt - eye), length(tgt - eye), t, n);
                    if (!blocked && (stuck || rnd() < dt * 2.0f)) {
                        c.state = CreatureState::Spit;
                        c.stateTime = 0;
                        c.struck = false;
                        c.speed = 0;
                        setClip(c, CreatureClip::Spit, false);
                        c.clipTime = 0;
                        break;
                    }
                }
                if (facing && c.lungeWait <= 0 && dist >= params.lungeMin * s && dist <= params.lungeMax * s &&
                    rnd() < dt * 1.5f) {
                    c.state = CreatureState::Lunge;
                    c.stateTime = 0;
                    c.struck = false;
                    c.lungeWait = params.lungeCooldown;
                    setClip(c, CreatureClip::Lunge, false);
                    c.clipTime = 0;
                    events.push_back({CreatureEvent::Lunge, int(i), 0, c.pos, c.size});
                    break;
                }
                // Close in from this creature's own side so a pack spreads around the player.
                Vec3 goal = player;
                if (dist < 8.0f * s) {
                    const Vec3 ring{std::sin(c.surround), 0, std::cos(c.surround)};
                    goal = player + ring * (0.8f * reach);
                }
                steer(goal, params.runSpeed * ss);
                break;
            }
            case CreatureState::Attack: {
                turn(faceYaw, params.turnRate * 0.5f);
                const float dur = clipDuration[int(CreatureClip::Attack)];
                if (!c.struck && c.clipTime >= dur * params.biteAt) {
                    c.struck = true;
                    if (playerAlive && dist <= reach + 0.4f * s)
                        events.push_back({CreatureEvent::Strike, int(i), params.biteDamage * ss, c.pos, c.size});
                }
                if (c.clipTime >= dur * 0.98f) {
                    c.state = CreatureState::Chase;
                    c.stateTime = 0;
                    c.biteWait = params.bitePause;
                }
                break;
            }
            case CreatureState::Lunge: {
                // Airborne between 25% and 70% of the clip: travels toward where it aimed.
                const float dur = clipDuration[int(CreatureClip::Lunge)];
                const float f = c.clipTime / std::max(dur, 1e-3f);
                if (f > 0.25f && f < 0.7f) tryMove(c, headingDir(c.yaw) * (params.lungeSpeed * ss * dt));
                if (!c.struck && f >= 0.55f) {
                    c.struck = true;
                    if (playerAlive && dist <= reach + 0.6f * s)
                        events.push_back({CreatureEvent::Strike, int(i), params.lungeDamage * ss, c.pos, c.size});
                }
                if (f >= 0.98f) {
                    c.state = CreatureState::Chase;
                    c.stateTime = 0;
                }
                break;
            }
            case CreatureState::Spit: {
                turn(faceYaw, params.turnRate);
                const float dur = clipDuration[int(CreatureClip::Spit)];
                if (!c.struck && c.clipTime >= dur * params.spitAt) {
                    c.struck = true;
                    launchSpit(int(i), c, player + Vec3{0, 1.1f, 0}, events);
                }
                if (c.clipTime >= dur * 0.98f) {
                    c.state = CreatureState::Chase;
                    c.stateTime = 0;
                    c.blockedTime = 0;
                    c.spitWait = params.spitCooldown + rnd() * 2.0f;
                }
                break;
            }
            case CreatureState::Hit:
                c.speed = 0;
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
                if (std::hypot(c.home.x - c.pos.x, c.home.z - c.pos.z) < 0.6f) {
                    c.state = CreatureState::Idle;
                    break;
                }
                steer(c.home, params.walkSpeed * ss);
                if (c.stateTime > 30.0f) {  // can't get back: settle where it is
                    c.state = CreatureState::Idle;
                    c.home = c.pos;
                }
                break;
            }
            case CreatureState::Dead:
                if (c.exploded) { c.remove = true; break; }  // the game layer shows the gore
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
    updateSpits(dt, player, playerAlive, events);
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
