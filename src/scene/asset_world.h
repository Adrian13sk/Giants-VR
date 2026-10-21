// Scene built from the user's ORIGINAL Giants: Citizen Kabuto data files.
// Current scope (M7 step 1): mount the GZP archives and display original character models
// at their original relative scale around the player. Terrain and level object placement
// follow in later milestones.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "assets/gzp.h"
#include "render/renderer.h"
#include "scene/model_cache.h"

namespace gvr {

class AssetWorld {
public:
    bool init(const std::string& dataDir, Renderer& r);
    void update(float dt);
    void render(Renderer& r);
    bool loaded() const { return !placed_.empty(); }
    const std::string& status() const { return status_; }
    const AssetFS& fs() const { return fs_; }
    ModelCache* models() { return cache_.get(); }

    struct Placed {
        const RenderModel* model;
        Vec3 position;
        float yaw;
        float spin;  // radians/sec turntable speed
    };
    std::vector<Placed>& placed() { return placed_; }

private:
    AssetFS fs_;
    std::unique_ptr<ModelCache> cache_;
    std::vector<Placed> placed_;
    std::string status_;
};

}  // namespace gvr
