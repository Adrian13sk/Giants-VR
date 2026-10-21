#include "assets/world_bin.h"

#include <cstring>

#include "assets/binreader.h"

namespace gvr {

namespace {
// Total record size including the opcode byte. 0 = unknown (stop), -1 = terminator,
// -2 = variable (handled in code).
int recordSize(uint8_t op) {
    switch (op) {
        case 19: return 2;  case 23: return 5;  case 29: case 30: case 31: case 32: case 33: case 34: return 17;
        case 35: return -1; case 36: case 37: case 38: return 17; case 39: return 18; case 40: return 4;
        case 41: return 12; case 42: return 21; case 43: return 5; case 44: return -1; case 45: return 38;
        case 46: return 13; case 47: return 29; case 48: return 19; case 49: return 41; case 50: return 6;
        case 52: case 53: case 54: return 45; case 57: return 29; case 58: return 37; case 59: return 2;
        case 60: return 17; case 61: return -1; case 62: return 21; case 63: return 9; case 64: return 26;
        case 65: return 9; case 66: return 93; case 67: return -1; case 68: case 69: return 1;
        case 70: return 29; case 71: return 21; case 72: return 13; case 73: return 5; case 74: return 1;
        case 76: return 7; case 77: return 1; case 78: return 34; case 79: return 65; case 80: return -2;
        case 81: return 13; case 82: case 83: return 5; case 84: return 45; case 88: return 12;
        case 89: return -2; case 90: case 91: return 13; case 92: return 38; case 93: case 94: return 33;
        case 95: return 13; case 96: return 5; case 97: case 98: return 9; case 99: case 100: return 1;
        case 101: return 13; case 102: return 9; case 103: case 104: return -2; case 106: return 4;
        case 107: return 5; case 108: case 109: case 110: return -1; case 111: return 65; case 112: return -1;
        case 113: return 34; case 114: case 115: case 116: return 45; case 117: return 5; case 118: return -2;
        case 119: case 120: return 49; case 121: return 93; case 129: return 5; case 130: return 33;
        case 132: return -2; case 133: case 134: case 135: case 136: return 45; case 137: case 138: return 46;
        default: return 0;
    }
}

std::string strAt(const uint8_t* p, size_t maxLen) {
    const char* c = reinterpret_cast<const char*>(p);
    return std::string(c, strnlen(c, maxLen));
}
}  // namespace

bool parseWorldBin(const uint8_t* data, size_t size, WorldDefinition& w, std::string* error) {
    BinReader r(data, size);
    w = WorldDefinition{};
    if (r.u32() != 0x1A0002E5u) {
        if (error) *error = "world bin: bad magic";
        return false;
    }
    uint32_t sect[7];
    for (auto& s : sect) s = r.u32();
    r.seek(sect[0]);
    const uint32_t len = r.u32();
    if (!r.ok() || r.pos() + len > size) {
        if (error) *error = "world bin: bad logic section";
        return false;
    }
    const uint8_t* s = data + r.pos();
    w.streamSize = len;
    size_t p = 0;
    auto readStrz = [&](std::string& out) {
        size_t e = p;
        while (e < len && s[e]) ++e;
        out.assign(reinterpret_cast<const char*>(s + p), e - p);
        p = e + 1;
    };
    readStrz(w.header1);
    readStrz(w.header2);
    w.terrainName = w.header2;

    while (p < len) {
        const uint8_t op = s[p];
        if (op == 0xFF) { w.complete = true; w.stopOpcode = 0xFF; ++p; break; }
        int sz = recordSize(op);
        if (sz == -1) { w.stopOpcode = op; ++p; w.complete = (p >= len - 1); break; }
        if (sz == 0) { w.stopOpcode = op; break; }
        if (sz == -2) {
            if (op == 80) sz = p + 1 < len ? 2 + 4 * s[p + 1] : 0;
            else if (op == 103 || op == 104) {
                uint32_t n = 0;
                if (p + 5 <= len) std::memcpy(&n, s + p + 1, 4);
                sz = n > 64 ? 0 : 5 + 4 * int(n);
            } else {  // 89, 132, 118: NUL-terminated string (+20 bytes for 118)
                size_t e = p + 1;
                while (e < len && s[e]) ++e;
                sz = int(e + 1 - p) + (op == 118 ? 20 : 0);
            }
            if (sz == 0) { w.stopOpcode = op; break; }
        }
        if (p + size_t(sz) > len) { w.stopOpcode = op; break; }
        const uint8_t* pl = s + p + 1;
        switch (op) {
            case 30: w.skyTexture = strAt(pl, 16); break;
            case 37: w.seaTexture = strAt(pl, 16); break;
            case 38: w.glowTexture = strAt(pl, 16); break;
            case 52: w.groundTexture = strAt(pl, 32); break;
            case 53: w.wallTexture1 = strAt(pl, 32); break;
            case 54: w.wallTexture2 = strAt(pl, 32); break;
            case 40: std::memcpy(w.ambientRGB, pl, 3); break;
            case 41:
                std::memcpy(&w.fogNear, pl, 4);
                std::memcpy(&w.fogFar, pl + 4, 4);
                std::memcpy(w.fogRGB, pl + 8, 3);
                w.hasFog = true;
                break;
            case 88:
                std::memcpy(&w.waterFogNear, pl, 4);
                std::memcpy(&w.waterFogFar, pl + 4, 4);
                std::memcpy(w.waterFogRGB, pl + 8, 3);
                break;
            case 92: w.scenarios.push_back({pl[0], strAt(pl + 5, 32)}); break;
            case 42: {
                WorldObjectRecord o;
                std::memcpy(&o.type, pl, 4);
                std::memcpy(&o.x, pl + 4, 4);
                std::memcpy(&o.y, pl + 8, 4);
                std::memcpy(&o.z, pl + 12, 4);
                std::memcpy(&o.angle, pl + 16, 4);
                w.objects.push_back(o);
                break;
            }
            default: break;
        }
        p += size_t(sz);
        ++w.recordsParsed;
    }
    if (p >= len) w.complete = true;
    w.streamConsumed = p;
    return true;
}

}  // namespace gvr
