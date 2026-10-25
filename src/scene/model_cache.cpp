#include "scene/model_cache.h"

#include <cmath>
#include <cstdlib>

#include <algorithm>
#include <map>

#include "assets/image.h"
#include "assets/skin_anim.h"
#include "core/log.h"

namespace gvr {

// Provisional: 1 Giants unit = 0.2 m, chosen so the animated Meccaryn (10.5 units) is ~2.1 m
// tall; Smartie ~0.8 m, Reaper ~2.2 m, Kabuto ~16 m. Unverified against the original engine.
float gGameUnitsToMeters = 0.2f;

// Whether triangle winding must be reversed after the Z-up -> Y-up conversion so that front
// faces are counter-clockwise (GL convention). Determined from closed meshes; see tests.
static constexpr bool kFlipWinding = false;  // gzptool validate: 103/106 closed-ish meshes have positive volume without a flip

std::vector<BuiltPart> buildGbsParts(const GbsModel& m, bool ignoreRgb) {
    std::map<std::pair<std::string, uint32_t>, size_t> partIndex;
    std::vector<BuiltPart> parts;
    for (const auto& so : m.subObjects) {
        // Baked lighting is per sub-object: many models (rafts, canopies, barracks, palm
        // leaves, characters) ship all-zero RGB for some or all sub-objects; those are lit
        // dynamically instead of rendering black.
        // Sub-objects whose RGB is parametric skin data (see looksLikeSkinData) are lit too.
        bool bakedRgb = false;
        if (!ignoreRgb && !m.rgbs.empty()) {
            const size_t b = std::min(size_t(std::max(so.vstart, 0)), m.rgbs.size());
            const size_t e = std::min(size_t(std::max(so.vstart + so.vcount, 0)), m.rgbs.size());
            for (size_t i = b; i < e; ++i)
                if (m.rgbs[i][0] | m.rgbs[i][1] | m.rgbs[i][2]) { bakedRgb = true; break; }
            if (bakedRgb && (looksLikeSkinData(&m.rgbs[b], e - b) || looksLikeEncodedVectors(&m.rgbs[b], e - b)))
                bakedRgb = false;
        }
        const uint32_t blendFlags = so.flags & (GbsModel::kSubAdditive | GbsModel::kSubSubtractive | GbsModel::kSubGlass);
        auto key = std::make_pair(toLower(so.texture), blendFlags | (bakedRgb ? 0u : 0x80000000u));
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
                part.posIndex.push_back(m.displayToPosition[di]);
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

bool looksLikeSkinData(const std::array<uint8_t, 3>* rgb, size_t n) {
    // Skinned buildings export parametric RGB instead of lighting: blue is only 0 or 254
    // (side of the wall) and red ramps across the full range (position along the piece).
    if (n == 0) return false;
    int rmin = 255, rmax = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& c = rgb[i];
        if (c[2] > 2 && c[2] < 253) return false;
        rmin = std::min<int>(rmin, c[0]);
        rmax = std::max<int>(rmax, c[0]);
    }
    return rmax - rmin > 200;
}

bool looksLikeEncodedVectors(const std::array<uint8_t, 3>* rgb, size_t n) {
    // Some models (Smartie domes, Mecc bunkers: blocker B8) carry RGB that decodes as
    // c * 2/255 - 1 to roughly unit-length vectors (normals?) and is strongly coloured, giving a
    // rainbow when used as lighting. Baked lighting in the shipped data is close to grey (mean
    // saturation < 0.26 on every model checked, except the raft whose RGB never decodes to unit
    // vectors). Provisional rule: mean saturation > 0.4 and >= 10% unit-length vectors.
    if (n < 4) return false;
    float sat = 0;
    size_t unit = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& c = rgb[i];
        sat += float(std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]})) / 255.0f;
        float l2 = 0;
        for (int k = 0; k < 3; ++k) {
            const float v = float(c[k]) * (2.0f / 255.0f) - 1.0f;
            l2 += v * v;
        }
        if (l2 >= 0.85f * 0.85f && l2 <= 1.12f * 1.12f) ++unit;
    }
    return sat / float(n) > 0.4f && float(unit) >= 0.1f * float(n);
}

bool bakeDefaultSkinPose(const AssetFS& fs, const std::string& base, const std::string& animIn, GbsModel& g) {
    // "<file>.anm@0": the animation's first frame instead of the nodes' default transforms.
    std::string anim = animIn;
    const bool frame0 = anim.size() > 2 && anim.compare(anim.size() - 2, 2, "@0") == 0;
    if (frame0) anim.resize(anim.size() - 2);
    std::vector<uint8_t> bytes;
    std::string err;
    SknModel skn;
    AnmAnimation an;
    if (!fs.read(base + ".skn", bytes) || !parseSkn(bytes.data(), bytes.size(), skn, &err)) return false;
    if (!fs.read(anim, bytes) || !parseAnm(bytes.data(), bytes.size(), an, &err)) {
        LOGW("Skin pose %s: %s unavailable %s", base.c_str(), anim.c_str(), err.c_str());
        return false;
    }
    // The exported GBS positions of skinned buildings are a distorted copy; the intended shape
    // is the skin evaluated with the nodes' default transforms (keys = build/collapse effects).
    if (!frame0)
        for (auto& n : an.nodes) { n.posKeys.clear(); n.rotKeys.clear(); n.scaleKeys.clear(); }
    std::vector<Mat4> world;
    an.evaluate(0.0f, world);
    // The skin must match the mesh exactly (object count and per-object vertex counts).
    if (skn.objects.size() != g.maxObjs.size()) return false;
    for (size_t o = 0; o < g.maxObjs.size(); ++o)
        if (int32_t(skn.objects[o].vertexCount) != g.maxObjs[o].vcount) return false;
    size_t bound = 0;
    const size_t nObj = g.maxObjs.size();
    for (size_t o = 0; o < nObj; ++o) {
        for (const SknCluster& c : skn.objects[o].clusters) {
            auto bn = skn.boneNames.find(c.bone);
            if (bn == skn.boneNames.end()) continue;
            auto ni = an.byName.find(toLower(bn->second));
            if (ni == an.byName.end()) continue;
            for (const auto& v : c.verts) {
                const size_t pi = size_t(g.maxObjs[o].vstart) + v.index;
                if (pi >= g.positions.size()) continue;
                const Vec4 w = world[size_t(ni->second)] * Vec4{v.x, v.y, v.z, 1.0f};
                g.positions[pi] = {w.x, w.y, w.z};
                ++bound;
            }
        }
    }
    if (bound == 0) LOGW("Skin pose %s: no vertices bound", base.c_str());
    return bound > 0;
}

namespace {
// Rework: the original textures are small (mostly 64-256 px) and look soft up close in VR. Each one
// up to 256 px is doubled (Catmull-Rom) and its colour sharpened (unsharp mask, alpha left as is),
// so the extra texels carry crisper detail into the GPU's filtering. GVR_UPSCALE=0 turns it off.
float cubicW(float x) {
    x = std::fabs(x);
    if (x < 1.0f) return 1.5f * x * x * x - 2.5f * x * x + 1.0f;
    if (x < 2.0f) return -0.5f * x * x * x + 2.5f * x * x - 4.0f * x + 2.0f;
    return 0.0f;
}
}  // namespace

void upscaleSharpen(Image& img) {
    static const bool off = [] { const char* e = std::getenv("GVR_UPSCALE"); return e && e[0] == '0'; }();
    if (off || img.width < 8 || img.height < 8 || img.width > 256 || img.height > 256) return;
    const int w = img.width, h = img.height, W = w * 2, H = h * 2;
    // The two output phases of a 2x Catmull-Rom (samples at +-0.25 texel): fixed 4-tap weights.
    float k0[4], k1[4];
    // Output 2x samples at x - 0.25 from taps x-2..x+1 (distances 1.75, 0.75, -0.25, -1.25); output
    // 2x+1 at x + 0.25 from taps x-1..x+2 (1.25, 0.25, -0.75, -1.75). (Fixed: k0 was one tap off, so
    // every other output row/column came from the wrong place - the ground looked blocky.)
    for (int i = 0; i < 4; ++i) { k0[i] = cubicW(1.75f - float(i)); k1[i] = cubicW(1.25f - float(i)); }
    // Horizontal pass: w x h -> W x h; then vertical: W x h -> W x H (separable, wrapping).
    std::vector<float> hor(size_t(W) * h * 4), up(size_t(W) * H * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) {
                float a = 0, b = 0;
                for (int i = 0; i < 4; ++i) {
                    // Output 2x reads around x - 0.25 (taps x-2..x+1), 2x+1 around x + 0.25 (taps x-1..x+2).
                    const int xa = ((x - 2 + i) % w + w) % w, xb = ((x - 1 + i) % w + w) % w;
                    a += k0[i] * float(img.rgba[size_t(y * w + xa) * 4 + size_t(c)]);
                    b += k1[i] * float(img.rgba[size_t(y * w + xb) * 4 + size_t(c)]);
                }
                hor[size_t(y * W + 2 * x) * 4 + size_t(c)] = a;
                hor[size_t(y * W + 2 * x + 1) * 4 + size_t(c)] = b;
            }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 4; ++c) {
                float a = 0, b = 0;
                for (int i = 0; i < 4; ++i) {
                    const int ya = ((y - 2 + i) % h + h) % h, yb = ((y - 1 + i) % h + h) % h;
                    a += k0[i] * hor[size_t(ya * W + x) * 4 + size_t(c)];
                    b += k1[i] * hor[size_t(yb * W + x) * 4 + size_t(c)];
                }
                up[size_t(2 * y * W + x) * 4 + size_t(c)] = a;
                up[size_t((2 * y + 1) * W + x) * 4 + size_t(c)] = b;
            }
    // Unsharp mask on colour (blur [1 2 1] x [1 2 1], separable); alpha as upscaled.
    std::vector<float> bh(size_t(W) * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c) {
                const int xl = (x - 1 + W) % W, xr = (x + 1) % W;
                bh[size_t(y * W + x) * 3 + size_t(c)] = 0.25f * (up[size_t(y * W + xl) * 4 + size_t(c)] + up[size_t(y * W + xr) * 4 + size_t(c)]) +
                                                        0.5f * up[size_t(y * W + x) * 4 + size_t(c)];
            }
    std::vector<uint8_t> out(size_t(W) * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int yu = (y - 1 + H) % H, yd = (y + 1) % H;
            for (int c = 0; c < 4; ++c) {
                const float v = up[size_t(y * W + x) * 4 + size_t(c)];
                float r = v;
                if (c < 3) {
                    const float blur = 0.25f * (bh[size_t(yu * W + x) * 3 + size_t(c)] + bh[size_t(yd * W + x) * 3 + size_t(c)]) +
                                       0.5f * bh[size_t(y * W + x) * 3 + size_t(c)];
                    r = v + 0.45f * (v - blur);
                }
                out[size_t(y * W + x) * 4 + size_t(c)] = uint8_t(std::max(0.0f, std::min(255.0f, r + 0.5f)));
            }
        }
    img.width = W;
    img.height = H;
    img.rgba.swap(out);
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
            upscaleSharpen(img);
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

bool ModelCache::image(const std::string& name, Image& out) const {
    std::vector<uint8_t> bytes;
    const char* exts[] = {".tga", ".bmp", ""};
    for (const char* ext : exts) {
        if (fs_.read(name + ext, bytes)) break;
        bytes.clear();
    }
    return !bytes.empty() && decodeImage(bytes.data(), bytes.size(), out, nullptr);
}

const RenderModel* ModelCache::get(const std::string& gbsName, const std::string& skinAnim, bool dynamicLit) {
    std::string key = toLower(gbsName) + "|" + toLower(skinAnim) + (dynamicLit ? "|lit" : "");
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
    bool skinned = false;
    std::string base = gbsName;
    if (base.size() > 4) base.resize(base.size() - 4);
    if (!skinAnim.empty()) skinned = bakeDefaultSkinPose(fs_, base, skinAnim, gbs);
    // Meshes exported from a skinned scene without a usable skin of their own (e.g.
    // smw_wall_hi_hole; borrowing smw_wall_hi's skin was tried and its vertex order differs):
    // keep the stored shape but never show the parametric RGB as colour.
    const bool ignoreRgb = dynamicLit || skinned || looksLikeSkinData(gbs.rgbs.data(), gbs.rgbs.size());
    auto rm = std::make_unique<RenderModel>();
    rm->name = gbsName;
    rm->boundsMin = {1e30f, 1e30f, 1e30f};
    rm->boundsMax = {-1e30f, -1e30f, -1e30f};
    for (auto& bp : buildGbsParts(gbs, ignoreRgb)) {
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
        if (bp.subFlags & GbsModel::kSubGlass) part.glass = true;
        if (!alpha)  // alpha-tested parts are foliage/grilles: not solid
            for (uint32_t ix : bp.mesh.indices) {
                const auto& v = bp.mesh.vertices[ix];
                rm->collisionTris.push_back(Vec3{v.pos[0], v.pos[1], v.pos[2]});
            }
        part.mesh = renderer_.uploadMesh(bp.mesh);
        rm->triangles += part.triangles;
        rm->parts.push_back(part);
    }
    LOGI("Model %s%s: %zu parts, %u tris, %u missing textures", gbsName.c_str(),
         skinned ? " (skin rest pose)" : "", rm->parts.size(), rm->triangles, rm->missingTextures);
    const RenderModel* out = rm.get();
    std::stable_partition(rm->parts.begin(), rm->parts.end(), [](const RenderModel::Part& q) { return !q.glass; });
    models_[key] = std::move(rm);
    return out;
}

}  // namespace gvr
