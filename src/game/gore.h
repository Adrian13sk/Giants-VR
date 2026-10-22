// Blood, gore and corpse cleaners (renderer-independent, unit-tested).
//
// From the user's description of the original: shot enemies bleed; killed ones lie where they
// fell; after a while small scavengers ("Cleaners": Cleaner_l0 with Cleaner_walk / _eat / _jump
// and the CL_EATST / CL_EATLP / CL_EATEN sounds) come and eat the body, which disappears; a body
// can also be shot until it bursts in a spray of blood or falls apart into pieces. The art is the
// original (aablood.gzp's red efx_blood / efx_splat / efx_spray, the rpman_chunk_* and
// rip_gore_* pieces); the numbers (drop counts, timings, how many Cleaners) are ours.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/math.h"

namespace gvr {

// A drop of blood in flight: lands as a small splat (on the ground) or a cloud (in water).
struct BloodDrop {
    Vec3 pos, vel;
    float size = 0.1f, life = 0;
};

// A splat on the ground or a red cloud on the water: grows to its size, then fades out.
struct BloodDecal {
    Vec3 pos, normal{0, 1, 0};
    float size = 0.3f, grow = 0.2f, angle = 0, age = 0, life = 40.0f;
    int texture = 0;      // 0 efx_splat, 1 efx_splat2, 2 bloodblob (pools, clouds in water)
    bool water = false;
    float alpha() const;  // current opacity (fades in quickly, out over the last quarter)
    float scale() const;  // current size
};

// A puff of sprayed blood (billboard), expanding and fading in a fraction of a second.
struct BloodSpray {
    Vec3 pos, vel;
    float size = 0.4f, age = 0, life = 0.45f;
};

class BloodSystem {
public:
    std::function<float(float, float)> groundHeight;  // terrain (and sea floor) height
    float seaLevel = -1e9f;
    size_t maxDrops = 160, maxDecals = 64, maxSprays = 40;
    std::vector<BloodDrop> drops;
    std::vector<BloodDecal> decals;
    std::vector<BloodSpray> sprays;

    // A wound: drops thrown along `dir` (spread around it), a spray puff at the wound.
    void burst(const Vec3& at, const Vec3& dir, int count, float speed, float size);
    // A spreading pool under a body (or a red cloud where it lies in water).
    void pool(const Vec3& at, float size);
    void update(float dt);

private:
    void addDecal(const BloodDecal& d);
    float rnd();
    unsigned rng_ = 4242u;
};

// --- Cleaners: scavengers that eat corpses --------------------------------------------------
enum class CleanerState { Approach, Eat, Leave };

struct Cleaner {
    Vec3 pos;
    float yaw = 0, speed = 0, t = 0, anim = 0;
    CleanerState state = CleanerState::Approach;
    uint32_t target = 0;  // corpse id
    Vec3 targetPos;
    float offset = 0;     // angle around the body where it eats
    float sink = 0;       // leaving: digs away into the ground
    bool remove = false;
    int slot = -1;        // model (owned by the game app)
};

struct CleanerEvent {
    enum Kind { Arrive, StartEat, Finished, Gone } kind;
    uint32_t target;
    Vec3 pos;
};

class CleanerDirector {
public:
    std::function<float(float, float)> groundHeight;
    float walkSpeed = 1.6f;        // m/s (scaled by the walk clip's playback)
    float eatTime = 9.0f;          // s: two Cleaners eat a body in about this long
    float spawnDistance = 9.0f;    // m from the body
    std::vector<Cleaner> cleaners;

    // Sends `count` Cleaners to body `id` at `at`, approaching from `fromYaw` (radians, any).
    void call(uint32_t id, const Vec3& at, int count, float fromYaw);
    bool busyWith(uint32_t id) const;
    // `corpse(id, pos)` reports where a body is (false: gone, e.g. blown apart). `eaten` receives
    // the eating progress made this frame per body id (0..1 in total per body).
    void update(float dt, const std::function<bool(uint32_t, Vec3&)>& corpse, std::vector<std::pair<uint32_t, float>>& eaten,
                std::vector<CleanerEvent>& events);

private:
    float rnd();
    unsigned rng_ = 977u;
};

}  // namespace gvr
