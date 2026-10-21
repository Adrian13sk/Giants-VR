#include "scene/skinned_model.h"

#include <algorithm>

#include "core/log.h"

namespace gvr {

bool SkinnedModel::load(const AssetFS& fs, ModelCache& textures, Renderer& r, const std::string& base,
                        const std::string& anim) {
    base_ = base;
    std::vector<uint8_t> bytes;
    std::string err;
    if (!fs.read(base + ".gbs", bytes) || !parseGbs(bytes.data(), bytes.size(), gbs_, &err)) {
        LOGW("Skinned %s: GBS unavailable %s", base.c_str(), err.c_str());
        return false;
    }
    SknModel skn;
    if (!fs.read(base + ".skn", bytes) || !parseSkn(bytes.data(), bytes.size(), skn, &err)) {
        LOGW("Skinned %s: SKN unavailable %s", base.c_str(), err.c_str());
        return false;
    }
    if (!fs.read(anim, bytes) || !parseAnm(bytes.data(), bytes.size(), anim_, &err)) {
        LOGW("Skinned %s: ANM %s unavailable %s", base.c_str(), anim.c_str(), err.c_str());
        return false;
    }

    // Bind every GBS position to (animation node, bone-local position). SKN objects map to
    // GBS max-objects in order; cluster vertex indices are relative to the object.
    bindings_.assign(gbs_.positions.size(), Binding{});
    if (skn.objects.size() != gbs_.maxObjs.size())
        LOGW("Skinned %s: %zu SKN objects vs %zu GBS objects", base.c_str(), skn.objects.size(),
             gbs_.maxObjs.size());
    const size_t nObj = std::min(skn.objects.size(), gbs_.maxObjs.size());
    for (size_t o = 0; o < nObj; ++o) {
        const SknObject& so = skn.objects[o];
        const GbsMaxObj& mo = gbs_.maxObjs[o];
        if (int32_t(so.vertexCount) != mo.vcount) {
            LOGW("Skinned %s: object %zu vertex count mismatch (%u vs %d)", base.c_str(), o,
                 so.vertexCount, mo.vcount);
            continue;
        }
        for (const SknCluster& c : so.clusters) {
            auto bn = skn.boneNames.find(c.bone);
            if (bn == skn.boneNames.end()) continue;
            auto node = anim_.byName.find(toLower(bn->second));
            if (node == anim_.byName.end()) continue;
            for (const auto& v : c.verts) {
                const size_t pi = size_t(mo.vstart) + v.index;
                if (pi < bindings_.size()) bindings_[pi] = {node->second, Vec3{v.x, v.y, v.z}};
            }
        }
    }
    bound_ = unbound_ = 0;
    for (const auto& b : bindings_) (b.node >= 0 ? bound_ : unbound_)++;

    bmin_ = {1e30f, 1e30f, 1e30f};
    bmax_ = {-1e30f, -1e30f, -1e30f};
    for (auto& bp : buildGbsParts(gbs_)) {
        Part p;
        bool alpha = false;
        if (!bp.texture.empty()) p.texture = textures.texture(bp.texture, &alpha);
        if (alpha) p.blend = BlendMode::AlphaTest;
        p.lit = bp.lit;
        for (const auto& v : bp.mesh.vertices) {
            bmin_ = {std::min(bmin_.x, v.pos[0]), std::min(bmin_.y, v.pos[1]), std::min(bmin_.z, v.pos[2])};
            bmax_ = {std::max(bmax_.x, v.pos[0]), std::max(bmax_.y, v.pos[1]), std::max(bmax_.z, v.pos[2])};
        }
        p.verts = bp.mesh.vertices;
        p.indices = bp.mesh.indices;
        p.posIndex = bp.posIndex;
        p.mesh = r.uploadMesh(bp.mesh);
        parts_.push_back(std::move(p));
    }
    LOGI("Skinned %s + %s: %u/%zu positions bound, %zu nodes, %.2f s, %u unsupported tracks",
         base.c_str(), anim.c_str(), bound_, bindings_.size(), anim_.nodes.size(),
         anim_.durationSeconds(), anim_.unsupportedTracks);
    return bound_ > 0;
}

void SkinnedModel::pose(Renderer& r, float seconds) {
    anim_.evaluate(seconds, world_);
    posed_.resize(gbs_.positions.size());
    for (size_t i = 0; i < gbs_.positions.size(); ++i) {
        const Binding& b = bindings_[i];
        if (b.node >= 0) {
            Vec4 w = world_[size_t(b.node)] * Vec4{b.local.x, b.local.y, b.local.z, 1.0f};
            posed_[i] = giantsToRuntime(w.x, w.y, w.z);
        } else {
            const auto& p = gbs_.positions[i];
            posed_[i] = giantsToRuntime(p[0], p[1], p[2]);
        }
    }
    for (Part& p : parts_) {
        std::vector<Vec3> acc(p.verts.size(), Vec3{0, 0, 0});
        for (size_t v = 0; v < p.verts.size(); ++v) {
            const Vec3& q = posed_[p.posIndex[v]];
            p.verts[v].pos[0] = q.x; p.verts[v].pos[1] = q.y; p.verts[v].pos[2] = q.z;
        }
        for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
            const float* a = p.verts[p.indices[i]].pos;
            const float* b = p.verts[p.indices[i + 1]].pos;
            const float* c = p.verts[p.indices[i + 2]].pos;
            Vec3 n = cross(Vec3{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, Vec3{c[0] - a[0], c[1] - a[1], c[2] - a[2]});
            for (int k = 0; k < 3; ++k) acc[p.indices[i + k]] += n;
        }
        for (size_t v = 0; v < p.verts.size(); ++v) {
            Vec3 n = normalize(acc[v]);
            p.verts[v].normal[0] = n.x; p.verts[v].normal[1] = n.y; p.verts[v].normal[2] = n.z;
        }
        r.updateMeshVertices(p.mesh, p.verts);
    }
}

void SkinnedModel::render(Renderer& r, const Mat4& world) const {
    for (const Part& p : parts_) {
        DrawParams dp;
        dp.texture = p.texture;
        dp.blend = p.blend;
        dp.unlit = !p.lit;
        r.draw(p.mesh, world, dp);
    }
}

}  // namespace gvr

namespace gvr {
void SkinnedModel::posedBounds(Vec3& mn, Vec3& mx) const {
    mn = {1e30f, 1e30f, 1e30f};
    mx = {-1e30f, -1e30f, -1e30f};
    for (const Vec3& p : posed_) {
        mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
        mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    }
}
}  // namespace gvr
