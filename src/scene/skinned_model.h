// Original character: GBS mesh + SKN rigid skin + ANM animation, CPU-skinned each frame.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "assets/gbs.h"
#include "assets/gzp.h"
#include "assets/skin_anim.h"
#include "render/renderer.h"
#include "scene/model_cache.h"

namespace gvr {

class SkinnedModel {
public:
    // Loads <base>.gbs + <base>.skn from the FS and the given animation (.anm) as animation 0.
    bool load(const AssetFS& fs, ModelCache& textures, Renderer& r, const std::string& base,
              const std::string& anim);
    // Loads another animation for the same skin; returns its index or -1.
    int addAnimation(const AssetFS& fs, const std::string& anim);
    // Evaluates animation 0 at `seconds` and re-uploads the skinned vertices.
    void pose(Renderer& r, float seconds) { poseBlend(r, 0, seconds, 0, seconds, 0.0f); }
    // Skins with animation a at time ta and animation b at time tb and blends the resulting
    // vertex positions: (1-wb)*A + wb*B (cross-fades between clips without popping).
    void poseBlend(Renderer& r, int a, float ta, int b, float tb, float wb);
    float animDuration(int i) const;
    int animationCount() const { return int(anims_.size()); }
    void render(Renderer& r, const Mat4& world) const;
    const std::string& name() const { return base_; }
    uint32_t boundVertices() const { return bound_; }
    uint32_t unboundVertices() const { return unbound_; }
    float animDuration() const { return animDuration(0); }
    Vec3 restMin() const { return bmin_; }
    Vec3 restMax() const { return bmax_; }
    void posedBounds(Vec3& mn, Vec3& mx) const;

private:
    struct Binding { int bone = -1; Vec3 local; };  // bone: index into bones_
    struct Anim {
        AnmAnimation anim;
        std::vector<int> boneToNode;  // per bones_ entry, node index or -1
    };
    void skin(int anim, float t, std::vector<Vec3>& out);
    struct Part {
        MeshId mesh = 0;
        TextureId texture = 0;
        BlendMode blend = BlendMode::Opaque;
        bool lit = true;
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
        std::vector<uint32_t> posIndex;
    };
    std::string base_;
    GbsModel gbs_;
    std::vector<std::string> bones_;  // lower-case bone names used by the skin
    std::vector<Anim> anims_;
    std::vector<Binding> bindings_;  // per GBS position
    std::vector<Vec3> posedB_;
    std::vector<Part> parts_;
    std::vector<Mat4> world_;
    std::vector<Vec3> posed_;
    uint32_t bound_ = 0, unbound_ = 0;
    Vec3 bmin_, bmax_;
};

}  // namespace gvr
