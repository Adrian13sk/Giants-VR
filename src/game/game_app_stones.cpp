// GameApp, rework: stones you can pick up. The stones lying near the player (game_app_grass.cpp) are
// the ground's own; the Meccaryn and Delphi bend down, squeeze a grip with the hand on a hand-sized one
// and it comes off the ground into the hand. Thrown, it flies, bounces and clatters; a hard throw that
// hits a Reaper, a creature or a Smartie hurts it (a head hit twice as much). Let go gently, it drops.
// Stones that lie still far behind are forgotten. Our own design.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/creature.h"
#include "game/game_app.h"
#include "game/villager.h"
#include "scene/asset_world.h"
#include "scene/terrain.h"

namespace gvr {

namespace {
constexpr size_t kMaxStones = 16;
constexpr float kHandSize = 0.34f;  // the largest stone a hand takes (its longest half-axis x 2, m)
float stoneSize(const Vec3& scale) { return std::max(scale.x, std::max(scale.y, scale.z)); }
}  // namespace

void GameApp::updateStones(float dt, const InputFrame& in) {
    if (stonesWorld_ != assetWorld_.get()) {
        stonesWorld_ = assetWorld_.get();
        stones_.clear();
    }
    const bool can = playMode_ && firstPerson_ && !dying_ && hero_ != Hero::Kabuto && !cutsceneActive() && !riding_ && assetWorld_;
    static const bool trace = std::getenv("GVR_GRAB_TRACE") != nullptr;
    auto ground = [&](const Vec3& p) { return player_.groundHeight ? player_.groundHeight(p.x, p.z) : player_.position.y - 1.0f; };
    // Taking one: a grip's edge with the hand on a loose stone or one of the ground's.
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        const bool g = c.squeeze > 0.7f;
        const bool edge = g && !stoneGripWas_[h];
        stoneGripWas_[h] = g;
        if (!can || !edge || !c.gripPoseValid || stoneInHand(h) || heldBody_.hand == h || (heldVillager_ != 0 && heldVillagerHand_ == h) || climbHold_[h]) continue;
        const Vec3 hp = rig_.toWorld(c.gripPose).position;
        const Vec3 p = reachPoint(hp);  // (where it reaches: the hand, or lower - GROUND REACH)
        // A dropped gun or a cut-off limb lying there: taken first.
        if (grabGib(h, hp, p, rig_.toWorld(c.gripPose).orientation)) continue;
        float best = 1e9f;
        int loose = -1;
        for (size_t i = 0; i < stones_.size(); ++i) {
            const LooseStone& st = stones_[i];
            if (st.hand >= 0) continue;
            const float d = length(st.pos - p) - stoneSize(st.d.scale);
            if (d < 0.22f && d < best) { best = d; loose = int(i); }
        }
        PebbleDesc pick;
        int pgx = 0, pgz = 0;
        bool found = false;
        if (loose < 0 && pebbleRadius_ > 0) {
            const int cx = int(std::floor(p.x / kPebbleCell)), cz = int(std::floor(p.z / kPebbleCell));
            for (int gz = cz - 1; gz <= cz + 1; ++gz)
                for (int gx = cx - 1; gx <= cx + 1; ++gx) {
                    PebbleDesc d;
                    if (!pebbleAt(gx, gz, pebbleCentre_, pebbleRadius_, d) || pebbleTaken(gx, gz)) continue;
                    if (stoneSize(d.scale) * 2.0f > kHandSize) continue;  // (a boulder: not in one hand)
                    const float dist = length(d.base - p) - stoneSize(d.scale);
                    if (dist < 0.22f && dist < best) { best = dist; pick = d; pgx = gx; pgz = gz; found = true; }
                }
        }
        if (loose < 0 && !found) {
            if (trace) {
                // (the nearest hand-sized ground stone, for writing tests)
                float nd = 1e9f;
                Vec3 at;
                const int cx = int(std::floor(p.x / kPebbleCell)), cz = int(std::floor(p.z / kPebbleCell));
                for (int gz = cz - 4; gz <= cz + 4; ++gz)
                    for (int gx = cx - 4; gx <= cx + 4; ++gx) {
                        PebbleDesc d;
                        if (!pebbleAt(gx, gz, pebbleCentre_, pebbleRadius_, d) || pebbleTaken(gx, gz) || stoneSize(d.scale) * 2.0f > kHandSize) continue;
                        if (length(d.base - p) < nd) { nd = length(d.base - p); at = d.base; }
                    }
                LOGI("grab trace: hand %d at (%.2f %.2f %.2f) - no stone within reach; nearest hand-sized one %.2f m away at (%.2f %.2f %.2f)", h, p.x, p.y, p.z, nd, at.x, at.y, at.z);
            }
            continue;
        }
        if (found) {
            takenPebbles_.push_back({pgx, pgz});
            if (stones_.size() >= kMaxStones) {  // (the oldest lying stone makes room)
                size_t old = 0;
                float age = -1;
                for (size_t i = 0; i < stones_.size(); ++i)
                    if (stones_[i].hand < 0 && !stones_[i].flying && stones_[i].life > age) { age = stones_[i].life; old = i; }
                if (age >= 0) stones_.erase(stones_.begin() + long(old));
            }
            if (stones_.size() < kMaxStones) {
                LooseStone st;
                st.d = pick;
                st.pos = pick.base;
                stones_.push_back(st);
                loose = int(stones_.size()) - 1;
            }
            buildPebbles(pebbleCentre_, pebbleRadius_);  // (gone from the ground)
        }
        if (loose < 0) continue;
        LooseStone& st = stones_[size_t(loose)];
        st.hand = h;
        st.flying = false;
        st.nRecent = 0;
        st.prev = c.gripPose.position;  // (tracking space: handVelocity)
        st.hit = 0;
        haptics_.push_back({h, 0.35f, 0.05f});
        sfx("ex_stons", st.pos, 0.25f, 1.6f);
        LOGI("Picked up a stone (%.2f m) with hand %d", 2.0f * stoneSize(st.d.scale), h);
    }
    // Gibs in a hand: they follow it; let go, they fly with its speed (the gib physics takes over).
    for (auto& g : gibs_) {
        if (g.hand < 0) continue;
        const ControllerState& c = in.hand[g.hand];
        if (!can || !c.gripPoseValid) { g.hand = -1; continue; }
        const Pose gp = rig_.toWorld(c.gripPose);
        const Quat rot = gp.orientation * g.heldRot;
        const Vec3 p = gp.position + gp.orientation.rotate(g.heldOff);
        if (dt > 0) g.recent[g.nRecent++ % 12] = handVelocity(c.gripPose.position, g.prev, dt);
        g.pos = p;
        // (axis-angle for the draw)
        const float w = std::clamp(rot.w, -1.0f, 1.0f), sn = std::sqrt(std::max(0.0f, 1.0f - w * w));
        g.angle = 2.0f * std::acos(w);
        g.axis = sn > 1e-4f ? Vec3{rot.x / sn, rot.y / sn, rot.z / sn} : Vec3{0, 1, 0};
        g.spin = 0;
        g.vel = Vec3{0, 0, 0};
        if (c.squeeze < 0.35f) {
            Vec3 thr{0, 0, 0};
            for (int k = 0; k < std::min(g.nRecent, 12); ++k)
                if (length(g.recent[k]) > length(thr)) thr = g.recent[k];
            g.vel = length(thr) > 1.5f ? thr : Vec3{0, 0, 0};
            g.spin = length(thr) > 1.5f ? 3.0f + 6.0f * frand() : 0.0f;
            g.hand = -1;
            g.life = 0;
            g.thrown = length(g.vel) > 4.0f;
            LOGI("Let go of a %s (%.1f m/s)", g.owner ? "gun" : "piece", length(g.vel));
        }
    }
    // Held, flying, lying.
    for (size_t i = 0; i < stones_.size(); ++i) {
        LooseStone& st = stones_[i];
        const float sz = stoneSize(st.d.scale);
        if (st.hand >= 0) {
            const ControllerState& c = in.hand[st.hand];
            if (!can || !c.gripPoseValid) { st.hand = -1; st.flying = true; st.vel = Vec3{0, 0, 0}; continue; }
            const Pose gp = rig_.toWorld(c.gripPose);
            const Vec3 p = gp.position + gp.orientation.rotate(Vec3{0, -0.03f - 0.35f * sz, -0.06f});  // (in the palm, under the fingers)
            if (dt > 0) st.recent[st.nRecent++ % 12] = handVelocity(c.gripPose.position, st.prev, dt);
            st.pos = p;
            st.rot = gp.orientation;
            if (trace && st.nRecent % 36 == 1) LOGI("grab trace: stone in hand %d at (%.2f %.2f %.2f), head (%.2f %.2f %.2f)", st.hand, p.x, p.y, p.z,
                                                     rig_.toWorld(in.head).position.x, rig_.toWorld(in.head).position.y, rig_.toWorld(in.head).position.z);
            if (c.squeeze < 0.35f) {
                Vec3 thr{0, 0, 0};
                for (int k = 0; k < std::min(st.nRecent, 12); ++k)
                    if (length(st.recent[k]) > length(thr)) thr = st.recent[k];
                st.vel = length(thr) > 1.5f ? thr * 1.15f : Vec3{0, 0, 0};  // (a flick of the wrist adds a little)
                st.spinAxis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
                st.spin = 4.0f + 10.0f * frand();
                st.hand = -1;
                st.flying = true;
                st.life = 0;
                if (length(st.vel) > 1.5f) LOGI("Threw a stone at %.1f m/s", length(st.vel));
            }
            continue;
        }
        st.life += dt;
        if (!st.flying) continue;
        const Vec3 was = st.pos;
        st.vel.y -= 9.8f * dt;
        st.pos = st.pos + st.vel * dt;
        st.rot = Quat::fromAxisAngle(st.spinAxis, st.spin * dt) * st.rot;
        const float speed = length(st.vel);
        // Into the water: a plop, then it sinks out of sight.
        if (was.y >= player_.seaLevel && st.pos.y < player_.seaLevel) {
            waterSplash(st.pos, 0.2f + 0.3f * sz, 1);
            st.vel = st.vel * 0.3f;
        }
        if (st.pos.y < player_.seaLevel) st.vel = st.vel * std::max(0.0f, 1.0f - 4.0f * dt);
        // A hit: the first one it meets while going fast.
        if (speed > 4.0f && st.hit == 0) {
            const Vec3 dir = st.vel * (1.0f / speed);
            for (size_t k = 0; k < enemies_.enemies.size() && st.hit == 0; ++k) {
                const Enemy& e = enemies_.enemies[k];
                if (e.state == EnemyState::Dead) continue;
                const float top = enemies_.bodyHeight(e);
                if (std::hypot(st.pos.x - e.pos.x, st.pos.z - e.pos.z) > 0.42f + sz || st.pos.y < e.pos.y || st.pos.y > e.pos.y + top) continue;
                const bool head = st.pos.y > e.pos.y + 0.82f * top;
                st.hit = e.uid;
                const bool killed = thrownHitEnemy(int(k), (4.0f + 1.1f * speed) * (head ? 2.0f : 1.0f), st.pos, dir);
                enemies_.push(int(k), Vec3{dir.x, 0, dir.z} * std::min(3.0f, 0.25f * speed));
                sfx(head ? "bi_hit2" : "bi_hit1", st.pos, 0.8f, 1.2f);
                LOGI("A stone hits Reaper %u%s at %.1f m/s%s", e.uid, head ? " on the head" : "", speed, killed ? " - down" : "");
            }
            for (size_t k = 0; k < creatures_.creatures.size() && st.hit == 0; ++k) {
                const Creature& cr = creatures_.creatures[k];
                if (cr.state == CreatureState::Dead || CreatureDirector::underground(cr)) continue;
                if (length(st.pos - (cr.pos + Vec3{0, 0.5f * cr.scale, 0})) > 0.7f * cr.scale + sz) continue;
                st.hit = 0x80000000u | cr.uid;
                thrownHitCreature(int(k), 4.0f + 1.1f * speed, st.pos, dir);
                sfx("bi_hit1", st.pos, 0.8f, 1.0f);
                LOGI("A stone hits creature %u at %.1f m/s", cr.uid, speed);
            }
            for (size_t k = 0; k < villagers_.villagers.size() && st.hit == 0; ++k) {
                const Villager& v = villagers_.villagers[k];
                if (!VillagerDirector::alive(v) || length(st.pos - (v.pos + Vec3{0, 0.45f, 0})) > 0.4f + sz) continue;
                st.hit = 0x40000000u | v.uid;
                hitVillager(int(k), 2.0f + 0.6f * speed, st.pos, dir);
                LOGI("A stone hits Smartie %u at %.1f m/s", v.uid, speed);
            }
            if (st.hit != 0) st.vel = st.vel * -0.2f;  // (it bounces off)
        }
        // The ground: a clatter and a bounce, then it lies.
        const float g = ground(st.pos) + 0.5f * st.d.scale.y;
        if (st.pos.y < g) {
            if (speed > 2.5f && length(st.pos - player_.position) < 40.0f) sfx("ex_stons", st.pos, std::min(1.0f, 0.15f + 0.07f * speed), 0.9f + 0.4f * frand());
            st.pos.y = g;
            st.vel = Vec3{st.vel.x * 0.55f, std::fabs(st.vel.y) * 0.3f, st.vel.z * 0.55f};
            st.spin *= 0.6f;
            if (length(st.vel) < 0.7f) { st.flying = false; st.vel = Vec3{0, 0, 0}; }
        }
    }
    // Forgotten: lying a minute, far behind.
    stones_.erase(std::remove_if(stones_.begin(), stones_.end(), [&](const LooseStone& st) {
                      return st.hand < 0 && !st.flying && st.life > 60.0f && length(st.pos - player_.position) > 40.0f;
                  }),
                  stones_.end());
    // The mesh, once a frame.
    if (!stoneMesh_ && !stones_.empty()) {
        MeshData md;
        const size_t nv = kMaxStones * size_t(kPebbleTris) * 3;
        md.vertices.assign(nv, Vertex{});
        for (size_t k = 0; k < nv; ++k) md.indices.push_back(uint32_t(k));
        stoneMesh_ = renderer_.uploadMesh(md);
        stoneBuf_.assign(nv, Vertex{});
    }
    if (!stoneMesh_) return;
    const size_t used = stones_.size() * size_t(kPebbleTris) * 3;
    // Only when something moved (held or flying) or the set changed: no upload while they all lie still
    // (a buffer update the GPU may still be drawing from can stall a tiler).
    bool moving = used != stoneUsed_;
    for (const auto& st : stones_) moving = moving || st.hand >= 0 || st.flying;
    if (!moving && !stoneDirty_) return;
    stoneDirty_ = moving;  // (one more upload after the last one stops: its resting pose)
    for (size_t k = 0; k < stones_.size(); ++k) emitPebble(&stoneBuf_[k * size_t(kPebbleTris) * 3], stones_[k].d, stones_[k].rot, stones_[k].pos);
    for (size_t k = used; k < std::max(used, stoneUsed_); ++k) stoneBuf_[k] = Vertex{};
    const size_t upto = std::max(used, stoneUsed_);
    if (upto) renderer_.updateMeshVertexRange(stoneMesh_, 0, stoneBuf_.data(), upto);
    stoneUsed_ = used;
}

Vec3 GameApp::updateSwimStrokes(float dt, const InputFrame& in) {
    const bool can = playMode_ && firstPerson_ && !dying_ && hero_ != Hero::Kabuto && player_.swimming() && !cutsceneActive() && dt > 0;
    // Water drag: the push dies away in about a second.
    strokeVel_ = strokeVel_ * std::exp(-1.1f * dt);
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        if (!can || !c.gripPoseValid || c.squeeze < 0.6f || stoneInHand(h) || heldBody_.hand == h) { strokeHave_[h] = false; continue; }
        // The hand's own movement (in tracking space, turned into the world: walking or a snap turn doesn't count).
        const Vec3 p = c.gripPose.position;
        if (strokeHave_[h]) {
            Vec3 d = rig_.worldFromTracking.orientation.rotate(p - strokePrev_[h]);
            d.y = 0;
            if (length(d) < 0.5f) strokeVel_ = strokeVel_ - d * 2.5f;  // (a 0.6 m pull: about 1.5 m/s)
        }
        strokePrev_[h] = p;
        strokeHave_[h] = true;
    }
    if (!can) strokeVel_ = Vec3{0, 0, 0};
    const float L = length(strokeVel_), cap = 1.6f * player_.swimSpeed;
    if (L > cap) strokeVel_ = strokeVel_ * (cap / L);
    static const bool trace = std::getenv("GVR_SWIM_TRACE") != nullptr;
    static float tt = 0;
    if (trace && can && (tt += dt) > 0.25f) { tt = 0; LOGI("swim trace: stroke %.2f m/s, speed %.2f m/s", length(strokeVel_), length(Vec3{player_.velocity.x, 0, player_.velocity.z})); }
    return strokeVel_;
}

Vec3 GameApp::reachPoint(const Vec3& hand) const {
    if (!settings_.reachAssist || hero_ == Hero::Kabuto || !player_.groundHeight || !lastInput_.headValid) return hand;
    // Only a hand reaching out in front (arms hanging at your sides, or a grip squeezed to sprint, take nothing).
    const Pose head = rig_.toWorld(lastInput_.head);
    Vec3 f = head.orientation.rotate(Vec3{0, 0, -1});
    f.y = 0;
    if (length(f) < 1e-3f) return hand;
    f = normalize(f);
    const float ahead = dot(Vec3{hand.x - head.position.x, 0, hand.z - head.position.z}, f);
    if (ahead < 0.22f) return hand;
    const float h = hand.y - player_.groundHeight(hand.x, hand.z);
    if (h > 1.1f) return hand;
    const float drop = std::clamp(h - 0.12f, 0.0f, 0.55f) * std::clamp((1.1f - h) / 0.3f, 0.0f, 1.0f) * std::clamp((ahead - 0.22f) / 0.1f, 0.0f, 1.0f);
    return hand - Vec3{0, drop, 0};
}

bool GameApp::stoneInHand(int h) const {
    for (const auto& st : stones_)
        if (st.hand == h) return true;
    for (const auto& g : gibs_)
        if (g.hand == h) return true;
    return false;
}

bool GameApp::grabGib(int h, const Vec3& hand, const Vec3& reach, const Quat& handRot) {
    Gib* best = nullptr;
    float bd = 0.35f;
    for (auto& g : gibs_) {
        if (g.hand >= 0 || !g.model || (g.owner == 0 && !g.own) || g.life > 17.0f) continue;  // (guns and limbs; not sinking away)
        // Its middle in the world (a limb's position is its cut).
        const Vec3 c = (g.model->boundsMin + g.model->boundsMax) * 0.5f;
        const Mat4 xf = Mat4::translation(g.pos) * Mat4::rotation(Quat::fromAxisAngle(g.axis, g.angle)) * g.base * Mat4::scale({g.scale, g.scale, g.scale});
        const Vec4 wc = xf * Vec4{c.x, c.y, c.z, 1.0f};
        const Vec3 m{wc.x, wc.y, wc.z};
        const float d = std::min(length(m - hand), length(m - reach));
        if (d < bd) { bd = d; best = &g; }
    }
    if (!best) return false;
    Gib& g = *best;
    // Held where it is relative to the hand, brought to the hand (the grip is on it).
    const Quat inv{-handRot.x, -handRot.y, -handRot.z, handRot.w};
    g.heldRot = inv * Quat::fromAxisAngle(g.axis, g.angle);
    g.heldOff = Vec3{0, -0.04f, -0.06f};
    g.hand = h;
    g.nRecent = 0;
    g.prev = lastInput_.hand[h].gripPose.position;  // (tracking space: handVelocity)
    haptics_.push_back({h, 0.4f, 0.06f});
    if (g.owner) sfx("me_drpwp", g.pos, 0.4f, 1.4f);
    LOGI("Picked up a %s with hand %d", g.owner ? "dropped gun" : "severed piece", h);
    return true;
}

void GameApp::renderStones() {
    static int logged = 0;
    if (stoneUsed_ && logged < 3 && std::getenv("GVR_GRAB_TRACE")) { ++logged; LOGI("grab trace: drawing %zu stones (mesh %u, first vertex %.2f %.2f %.2f, stone at %.2f %.2f %.2f, size %.2f)", stones_.size(), unsigned(stoneMesh_), stoneBuf_[0].pos[0], stoneBuf_[0].pos[1], stoneBuf_[0].pos[2], stones_[0].pos.x, stones_[0].pos.y, stones_[0].pos.z, stones_[0].d.scale.x); }
    if (!stoneMesh_ || stoneUsed_ == 0 || renderer_.inReflectionPass()) return;
    DrawParams dp;
    dp.texture = assetWorld_ ? assetWorld_->terrain().groundTexture() : 0;
    dp.material = DrawParams::Material::Stone;
    dp.unlit = true;  // (the light baked where it lay)
    renderer_.draw(stoneMesh_, Mat4::identity(), dp);
}

}  // namespace gvr
