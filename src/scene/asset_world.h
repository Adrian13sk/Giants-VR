// Scene built from the user's ORIGINAL Giants: Citizen Kabuto data files:
// world definition (w_<World>.bin), GTI terrain with its textures and lightmap, sea plane,
// fog, and a showcase of original character models placed on the terrain.
// Object placement from the world/mission files is not implemented yet (see BLOCKERS.md).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "assets/gzp.h"
#include "assets/object_defs.h"
#include "assets/world_bin.h"
#include "render/renderer.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"
#include "scene/terrain.h"

namespace gvr {

class AssetWorld {
public:
    bool init(const std::string& dataDir, Renderer& r, const std::string& worldName);
    void update(float dt);
    void render(Renderer& r);
    bool loaded() const { return !placed_.empty() || !skinned_.empty() || hasTerrain_; }
    bool hasTerrain() const { return hasTerrain_; }
    const std::string& status() const { return status_; }
    const AssetFS& fs() const { return fs_; }
    ModelCache* models() { return cache_.get(); }
    const Terrain& terrain() const { return terrain_; }
    Vec3 spawnPoint() const { return spawn_; }
    float spawnYaw() const { return spawnYaw_; }
    const WorldDefinition& world() const { return world_; }

    struct Placed {
        const RenderModel* model;
        Vec3 position;
        float yaw;
        float spin;  // radians/sec turntable speed
    };
    std::vector<Placed>& placed() { return placed_; }
    size_t skinnedCount() const { return skinned_.size(); }
    size_t worldObjectCount() const { return worldObjects_.size(); }

private:
    bool loadWorld(const std::string& name, Renderer& r);
    void placeShowcase(const Vec3& centre);
    void placeWorldObjects(const std::string& dataDir);

    struct SkinnedPlaced {
        std::unique_ptr<SkinnedModel> model;
        Vec3 position;
        float yaw;
    };
    std::vector<SkinnedPlaced> skinned_;
    Renderer* renderer_ = nullptr;
    float time_ = 0.0f;
    AssetFS fs_;
    std::unique_ptr<ModelCache> cache_;
    std::vector<Placed> placed_;
    struct WorldObject { const RenderModel* model; Mat4 transform; Vec3 centre; float radius; };
    std::vector<WorldObject> worldObjects_;
    ObjectRegistry registry_;
    std::string status_;
    WorldDefinition world_;
    Terrain terrain_;
    bool hasTerrain_ = false;
    MeshId seaMesh_ = 0;
    MeshId skyMesh_ = 0;
    TextureId skyTexture_ = 0;
    TextureId seaTexture_ = 0;
    Vec3 spawn_{0, 0, 0};
    float spawnYaw_ = 0.0f;
};

}  // namespace gvr
