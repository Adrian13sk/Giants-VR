#include "render/quad_batch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gvr {

bool QuadBatch::init(Renderer& r, size_t capacity) {
    capacity_ = capacity;
    MeshData m;
    m.vertices.resize(capacity * 4);
    for (auto& v : m.vertices) std::memset(&v, 0, sizeof v);
    for (size_t q = 0; q < capacity; ++q) {
        const uint32_t b = uint32_t(q * 4);
        m.indices.insert(m.indices.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
    verts_ = m.vertices;
    mesh_ = r.uploadMesh(m);
    return mesh_ != 0;
}

void QuadBatch::add(const Vec3 c[4], const uint8_t rgba[4]) {
    static const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    add(c, uv, rgba);
}

void QuadBatch::add(const Vec3 c[4], const float uv[4][2], const uint8_t rgba[4]) {
    add(c, uv, rgba, normalize(cross(c[1] - c[0], c[2] - c[0])));
}

void QuadBatch::add(const Vec3 c[4], const float uv[4][2], const uint8_t rgba[4], const Vec3& n) {
    if (count_ >= capacity_) return;
    for (int k = 0; k < 4; ++k) {
        Vertex& v = verts_[count_ * 4 + size_t(k)];
        v.pos[0] = c[k].x; v.pos[1] = c[k].y; v.pos[2] = c[k].z;
        v.normal[0] = n.x; v.normal[1] = n.y; v.normal[2] = n.z;
        v.uv[0] = uv[k][0]; v.uv[1] = uv[k][1];
        for (int i = 0; i < 4; ++i) v.color[i] = rgba[i];
    }
    ++count_;
}

void QuadBatch::addVertices(const Vertex v[4]) {
    if (count_ >= capacity_) return;
    for (int k = 0; k < 4; ++k) verts_[count_ * 4 + size_t(k)] = v[k];
    ++count_;
}

void QuadBatch::addBillboard(const Vec3& c, float size, const Vec3& right, const Vec3& up, float angle, const uint8_t rgba[4]) {
    const float h = 0.5f * size, ca = std::cos(angle), sa = std::sin(angle);
    const Vec3 r = (right * ca + up * sa) * h, u = (up * ca - right * sa) * h;
    const Vec3 q[4] = {c - r - u, c + r - u, c + r + u, c - r + u};
    add(q, rgba);
}

void QuadBatch::upload(Renderer& r) {
    if (!mesh_) return;
    // Quads used last frame but not now collapse to a point.
    for (size_t q = count_; q < uploaded_; ++q)
        for (int k = 0; k < 4; ++k) std::memset(verts_[q * 4 + size_t(k)].pos, 0, sizeof(float) * 3);
    const size_t n = std::max(count_, uploaded_);
    if (n > 0) r.updateMeshVertexRange(mesh_, 0, verts_.data(), n * 4);
    uploaded_ = count_;
}

void QuadBatch::draw(Renderer& r, const DrawParams& dp) const {
    if (mesh_ && uploaded_ > 0) r.draw(mesh_, Mat4::identity(), dp);
}

}  // namespace gvr
