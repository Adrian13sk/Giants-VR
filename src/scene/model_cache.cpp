#include "scene/model_cache.h"

#include <algorithm>
#include <map>

#include "assets/image.h"
#include "core/log.h"

namespace gvr {

// Provisional: 1 Giants unit = 0.3 m. See docs/formats/GBS.md for the evidence and status.
float gGameUnitsToMeters = 0.3f;

// Whether triangle winding must be reversed after the Z-up -> Y-up conversion so that front
// faces are counter-clockwise (GL convention). Determined from closed meshes; see tests.
static constexpr bool kFlipWinding = false;  // gzptool validate: 103/106 closed-ish meshes have positive volume without a flip

std::vector<BuiltPart> buildGbsParts(const GbsModel& m) {
    // Character models ship an all-zero RGB array; the original engine lights those
    // dynamically. Only treat RGBs as baked lighting when they contain data.
    bool bakedRgb = false;
    for (const auto& c : m.rgbs)
        if (c[0] | c[1] | c[2]) { bakedRgb = true; break; }
    std::map<std::pair<std::string, uint32_t>, size_t> partIndex;
    std::vector<BuiltPart> parts;
    for (const auto& so : m.subObjects) {
        const uint32_t blendFlags = so.flags & (GbsModel::kSubAdditive | GbsModel::kSubSubtractive);
        auto key = std::make_pair(toLower(so.texture), blendFlags);
        size_t pi;
        auto it = partIndex.find(key);
        if (it == partIndex.end()) {
            pi = parts.size();
            partIndex[key] = pi;
            parts.push_back({});
            parts[pi].texture = so.texture;
            parts[pi].subFlags = blendFlags;
            parts[pi].lit = !bakedRgb;
        } else {
            pi = it->second;
        }
        BuiltPart& part = parts[pi];
        std::unordered_map<uint32_t, uint32_t> remap;
        for (size_t t = 0; t + 2 < so.indices.size(); t += 3) {
            uint32_t tri[3] = {so.indices[t], so.indices[t + 1], so.indices[t + 2]};
            if (kFlipWinding) std::swap(tri[1], tri[2]);
            for (uint32_t di : tri) {
                auto r = remap.find(di);
                if (r != remap.end()) {
                    part.mesh.indices.push_back(r->second);
                    continue;
                }
                Vertex v{};
                const auto& p = m.positions[m.displayToPosition[di]];
                Vec3 rp = giantsToRuntime(p[0], p[1], p[2]);
                v.pos[0] = rp.x; v.pos[1] = rp.y; v.pos[2] = rp.z;
                if (!m.uvs.empty()) { v.uv[0] = m.uvs[di][0]; v.uv[1] = m.uvs[di][1]; }
                if (bakedRgb) {
                    v.color[0] = m.rgbs[di][0]; v.color[1] = m.rgbs[di][1]; v.color[2] = m.rgbs[di][2];
                } else {
                    v.color[0] = v.color[1] = v.color[2] = 255;
                }
                v.color[3] = 255;
                uint32_t ni = static_cast<uint32_t>(part.mesh.vertices.size());
                part.mesh.vertices.push_back(v);
                remap[di] = ni;
                part.mesh.indices.push_back(ni);
            }
        }
    }
    // Smooth vertex normals from face normals (used only when lighting is enabled).
    for (auto& part : parts) {
        auto& vs = part.mesh.vertices;
        std::vector<Vec3> acc(vs.size(), Vec3{0, 0, 0});
        const auto& ix = part.mesh.indices;
        for (size_t i = 0; i + 2 < ix.size(); i += 3) {
            Vec3 a{vs[ix[i]].pos[0], vs[ix[i]].pos[1], vs[ix[i]].pos[2]};
            Vec3 b{vs[ix[i + 1]].pos[0], vs[ix[i + 1]].pos[1], vs[ix[i + 1]].pos[2]};
            Vec3 c{vs[ix[i + 2]].pos[0], vs[ix[i + 2]].pos[1], vs[ix[i + 2]].pos[2]};
            Vec3 n = cross(b - a, c - a);
            for (int k = 0; k < 3; ++k) acc[ix[i + k]] += n;
        }
        for (size_t i = 0; i < vs.size(); ++i) {
            Vec3 n = normalize(acc[i]);
            vs[i].normal[0] = n.x; vs[i].normal[1] = n.y; vs[i].normal[2] = n.z;
        }
    }
    return parts;
}

TextureId ModelCache::texture(const std::string& name, bool* hadAlpha) {
    std::string key = toLower(name);
    auto it = textures_.find(key);
    if (it != textures_.end()) {
        if (hadAlpha) *hadAlpha = it->second.alpha;
        return it->second.id;
    }
    std::vector<uint8_t> bytes;
    TexEntry e{0, false};
    const char* exts[] = {".tga", ".bmp", ""};
    for (const char* ext : exts) {
        if (fs_.read(name + ext, bytes)) break;
        bytes.clear();
    }
    if (!bytes.empty()) {
        Image img;
        std::string err;
        if (decodeImage(bytes.data(), bytes.size(), img, &err)) {
            e.id = renderer_.uploadTexture(img.width, img.height, img.rgba.data(), true, true);
            e.alpha = img.hadAlpha;
        } else {
            LOGW("Texture %s: decode failed: %s", name.c_str(), err.c_str());
        }
    } else {
        LOGW("Texture %s not found", name.c_str());
    }
    textures_[key] = e;
    if (hadAlpha) *hadAlpha = e.alpha;
    return e.id;
}

const RenderModel* ModelCache::get(const std::string& gbsName) {
    std::string key = toLower(gbsName);
    auto it = models_.find(key);
    if (it != models_.end()) return it->second.get();

    std::vector<uint8_t> bytes;
    if (!fs_.read(gbsName, bytes)) {
        LOGW("Model %s not found", gbsName.c_str());
        models_[key] = nullptr;
        return nullptr;
    }
    GbsModel gbs;
    std::string err;
    if (!parseGbs(bytes.data(), bytes.size(), gbs, &err)) {
        LOGW("Model %s: %s", gbsName.c_str(), err.c_str());
        models_[key] = nullptr;
        return nullptr;
    }
    auto rm = std::make_unique<RenderModel>();
    rm->name = gbsName;
    rm->boundsMin = {1e30f, 1e30f, 1e30f};
    rm->boundsMax = {-1e30f, -1e30f, -1e30f};
    for (auto& bp : buildGbsParts(gbs)) {
        RenderModel::Part part;
        part.textureName = bp.texture;
        part.lit = bp.lit;
        part.triangles = static_cast<uint32_t>(bp.mesh.indices.size() / 3);
        for (const auto& v : bp.mesh.vertices) {
            rm->boundsMin = {std::min(rm->boundsMin.x, v.pos[0]), std::min(rm->boundsMin.y, v.pos[1]),
                             std::min(rm->boundsMin.z, v.pos[2])};
            rm->boundsMax = {std::max(rm->boundsMax.x, v.pos[0]), std::max(rm->boundsMax.y, v.pos[1]),
                             std::max(rm->boundsMax.z, v.pos[2])};
        }
        bool alpha = false;
        if (!bp.texture.empty()) {
            part.texture = texture(bp.texture, &alpha);
            if (!part.texture) ++rm->missingTextures;
        }
        // Sub-object flag 0x10/0x20 is the 3ds Max transparency *type*, not a blend switch
        // (opaque characters carry 0x10), so blending is driven by the texture alpha for now.
        if (alpha) part.blend = BlendMode::AlphaTest;
        part.mesh = renderer_.uploadMesh(bp.mesh);
        rm->triangles += part.triangles;
        rm->parts.push_back(part);
    }
    LOGI("Model %s: %zu parts, %u tris, %u missing textures", gbsName.c_str(), rm->parts.size(),
         rm->triangles, rm->missingTextures);
    const RenderModel* out = rm.get();
    models_[key] = std::move(rm);
    return out;
}

}  // namespace gvr
