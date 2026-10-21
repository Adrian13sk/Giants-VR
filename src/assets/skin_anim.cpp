#include "assets/skin_anim.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "assets/gzp.h"

namespace gvr {

namespace {

struct Img {
    const uint8_t* d;
    size_t n;
    bool ok = true;
    uint32_t u32(size_t off) {
        if (off + 4 > n) { ok = false; return 0; }
        uint32_t v;
        std::memcpy(&v, d + off, 4);
        return v;
    }
    uint16_t u16(size_t off) {
        if (off + 2 > n) { ok = false; return 0; }
        uint16_t v;
        std::memcpy(&v, d + off, 2);
        return v;
    }
    float f32(size_t off) {
        uint32_t u = u32(off);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    std::string str(size_t off, size_t maxLen) {
        if (off >= n) { ok = false; return {}; }
        const char* c = reinterpret_cast<const char*>(d + off);
        return std::string(c, strnlen(c, std::min(maxLen, n - off)));
    }
};

bool printable(const std::string& s) {
    if (s.empty() || s[0] == ' ') return false;
    for (unsigned char c : s)
        if (c < 32 || c > 126) return false;  // bone names may contain spaces ("arm R")
    return true;
}

// NAM table: consecutive {u16 entrySize, u16 type, u32 ptr, char name[]} entries. Its start
// varies between files (0x20 in sm_L0.skn, 0x28 after an 8-byte table header in kB_L1.skn),
// so the longest valid chain starting in the first 0x80 bytes is used.
struct NamEntry { std::string name; uint32_t ptr; };
std::vector<NamEntry> readNam(Img& im, uint32_t type) {
    std::vector<NamEntry> best;
    for (size_t start = 0x14; start < 0x80 && start + 8 <= im.n; start += 4) {
        std::vector<NamEntry> out;
        size_t p = start;
        while (p + 8 <= im.n) {
            const uint16_t sz = im.u16(p), ty = im.u16(p + 2);
            if (ty != type || sz < 9 || sz > 128 || p + sz > im.n) break;
            const uint32_t ptr = im.u32(p + 4);
            std::string nm = im.str(p + 8, sz - 8);
            if (ptr == 0 || ptr >= im.n || !printable(nm)) break;
            out.push_back({nm, ptr});
            p += sz;
        }
        if (out.size() > best.size()) best = std::move(out);
    }
    im.ok = true;
    return best;
}

Quat stdQuat(float x, float y, float z, float w) { return normalize(Quat{-x, -y, -z, w}); }

}  // namespace

bool parseSkn(const uint8_t* data, size_t size, SknModel& out, std::string* error) {
    Img im{data, size};
    out = SknModel{};
    auto fail = [&](const std::string& m) { if (error) *error = "SKN: " + m; return false; };
    if (size < 0x20 || im.u32(0) != size) return fail("bad header");
    auto nam = readNam(im, 4);
    const size_t namEnd = 0x20;
    if (nam.empty()) return fail("no skin objects");
    for (const auto& e : nam) {
        SknObject o;
        o.name = e.name;
        o.vertexCount = im.u32(e.ptr + 0x0C);
        const uint32_t nClusters = im.u32(e.ptr + 0x10);
        if (!im.ok || nClusters > 4096 || o.vertexCount > 200000) return fail("bad object " + e.name);
        for (uint32_t c = 0; c < nClusters; ++c) {
            const uint32_t cp = im.u32(e.ptr + 0x14 + 4 * c);
            SknCluster cl;
            cl.bone = im.u32(cp + 8);
            const uint32_t count = im.u32(cp + 36);
            if (!im.ok || count > 200000) return fail("bad cluster");
            cl.verts.resize(count);
            for (uint32_t k = 0; k < count; ++k) {
                const size_t vp = cp + 40 + size_t(k) * 16;
                cl.verts[k] = {im.f32(vp), im.f32(vp + 4), im.f32(vp + 8), im.u32(vp + 12)};
                if (cl.verts[k].index >= o.vertexCount) return fail("cluster vertex index out of range");
            }
            o.clusters.push_back(std::move(cl));
        }
        out.objects.push_back(std::move(o));
    }
    // Bone name table: entries {u16 size, u16 8, u32, u32 0, char name[], u16 kind (1,2,9 seen), u16 boneIndex}.
    for (size_t p = namEnd; p + 20 <= size; p += 4) {
        const uint16_t sz = im.u16(p), ty = im.u16(p + 2);
        if (ty != 8 || sz < 20 || sz > 64 || p + sz > size) continue;
        if (im.u32(p + 8) != 0 || im.u16(p + sz - 4) == 0 || im.u16(p + sz - 4) > 255) continue;
        std::string nm = im.str(p + 12, sz - 16);
        if (!printable(nm)) continue;
        out.boneNames[im.u16(p + sz - 2)] = nm;
    }
    for (const auto& o : out.objects)
        for (const auto& c : o.clusters)
            if (!out.boneNames.count(c.bone)) return fail("cluster references unknown bone " + std::to_string(c.bone));
    return im.ok;
}

bool parseAnm(const uint8_t* data, size_t size, AnmAnimation& a, std::string* error) {
    Img im{data, size};
    a = AnmAnimation{};
    auto fail = [&](const std::string& m) { if (error) *error = "ANM: " + m; return false; };
    if (size < 0x20 || im.u32(0) != size) return fail("bad header");
    auto nam = readNam(im, 4);
    if (nam.empty()) return fail("no nodes");

    // Animation header: referenced by every node at +0x0C.
    const uint32_t hdr = im.u32(nam[0].ptr + 0x0C);
    if (im.u32(hdr + 4) != 0xB) return fail("unsupported ANM version");
    a.startTick = static_cast<int32_t>(im.u32(hdr + 0x10));
    a.endTick = static_cast<int32_t>(im.u32(hdr + 0x14));
    a.ticksPerFrame = im.u32(hdr + 0x18);
    a.fps = im.u32(hdr + 0x1C);

    std::unordered_map<uint32_t, int> byPtr;
    for (size_t i = 0; i < nam.size(); ++i) byPtr[nam[i].ptr] = int(i);
    a.nodes.resize(nam.size());
    for (size_t i = 0; i < nam.size(); ++i) {
        const uint32_t p = nam[i].ptr;
        AnmNode& n = a.nodes[i];
        n.name = nam[i].name;
        const uint32_t parentPtr = im.u32(p + 0x10);
        auto it = byPtr.find(parentPtr);
        n.parent = (parentPtr && it != byPtr.end()) ? it->second : -1;
        n.pos = {im.f32(p + 0x6C), im.f32(p + 0x70), im.f32(p + 0x74)};
        n.rot = stdQuat(im.f32(p + 0x78), im.f32(p + 0x7C), im.f32(p + 0x80), im.f32(p + 0x84));
        n.scale = {im.f32(p + 0x88), im.f32(p + 0x8C), im.f32(p + 0x90)};
        for (int t = 0; t < 3; ++t) {
            const uint32_t tp = im.u32(p + 0x98 + 4 * t);
            if (!tp) continue;
            const uint8_t keyType = tp < size ? data[tp] : 0;
            const uint32_t count = im.u32(tp + 8);
            if (!im.ok || count > 100000) return fail("bad track");
            for (uint32_t k = 0; k < count; ++k) {
                const uint32_t kp = im.u32(tp + 12 + 4 * k);
                const int32_t tick = static_cast<int32_t>(im.u32(kp));
                // Value offsets: TCB verified (vec3 +48, quat +24); LIN/BEZ assumed +4.
                if (t == 1) {
                    size_t vo;
                    if (keyType == 7) vo = 24;
                    else if (keyType == 8 || keyType == 9) vo = 4;
                    else { ++a.unsupportedTracks; break; }
                    n.rotKeys.push_back({tick, stdQuat(im.f32(kp + vo), im.f32(kp + vo + 4), im.f32(kp + vo + 8),
                                                       im.f32(kp + vo + 12))});
                } else {
                    size_t vo;
                    if (keyType == 4) vo = 48;
                    else if (keyType == 5 || keyType == 6) vo = 4;
                    else { ++a.unsupportedTracks; break; }
                    AnmKeyVec3 key{tick, {im.f32(kp + vo), im.f32(kp + vo + 4), im.f32(kp + vo + 8)}};
                    (t == 0 ? n.posKeys : n.scaleKeys).push_back(key);
                }
            }
        }
        a.byName[toLower(n.name)] = int(i);
    }
    if (!im.ok) return fail("truncated");
    return true;
}

namespace {
template <typename K>
size_t findSegment(const std::vector<K>& keys, float tick, float& u) {
    if (tick <= keys.front().tick) { u = 0; return 0; }
    if (tick >= keys.back().tick) { u = 0; return keys.size() - 1; }
    size_t hi = 1;
    while (hi < keys.size() && keys[hi].tick < tick) ++hi;
    const float t0 = float(keys[hi - 1].tick), t1 = float(keys[hi].tick);
    u = t1 > t0 ? (tick - t0) / (t1 - t0) : 0.0f;
    return hi - 1;
}

Vec3 sampleVec(const std::vector<AnmKeyVec3>& keys, float tick, const Vec3& def) {
    if (keys.empty()) return def;
    float u;
    size_t i = findSegment(keys, tick, u);
    if (u == 0 || i + 1 >= keys.size()) return keys[i].value;
    return keys[i].value * (1 - u) + keys[i + 1].value * u;
}

Quat slerp(Quat a, Quat b, float u) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f)
        return normalize(Quat{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u, a.w + (b.w - a.w) * u});
    const float th = std::acos(d), s = std::sin(th);
    const float wa = std::sin((1 - u) * th) / s, wb = std::sin(u * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

Quat sampleQuat(const std::vector<AnmKeyQuat>& keys, float tick, const Quat& def) {
    if (keys.empty()) return def;
    float u;
    size_t i = findSegment(keys, tick, u);
    if (u == 0 || i + 1 >= keys.size()) return keys[i].value;
    return slerp(keys[i].value, keys[i + 1].value, u);
}
}  // namespace

void AnmAnimation::evaluate(float seconds, std::vector<Mat4>& world) const {
    const float tps = float(ticksPerFrame) * float(fps);
    const float span = float(endTick - startTick);
    float tick = float(startTick);
    if (span > 0 && tps > 0) tick += std::fmod(std::max(0.0f, seconds) * tps, span);
    std::vector<Mat4> local(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const AnmNode& n = nodes[i];
        // Linear position / slerp rotation between keys. The TCB tangents stored in the file
        // are not used yet (approximation; motion is slightly less smooth than the original).
        local[i] = Mat4::trs(sampleVec(n.posKeys, tick, n.pos), sampleQuat(n.rotKeys, tick, n.rot),
                             sampleVec(n.scaleKeys, tick, n.scale));
    }
    world.assign(nodes.size(), Mat4::identity());
    std::vector<uint8_t> state(nodes.size(), 0);
    std::function<const Mat4&(size_t)> get = [&](size_t i) -> const Mat4& {
        if (state[i] == 2) return world[i];
        if (state[i] == 1) { world[i] = local[i]; state[i] = 2; return world[i]; }  // cycle guard
        state[i] = 1;
        const int p = nodes[i].parent;
        world[i] = p >= 0 ? get(size_t(p)) * local[i] : local[i];
        state[i] = 2;
        return world[i];
    };
    for (size_t i = 0; i < nodes.size(); ++i) get(i);
}

}  // namespace gvr
