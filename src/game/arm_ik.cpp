#include "game/arm_ik.h"

#include <algorithm>
#include <cmath>

namespace gvr {

static Vec3 posOf(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }

Quat rotationBetween(const Vec3& a0, const Vec3& b0) {
    const Vec3 a = normalize(a0), b = normalize(b0);
    const float d = dot(a, b);
    if (d > 0.999999f) return Quat{};
    if (d < -0.999999f) {
        Vec3 axis = cross(a, Vec3{1, 0, 0});
        if (length(axis) < 1e-4f) axis = cross(a, Vec3{0, 1, 0});
        return Quat::fromAxisAngle(normalize(axis), kPi);
    }
    const Vec3 c = cross(a, b);
    return normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

void rotateSubtree(std::vector<Mat4>& world, const std::vector<std::vector<int>>& children, int root,
                   const Vec3& pivot, const Mat4& r) {
    const Mat4 xf = Mat4::translation(pivot) * r * Mat4::translation(pivot * -1.0f);
    std::vector<int> stack{root};
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        world[size_t(n)] = xf * world[size_t(n)];
        for (int c : children[size_t(n)]) stack.push_back(c);
    }
}

// Rotation (as Mat4) mapping orthonormal basis {a0,a1,a2} onto {b0,b1,b2}: B * A^T.
static Mat4 basisRotation(const Vec3 a[3], const Vec3 b[3]) {
    Mat4 m = Mat4::identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = 0;
            for (int k = 0; k < 3; ++k) {
                const float bk[3] = {b[k].x, b[k].y, b[k].z}, ak[3] = {a[k].x, a[k].y, a[k].z};
                s += bk[r] * ak[c];
            }
            m.m[c * 4 + r] = s;
        }
    return m;
}

static void handBasis(const std::vector<Mat4>& w, const ArmChain& c, Vec3 out[3], float* palmDist) {
    const Vec3 h = posOf(w[size_t(c.hand)]);
    Vec3 f{0, 0, 0};
    int n = 0;
    for (int i = 0; i < 4; ++i)
        if (c.finger[i] >= 0) { f += posOf(w[size_t(c.finger[i])]); ++n; }
    f = f * (1.0f / float(n));
    out[0] = normalize(f - h);
    const Vec3 across = (posOf(w[size_t(c.finger[0])]) + posOf(w[size_t(c.finger[1] >= 0 ? c.finger[1] : c.finger[0])])) * 0.5f -
                        (posOf(w[size_t(c.finger[3])]) + posOf(w[size_t(c.finger[2] >= 0 ? c.finger[2] : c.finger[3])])) * 0.5f;
    out[1] = normalize(across - out[0] * dot(across, out[0]));
    out[2] = cross(out[0], out[1]);
    if (palmDist) *palmDist = length(f - h) * 0.6f;
}

void stretchBone(std::vector<Mat4>& world, const std::vector<std::vector<int>>& children, int node, int child,
                 float factor) {
    const Vec3 delta = (posOf(world[size_t(child)]) - posOf(world[size_t(node)])) * (factor - 1.0f);
    world[size_t(node)] = world[size_t(node)] * Mat4::scale({1.0f, 1.0f, factor});
    const Mat4 tr = Mat4::translation(delta);
    std::vector<int> stack{child};
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        world[size_t(n)] = tr * world[size_t(n)];
        for (int k : children[size_t(n)]) stack.push_back(k);
    }
}

float solveArmIk(std::vector<Mat4>& world, const std::vector<std::vector<int>>& children, const ArmChain& c,
                 const ArmTarget& t) {
    if (!c.valid()) return -1.0f;
    if (t.armStretch != 1.0f) {
        stretchBone(world, children, c.upper, c.fore, t.armStretch);
        stretchBone(world, children, c.fore, c.hand, t.armStretch);
    }
    // 1. Hand orientation first (in the current pose) to know where the wrist must go.
    Vec3 cur[3], want[3];
    float palm = 0;
    handBasis(world, c, cur, &palm);
    want[0] = normalize(t.forward);
    want[1] = normalize(t.up - want[0] * dot(t.up, want[0]));
    want[2] = cross(want[0], want[1]);
    const Vec3 wristTarget = t.grip - want[0] * palm;

    // 2a. Shoulder assist: when the target is near or beyond full reach, swing the clavicle
    // toward it (up to ~11 degrees), like a person reaching out.
    if (c.clavicle >= 0) {
        const Vec3 C = posOf(world[size_t(c.clavicle)]);
        const Vec3 S0 = posOf(world[size_t(c.upper)]);
        const float reach = length(posOf(world[size_t(c.fore)]) - S0) + length(posOf(world[size_t(c.hand)]) - posOf(world[size_t(c.fore)]));
        const float d0 = length(wristTarget - S0);
        const float frac = std::max(0.0f, std::min(1.0f, (d0 - 0.9f * reach) / (0.3f * reach)));
        if (frac > 0.0f && length(S0 - C) > 1e-4f) {
            const Quat q = rotationBetween(S0 - C, wristTarget - C);
            const float angle = 2.0f * std::acos(std::min(1.0f, std::fabs(q.w)));
            if (angle > 1e-4f) {
                const Vec3 axis = normalize(Vec3{q.x, q.y, q.z} * (q.w < 0 ? -1.0f : 1.0f));
                const float use = std::min(angle, 0.2f * frac);  // at most ~11 degrees
                rotateSubtree(world, children, c.clavicle, C, Mat4::rotation(Quat::fromAxisAngle(axis, use)));
            }
        }
    }
    // 2b. Two-bone solve: shoulder S, elbow E, wrist H.
    const Vec3 S = posOf(world[size_t(c.upper)]);
    const Vec3 E = posOf(world[size_t(c.fore)]);
    const Vec3 H = posOf(world[size_t(c.hand)]);
    const float a = length(E - S), b = length(H - E);
    Vec3 toT = wristTarget - S;
    float d = length(toT);
    if (d < 1e-5f || a < 1e-5f || b < 1e-5f) return -1.0f;
    const Vec3 dir = toT * (1.0f / d);
    const float dc = std::max(std::fabs(a - b) + 1e-3f, std::min(d, a + b - 1e-3f));
    // Elbow lies on the circle around dir; choose the point toward the pole.
    const float x = (a * a - b * b + dc * dc) / (2.0f * dc);  // distance along dir to the circle centre
    const float rr = std::sqrt(std::max(0.0f, a * a - x * x));
    Vec3 pole = t.pole - dir * dot(t.pole, dir);
    if (length(pole) < 1e-4f) pole = normalize(cross(dir, Vec3{0, 0, 1}));
    pole = normalize(pole);
    const Vec3 Ewant = S + dir * x + pole * rr;

    rotateSubtree(world, children, c.upper, S, Mat4::rotation(rotationBetween(E - S, Ewant - S)));
    const Vec3 E1 = posOf(world[size_t(c.fore)]);
    const Vec3 H1 = posOf(world[size_t(c.hand)]);
    const Vec3 Hwant = S + dir * dc;
    rotateSubtree(world, children, c.fore, E1, Mat4::rotation(rotationBetween(H1 - E1, Hwant - E1)));

    // 3. Forearm twist: the forearm carries part of the wrist roll, so the rigid hand does not
    // visibly break away from the forearm when the controller is rolled.
    if (t.twistShare > 0.0f) {
        const Vec3 E2 = posOf(world[size_t(c.fore)]);
        const Vec3 f = normalize(posOf(world[size_t(c.hand)]) - E2);
        handBasis(world, c, cur, nullptr);
        Vec3 cu = cur[1] - f * dot(cur[1], f), wu = want[1] - f * dot(want[1], f);
        if (length(cu) > 1e-3f && length(wu) > 1e-3f) {
            cu = normalize(cu);
            wu = normalize(wu);
            const float roll = std::atan2(dot(cross(cu, wu), f), dot(cu, wu));
            rotateSubtree(world, children, c.fore, E2, Mat4::rotation(Quat::fromAxisAngle(f, roll * t.twistShare)));
        }
    }
    // 4. Hand orientation about the (new) wrist, then hand scale.
    handBasis(world, c, cur, nullptr);
    const Vec3 wrist = posOf(world[size_t(c.hand)]);
    rotateSubtree(world, children, c.hand, wrist, basisRotation(cur, want));
    if (t.handScale != 1.0f)
        rotateSubtree(world, children, c.hand, wrist, Mat4::scale({t.handScale, t.handScale, t.handScale}));
    return length(posOf(world[size_t(c.hand)]) - wristTarget);
}

void curlFingers(std::vector<Mat4>& world, const std::vector<std::vector<int>>& children, const ArmChain& c,
                 const float curl[4]) {
    if (!c.valid()) return;
    Vec3 basis[3];
    handBasis(world, c, basis, nullptr);
    // Palm side: the middle fingers rest slightly bent toward the palm (the outer digits can
    // be splayed outward, so they do not vote). One direction for the whole hand.
    Vec3 dev{0, 0, 0};
    for (int i = 1; i <= 2; ++i) {
        if (c.finger[i] < 0 || c.fingerMid[i] < 0) continue;
        const Vec3 seg = posOf(world[size_t(c.fingerMid[i])]) - posOf(world[size_t(c.finger[i])]);
        dev += seg - basis[0] * dot(seg, basis[0]);
    }
    Vec3 palm = dev - basis[1] * dot(dev, basis[1]);
    palm = length(palm) > 1e-4f ? normalize(palm) : basis[2];
    for (int i = 0; i < 4; ++i) {
        const int base = c.finger[i], mid = c.fingerMid[i];
        if (base < 0 || mid < 0 || curl[i] <= 0.0f) continue;
        const Vec3 b = posOf(world[size_t(base)]);
        const Vec3 seg = posOf(world[size_t(mid)]) - b;
        const Vec3 axis = normalize(cross(normalize(seg), palm));
        rotateSubtree(world, children, base, b, Mat4::rotation(Quat::fromAxisAngle(axis, curl[i] * 1.3f)));
        const Vec3 m = posOf(world[size_t(mid)]);
        rotateSubtree(world, children, mid, m, Mat4::rotation(Quat::fromAxisAngle(axis, curl[i] * 1.2f)));
    }
}

}  // namespace gvr
