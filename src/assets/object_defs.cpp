#include "assets/object_defs.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>

#include "assets/gzp.h"

namespace gvr {

namespace {
uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
float rdf(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

// Bounds-checked cursor over the definition blob.
struct Cursor {
    const uint8_t* b;
    size_t n;
    bool ok = true;
    bool has(size_t p, size_t k) const { return p <= n && k <= n - p; }
    uint32_t u32(size_t p) { if (!has(p, 4)) { ok = false; return 0; } return rd32(b + p); }
    float f32(size_t p) { if (!has(p, 4)) { ok = false; return 0; } return rdf(b + p); }
    uint8_t u8(size_t p) { if (!has(p, 1)) { ok = false; return 0; } return b[p]; }
    // Returns the offset just past the NUL of the string at p.
    size_t strz(size_t p, std::string* out = nullptr) {
        size_t e = p;
        while (e < n && b[e]) ++e;
        if (e >= n) { ok = false; return n; }
        if (out) out->assign(reinterpret_cast<const char*>(b + p), e - p);
        return e + 1;
    }
};

// Type-op sizes (bytes including the op) whose cursor advance is fixed in the original loader.
// 'z' = op + f32 + strz; 's' = op + strz. 0 = not known (walking stops there).
int typeOpSize(uint8_t op) {
    switch (op) {
        case 0x14: case 0x16: case 0x18: case 0x1B: case 0x1D: case 0x20: case 0x22: case 0x23:
        case 0x24: case 0x3F: case 0x43: case 0x27: case 0x2B: case 0x2E: case 0x2F:
            return 1;
        case 0x13: case 0x1E: case 0x36:
            return 2;
        case 0x01: case 0x05: case 0x06: case 0x07: case 0x09: case 0x0A: case 0x0B: case 0x0D:
        case 0x15: case 0x17: case 0x19: case 0x1A: case 0x25: case 0x29: case 0x30: case 0x31:
        case 0x32: case 0x37: case 0x3E: case 0x3B: case 0x45: case 0x69:
            return 5;
        case 0x04: case 0x08: case 0x1C: case 0x21: case 0x26: case 0x33: case 0x34: case 0x35:
            return 9;
        case 0x03: case 0x0C: case 0x38: case 0x42:
            return 13;
        case 0x1F: case 0x28: case 0x3C:
            return 17;
        case 0x02: return -1;  // 'z'
        case 0x2C: case 0x2D: return -2;  // 's'
        default: return 0;
    }
}
}  // namespace

size_t ObjectRegistry::build(const std::string& binDir) {
    DIR* dir = opendir(binDir.c_str());
    if (!dir) return 0;
    std::vector<std::string> files;
    while (dirent* de = readdir(dir)) {
        std::string nm = de->d_name;
        if (nm.size() > 4 && toLower(nm.substr(nm.size() - 4)) == ".bin") files.push_back(nm);
    }
    closedir(dir);
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        std::ifstream in(binDir + "/" + f, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!d.empty()) addDefinitionFile(d.data(), d.size());
    }
    return types_.size();
}

bool ObjectRegistry::addDefinitionFile(const uint8_t* d, size_t n, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    if (n < 32 || rd32(d) != 0x1A0002E5u) return fail("not a 0x1A0002E5 file");
    const uint32_t off = rd32(d + 4 + 3 * 4);
    if (off > n - 4) return fail("bad section offset");
    const uint32_t size = rd32(d + off);
    if (size == 0) return true;  // no definitions
    if (size > n - off - 4) return fail("section overruns file");
    Cursor c{d + off + 4, size};

    size_t p = 0;
    auto table = [&](std::vector<uint32_t>& out) {
        const uint32_t k = c.u32(p);
        p += 4;
        if (!c.ok || k > size / 4) { c.ok = false; return; }
        out.resize(k);
        for (uint32_t i = 0; i < k; ++i) out[i] = c.u32(p + 4 * i);
        p += 4 * size_t(k);
    };
    std::vector<uint32_t> meshOffs, xOffs, groupOffs, typeOffs;
    table(meshOffs);
    for (uint32_t o : meshOffs) {
        std::string name;
        const uint32_t id = c.u32(o);
        c.strz(o + 4, &name);
        if (!c.ok) return fail("bad mesh entry");
        meshNames_[id] = name;
    }
    table(xOffs);
    table(groupOffs);
    if (!c.ok) return fail("bad tables");
    for (uint32_t o : groupOffs) {
        ObjectGroup g;
        const uint32_t gid = c.u32(o);
        size_t q = o + 4;
        for (;;) {
            const uint8_t op = c.u8(q++);
            if (!c.ok) return fail("group runs past section");
            if (op == 0xFF) break;
            switch (op) {
                case 0x51:  // LOD: u32 mesh, f32 angle, f32 distance
                    g.lodMeshes.push_back(c.u32(q));
                    q += 12;
                    break;
                case 0x59: case 0x5A: case 0x5B:  // special LOD slots (collision etc.)
                    q += 4;
                    break;
                case 0x52: {  // animation: u8 slot, strz file
                    const int slot = c.u8(q);
                    std::string a;
                    q = c.strz(q + 1, &a);
                    g.anims[slot] = toLower(a);
                    break;
                }
                case 0x53: q += 17; break;
                case 0x54: q += 22; break;
                case 0x55: break;
                case 0x56: q += 8; break;
                case 0x57: case 0x58: case 0x5C: q += 12; break;
                case 0x7F: case 0x80:  // timed fx event: u32 tick, 5 strz, u32
                    q += 4;
                    for (int i = 0; i < 5; ++i) q = c.strz(q);
                    q += 4;
                    break;
                default:
                    return fail("unknown group op");
            }
        }
        groups_[gid] = std::move(g);
    }
    table(typeOffs);
    if (!c.ok) return fail("bad type table");
    for (uint32_t o : typeOffs) {
        ObjectType t;
        const uint32_t tid = c.u32(o);
        size_t q = o + 4;
        for (int i = 0; i < 64 && c.ok; ++i) {
            const uint8_t op = c.u8(q);
            if (op == 0x01) { t.group = c.u32(q + 1); t.hasGroup = true; }
            else if (op == 0x1E) t.objectClass = c.u8(q + 1);
            else if (op == 0x17) t.scale = c.f32(q + 1);
            const int sz = typeOpSize(op);
            if (sz == 0) break;
            if (sz == -1) q = c.strz(q + 5);
            else if (sz == -2) q = c.strz(q + 1);
            else q += size_t(sz);
        }
        if (!c.ok) return fail("type runs past section");
        types_[tid] = t;
    }
    return true;
}

const ObjectType* ObjectRegistry::type(uint32_t id) const {
    auto t = types_.find(id);
    return t == types_.end() ? nullptr : &t->second;
}

const ObjectGroup* ObjectRegistry::group(uint32_t id) const {
    auto g = groups_.find(id);
    return g == groups_.end() ? nullptr : &g->second;
}

std::string ObjectRegistry::modelName(uint32_t typeId) const {
    const ObjectType* t = type(typeId);
    if (!t || !t->hasGroup) return {};
    const ObjectGroup* g = group(t->group);
    if (!g || g->lodMeshes.empty()) return {};
    auto m = meshNames_.find(g->lodMeshes[0]);
    return m == meshNames_.end() ? std::string() : m->second;
}

std::string ObjectRegistry::modelAnim(uint32_t typeId) const {
    const ObjectType* t = type(typeId);
    if (!t || !t->hasGroup) return {};
    const ObjectGroup* g = group(t->group);
    if (!g || g->anims.empty()) return {};
    auto a = g->anims.find(1);
    return a != g->anims.end() ? a->second : g->anims.begin()->second;
}

std::string ObjectRegistry::anim(uint32_t typeId, int slot) const {
    const ObjectType* t = type(typeId);
    if (!t || !t->hasGroup) return {};
    const ObjectGroup* g = group(t->group);
    if (!g) return {};
    auto a = g->anims.find(slot);
    return a == g->anims.end() ? std::string() : a->second;
}

bool isInvisibleHelper(const std::string& name) {
    const std::string l = toLower(name);
    static const char* prefixes[] = {"marker", "sphere_coll", "box_coll", "ref_", "a_", "light", "coll_",
                                     "trigger", "spline", "camera", "dummy", "null"};
    for (const char* p : prefixes)
        if (l.rfind(p, 0) == 0) return true;
    return l.find("_coll") != std::string::npos;
}

}  // namespace gvr
