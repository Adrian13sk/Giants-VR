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
    uint32_t triangles = 0;
    uint32_t missingTextures = 0;
};

class ModelCache {
public:
    ModelCache(const AssetFS& fs, Renderer& r) : fs_(fs), renderer_(r) {}
    // Returns nullptr if the model cannot be found or parsed (reason logged).
    const RenderModel* get(const std::string& gbsName);
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
    std::string texture;
    uint32_t subFlags = 0;
    bool lit = false;
};
std::vector<BuiltPart> buildGbsParts(const GbsModel& m);

}  // namespace gvr
