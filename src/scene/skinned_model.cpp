#include "scene/skinned_model.h"

#include <algorithm>
#include <unordered_map>

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

    // Bind every GBS position to (bone, bone-local position). SKN objects map to GBS
    // max-objects in order; cluster vertex indices are relative to the object. Bones are
    // matched to animation nodes by name, per animation (clips have different node sets).
    bindings_.assign(gbs_.positions.size(), Binding{});
    if (skn.objects.size() != gbs_.maxObjs.size())
        LOGW("Skinned %s: %zu SKN objects vs %zu GBS objects", base.c_str(), skn.objects.size(),
             gbs_.maxObjs.size());
    std::unordered_map<std::string, int> boneIndex;
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
            const std::string name = toLower(bn->second);
            auto bi = boneIndex.find(name);
            if (bi == boneIndex.end()) {
                bi = boneIndex.emplace(name, int(bones_.size())).first;
                bones_.push_back(name);
            }
            for (const auto& v : c.verts) {
                const size_t pi = size_t(mo.vstart) + v.index;
                if (pi < bindings_.size()) bindings_[pi] = {bi->second, Vec3{v.x, v.y, v.z}};
            }
        }
    }
    if (addAnimation(fs, anim) != 0) return false;
    const Anim& a0 = anims_[0];
    bound_ = unbound_ = 0;
    for (const auto& b : bindings_) (b.bone >= 0 && a0.boneToNode[size_t(b.bone)] >= 0 ? bound_ : unbound_)++;

    bmin_ = {1e30f, 1e30f, 1e30f};
    bmax_ = {-1e30f, -1e30f, -1e30f};
    // Skinned meshes are lit dynamically: their RGB array is zero or holds skin data.
    for (auto& bp : buildGbsParts(gbs_, /*ignoreRgb=*/true)) {
        Part p;
        bool alpha = false;
        if (!bp.texture.empty()) p.texture = textures.texture(bp.texture, &alpha);
        if (alpha) p.blend = BlendMode::AlphaTest;
        p.glass = (bp.subFlags & GbsModel::kSubGlass) != 0;
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
    // Glass last, so what it covers (the face behind the visor) is drawn first.
    std::stable_partition(parts_.begin(), parts_.end(), [](const Part& q) { return !q.glass; });
    LOGI("Skinned %s + %s: %u/%zu positions bound, %zu nodes, %.2f s, %u unsupported tracks",
         base.c_str(), anim.c_str(), bound_, bindings_.size(), a0.anim.nodes.size(),
         a0.anim.durationSeconds(), a0.anim.unsupportedTracks);
    return bound_ > 0;
}

int SkinnedModel::addAnimation(const AssetFS& fs, const std::string& name) {
    std::vector<uint8_t> bytes;
    std::string err;
    AnmAnimation anim;
    if (!fs.read(name, bytes) || !parseAnm(bytes.data(), bytes.size(), anim, &err)) {
        LOGW("Skinned %s: ANM %s unavailable %s", base_.c_str(), name.c_str(), err.c_str());
        return -1;
    }
    return addAnimation(std::move(anim), name);
}

size_t SkinnedModel::boneMatches(const AnmAnimation& anim) const {
    size_t n = 0;
    for (const auto& b : bones_) n += anim.byName.count(b);
    return n;
}

int SkinnedModel::addAnimation(AnmAnimation&& anim, const std::string& name) {
    Anim a;
    a.anim = std::move(anim);
    a.boneToNode.assign(bones_.size(), -1);
    size_t missing = 0;
    for (size_t b = 0; b < bones_.size(); ++b) {
        auto n = a.anim.byName.find(bones_[b]);
        if (n != a.anim.byName.end()) a.boneToNode[b] = n->second;
        else ++missing;
    }
    if (missing) LOGW("Skinned %s: %s lacks %zu of %zu bones", base_.c_str(), name.c_str(), missing, bones_.size());
    a.children.assign(a.anim.nodes.size(), {});
    for (size_t n = 0; n < a.anim.nodes.size(); ++n) {
        const int p = a.anim.nodes[n].parent;
        if (p >= 0 && size_t(p) < a.anim.nodes.size()) a.children[size_t(p)].push_back(int(n));
    }
    anims_.push_back(std::move(a));
    return int(anims_.size()) - 1;
}

float SkinnedModel::animDuration(int i) const {
    return i >= 0 && size_t(i) < anims_.size() ? anims_[size_t(i)].anim.durationSeconds() : 0.0f;
}

void SkinnedModel::skin(int ai, float t, std::vector<Vec3>& out) {
    const Anim& a = anims_[size_t(std::max(0, std::min(ai, int(anims_.size()) - 1)))];
    a.anim.evaluate(t, world_);
    if (modifier_) modifier_(a.anim, a.children, world_);
    out.resize(gbs_.positions.size());
    for (size_t i = 0; i < gbs_.positions.size(); ++i) {
        const Binding& b = bindings_[i];
        const int node = b.bone >= 0 ? a.boneToNode[size_t(b.bone)] : -1;
        if (node >= 0) {
            const Vec4 w = world_[size_t(node)] * Vec4{b.local.x, b.local.y, b.local.z, 1.0f};
            out[i] = giantsToRuntime(w.x, w.y, w.z);
        } else {
            const auto& p = gbs_.positions[i];
            out[i] = giantsToRuntime(p[0], p[1], p[2]);
        }
    }
}

void SkinnedModel::poseBlend(Renderer& r, int a, float ta, int b, float tb, float wb) {
    if (anims_.empty()) return;
    skin(a, ta, posed_);
    if (wb < 0.5f) { domWorld_ = world_; domClip_ = a; }
    if (wb > 0.0f) {
        skin(b, tb, posedB_);
        if (wb >= 0.5f) { domWorld_ = world_; domClip_ = b; }
        const float wa = 1.0f - wb;
        for (size_t i = 0; i < posed_.size(); ++i) posed_[i] = posed_[i] * wa + posedB_[i] * wb;
    }
    if (!hideMask_.empty()) {
        // Collapse hidden vertices onto their centroid: their triangles become degenerate.
        Vec3 c{0, 0, 0};
        size_t n = 0;
        for (size_t i = 0; i < posed_.size(); ++i)
            if (hideMask_[i]) { c += posed_[i]; ++n; }
        if (n) {
            c = c * (1.0f / float(n));
            hiddenCentroid_ = c;
            if (hideEnabled_)
                for (size_t i = 0; i < posed_.size(); ++i)
                    if (hideMask_[i]) posed_[i] = c;
        }
    }
    for (Part& p : parts_) {
        std::vector<Vec3> acc(p.verts.size(), Vec3{0, 0, 0});
        for (size_t v = 0; v < p.verts.size(); ++v) {
            const Vec3& q = posed_[p.posIndex[v]];
            p.verts[v].pos[0] = q.x; p.verts[v].pos[1] = q.y; p.verts[v].pos[2] = q.z;
        }
        for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
            const float* pa = p.verts[p.indices[i]].pos;
            const float* pb = p.verts[p.indices[i + 1]].pos;
            const float* pc = p.verts[p.indices[i + 2]].pos;
            Vec3 n = cross(Vec3{pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]}, Vec3{pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]});
            for (int k = 0; k < 3; ++k) acc[p.indices[i + k]] += n;
        }
        for (size_t v = 0; v < p.verts.size(); ++v) {
            Vec3 n = normalize(acc[v]);
            p.verts[v].normal[0] = n.x; p.verts[v].normal[1] = n.y; p.verts[v].normal[2] = n.z;
        }
        r.updateMeshVertices(p.mesh, p.verts);
    }
}

void SkinnedModel::setHiddenBones(const std::vector<std::string>& names) {
    hideMask_.assign(bindings_.size(), 0);
    for (size_t i = 0; i < bindings_.size(); ++i) {
        const int b = bindings_[i].bone;
        if (b < 0) continue;
        for (const auto& n : names)
            if (bones_[size_t(b)] == n) hideMask_[i] = 1;
    }
}

bool SkinnedModel::nodeWorld(const std::string& name, Mat4& out) const {
    if (domClip_ < 0 || size_t(domClip_) >= anims_.size()) return false;
    const auto& by = anims_[size_t(domClip_)].anim.byName;
    auto it = by.find(name);
    if (it == by.end() || size_t(it->second) >= domWorld_.size()) return false;
    out = domWorld_[size_t(it->second)];
    return true;
}

bool SkinnedModel::boneVertexBounds(const std::string& name, Vec3& mn, Vec3& mx) const {
    mn = {1e30f, 1e30f, 1e30f};
    mx = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (size_t i = 0; i < bindings_.size() && i < posed_.size(); ++i) {
        const int b = bindings_[i].bone;
        if (b < 0 || bones_[size_t(b)] != name) continue;
        const Vec3& p = posed_[i];
        mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
        mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
        any = true;
    }
    return any;
}

void SkinnedModel::render(Renderer& r, const Mat4& world) const {
    for (const Part& p : parts_) {
        DrawParams dp;
        dp.texture = p.texture;
        dp.blend = p.blend;
        dp.unlit = !p.lit;
        if (p.glass) {  // tinted see-through glass
            dp.blend = BlendMode::AlphaBlend;
            dp.tint = {1, 1, 1, 0.3f};
        }
        r.draw(p.mesh, world, dp);
    }
}

void SkinnedModel::renderShadow(Renderer& r, const Mat4& world) const {
    for (const Part& p : parts_)
        if (!p.glass) r.drawShadow(p.mesh, world);
}

void SkinnedModel::renderGlow(Renderer& r, const Mat4& world, float brightness) const {
    for (const Part& p : parts_) {
        DrawParams dp;
        dp.texture = p.texture;
        dp.blend = p.blend;
        dp.unlit = true;
        dp.tint = {brightness, brightness, brightness, 1.0f};
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
