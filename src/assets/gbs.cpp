#include "assets/gbs.h"

#include <cstring>

#include "assets/binreader.h"

namespace gvr {

size_t GbsModel::triangleCount() const {
    size_t n = 0;
    for (const auto& s : subObjects) n += s.indices.size() / 3;
    return n;
}

bool parseGbs(const uint8_t* data, size_t size, GbsModel& m, std::string* error) {
    BinReader r(data, size);
    auto fail = [&](const std::string& msg) {
        if (error) *error = "GBS: " + msg + " (at offset " + std::to_string(r.pos()) + ")";
        return false;
    };
    m = GbsModel{};
    uint32_t magic = r.u32();
    if (magic != GbsModel::kMagic) return fail("bad magic");
    m.flags = r.u32();
    const uint32_t nPos = r.u32();
    if (!r.ok() || nPos > 1000000) return fail("bad vertex count");
    m.positions.resize(nPos);
    for (auto& p : m.positions) { p[0] = r.f32(); p[1] = r.f32(); p[2] = r.f32(); }

    if (m.flags & GbsModel::kFlagNormals) {
        r.u32();  // number of normal definitions
        const uint32_t words = r.u32();
        if (!r.ok() || words > 4000000) return fail("bad normal table");
        m.normalDefs.resize(words);
        for (auto& w : m.normalDefs) w = r.u16();
    }

    const uint32_t nDisp = r.u32();
    if (!r.ok() || nDisp > 1000000) return fail("bad display vertex count");
    m.displayToPosition.resize(nDisp);
    for (auto& v : m.displayToPosition) {
        v = r.u16();
        if (v >= nPos) return fail("display vertex references missing position");
    }
    if (m.flags & GbsModel::kFlagNormals) {
        m.displayNormal.resize(nDisp);
        for (auto& v : m.displayNormal) v = r.u16();
    }
    if (m.flags & GbsModel::kFlagUVs) {
        m.uvs.resize(nDisp);
        for (auto& uv : m.uvs) { uv[0] = r.f32(); uv[1] = r.f32(); }
    }
    if (m.flags & GbsModel::kFlagRGBs) {
        m.rgbs.resize(nDisp);
        for (auto& c : m.rgbs) { c[0] = r.u8(); c[1] = r.u8(); c[2] = r.u8(); }
    }

    const uint32_t nMax = r.u32();
    if (!r.ok() || nMax > 65536) return fail("bad max-object count");
    m.maxObjs.resize(nMax);
    int64_t vsum = 0;
    for (auto& mo : m.maxObjs) {
        mo.vstart = r.i32(); mo.vcount = r.i32(); mo.nstart = r.i32();
        mo.ncount = r.i32(); mo.noffset = r.i32();
        vsum += mo.vcount;
    }
    if (vsum != static_cast<int64_t>(nPos)) return fail("max-object vertex counts do not sum to vertex count");

    const uint32_t nSub = r.u32();
    if (!r.ok() || nSub > 65536) return fail("bad sub-object count");
    m.subObjects.resize(nSub);
    for (auto& so : m.subObjects) {
        so.name = r.fixedString(32);
        so.maxObjIndex = r.u32();
        const int32_t totalTris = r.i32();
        const int32_t nWords = r.i32();
        if (!r.ok() || nWords < 0 || nWords > 4000000 || so.maxObjIndex >= nMax)
            return fail("bad sub-object header");
        std::vector<uint16_t> words(static_cast<size_t>(nWords));
        for (auto& w : words) w = r.u16();
        // Triangle runs: [count, (a,b,c) x count] repeated; stops at count 0 or end of data.
        size_t p = 0;
        while (p < words.size()) {
            const uint16_t count = words[p++];
            if (count == 0) break;
            if (p + size_t(count) * 3 > words.size()) return fail("triangle run overflows data");
            for (uint16_t t = 0; t < count; ++t) {
                for (int k = 0; k < 3; ++k) {
                    uint16_t idx = words[p++];
                    if (idx >= nDisp) return fail("triangle index out of range");
                    so.indices.push_back(idx);
                }
            }
        }
        if (static_cast<int32_t>(so.indices.size() / 3) != totalTris)
            return fail("triangle count mismatch in sub-object " + so.name);
        so.vstart = r.i32();
        so.vcount = r.i32();
        if (m.flags & GbsModel::kFlagUVs) {
            so.texture = r.fixedString(32);
            so.texture2 = r.fixedString(32);
        }
        so.falloff = r.f32();
        if (m.flags & GbsModel::kFlagRGBs) so.blend = r.f32();
        so.flags = r.u32();
        so.emissive = r.u32();
        so.ambient = r.u32();
        so.diffuse = r.u32();
        so.specular = r.u32();
        so.power = r.f32();
        if (!r.ok()) return fail("truncated sub-object");
    }
    if (!r.ok()) return fail("truncated file");
    if (r.remaining() != 0) return fail("unexpected trailing bytes: " + std::to_string(r.remaining()));
    return true;
}

}  // namespace gvr
