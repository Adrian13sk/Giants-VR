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
bool printableChar(uint8_t c) { return c >= 0x20 && c <= 0x7e; }
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
    finalize();
    return typeToGroup_.size();
}

void ObjectRegistry::addDefinitionFile(const uint8_t* d, size_t n) {
    // Pass 1: mesh/group entries, verified by the LOD record that repeats the mesh id.
    size_t i = 4;
    while (i < n) {
        if (!printableChar(d[i]) || printableChar(d[i - 1])) { ++i; continue; }
        size_t e = i;
        while (e < n && printableChar(d[e]) && e - i < 48) ++e;
        if (e >= n || d[e] != 0 || e - i < 2) { i = e + 1; continue; }
        const uint32_t meshId = rd32(d + i - 4);
        if (meshId > 0 && meshId < 100000) {
            for (size_t pad = 1; pad <= 4; ++pad) {
                const size_t g = e + pad;
                if (g + 9 > n) break;
                if (d[g + 4] == 0x51 && rd32(d + g + 5) == meshId) {
                    meshNames_[meshId] = std::string(reinterpret_cast<const char*>(d + i), e - i);
                    groupMesh_.emplace(rd32(d + g), meshId);
                    break;
                }
            }
        }
        i = e + 1;
    }
    pending_.emplace_back(d, d + n);
}

void ObjectRegistry::finalize() {
    // Pass 2: type records {typeId, 0x01, groupId} at a record boundary (0xFF within the three
    // preceding bytes, or three zero bytes at the start of the stream).
    for (const auto& f : pending_) {
        const uint8_t* d = f.data();
        const size_t n = f.size();
        for (size_t s = 3; s + 9 <= n; ++s) {
            if (d[s + 4] != 0x01) continue;
            const uint32_t group = rd32(d + s + 5);
            if (!groupMesh_.count(group)) continue;
            const uint32_t type = rd32(d + s);
            if (type == 0 || type >= 100000) continue;
            const bool boundary = d[s - 1] == 0xFF || d[s - 2] == 0xFF || d[s - 3] == 0xFF ||
                                  (d[s - 1] == 0 && d[s - 2] == 0 && d[s - 3] == 0);
            if (boundary) typeToGroup_.emplace(type, group);
        }
    }
    pending_.clear();
    pending_.shrink_to_fit();
}

std::string ObjectRegistry::modelName(uint32_t type) const {
    auto t = typeToGroup_.find(type);
    if (t == typeToGroup_.end()) return {};
    auto g = groupMesh_.find(t->second);
    if (g == groupMesh_.end()) return {};
    auto m = meshNames_.find(g->second);
    return m == meshNames_.end() ? std::string() : m->second;
}

std::vector<ObjectPlacement> scanMissionPlacements(const uint8_t* d, size_t n, const ObjectRegistry& reg,
                                                   const float b[4]) {
    std::vector<ObjectPlacement> out;
    for (size_t i = 0; i + 21 <= n; ++i) {
        if (d[i] != 0x03) continue;
        ObjectPlacement p;
        p.type = rd32(d + i + 1);
        if (!reg.knows(p.type)) continue;
        p.x = rdf(d + i + 5);
        p.y = rdf(d + i + 9);
        p.z = rdf(d + i + 13);
        p.yawDegrees = rdf(d + i + 17);
        if (!(p.x >= b[0] && p.x <= b[1] && p.y >= b[2] && p.y <= b[3] && p.z > -500 && p.z < 3000)) continue;
        if (!(p.yawDegrees > -10000 && p.yawDegrees < 10000)) continue;
        out.push_back(p);
        i += 20;  // records do not overlap
    }
    return out;
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
