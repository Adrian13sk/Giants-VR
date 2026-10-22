// Minimal vector / quaternion / matrix math. Column-major 4x4 matrices (OpenGL convention).
// Coordinate convention for the runtime: right-handed, +Y up, -Z forward (same as OpenXR).
#pragma once

#include <cmath>
#include <cstring>

namespace gvr {

constexpr float kPi = 3.14159265358979323846f;

struct Vec2 { float x = 0, y = 0; };

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 1e-8f ? v * (1.0f / l) : Vec3{0, 0, 0};
}

struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    static Quat fromAxisAngle(const Vec3& axis, float radians) {
        Vec3 a = normalize(axis);
        float s = std::sin(radians * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
    }
    Quat operator*(const Quat& q) const {
        return {w * q.x + x * q.w + y * q.z - z * q.y,
                w * q.y - x * q.z + y * q.w + z * q.x,
                w * q.z + x * q.y - y * q.x + z * q.w,
                w * q.w - x * q.x - y * q.y - z * q.z};
    }
    Quat conjugate() const { return {-x, -y, -z, w}; }
    Vec3 rotate(const Vec3& v) const {
        // v' = q v q^-1, optimized form.
        Vec3 u{x, y, z};
        Vec3 t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }
};

inline Quat normalize(const Quat& q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-8f) return {};
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}

// Yaw (rotation about +Y) of a quaternion, in radians.
inline float yawOf(const Quat& q) {
    Vec3 fwd = q.rotate({0, 0, -1});
    return std::atan2(-fwd.x, -fwd.z);
}

struct Pose {
    Quat orientation;
    Vec3 position;
    // Compose: (this * other) applies `other` first, then this.
    Pose operator*(const Pose& o) const {
        return {orientation * o.orientation, position + orientation.rotate(o.position)};
    }
    Pose inverse() const {
        Quat ic = orientation.conjugate();
        return {ic, ic.rotate(-position)};
    }
    Vec3 transformPoint(const Vec3& p) const { return position + orientation.rotate(p); }
};

struct Mat4 {
    float m[16];  // column-major: m[col*4 + row]
    static Mat4 identity() {
        Mat4 r{};
        std::memset(r.m, 0, sizeof(r.m));
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
    Vec4 operator*(const Vec4& v) const {
        return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
                m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
                m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
                m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w};
    }
    static Mat4 translation(const Vec3& t) {
        Mat4 r = identity();
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }
    static Mat4 scale(const Vec3& s) {
        Mat4 r = identity();
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }
    static Mat4 rotation(const Quat& q) {
        Mat4 r = identity();
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        r.m[0] = 1 - 2 * (yy + zz); r.m[1] = 2 * (xy + wz);     r.m[2] = 2 * (xz - wy);
        r.m[4] = 2 * (xy - wz);     r.m[5] = 1 - 2 * (xx + zz); r.m[6] = 2 * (yz + wx);
        r.m[8] = 2 * (xz + wy);     r.m[9] = 2 * (yz - wx);     r.m[10] = 1 - 2 * (xx + yy);
        return r;
    }
    // Inverse of an affine matrix (3x3 linear part + translation; scale/mirror allowed).
    Mat4 affineInverse() const {
        const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
        const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        const float det = a * A + b * B + c * C;
        Mat4 r = identity();
        if (det == 0.0f) return r;
        const float id = 1.0f / det;
        r.m[0] = A * id;                 r.m[4] = -(b * i - c * h) * id; r.m[8] = (b * f - c * e) * id;
        r.m[1] = B * id;                 r.m[5] = (a * i - c * g) * id;  r.m[9] = -(a * f - c * d) * id;
        r.m[2] = C * id;                 r.m[6] = -(a * h - b * g) * id; r.m[10] = (a * e - b * d) * id;
        const float tx = m[12], ty = m[13], tz = m[14];
        r.m[12] = -(r.m[0] * tx + r.m[4] * ty + r.m[8] * tz);
        r.m[13] = -(r.m[1] * tx + r.m[5] * ty + r.m[9] * tz);
        r.m[14] = -(r.m[2] * tx + r.m[6] * ty + r.m[10] * tz);
        return r;
    }
    static Mat4 fromPose(const Pose& p) { return translation(p.position) * rotation(p.orientation); }
    static Mat4 trs(const Vec3& t, const Quat& q, const Vec3& s) {
        return translation(t) * rotation(q) * scale(s);
    }
    // Asymmetric-frustum projection from OpenXR-style field-of-view half angles (radians).
    // Produces an OpenGL clip space (z in [-1,1]); infinite far plane if farZ <= nearZ.
    static Mat4 projectionFov(float angleLeft, float angleRight, float angleUp, float angleDown,
                              float nearZ, float farZ) {
        const float tanL = std::tan(angleLeft), tanR = std::tan(angleRight);
        const float tanU = std::tan(angleUp), tanD = std::tan(angleDown);
        const float w = tanR - tanL, h = tanU - tanD;
        Mat4 r{};
        std::memset(r.m, 0, sizeof(r.m));
        r.m[0] = 2.0f / w;
        r.m[5] = 2.0f / h;
        r.m[8] = (tanR + tanL) / w;
        r.m[9] = (tanU + tanD) / h;
        r.m[11] = -1.0f;
        if (farZ <= nearZ) {
            r.m[10] = -1.0f;
            r.m[14] = -2.0f * nearZ;
        } else {
            r.m[10] = -(farZ + nearZ) / (farZ - nearZ);
            r.m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
        }
        return r;
    }
};

}  // namespace gvr
