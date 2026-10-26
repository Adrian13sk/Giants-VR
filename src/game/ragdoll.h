// Rework (user: "Try to implement ragdolls somehow"): a Verlet ragdoll over a skinned character's
// node tree. The pose a body dies in is captured (model-space node matrices); the nodes that stand
// apart from their simulated ancestor become particles, held by sticks to their parent particle,
// rigid among siblings (hips, shoulders), and softly to the grandparent (a little posture). Each
// pose, every simulated node's frame is rebuilt from its particle and its child's (and parent's),
// and the nodes in between follow their simulated ancestor rigidly. Our own design.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/math.h"

namespace gvr {

class Ragdoll {
public:
    // world: model-space node matrices (game units) of the pose to start from; parent[i]: node
    // parents (-1 roots); toWorld: model space -> runtime world (metres); vel: the body's velocity
    // (m/s); spin: angular velocity (rad/s, world) about the body's centre; minBone: model-space
    // distance below which a node rides on its ancestor instead of being a particle.
    void start(const std::vector<Mat4>& world, const std::vector<int>& parent, const Mat4& toWorld, const Vec3& vel,
               const Vec3& spin, float minBone);
    void stop() { active = false; pinned_ = -1; }
    // Advances the simulation (fixed 1/60 s substeps); ground(x, z) is the terrain height.
    void step(float dt, const std::function<float(float, float)>& ground);
    // Writes the simulated pose into `world` (same node layout as at start), for the current
    // model->world transform.
    void apply(std::vector<Mat4>& world, const Mat4& toWorld) const;
    // Holds the particle nearest the body's middle at `at` (a blade through him); unpin lets go
    // (the motion it had carries on).
    void pin(const Vec3& at);
    void unpin() { pinned_ = -1; }
    // Pushes the whole body (a blast, a blow).
    void kick(const Vec3& dv);
    Vec3 centre() const;
    float lowest() const;
    size_t particles() const { return x_.size(); }
    Vec3 particle(size_t i) const { return x_[i]; }

    bool active = false, asleep = false;
    uint32_t uid = 0;  // owner (the game layer: the enemy using this model)
    int clip = -1;     // the clip the pose was captured from (keep posing with it)
    float age = 0;
    float impact = 0;
    double lastThud = -1e9;  // (game layer: when its last landing sound played)  // the hardest landing (m/s, downward) since the game layer last read it (sounds)
    // Game layer: start at the next pose with this velocity and spin; posed once since asleep.
    bool pending = false, posedAsleep = false;
    Vec3 startVel, startSpin;

private:
    struct Stick { int a, b; float rest, k; };  // k < 0: keeps at least -k*rest apart
    std::vector<Mat4> rest_;      // model-space node matrices at capture
    std::vector<Mat4> restInv_;
    std::vector<int> owner_;      // node -> particle whose frame it rides on
    std::vector<int> pnode_;      // particle -> node
    std::vector<int> pparent_;    // particle -> parent particle (-1)
    std::vector<int> pchild_;     // particle -> primary child particle (-1)
    std::vector<Vec3> x_, xp_;    // world positions, previous
    std::vector<Stick> sticks_;
    int pinned_ = -1, middle_ = 0;
    Vec3 pinAt_;
    float still_ = 0, acc_ = 0;
};

}  // namespace gvr
