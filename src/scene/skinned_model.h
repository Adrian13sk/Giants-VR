// Original character: GBS mesh + SKN rigid skin + ANM animation, CPU-skinned each frame.
#pragma once

#include <functional>
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
    // Adds an already decoded animation (e.g. one actor's tree from a cutscene director).
    int addAnimation(AnmAnimation&& anim, const std::string& name);
    // How many of the skin's bones `anim` has nodes for (picks an actor's tree in a cutscene).
    size_t boneMatches(const AnmAnimation& anim) const;
    size_t boneCount() const { return bones_.size(); }
    // Evaluates animation 0 at `seconds` and re-uploads the skinned vertices.
    void pose(Renderer& r, float seconds) { poseBlend(r, 0, seconds, 0, seconds, 0.0f); }
    // Skins with animation a at time ta and animation b at time tb and blends the resulting
    // vertex positions: (1-wb)*A + wb*B (cross-fades between clips without popping).
    void poseBlend(Renderer& r, int a, float ta, int b, float tb, float wb);
    float animDuration(int i) const;
    int animationCount() const { return int(anims_.size()); }

    // Called with every evaluated pose (bone world matrices, model space, game units) before
    // skinning, e.g. arm IK. children[i] lists the child nodes of node i of that clip.
    using PoseModifier = std::function<void(const AnmAnimation& clip, const std::vector<std::vector<int>>& children,
                                            std::vector<Mat4>& world)>;
    void setPoseModifier(PoseModifier m) { modifier_ = std::move(m); }
    // Vertices bound to these bones (and their descendants' bones) collapse to a point when
    // hiding is enabled (first-person: hide the head the camera sits inside).
    void setHiddenBones(const std::vector<std::string>& lowerCaseNames);
    // The bones on a head (eyes, lips, hair, jaw, teeth, tongue, brows): hidden with the head in
    // first person, or an idle clip swings them into the eyes (Delphi's face, Kabuto's teeth).
    static std::vector<std::string> headBones() {
        std::vector<std::string> v = {"head", "neck", "l_eye01", "r_eye01", "lip_bottom", "lip_top", "jaw bottom", "jaw top", "l_eyebrow",
                                      "r_eyebrow", "teeth bottom", "teeth top", "tongue1", "tongue2", "tongue3", "hair_back01", "hair_back02",
                                      "hair_back03", "hair_back04"};
        for (int k = 1; k <= 5; ++k) {
            v.push_back("hair0" + std::to_string(k));
            v.push_back("l_curl0" + std::to_string(k));
            v.push_back("r_curl0" + std::to_string(k));
        }
        return v;
    }
    void setHideEnabled(bool on) { hideEnabled_ = on; }
    // Rework, dismemberment: cuts `bone` and everything below it off this instance. From now on its
    // vertices collapse onto the joint (the stump); the cut piece, as posed now, is returned as
    // meshes in model space relative to the joint (the caller owns them), and the joint itself.
    struct Piece { MeshId mesh = 0; TextureId texture = 0; BlendMode blend = BlendMode::Opaque; bool lit = true; };
    bool sever(Renderer& r, const std::string& lowerCaseBone, std::vector<Piece>& out, Vec3& joint);
    void clearSever() { severMask_.clear(); severBones_.clear(); }
    bool severed() const { return !severMask_.empty(); }
    // The model-space centre of a bone's vertices in the last pose (dismemberment: which limb a cut hits).
    bool boneCentre(const std::string& lowerCaseBone, Vec3& out) const;
    // Centroid of the hidden-bone vertices in the last pose (model-local metres).
    Vec3 hiddenCentroid() const { return hiddenCentroid_; }
    // World matrix (model space, game units, Z-up) of a node in the last pose (dominant clip,
    // after the pose modifier). False if the node does not exist.
    bool nodeWorld(const std::string& lowerCaseName, Mat4& out) const;
    // Bounds (posed, model-local runtime metres) of the vertices bound to a bone; false if none.
    bool boneVertexBounds(const std::string& lowerCaseName, Vec3& mn, Vec3& mx) const;
    void render(Renderer& r, const Mat4& world) const;
    // With a colour multiplier (hit flashes).
    void render(Renderer& r, const Mat4& world, const Vec4& tint) const;
    void renderShadow(Renderer& r, const Mat4& world) const;
    // Unlit, self-illuminated draw (effects such as the Rippers' spit balls).
    // A see-through, glowing copy (additive, no depth write): a soul or a ghost.
    void renderGhost(Renderer& r, const Mat4& world, const Vec3& rgb) const;
    void renderGlow(Renderer& r, const Mat4& world, float brightness = 1.0f) const;
    const std::string& name() const { return base_; }
    uint32_t boundVertices() const { return bound_; }
    uint32_t unboundVertices() const { return unbound_; }
    float animDuration() const { return animDuration(0); }
    Vec3 restMin() const { return bmin_; }
    Vec3 restMax() const { return bmax_; }
    void posedBounds(Vec3& mn, Vec3& mx) const;
    // Bounds of animation `anim` at `seconds` without touching the displayed pose (no upload).
    void boundsAt(int anim, float seconds, Vec3& mn, Vec3& mx);
    // Keep a second, complete copy of the posed mesh while hiding is on (the first-person body
    // whose head is hidden still casts a whole shadow and shows whole in reflections).
    void setKeepFull(bool on) { keepFull_ = on; }
    void renderFull(Renderer& r, const Mat4& world) const;
    void renderShadowFull(Renderer& r, const Mat4& world) const;

private:
    struct Binding { int bone = -1; Vec3 local; };  // bone: index into bones_
    struct Anim {
        AnmAnimation anim;
        std::vector<int> boneToNode;  // per bones_ entry, node index or -1
        std::vector<std::vector<int>> children;
    };
    PoseModifier modifier_;
    std::vector<uint8_t> hideMask_;  // per GBS position
    std::vector<uint8_t> severMask_;  // per GBS position: 0, or 1 + the index of its cut in severBones_
    std::vector<int> severBones_;     // bones_ indices of the cuts
    bool hideEnabled_ = false;
    bool keepFull_ = false, fullValid_ = false;
    Vec3 hiddenCentroid_{0, 0, 0};
    std::vector<Mat4> domWorld_;
    int domClip_ = -1;
    void skin(int anim, float t, std::vector<Vec3>& out);
    Vec3 jointOf(int bone) const;
    struct Part {
        MeshId mesh = 0;
        MeshId fullMesh = 0;  // keepFull_: the same part with nothing hidden
        TextureId texture = 0;
        BlendMode blend = BlendMode::Opaque;
        bool lit = true;
        bool glass = false;
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
