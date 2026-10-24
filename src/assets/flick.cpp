#include "assets/flick.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gvr {

namespace {
uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
std::string cstr(const uint8_t* d, size_t n, uint32_t off) {
    if (off == 0 || off >= n) return {};
    size_t e = off;
    while (e < n && d[e]) ++e;
    return std::string(reinterpret_cast<const char*>(d + off), e - off);
}
}  // namespace

bool parseFlick(const uint8_t* d, size_t n, FlickShot& out, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    out = FlickShot{};
    if (n < 0x20 || rd32(d) != 3) return fail("not a version-3 flick");
    if (rd32(d + 4) != n) return fail("size mismatch");
    if (const uint32_t nx = rd32(d + 20)) out.next = cstr(d, n, nx);
    size_t p = 0x18;
    float waited = 0;  // shot time reached by the waits so far (cue and subtitle times)
    while (p + 8 <= n) {
        const uint32_t op = rd32(d + p), sz = rd32(d + p + 4);
        if (sz < 2 || p + 4 * size_t(sz) > n) return fail("bad record size");
        const uint8_t* w = d + p + 8;  // payload words
        const size_t nw = sz - 2;
        auto word = [&](size_t i) -> uint32_t { return i < nw ? rd32(w + 4 * i) : 0; };
        switch (op) {
            case 1: {  // director: embedded animation image at offset word[1]
                const uint32_t off = word(1);
                if (!out.hasDirector && off && off + 4 <= n) {
                    const uint32_t len = rd32(d + off);
                    std::string err;
                    if (len <= n - off && parseAnm(d + off, len, out.director, &err)) out.hasDirector = true;
                }
                break;
            }
            case 2: {  // actor: {director, nodeOff, actorIdx, objectType}
                FlickActor a;
                a.director = int(word(0));
                a.node = cstr(d, n, word(1));
                a.index = int(word(2));
                a.type = int(word(3));
                out.actors.push_back(a);
                break;
            }
            case 3: {  // attach: {parentActor, nodeOff, actorIdx, animOff}
                const int idx = int(word(2));
                for (auto& a : out.actors)
                    if (a.index == idx) {
                        a.parentActor = int(word(0));
                        a.parentNode = cstr(d, n, word(1));
                        a.anim = cstr(d, n, word(3));
                    }
                break;
            }
            case 4: out.cameraNode = cstr(d, n, word(2)); break;
            case 0x20: {  // place a free actor: {f32 x, y, z, yaw, actorIdx} (world, game units)
                const int idx = int(word(4));
                auto f = [&](size_t i) { float v; const uint32_t u = word(i); std::memcpy(&v, &u, 4); return v; };
                for (auto& a : out.actors)
                    if (a.index == idx) { a.placed = true; a.px = f(0); a.py = f(1); a.pz = f(2); a.pyaw = f(3); }
                break;
            }
            case 0x19: {  // ground node: {actorIdx, nodeOff}
                const int idx = int(word(0));
                for (auto& a : out.actors)
                    if (a.index == idx) a.groundNode = cstr(d, n, word(1));
                break;
            }
            case 0x22: {  // ground follow: {actorIdx, 6, 5, f32 x0, y0, x1, y1} (footprint, game units)
                const int idx = int(word(0));
                auto f = [&](size_t i) { float v; const uint32_t u = word(i); std::memcpy(&v, &u, 4); return v; };
                for (auto& a : out.actors)
                    if (a.index == idx && nw >= 7) {
                        a.groundFollow = true;
                        a.footMin[0] = f(3); a.footMin[1] = f(4);
                        a.footMax[0] = f(5); a.footMax[1] = f(6);
                    }
                break;
            }
            case 0x10: {
                const std::string key = cstr(d, n, word(0));
                if (!key.empty()) {
                    out.subtitles.push_back(key);
                    out.subtitleTimes.push_back(waited);
                }
                break;
            }
            case 0x34: if (out.stream.empty()) out.stream = cstr(d, n, word(0)); break;
            case 0x0B:
            case 0x0C:
                out.visibility.push_back({waited, int(word(0)), op == 0x0B});
                break;
            case 0x1F:
                if (nw >= 2) {
                    float lv, sec;
                    const uint32_t a = word(0), b = word(1);
                    std::memcpy(&lv, &a, 4);
                    std::memcpy(&sec, &b, 4);
                    int same = 0;
                    for (const auto& f : out.fades) same += std::fabs(f.time - waited) < 1e-4f;
                    if (same < 2) out.fades.push_back({waited, std::max(0.0f, std::min(1.0f, lv)), std::max(0.0f, sec)});
                }
                break;
            case 9:  // wait until timeline frame N
                if (out.hasDirector && out.director.fps && out.director.ticksPerFrame) {
                    const float t = (float(word(0)) * float(out.director.ticksPerFrame) - float(out.director.startTick)) /
                                    (float(out.director.ticksPerFrame) * float(out.director.fps));
                    waited = std::max(waited, t);
                }
                break;
            case 10:  // wait until timeline tick N
                if (out.hasDirector && out.director.fps && out.director.ticksPerFrame)
                    waited = std::max(waited, (float(word(0)) - float(out.director.startTick)) /
                                                  (float(out.director.ticksPerFrame) * float(out.director.fps)));
                break;
            case 0x2C: {
                const std::string s = cstr(d, n, word(0));
                if (!s.empty()) out.cues.push_back({waited, s});
                break;
            }
            default: break;
        }
        p += 4 * size_t(sz);
        if (op == 5 && p + 8 > n) break;
        if (op > 0x40) return fail("unknown op");
        // The bytecode ends where the string pool begins (no further valid records).
        if (p + 8 <= n) {
            const uint32_t nop = rd32(d + p), nsz = rd32(d + p + 4);
            if (nop == 0 || nop > 0x40 || nsz < 2 || nsz > 64) break;
        }
    }
    return true;
}

}  // namespace gvr
