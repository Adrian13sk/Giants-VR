#include "scene/asset_world.h"

#include <cstdlib>

#include <algorithm>
#include <cmath>

#include "assets/gti.h"
#include "assets/mission.h"
#include "assets/skin_anim.h"
#include "core/log.h"

namespace gvr {

bool AssetWorld::init(const std::string& dataDir, Renderer& r, const std::string& worldName, const std::string& startMission) {
    int n = fs_.mountAllArchives(dataDir);
    if (n == 0) {
        status_ = "No original game archives (*.gzp) found in " + dataDir;
        LOGW("%s", status_.c_str());
        return false;
    }
    fs_.mountDirectory(dataDir);
    cache_ = std::make_unique<ModelCache>(fs_, r);
    renderer_ = &r;

    worldName_ = worldName;
    hasTerrain_ = loadWorld(worldName, r);
    if (hasTerrain_) {
        const size_t nTypes = registry_.build(dataDir);
        LOGI("Object registry: %zu types, %zu groups, %zu meshes", nTypes, registry_.groupCount(),
             registry_.meshCount());
        collectMissions(worldName);
        // Test hook / mission select: GVR_MISSION=<name> starts that mission instead of the first.
        std::string first = missions_.empty() ? std::string() : missions_.front();
        for (const auto& n : missions_)
            if (!startMission.empty() && toLower(n) == toLower(startMission)) first = n;
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
    {
        // Terrain variety from original textures of other worlds: the desert sand (story3_grnd,
        // paled in the shader) for beaches and the sea bed, an earthy ground (Rstory1_ground) in
        // patches, a warm second rock (story3_rock); plus our tileable blend noise.
        const std::vector<uint8_t> noise = makeTerrainNoise(256, 11u);
        if (terrainNoise_) r.destroyTexture(terrainNoise_);
        const TextureId noiseTex = r.uploadTexture(256, 256, noise.data(), true, true);
        terrainNoise_ = noiseTex;
        const TextureId sand = cache_->texture("story3_grnd"), grass2 = cache_->texture("Rstory1_ground");
        const TextureId rock2 = cache_->texture("story3_rock");
        r.setTerrainLayers(wall ? wall : ground, sand, grass2, rock2, noiseTex);
    }

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
    // Water and sky colours for the water surface's reflections (from the fog = horizon colour).
    env.seaLevel = 0.0f;
    env.skyHorizon = env.fogColor;
    env.skyZenith = Vec3{env.fogColor.x * 0.55f + 0.04f, env.fogColor.y * 0.65f + 0.06f, env.fogColor.z * 0.8f + 0.12f};
    env.waterColor = Vec3{env.fogColor.x * 0.10f + 0.01f, env.fogColor.y * 0.22f + 0.04f, env.fogColor.z * 0.22f + 0.05f};

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
    {
        // Water depth map for the water shader (clear shallows, foam on the shore, dark deep
        // water): the terrain's depth below the sea surface, 2 m texels, 0..24 m in a byte.
        const float x0 = bmin.x - 20.0f, z0 = bmin.z - 20.0f;
        const float wx = (bmax.x - bmin.x) + 40.0f, wz = (bmax.z - bmin.z) + 40.0f;
        const int nx = std::max(16, std::min(1024, int(wx / 2.0f))), nz = std::max(16, std::min(1024, int(wz / 2.0f)));
        std::vector<uint8_t> depth(size_t(nx) * size_t(nz), 255);
        for (int j = 0; j < nz; ++j)
            for (int i = 0; i < nx; ++i) {
                float h;
                const float x = x0 + (float(i) + 0.5f) * wx / float(nx), z = z0 + (float(j) + 0.5f) * wz / float(nz);
                if (terrain_.heightAt(x, z, h)) depth[size_t(j) * size_t(nx) + size_t(i)] = uint8_t(std::max(0.0f, std::min(1.0f, (env.seaLevel - h) / 24.0f)) * 255.0f + 0.5f);
            }
        r.setWaterDepthMap(nx, nz, depth.data(), x0, z0, wx, wz);
        LOGI("Water depth map %dx%d over %.0f x %.0f m", nx, nz, wx, wz);
    }

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
                v.uv[1] = k ? 0.01f : 0.99f;  // inset: the texture repeats, v=0 would blend with the bottom row (seam)
                v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
                band.vertices.push_back(v);
            }
        }
        for (int i = 0; i < slices; ++i) {
            uint32_t b = uint32_t(i * 2);
            band.indices.insert(band.indices.end(), {b, b + 2, b + 1, b + 1, b + 2, b + 3});
        }
        // Dome cap above the band: the band's top row of texels fading into the zenith colour
        // (vertex alpha), so the sky is closed instead of ending at 43 degrees.
        const uint32_t capBase = uint32_t(band.vertices.size());
        const float caps[3] = {hi, 1.15f, 1.5707f};
        const uint8_t alphas[3] = {255, 90, 0};
        for (int ring = 0; ring < 3; ++ring)
            for (int i = 0; i <= slices; ++i) {
                const float a = 2 * kPi * i / slices, el = caps[ring];
                Vertex v{};
                v.pos[0] = std::sin(a) * std::cos(el) * radius;
                v.pos[1] = std::sin(el) * radius;
                v.pos[2] = -std::cos(a) * std::cos(el) * radius;
                v.uv[0] = float(i) / slices;
                v.uv[1] = 0.01f;
                v.color[0] = v.color[1] = v.color[2] = 255;
                v.color[3] = alphas[ring];
                band.vertices.push_back(v);
            }
        const uint32_t row = uint32_t(slices + 1);
        for (int ring = 0; ring < 2; ++ring)
            for (int i = 0; i < slices; ++i) {
                const uint32_t a = capBase + uint32_t(ring) * row + uint32_t(i), b = a + row;
                band.indices.insert(band.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
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
    std::string hold;     // hangs from the mount point by this hand bone ("ref_lhand")
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

void AssetWorld::collectMissions(const std::string& worldName) {
    missions_.clear();
    for (const auto& sc : world_.scenarios)
        if (sc.kind == 2 && fs_.exists("wm_" + sc.name + ".bin")) missions_.push_back(sc.name);
    if (!missions_.empty()) return;
    // The world streams of Story2, Story3 and Story5 stop before their scenario list (an
    // undecoded opcode, B5): their missions are found by file name instead, in story order -
    // "<n><letter>_<k>_<name>" (2a_1_getmeat, 2b_1, 2b_2...) and "story<n>_<name>_<k>" (by k).
    const std::string w = toLower(worldName);
    if (w.rfind("story", 0) != 0 || w.size() != 6) {
        if (w == "the_end" && fs_.exists("wm_end.bin")) missions_.push_back("end");
    } else {
        const char n = w[5];
        std::vector<std::pair<std::string, std::string>> found;  // sort key, mission name
        for (const std::string& f : fs_.list(".bin")) {
            const std::string lf = toLower(f);
            if (lf.rfind("wm_", 0) != 0) continue;
            const std::string m = lf.substr(3, lf.size() - 7);
            if (m.size() > 2 && m[0] == n && m[1] >= 'a' && m[1] <= 'z' && m[2] == '_') {
                found.push_back({m, m});
            } else if (m.rfind(w + "_", 0) == 0) {
                const size_t us = m.find_last_of('_');
                const int k = us == std::string::npos ? 0 : std::atoi(m.c_str() + us + 1);
                char key[16];
                std::snprintf(key, sizeof key, "%04d", k);
                found.push_back({key, m});
            }
        }
        std::sort(found.begin(), found.end());
        for (auto& f : found) missions_.push_back(f.second);
    }
    for (const auto& m : missions_) world_.scenarios.push_back({2, m});  // so placeWorldObjects finds them
    if (!missions_.empty()) LOGI("World %s: %zu missions found by file name (scenario list not decoded)", worldName.c_str(), missions_.size());
}

std::string AssetWorld::nextStoryWorld(const std::string& world) {
    static const char* order[] = {"story1", "story2", "story3", "story4", "story5", "the_end"};
    const std::string w = toLower(world);
    for (size_t i = 0; i + 1 < sizeof(order) / sizeof(order[0]); ++i)
        if (w == order[i]) return i + 2 < sizeof(order) / sizeof(order[0]) ? "Story" + std::to_string(i + 2) : "the_end";
    return {};
}

bool AssetWorld::switchWorld(const std::string& name) {
    if (!renderer_) return false;
    Renderer& r = *renderer_;
    terrain_.release(r);
    if (skyMesh_) { r.destroyMesh(skyMesh_); skyMesh_ = 0; }
    if (seaMesh_) { r.destroyMesh(seaMesh_); seaMesh_ = 0; }
    world_ = WorldDefinition{};
    skinned_.clear();
    placed_.clear();
    hiddenTypes_.clear();
    worldName_ = name;
    hasTerrain_ = loadWorld(name, r);
    if (!hasTerrain_) {
        LOGW("World %s failed to load", name.c_str());
        return false;
    }
    collectMissions(name);
    placeWorldObjects(missions_.empty() ? std::string() : missions_.front());
    LOGI("Switched to world %s (%zu missions, first %s)", name.c_str(), missions_.size(),
         missions_.empty() ? "-" : missions_.front().c_str());
    return true;
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
    nests_.clear();
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
            float x = o.x, y = o.y, z = o.z;
            bool onPost = false;
            std::string hold;
            if (o.parent >= 0) {
                // Mounted (op 0x16) on another object's hardpoint: the parent model's reference
                // nodes in its own .anm ("Ref_guard01" in guard_tower1's crow's nest, "Ref_Lhand"
                // on mecc_crash1 where Tel hangs on). Mount byte 255 = the first, k = the k-th
                // (provisional: only 255 and 0 occur in Story1). Carried items (the jetpack) are
                // not shown yet.
                ++mounted;
                const std::string cm = registry_.modelName(o.type);
                const bool mecc = toLower(cm).rfind("mc_", 0) == 0;
                if ((!isCharacter(registry_, o.type, cm) && !mecc) || size_t(o.parent) >= mission_.objects.size()) continue;
                const MissionObject& par = mission_.objects[size_t(o.parent)];
                const std::string pm = registry_.modelName(par.type);
                const float ps = par.scale > 0 ? par.scale : typeScale(par.type);
                Placement pp{par.type, par.x, par.y, par.z, par.yaw, par.pitch, par.roll, ps};
                std::vector<uint8_t> ab;
                AnmAnimation pa;
                std::string pan = registry_.modelAnim(par.type);
                if (pan.empty()) pan = pm + ".anm";
                bool found = false;
                if (!pm.empty() && fs_.read(pan, ab) && parseAnm(ab.data(), ab.size(), pa)) {
                    std::vector<Mat4> w;
                    pa.evaluate(0.0f, w);
                    int want = o.mount[0] == 255 ? 0 : int(o.mount[0]), k = 0;
                    for (size_t n = 0; n < pa.nodes.size() && !found; ++n) {
                        if (toLower(pa.nodes[n].name).rfind("ref_", 0) != 0) continue;
                        if (k++ != want) continue;
                        const Vec3 lr = giantsToRuntime(w[n].m[12], w[n].m[13], w[n].m[14]);
                        const Vec4 r = placementMatrix(pp) * Vec4{lr.x, lr.y, lr.z, 1.0f};
                        x = -r.x / gGameUnitsToMeters;
                        y = r.z / gGameUnitsToMeters;
                        z = r.y / gGameUnitsToMeters;
                        hold = toLower(pa.nodes[n].name);
                        found = true;
                    }
                }
                if (!found) {
                    // No hardpoints: stand on top of the parent's bounds.
                    const RenderModel* m = pm.empty() ? nullptr : cache_->get(pm + ".gbs", registry_.modelAnim(par.type));
                    if (!m) continue;
                    z = par.z + m->boundsMax.y * ps / gGameUnitsToMeters;
                }
                onPost = true;
                LOGI("Mounted %s on %s at %s (%.0f %.0f %.0f)", cm.c_str(), pm.c_str(), found ? hold.c_str() : "top", x, y, z);
                // A hand-hold hardpoint: the character hangs from it by that hand (Tel in 1c_1).
                if (hold != "ref_lhand" && hold != "ref_rhand") hold.clear();
            }
            Placement pl{o.type, x, y, z, o.yaw, o.pitch, o.roll, o.scale > 0 ? o.scale : typeScale(o.type), true,
                         int(o.flags & 0x0F), onPost};
            pl.hold = hold;
            all.push_back(pl);
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
        if (p.mission && (isCharacter(registry_, p.type, name) || !p.hold.empty()) && fs_.exists(name + ".skn")) {
            // Animated by the game layer (AI or ambient loop), not baked into a static pose.
            const ObjectType* t = registry_.type(p.type);
            Vec3 pos = giantsToRuntime(p.x, p.y, p.z);
            float gy;
            if (!p.posted && hasTerrain_ && terrain_.heightAt(pos.x, pos.z, gy)) pos.y = gy;
            // A sentry stands on the highest walkable surface of its structure (the tower's
            // platform, below the roof tip the bounds give).
            characters_.push_back({p.type, name, t ? t->objectClass : -1, pos, p.yaw * kPi / 180.0f, p.scale, p.trigger, p.posted,
                                   p.hold});
            continue;
        }
        // Guard towers stand as their animation's first frame: the clip is their collapse (upright,
        // shaking, bending over), and the bind pose is the collapsed one.
        std::string skinAnim = registry_.modelAnim(p.type);
        if (toLower(name).find("guard_tower") != std::string::npos && !skinAnim.empty()) skinAnim += "@0";
        const RenderModel* m = cache_->get(name + ".gbs", skinAnim);
        if (!m) { ++missing; continue; }
        const Mat4 xf = placementMatrix(p);
        if (const ObjectType* ot = registry_.type(p.type); ot && ot->objectClass == 47) {
            // Ripper nests (class 47) are live: the game layer spawns from them, shrinks and
            // destroys them, so they are not static scenery.
            Vec3 at = giantsToRuntime(p.x, p.y, p.z);
            float gy;
            if (hasTerrain_ && terrain_.heightAt(at.x, at.z, gy)) at.y = gy;
            const float radius = 0.5f * length(m->boundsMax - m->boundsMin) * p.scale;
            nests_.push_back({p.type, m, Mat4::translation(at) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, p.yaw * kPi / 180.0f)) *
                                             Mat4::scale({p.scale, p.scale, p.scale}),
                              at, radius, ot->hitPoints > 0 ? ot->hitPoints : 100.0f});
            continue;
        }
        const Vec3 lc = (m->boundsMin + m->boundsMax) * 0.5f;
        const Vec4 wc = xf * Vec4{lc.x, lc.y, lc.z, 1.0f};
        const float radius = 0.5f * length(m->boundsMax - m->boundsMin) * p.scale;
        WorldObject wo;
        wo.model = m;
        wo.transform = wo.placement = xf;
        wo.centre = Vec3{wc.x, wc.y, wc.z};
        wo.radius = radius;
        wo.type = p.type;
        wo.name = toLower(name);
        wo.scale = p.scale;
        worldObjects_.push_back(wo);
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
            worldObjects_.back().collision = collision_.addObject(w, int(worldObjects_.size()) - 1);
        }
    }
    // Hang points: structures whose own .anm carries ref_smartie_NN nodes (w_story1_hang in 1b:
    // nine points on the rock pillars' arms, where the opening cutscene shows the Smarties).
    // Smarties waiting to be rescued (trigger 3) are hung there in order.
    {
        std::vector<Vec3> points;
        for (const auto& p : all) {
            if (!p.mission) continue;
            const std::string pm = registry_.modelName(p.type);
            if (pm.empty() || toLower(pm).find("hang") == std::string::npos) continue;
            std::vector<uint8_t> ab;
            AnmAnimation pa;
            std::string an = registry_.modelAnim(p.type);
            if (an.empty()) an = pm + ".anm";
            if (!fs_.read(an, ab) || !parseAnm(ab.data(), ab.size(), pa)) continue;
            std::vector<Mat4> w;
            pa.evaluate(0.0f, w);
            for (size_t n = 0; n < pa.nodes.size(); ++n) {
                if (toLower(pa.nodes[n].name).rfind("ref_smartie", 0) != 0) continue;
                const Vec3 lr = giantsToRuntime(w[n].m[12], w[n].m[13], w[n].m[14]);
                const Vec4 r = placementMatrix(p) * Vec4{lr.x, lr.y, lr.z, 1.0f};
                points.push_back({r.x, r.y, r.z});
            }
        }
        size_t k = 0;
        for (auto& c : characters_)
            if (c.trigger == 3 && c.objectClass == 18 && k < points.size()) {
                c.hang = points[k++];
                c.hasHang = true;
            }
        if (!points.empty())
            LOGI("Hang points: %zu, %zu Smarties hung; first at (%.1f %.1f %.1f)", points.size(), k, points[0].x, points[0].y, points[0].z);
    }
    LOGI("Level objects: %zu placed, %zu characters, %zu helpers skipped, %zu without a model", worldObjects_.size(),
         characters_.size(), hidden, missing);
    LOGI("Collision: %zu solid objects, %zu triangles", collision_.objectCount(), collision_.triangleCount());
}

void AssetWorld::renderShadowCasters(Renderer& r, const Vec3& centre, float radius) const {
    for (const auto& o : worldObjects_) {
        if (o.hidden || length(o.centre - centre) - o.radius > radius) continue;
        if (o.radius > 120.0f) continue;  // whole-island rock sets: their own shading is baked
        for (const auto& part : o.model->parts)
            if (!part.glass && part.blend != BlendMode::AlphaBlend) r.drawShadow(part.mesh, o.transform);
    }
}

void AssetWorld::update(float dt) {
    time_ += dt;
    for (auto& p : placed_) p.yaw += p.spin * dt;
    for (auto& s : skinned_) s.model->pose(*renderer_, time_);
}

void AssetWorld::render(Renderer& r, bool reflection) {
    if (skyMesh_) {
        DrawParams sp;
        sp.texture = skyTexture_;
        sp.unlit = true;
        sp.noFog = true;
        sp.doubleSided = true;
        sp.depthWrite = false;  // always behind everything
        sp.material = DrawParams::Material::Sky;
        r.draw(skyMesh_, Mat4::translation(r.currentView().eyePosition), sp);
    }
    if (hasTerrain_) terrain_.render(r);
    const Vec3 eye = r.currentView().eyePosition;
    const float maxDist = r.environment().fogFar;
    for (const auto& o : worldObjects_) {
        if (o.hidden) continue;
        if (!hiddenTypes_.empty() && std::find(hiddenTypes_.begin(), hiddenTypes_.end(), int(o.type)) != hiddenTypes_.end()) continue;
        // Cull objects outside the view frustum or fully fogged out. The reflection skips what
        // can't show in it: anything entirely under water, small things far away.
        const float dist = length(o.centre - eye) - o.radius;
        if (dist > maxDist || !r.sphereVisible(o.centre, o.radius) ||
            (reflection && (o.centre.y + o.radius < r.environment().seaLevel || o.radius < dist * 0.012f))) {
            r.countCulled();
            continue;
        }
        for (size_t pi = 0; pi < o.model->parts.size(); ++pi) {
            const auto& part = o.model->parts[pi];
            DrawParams dp;
            dp.texture = part.texture;
            if (o.damaged && pi < o.damagedTex.size() && o.damagedTex[pi]) dp.texture = o.damagedTex[pi];
            dp.blend = part.blend;
            if (part.glass) { dp.blend = BlendMode::AlphaBlend; dp.tint.w *= 0.3f; }
            dp.unlit = !part.lit;
            if (part.blend == BlendMode::AlphaTest && o.burn > 0.0f) {
                // Cut-out parts (leaves, fronds) can't take the overlay: they wither and blacken.
                const float b = std::min(1.0f, o.burn);
                dp.tint = {1.0f - 0.72f * b, 1.0f - 0.8f * b, 1.0f - 0.88f * b, dp.tint.w};
            }
            r.draw(part.mesh, o.transform, dp);
        }
        if (o.burn > 0.01f && burnTex_[0] && !reflection) {
            // Char spreading over the surface: the previous coverage level in full, the next one
            // fading in (same mesh again, depth-equal, alpha blended); then the embers.
            const float b = std::min(0.999f, o.burn) * 3.0f;
            const int level = int(b);
            const float frac = b - float(level);
            for (const auto& part : o.model->parts) {
                if (part.glass || part.blend != BlendMode::Opaque) continue;
                DrawParams dp;
                dp.blend = BlendMode::AlphaBlend;
                dp.depthWrite = false;
                dp.unlit = !part.lit;
                dp.doubleSided = false;
                if (level > 0) {
                    dp.texture = burnTex_[level - 1];
                    r.draw(part.mesh, o.transform, dp);
                }
                dp.texture = burnTex_[level];
                dp.tint.w = frac;
                r.draw(part.mesh, o.transform, dp);
                if (o.glow > 0.01f && emberTex_) {
                    DrawParams gp;
                    gp.texture = emberTex_;
                    gp.blend = BlendMode::Additive;
                    gp.depthWrite = false;
                    gp.unlit = true;
                    gp.noFog = true;
                    gp.tint = {o.glow, o.glow, o.glow, 1.0f};
                    r.draw(part.mesh, o.transform, gp);
                }
            }
        }
    }
    for (const auto& p : placed_) {
        Mat4 xf = Mat4::translation(p.position) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, p.yaw));
        for (const auto& part : p.model->parts) {
            DrawParams dp;
            dp.texture = part.texture;
            dp.blend = part.blend;
            if (part.glass) { dp.blend = BlendMode::AlphaBlend; dp.tint.w *= 0.3f; }
            // Baked vertex colours are the lighting for static models; characters are lit.
            dp.unlit = !part.lit;
            r.draw(part.mesh, xf, dp);
        }
    }
    for (const auto& s : skinned_)
        s.model->render(r, Mat4::translation(s.position) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, s.yaw)));
    if (seaMesh_ && !reflection) {
        DrawParams sp;
        sp.texture = seaTexture_;
        sp.unlit = true;
        sp.blend = BlendMode::AlphaBlend;
        sp.tint = {1, 1, 1, 1.0f};
        sp.doubleSided = true;
        sp.material = DrawParams::Material::Water;
        r.draw(seaMesh_, Mat4::identity(), sp);
    }
}

}  // namespace gvr
