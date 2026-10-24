#include "assets/weapondefs.h"

#include <cmath>
#include <cstring>

namespace gvr {

namespace {
uint32_t u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
float f32(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }
std::string cstr(const uint8_t* d, size_t size, uint32_t off) {
    if (off == 0 || off >= size) return {};
    size_t e = off;
    while (e < size && d[e]) ++e;
    return std::string(reinterpret_cast<const char*>(d + off), e - off);
}
}  // namespace

const WeaponDef* WeaponDefs::weapon(int id) const {
    for (const auto& w : weapons)
        if (w.id == id) return &w;
    return nullptr;
}

const ItemDef* WeaponDefs::item(int id) const {
    for (const auto& i : items)
        if (i.id == id) return &i;
    return nullptr;
}

const WeaponDef* WeaponDefs::weaponByFx(const char* fx, float interval) const {
    for (const auto& w : weapons)
        if (w.fxProjectile == fx && (interval < 0 || std::fabs(w.interval - interval) < 1e-3f)) return &w;
    return nullptr;
}

bool parseWeaponDefs(const uint8_t* d, size_t size, WeaponDefs& out, std::string* err) {
    out = {};
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (size < 8 || u32(d) != 1) return fail("not a weapondefs file (version)");
    if (u32(d + 4) != size) return fail("size field does not match the file");
    // Pass 1: records as {key, values} lists.
    struct KV { uint32_t key, n; const uint8_t* v; };
    std::vector<std::vector<KV>> recs(1);
    size_t o = 8;
    while (o + 8 <= size) {
        const uint32_t key = u32(d + o), type = u32(d + o + 4);
        if (key == 2 && type == 2) { recs.emplace_back(); o += 8; continue; }
        if (type < 2 || type > 34) break;  // the string pool
        const uint32_t n = type - 2;
        if (o + 8 + 4 * size_t(n) > size) return fail("record runs past the end");
        recs.back().push_back({key, n, d + o + 8});
        o += 8 + 4 * size_t(n);
    }
    if (recs.size() < 2) return fail("no records");
    auto str = [&](const KV& kv) { return kv.n ? cstr(d, size, u32(kv.v)) : std::string(); };
    // Weapons: every record but the last.
    for (size_t r = 0; r + 1 < recs.size(); ++r) {
        WeaponDef w;
        for (const auto& kv : recs[r]) {
            if (!kv.n) continue;
            switch (kv.key) {
                case 1: w.id = int(u32(kv.v)); break;
                case 3: w.projectileType = int(u32(kv.v)); break;
                case 9: for (uint32_t k = 0; k < 4 && k < kv.n; ++k) w.splash[k] = f32(kv.v + 4 * k); break;
                case 10: w.damage = f32(kv.v); break;
                case 13: w.interval = f32(kv.v); break;
                case 24: w.speed = f32(kv.v); break;  // repeated in one record (43): the last wins
                case 34: w.range = f32(kv.v); break;
                case 22: w.fxProjectile = str(kv); break;
                case 23: w.fxImpact = str(kv); break;
                case 53: w.fxSplash = str(kv); break;
                case 29: w.fxNozzle = str(kv); break;
                default: break;
            }
        }
        if (w.id >= 0) out.weapons.push_back(w);
    }
    // Items: runs in the last record, each opened by key 36.
    ItemDef* cur = nullptr;
    for (const auto& kv : recs.back()) {
        if (kv.key == 36 && kv.n) { out.items.push_back({}); cur = &out.items.back(); cur->id = int(u32(kv.v)); continue; }
        if (!cur || !kv.n) continue;
        const int v = int(int32_t(u32(kv.v)));
        switch (kv.key) {
            case 39: cur->objectType = v; break;
            case 41: cur->pickupType = v; break;
            case 42: cur->ammoItem = v; break;
            case 45: cur->maxAmmo = v; break;
            case 46: cur->ammoPerPickup = v; break;
            case 47: cur->category = v; break;
            case 54: cur->race = v; break;
            case 48: cur->icon = str(kv); break;
            default: break;
        }
    }
    if (out.weapons.empty() || out.items.empty()) return fail("no weapons or no items");
    return true;
}

}  // namespace gvr
