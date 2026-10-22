#include "scene/asset_world.h"

#include <cstdlib>

#include <algorithm>
#include <cmath>

#include "assets/gti.h"
#include "assets/mission.h"
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
    renderer_ = &r;

    hasTerrain_ = loadWorld(worldName, r);
    if (hasTerrain_) {
        const size_t nTypes = registry_.build(dataDir);
        LOGI("Object registry: %zu types, %zu groups, %zu meshes", nTypes, registry_.groupCount(),
             registry_.meshCount());
        for (const auto& sc : world_.scenarios)
            if (sc.kind == 2 && fs_.exists("wm_" + sc.name + ".bin")) missions_.push_back(sc.name);
        // Test hook / mission select: GVR_MISSION=<name> starts that mission instead of the first.
        std::string first = missions_.empty() ? std::string() : missions_.front();
        if (const char* m = std::getenv("GVR_MISSION"))
            for (const auto& n : missions_)
                if (toLower(n) == toLower(m)) first = n;
        placeWorldObjects(first);
    }
    // Test showcase (Smartie, Kabuto) only when the mission doesn't populate the level itself.
    if (characters_.empty()) placeShowcase(spawn_ + Vec3{0, 0, -6.0f});
    status_ = "Mounted " + std::to_string(n) + " archives; world " + worldName +
              (hasTerrain_ ? " loaded" : " NOT loaded") + "; " + std::to_string(placed_.size()) +
              " static + " + std::to_string(skinned_.size()) + " animated original models placed";
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
    // Original characters with original animations (CPU-skinned), then static props.
    struct Anim { const char* base; const char* anim; };
    const Anim characters[] = {
        {"sm_L0", "sm_stand.anm"},                  // Smartie
        {"kB_L1", "kb_walk.anm"},                   // Kabuto (walk cycle in place)
    };
    float x = centre.x - 6.0f;
    for (const auto& c : characters) {
        auto m = std::make_unique<SkinnedModel>();
        if (!m->load(fs_, *cache_, *renderer_, c.base, c.anim)) continue;
        m->pose(*renderer_, 0.0f);
        Vec3 pmn, pmx;
        m->posedBounds(pmn, pmx);  // the export (rest) pose can be far from the animated pose
        LOGI("  %s posed height %.2f m", c.base, pmx.y - pmn.y);
        float radius = 0.5f * std::max(pmx.x - pmn.x, pmx.z - pmn.z);
        x += radius;
        Vec3 p{x, centre.y, centre.z - radius};
        float gy;
        if (hasTerrain_ && terrain_.heightAt(p.x, p.z, gy)) p.y = gy;
        // Original models face -Z in the runtime frame; turn them to face the spawn point.
        skinned_.push_back({std::move(m), p, kPi});
        x += radius + 1.5f;
    }
    struct Candidate { const char* names[3]; };
    const Candidate props[] = {
        {{"MC_turret.gbs", "MC_turret_L1.gbs", nullptr}},
        {{"Ptree.gbs", "Ptree_L1.gbs", nullptr}},       // palm tree
    };
    for (const auto& c : props) {
        const RenderModel* m = nullptr;
        for (const char* nm : c.names)
            if (nm && (m = cache_->get(nm))) break;
        if (!m) continue;
        float radius = 0.5f * std::max(m->boundsMax.x - m->boundsMin.x, m->boundsMax.z - m->boundsMin.z);
        x += radius;
        Vec3 p{x, centre.y, centre.z - radius};
        float gy;
        if (hasTerrain_ && terrain_.heightAt(p.x, p.z, gy)) p.y = gy;
        placed_.push_back({m, p, 0.0f, 0.0f});
        x += radius + 1.5f;
    }
}

// Placement transform in the runtime frame. The game frame is Z-up; the conversion
// (-x, z, y) is a proper rotation, so a rotation of a degrees about a game axis g becomes a
// rotation of a degrees about the converted axis: game Z -> runtime +Y, game X -> runtime -X,
// game Y -> runtime +Z. Yaw (about Z) is verified on placed walls; the pitch/roll order used for
// op-4 objects (82 in all missions, small tilts) is provisional.
struct Placement {
    uint32_t type;
    float x, y, z, yaw, pitch, roll, scale;
    bool mission = false;
    int trigger = 0;  // mission trigger kind (op 8 low bits)
    bool posted = false;  // standing on a structure (mounted): keep its height, don't drop to the terrain
};

// Creature object classes ("ospec", type op 0x1E) seen in the mission files: 1 Kabuto,
// 4 offspring, 18 Smartie variants, 23 Ripper, 42 charger, 45 Dactyl, 51 Reaper grunt,
// 53 Verm, 99 Raik. Smarties (sm_l0) and Sonaks carry no class op but many clips.
static bool isCharacter(const ObjectRegistry& reg, uint32_t type, const std::string& model) {
    const ObjectType* t = reg.type(type);
    if (!t) return false;
    static const int classes[] = {1, 4, 18, 23, 42, 45, 51, 53, 99};
    for (int c : classes)
        if (t->objectClass == c) return true;
    const ObjectGroup* g = t->hasGroup ? reg.group(t->group) : nullptr;
    if (g && g->anims.size() >= 5 && t->objectClass < 0) return true;
    return toLower(model).rfind("smtimmy", 0) == 0;  // Timmy (placed for the opening cutscene)
}
static Mat4 placementMatrix(const Placement& p) {
    const float k = kPi / 180.0f;
    const Quat q = Quat::fromAxisAngle({0, 1, 0}, p.yaw * k) * Quat::fromAxisAngle({-1, 0, 0}, p.pitch * k) *
                   Quat::fromAxisAngle({0, 0, 1}, p.roll * k);
    return Mat4::translation(giantsToRuntime(p.x, p.y, p.z)) * Mat4::rotation(q) *
           Mat4::scale({p.scale, p.scale, p.scale});
}

bool AssetWorld::selectMission(const std::string& name) {
    bool known = false;
    for (const auto& n : missions_) known |= n == name;
    if (!known || !hasTerrain_) return false;
    placeWorldObjects(name);
    return missionName_ == name;
}

bool AssetWorld::objectBounds(const std::string& model, Vec3& mn, Vec3& mx) const {
    const std::string want = toLower(model) + ".gbs";
    for (const auto& o : worldObjects_) {
        if (!o.model || toLower(o.model->name) != want) continue;
        mn = {1e30f, 1e30f, 1e30f};
        mx = {-1e30f, -1e30f, -1e30f};
        for (int c = 0; c < 8; ++c) {
            const Vec3 lc{c & 1 ? o.model->boundsMax.x : o.model->boundsMin.x, c & 2 ? o.model->boundsMax.y : o.model->boundsMin.y,
                          c & 4 ? o.model->boundsMax.z : o.model->boundsMin.z};
            const Vec4 w = o.transform * Vec4{lc.x, lc.y, lc.z, 1.0f};
            mn = {std::min(mn.x, w.x), std::min(mn.y, w.y), std::min(mn.z, w.z)};
            mx = {std::max(mx.x, w.x), std::max(mx.y, w.y), std::max(mx.z, w.z)};
        }
        return true;
    }
    return false;
}

std::string AssetWorld::nextMission() const {
    for (size_t i = 0; i + 1 < missions_.size(); ++i)
        if (missions_[i] == missionName_) return missions_[i + 1];
    return {};
}

void AssetWorld::placeWorldObjects(const std::string& missionName) {
    // A mission's setup stream places its structures, plants and creatures on top of the world's
    // own objects; switching missions rebuilds the level state from scratch (as the original
    // does when it loads a mission).
    worldObjects_.clear();
    characters_.clear();
    triggers_.clear();
    collision_.clear();
    mission_ = MissionData{};
    missionName_.clear();
    auto typeScale = [&](uint32_t t) {
        const ObjectType* ot = registry_.type(t);
        return ot && ot->scale > 0 ? ot->scale : 1.0f;
    };
    std::vector<Placement> all;
    for (const auto& o : world_.objects) all.push_back({o.type, o.x, o.y, o.z, o.angle, 0, 0, typeScale(o.type)});
    size_t mounted = 0;
    for (const auto& sc : world_.scenarios) {
        if (sc.kind != 2 || sc.name != missionName) continue;
        std::vector<uint8_t> bytes;
        if (!fs_.read("wm_" + sc.name + ".bin", bytes)) {
            LOGW("Mission file wm_%s.bin not found", sc.name.c_str());
            continue;
        }
        std::string err;
        if (!parseMission(bytes.data(), bytes.size(), mission_, &err)) {
            LOGW("Mission %s: %s", sc.name.c_str(), err.c_str());
            mission_ = MissionData{};
            continue;
        }
        missionName_ = sc.name;
        // Player start: the mission's "marker" object with id 1 (op 5), facing its yaw.
        // Provisional reading: multiplayer-style start markers; in 1a_1_timmy it stands ~280 m
        // from the Smartie village, facing it.
        for (const auto& o : mission_.objects) {
            if (o.group != 1 || o.parent >= 0 || toLower(registry_.modelName(o.type)) != "marker") continue;
            Vec3 p = giantsToRuntime(o.x, o.y, o.z);
            float gy;
            if (terrain_.heightAt(p.x, p.z, gy)) p.y = gy;
            spawn_ = p;
            spawnYaw_ = o.yaw * kPi / 180.0f;
            LOGI("Spawn from mission start marker: %.1f %.1f %.1f yaw %.0f deg", p.x, p.y, p.z, o.yaw);
            break;
        }
        for (const auto& o : mission_.objects) {
            float z = o.z;
            bool onPost = false;
            if (o.parent >= 0) {
                // Mounted (op 0x16). Characters on a structure's default mount point (255) - the
                // Reaper sentries on guard towers - stand on its top; other mounts (carried items
                // such as the jetpack, Tel in his crashed plane on hardpoint 0) are not shown yet:
                // hardpoint positions live in data we haven't decoded.
                ++mounted;
                const std::string cm = registry_.modelName(o.type);
                if (o.mount[0] != 255 || !isCharacter(registry_, o.type, cm) || size_t(o.parent) >= mission_.objects.size()) continue;
                const MissionObject& par = mission_.objects[size_t(o.parent)];
                const std::string pm = registry_.modelName(par.type);
                const RenderModel* m = pm.empty() ? nullptr : cache_->get(pm + ".gbs", registry_.modelAnim(par.type));
                if (!m) continue;
                const float ps = par.scale > 0 ? par.scale : typeScale(par.type);
                z = par.z + m->boundsMax.y * ps / gGameUnitsToMeters;
                onPost = true;
            }
            all.push_back({o.type, o.x, o.y, z, o.yaw, o.pitch, o.roll, o.scale > 0 ? o.scale : typeScale(o.type), true,
                           int(o.flags & 0x0F), onPost});
        }
        LOGI("Mission %s: %zu objects (%zu mounted), start flick '%s'", sc.name.c_str(), mission_.objects.size(),
             mounted, mission_.startFlick.c_str());
        break;
    }
    size_t hidden = 0, missing = 0;
    for (const auto& p : all) {
        const std::string name = registry_.modelName(p.type);
        if (p.mission && p.trigger && !isCharacter(registry_, p.type, name)) {
            // Proximity-type triggers on objects (Timmy's tower, invisible trigger spheres): their
            // world-space bounds, from the model when it has one.
            TriggerObject t;
            t.kind = p.trigger;
            t.type = p.type;
            const Vec3 at = giantsToRuntime(p.x, p.y, p.z);
            t.boundsMin = at - Vec3{0.5f, 0.0f, 0.5f};
            t.boundsMax = at + Vec3{0.5f, 2.0f, 0.5f};
            if (const RenderModel* m = name.empty() ? nullptr : cache_->get(name + ".gbs", registry_.modelAnim(p.type))) {
                const Mat4 xf = placementMatrix(p);
                Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
                for (int c = 0; c < 8; ++c) {
                    const Vec3 lc{c & 1 ? m->boundsMax.x : m->boundsMin.x, c & 2 ? m->boundsMax.y : m->boundsMin.y,
                                  c & 4 ? m->boundsMax.z : m->boundsMin.z};
                    const Vec4 w = xf * Vec4{lc.x, lc.y, lc.z, 1.0f};
                    mn = {std::min(mn.x, w.x), std::min(mn.y, w.y), std::min(mn.z, w.z)};
                    mx = {std::max(mx.x, w.x), std::max(mx.y, w.y), std::max(mx.z, w.z)};
                }
                t.boundsMin = mn;
                t.boundsMax = mx;
            }
            triggers_.push_back(t);
        }
        if (name.empty() || isInvisibleHelper(name)) { ++hidden; continue; }
        if (p.mission && isCharacter(registry_, p.type, name) && fs_.exists(name + ".skn")) {
            // Animated by the game layer (AI or ambient loop), not baked into a static pose.
            const ObjectType* t = registry_.type(p.type);
            Vec3 pos = giantsToRuntime(p.x, p.y, p.z);
            float gy;
            if (!p.posted && hasTerrain_ && terrain_.heightAt(pos.x, pos.z, gy)) pos.y = gy;
            // A sentry stands on the highest walkable surface of its structure (the tower's
            // platform, below the roof tip the bounds give).
            if (p.posted && collision_.groundAt(pos.x, pos.z, pos.y + 0.05f, gy)) pos.y = gy;
            characters_.push_back({p.type, name, t ? t->objectClass : -1, pos, p.yaw * kPi / 180.0f, p.scale, p.trigger, p.posted});
            continue;
        }
        const RenderModel* m = cache_->get(name + ".gbs", registry_.modelAnim(p.type));
        if (!m) { ++missing; continue; }
        const Mat4 xf = placementMatrix(p);
        const Vec3 lc = (m->boundsMin + m->boundsMax) * 0.5f;
        const Vec4 wc = xf * Vec4{lc.x, lc.y, lc.z, 1.0f};
        const float radius = 0.5f * length(m->boundsMax - m->boundsMin) * p.scale;
        worldObjects_.push_back({m, xf, Vec3{wc.x, wc.y, wc.z}, radius});
        // Solid unless it is vegetation the original lets you walk through.
        const std::string ln = toLower(name);
        bool soft = false;
        for (const char* k : {"weed", "shrub", "grass", "flower", "plant", "lilly", "leaf", "bush"})
            if (ln.find(k) != std::string::npos) soft = true;
        if (!soft && !m->collisionTris.empty()) {
            std::vector<Vec3> w;
            w.reserve(m->collisionTris.size());
            for (const Vec3& v : m->collisionTris) {
                const Vec4 p4 = xf * Vec4{v.x, v.y, v.z, 1.0f};
                w.push_back({p4.x, p4.y, p4.z});
            }
            collision_.addObject(w);
        }
    }
    LOGI("Level objects: %zu placed, %zu characters, %zu helpers skipped, %zu without a model", worldObjects_.size(),
         characters_.size(), hidden, missing);
    LOGI("Collision: %zu solid objects, %zu triangles", collision_.objectCount(), collision_.triangleCount());
}

void AssetWorld::update(float dt) {
    time_ += dt;
    for (auto& p : placed_) p.yaw += p.spin * dt;
    for (auto& s : skinned_) s.model->pose(*renderer_, time_);
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
    const Vec3 eye = r.currentView().eyePosition;
    const float maxDist = r.environment().fogFar;
    for (const auto& o : worldObjects_) {
        // Cull objects outside the view frustum or fully fogged out.
        if (length(o.centre - eye) - o.radius > maxDist || !r.sphereVisible(o.centre, o.radius)) {
            r.countCulled();
            continue;
        }
        for (const auto& part : o.model->parts) {
            DrawParams dp;
            dp.texture = part.texture;
            dp.blend = part.blend;
            dp.unlit = !part.lit;
            r.draw(part.mesh, o.transform, dp);
        }
    }
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
    for (const auto& s : skinned_)
        s.model->render(r, Mat4::translation(s.position) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, s.yaw)));
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
