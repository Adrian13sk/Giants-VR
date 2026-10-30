// GameApp: Combat: Reaper waves, the player's damage and death, blasts, rockets, nests, fish schools, the fight update. Same class; see game_app.h.
// (Split out of game_app.cpp, 2026-10-28: moved as it was.)
#include "game/game_app.h"
#include "game/hero_clips.h"
#include "core/log.h"
#include "core/prof.h"
#include "scene/asset_world.h"
#include "audio/mixer.h"
#include "audio/music.h"
#include "assets/gtext.h"
#include "assets/skin_anim.h"
#include "game/cutscene.h"
#include "game/arm_ik.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"
#include <algorithm>
#include <fstream>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>

namespace gvr {

// Weapon target ids >= this are creatures (index = id - kCreatureTarget); lower ids are Reapers.
static constexpr int kCreatureTarget = 1000;
void GameApp::spawnWave() {
    if (enemyPool_.empty()) return;
    ++wave_;
    // Three Reapers ~35 m out in front of the player (spread left/right), on solid terrain.
    const float base = player_.yaw;
    int placed = 0;
    for (int attempt = 0; attempt < 24 && placed < 3; ++attempt) {
        const float a = base + (frand() - 0.5f) * 1.4f;
        const float r = 30.0f + frand() * 10.0f;
        const Vec3 p = player_.position + Vec3{-std::sin(a), 0, -std::cos(a)} * r;
        float h;
        if (!assetWorld_->terrain().heightAt(p.x, p.z, h)) continue;
        if (h < 0.5f) continue;  // not in the sea
        enemies_.spawn(p, a + kPi);  // facing the player
        ++placed;
    }
    LOGI("Wave %d: %d Reapers", wave_, placed);
}

void GameApp::damagePlayer(float amount, DeathCause cause) {
    if (playerHealth_ <= 0 || dying_) return;
    // (counted even in god mode: devtest delphi_cloak_fight; not the first half second - bolts already in flight
    // when she cast it land, as they would)
    if (cloakT_ > 0 && cloakT_ < 11.5f) {
        ++cloakHits_;
        static const bool trace = std::getenv("GVR_SPELL_TRACE") != nullptr;
        if (trace) LOGI("spell trace: hit while cloaked (%.0f, cause %d, cloak %.1f s left)", double(amount), int(cause), double(cloakT_));
    }
    {
        // Baz's shield takes what it can - before god mode, so a test can see it; counted in the damage the
        // difficulty makes of the hit (its 150 is the same on every level).
        const float k = std::max(0.01f, difficultyDamage());
        amount = shieldAbsorb(amount * k, cause) / k;
        if (amount <= 0) {
            if (shieldT_ > 0) haptics_.push_back({rig_.turnHand(), 0.25f, 0.04f});
            return;
        }
    }
    static const bool god = std::getenv("GVR_GOD") != nullptr;  // test hook: the player can't be hurt (perf runs)
    if (god) return;
    if (hero_ == Hero::Kabuto) amount *= 0.3f;  // a giant's hide: Reaper bolts and bites sting
    amount *= difficultyDamage();               // (2026-10-28) Options -> DIFFICULTY
    playerHealth_ -= amount;
    static const bool dtrace = std::getenv("GVR_DAMAGE_TRACE") != nullptr;  // test hook
    if (dtrace) LOGI("player hurt %.1f (cause %d), health %.0f", amount, int(cause), playerHealth_);
    lastHurt_ = cause;
    if (playerHealth_ < 45.0f && playerHealth_ > 0 && cause != DeathCause::Fall) timmySay(frand() < 0.5f ? "smchbaseattack4" : "smchbaseattack1");
    sinceDamage_ = 0;
    damageFlash_ = 1.0f;
    if (hurtCooldown_ <= 0) {
        static const char* hurt[] = {"me_shrt1", "me_shrt2", "me_shrt3"};
        sfx2D(hero_ == Hero::Kabuto ? "ka_wound" : hurt[int(frand() * 2.99f)], 0.8f);
        hurtCooldown_ = 0.6f;
    }
    // A jolt in the hands, stronger for a bigger hit and on the side it came from.
    {
        const float a = std::min(1.0f, 0.35f + amount / 40.0f);
        float side = 0.0f;  // -1 left .. +1 right
        if (hitDirTimer_ > 1.1f && lastInput_.headValid) {
            const Pose head = rig_.toWorld(lastInput_.head);
            Vec3 d = hitFrom_ - head.position;
            d.y = 0;
            if (length(d) > 1e-3f) side = dot(normalize(d), head.orientation.rotate(Vec3{1, 0, 0}));
        }
        haptics_.push_back({kLeft, a * (1.0f - 0.45f * std::max(0.0f, side)), 0.12f});
        haptics_.push_back({kRight, a * (1.0f + 0.45f * std::min(0.0f, side)), 0.12f});
    }
    // The player bleeds too (bites and bullets).
    if (cause != DeathCause::Drowned) bleed(player_.position + Vec3{0, 1.2f, 0}, Vec3{frand() - 0.5f, 0.3f, frand() - 0.5f}, false, 1.0f);
    if (playerHealth_ <= 0) startDeath();
}

void GameApp::startDeath() {
    playerHealth_ = 0;
    dying_ = true;
    endCloak("she fell");
    if (gyro_.riding) gyroOut("Baz is down");
    stopRepairLoop();
    if (shieldT_ > 0) { shieldT_ = 0; shieldCool_ = 40.0f; }
    enemies_.playerHidden = enemies_.playerMoving = false;
    repairOn_ = false;
    telPhase_ = 0;      // (2026-10-29 review: a blink begun as she died froze half dark, and finished after the respawn)
    telFade_ = 0;
    telAiming_ = false;
    rsBeam_ = false;  // (Delphi's REPAIR: its HUD pulse and dwell, 2026-10-29 review)
    rsAimT_ = 0;
    ++statDeaths_;
    deathTime_ = 0;
    deathCause_ = player_.swimming() ? DeathCause::Drowned : lastHurt_;
    deathWasFp_ = firstPerson_;
    firstPerson_ = false;  // see the body: the camera steps back from it
    tpAiming_ = false;
    armTargets_[0].valid = armTargets_[1].valid = false;
    haptics_.push_back({kLeft, 1.0f, 0.4f});
    haptics_.push_back({kRight, 1.0f, 0.4f});
    if (deathCause_ == DeathCause::Blast && hero_ != Hero::Mecc) deathCause_ = DeathCause::Fall;  // only the Meccaryn has chunks
    if (deathCause_ == DeathCause::Blast) {
        // Blown apart: the original Meccaryn chunks (MC_chunk1..6), blood everywhere.
        playerGibbed_ = true;
        const Vec3 at = player_.position + Vec3{0, 1.0f, 0};
        for (int k = 0; k < 6; ++k) {
            if (!meccChunk_[k]) continue;
            Gib g;
            g.model = meccChunk_[k];
            g.pos = at + Vec3{(frand() - 0.5f) * 0.6f, (frand() - 0.3f) * 0.8f, (frand() - 0.5f) * 0.6f};
            g.vel = Vec3{(frand() - 0.5f) * 9.0f, 4.0f + 5.0f * frand(), (frand() - 0.5f) * 9.0f};
            g.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
            g.angle = frand() * 6.28f;
            g.spin = 5.0f + 6.0f * frand();
            g.scale = 1.0f;
            g.life = -4.0f;  // lie a little longer than the others
            gibs_.push_back(g);
        }
        blood_.burst(at, Vec3{0, 1, 0}, 40, 7.0f, 0.09f);
        blood_.pool(player_.position, 2.0f);
        sfx("me_mslex", at, 1.0f, 0.9f);
        sfx(frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2", at, 1.0f);
    } else {
        playerGibbed_ = false;
        sfx2D(hero_ == Hero::Kabuto ? (frand() < 0.5f ? "ka_deth1" : "ka_deth2") : "me_shrt3", 1.0f, 0.85f);
        if (deathCause_ != DeathCause::Drowned) blood_.pool(player_.position, 1.4f);
    }
    if (skirmish_ && skNet_ != SkNetMode::Join) sk_.localDied(skLocal_);  // (a joiner's death goes to the host with its state)
    static const char* const kCause[] = {"shot", "bitten", "eaten in the water", "blown apart", "fell"};
    LOGI("Player died (%s)", kCause[int(deathCause_)]);
}

void GameApp::updateDeath(float dt, const InputFrame& in) {
    deathTime_ += dt;
    PlayerInput none;
    none.viewYaw = player_.yaw;
    player_.update(dt, none);
    placeCamera(dt, false, &in);
    float sink = 0;
    if (deathCause_ == DeathCause::Drowned) {
        sink = std::max(0.0f, deathTime_ - 1.2f) * 0.5f;  // goes under
        if (hero_ == Hero::Mecc) {  // (Kabuto simply drowns; Delphi never meets them)
            if (frand() < dt * 3.0f) bloodInWater(player_.position, 0.8f);
            if (deathTime_ < 4.0f && frand() < dt * 3.5f) piranhaBite(player_.position, 1.2f, true);  // the frenzy goes on
        }
    }
    bodyXf_ = Mat4::translation(player_.position + Vec3{0, heroLift_ - sink, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, player_.yaw));
    if (hero_ == Hero::Kabuto && kbGrowShown_ < 0.999f) bodyXf_ = bodyXf_ * Mat4::scale(Vec3{1, 1, 1} * kbGrowShown_);
    if (playerModel_ && !playerGibbed_) {
        playerModel_->setHideEnabled(false);
        const int clip = deathClip_[deathCause_ == DeathCause::Drowned ? 1 : (deathCause_ == DeathCause::Fall ? 2 : 0)];
        if (clip >= 0) {
            const float d = playerModel_->animDuration(clip);
            const float t = std::min(deathTime_, d * 0.999f);
            playerModel_->poseBlend(renderer_, clip, t, clip, t, 0.0f);
        }
    }
    timmyOn_ = false;
    if (deathTime_ > 5.5f) {
        // Back at the mission's start, whole again.
        dying_ = false;
        playerGibbed_ = false;
        playerHealth_ = 100.0f;
        firstPerson_ = deathWasFp_;
        piranhaAttack_ = false;
        waterTime_ = 0;
        if (skirmish_) teleportPlayer(sk_.spawnPoint(skLocal_), player_.yaw);  // at his team's base
        else if (haveCheckpoint_) teleportPlayer(clearSpawn(checkpointAt_, checkpointYaw_), checkpointYaw_);  // (the last objective's spot)
        else teleportPlayer(clearSpawn(assetWorld_->spawnPoint(), assetWorld_->spawnYaw()), assetWorld_->spawnYaw());
        respawnFade_ = 1.0f;
        telPhase_ = 0;
        telFade_ = 0;
        rtAimT_ = 0;
        rtSpotOk_ = false;
        recallTurret("Baz respawned");
        LOGI("Player respawned%s", haveCheckpoint_ && !skirmish_ ? " at the checkpoint" : "");
    }
}

void GameApp::explode(const Vec3& at, float radius, float damage, bool fire, int hurtsPlayer) {
    recentBlasts_.push_back({at, radius, time_});  // (rework: ragdolls take the push)
    // Rework: the ground is charred where it went off (not Kabuto's quakes: no fire there).
    if (!blastNoFire_ && (fire || hurtsPlayer == 1 || (damage > 0 && hero_ != Hero::Kabuto)))
        addGroundMark(0, at, std::min(7.0f, 0.8f + 0.55f * radius), frand() * 6.2832f, 75.0f);
    addFlash(at + Vec3{0, 1.0f, 0}, fire ? Vec3{2.6f, 1.5f, 0.6f} : Vec3{1.8f, 1.5f, 1.2f}, std::min(30.0f, 4.0f + radius * 3.0f), 0.45f);
    // Fireballs for fire, grenades and rockets - not Kabuto's quakes and bursting boulders (one at his own feet filled the view).
    if (!blastNoFire_ && (fire || hurtsPlayer == 1 || (damage > 0 && hero_ != Hero::Kabuto))) addFireball(at + Vec3{0, 0.4f, 0}, radius);
    if (at.y < player_.seaLevel + 1.0f && (!enemies_.groundHeight || enemies_.groundHeight(at.x, at.z) < player_.seaLevel - 0.2f))
        waterSplash(at, std::min(3.0f, 0.5f * radius), 2);  // in (or just over) the water
    creatures_.disturb(at, std::max(15.0f, radius * 4.0f));  // the ground shakes: buried Rippers come up
    if (hero_ == Hero::Kabuto) rockHit(at, radius);          // a blow at a volcanic rock pillar's top
    // With the original effects on (Options), the blast is fx.bin's (its own sound too).
    const std::string fxName = blastFx_;
    blastFx_.clear();
    const bool dataFx = !fxName.empty() && playDataFx(fxName, at);
    if (fire && !dataFx) {
        blasts_.push_back({at, 3, 0.0f});
        sfx("me_mslex", at, 1.0f, 0.95f + 0.1f * frand());
    }
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const Enemy& e = enemies_.enemies[i];
        if (e.state == EnemyState::Dead) continue;
        const float d = length(e.pos + Vec3{0, 0.9f, 0} - at);
        if (d > radius) continue;
        const bool killed = enemies_.damage(int(i), damage * (1.0f - 0.6f * d / radius), /*blast=*/true);
        if (killed && enemies_.enemies[i].objective >= 0) completeObjective(enemies_.enemies[i].objective);
        bleed(enemies_.enemies[i].pos + Vec3{0, 1.0f, 0}, normalize(enemies_.enemies[i].pos - at + Vec3{0, 0.5f, 0}), killed, 1.0f);
        if (killed) blood_.pool(enemies_.enemies[i].pos, 1.3f);
        // Rework: the blast throws him back (alive: a shove; killed: the body slides away with it).
        {
            Enemy& en = enemies_.enemies[i];
            Vec3 away = en.pos - at;
            away.y = 0;
            const float k = (1.0f - d / radius) * std::min(14.0f, 4.0f + damage * 0.08f);
            en.vel = en.vel + normalize(away + Vec3{1e-3f, 0, 0}) * k;
        }
    }
    skirmishBlast(at, radius, damage);
    enemies_.alertNear(at, 30.0f);
    // Bodies near the blast are torn apart.
    for (auto& e : enemies_.enemies)
        if (e.state == EnemyState::Dead && e.stateTime > 0.6f && !e.gibbed && length(e.pos - at) < radius * 0.8f) {
            e.gibbed = true;
            gibCorpse(e.pos, e.yaw, e.pos - at, true, 1.0f);
        }
    for (auto& c : creatures_.creatures)
        if (c.state == CreatureState::Dead && c.stateTime > 0.6f && !c.exploded && !c.gibbed && length(c.pos - at) < radius * 0.8f) {
            c.gibbed = true;
            gibCorpse(c.pos, c.yaw, c.pos - at, false, c.scale);
        }
    for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
        const Creature& c = creatures_.creatures[i];
        if (c.state == CreatureState::Dead) continue;
        const float d = length(c.pos + Vec3{0, 0.5f * c.scale, 0} - at);
        if (d > radius) continue;
        std::vector<CreatureEvent> ev;
        Vec3 dir = c.pos - at;
        dir.y = 0;
        dir = length(dir) > 1e-3f ? normalize(dir) : Vec3{0, 0, -1};
        const int slot = c.slot;
        const float scale = c.scale;
        if (creatures_.damage(int(i), damage * (1.0f - 0.6f * d / radius), dir, &ev) && slot >= 0 && size_t(slot) < chars_.size() &&
            chars_[size_t(slot)].objective >= 0)
            completeObjective(chars_[size_t(slot)].objective);
        for (const auto& e : ev) {
            if (e.kind == CreatureEvent::Die && e.burst) spawnGore(e.pos, e.dir, scale);
            creatureSound(e);
        }
    }
    for (auto& rv : race_.racers)  // Reaperski rivals in the blast spin out
        if (!rv.finished && length(rv.pos - at) < radius + 3.0f) rv.spin = 3.0f;
    for (size_t i = 0; i < chars_.size(); ++i) {
        const CharacterInst& ci = chars_[i];
        if (!ci.boss || ci.hp <= 0) continue;
        const float db = std::max(0.0f, length(ci.pos + Vec3{0, 0.5f * ci.bossH, 0} - at) - ci.bossR);
        if (db < radius) hitBoss(int(i), damage * (1.0f - 0.6f * db / radius), ci.pos + Vec3{0, 0.5f * ci.bossH, 0}, normalize(ci.pos - at + Vec3{0, 0.5f, 0}));
    }
    for (size_t i = 0; i < vimps_.vimps.size(); ++i) {
        const Vimp& v = vimps_.vimps[i];
        const float dv = length(v.pos + Vec3{0, 2.0f, 0} - at);
        if (VimpDirector::killable(v) && dv < radius + 1.5f)
            hitVimp(int(i), damage * (1.0f - 0.6f * std::min(1.0f, dv / (radius + 1.5f))), v.pos + Vec3{0, 2.0f, 0}, normalize(v.pos - at + Vec3{0, 0.5f, 0}));
    }
    // Rework: Smarties in the blast.
    for (size_t i = 0; i < villagers_.villagers.size(); ++i) {
        const Villager& v = villagers_.villagers[i];
        const float dv = length(v.pos + Vec3{0, 0.4f, 0} - at);
        if (VillagerDirector::alive(v) && dv < radius + 0.5f)
            hitVillager(int(i), damage * (1.0f - 0.6f * std::min(1.0f, dv / (radius + 0.5f))), v.pos + Vec3{0, 0.4f, 0}, normalize(v.pos - at + Vec3{0, 0.5f, 0}));
    }
    // Scenery in the blast: distance to the object's upright axis, less its size.
    for (size_t k = 0; k < destruct_.items.size(); ++k) {
        const Destructible& it = destruct_.items[k];
        if (!it.alive()) continue;
        const float y = std::max(it.base.y, std::min(it.base.y + it.height, at.y));
        const float dd = std::max(0.0f, length(Vec3{it.base.x, y, it.base.z} - at) - 0.5f * it.radius);
        if (dd < radius) damageObject(int(k), damage * (1.0f - 0.5f * dd / radius), true, at);
    }
    for (size_t k = 0; k < nests_.size(); ++k) {
        const float dn = length(nests_[k].pos - at);
        if (dn < radius + nests_[k].radius * 0.4f) damageNest(int(k), damage * (1.0f - 0.5f * std::min(1.0f, dn / radius)));
    }
    LOGI("%s at (%.1f %.1f %.1f), %zu creatures alive", fire ? "Rocket exploded" : "Quake", at.x, at.y, at.z, creatures_.aliveCount());
    // His own rockets throw the Meccaryn (rocket jumps; a blast under him kills his fall speed)
    // and hurt him the closer he is: up to 40 at point blank, nothing at the blast's edge.
    const Vec3 rel = player_.position + Vec3{0, 0.9f, 0} - at;
    const bool hurts = hurtsPlayer < 0 ? fire : hurtsPlayer != 0;
    const float dp = hurts ? length(rel) : 1e9f;  // a quake (Kabuto's stomp) doesn't throw its maker
    if (playMode_ && dp < radius && !dying_) {
        const float f = 1.0f - dp / radius;
        const float hurt = 40.0f * f * std::sqrt(f);
        if (hurt > 1.0f) {
            hitFrom_ = at;
            hitDirTimer_ = 1.2f;
            damagePlayer(hurt, DeathCause::Blast);
        }
    }
    if (playMode_ && dp < radius * 1.2f) {
        const float k = 1.0f - dp / (radius * 1.2f);
        const Vec3 dir = dp > 1e-3f ? rel * (1.0f / dp) : Vec3{0, 1, 0};
        player_.velocity.x += dir.x * 9.0f * k;
        player_.velocity.z += dir.z * 9.0f * k;
        player_.velocity.y = std::max(player_.velocity.y, 0.0f) + (4.0f + std::max(0.0f, dir.y) * 8.0f) * k;
        player_.onGround = false;
        haptics_.push_back({kLeft, 0.8f * k, 0.15f});
        haptics_.push_back({kRight, 0.8f * k, 0.15f});
    }
    // Reapers standing near the blast are shoved.
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const Vec3 r = enemies_.enemies[i].pos - at;
        const float d = length(r);
        if (d < radius * 1.5f && d > 1e-3f) enemies_.push(int(i), r * (6.0f * (1.0f - d / (radius * 1.5f)) / d));
    }
}

void GameApp::updateRockets(float dtAll) {
    for (auto& r : rockets_) {
        if (r.life >= 1e9f) continue;  // (spent elsewhere - burst in a fire circle, a jetski's fuse: erased below, never burst again)
        // (SLOW TIME: the enemies' missiles, bazooka rockets and grenades fly slow too - review 2026-10-28)
        const float dt = (r.kind == 2 || r.kind == 3 || r.kind == 6) ? dtAll * enemyTimeScale() : dtAll;
        r.life += dt;
        if (r.kind == 2) {
            // A SAM's homing missile: turns toward the player (limited rate), bursts close to him,
            // on the ground or when its fuel runs out.
            // A burning flare in reach draws it off the player.
            Vec3 chest = player_.position + Vec3{0, 0.5f * player_.height, 0};
            Vec3 flare;
            if (flareTarget(r.pos, flare)) chest = flare;
            const Vec3 to = chest - r.pos;
            const float dist = length(to);
            const float sp = length(r.vel);
            Vec3 f = r.vel * (1.0f / std::max(1e-3f, sp));
            const Vec3 want = to * (1.0f / std::max(1e-3f, dist));
            const float k = std::min(1.0f, (1.8f + 40.0f / std::max(5.0f, dist)) * dt);  // tighter when close: no orbiting
            f = normalize(f + (want - f) * k);
            r.vel = f * std::min(30.0f, sp + 10.0f * dt);
            if (!rocketTrail(r) && frand() < dt * 25.0f) smokePuff(FxTex::Smoke, r.pos, Vec3{0, 0.3f, 0}, 0.3f, 1.4f, 1.2f, 0.45f, 0.1f);
            float g = -1e9f;
            const bool ground = assetWorld_ && assetWorld_->terrain().heightAt(r.pos.x, r.pos.z, g) && r.pos.y < g + 0.3f;
            if (dist < 3.0f || ground || r.pos.y < 0.0f || r.life > 9.0f || dying_) {
                LOGI("SAM missile bursts %.1f m from the player", length(r.pos - (player_.position + Vec3{0, 0.5f * player_.height, 0})));
                explode(r.pos, 3.5f, 60.0f);
                r.life = 1e9f;
                continue;
            }
            r.pos += r.vel * dt;
            continue;
        }
        if (r.kind == 5 || r.kind == 6) {
            // Lobbed: a Millenium bomb bounces once and bursts at its next landing; a grenade bounces
            // and rolls until its fuse (2.5 s) runs out. Either bursts on whoever it strikes.
            const bool gren = r.kind == 6;
            r.vel.y -= 9.8f * dt;
            const float st = length(r.vel) * dt;
            const Vec3 d5 = normalize(r.vel);
            float t5 = st;
            Vec3 n5{0, 1, 0};
            int tg = -1;
            bool h5 = st > 1e-5f && weapon_.raycast && weapon_.raycast(r.pos, d5, st, t5, n5, tg);
            if (!h5 && r.pos.y + r.vel.y * dt < 0.0f) { h5 = true; t5 = st; n5 = {0, 1, 0}; tg = -1; }  // the sea
            const bool fuse = gren ? r.life > 2.5f : r.life > 5.0f;
            if (!fuse && h5 && tg == -1 && r.pos.y > 0.0f && (gren || r.bounces == 0)) {
                r.pos += d5 * std::max(0.0f, t5 - 0.05f);
                r.vel = (r.vel - n5 * (2.0f * dot(r.vel, n5))) * (gren ? 0.4f : 0.45f);  // off the slope, losing most of its speed
                ++r.bounces;
                if (r.bounces <= 3 && length(r.vel) > 1.0f) sfx(gren ? "bi_dirt1" : "me_mtrht", r.pos, gren ? 0.5f : 0.8f, 1.0f + 0.1f * frand());
                continue;
            }
            if (h5 || fuse) {
                const Vec3 at = h5 ? r.pos + d5 * t5 : r.pos;
                if (gren) {
                    // weapon 2: 80 damage, splash 75 over (.., 30, 50) - near the rocket's 80 / (.., 30, 55).
                    blastFx_ = "FX_EXPLOSION_GRENADE";
                    if (playDataFx(blastFx_, at)) {
                        blastFx_.clear();
                        explode(at + Vec3{0, 0.3f, 0}, 3.6f, 130.0f, false, 1);
                    } else {
                        blastFx_.clear();
                        explode(at + Vec3{0, 0.3f, 0}, 3.6f, 130.0f, false, 1);
                        blasts_.push_back({at + Vec3{0, 0.3f, 0}, 2, 0.0f});
                        sfx("me_grnex", at, 1.0f, 0.95f + 0.1f * frand());
                    }
                    float near = 1e9f;
                    for (const auto& e : enemies_.enemies)
                        if (e.state != EnemyState::Dead) near = std::min(near, length(e.pos + Vec3{0, 0.9f, 0} - at));
                    LOGI("Grenade bursts %.1f m away (nearest Reaper %.1f m from it, %d bounces)", length(at - player_.position), near, r.bounces);
                } else {
                    // weapon 1: 150 damage, splash 150 over (.., 50, 80) - against the rocket's 80 / (.., 30, 55).
                    blastFx_ = "FX_EXPLOSION_LBOMB";
                    explode(at, 5.5f, 225.0f);
                }
                r.life = 1e9f;
                continue;
            }
            r.pos += r.vel * dt;
            continue;
        }
        if (r.kind == 8) {
            // Delphi's homing arrow: turns (limited rate) toward the nearest living foe within 60 degrees of
            // its flight and 80 m; a faint green trail.
            const float sp = length(r.vel);
            Vec3 f = r.vel * (1.0f / std::max(1e-3f, sp));
            Vec3 best;
            float bestD = 80.0f;
            int bestId = -1;
            auto consider = [&](const Vec3& p, int id) {
                const Vec3 to = p - r.pos;
                const float d = length(to);
                if (d < bestD && dot(to * (1.0f / std::max(1e-3f, d)), f) > 0.5f) { bestD = d; best = p; bestId = id; }
            };
            for (size_t i = 0; i < enemies_.enemies.size(); ++i)
                if (enemies_.enemies[i].state != EnemyState::Dead) consider(enemyChest(enemies_.enemies[i]), int(i));
            for (size_t i = 0; i < creatures_.creatures.size(); ++i)
                if (creatures_.creatures[i].state != CreatureState::Dead)
                    consider(creatures_.creatures[i].pos + Vec3{0, 0.5f * creatures_.creatures[i].scale, 0}, kCreatureTarget + int(i));
            for (size_t i = 0; i < vimps_.vimps.size(); ++i)
                if (VimpDirector::killable(vimps_.vimps[i])) consider(vimps_.vimps[i].pos + Vec3{0, 2.0f, 0}, kVimpTarget + int(i));
            if (bestD < 1.0f && bestId >= 0 && weapon_.onHit) {
                // Close enough: it strikes the foe it was chasing (and bursts).
                hitMul_ = r.dmg;
                weapon_.onHit(bestId, best, f);
                hitMul_ = 1.0f;
                explode(best, 1.8f, 40.0f, false, 1);
                sfx("me_mslex", best, 0.9f, 1.1f);
                r.life = 1e9f;
                continue;
            }
            if (bestD < 80.0f) {
                if (r.bounces == 0) { r.bounces = 1; LOGI("Homing arrow locked on a foe %.0f m away", bestD); }
                const Vec3 want = normalize(best - r.pos);
                f = normalize(f + (want - f) * std::min(1.0f, 8.0f * dt));
                r.vel = f * sp;
            }
            if (!rocketTrail(r) && frand() < dt * 20.0f) smokePuff(FxTex::FireGreen, r.pos, Vec3{0, 0.1f, 0}, 0.12f, 0.3f, 0.4f, 0.6f, 0.0f);
        }
        if (r.kind == 9) {
            // Baz's homing rocket: turns (limited rate) toward the nearest living foe within 60 degrees of its flight
            // and 100 m, then bursts as a rocket does.
            const float sp = length(r.vel);
            Vec3 f = r.vel * (1.0f / std::max(1e-3f, sp));
            Vec3 best;
            float bestD = 100.0f;
            auto consider = [&](const Vec3& p) {
                const Vec3 to = p - r.pos;
                const float d = length(to);
                if (d < bestD && dot(to * (1.0f / std::max(1e-3f, d)), f) > 0.5f) { bestD = d; best = p; }
            };
            for (const auto& e : enemies_.enemies)
                if (e.state != EnemyState::Dead) consider(enemyChest(e));
            for (const auto& c : creatures_.creatures)
                if (c.state != CreatureState::Dead && !CreatureDirector::underground(c)) consider(c.pos + Vec3{0, 0.5f * c.scale, 0});
            for (const auto& c : chars_)  // (bosses)
                if (c.boss && c.hp > 0) consider(c.pos + Vec3{0, 0.5f * c.bossH, 0});
            if (bestD < 100.0f) {
                if (r.bounces == 0) { r.bounces = 1; LOGI("Homing rocket locked on a foe %.0f m away", bestD); }
                f = normalize(f + (normalize(best - r.pos) - f) * std::min(1.0f, 7.0f * dt));
                r.vel = f * sp;
            }
            // (its trail: FX_homing with the original effects, else updateFx's flame and smoke as on any rocket)
        }
        if (r.kind == 10) {
            // Baz's proximity rocket: bursts as it passes within 3 m of a living foe - armed only once it is 5 m from
            // him (its burst reaches 4 m: a Reaper beside him in a melee must not set it off at his elbow).
            bool near = false;
            Vec3 burstAt = r.pos;
            if (length(r.pos - (player_.position + Vec3{0, 0.9f, 0})) > 5.0f) {
                // (over this frame's whole step: at a low frame rate it moves 1-2 m a frame)
                const Vec3 a = r.pos, seg = r.vel * dt;
                const float sl = std::max(1e-6f, dot(seg, seg));
                Vec3 closest = a;
                auto dist = [&](const Vec3& c) {
                    const float u = std::max(0.0f, std::min(1.0f, dot(c - a, seg) / sl));
                    closest = a + seg * u;
                    return length(closest - c);
                };
                for (const auto& e : enemies_.enemies)
                    if (e.state != EnemyState::Dead && dist(enemyChest(e)) < 3.0f) { near = true; break; }
                for (const auto& c : creatures_.creatures)
                    if (!near && c.state != CreatureState::Dead && !CreatureDirector::underground(c) && dist(c.pos + Vec3{0, 0.5f * c.scale, 0}) < 3.0f + 0.5f * c.scale) near = true;
                for (const auto& c : chars_)  // (the bosses - the Charger, Sonaks, a rival Kabuto - live here, not in enemies_)
                    if (!near && c.boss && c.hp > 0 && dist(c.pos + Vec3{0, 0.5f * c.bossH, 0}) < 3.0f + c.bossR) near = true;
                if (near) burstAt = closest;
            }
            static const bool ptrace = std::getenv("GVR_PROX_TRACE") != nullptr;  // test hook: its closest foe
            if (ptrace && int(r.life * 10) != int((r.life - dt) * 10)) {
                float best = 1e9f;
                for (const auto& e : enemies_.enemies)
                    if (e.state != EnemyState::Dead) best = std::min(best, length(enemyChest(e) - r.pos));
                LOGI("prox trace: %.1f s, nearest foe %.1f m", double(r.life), double(best));
            }
            if (near) {
                blastFx_ = "FX_EXPLOSION_ROCKET";
                explode(burstAt, 4.0f, 120.0f);
                sfx("me_prxex1", burstAt, 1.0f);
                LOGI("Proximity rocket bursts by a foe");
                r.life = 1e9f;
                continue;
            }
        }
        if (r.kind == 11) {
            // (2026-10-29) The summoned sea monster's fireball (weapon 48, fx_seamonster_fireball): lobbed, burning.
            r.vel.y -= 9.8f * dt;
            if (!rocketTrail(r) && frand() < dt * 40.0f) smokePuff(FxTex::Fire, r.pos, Vec3{0, 0.5f, 0}, 0.6f, 1.6f, 0.5f, 0.9f, 0.0f);
        }
        if (r.kind == 3) {
            // A Reaper's bazooka rocket: smoke trail; bursts near the player as well as on what it hits.
            if (!rocketTrail(r) && frand() < dt * 25.0f) smokePuff(FxTex::SmokeGrey, r.pos, Vec3{0, 0.3f, 0}, 0.25f, 1.2f, 1.0f, 0.45f, 0.1f);
            if (length(r.pos - (player_.position + Vec3{0, 0.5f * player_.height, 0})) < 1.6f) {
                explode(r.pos, 3.5f, 55.0f);
                r.life = 1e9f;
                continue;
            }
        }
        const float step = length(r.vel) * dt;
        const Vec3 dir = normalize(r.vel);
        float t;
        Vec3 n;
        int target = -1;
        bool hit = weapon_.raycast && weapon_.raycast(r.pos, dir, step, t, n, target);
        if (!hit) {
            // The sea stops rockets too.
            const Vec3 next = r.pos + r.vel * dt;
            if (next.y < 0.0f && r.pos.y >= 0.0f) { hit = true; t = step * (r.pos.y / std::max(1e-4f, r.pos.y - next.y)); }
        }
        if (hit || r.life > 5.0f) {
            if (r.kind == 1) explode(hit ? r.pos + dir * t : r.pos, 2.6f, 75.0f);  // fire bolt: smaller blast
            else if (r.kind == 3) explode(hit ? r.pos + dir * t : r.pos, 3.5f, 55.0f);
            else if (r.kind == 11) {  // (weapon 48: 150 damage, a wide burst; it can burn her too, as the original's line says)
                const Vec3 at = hit ? r.pos + dir * t : r.pos;
                explode(at, 5.0f, 150.0f, true);
                LOGI("Sea monster's fireball bursts at (%.1f %.1f %.1f)", double(at.x), double(at.y), double(at.z));
            }
            else if (r.kind == 4 || r.kind == 7 || r.kind == 8) {
                // Shotgun shell (weapon 6: 40 damage, splash 40) or an RPG-bow arrow (fxb_bow_rpg: the
                // same numbers): the hit itself like a bullet of that weight, then a small burst round it.
                const Vec3 at = hit ? r.pos + dir * t : r.pos;
                if (hit && target != -1 && weapon_.onHit) {
                    const bool wasAlive = target >= 0 && target < 1000 && size_t(target) < enemies_.enemies.size() &&
                                          enemies_.enemies[size_t(target)].state != EnemyState::Dead;
                    hitMul_ = r.dmg > 0 ? r.dmg : guns_[kGunShotgun].damage;
                    weapon_.onHit(target, at, dir);
                    hitMul_ = 1.0f;
                    // Rework: a shotgun shell that kills a Reaper close by tears off the limb it struck (often).
                    static const bool always = std::getenv("GVR_DISMEMBER_ALWAYS") != nullptr;  // (test hook)
                    if (std::getenv("GVR_DISMEMBER_TRACE")) LOGI("shell hit target %d alive-before %d", target, int(wasAlive));
                    if (wasAlive && r.kind == 4) {
                        Enemy& ke = enemies_.enemies[size_t(target)];
                        if (ke.state == EnemyState::Dead && !ke.gibbed && length(ke.pos - player_.position) < 8.0f && (always || frand() < 0.55f))
                            dismember(ke, at, dir);
                    }
                }
                explode(at, 1.8f, 40.0f, false, 1);
                if (!playDataFx("fx_Explosion_shotgun", at)) {  // weapon 6's / the RPG bow's impact effect
                    blasts_.push_back({at, 1, 0.0f});
                    sfx(r.kind == 4 ? "me_rsgex" : "me_mslex", at, 0.9f, 0.95f + 0.1f * frand());
                }
            }
            else {
                blastFx_ = "FX_EXPLOSION_ROCKET";
                explode(hit ? r.pos + dir * t : r.pos, 4.0f, 120.0f);
                if (r.kind == 9) sfx("me_homex", hit ? r.pos + dir * t : r.pos, 1.0f);  // (the homing rocket's own burst)
                if (r.kind == 10) sfx("me_prxex1", hit ? r.pos + dir * t : r.pos, 1.0f);
            }
            r.life = 1e9f;
            continue;
        }
        r.pos += r.vel * dt;
    }
    syncRocketFx();
    rockets_.erase(std::remove_if(rockets_.begin(), rockets_.end(), [](const Rocket& r) { return r.life >= 1e9f; }), rockets_.end());
}

void GameApp::updateNests(float dt) {
    if (nests_.empty()) return;
    // Which pooled Ripper models are in use (a creature refers to them)?
    std::vector<char> used(chars_.size(), 0);
    for (const auto& c : creatures_.creatures)
        if (c.slot >= 0 && size_t(c.slot) < used.size()) used[size_t(c.slot)] = 1;
    for (size_t i = 0; i < nests_.size(); ++i) {
        NestInst& n = nests_[i];
        n.shown += (n.target - n.shown) * std::min(1.0f, dt * 6.0f);  // shrinks smoothly when hit
        if (!n.alive) continue;
        if (n.dying >= 0) {
            n.dying += dt;
            n.target = 0.0f;
            if (n.dying > 0.6f) { n.alive = false; LOGI("Ripper nest %zu destroyed", i); }
            continue;
        }
        if (!playMode_ || playerHealth_ <= 0 || length(n.pos - player_.position) > 70.0f) continue;
        n.timer -= dt;
        if (n.timer > 0) continue;
        n.timer = 6.0f + 4.0f * frand();
        // Up to three of its brood at a time.
        int brood = 0;
        for (const auto& c : creatures_.creatures) brood += c.hunter && c.state != CreatureState::Dead && length(c.home - n.pos) < 1.0f;
        if (brood >= 3) continue;
        for (size_t k = 0; k < chars_.size(); ++k) {
            CharacterInst& ci = chars_[k];
            if (!ci.pooled || used[k] || (ci.size == CreatureSize::Small) != n.small) continue;
            const Vec3 to = player_.position - n.pos;
            const float yaw = std::atan2(-to.x, -to.z);
            const ObjectType* t = assetWorld_->registry().type(uint32_t(ci.type));
            const float hp = t && t->hitPoints > 0 ? t->hitPoints : 40.0f;
            const int idx = creatures_.spawnHunter(n.pos, yaw, ci.scale, ci.size, hp);
            creatures_.creatures[size_t(idx)].slot = int(k);
            used[k] = 1;
            ci.objective = -1;
            sfx(ci.size == CreatureSize::Small ? "rs_spawn" : "rm_spawn", n.pos, 0.9f);
            LOGI("Ripper nest %zu: a Ripper hatches", i);
            break;
        }
    }
}

void GameApp::damageNest(int i, float amount) {
    if (i < 0 || size_t(i) >= nests_.size()) return;
    NestInst& n = nests_[size_t(i)];
    if (!n.alive || n.dying >= 0) return;
    n.hp -= amount;
    // It shrinks as it is hurt, and goes when it is spent.
    n.target = 0.35f + 0.65f * std::max(0.0f, n.hp / n.maxHp);
    sfx("ex_bdys1", n.pos + Vec3{0, 0.6f, 0}, 0.8f, 0.8f + 0.2f * frand());
    if (n.hp <= 0) {
        n.dying = 0;
        blasts_.push_back({n.pos + Vec3{0, 0.6f, 0}, 3, 0.0f});
        sfx("ex_bdyl1", n.pos, 1.0f);
        sfx("rl_deat1", n.pos, 0.8f, 0.7f);
    }
}

void GameApp::startSchool(School& s) {
    s.active = true;
    s.biteTimer = 0.4f;
    for (int i = 0; i < kPiranhas; ++i) s.fish[i] = Piranha{float(i) * 1.2566f + frand(), -1.0f};
}

void GameApp::updateSchools(float dt) {
    // School 0 hunts the player; the others the Smarties in the water (walkthrough: they devour a
    // Smartie in about five seconds).
    if (schools_.empty()) schools_.resize(1);
    School& ps = schools_[0];
    if (piranhaAttack_ && !ps.active) startSchool(ps);
    ps.active = piranhaAttack_;
    if (ps.active) ps.centre = player_.position;
    for (size_t i = 0; i < rescue_.smarties.size(); ++i) {
        const RescueSmartie& sm = rescue_.smarties[i];
        School* found = nullptr;
        for (size_t k = 1; k < schools_.size(); ++k)
            if (schools_[k].target == int(i)) found = &schools_[k];
        const bool prey = sm.state == SmartieState::Drowning;
        if (prey && !found) {
            School sc;
            sc.target = int(i);
            sc.centre = sm.pos;
            startSchool(sc);
            schools_.push_back(sc);
            continue;
        }
        if (found) {
            found->active = prey;
            if (prey) {
                found->centre = Vec3{sm.pos.x, 0.0f, sm.pos.z};
                found->biteTimer -= dt;
                if (found->biteTimer <= 0) {
                    found->biteTimer = 0.55f + 0.3f * frand();
                    sfx(frand() < 0.5f ? "pi_swrm1" : "pi_swrm2", sm.pos, 0.9f, 0.95f + 0.2f * frand());
                    piranhaBite(sm.pos, 0.75f, false);
                }
            }
        }
    }
    for (auto& sc : schools_) {
        sc.vis += ((sc.active ? 1.0f : 0.0f) - sc.vis) * std::min(1.0f, dt * 2.5f);
        if (sc.vis <= 0.01f) continue;
        for (int i = 0; i < kPiranhas; ++i) {
            Piranha& f = sc.fish[i];
            f.angle += dt * (1.6f + 0.25f * float(i));
            if (f.jump >= 0) f.jump += dt;
            if (f.jump > 2.3f) f.jump = -1;
            if (f.jump < 0 && sc.active && frand() < dt * (sc.target < 0 ? 0.25f : 0.6f)) f.jump = 0;  // leaps at the prey
        }
    }
    schools_.erase(std::remove_if(schools_.begin() + 1, schools_.end(), [](const School& sc) { return !sc.active && sc.vis < 0.01f; }),
                   schools_.end());
}

void GameApp::updateCombat(float dt) {
    GVR_PROF("combat (all)");
    { GVR_PROF("rockets"); updateRockets(dt); }
    { GVR_PROF("nests"); updateNests(dt); updateSpawners(dt); }
    // Water: entering it splashes; swimming leaves rings. Piranhas live in all deep water (the 1b
    // briefing warns of them): after a moment there they gather round and bite; leaving deep water
    // (shallows, the shore, the jetpack) sends them away. Rates and damage ours.
    const bool swim = playMode_ && player_.swimming();
    if (swim && !wasSwimming_) waterSplash(player_.position, 1.2f, 2);
    { GVR_PROF("waterflow"); updateWaterFlow(dt); updateHandWater(dt); }
    wasSwimming_ = swim;
    for (auto& f : waterFx_) f.age += dt;
    waterFx_.erase(std::remove_if(waterFx_.begin(), waterFx_.end(), [](const WaterFx& f) { return f.age > 2.0f; }), waterFx_.end());
    GVR_PROF_BEGIN(waterMisc, "water misc");
    if (swim && hero_ == Hero::Kabuto) {
        // Rework (user): no piranha takes on a giant - but Kabuto can't swim: out of his depth he
        // goes under and drowns unless he gets back to where he can stand.
        waterTime_ += dt;
        piranhaAttack_ = false;
        if (waterTime_ > 0.6f) {
            if (waterTime_ - dt <= 0.6f) {
                timmyLine_ = "TOO DEEP - KABUTO CAN'T SWIM! BACK TO THE SHALLOWS";
                timmyLineT_ = 4.0f;
                LOGI("Kabuto is out of his depth: drowning");
            }
            biteTimer_ -= dt;
            if (biteTimer_ <= 0) {
                biteTimer_ = 0.5f;
                damagePlayer(12.0f, DeathCause::Drowned);  // (his hide's 0.3: ~7 a second)
                sfx2D(frand() < 0.5f ? "ka_wound" : "ka_mdht1", 0.6f, 0.8f);
                waterSplash(player_.position + Vec3{0, player_.height * 0.7f, 0}, 0.25f * player_.height, 1);
                for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.4f, 0.1f});
            }
        }
    } else if (swim && hero_ == Hero::Delphi) {
        // A Sea Reaper is at home in the water: no piranhas for her.
        waterTime_ = 0;
        piranhaAttack_ = false;
    } else if (swim) {
        waterTime_ += dt;
        if (waterTime_ > 0.8f) {
            if (!piranhaAttack_) {
                piranhaAttack_ = true;
                LOGI("Piranhas!");
            }
            biteTimer_ -= dt;
            if (biteTimer_ <= 0) {
                biteTimer_ = 0.5f;
                sfx(frand() < 0.5f ? "pi_swrm1" : "pi_swrm2", player_.position, 1.0f, 0.9f + 0.2f * frand());
                damagePlayer(8.0f, DeathCause::Drowned);
                piranhaBite(player_.position, 1.0f, true);
                for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.6f, 0.08f});
            }
        }
    } else {
        waterTime_ = 0;
        biteTimer_ = 0;
        piranhaAttack_ = false;
    }
    // Wading and swimming: the water round the legs (updateWaterFlow).
    // NPCs in the water leave rings too (Rippers and Reapers floating or wading).
    {
        size_t k = 0;
        const size_t n = creatures_.creatures.size() + enemies_.enemies.size();
        if (npcRipple_.size() != n) npcRipple_.assign(n, 0.0f);
        auto ripple = [&](const Vec3& p, bool moving, float size) {
            float& t = npcRipple_[k++];
            t -= dt;
            if (p.y < -0.15f && t <= 0 && length(p - player_.position) < 60.0f && waterFx_.size() < 48) {
                t = moving ? 0.45f : 1.2f;
                waterFx_.push_back({Vec3{p.x, 0.03f, p.z}, 0.0f, 1, size});
            }
        };
        for (const auto& c : creatures_.creatures) ripple(c.pos, c.speed > 0.3f && c.state != CreatureState::Dead, 1.2f * c.scale);
        for (const auto& e : enemies_.enemies) ripple(e.pos, e.state == EnemyState::Advance, 1.6f);
    }
    GVR_PROF_END(waterMisc);
    { GVR_PROF("schools"); updateSchools(dt); }
    // Health regenerates after 4 s without damage.
    sinceDamage_ += dt;
    if (sinceDamage_ > 4.0f && !dying_) playerHealth_ = std::min(100.0f, playerHealth_ + 12.0f * dt);
    respawnFade_ = std::max(0.0f, respawnFade_ - dt * 1.2f);
    // Test hook GVR_OBJECTIVE_AT=<s of play>,<index>: that objective counts as done then.
    if (static const char* oa = std::getenv("GVR_OBJECTIVE_AT"); oa && playMode_) {
        static float played = 0;
        float at = 0;
        int idx = 0;
        played += dt;
        if (std::sscanf(oa, "%f,%d", &at, &idx) == 2 && played >= at && played - dt < at) completeObjective(idx);
    }
    // Test hook GVR_DIE_AT=<s of play>: the player dies then (god mode or not) - the respawn's own test.
    if (static const char* da = std::getenv("GVR_DIE_AT"); da && !dying_ && playMode_) {
        static float played = 0;
        played += dt;
        if (played >= float(std::atof(da)) && played - dt < float(std::atof(da))) {
            LOGI("Test: the player dies (GVR_DIE_AT)");
            playerHealth_ = 0;
            lastHurt_ = DeathCause::Shot;
            startDeath();
        }
    }
    damageFlash_ = std::max(0.0f, damageFlash_ - dt * 3.0f);

    enemies_.params.punch = hero_ != Hero::Kabuto;
    { GVR_PROF("enemies"); enemies_.update(dt * enemyTimeScale(), player_.position, playMode_ && playerHealth_ > 0 && !dying_, shooters_); }
    for (int i : enemies_.punches) {  // rework: a Reaper's punch landed
        const Enemy& e = enemies_.enemies[size_t(i)];
        hitFrom_ = e.pos;
        hitDirTimer_ = 1.2f;
        damagePlayer(9.0f, DeathCause::Shot);
        sfx("ex_bdys1", player_.position + Vec3{0, 1.4f, 0}, 0.9f, 0.7f + 0.1f * frand());
        Vec3 push{player_.position.x - e.pos.x, 0, player_.position.z - e.pos.z};
        if (length(push) > 1e-3f) player_.velocity = player_.velocity + normalize(push) * 3.5f;  // knocked back a step
        LOGI("Reaper %u punches you", e.uid);
    }
    bushEvents();
    { GVR_PROF("skirmish"); updateSkirmish(dt); }
    updateGroundMarks(dt);
    updateCasings(dt);
    { GVR_PROF("ragdolls"); updateBodyGrab(dt, lastInput_); updateStones(dt, lastInput_); updateHeldReaperGuns(dt, lastInput_); updateRagdolls(dt); updateHints(dt); }
    // Model slots follow the enemies (the director removes sunk corpses).
    std::fill(slotUsed_.begin(), slotUsed_.end(), 0);
    for (auto& e : enemies_.enemies)
        if (e.slot >= 0 && size_t(e.slot) < slotUsed_.size()) slotUsed_[size_t(e.slot)] = 1;
    for (auto& e : enemies_.enemies)
        if (e.slot < 0) {
            for (size_t k = 0; k < slotUsed_.size(); ++k)
                if (!slotUsed_[k]) { e.slot = int(k); slotUsed_[k] = 1; slotLift_[k] = enemyFootOffset_; enemyPool_[k]->clearSever(); break; }
            if (e.slot < 0 && e.state != EnemyState::Dead) {
                // Rework: every model in use - the living take the farthest corpse's (it goes).
                Enemy* far = nullptr;
                for (auto& c : enemies_.enemies)
                    if (c.state == EnemyState::Dead && c.slot >= 0 && !c.remove && c.uid != heldBody_.uid && c.uid != thrownBody_.uid && c.uid != impaledUid_ &&
                        (!far || length(c.pos - player_.position) > length(far->pos - player_.position)))
                        far = &c;  // (not one in your hand, in flight, or on Delphi's blade)
                if (far) { e.slot = far->slot; far->slot = -1; far->remove = true; slotLift_[size_t(e.slot)] = enemyFootOffset_; enemyPool_[size_t(e.slot)]->clearSever(); }
                // (2026-10-30) No corpse to take either: one more model, rather than a Reaper with no body (but a name tag).
                if (e.slot < 0 && int(enemyPool_.size()) < kMaxEnemyModels && addEnemyModel()) {
                    e.slot = int(enemyPool_.size()) - 1;
                    slotUsed_[size_t(e.slot)] = 1;
                    LOGI("Enemies: model slot %d added for Reaper %u", e.slot, e.uid);
                }
            }
        }
    // Skinning is the costly part: Reapers too far to make out (the giant sees further) keep their
    // last pose; mid-distance ones are re-posed every other frame.
    const float poseFar = hero_ == Hero::Kabuto ? 400.0f : 130.0f;
    ++enemyPoseTick_;
    GVR_PROF_BEGIN(poseEnemies, "pose enemies");
    for (auto& e : enemies_.enemies) {
        if (e.slot < 0) continue;
        const float pd = length(e.pos - player_.position);
        const float near = hero_ == Hero::Kabuto ? 75.0f : 25.0f;
        const uint32_t every = pd < near ? 1u : (pd < near * 2.4f ? 2u : 4u);  // animation LOD by distance
        if (Ragdoll* rd = ragdollOf(e)) {  // rework: a ragdoll is posed every frame until it lies still
            if (pd > poseFar || (rd->active && rd->asleep && rd->posedAsleep)) continue;
            const int c = rd->clip >= 0 ? rd->clip : int(e.clip);
            posingEnemy_ = &e;
            enemyPool_[size_t(e.slot)]->poseBlend(renderer_, c, e.clipTime, c, e.clipTime, 0.0f);
            posingEnemy_ = nullptr;
            continue;  // (its lift stays as it was)
        }
        if (pd > poseFar || ((enemyPoseTick_ + uint32_t(e.slot)) % every) != 0) continue;
        SkinnedModel& m = *enemyPool_[size_t(e.slot)];
        m.poseBlend(renderer_, int(e.prevClip), e.prevTime, int(e.clip), e.clipTime, e.fadeWeight(enemies_.params.crossfade));
        // Ground contact from the actual pose: the lowest point (planted foot, or the body lying
        // in a death pose) rests on the ground. Smoothed lightly against single-frame pops.
        Vec3 mn, mx;
        m.posedBounds(mn, mx);
        float& lift = slotLift_[size_t(e.slot)];
        lift += (-mn.y - lift) * std::min(1.0f, dt / 0.06f);
    }
    GVR_PROF_END(poseEnemies);
    { GVR_PROF("reaper guns"); placeReaperWeapons(); }  // the guns in their hands, from this frame's poses
    // Enemy shots from each Reaper's gun nozzle toward the player's chest, with spread.
    for (auto& e : enemies_.enemies) e.rocketCool = std::max(0.0f, e.rocketCool - dt * enemyTimeScale());
    // Rework: a marksman's gun glows red for the moment before each shot - see it, and move.
    for (const auto& e : enemies_.enemies)
        if (e.role == 1 && e.state == EnemyState::Fire && e.burstLeft > 0 && e.fireCooldown < 0.7f && e.weapon != 1 && embers_.size() < 220) {
            const Quat r = Quat::fromAxisAngle({0, 1, 0}, e.yaw);
            Vec3 m = e.pos + Vec3{0, 1.5f, 0} + r.rotate(Vec3{0.3f, 0, -0.6f});
            reaperMuzzle(e, m);
            const float k = 1.0f - std::max(0.0f, e.fireCooldown) / 0.7f;
            embers_.push_back({m, Vec3{0, 0, 0}, 0.0f, dt * 1.5f, 0.3f + 0.5f * k});
        }
    for (int i : shooters_) {
        Enemy& e = enemies_.enemies[size_t(i)];
        const Quat r = Quat::fromAxisAngle({0, 1, 0}, e.yaw);
        Vec3 muzzle = e.pos + Vec3{0, 1.5f, 0} + r.rotate(Vec3{0.3f, 0, -0.6f});
        Mat4 nz;
        if (reaperMuzzle(e, muzzle)) {
        } else if (e.slot >= 0 && enemyPool_[size_t(e.slot)]->nodeWorld("ref_nozzle", nz))
            muzzle = e.pos + Vec3{0, slotLift_[size_t(e.slot)], 0} + r.rotate(giantsToRuntime(nz.m[12], nz.m[13], nz.m[14]));
        if (e.weapon == 1 && e.weaponOn) {
            // Bazooka: one slow rocket every few seconds instead of the burst (ours).
            if (e.rocketCool > 0) continue;
            e.rocketCool = 3.5f;
            const Vec3 aim = player_.position + Vec3{0, 0.5f * player_.height, 0};
            Vec3 dir = normalize(aim - muzzle);
            dir = normalize(dir + Vec3{frand() - 0.5f, (frand() - 0.5f) * 0.3f, frand() - 0.5f} * (0.04f + 0.002f * length(aim - muzzle)));
            rockets_.push_back({muzzle + dir * 0.8f, dir * 22.0f, 0.0f, 3});
            sfx("pr_wsmil", muzzle, 0.9f, 1.1f);
            continue;
        }
        const Vec3 target = player_.position + Vec3{0, (0.5f + 0.4f * frand()) * playerHitTop() * (player_.height / (1.65f * heroScale_)), 0};
        Vec3 dir = normalize(target - muzzle);
        // Spread grows with distance: long shots mostly miss, close ones are dangerous.
        const float spread = (0.06f + 0.004f * length(target - muzzle)) * (e.role == 1 ? 0.35f : 1.0f) * difficultySpread();  // (a marksman aims)
        static const bool shotTrace = std::getenv("GVR_HIT_TRACE") != nullptr;
        if (shotTrace) LOGI("enemy shot: Reaper %u%s at %.1f m", e.uid, e.role == 1 ? " (marksman)" : "", length(target - muzzle));
        dir = normalize(dir + Vec3{frand() - 0.5f, (frand() - 0.5f) * 0.6f, frand() - 0.5f} * spread);
        enemyFire_.spawnShot(muzzle, dir);
        sfx("rh_sprsh", muzzle, 0.8f, 0.9f + 0.2f * frand());
    }
    // Raiders firing at a base building (game_app_base.cpp sets Enemy::siege).
    for (int i : enemies_.siegeShots) {
        const Enemy& e = enemies_.enemies[size_t(i)];
        const Quat r = Quat::fromAxisAngle({0, 1, 0}, e.yaw);
        Vec3 muzzle = e.pos + Vec3{0, 1.5f, 0} + r.rotate(Vec3{0.3f, 0, -0.6f});
        reaperMuzzle(e, muzzle);
        Vec3 dir = normalize(e.siegeAt - muzzle);
        dir = normalize(dir + Vec3{frand() - 0.5f, (frand() - 0.5f) * 0.4f, frand() - 0.5f} * 0.08f);
        enemyFire_.spawnShot(muzzle, dir);
        sfx("rh_sprsh", muzzle, 0.6f, 0.9f + 0.2f * frand());
    }
    enemyFire_.update(dt * enemyTimeScale(), false, Vec3{}, Vec3{0, 0, -1});  // (SLOW TIME: their shots too)
    // Next wave after the last Reaper has fallen.
    { GVR_PROF("chars"); updateCharacters(dt); }
    { GVR_PROF("gore"); updateGore(dt); }
    { GVR_PROF("destruction"); updateDestruction(dt); }
    { GVR_PROF("burn"); updateBurn(); }
    { GVR_PROF("villagers"); updateVillagers(dt); updateVillagerHands(dt, lastInput_); updateSmartieTouch(dt, lastInput_); }
    { GVR_PROF("vimps"); updateVimps(dt); }
    { GVR_PROF("birds"); updateBirds(dt); updateHitchers(dt); }
    { GVR_PROF("fish"); updateFish(dt); updateSeaMonster(dt); }
    { GVR_PROF("ambience"); updateAmbience(dt); updateTreeShake(dt); updateSpeedWind(dt); updatePlankton(dt); }
    { GVR_PROF("npc ragdolls"); updateNpcRagdolls(dt); }
    { GVR_PROF("bosses"); updateBosses(dt * enemyTimeScale()); }
    { GVR_PROF("sams"); updateSams(dt * enemyTimeScale()); }
    { GVR_PROF("fx"); updateFx(dt); }
    { GVR_PROF("spells"); stepSpells(dt); }
    updateHorn(dt);
    // Sentries killed by their fall.
    for (int i : enemies_.landed) {
        const Enemy& e = enemies_.enemies[size_t(i)];
        if (e.objective >= 0) completeObjective(e.objective);
        blood_.pool(e.pos, 1.4f);
        bleed(e.pos + Vec3{0, 0.4f, 0}, Vec3{0, 1, 0}, true, 1.0f);
        sfx("me_hdfl1", e.pos, 1.0f, 0.8f);
    }
    { GVR_PROF("mission"); updateMission(dt); }
    if (enemies_.aliveCount() == 0 && !missionHostiles_ && objectives_.empty() && !skirmish_) {
        waveTimer_ += dt;
        if (waveTimer_ > 8.0f) { waveTimer_ = 0; spawnWave(); }
    } else {
        waveTimer_ = 0;
    }
}

}  // namespace gvr
