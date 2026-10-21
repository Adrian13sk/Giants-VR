#include "scene/asset_world.h"

#include "core/log.h"

namespace gvr {

bool AssetWorld::init(const std::string& dataDir, Renderer& r) {
    int n = fs_.mountAllArchives(dataDir);
    if (n == 0) {
        status_ = "No original game archives (*.gzp) found in " + dataDir;
        LOGW("%s", status_.c_str());
        return false;
    }
    fs_.mountDirectory(dataDir);
    cache_ = std::make_unique<ModelCache>(fs_, r);

    // Showcase of original characters (first available LOD 0/1 model of each).
    struct Candidate { const char* names[3]; };
    const Candidate showcase[] = {
        {{"sm_L0.gbs", "sm_L1.gbs", nullptr}},          // Smartie
        {{"rpman_l0.gbs", "rpman_l1.gbs", nullptr}},    // Reaper
        {{"kB_L1.gbs", "kB_L2.gbs", nullptr}},          // Kabuto
        {{"MC_turret.gbs", "MC_turret_L1.gbs", nullptr}},
    };
    float x = -3.0f;
    for (const auto& c : showcase) {
        const RenderModel* m = nullptr;
        for (const char* nm : c.names)
            if (nm && (m = cache_->get(nm))) break;
        if (!m) continue;
        float width = m->boundsMax.x - m->boundsMin.x;
        float depth = m->boundsMax.z - m->boundsMin.z;
        float radius = 0.5f * std::max(width, depth);
        x += radius;
        placed_.push_back({m, Vec3{x, 0.0f, -4.0f - radius}, 0.0f, 0.25f});
        x += radius + 1.5f;
    }
    status_ = "Mounted " + std::to_string(n) + " archives, " + std::to_string(placed_.size()) +
              " original models placed";
    LOGI("%s", status_.c_str());
    return !placed_.empty();
}

void AssetWorld::update(float dt) {
    for (auto& p : placed_) p.yaw += p.spin * dt;
}

void AssetWorld::render(Renderer& r) {
    for (const auto& p : placed_) {
        Mat4 xf = Mat4::translation(p.position) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, p.yaw));
        for (const auto& part : p.model->parts) {
            DrawParams dp;
            dp.texture = part.texture;
            dp.blend = part.blend;
            // Baked vertex colours are the lighting for static models; characters are lit.
            dp.unlit = !part.lit;
            r.draw(part.mesh, xf, dp);
        }
    }
}

}  // namespace gvr
