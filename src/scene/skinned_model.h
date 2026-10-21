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
    // Loads <base>.gbs + <base>.skn from the FS and the given animation (.anm).
    bool load(const AssetFS& fs, ModelCache& textures, Renderer& r, const std::string& base,
              const std::string& anim);
    // Evaluates the animation at `seconds` and re-uploads the skinned vertices.
    void pose(Renderer& r, float seconds);
    void render(Renderer& r, const Mat4& world) const;
    const std::string& name() const { return base_; }
    uint32_t boundVertices() const { return bound_; }
    uint32_t unboundVertices() const { return unbound_; }
    float animDuration() const { return anim_.durationSeconds(); }
    Vec3 restMin() const { return bmin_; }
    Vec3 restMax() const { return bmax_; }
    void posedBounds(Vec3& mn, Vec3& mx) const;

private:
    struct Binding { int node = -1; Vec3 local; };
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
    AnmAnimation anim_;
    std::vector<Binding> bindings_;  // per GBS position
    std::vector<Part> parts_;
    std::vector<Mat4> world_;
    std::vector<Vec3> posed_;
    uint32_t bound_ = 0, unbound_ = 0;
    Vec3 bmin_, bmax_;
};

}  // namespace gvr
