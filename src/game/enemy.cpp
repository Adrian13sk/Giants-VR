#include "game/enemy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
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
    e.uid = nextUid_++;
    if (params.marksmen > 0 && float(nextRand() % 1000) < params.marksmen * 1000.0f) e.role = 1;
    else if (params.crouchers > 0 && float(nextRand() % 1000) < params.crouchers * 1000.0f) e.role = 2;
    e.home = e.patrolTo = e.pos;
    e.patrolWait = 2.0f + float(nextRand() % 4000) * 0.001f;
    enemies.push_back(e);
    return int(enemies.size()) - 1;
}

size_t EnemyDirector::aliveCount() const {
    size_t n = 0;
    for (const auto& e : enemies) n += e.state != EnemyState::Dead;
    return n;
}

void EnemyDirector::setClip(Enemy& e, EnemyClip c, bool loop) {
    if (e.actT > 0) return;  // an action (punch, throw) plays out first
    e.loopClip = loop;
    if (c == e.clip) return;
    e.prevClip = e.clip;
    e.prevTime = e.clipTime;
    e.clip = c;
    e.clipTime = 0;
    e.fade = 0;
}

void EnemyDirector::playAction(int i, EnemyClip c) {
    if (i < 0 || size_t(i) >= enemies.size()) return;
    Enemy& e = enemies[size_t(i)];
    if (e.state == EnemyState::Dead || e.state == EnemyState::Falling) return;
    e.actT = 0;
    setClip(e, c, false);
    e.clipTime = 0;
    e.actT = clipDuration[int(c)];
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

bool EnemyDirector::lineOfSight(const Vec3& from, const Vec3& to) const {
    const Vec3 d = to - from;
    const float len = length(d);
    if (len < 1e-3f) return true;
    if (groundHeight) {
        const int n = std::max(2, int(len / 3.0f));
        for (int k = 1; k < n; ++k) {
            const Vec3 p = from + d * (float(k) / float(n));
            if (groundHeight(p.x, p.z) > p.y + 0.2f) return false;
        }
    }
    float t;
    Vec3 nn;
    return !(collision && collision->raycast(from, d * (1.0f / len), len, t, nn));
}

void EnemyDirector::knockOff(int i, const Vec3& v) {
    if (i < 0 || size_t(i) >= enemies.size()) return;
    Enemy& e = enemies[size_t(i)];
    if (e.state == EnemyState::Dead || e.state == EnemyState::Falling) return;
    e.posted = false;
    e.roofed = false;
    e.wallHit = 0;
    e.postObject = -1;   // (off his tower: its fall later doesn't knock him over again wherever he is)
    e.dropping = false;  // (a hover pack's descent would keep steering him)
    e.actT = 0;
    e.punchPending = false;
    e.state = EnemyState::Falling;
    e.stateTime = 0;
    e.vel = v;
    e.fallFrom = e.pos.y;
    e.aware = true;
    setClip(e, EnemyClip::FallCycle, true);
}

void EnemyDirector::push(int i, const Vec3& impulse) {
    if (i < 0 || size_t(i) >= enemies.size()) return;
    Enemy& e = enemies[size_t(i)];
    e.kick = e.kick + impulse;  // (the dead too: a ragdoll takes it)
    if (e.state == EnemyState::Dead || e.posted) return;
    e.vel = e.vel + Vec3{impulse.x, 0, impulse.z};
}

void EnemyDirector::alertNear(const Vec3& at, float radius) {
    for (auto& e : enemies)
        if (e.state == EnemyState::Idle && !e.aware && length(e.pos - at) < radius) {
            e.aware = true;
            e.alert = 0.3f + float(nextRand() % 600) * 0.001f;
        }
}

bool EnemyDirector::damage(int i, float amount, bool blast) {
    if (i < 0 || size_t(i) >= enemies.size()) return false;
    Enemy& e = enemies[size_t(i)];
    if (e.state == EnemyState::Dead) return false;
    e.health -= amount;
    e.hitFlash = 1.0f;
    e.aware = true;
    e.actT = 0;
    if (e.dropping && e.health <= 0) e.dropping = false;  // (falls from the sky: see update)
    e.punchPending = false;
    // (2026-10-28 review) In the air - held by Delphi's FOLLOW or Kabuto, thrown, knocked off a tower - and still alive:
    // he flinches but keeps falling (the Hit state would put him on the ground at once, from any height).
    if (e.state == EnemyState::Falling && e.health > 0) return false;
    if (e.health <= 0) {
        e.state = EnemyState::Dead;
        e.stateTime = 0;
        // Rework: the death fits the hit - a head shot (rpman_hit_eye_die), the legs shot from under
        // him (rpman_hit_to_knees_die), otherwise one of the three body-hit deaths.
        EnemyClip die = EnemyClip(int(EnemyClip::Die1) + int(nextRand() % 3));
        if (e.hitHeight > 0.8f) die = EnemyClip::DieHead;
        else if (e.hitHeight < 0.4f) die = EnemyClip::DieKnees;
        setClip(e, blast ? EnemyClip::Blown : die, false);
        e.hitHeight = 0.5f;
        return true;
    }
    e.hitHeight = 0.5f;
    e.state = EnemyState::Hit;
    e.stateTime = 0;
    // Rework: a heavy blow (a rocket's edge, the shotgun up close) knocks him off his feet; he gets
    // up again (rpman_hit_big, rpman_hit_big_getup).
    const EnemyClip hc = amount >= 45.0f && !e.posted ? EnemyClip::HitBig : ((nextRand() & 1) ? EnemyClip::Hit1 : EnemyClip::Hit2);
    setClip(e, hc, false);
    e.clipTime = 0;  // restart the reaction even if already playing
    e.fade = std::min(e.fade, params.crossfade * 0.5f);
    return false;
}

void EnemyDirector::update(float dt, const Vec3& player, bool playerAlive, std::vector<int>& shooters) {
    shooters.clear();
    pokeStarted = pokeFound = false;
    landed.clear();
    punches.clear();
    siegeShots.clear();
    for (size_t i = 0; i < enemies.size(); ++i) {
        Enemy& e = enemies[i];
        e.stateTime += dt;
        e.hitFlash = std::max(0.0f, e.hitFlash - dt * 6.0f);
        const Vec3 to{player.x - e.pos.x, 0, player.z - e.pos.z};
        const float dist = std::sqrt(to.x * to.x + to.z * to.z);
        const float faceYaw = std::atan2(-to.x, -to.z);
        auto turn = [&](float target) {
            const float d = angleDelta(e.yaw, target), step = params.turnRate * dt;
            e.yaw = angleDelta(0.0f, e.yaw + (std::fabs(d) <= step ? d : (d > 0 ? step : -step)));
        };
        // Engaged (aware) Reapers keep track of the player within sight range; an unaware one
        // must see him: in front, with a clear line, or hear him close by.
        const Vec3 eyeP = e.pos + Vec3{0, 1.6f, 0}, chest = player + Vec3{0, 1.2f, 0};
        // Line of sight (a ray through the collision world) is re-checked a few times a second, not
        // every frame per Reaper.
        e.losT -= dt;
        if (playerAlive && dist < params.sightRange && !e.aware && dist >= params.hearRange && e.losT <= 0) {
            e.los = lineOfSight(eyeP, chest);
            e.losT = 0.15f + float(nextRand() % 100) * 0.001f;
        }
        bool sees = playerAlive && dist < params.sightRange && (e.aware || dist < params.hearRange || e.los);
        if (playerHidden && e.aware && e.state != EnemyState::Dead) {
            // Engaged, but the player has gone into his bush: lost after a while unless close.
            e.lost = dist > 8.0f ? e.lost + dt : 0.0f;
            if (e.lost > 3.0f) {
                e.aware = false;
                e.lost = 0;
                if (e.state == EnemyState::Advance || e.state == EnemyState::Fire) { e.state = EnemyState::Idle; e.stateTime = 0; e.alert = 0; }
            }
            sees = sees && dist <= 8.0f;
        }
        // (2026-10-28 review) Hidden from one that hasn't found him (Delphi's cloak, the bush): not seen, however
        // close or in line of sight - only the poke or a dash nearby gives the player away.
        if (playerHidden && !e.aware) sees = false;
        // (test hook GVR_PASSIVE_REAPERS: they never notice the player - targets that stand still)
        static const bool passive = std::getenv("GVR_PASSIVE_REAPERS") != nullptr;
        if (passive) { sees = false; e.aware = false; }
        // Rework: a marksman keeps his distance and fires single aimed shots.
        const bool marks = e.role == 1;
        // (disarmed: no shooting - he runs right up to punch)
        const float shootR = e.disarmed ? 0.0f : (marks ? std::max(params.shootRange, 45.0f) : params.shootRange);
        const float fireR = e.disarmed ? 1.1f : (marks ? 26.0f : params.fireRange);
        // Shot in the leg: he limps along at under half speed for a while.
        e.limpT = std::max(0.0f, e.limpT - dt);
        const float legs = e.limpT > 0 ? 0.42f : 1.0f;
        const int burstN = marks ? 1 : params.burstSize;
        const float burstP = marks ? 2.6f : params.burstPause;
        (void)shootR; (void)fireR; (void)burstN; (void)burstP;
        // Rework: coming down on a hover pack - drifting toward the player, then on his feet and fighting.
        if (e.dropping && e.state != EnemyState::Dead) {
            const float g = groundHeight ? groundHeight(e.pos.x, e.pos.z) : 0.0f;
            setClip(e, EnemyClip::Hover, true);
            turn(faceYaw);
            if (dist > 6.0f) tryMove(e, Vec3{to.x, 0, to.z} * (3.0f * dt / std::max(dist, 1e-3f)));
            e.pos.y -= std::min(6.0f, 1.5f + 0.4f * (e.pos.y - g)) * dt;  // slowing as he nears the ground
            if (e.pos.y <= g + 0.05f) {
                e.pos.y = g;
                e.dropping = false;
                e.state = EnemyState::Advance;
                e.stateTime = 0;
                e.flankTimer = 0.5f;
            }
            e.clipTime += dt;
            continue;
        }
        if (e.state == EnemyState::Dead && !e.posted && groundHeight && !e.ragdoll) {
            // Shot out of the air: he drops.
            const float g = groundHeight(e.pos.x, e.pos.z);
            if (e.pos.y > g + 0.05f) {
                e.vel.y -= 14.0f * dt;
                e.pos.y = std::max(g, e.pos.y + e.vel.y * dt);
                if (e.pos.y <= g) e.vel.y = 0;
            }
        }
        // Rework: an action clip (punch, throw) plays out; he turns to you meanwhile and the punch
        // lands a little before the middle of the swing if you are still in reach.
        e.punchCool = std::max(0.0f, e.punchCool - dt);
        if (e.actT > 0 && e.state != EnemyState::Dead && e.state != EnemyState::Falling) {
            e.actT -= dt;
            turn(faceYaw);
            if (e.punchPending && e.clip == EnemyClip::Punch && e.clipTime > clipDuration[int(EnemyClip::Punch)] * 0.42f) {
                e.punchPending = false;
                if (playerAlive && dist < 2.1f && std::fabs(player.y - e.pos.y) < 2.0f) punches.push_back(int(i));  // (not one hovering over him)
            }
            e.clipTime += dt;
            e.clipTime = std::min(e.clipTime, clipDuration[int(e.clip)] * 0.999f);
            continue;
        }
        // Rework: you right up against him - he punches (rpman_punch) instead of firing point blank.
        // (Not at a bush he hasn't seen through yet: the poke is how he finds you - 2026-10-28.)
        if (params.punch && playerAlive && sees && dist < 1.7f && std::fabs(player.y - e.pos.y) < 2.0f && e.punchCool <= 0 && !e.posted && !(playerHidden && !e.aware) &&
            (e.state == EnemyState::Advance || e.state == EnemyState::Fire || e.state == EnemyState::Idle)) {
            setClip(e, EnemyClip::Punch, false);
            e.clipTime = 0;
            e.actT = clipDuration[int(EnemyClip::Punch)];
            e.punchCool = e.actT + 1.4f;
            e.punchPending = true;
            e.aware = true;
            continue;
        }
        switch (e.state) {
            case EnemyState::Idle: {
                if (!playerAlive) e.aware = false;
                if (e.siege && e.alert <= 0 && e.poke < 0) {
                    // Firing at a building: bursts as at the player, aimed at the target.
                    turn(std::atan2(-(e.siegeAt.x - e.pos.x), -(e.siegeAt.z - e.pos.z)));
                    setClip(e, EnemyClip::Fire, true);
                    e.fireCooldown -= dt;
                    if (e.fireCooldown <= 0) {
                        if (e.burstLeft <= 0) e.burstLeft = params.burstSize;
                        siegeShots.push_back(int(i));
                        e.fireCooldown = --e.burstLeft > 0 ? params.fireInterval : params.burstPause;
                    }
                    break;
                }
                if (e.alert > 0) {
                    // Noticed: stops, turns to the player with the alert scan, then acts.
                    setClip(e, EnemyClip::Alert, true);
                    turn(faceYaw);
                    e.alert -= dt;
                    if (e.alert <= 0) { e.state = EnemyState::Advance; e.stateTime = 0; }
                    break;
                }
                if (e.poke >= 0) {
                    // A suspicious bush: walks up to it, pokes it (the clip), and finds the player
                    // if he is still there when the poke lands.
                    e.poke += dt;
                    if (!playerHidden || !playerAlive) { e.poke = -1; break; }
                    // (up to 2 m: the bodies keep ~1.6 m apart, so "closer than 1.6" never came and he walked on the spot)
                    if (dist > 2.0f && e.poke < 6.0f) {
                        setClip(e, EnemyClip::Walk, true);
                        turn(faceYaw);
                        tryMove(e, Vec3{-std::sin(e.yaw), 0, -std::cos(e.yaw)} * (params.patrolSpeed * dt));
                        break;
                    }
                    turn(faceYaw);
                    setClip(e, EnemyClip::Poke, false);
                    if (e.clipTime >= clipDuration[int(EnemyClip::Poke)] * 0.55f) {
                        e.poke = -1;
                        if (dist < 2.6f) {
                            e.aware = true;
                            e.alert = 0.2f;
                            pokeFound = true;
                        } else {
                            setClip(e, EnemyClip::Idle, true);
                        }
                    }
                    break;
                }
                e.lookTimer -= dt;
                if (playerAlive && playerHidden && e.aware && dist < 8.0f) {
                    e.alert = 0.2f;  // knows he's there: comes for him
                    break;
                }
                if (playerAlive && e.lookTimer <= 0 && playerHidden && !e.aware) {
                    // Hidden: only a bush close by (or one moving about) catches its eye.
                    e.lookTimer = 0.3f;
                    const bool front = std::fabs(angleDelta(e.yaw, faceYaw)) < params.noticeAngle;
                    if (dist < 3.5f || (playerMoving && front && dist < 9.0f)) {
                        e.poke = 0;
                        pokeStarted = true;
                    }
                }
                if (playerAlive && e.lookTimer <= 0 && !playerHidden) {
                    e.lookTimer = 0.2f + float(nextRand() % 100) * 0.001f;
                    const bool front = std::fabs(angleDelta(e.yaw, faceYaw)) < params.noticeAngle;
                    if (e.aware ? dist < params.sightRange
                                : (dist < params.hearRange || (dist < params.sightRange && front && e.los))) {
                        e.aware = true;
                        e.alert = e.posted ? 0.3f : params.noticeDelay;
                        // Its squad hears it.
                        for (auto& o : enemies)
                            if (&o != &e && o.state == EnemyState::Idle && !o.aware && length(o.pos - e.pos) < 18.0f) {
                                o.aware = true;
                                o.alert = params.noticeDelay + float(nextRand() % 800) * 0.001f;
                            }
                        break;
                    }
                }
                if (e.posted) { setClip(e, EnemyClip::Idle, true); break; }
                // Patrol: a short walk to a nearby spot, a look around, back again.
                if (e.patrolWait > 0) {
                    setClip(e, EnemyClip::Idle, true);
                    e.patrolWait -= dt;
                    if (e.patrolWait <= 0) {
                        const float a = float(nextRand() % 6283) * 0.001f;
                        const float r = params.patrolRadius * (0.3f + float(nextRand() % 700) * 0.001f);
                        e.patrolTo = e.home + Vec3{std::sin(a) * r, 0, std::cos(a) * r};
                        e.stateTime = 0;
                    }
                    break;
                }
                const Vec3 g{e.patrolTo.x - e.pos.x, 0, e.patrolTo.z - e.pos.z};
                const float gd = std::sqrt(g.x * g.x + g.z * g.z);
                if (gd < 0.4f || e.stateTime > 10.0f) {
                    e.patrolWait = 3.0f + float(nextRand() % 4000) * 0.001f;
                    break;
                }
                setClip(e, EnemyClip::Patrol, true);
                turn(std::atan2(-g.x, -g.z));
                if (std::fabs(angleDelta(e.yaw, std::atan2(-g.x, -g.z))) < 0.5f &&
                    !tryMove(e, Vec3{-std::sin(e.yaw), 0, -std::cos(e.yaw)} * (params.patrolSpeed * dt)))
                    e.patrolWait = 1.0f;  // blocked: wait, then pick another spot
                break;
            }
            case EnemyState::Advance: {
                // Run (no shooting while moving) toward a spot beside the player, then stop and fire.
                if (!sees) { e.state = EnemyState::Idle; break; }
                if (e.posted) {  // a sentry fires from its post once the player is in range
                    turn(faceYaw);
                    setClip(e, EnemyClip::Idle, true);
                    if (dist <= shootR * 1.3f) {
                        e.state = EnemyState::Fire;
                        e.stateTime = 0;
                        e.burstLeft = burstN;
                        e.burstsLeft = params.burstsPerStop;
                        e.fireCooldown = marks ? 0.7f : 0.35f;  // (a marksman charges his shot: see game_app)
                    }
                    break;
                }
                e.flankTimer -= dt;
                const Vec3 dirTo = to * (1.0f / std::max(dist, 1e-3f));
                const Vec3 side{-dirTo.z, 0, dirTo.x};
                const float fl = e.disarmed ? 0.0f : e.flank;  // (fists: straight at you)
                Vec3 goal{player.x + side.x * fl - dirTo.x * fireR - e.pos.x, 0,
                          player.z + side.z * fl - dirTo.z * fireR - e.pos.z};
                // Rework: badly hurt, he falls back - away from the player and to one side - before he
                // turns to fight again.
                const bool retreating = e.retreatT > 0;
                // Cover chosen: run to it (given up if the player has moved round it).
                if (e.inCover && length(Vec3{player.x - e.coverFrom.x, 0, player.z - e.coverFrom.z}) > 10.0f) e.inCover = false;
                if (e.inCover && !retreating) goal = Vec3{e.coverAt.x - e.pos.x, 0, e.coverAt.z - e.pos.z};
                if (e.disarmed && e.fetchGun && !retreating) goal = Vec3{e.gunAt.x - e.pos.x, 0, e.gunAt.z - e.pos.z};  // (his gun, on the ground)
                if (retreating) {
                    e.retreatT -= dt;
                    goal = Vec3{-dirTo.x * 12.0f + side.x * e.flank, 0, -dirTo.z * 12.0f + side.z * e.flank};
                }
                const float goalDist = std::sqrt(goal.x * goal.x + goal.z * goal.z);
                const bool arrived = goalDist < 1.0f || dist <= fireR;
                // Stop to shoot when in range and the current run has lasted long enough (or arrived).
                if (!retreating && dist <= shootR && (arrived || e.flankTimer <= 0)) {
                    e.state = EnemyState::Fire;
                    e.stateTime = 0;
                    e.burstLeft = burstN;
                    e.burstsLeft = params.burstsPerStop;
                    e.fireCooldown = marks ? 0.7f : 0.35f;  // (a marksman charges his shot: see game_app)
                    break;
                }
                setClip(e, e.limpT > 0 ? EnemyClip::Walk : EnemyClip::Run, true);  // (limping: a walk)
                // Steer around cliffs: while detouring, the heading is offset from the goal.
                e.detourTime -= dt;
                if (e.detourTime <= 0) e.detour = 0;
                turn(std::atan2(-goal.x, -goal.z) + e.detour);
                const Vec3 fwd{-std::sin(e.yaw), 0, -std::cos(e.yaw)};
                if (tryMove(e, fwd * (params.runSpeed * legs * dt))) {
                    e.blockedTime = 0;
                } else {
                    // Can't climb this: try going around; if the player is in range, shoot instead.
                    e.blockedTime += dt;
                    if (e.blockedTime > 0.3f) {
                        if (dist <= shootR) {
                            e.state = EnemyState::Fire;
                            e.stateTime = 0;
                            e.burstLeft = burstN;
                            e.burstsLeft = params.burstsPerStop;
                            e.fireCooldown = marks ? 0.7f : 0.35f;  // (a marksman charges his shot: see game_app)
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
                if (e.disarmed) { e.state = EnemyState::Advance; e.stateTime = 0; e.inCover = false; break; }  // (nothing to fire)
                if (!sees) { e.state = EnemyState::Idle; break; }
                e.fireCooldown -= dt;
                if (e.burstLeft > 0) {
                    e.ducked = false;  // (up to fire: the next pause ducks again)
                    // Rework: pressing in - a Reaper not yet at his stand-off distance walks at you while
                    // he fires (rpman_walk_fire), instead of standing still to be shot.
                    const bool press = (params.pressIn && e.role != 1) && !e.posted && dist > fireR + 2.0f;
                    if (press) {
                        setClip(e, EnemyClip::Walk, true);
                        const Vec3 f{-std::sin(e.yaw), 0, -std::cos(e.yaw)};
                        tryMove(e, f * (params.walkSpeed * legs * dt));
                    } else {
                        setClip(e, e.weapon == 1 ? EnemyClip::BzkaFire : marks ? EnemyClip::KneelFire : (e.role == 2 || e.inCover ? EnemyClip::CrouchFire : EnemyClip::Fire), true);  // (a marksman kneels to aim; a croucher crouches)
                    }
                    const bool facing = std::fabs(angleDelta(e.yaw, faceYaw)) < 0.35f;
                    if (e.fireCooldown <= 0 && facing) {
                        shooters.push_back(int(i));
                        --e.burstLeft;
                        e.fireCooldown = e.burstLeft > 0 ? params.fireInterval : burstP;
                    }
                } else {
                    // Between bursts: a Reaper in cover ducks right down behind it (rpman_rock_into, then
                    // rpman_rock_breathe: rework); a croucher stays down (rpman_crouch_breathe).
                    if (e.inCover && e.weapon != 1) {
                        // Behind a tree he steps in and stands flat against the trunk, a peek out now
                        // and then (rpman_behind_tree_hide / _breathe / _peek); behind a rock he ducks.
                        const EnemyClip into = e.treeCover ? EnemyClip::TreeHide : EnemyClip::RockInto;
                        const EnemyClip hold = e.treeCover ? EnemyClip::TreeBreathe : EnemyClip::RockBreathe;
                        if (!e.ducked) { setClip(e, into, false); e.ducked = true; }
                        else if (e.clip == into && e.clipTime >= clipDuration[int(into)] * 0.97f) setClip(e, hold, true);
                        else if (e.treeCover && e.clip == hold && e.fireCooldown > 1.2f && (nextRand() % 1000) < 4) setClip(e, EnemyClip::TreePeek, false);
                        else if (e.clip == EnemyClip::TreePeek && e.clipTime >= clipDuration[int(EnemyClip::TreePeek)] * 0.97f) setClip(e, hold, true);
                        else if (e.clip != into && e.clip != hold && e.clip != EnemyClip::TreePeek) setClip(e, hold, true);
                    } else {
                        setClip(e, e.role == 2 && e.weapon != 1 ? EnemyClip::CrouchIdle : EnemyClip::Idle, true);
                    }
                    if (e.fireCooldown <= 0) {
                        if (--e.burstsLeft > 0) {
                            e.burstLeft = burstN;
                            if (marks) e.fireCooldown = 0.7f;
                        } else {
                            // Reposition: pick a new flank spot and run there - behind cover if there's
                            // some near it (rework).
                            e.flank = (float(nextRand() % 2001) / 1000.0f - 1.0f) * 10.0f;
                            e.flankTimer = 1.5f + float(nextRand() % 2000) / 1000.0f;
                            e.state = EnemyState::Advance;
                            e.stateTime = 0;
                            e.inCover = false;
                            if (findCover && !e.posted && float(nextRand() % 1000) < params.coverShare * 1000.0f) {
                                const Vec3 side{-to.z / std::max(dist, 1e-3f), 0, to.x / std::max(dist, 1e-3f)};
                                const Vec3 want = e.pos + side * e.flank;
                                Vec3 spot;
                                bool tree = false;
                                if (findCover(want, player, spot, tree) && length(Vec3{spot.x - e.pos.x, 0, spot.z - e.pos.z}) < 18.0f) {
                                    e.inCover = true;
                                    e.treeCover = tree;
                                    e.coverAt = spot;
                                    e.coverFrom = player;
                                    e.flankTimer = 6.0f;
                                    static const bool trace = std::getenv("GVR_COVER_TRACE") != nullptr;
                                    if (trace) LOGI("Reaper %u takes cover %.1f m away%s", e.uid, length(Vec3{spot.x - e.pos.x, 0, spot.z - e.pos.z}), tree ? " (a tree)" : "");
                                }
                            }
                        }
                    }
                }
                break;
            case EnemyState::Hit:
                if (e.clip == EnemyClip::HitBig && e.stateTime >= clipDuration[int(e.clip)] * 0.95f) {
                    setClip(e, EnemyClip::GetUp, false);  // down: up again
                    e.stateTime = 0;
                    break;
                }
                if (e.stateTime >= clipDuration[int(e.clip)] * 0.95f) {
                    // After being hit: usually run for cover to a new spot.
                    e.state = EnemyState::Advance;
                    e.stateTime = 0;
                    e.flank = (float(nextRand() % 2001) / 1000.0f - 1.0f) * 10.0f;
                    e.flankTimer = 1.0f + float(nextRand() % 1500) / 1000.0f;
                    if (e.health < 35.0f && !e.posted && nextRand() % 10 < 6) e.retreatT = 2.0f + float(nextRand() % 1500) / 1000.0f;
                }
                break;
            case EnemyState::Falling: {
                e.vel.y -= 14.0f * dt;
                const float prevY = e.pos.y;
                // Walls and rock: a body flying sideways (thrown, blown off a tower) stops against them
                // instead of passing through - a swept ray at chest height, so a fast throw can't skip a
                // thin wall between two frames. He bounces back a little and drops.
                const Vec3 hv{e.vel.x, 0, e.vel.z};
                const float hs = length(hv);
                if (collision && hs > 0.01f) {
                    const Vec3 dir = hv * (1.0f / hs);
                    const float reach = hs * dt + params.radius;
                    float t = 0;
                    Vec3 n;
                    if (collision->raycast(e.pos + Vec3{0, params.height * 0.45f, 0}, dir, reach, t, n)) {
                        e.pos += dir * std::max(0.0f, t - params.radius);
                        e.wallHit = std::max(e.wallHit, hs);
                        e.vel.x = -dir.x * hs * 0.15f;
                        e.vel.z = -dir.z * hs * 0.15f;
                    } else {
                        e.pos += hv * dt;
                    }
                } else {
                    e.pos += hv * dt;
                }
                e.pos.y += e.vel.y * dt;
                e.fallFrom = std::max(e.fallFrom, e.pos.y);  // (thrown upward: the fall counts from the top of the arc)
                float g = groundHeight ? groundHeight(e.pos.x, e.pos.z) : 0.0f;
                // A roof, wall top or ledge under him (one he was above a moment ago) catches him.
                float roof;
                bool onRoof = false;
                if (collision && e.vel.y <= 0 && collision->groundAt(e.pos.x, e.pos.z, prevY + 0.05f, roof) && roof > g + 0.4f) { g = roof; onRoof = true; }
                if (e.pos.y <= g) {
                    e.pos.y = g;
                    // On a roof he stays up there (a sentry where he lands; his body stays there): the
                    // terrain-following and ragdoll code below would drop him through it.
                    if (onRoof) { e.posted = true; e.postY = g; e.roofed = true; e.postObject = -1; }
                    const bool lethal = e.fallFrom - g > lethalFall;
                    if (lethal) e.kick = e.vel * 0.5f;  // (a ragdoll lands with it)
                    e.vel = {0, 0, 0};
                    e.stateTime = 0;
                    if (lethal) {
                        e.health = 0;
                        e.state = EnemyState::Dead;
                        setClip(e, EnemyClip::FallLand, false);
                        landed.push_back(int(i));
                    } else {
                        e.state = EnemyState::Hit;
                        setClip(e, EnemyClip::Hit1, false);
                    }
                }
                break;
            }
            case EnemyState::Dead:
                if (params.corpsesPersist) {
                    e.sink = e.eaten * 0.5f;  // sinks as the Cleaners eat it
                    if (e.gibbed || e.eaten >= 1.0f) e.remove = true;
                    break;
                }
                if (e.stateTime > params.corpseTime) e.sink += dt * 0.35f;
                if (e.sink > params.height) e.remove = true;
                break;
        }
        // Shoved by a blast: slides, the push dying away quickly.
        if (e.state != EnemyState::Falling && !e.ragdoll && (e.state != EnemyState::Dead || (e.stateTime < 1.5f && !e.posted)) && (e.vel.x != 0 || e.vel.z != 0)) {
            tryMove(e, Vec3{e.vel.x, 0, e.vel.z} * dt);
            e.vel = e.vel * std::max(0.0f, 1.0f - 5.0f * dt);
            if (std::fabs(e.vel.x) + std::fabs(e.vel.z) < 0.05f) e.vel = {0, 0, 0};
        }
        // Ground too steep to stand on: slide downhill (as the player does).
        // Fell onto a roof that has since gone (wrecked, burnt): he drops.
        if (e.roofed && e.posted && collision) {
            float y;
            if (!collision->groundAt(e.pos.x, e.pos.z, e.postY + 0.05f, y) || y < e.postY - 0.3f) {
                e.roofed = false;
                if (e.state == EnemyState::Dead) e.posted = false;
                else knockOff(int(i), Vec3{0, 0.5f, 0});
            }
        }
        if (e.posted) {
            e.pos.y = e.postY - e.sink;
        } else if (e.state != EnemyState::Dead && groundHeight) {
            const float h = 0.5f;
            const float gx = (groundHeight(e.pos.x + h, e.pos.z) - groundHeight(e.pos.x - h, e.pos.z)) / (2 * h);
            const float gz = (groundHeight(e.pos.x, e.pos.z + h) - groundHeight(e.pos.x, e.pos.z - h)) / (2 * h);
            const float grade = std::sqrt(gx * gx + gz * gz);
            if (grade > params.maxSlope && e.state != EnemyState::Falling) e.pos += Vec3{-gx / grade, 0, -gz / grade} * (params.slideSpeed * dt);
        }
        // Ground and buildings.
        if (!e.posted && e.state != EnemyState::Falling) {
            if (e.state != EnemyState::Dead && collision) collision->pushOut(e.pos, params.radius, params.height, 0.45f);
            if (groundHeight) e.pos.y = groundHeight(e.pos.x, e.pos.z) - e.sink;
        }
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
            if (p.state == EnemyState::Dead || q.state == EnemyState::Dead || p.posted || q.posted) continue;
            if (p.state == EnemyState::Falling || q.state == EnemyState::Falling) continue;  // (one in the air doesn't shove those under him)
            Vec3 d{q.pos.x - p.pos.x, 0, q.pos.z - p.pos.z};
            const float l = std::sqrt(d.x * d.x + d.z * d.z), minD = 2.0f * params.radius + 0.2f;
            if (l < minD && l > 1e-4f) {
                d = d * ((minD - l) * 0.5f / l);
                p.pos = p.pos - d;
                q.pos += d;
            }
        }
    // (2026-10-28 review) The frame's index lists are made before the removed are erased: carried across by uid.
    auto keepUids = [this](std::vector<int>& v) {
        std::vector<uint32_t> u;
        for (int k : v)
            if (k >= 0 && size_t(k) < enemies.size()) u.push_back(enemies[size_t(k)].uid);
        return u;
    };
    const std::vector<uint32_t> landedU = keepUids(landed), punchU = keepUids(punches);
    enemies.erase(std::remove_if(enemies.begin(), enemies.end(), [](const Enemy& e) { return e.remove; }), enemies.end());
    auto back = [this](const std::vector<uint32_t>& u, std::vector<int>& v) {
        v.clear();
        for (uint32_t id : u)
            for (size_t k = 0; k < enemies.size(); ++k)
                if (enemies[k].uid == id) { v.push_back(int(k)); break; }
    };
    back(landedU, landed);
    back(punchU, punches);
}

bool EnemyDirector::raycast(const Vec3& o, const Vec3& d, float maxDist, float& t, Vec3& normal, int& index) const {
    bool hit = false;
    float best = maxDist;
    for (size_t i = 0; i < enemies.size(); ++i) {
        const Enemy& e = enemies[i];
        if (e.state == EnemyState::Dead) continue;  // shots pass over corpses
        float th;
        const Vec3 a = e.pos + Vec3{0, params.radius, 0}, b = e.pos + Vec3{0, bodyHeight(e) - params.radius, 0};
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
