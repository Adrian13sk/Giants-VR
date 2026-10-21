// Loads original GBS models and their TGA textures from the AssetFS and uploads them to the GPU.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/gbs.h"
#include "assets/gzp.h"
#include "render/renderer.h"

namespace gvr {

// Giants world units -> metres. Determined empirically; see docs/formats/GBS.md ("Scale").
extern float gGameUnitsToMeters;

// Converts a Giants (Z-up) position to the runtime frame (Y-up, -Z forward), in metres.
inline Vec3 giantsToRuntime(float x, float y, float z) {
    return Vec3{-x, z, y} * gGameUnitsToMeters;
}

struct RenderModel {
    struct Part {
        MeshId mesh = 0;
        TextureId texture = 0;
        BlendMode blend = BlendMode::Opaque;
        std::string textureName;
        uint32_t triangles = 0;
        bool lit = false;  // true: dynamic lighting (no usable baked vertex colours)
    };
    std::string name;
    std::vector<Part> parts;
    Vec3 boundsMin, boundsMax;  // runtime frame, metres
    std::vector<Vec3> collisionTris;  // opaque parts, model-local metres, 3 per triangle
    uint32_t triangles = 0;
    uint32_t missingTextures = 0;
};

class ModelCache {
public:
    ModelCache(const AssetFS& fs, Renderer& r) : fs_(fs), renderer_(r) {}
    // Returns nullptr if the model cannot be found or parsed (reason logged).
    // skinAnim: for skinned static objects (<base>.skn + this .anm), the mesh is baked in the
    // animation's default (rest) pose; their vertex RGB is skin data, not lighting.
    // dynamicLit: ignore baked RGB (objects carried by characters move, so baked light is wrong).
    const RenderModel* get(const std::string& gbsName, const std::string& skinAnim = std::string(),
                           bool dynamicLit = false);
    TextureId texture(const std::string& name, bool* hadAlpha = nullptr);

private:
    const AssetFS& fs_;
    Renderer& renderer_;
    std::unordered_map<std::string, std::unique_ptr<RenderModel>> models_;
    struct TexEntry { TextureId id; bool alpha; };
    std::unordered_map<std::string, TexEntry> textures_;
};

// Builds CPU mesh data for one GBS model (exposed for tests and offline tools).
struct BuiltPart {
    MeshData mesh;
    std::vector<uint32_t> posIndex;  // per vertex: source GBS position index (for skinning)
    std::string texture;
    uint32_t subFlags = 0;
    bool lit = false;
};
// ignoreRgb: treat every sub-object as dynamically lit (skinned meshes, whose RGB array holds
// skinning data rather than baked lighting).
std::vector<BuiltPart> buildGbsParts(const GbsModel& m, bool ignoreRgb = false);
// Replaces g.positions with <base>.skn evaluated in the default pose of `anim`. False if the
// object has no usable skin.
bool looksLikeSkinData(const std::array<uint8_t, 3>* rgb, size_t n);
bool bakeDefaultSkinPose(const AssetFS& fs, const std::string& base, const std::string& anim, GbsModel& g);

}  // namespace gvr
