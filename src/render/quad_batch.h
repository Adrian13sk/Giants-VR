// Many textured quads in one draw call: particles, blood splats and drops. The quads are built
// on the CPU each frame into a fixed-capacity mesh (unused quads collapse to nothing), so
// hundreds of billboards cost one draw per texture instead of one each.
#pragma once

#include <vector>

#include "render/renderer.h"

namespace gvr {

class QuadBatch {
public:
    bool init(Renderer& r, size_t capacity);
    void clear() { count_ = 0; }
    bool full() const { return count_ >= capacity_; }
    // Corners in order (counter-clockwise seen from the front); uv (0,0)-(1,1) over the texture.
    void add(const Vec3 corners[4], const uint8_t rgba[4]);
    // Same with explicit texture coordinates per corner.
    void add(const Vec3 corners[4], const float uv[4][2], const uint8_t rgba[4]);
    // Same with a given normal (e.g. straight up, so a card is lit like the ground under it).
    void add(const Vec3 corners[4], const float uv[4][2], const uint8_t rgba[4], const Vec3& normal);
    // A camera-facing square (right/up: the viewer's axes), rolled by `angle`.
    void addBillboard(const Vec3& centre, float size, const Vec3& right, const Vec3& up, float angle, const uint8_t rgba[4]);
    // Uploads this frame's quads (once per frame, before drawing).
    void upload(Renderer& r);
    void draw(Renderer& r, const DrawParams& dp) const;
    size_t count() const { return count_; }

private:
    MeshId mesh_ = 0;
    size_t capacity_ = 0, count_ = 0, uploaded_ = 0;
    std::vector<Vertex> verts_;
};

}  // namespace gvr
