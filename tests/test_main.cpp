// Unit tests that need no proprietary data (synthetic inputs only). Asset tests that require
// the user's original files are run with gzptool validate (see TEST_REPORT.md).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "assets/gbs.h"
#include "assets/gzp.h"
#include "core/math.h"
#include "game/vr_rig.h"

using namespace gvr;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)
static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
static void putf(std::vector<uint8_t>& v, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(v, u); }

static void testLzss() {
    // Literal run "ABCD" then a back-reference copying 6 bytes from window position of 'A'.
    // Window starts at 0xFEE, so 'A' was written at 0xFEE.
    std::vector<uint8_t> in = {0x0F, 'A', 'B', 'C', 'D', 0xEE, 0xF3};
    std::vector<uint8_t> out;
    CHECK(lzssDecompress(in.data(), in.size(), 10, out));
    CHECK(std::string(out.begin(), out.end()) == "ABCDABCDAB");
    // Reference into the space-initialised window.
    std::vector<uint8_t> in2 = {0x00, 0x00, 0x00};  // flag 0 -> ref pos 0, len 3
    CHECK(lzssDecompress(in2.data(), in2.size(), 3, out));
    CHECK(std::string(out.begin(), out.end()) == "   ");
    // Truncated input reports failure.
    CHECK(!lzssDecompress(in.data(), 3, 10, out));
}

static void testGzp() {
    // Build an archive with one stored and one LZSS file.
    std::vector<uint8_t> a;
    put32(a, 0x6608F101); put32(a, 0);  // index offset patched later
    const std::string stored = "hello";
    uint32_t off1 = uint32_t(a.size());
    put32(a, 16 + 5); put32(a, 5); put32(a, 0); put32(a, 2);
    a.insert(a.end(), stored.begin(), stored.end());
    std::vector<uint8_t> comp = {0x0F, 'A', 'B', 'C', 'D', 0xEE, 0xF3};
    uint32_t off2 = uint32_t(a.size());
    put32(a, 16 + uint32_t(comp.size())); put32(a, 10); put32(a, 0); put32(a, 1);
    a.insert(a.end(), comp.begin(), comp.end());
    uint32_t idx = uint32_t(a.size());
    put32(a, 0); put32(a, 2);
    auto entry = [&](const char* name, uint32_t size, uint32_t usz, uint32_t off, uint8_t c) {
        put32(a, size); put32(a, usz); put32(a, 0); put32(a, off); a.push_back(c);
        a.push_back(uint8_t(std::strlen(name) + 1));
        a.insert(a.end(), name, name + std::strlen(name) + 1);
    };
    entry("Stored.TXT", 21, 5, off1, 2);
    entry("packed.bin", 16 + uint32_t(comp.size()), 10, off2, 1);
    std::memcpy(&a[4], &idx, 4);
    GzpArchive g;
    std::string err;
    CHECK(g.openMemory(a, "synthetic", &err));
    CHECK(g.entries().size() == 2);
    std::vector<uint8_t> out;
    const GzpEntry* e = g.find("stored.txt");
    CHECK(e && g.extract(*e, out) && std::string(out.begin(), out.end()) == "hello");
    e = g.find("PACKED.BIN");
    CHECK(e && g.extract(*e, out) && std::string(out.begin(), out.end()) == "ABCDABCDAB");
    a[0] = 0;
    CHECK(!g.openMemory(a, "bad", &err));
}

static void testGbs() {
    // Minimal GBS: one triangle, UV + RGB flags, one max object, one sub-object.
    std::vector<uint8_t> d;
    put32(d, 0xAA0100BE); put32(d, 0x6);
    put32(d, 3);
    const float P[9] = {0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (float f : P) putf(d, f);
    put32(d, 3);
    for (uint16_t i = 0; i < 3; ++i) put16(d, i);
    for (int i = 0; i < 3; ++i) { putf(d, 0.5f); putf(d, 0.25f); }
    for (int i = 0; i < 3; ++i) { d.push_back(10); d.push_back(20); d.push_back(30); }
    put32(d, 1);
    put32(d, 0); put32(d, 3); put32(d, 0); put32(d, 0); put32(d, 0);
    put32(d, 1);
    char name[32] = "tri"; d.insert(d.end(), name, name + 32);
    put32(d, 0); put32(d, 1); put32(d, 4);
    put16(d, 1); put16(d, 0); put16(d, 1); put16(d, 2);
    put32(d, 0); put32(d, 3);
    char tex[32] = "sometex"; d.insert(d.end(), tex, tex + 32);
    char tex2[32] = {}; d.insert(d.end(), tex2, tex2 + 32);
    putf(d, 0); putf(d, 0); put32(d, 0x10);
    for (int i = 0; i < 4; ++i) put32(d, 0x00FFFFFF);
    putf(d, 25.0f);
    GbsModel m;
    std::string err;
    CHECK(parseGbs(d.data(), d.size(), m, &err));
    if (!err.empty()) std::printf("  gbs error: %s\n", err.c_str());
    CHECK(m.subObjects.size() == 1 && m.subObjects[0].indices.size() == 3);
    CHECK(m.subObjects[0].texture == "sometex");
    CHECK(m.rgbs.size() == 3 && m.rgbs[0][2] == 30);
    // Out-of-range triangle index must be rejected.
    std::vector<uint8_t> bad = d;
    size_t triOff = d.size() - (32 + 32 + 4 + 4 + 4 + 16 + 4) - 8 - 6;
    bad[triOff] = 9;
    CHECK(!parseGbs(bad.data(), bad.size(), m, &err));
    // Truncation must be rejected.
    CHECK(!parseGbs(d.data(), d.size() - 3, m, &err));
}

static void testMath() {
    Quat q = Quat::fromAxisAngle({0, 1, 0}, kPi / 2);
    Vec3 v = q.rotate({0, 0, -1});
    CHECK(near(v.x, -1) && near(v.z, 0));  // turning left 90 deg: forward becomes -X
    CHECK(near(yawOf(q), kPi / 2));
    Pose p{q, {1, 2, 3}};
    Pose id = p * p.inverse();
    CHECK(near(id.position.x, 0) && near(id.position.y, 0) && near(id.orientation.w, 1));
    Mat4 m = Mat4::fromPose(p);
    Vec4 r = m * Vec4{0, 0, -1, 1};
    Vec3 e = p.transformPoint({0, 0, -1});
    CHECK(near(r.x, e.x) && near(r.y, e.y) && near(r.z, e.z));
    // Symmetric 90 deg FOV projection maps a point at the frustum edge to NDC x = 1.
    Mat4 proj = Mat4::projectionFov(-kPi / 4, kPi / 4, kPi / 4, -kPi / 4, 0.1f, 100.0f);
    Vec4 c = proj * Vec4{1, 0, -1, 1};
    CHECK(near(c.x / c.w, 1.0f));
}

static void testRig() {
    VrRig rig;
    rig.comfort.snapTurn = true;
    InputFrame in;
    in.headValid = true;
    in.head.position = {0.5f, 1.6f, 0.5f};
    // Snap turn right should rotate -30 deg about the head and keep the head in place.
    Vec3 before = rig.toWorld(in.head).position;
    in.hand[kRight].thumbstick = {1.0f, 0};
    rig.update(0.016f, in);
    Vec3 after = rig.toWorld(in.head).position;
    CHECK(near(before.x, after.x) && near(before.z, after.z));
    CHECK(near(yawOf(rig.worldFromTracking.orientation), -kPi / 6));
    // Holding the stick does not keep snapping.
    rig.update(0.016f, in);
    CHECK(near(yawOf(rig.worldFromTracking.orientation), -kPi / 6));
    // Move forward 1 s at full deflection with head facing -Z: moves 3 m along the rig's forward.
    VrRig r2;
    in = InputFrame{};
    in.headValid = true;
    in.hand[kLeft].thumbstick = {0, 1};
    r2.update(1.0f, in);
    CHECK(near(r2.worldFromTracking.position.z, -3.0f, 1e-3f));
    // Left-handed mode swaps the sticks.
    VrRig r3;
    r3.comfort.leftHanded = true;
    r3.update(1.0f, in);
    CHECK(near(r3.worldFromTracking.position.z, 0.0f));
}

int main() {
    testLzss();
    testGzp();
    testGbs();
    testMath();
    testRig();
    if (g_failures) {
        std::printf("%d test failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
