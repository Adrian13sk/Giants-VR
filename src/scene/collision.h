// Static world collision for characters (renderer-independent, unit-tested).
//
// Placed level objects contribute their opaque triangles in world space. Characters are
// vertical capsules: steep triangles ("walls") push them out horizontally, walkable triangles
// ("floors": walls' tops, roofs, tables) act as ground they can stand on or step onto.
#pragma once

#include <vector>

#include "core/math.h"

namespace gvr {

class CollisionWorld {
public:
    // Adds one object's triangles (world space, 3 vertices per triangle). Returns its index (for
    // setEnabled) or -1 if it had no usable triangles. `tag`: caller's id, reported by raycast.
    int addObject(const std::vector<Vec3>& worldTris, int tag = -1);
    // Disabled objects (destroyed buildings, fallen trees) are ignored by every query.
    void setEnabled(int index, bool on) { if (index >= 0 && size_t(index) < objects_.size()) objects_[size_t(index)].enabled = on; }
    void clear() { objects_.clear(); }
    size_t objectCount() const { return objects_.size(); }
    size_t triangleCount() const;

    // Pushes a capsule (feet position, radius, height) out of steep geometry. Geometry below
    // feet.y + stepUp is ignored (it is stepped onto instead). Returns true if moved.
    bool pushOut(Vec3& feet, float radius, float height, float stepUp) const;
    // Highest walkable surface under (x, z) whose height is <= maxY. False if none.
    bool groundAt(float x, float z, float maxY, float& outY) const;
    // First hit along origin + dir * t, t in [0, maxDist] (dir normalised). Both triangle sides.
    bool raycast(const Vec3& origin, const Vec3& dir, float maxDist, float& tHit, Vec3& normal, int* tag = nullptr) const;

private:
    struct Object {
        Vec3 mn, mx;
        std::vector<Vec3> wall;   // steep triangles
        std::vector<Vec3> floor;  // walkable triangles
        int tag = -1;
        bool enabled = true;
    };
    std::vector<Object> objects_;
};

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c);

}  // namespace gvr
