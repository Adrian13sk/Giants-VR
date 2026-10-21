#include "assets/mission.h"

#include <cstring>

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
std::string name32(const uint8_t* p) {
    size_t k = 0;
    while (k < 32 && p[k]) ++k;
    return std::string(reinterpret_cast<const char*>(p), k);
}
}  // namespace

bool parseMission(const uint8_t* d, size_t n, MissionData& out, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    out = MissionData{};
    if (n < 5 || rd32(d) != 4) return fail("bad mission version");
    size_t p = 4;
    int cur = -1;
    std::vector<int> stack;
    auto need = [&](size_t k) { return p <= n && k <= n - p; };
    while (true) {
        if (!need(1)) return fail("missing terminator");
        const uint8_t op = d[p++];
        auto obj = [&]() -> MissionObject* { return cur >= 0 ? &out.objects[size_t(cur)] : nullptr; };
        // Fixed payload size per op (counted lists handled below).
        size_t sz = 0;
        switch (op) {
            case 0x01: case 0x02: case 0x17: case 0x1C: case 0x1D: case 0x22: case 0x24: case 0x25: sz = 0; break;
            case 0x05: case 0x08: case 0x20: sz = 1; break;
            case 0x07: case 0x09: case 0x0C: case 0x0D: case 0x0F: case 0x10: case 0x11: case 0x12:
            case 0x21: case 0x23: sz = 4; break;
            case 0x16: sz = 6; break;
            case 0x1A: case 0x1B: case 0x26: sz = 8; break;
            case 0x06: case 0x18: sz = 12; break;
            case 0x0B: sz = 16; break;
            case 0x03: sz = 20; break;
            case 0x04: sz = 28; break;
            case 0x13: case 0x14: case 0x15: case 0x19: sz = 32; break;
            case 0x0A: {  // u8 n + n * u32
                if (!need(1)) return fail("truncated");
                sz = 1 + 4 * size_t(d[p]);
                break;
            }
            case 0x0E: case 0x1E: case 0x1F: {  // u32 n + n * u32
                if (!need(4)) return fail("truncated");
                const uint32_t k = rd32(d + p);
                if (k > n) return fail("bad list length");
                sz = 4 + 4 * size_t(k);
                break;
            }
            default:
                return fail("unknown mission op");
        }
        if (!need(sz)) return fail("truncated");
        const uint8_t* a = d + p;
        // Ops that modify the current object require one.
        const bool objOp = op == 0x05 || op == 0x06 || op == 0x07 || op == 0x08 || op == 0x09 || op == 0x0A ||
                           op == 0x14 || op == 0x16 || op == 0x18 || (op >= 0x1A && op <= 0x21);
        if (objOp && cur < 0) return fail("object op without an object");
        switch (op) {
            case 0x02:
                p += sz;
                if (p != n) return fail("data after terminator");
                return true;
            case 0x03:
            case 0x04: {
                MissionObject o;
                o.type = rd32(a);
                o.x = rdf(a + 4);
                o.y = rdf(a + 8);
                o.z = rdf(a + 12);
                o.yaw = rdf(a + 16);
                if (op == 0x04) {
                    o.pitch = rdf(a + 20);
                    o.roll = rdf(a + 24);
                }
                out.objects.push_back(std::move(o));
                cur = int(out.objects.size()) - 1;
                stack.clear();
                break;
            }
            case 0x05: obj()->team = a[0]; break;
            case 0x08: obj()->flags |= a[0] & 0x0F; break;
            case 0x09: obj()->scale = rdf(a); break;
            case 0x0B: for (int i = 0; i < 4; ++i) out.setup[i] = rd32(a + 4 * i); break;
            case 0x0E: {
                const uint32_t k = rd32(a);
                for (uint32_t i = 0; i < k; ++i) out.items.push_back(rd32(a + 4 + 4 * i));
                break;
            }
            case 0x13: out.flick13 = name32(a); break;
            case 0x14: obj()->flicks.push_back(name32(a)); break;
            case 0x15: out.startFlick = name32(a); break;
            case 0x19: out.flick19 = name32(a); break;
            case 0x16: {
                // A second object mounted on the current one; becomes current until op 0x17.
                const MissionObject& parent = *obj();
                MissionObject o;
                o.type = rd32(a);
                o.x = parent.x;
                o.y = parent.y;
                o.z = parent.z;
                o.yaw = parent.yaw;
                o.parent = cur;
                o.mount[0] = a[4];
                o.mount[1] = a[5];
                stack.push_back(cur);
                out.objects.push_back(std::move(o));
                cur = int(out.objects.size()) - 1;
                break;
            }
            case 0x17:
                if (stack.empty()) return fail("mount stack underflow");
                cur = stack.back();
                stack.pop_back();
                break;
            case 0x18:
                obj()->yaw = rdf(a);
                obj()->pitch = rdf(a + 4);
                obj()->roll = rdf(a + 8);
                break;
            default:
                break;  // state the port doesn't use yet (AI parameters, mission settings)
        }
        p += sz;
    }
}

}  // namespace gvr
