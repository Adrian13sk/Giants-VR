#include "assets/fx_defs.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace gvr {

namespace {
// u = u32, f = f32, b = byte, s = NUL-terminated name. Derived from the data (see the header).
struct Spec { uint8_t op; const char* args; };
const Spec kSpecs[] = {
    {0x00, ""}, {0x02, ""}, {0x03, "u"}, {0x04, "fff"}, {0x05, "ffffff"}, {0x06, ""}, {0x07, ""}, {0x08, "ff"},
    {0x09, "ffff"}, {0x0a, "usu"}, {0x0b, "f"}, {0x0c, "ff"}, {0x0d, "s"}, {0x0e, "f"}, {0x0f, "ff"}, {0x10, "usu"},
    {0x11, "f"}, {0x12, "ff"}, {0x13, "ff"}, {0x14, "fff"}, {0x15, ""}, {0x16, ""}, {0x17, "f"}, {0x18, "ff"},
    {0x19, "f"}, {0x1b, "f"}, {0x1c, "ff"}, {0x1d, "sf"}, {0x1e, "sff"}, {0x20, "ffffff"}, {0x21, ""}, {0x22, "ff"},
    {0x24, "uuu"}, {0x25, "f"}, {0x26, "f"}, {0x27, "sfu"}, {0x28, "sffff"}, {0x29, "uf"}, {0x2a, "u"}, {0x2b, "uu"},
    {0x2c, "u"}, {0x2e, "f"}, {0x30, "f"}, {0x32, "fff"}, {0x33, "ffffff"}, {0x34, "ff"}, {0x35, "ffff"}, {0x36, "ff"},
    {0x37, "ffff"}, {0x38, "u"}, {0x39, "uuf"}, {0x3a, "ff"}, {0x3c, "f"}, {0x3d, "ffffff"}, {0x3e, ""}, {0x40, "ff"},
    {0x41, "fffffffffff"}, {0x42, "f"}, {0x46, "fffff"}, {0x47, "u"}, {0x48, ""}, {0x49, ""}, {0x4a, ""}, {0x53, "ffffff"},
    {0x55, ""}, {0x56, "sfff"}, {0x57, "sffuu"}, {0x58, ""}, {0x59, ""}, {0x5a, "su"}, {0x5b, "sfff"}, {0x5c, "sffffff"},
    {0x5f, "f"}, {0x67, "f"}, {0x69, "f"}, {0x6f, "ff"}, {0x70, "usu"}, {0x71, "sfff"}, {0x74, "ffffff"}, {0x76, "bsff"},
    {0x78, "bsff"}, {0x79, "ff"}, {0x7a, "sffffff"}, {0x7c, "uuuuu"}, {0x80, "ff"}, {0x81, "uuuuu"}, {0x83, "uu"}, {0x84, ""},
    {0x85, ""}, {0x87, "uu"}, {0x88, "ffff"},
};

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
}  // namespace

const char* fxOpSpec(uint8_t op) {
    for (const auto& s : kSpecs)
        if (s.op == op) return s.args;
    return nullptr;
}

const FxDef* FxLibrary::find(const std::string& name) const {
    auto it = byName.find(lower(name));
    return it == byName.end() ? nullptr : &effects[it->second];
}

bool parseFxLibrary(const uint8_t* d, size_t size, FxLibrary& out, std::string* err) {
    out = {};
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (size < 12 || rd32(d) != 1) return fail("not an fx library (version)");
    if (size_t(rd32(d + 4)) + 12 < size || size_t(rd32(d + 4)) > size) return fail("size field does not match the file");
    const uint32_t count = rd32(d + 8);
    if (count == 0 || count > 100000) return fail("bad effect count");
    // Directory.
    struct Entry { std::string name; size_t off; };
    std::vector<Entry> dir;
    size_t o = 12;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = static_cast<const uint8_t*>(std::memchr(d + o, 0, size - o));
        if (!e || size_t(e - d) + 5 > size) return fail("directory runs past the end");
        Entry en;
        en.name.assign(reinterpret_cast<const char*>(d + o), size_t(e - (d + o)));
        en.off = 12 + size_t(rd32(e + 1));
        o = size_t(e - d) + 5;
        if (en.off >= size) return fail("effect '" + en.name + "' starts past the end");
        dir.push_back(en);
    }
    // Each effect runs to the next one's start (or the end of the file).
    std::vector<size_t> starts;
    for (const auto& e : dir) starts.push_back(e.off);
    std::sort(starts.begin(), starts.end());
    for (const auto& e : dir) {
        const size_t end = [&] {
            auto it = std::upper_bound(starts.begin(), starts.end(), e.off);
            return it == starts.end() ? size : *it;
        }();
        FxDef def;
        def.name = e.name;
        size_t p = e.off;
        while (p < end) {
            FxOp op;
            op.op = d[p++];
            const char* spec = fxOpSpec(op.op);
            if (!spec) return fail("effect '" + e.name + "': unknown op " + std::to_string(op.op));
            for (const char* c = spec; *c; ++c) {
                if (*c == 'u' || *c == 'f') {
                    if (p + 4 > end) return fail("effect '" + e.name + "': op runs past its end");
                    if (*c == 'u') op.u.push_back(rd32(d + p));
                    else { float f; std::memcpy(&f, d + p, 4); op.f.push_back(f); }
                    p += 4;
                } else if (*c == 'b') {
                    if (p + 1 > end) return fail("effect '" + e.name + "': op runs past its end");
                    op.b = d[p++];
                } else {  // 's'
                    const uint8_t* z = static_cast<const uint8_t*>(std::memchr(d + p, 0, end - p));
                    if (!z) return fail("effect '" + e.name + "': unterminated name");
                    op.s.assign(reinterpret_cast<const char*>(d + p), size_t(z - (d + p)));
                    p = size_t(z - d) + 1;
                }
            }
            def.ops.push_back(op);
            if (op.op == 0x02 && p == end) break;
        }
        if (p != end) return fail("effect '" + e.name + "' does not end where the next begins");
        out.byName[lower(def.name)] = out.effects.size();
        out.effects.push_back(std::move(def));
    }
    return true;
}

}  // namespace gvr
