// Scene built from the user's ORIGINAL Giants: Citizen Kabuto data files:
// world definition (w_<World>.bin), GTI terrain with its textures and lightmap, sea plane,
// fog, and a showcase of original character models placed on the terrain.
// Objects come from the world stream (op 42) and the first mission's setup stream (wm_*.bin).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "assets/gzp.h"
#include "assets/mission.h"
#include "assets/object_defs.h"
#include "assets/world_bin.h"
#include "render/renderer.h"
#include "scene/collision.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"
#include "scene/terrain.h"

namespace gvr {

class AssetWorld {
public:
    // startMission: the mission to begin with (a saved checkpoint); empty = the world's first.
    bool init(const std::string& dataDir, Renderer& r, const std::string& worldName, const std::string& startMission = std::string());
    void update(float dt);
    void render(Renderer& r, bool reflection = false);  // reflection: the mirrored pass (no water, less detail)
    // World objects near `centre` into the current shadow pass.
    void renderShadowCasters(Renderer& r, const Vec3& centre, float radius) const;
    bool loaded() const { return !placed_.empty() || !skinned_.empty() || hasTerrain_; }
    bool hasTerrain() const { return hasTerrain_; }
    const std::string& status() const { return status_; }
    const AssetFS& fs() const { return fs_; }
    ModelCache* models() { return cache_.get(); }
    const Terrain& terrain() const { return terrain_; }
    // Last render(): world objects drawn and their parts (draw calls), for profiling.
    uint32_t statObjects() const { return statObjects_; }
    uint32_t statParts() const { return statParts_; }
    Vec3 spawnPoint() const { return spawn_; }
    float spawnYaw() const { return spawnYaw_; }
    const WorldDefinition& world() const { return world_; }
    const CollisionWorld& collision() const { return collision_; }
    const ObjectRegistry& registry() const { return registry_; }
    const MissionData& mission() const { return mission_; }
    // Animated characters placed by the mission (creatures, Smarties, Timmy): runtime frame,
    // feet on the terrain, yaw in radians about +Y (0 = facing -Z).
    struct Character {
        uint32_t type;
        std::string model;
        int objectClass;
        Vec3 position;
        float yaw;
        float scale;
        int trigger;  // mission trigger kind: 1 proximity, 2 death, 3 destination arrival, 4 Vimp meat
        bool posted = false;  // on a structure's mount point (e.g. a guard tower): position.y is its floor
        std::string hold;     // hanging from the mount point (position) by this hand bone, e.g. "ref_lhand"
        Vec3 hang;            // hanging point (hands), when hasHang: 1b's Smarties on the rock pillars
        bool hasHang = false;
        int group = -1;       // mission op 5 id (race missions: 11 = a rival rider, 12 = the starter)
    };
    const std::vector<Character>& characters() const { return characters_; }
    // Mission objects (not characters) carrying a trigger, with world-space bounds.
    struct TriggerObject {
        int kind;
        uint32_t type;
        Vec3 boundsMin, boundsMax;
    };
    const std::vector<TriggerObject>& triggerObjects() const { return triggers_; }
    int startObject_ = -1;  // mission object that is the player's start (skipped as a character)
    // Ripper nests (class 47): placed by the mission, run by the game layer.
    struct Nest {
        uint32_t type;
        const RenderModel* model;
        Mat4 transform;  // runtime, base on the terrain
        Vec3 position;
        float radius;    // bounding radius (m)
        float hitPoints;
    };
    const std::vector<Nest>& nests() const { return nests_; }
    const std::string& missionName() const { return missionName_; }
    // Missions of this world in story order (scenario kind 2 with a wm_<name>.bin file).
    const std::vector<std::string>& missions() const { return missions_; }
    // Rebuilds the level state (objects, characters, triggers, collision, spawn) for a mission.
    bool selectMission(const std::string& name);
    std::string nextMission() const;  // empty after the last one
    // Replaces the loaded world (terrain, sky, sea, objects) with another one and selects its
    // first mission; archives, the model cache and the object registry are kept.
    bool switchWorld(const std::string& name);
    const std::string& worldName() const { return worldName_; }
    // The world that follows `world` in the Meccaryn story (Story1 -> Story2 ... -> the_end), or empty.
    static std::string nextStoryWorld(const std::string& world);
    // The world this one's scenario list continues to (op 92 kind 3: Story5 -> rstory1, Rstory5 ->
    // kstory1, Kstory5 -> the_end); empty if none.
    std::string nextWorld() const;
    // World bounds of the first placed object using <model>.gbs (case-insensitive); false if none.
    bool objectBounds(const std::string& model, Vec3& mn, Vec3& mx) const;
    // Object types not drawn (a cutscene shows its own animated copy, e.g. the hatch).
    void setHiddenTypes(std::vector<int> types) { hiddenTypes_ = std::move(types); }

    struct Placed {
        const RenderModel* model;
        Vec3 position;
        float yaw;
        float spin;  // radians/sec turntable speed
    };
    std::vector<Placed>& placed() { return placed_; }
    size_t skinnedCount() const { return skinned_.size(); }
    size_t worldObjectCount() const { return worldObjects_.size(); }
    // Placed level objects, for the game layer (destruction): what they are and how they're drawn.
    struct WorldObject {
        const RenderModel* model;
        Mat4 transform;
        Vec3 centre;
        float radius;
        uint32_t type = 0;
        std::string name;     // model name (lower case)
        Mat4 placement;       // original transform
        float scale = 1;
        int collision = -1;   // CollisionWorld index (-1: not solid)
        bool hidden = false;
        // Damage look (set by the game layer): the data's scorched "<texture>_d" variants
        // swapped in, a char overlay of growing coverage (burn 0..1) and glowing embers.
        bool damaged = false;
        std::vector<TextureId> damagedTex;  // per model part (0 = no variant)
        float burn = 0, glow = 0;
    };
    // Char overlay textures of growing coverage (light, medium, heavy) and the ember glow.
    void setBurnTextures(TextureId light, TextureId medium, TextureId heavy, TextureId embers) {
        burnTex_[0] = light; burnTex_[1] = medium; burnTex_[2] = heavy; emberTex_ = embers;
    }
    std::vector<WorldObject>& worldObjects() { return worldObjects_; }
    CollisionWorld& collisionMutable() { return collision_; }

private:
    TextureId burnTex_[3] = {}, emberTex_ = 0;
    bool loadWorld(const std::string& name, Renderer& r);
    void collectMissions(const std::string& worldName);
    std::string worldName_;
    TextureId terrainNoise_ = 0;
    void placeShowcase(const Vec3& centre);
    void placeWorldObjects(const std::string& missionName);

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
    std::vector<int> hiddenTypes_;
    std::vector<WorldObject> worldObjects_;
    ObjectRegistry registry_;
    MissionData mission_;
    std::vector<Character> characters_;
    std::vector<TriggerObject> triggers_;
    std::vector<Nest> nests_;
    std::string missionName_;
    std::vector<std::string> missions_;
    CollisionWorld collision_;
    std::string status_;
    WorldDefinition world_;
    Terrain terrain_;
    uint32_t statObjects_ = 0, statParts_ = 0;
    bool hasTerrain_ = false;
    MeshId seaMesh_ = 0;
    MeshId skyMesh_ = 0;
    TextureId skyTexture_ = 0;
    TextureId seaTexture_ = 0;
    Vec3 spawn_{0, 0, 0};
    float spawnYaw_ = 0.0f;
};

}  // namespace gvr
