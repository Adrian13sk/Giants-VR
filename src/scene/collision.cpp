#include "scene/collision.h"

#include <algorithm>
#include <cmath>

namespace gvr {

namespace {
constexpr float kWalkableCos = 0.64278f;  // ~50 deg slope limit (same gate the original uses)
}

Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

void CollisionWorld::addObject(const std::vector<Vec3>& tris) {
    Object o;
    o.mn = {1e30f, 1e30f, 1e30f};
    o.mx = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        const Vec3 &a = tris[i], &b = tris[i + 1], &c = tris[i + 2];
        const Vec3 n = cross(b - a, c - a);
        const float len = length(n);
        if (len < 1e-8f) continue;
        // Upward faces of either winding count as floors (single-sided roofs, wall tops).
        const bool floor = std::fabs(n.y / len) >= kWalkableCos;
        auto& dst = floor ? o.floor : o.wall;
        dst.push_back(a);
        dst.push_back(b);
        dst.push_back(c);
        for (const Vec3* v : {&a, &b, &c}) {
            o.mn = {std::min(o.mn.x, v->x), std::min(o.mn.y, v->y), std::min(o.mn.z, v->z)};
            o.mx = {std::max(o.mx.x, v->x), std::max(o.mx.y, v->y), std::max(o.mx.z, v->z)};
        }
    }
    if (!o.wall.empty() || !o.floor.empty()) objects_.push_back(std::move(o));
}

size_t CollisionWorld::triangleCount() const {
    size_t n = 0;
    for (const auto& o : objects_) n += (o.wall.size() + o.floor.size()) / 3;
    return n;
}

bool CollisionWorld::pushOut(Vec3& feet, float radius, float height, float stepUp) const {
    bool moved = false;
    const float y0 = feet.y + stepUp, y1 = feet.y + height;
    for (int iter = 0; iter < 3; ++iter) {
        bool any = false;
        for (const auto& o : objects_) {
            if (feet.x + radius < o.mn.x || feet.x - radius > o.mx.x || feet.z + radius < o.mn.z ||
                feet.z - radius > o.mx.z || y1 < o.mn.y || y0 > o.mx.y)
                continue;
            for (size_t i = 0; i + 2 < o.wall.size(); i += 3) {
                const Vec3 &a = o.wall[i], &b = o.wall[i + 1], &c = o.wall[i + 2];
                const float tmin = std::min({a.y, b.y, c.y}), tmax = std::max({a.y, b.y, c.y});
                if (tmax < y0 || tmin > y1) continue;
                // Sample the capsule axis at the height closest to the triangle's span.
                const float samples[3] = {std::max(y0, tmin), std::min(y1, tmax), 0.5f * (std::max(y0, tmin) + std::min(y1, tmax))};
                for (float sy : samples) {
                    const Vec3 p{feet.x, sy, feet.z};
                    const Vec3 q = closestPointOnTriangle(p, a, b, c);
                    if (length(p - q) >= radius) continue;  // 3D test (samples are clamped to the triangle's span)
                    float dx = p.x - q.x, dz = p.z - q.z;
                    float d = std::sqrt(dx * dx + dz * dz);
                    if (d < 1e-5f) {
                        // Axis on the wall: push along the wall normal (side the feet came from is unknown).
                        const Vec3 n = cross(b - a, c - a);
                        const float nl = std::sqrt(n.x * n.x + n.z * n.z);
                        if (nl < 1e-6f) continue;
                        dx = n.x / nl; dz = n.z / nl; d = 0;
                    } else {
                        dx /= d; dz /= d;
                    }
                    feet.x += dx * (radius - d);
                    feet.z += dz * (radius - d);
                    any = moved = true;
                    break;
                }
            }
        }
        if (!any) break;
    }
    return moved;
}

bool CollisionWorld::groundAt(float x, float z, float maxY, float& outY) const {
    bool found = false;
    float best = -1e30f;
    for (const auto& o : objects_) {
        if (x < o.mn.x || x > o.mx.x || z < o.mn.z || z > o.mx.z || o.mn.y > maxY) continue;
        for (size_t i = 0; i + 2 < o.floor.size(); i += 3) {
            const Vec3 &a = o.floor[i], &b = o.floor[i + 1], &c = o.floor[i + 2];
            // Barycentric in XZ.
            const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
            if (std::fabs(d) < 1e-9f) continue;
            const float l1 = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
            const float l2 = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
            const float l3 = 1.0f - l1 - l2;
            if (l1 < 0 || l2 < 0 || l3 < 0) continue;
            const float y = l1 * a.y + l2 * b.y + l3 * c.y;
            if (y <= maxY && y > best) { best = y; found = true; }
        }
    }
    if (found) outY = best;
    return found;
}

}  // namespace gvr
