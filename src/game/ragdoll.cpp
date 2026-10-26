#include "game/ragdoll.h"

#include <algorithm>
#include <cmath>

namespace gvr {

Mat4 affineInverse(const Mat4& m);  // game_app.cpp

namespace {

Vec3 xform(const Mat4& m, const Vec3& p) {
    const Vec4 v = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {v.x, v.y, v.z};
}
Vec3 xdir(const Mat4& m, const Vec3& d) {
    const Vec4 v = m * Vec4{d.x, d.y, d.z, 0.0f};
    return {v.x, v.y, v.z};
}
Vec3 trans(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }

// Orthonormal frame from a primary and a secondary direction (columns e1, e2, e3).
bool frame(const Vec3& a, const Vec3& b, Vec3 out[3]) {
    const float la = length(a);
    if (la < 1e-6f) return false;
    out[0] = a * (1.0f / la);
    Vec3 p = b - out[0] * dot(b, out[0]);
    const float lp = length(p);
    if (lp < 1e-3f * std::max(1e-6f, length(b))) return false;
    out[1] = p * (1.0f / lp);
    out[2] = cross(out[0], out[1]);
    return true;
}

// Rotation (as a Mat4) taking frame f0 to frame f1: R = F1 * F0^T.
Mat4 frameRot(const Vec3 f0[3], const Vec3 f1[3]) {
    Mat4 r = Mat4::identity();
    for (int c = 0; c < 3; ++c)
        for (int rr = 0; rr < 3; ++rr) {
            const float* a0 = &f0[0].x;
            (void)a0;
            float s = 0;
            for (int k = 0; k < 3; ++k) {
                const float v1 = rr == 0 ? f1[k].x : (rr == 1 ? f1[k].y : f1[k].z);
                const float v0 = c == 0 ? f0[k].x : (c == 1 ? f0[k].y : f0[k].z);
                s += v1 * v0;
            }
            r.m[c * 4 + rr] = s;
        }
    return r;
}

// Shortest-arc rotation taking direction a to direction b.
Mat4 arcRot(const Vec3& a, const Vec3& b) {
    const Vec3 u = normalize(a), v = normalize(b);
    const float c = dot(u, v);
    Vec3 ax = cross(u, v);
    const float s = length(ax);
    if (s < 1e-6f) {
        if (c > 0) return Mat4::identity();
        Vec3 o = std::fabs(u.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        ax = normalize(cross(u, o));
        return Mat4::rotation(Quat::fromAxisAngle(ax, 3.14159265f));
    }
    return Mat4::rotation(Quat::fromAxisAngle(ax * (1.0f / s), std::atan2(s, c)));
}

}  // namespace

void Ragdoll::start(const std::vector<Mat4>& world, const std::vector<int>& parent, const Mat4& toWorld, const Vec3& vel,
                    const Vec3& spin, float minBone) {
    const size_t n = world.size();
    rest_ = world;
    restInv_.resize(n);
    for (size_t i = 0; i < n; ++i) restInv_[i] = affineInverse(world[i]);
    owner_.assign(n, -1);
    pnode_.clear();
    pparent_.clear();
    // Parents come before children in the node tables; walk in order (fall back to repeated passes).
    std::vector<char> done(n, 0);
    for (int pass = 0; pass < 8; ++pass) {
        bool all = true;
        for (size_t i = 0; i < n; ++i) {
            if (done[i]) continue;
            const int p = i < parent.size() ? parent[i] : -1;
            if (p >= 0 && size_t(p) < n && !done[size_t(p)]) { all = false; continue; }
            int anc = p >= 0 && size_t(p) < n ? owner_[size_t(p)] : -1;
            const bool own = anc < 0 || length(trans(world[i]) - trans(world[size_t(pnode_[size_t(anc)])])) >= minBone;
            if (own) {
                pnode_.push_back(int(i));
                pparent_.push_back(anc);
                owner_[i] = int(pnode_.size()) - 1;
            } else {
                owner_[i] = anc;
            }
            done[i] = 1;
        }
        if (all) break;
    }
    const size_t m = pnode_.size();
    // A root particle whose children all sit far away (a mover node at the feet) still works: it is
    // simply one more point of the body.
    pchild_.assign(m, -1);
    std::vector<float> best(m, 0.0f);
    for (size_t k = 0; k < m; ++k) {
        const int p = pparent_[k];
        if (p < 0) continue;
        const float d = length(trans(world[size_t(pnode_[k])]) - trans(world[size_t(pnode_[size_t(p)])]));
        if (d > best[size_t(p)]) { best[size_t(p)] = d; pchild_[size_t(p)] = int(k); }
    }
    x_.resize(m);
    xp_.resize(m);
    for (size_t k = 0; k < m; ++k) x_[k] = xform(toWorld, trans(world[size_t(pnode_[k])]));
    Vec3 c = centre();
    const float h = 1.0f / 60.0f;
    for (size_t k = 0; k < m; ++k) xp_[k] = x_[k] - (vel + cross(spin, x_[k] - c)) * h;
    sticks_.clear();
    auto add = [&](int a, int b, float k) {
        if (a < 0 || b < 0 || a == b) return;
        sticks_.push_back({a, b, length(x_[size_t(a)] - x_[size_t(b)]), k});
    };
    for (size_t k = 0; k < m; ++k) {
        const int p = pparent_[k];
        if (p < 0) continue;
        add(int(k), p, 1.0f);
        const int g = pparent_[size_t(p)];
        if (g >= 0) add(int(k), g, -0.6f);  // joints don't fold flat (only a minimum distance)
    }
    for (size_t a = 0; a < m; ++a)  // siblings held together: the hips, the shoulders
        for (size_t b = a + 1; b < m; ++b)
            if (pparent_[a] >= 0 && pparent_[a] == pparent_[b]) add(int(a), int(b), 1.0f);
    float bestD = 1e30f;
    middle_ = 0;
    for (size_t k = 0; k < m; ++k) {
        const float d = length(x_[k] - c);
        if (d < bestD) { bestD = d; middle_ = int(k); }
    }
    active = m > 0;
    asleep = false;
    age = still_ = acc_ = 0;
    pinned_ = -1;
}

Vec3 Ragdoll::centre() const {
    Vec3 c{0, 0, 0};
    for (const auto& p : x_) c += p;
    return x_.empty() ? c : c * (1.0f / float(x_.size()));
}

float Ragdoll::lowest() const {
    float y = 1e30f;
    for (const auto& p : x_) y = std::min(y, p.y);
    return y;
}

void Ragdoll::pin(const Vec3& at) {
    if (!active) return;
    pinned_ = middle_;
    pinAt_ = at;
    asleep = false;
    still_ = 0;
}

void Ragdoll::kick(const Vec3& dv) {
    if (!active) return;
    for (size_t k = 0; k < x_.size(); ++k) xp_[k] = xp_[k] - dv * (1.0f / 60.0f);
    asleep = false;
    still_ = 0;
}

void Ragdoll::step(float dt, const std::function<float(float, float)>& ground) {
    if (!active || asleep) return;
    const float h = 1.0f / 60.0f;
    acc_ = std::min(acc_ + dt, 4 * h);
    const float r = 0.07f;  // particle radius (m)
    while (acc_ >= h) {
        acc_ -= h;
        age += h;
        float moved = 0;
        for (size_t k = 0; k < x_.size(); ++k) {
            const Vec3 v = (x_[k] - xp_[k]) * 0.995f;
            xp_[k] = x_[k];
            x_[k] = x_[k] + v + Vec3{0, -9.8f * h * h, 0};
        }
        for (int it = 0; it < 8; ++it) {
            for (const Stick& s : sticks_) {
                Vec3& a = x_[size_t(s.a)];
                Vec3& b = x_[size_t(s.b)];
                const Vec3 d = b - a;
                const float l = length(d);
                if (l < 1e-6f) continue;
                if (s.k < 0 && l > -s.k * s.rest) continue;  // a minimum only
                const float want = s.k < 0 ? -s.k * s.rest : s.rest;
                const Vec3 corr = d * ((l - want) / l * 0.5f * (s.k < 0 ? 1.0f : s.k));
                const bool pa = s.a == pinned_, pb = s.b == pinned_;
                if (pa && !pb) b = b - corr * 2.0f;
                else if (pb && !pa) a = a + corr * 2.0f;
                else { a = a + corr; b = b - corr; }
            }
            if (pinned_ >= 0) x_[size_t(pinned_)] = pinAt_;
            if (ground)
                for (size_t k = 0; k < x_.size(); ++k) {
                    const float g = ground(x_[k].x, x_[k].z) + r;
                    if (x_[k].y < g) {
                        if (it == 0) impact = std::max(impact, (xp_[k].y - x_[k].y) / h);  // (the speed it came down at)
                        x_[k].y = g;
                        // Friction: most of the sliding stops on contact.
                        xp_[k].x += (x_[k].x - xp_[k].x) * 0.6f;
                        xp_[k].z += (x_[k].z - xp_[k].z) * 0.6f;
                        if (xp_[k].y < x_[k].y - 0.3f) xp_[k].y = x_[k].y - 0.3f;
                    }
                }
        }
        for (size_t k = 0; k < x_.size(); ++k) moved = std::max(moved, length(x_[k] - xp_[k]));
        still_ = moved < 0.004f ? still_ + h : 0.0f;
        if (pinned_ < 0 && ((age > 1.0f && still_ > 0.5f) || age > 10.0f)) { asleep = true; break; }
    }
}

void Ragdoll::apply(std::vector<Mat4>& world, const Mat4& toWorld) const {
    if (!active || world.size() != rest_.size()) return;
    const Mat4 inv = affineInverse(toWorld);
    const size_t m = pnode_.size();
    std::vector<Vec3> pm(m);
    for (size_t k = 0; k < m; ++k) pm[k] = xform(inv, x_[k]);
    std::vector<Mat4> rot(m, Mat4::identity());
    std::vector<char> have(m, 0);
    for (size_t k = 0; k < m; ++k) {  // parents first (pnode_ order follows the tree)
        const int c = pchild_[k], p = pparent_[k];
        const Vec3 o0 = trans(rest_[size_t(pnode_[k])]);
        if (c >= 0) {
            const Vec3 a0 = trans(rest_[size_t(pnode_[size_t(c)])]) - o0, a1 = pm[size_t(c)] - pm[k];
            Vec3 f0[3], f1[3];
            bool ok = false;
            if (p >= 0) {
                const Vec3 b0 = trans(rest_[size_t(pnode_[size_t(p)])]) - o0, b1 = pm[size_t(p)] - pm[k];
                ok = frame(a0, b0, f0) && frame(a1, b1, f1);
            } else {
                // The root: its two farthest children span it.
                int c2 = -1;
                float bd = 0;
                for (size_t j = 0; j < m; ++j)
                    if (pparent_[j] == int(k) && int(j) != c) {
                        const float d = length(trans(rest_[size_t(pnode_[j])]) - o0);
                        if (d > bd) { bd = d; c2 = int(j); }
                    }
                if (c2 >= 0) {
                    const Vec3 b0 = trans(rest_[size_t(pnode_[size_t(c2)])]) - o0, b1 = pm[size_t(c2)] - pm[k];
                    ok = frame(a0, b0, f0) && frame(a1, b1, f1);
                }
            }
            rot[k] = ok ? frameRot(f0, f1) : arcRot(a0, a1);
            have[k] = 1;
        } else if (p >= 0 && have[size_t(p)]) {
            rot[k] = rot[size_t(p)];  // an end (hand, head): turns with its limb
            have[k] = 1;
        }
    }
    std::vector<Mat4> delta(m);
    for (size_t k = 0; k < m; ++k) {
        const Mat4& r0 = rest_[size_t(pnode_[k])];
        Mat4 nw = rot[k] * r0;
        nw.m[12] = pm[k].x; nw.m[13] = pm[k].y; nw.m[14] = pm[k].z;
        delta[k] = nw * restInv_[size_t(pnode_[k])];
    }
    for (size_t i = 0; i < world.size(); ++i) {
        const int o = owner_[i];
        if (o >= 0) world[i] = delta[size_t(o)] * rest_[i];
    }
    (void)xdir;
}

}  // namespace gvr
