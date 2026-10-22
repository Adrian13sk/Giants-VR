#include "assets/gtext.h"

#include <cstring>

#include "assets/gzp.h"

namespace gvr {

bool GameText::parse(const uint8_t* d, size_t n, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    if (n < 12) return fail("too short");
    uint32_t cnt, kb, vb;
    std::memcpy(&cnt, d, 4);
    std::memcpy(&kb, d + 4, 4);
    std::memcpy(&vb, d + 8, 4);
    const size_t keys = 12 + 8 * size_t(cnt), vals = keys + kb;
    if (cnt > 1000000 || vals + vb > n) return fail("bad sizes");
    auto str = [&](size_t base, size_t limit, uint32_t off) {
        size_t p = base + off, e = p;
        while (e < limit && d[e]) ++e;
        return p < limit ? std::string(reinterpret_cast<const char*>(d + p), e - p) : std::string();
    };
    for (uint32_t i = 0; i < cnt; ++i) {
        uint32_t ko, vo;
        std::memcpy(&ko, d + 12 + 8 * i, 4);
        std::memcpy(&vo, d + 16 + 8 * i, 4);
        text_[toLower(str(keys, vals, ko))] = str(vals, vals + vb, vo);
    }
    return true;
}

std::string GameText::get(const std::string& key) const {
    auto it = text_.find(toLower(key));
    return it == text_.end() ? std::string() : it->second;
}

}  // namespace gvr
