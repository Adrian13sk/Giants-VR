// Two-bone arm IK on world-space bone matrices (VR hands driving the character's arms).
//
// Works directly on the evaluated world matrices of an ANM hierarchy: each correction is a
// world-space rotation of a bone *subtree* about its joint, so mirrored nodes (negative
// determinant, used by the Meccaryn's right arm) need no special handling.
#pragma once

#include <vector>

#include "core/math.h"

namespace gvr {

struct ArmChain {
    int clavicle = -1;                     // optional: rotates a little to extend reach
    int upper = -1, fore = -1, hand = -1;  // shoulder joint, elbow joint, wrist joint nodes
    int finger[4] = {-1, -1, -1, -1};      // finger base nodes: [0],[1] index side, [2],[3] pinky side
    int fingerMid[4] = {-1, -1, -1, -1};   // second finger joints
    bool valid() const { return upper >= 0 && fore >= 0 && hand >= 0 && finger[0] >= 0 && finger[3] >= 0; }
};

struct ArmTarget {
    Vec3 grip;     // where the palm should be (model space)
    Vec3 forward;  // direction the knuckles point (controller pointing direction)
    Vec3 up;       // direction from pinky side to index side (controller up)
    Vec3 pole;     // direction the elbow should bend toward
    float twistShare = 0.5f;  // fraction of the wrist roll carried by the forearm (no wrist break)
    float handScale = 1.0f;   // uniform hand scale about the wrist
    float armStretch = 1.0f;  // lengthens upper arm and forearm (along the bone) before solving
};

// Stretches node's own geometry along its bone axis (local +Z, toward `child`) by `factor`
// and moves the child subtree out by the extra length.
void stretchBone(std::vector<Mat4>& world, const std::vector<std::vector<int>>& children, int node, int child,
                 float factor);

Quat rotationBetween(const Vec3& from, const Vec3& to);
// Rotates `root` and all its descendants about `pivot` by `r` (world space).
void rotateSubtree(std::vector<Mat4>& world, const std::vector<std::vector<int>>& children, int root,
                   const Vec3& pivot, const Mat4& r);
// Solves one arm. Returns the residual wrist distance (0 when reachable).
float solveArmIk(std::vector<Mat4>& world, const std::vector<std::vector<int>>& children, const ArmChain& c,
                 const ArmTarget& t);

// Curls the fingers toward the palm (0 = as animated, 1 = fist). curl[i] per finger. The palm
// side is taken from each finger's own rest bend, so mirrored hands need no special case.
void curlFingers(std::vector<Mat4>& world, const std::vector<std::vector<int>>& children, const ArmChain& c,
                 const float curl[4]);

}  // namespace gvr
