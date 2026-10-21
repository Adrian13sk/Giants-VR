#include "scene/asset_world.h"

#include <algorithm>
#include <cmath>

#include "assets/gti.h"
#include "core/log.h"

namespace gvr {

bool AssetWorld::init(const std::string& dataDir, Renderer& r, const std::string& worldName) {
    int n = fs_.mountAllArchives(dataDir);
    if (n == 0) {
        status_ = "No original game archives (*.gzp) found in " + dataDir;
        LOGW("%s", status_.c_str());
        return false;
    }
    fs_.mountDirectory(dataDir);
    cache_ = std::make_unique<ModelCache>(fs_, r);

    hasTerrain_ = loadWorld(worldName, r);
    placeShowcase(spawn_ + Vec3{0, 0, -6.0f});
    status_ = "Mounted " + std::to_string(n) + " archives; world " + worldName +
              (hasTerrain_ ? " loaded" : " NOT loaded") + "; " + std::to_string(placed_.size()) +
              " original models placed";
    LOGI("%s", status_.c_str());
    return loaded();
}

bool AssetWorld::loadWorld(const std::string& name, Renderer& r) {
    std::vector<uint8_t> bytes;
    const std::string binName = "w_" + name + ".bin";
    if (!fs_.read(binName, bytes)) {
        LOGW("World file %s not found", binName.c_str());
        return false;
    }
    std::string err;
    if (!parseWorldBin(bytes.data(), bytes.size(), world_, &err)) {
        LOGW("%s: %s", binName.c_str(), err.c_str());
        return false;
    }
    LOGI("World %s: terrain=%s ground=%s wall=%s sky=%s sea=%s records=%zu objects=%zu stop=%d complete=%d",
         name.c_str(), world_.terrainName.c_str(), world_.groundTexture.c_str(), world_.wallTexture1.c_str(),
         world_.skyTexture.c_str(), world_.seaTexture.c_str(), world_.recordsParsed, world_.objects.size(),
         world_.stopOpcode, world_.complete);

    if (!fs_.read(world_.terrainName, bytes)) {
        LOGW("Terrain %s not found", world_.terrainName.c_str());
        return false;
    }
    GtiTerrain gti;
    if (!parseGti(bytes.data(), bytes.size(), gti, &err)) {
        LOGW("%s: %s", world_.terrainName.c_str(), err.c_str());
        return false;
    }
    TextureId ground = world_.groundTexture.empty() ? 0 : cache_->texture(world_.groundTexture);
    TextureId wall = world_.wallTexture1.empty() ? 0 : cache_->texture(world_.wallTexture1);
    if (!terrain_.build(gti, r, ground, wall)) return false;

    // Fog / sky colour from the world definition. ASSUMPTION (unverified): colour bytes are
    // stored B,G,R like a D3D colour word. Needs comparison with the original renderer.
    EnvironmentParams& env = r.environment();
    if (world_.hasFog && world_.fogFar > world_.fogNear) {
        env.fogNear = world_.fogNear * gGameUnitsToMeters;
        env.fogFar = world_.fogFar * gGameUnitsToMeters;
        env.fogColor = {world_.fogRGB[2] / 255.0f, world_.fogRGB[1] / 255.0f, world_.fogRGB[0] / 255.0f};
    } else {
        env.fogNear = 300.0f;
        env.fogFar = 2500.0f;
    }
    // Clear colour is linear; the sRGB framebuffer encodes on write, so convert the
    // sRGB-authored fog colour to linear for both uses.
    auto toLinear = [](float c) { return std::pow(c, 2.2f); };
    env.fogColor = {toLinear(env.fogColor.x), toLinear(env.fogColor.y), toLinear(env.fogColor.z)};
    env.clearColor = env.fogColor;

    // Sea plane at game height 0, covering the terrain with a wide margin.
    const uint8_t white[4] = {255, 255, 255, 255};
    Vec3 bmin = terrain_.boundsMin(), bmax = terrain_.boundsMax();
    float half = 0.5f * std::max(bmax.x - bmin.x, bmax.z - bmin.z) + 3000.0f;
    MeshData sea = prim::plane(half, 16, half / 15.0f, white);
    Mat4 at = Mat4::translation({0.5f * (bmin.x + bmax.x), 0.0f, 0.5f * (bmin.z + bmax.z)});
    MeshData placedSea;
    placedSea.append(sea, at);
    seaMesh_ = r.uploadMesh(placedSea);
    seaTexture_ = world_.seaTexture.empty() ? 0 : cache_->texture(world_.seaTexture);

    // Sky: the world's panoramic sky texture (e.g. story1_sky, 2048x512) on a band around
    // the viewer. Band elevation range is a provisional choice (unverified vs. original).
    skyTexture_ = world_.skyTexture.empty() ? 0 : cache_->texture(world_.skyTexture);
    if (skyTexture_) {
        MeshData band;
        const int slices = 48;
        const float radius = 1500.0f, lo = -0.08f, hi = 0.75f;  // elevation (radians)
        for (int i = 0; i <= slices; ++i) {
            float a = 2 * kPi * i / slices;
            for (int k = 0; k < 2; ++k) {
                float el = k ? hi : lo;
                Vertex v{};
                v.pos[0] = std::sin(a) * std::cos(el) * radius;
                v.pos[1] = std::sin(el) * radius;
                v.pos[2] = -std::cos(a) * std::cos(el) * radius;
                v.uv[0] = float(i) / slices;
                v.uv[1] = k ? 0.0f : 1.0f;
                v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
                band.vertices.push_back(v);
            }
        }
        for (int i = 0; i < slices; ++i) {
            uint32_t b = uint32_t(i * 2);
            band.indices.insert(band.indices.end(), {b, b + 2, b + 1, b + 1, b + 2, b + 3});
        }
        skyMesh_ = r.uploadMesh(band);
    }

    spawn_ = terrain_.suggestSpawn();
    LOGI("Spawn point (m): %.1f %.1f %.1f", spawn_.x, spawn_.y, spawn_.z);
    return true;
}

void AssetWorld::placeShowcase(const Vec3& centre) {
    // Original characters (first available LOD), placed in an arc in front of the spawn.
    struct Candidate { const char* names[3]; };
    const Candidate showcase[] = {
        {{"sm_L0.gbs", "sm_L1.gbs", nullptr}},          // Smartie
        {{"rpman_l0.gbs", "rpman_l1.gbs", nullptr}},    // Reaper
        {{"kB_L1.gbs", "kB_L2.gbs", nullptr}},          // Kabuto
        {{"MC_turret.gbs", "MC_turret_L1.gbs", nullptr}},
        {{"Ptree.gbs", "Ptree_L1.gbs", nullptr}},       // palm tree
    };
    float x = centre.x - 4.0f;
    for (const auto& c : showcase) {
        const RenderModel* m = nullptr;
        for (const char* nm : c.names)
            if (nm && (m = cache_->get(nm))) break;
        if (!m) continue;
        float radius = 0.5f * std::max(m->boundsMax.x - m->boundsMin.x, m->boundsMax.z - m->boundsMin.z);
        x += radius;
        Vec3 p{x, centre.y, centre.z - radius};
        float gy;
        if (hasTerrain_ && terrain_.heightAt(p.x, p.z, gy)) p.y = gy;
        placed_.push_back({m, p, 0.0f, 0.25f});
        x += radius + 1.5f;
    }
}

void AssetWorld::update(float dt) {
    for (auto& p : placed_) p.yaw += p.spin * dt;
}

void AssetWorld::render(Renderer& r) {
    if (skyMesh_) {
        DrawParams sp;
        sp.texture = skyTexture_;
        sp.unlit = true;
        sp.noFog = true;
        sp.doubleSided = true;
        sp.depthWrite = false;  // always behind everything
        r.draw(skyMesh_, Mat4::translation(r.currentView().eyePosition), sp);
    }
    if (hasTerrain_) terrain_.render(r);
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
    if (seaMesh_) {
        DrawParams sp;
        sp.texture = seaTexture_;
        sp.unlit = true;
        sp.blend = BlendMode::AlphaBlend;
        sp.tint = {1, 1, 1, 0.75f};
        sp.doubleSided = true;
        r.draw(seaMesh_, Mat4::identity(), sp);
    }
}

}  // namespace gvr
