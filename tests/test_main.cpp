// Unit tests that need no proprietary data (synthetic inputs only). Asset tests that require
// the user's original files are run with gzptool validate (see TEST_REPORT.md).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "assets/gbs.h"
#include "assets/gzp.h"
#include "assets/gti.h"
#include "assets/world_bin.h"
#include "core/math.h"
#include "game/arm_ik.h"
#include "scene/collision.h"
#include "game/player.h"
#include "game/weapon.h"
#include "game/enemy.h"
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
    // Grip boost while walking: 3 m/s x 5.
    VrRig r4;
    InputFrame b = in;
    b.hand[kLeft].squeeze = 1.0f;
    r4.update(1.0f, b);
    CHECK(near(r4.worldFromTracking.position.z, -15.0f, 1e-3f));
    // Fly mode: looking 90 deg up and pushing forward climbs; no ground snapping.
    VrRig r5;
    r5.flyMode = true;
    r5.groundHeight = [](float, float) { return 0.0f; };
    InputFrame f = in;
    f.head.orientation = Quat::fromAxisAngle({1, 0, 0}, kPi / 2);
    r5.update(1.0f, f);
    CHECK(near(r5.worldFromTracking.position.y, 15.0f, 1e-3f));
    CHECK(near(r5.worldFromTracking.position.z, 0.0f, 1e-3f));
    // Turn-stick Y descends (below the ground: noclip).
    f.hand[kLeft].thumbstick = {0, 0};
    f.hand[kRight].thumbstick = {0, -1};
    r5.update(2.0f, f);
    CHECK(near(r5.worldFromTracking.position.y, -15.0f, 1e-3f));
    // Leaving fly mode re-attaches to the ground.
    r5.flyMode = false;
    f.hand[kRight].thumbstick = {0, 0};
    r5.update(0.016f, f);
    CHECK(near(r5.worldFromTracking.position.y, 0.0f));
}

static void testPlayer() {
    PlayerController pc;
    pc.groundHeight = [](float x, float) { return x * 0.1f; };  // gentle slope rising toward +X
    pc.teleport({0, 5, 0}, 0.0f);
    CHECK(near(pc.position.y, 0.0f));  // teleport snaps to the ground
    PlayerInput in;
    // Stick forward while the viewer faces -X (yaw +90 deg): moves toward -X at runSpeed.
    in.move = {0, 1};
    in.viewYaw = kPi / 2;
    for (int i = 0; i < 72; ++i) pc.update(1.0f / 72.0f, in);
    CHECK(near(pc.position.x, -pc.params.runSpeed, 0.05f));
    CHECK(near(pc.position.z, 0.0f, 1e-3f));
    CHECK(near(pc.position.y, pc.position.x * 0.1f, 1e-3f));  // followed the slope downhill
    CHECK(near(pc.yaw, kPi / 2, 1e-3f));                       // body turned to face travel
    CHECK(pc.anim() == PlayerAnim::Run && pc.onGround);
    // Release the stick: idle.
    in.move = {0, 0};
    pc.update(0.1f, in);
    CHECK(pc.anim() == PlayerAnim::Idle);
    CHECK(pc.fadeWeight() < 1.0f);  // cross-fading from run
    for (int i = 0; i < 30; ++i) pc.update(1.0f / 72.0f, in);
    CHECK(near(pc.fadeWeight(), 1.0f));
    // Jetpack: rises while held, thrust clip; falls back and lands (landing clip).
    const float g0 = pc.position.y;
    in.jet = true;
    for (int i = 0; i < 72; ++i) pc.update(1.0f / 72.0f, in);
    CHECK(!pc.onGround && pc.position.y > g0 + 2.0f);
    CHECK(pc.anim() == PlayerAnim::Thrust);
    in.jet = false;
    pc.update(1.0f / 72.0f, in);
    CHECK(pc.anim() == PlayerAnim::Hover);
    for (int i = 0; i < 72 * 4 && !pc.onGround; ++i) pc.update(1.0f / 72.0f, in);
    CHECK(pc.onGround && near(pc.position.y, g0, 1e-3f));
    CHECK(pc.anim() == PlayerAnim::Land);
    for (int i = 0; i < 72; ++i) pc.update(1.0f / 72.0f, in);
    CHECK(pc.anim() == PlayerAnim::Idle);
    // First person: the body follows the view smoothly (never jumps in one frame), lets the
    // head look ~30 deg off freely, and re-centres over time.
    PlayerController fp;
    fp.faceView = true;
    fp.groundHeight = [](float, float) { return 0.0f; };
    fp.teleport({0, 0, 0}, 0.0f);
    PlayerInput look;
    look.viewYaw = 1.0f;  // head turned ~57 deg left, standing still
    fp.update(1.0f / 72.0f, look);
    CHECK(fp.yaw > 0.0f && fp.yaw < 0.2f);
    for (int i = 0; i < 72; ++i) fp.update(1.0f / 72.0f, look);
    CHECK(fp.yaw > 1.0f - 0.55f);  // carried within the cone after 1 s
    for (int i = 0; i < 72 * 5; ++i) fp.update(1.0f / 72.0f, look);
    CHECK(near(fp.yaw, 1.0f, 0.05f));  // re-centred
    // Slope limit: walking straight up a 60-degree slope is blocked; a 30-degree one is fine.
    for (int k = 0; k < 2; ++k) {
        PlayerController sl;
        const float grade = k == 0 ? std::tan(60.0f * kPi / 180.0f) : std::tan(30.0f * kPi / 180.0f);
        sl.groundHeight = [grade](float, float z) { return std::max(0.0f, -z * grade); };  // rises toward -Z
        sl.teleport({0, 0, 0.5f}, 0.0f);
        PlayerInput up;
        up.move = {0, 1};
        for (int i = 0; i < 72 * 2; ++i) sl.update(1.0f / 72.0f, up);
        if (k == 0) CHECK(sl.position.y < 0.2f);   // stuck at the foot of the cliff
        else CHECK(sl.position.y > 3.0f);          // walked up the hill
    }
    // Steeper than 50 deg: you skid downhill (mc_skid clip) without input; 30 deg: you stay put.
    for (int k = 0; k < 2; ++k) {
        PlayerController sl;
        const float grade = k == 0 ? std::tan(60.0f * kPi / 180.0f) : std::tan(30.0f * kPi / 180.0f);
        sl.groundHeight = [grade](float, float z) { return std::max(0.0f, -z * grade); };
        sl.teleport({0, 0, -3.0f}, 0.0f);
        PlayerInput none;
        bool slid = false;
        for (int i = 0; i < 72 * 3; ++i) { sl.update(1.0f / 72.0f, none); slid |= sl.anim() == PlayerAnim::Slide; }
        if (k == 0) CHECK(slid && sl.position.y < 0.3f && sl.position.z > -0.6f);  // reached the foot of the slope
        else CHECK(!slid && near(sl.position.z, -3.0f, 1e-3f));
    }
    // Jetpack: horizontal speed builds up gradually; fuel runs out (~4 s) and refills later.
    PlayerController jp;
    jp.groundHeight = [](float, float) { return 0.0f; };
    jp.teleport({0, 0, 0}, 0.0f);
    PlayerInput j;
    j.jet = true;
    j.move = {0, 1};
    jp.update(1.0f / 72.0f, j);
    CHECK(jp.horizontalSpeed() < 1.0f);  // not instant
    for (int i = 0; i < 72 * 3; ++i) jp.update(1.0f / 72.0f, j);
    CHECK(jp.horizontalSpeed() > 8.0f && jp.horizontalSpeed() <= jp.params.jetAirSpeed + 1e-3f);
    CHECK(jp.thrusting() && jp.fuel() < 0.3f);
    for (int i = 0; i < 72 * 2; ++i) jp.update(1.0f / 72.0f, j);
    CHECK(!jp.thrusting() && jp.fuel() == 0.0f);  // empty: no more thrust
    j.jet = false;
    for (int i = 0; i < 72 * 6; ++i) jp.update(1.0f / 72.0f, j);
    CHECK(jp.fuel() > 0.99f && jp.onGround);  // landed and refilled
    // Angle wrap helper.
    CHECK(near(angleDelta(3.0f, -3.0f), 2.0f * kPi - 6.0f, 1e-4f));
}

static void testArmIk() {
    // Synthetic arm: shoulder at origin, elbow (0,0,-1), wrist (0,0,-2) hanging down; four
    // fingers ahead (+Y) of the wrist, index side at +X. Mirror the whole arm (det -1) to
    // check that mirrored hierarchies need no special handling.
    for (int mirror = 0; mirror < 2; ++mirror) {
        const float sx = mirror ? -1.0f : 1.0f;
        std::vector<Vec3> pos = {{0, 0, 0}, {0, 0, -1}, {0, 0, -2},
                                 {0.15f * sx, 0.3f, -2}, {0.05f * sx, 0.32f, -2}, {-0.05f * sx, 0.32f, -2}, {-0.15f * sx, 0.3f, -2}};
        std::vector<Mat4> w;
        for (const Vec3& p : pos) w.push_back(Mat4::translation(p) * Mat4::scale({sx, 1, 1}));
        std::vector<std::vector<int>> ch = {{1}, {2}, {3, 4, 5, 6}, {}, {}, {}, {}};
        ArmChain c;
        c.upper = 0; c.fore = 1; c.hand = 2;
        for (int i = 0; i < 4; ++i) c.finger[i] = 3 + i;
        ArmTarget t;
        t.grip = {0.6f, 1.0f, -0.6f};
        t.forward = {0, 1, 0};
        t.up = {sx, 0, 0};
        t.pole = {0, 0, -1};
        const float residual = solveArmIk(w, ch, c, t);
        CHECK(residual >= 0 && residual < 1e-3f);
        auto P = [&](int i) { return Vec3{w[size_t(i)].m[12], w[size_t(i)].m[13], w[size_t(i)].m[14]}; };
        CHECK(near(length(P(1) - P(0)), 1.0f, 1e-3f));  // bone lengths preserved
        CHECK(near(length(P(2) - P(1)), 1.0f, 1e-3f));
        Vec3 f = (P(3) + P(4) + P(5) + P(6)) * 0.25f;
        CHECK(near(length(f - t.grip), 0.6f * length(f - P(2)), 1e-2f) || length(f - t.grip) < 0.2f);
        CHECK(dot(normalize(f - P(2)), t.forward) > 0.999f);           // knuckles point forward
        CHECK(dot(normalize(P(3) - P(6)), t.up) > 0.99f);              // index side up
        CHECK(P(1).z < -0.2f);                                           // elbow bends toward the pole (down)
    }
    {
        // Reach stretch: a target 3 units away (arm length 2) is reached with stretch <= 1.8.
        std::vector<Mat4> w;
        std::vector<Vec3> pos = {{0, 0, 0}, {0, 0, -1}, {0, 0, -2}, {0.15f, 0.3f, -2}, {0.05f, 0.32f, -2}, {-0.05f, 0.32f, -2}, {-0.15f, 0.3f, -2}};
        for (const Vec3& p : pos) w.push_back(Mat4::translation(p));
        std::vector<std::vector<int>> ch = {{1}, {2}, {3, 4, 5, 6}, {}, {}, {}, {}};
        ArmChain c;
        c.upper = 0; c.fore = 1; c.hand = 2;
        for (int i = 0; i < 4; ++i) c.finger[i] = 3 + i;
        ArmTarget t;
        t.grip = {0, 3.2f, 0};
        t.forward = {0, 1, 0};
        t.up = {1, 0, 0};
        t.pole = {0, 0, -1};
        t.maxReachStretch = 1.8f;
        CHECK(solveArmIk(w, ch, c, t) < 1e-3f);
        t.grip = {0, 6.0f, 0};  // beyond even the stretched arm: stops at the limit
        CHECK(solveArmIk(w, ch, c, t) > 0.1f);
    }
}

static std::vector<Vec3> boxTris(const Vec3& mn, const Vec3& mx) {
    const Vec3 v[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                       {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    const int f[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
    std::vector<Vec3> t;
    for (const auto& q : f) {
        t.insert(t.end(), {v[q[0]], v[q[1]], v[q[2]]});
        t.insert(t.end(), {v[q[0]], v[q[2]], v[q[3]]});
    }
    return t;
}

static void testCollision() {
    CHECK(near(length(closestPointOnTriangle({0, 5, 0}, {-1, 0, -1}, {1, 0, -1}, {0, 0, 1}) - Vec3{0, 0, 0}), 0.0f));
    CollisionWorld cw;
    cw.addObject(boxTris({-1, 0, -1}, {1, 2, 1}));      // 2 m tall block
    cw.addObject(boxTris({3, 0, -1}, {4, 0.3f, 1}));    // 30 cm step
    CHECK(cw.objectCount() == 2 && cw.triangleCount() == 24);
    // Walking into the block's +X face: pushed back to radius distance.
    Vec3 feet{1.2f, 0, 0.2f};
    CHECK(cw.pushOut(feet, 0.35f, 1.8f, 0.45f));
    CHECK(near(feet.x, 1.35f, 1e-3f) && near(feet.z, 0.2f, 1e-3f));
    // Near a corner: pushed out diagonally.
    Vec3 corner{1.1f, 0, 1.1f};
    cw.pushOut(corner, 0.35f, 1.8f, 0.45f);
    CHECK(length(Vec3{corner.x - 1, 0, corner.z - 1}) > 0.349f);
    // The low step is not a wall (stepped onto instead) and is ground.
    Vec3 onStep{2.8f, 0, 0};
    CHECK(!cw.pushOut(onStep, 0.35f, 1.8f, 0.45f));
    float g = 0;
    CHECK(cw.groundAt(3.5f, 0, 0.0f + 0.45f, g) && near(g, 0.3f));
    // Standing on the block top only when coming from above.
    CHECK(cw.groundAt(0, 0, 2.2f, g) && near(g, 2.0f));
    CHECK(!cw.groundAt(0, 0, 1.0f, g) || g < 1.0f);  // the 2 m top is not reported from below
    // A capsule standing on the block top is not pushed by its walls.
    Vec3 top{0.5f, 2.0f, 0.5f};
    CHECK(!cw.pushOut(top, 0.35f, 1.8f, 0.45f));
}

static void testWeapon() {
    WeaponSystem w;
    // Wall: plane z = -10 facing +z.
    int hitTarget = 99;
    w.raycast = [](const Vec3& o, const Vec3& d, float maxD, float& t, Vec3& n, int& target) {
        if (d.z >= 0) return false;
        t = (-10.0f - o.z) / d.z;
        n = {0, 0, 1};
        target = 7;
        return t >= 0 && t <= maxD;
    };
    w.onHit = [&](int target, const Vec3&, const Vec3&) { hitTarget = target; };
    const float dt = 1.0f / 72.0f;
    CHECK(w.update(dt, true, {0, 1, 0}, {0, 0, -1}));   // fires immediately
    CHECK(!w.update(dt, true, {0, 1, 0}, {0, 0, -1}));  // cooling down
    CHECK(w.projectiles().size() == 1 && w.effects().size() == 1);  // bolt + muzzle flash
    for (int i = 0; i < 20; ++i) w.update(dt, false, {0, 1, 0}, {0, 0, -1});
    CHECK(w.hits() == 1 && w.projectiles().empty());
    CHECK(near(w.lastHitDistance(), 10.0f, 1e-3f));
    CHECK(hitTarget == 7);
    bool impact = false;
    for (const auto& e : w.effects()) impact |= e.kind == WeaponEffect::Impact && near(e.pos.z, -9.95f, 1e-3f);
    CHECK(impact);
    // Holding the trigger: one shot per fireInterval.
    WeaponSystem a;
    int shots = 0;
    for (int i = 0; i < 72; ++i) shots += a.update(dt, true, {0, 0, 0}, {0, 1, 0}) ? 1 : 0;
    CHECK(shots == 5);  // t = 0, 0.2, 0.4, 0.6, 0.8 s within one second
}

static void testEnemies() {
    // Capsule ray: hits the near surface, misses beside it.
    float t;
    CHECK(rayCapsule({0, 1, 10}, {0, 0, -1}, 20, {0, 0.5f, 0}, {0, 1.8f, 0}, 0.5f, t) && near(t, 9.5f, 1e-3f));
    CHECK(!rayCapsule({1, 1, 10}, {0, 0, -1}, 20, {0, 0.5f, 0}, {0, 1.8f, 0}, 0.5f, t));
    CHECK(!rayCapsule({0, 1, 10}, {0, 0, -1}, 5, {0, 0.5f, 0}, {0, 1.8f, 0}, 0.5f, t));  // out of range

    EnemyDirector d;
    d.groundHeight = [](float, float) { return 0.0f; };
    for (auto& c : d.clipDuration) c = 0.6f;
    d.spawn({0, 0, -60}, 0.0f);
    std::vector<int> shots;
    const Vec3 player{0, 0, 0};
    const float dt = 1.0f / 72.0f;
    d.update(dt, player, true, shots);
    CHECK(d.enemies[0].state == EnemyState::Idle);  // 60 m: out of sight
    d.enemies[0].pos.z = -40;
    d.update(dt, player, true, shots);
    d.update(dt, player, true, shots);
    CHECK(d.enemies[0].state == EnemyState::Advance && d.enemies[0].clip == EnemyClip::Run);  // far: runs
    // Runs in, stops to shoot, runs to new spots: never shoots while running.
    int fired = 0, firedWhileRunning = 0, stops = 0;
    EnemyState prevState = d.enemies[0].state;
    float closest = 1e9f;
    for (int i = 0; i < 72 * 20; ++i) {
        d.update(dt, player, true, shots);
        fired += int(shots.size());
        if (!shots.empty() && d.enemies[0].clip == EnemyClip::Run) ++firedWhileRunning;
        if (d.enemies[0].state == EnemyState::Fire && prevState == EnemyState::Advance) ++stops;
        prevState = d.enemies[0].state;
        closest = std::min(closest, length(d.enemies[0].pos - player));
    }
    CHECK(fired >= 6 && firedWhileRunning == 0);
    CHECK(stops >= 2);                                   // repositioned between stops
    CHECK(closest <= d.params.shootRange);
    // Bring it to a stop for the hit test below.
    for (int i = 0; i < 72 * 10 && d.enemies[0].state != EnemyState::Fire; ++i) d.update(dt, player, true, shots);
    CHECK(d.enemies[0].state == EnemyState::Fire);
    // Ray from the player hits it; 4 bolts of 25 kill it.
    float th;
    Vec3 n;
    int idx = -1;
    const Vec3 dir = normalize(Vec3{d.enemies[0].pos.x, 1.2f, d.enemies[0].pos.z} - Vec3{0, 1.2f, 0});
    CHECK(d.raycast({0, 1.2f, 0}, dir, 100, th, n, idx) && idx == 0);
    CHECK(!d.damage(0, 25) && d.enemies[0].state == EnemyState::Hit);
    for (int i = 0; i < 72; ++i) d.update(dt, player, true, shots);
    CHECK(d.enemies[0].state != EnemyState::Hit);  // recovered from the hit (runs to a new spot)
    d.damage(0, 25);
    d.damage(0, 25);
    CHECK(d.damage(0, 25) && d.enemies[0].state == EnemyState::Dead && d.aliveCount() == 0);
    CHECK(!d.raycast({0, 1.2f, 0}, dir, 100, th, n, idx));  // shots pass over corpses
    // Corpse sinks away and is removed.
    for (int i = 0; i < 72 * 20; ++i) d.update(dt, player, true, shots);
    CHECK(d.enemies.empty());
}

static void testGti() {
    // 2x1 interior cells, lightmap flag; stream: skip 0, literal run of 2 cells.
    std::vector<uint8_t> d;
    put32(d, 0x94965003); put32(d, 0);
    putf(d, -100); putf(d, -50); putf(d, 0); putf(d, 10);
    put32(d, 2); put32(d, 1); putf(d, 50.0f); putf(d, 0.5f); putf(d, 0.5f); put32(d, 0); put32(d, 0);
    d.push_back(2); d.push_back(1); d.push_back(1); d.push_back(0);
    put32(d, 0); putf(d, 1.0f);
    char name[32] = "synthetic"; d.insert(d.end(), name, name + 32);
    d.push_back(1);  // run of 2 literal cells
    putf(d, 3.0f); d.push_back(5); d.push_back(10); d.push_back(20); d.push_back(30);
    putf(d, 7.0f); d.push_back(5); d.push_back(1); d.push_back(2); d.push_back(3);
    GtiTerrain t;
    std::string err;
    CHECK(parseGti(d.data(), d.size(), t, &err));
    CHECK(t.gridW == 4 && t.gridH == 3);
    CHECK(t.valid(1, 1) && t.valid(2, 1) && !t.valid(0, 0) && !t.valid(1, 2));
    CHECK(near(t.h(2, 1), 7.0f) && t.light[(1 * 4 + 1) * 3 + 2] == 30);
    CHECK(near(t.worldX(1), -100.0f) && near(t.worldX(2), -50.0f));
    CHECK(!parseGti(d.data(), d.size() - 2, t, &err));
}

static void testWorldBin() {
    std::vector<uint8_t> d;
    put32(d, 0x1A0002E5);
    for (int i = 0; i < 7; ++i) put32(d, 32);
    std::vector<uint8_t> s;
    const char* h1 = "box01"; s.insert(s.end(), h1, h1 + 6);
    const char* h2 = "test.gti"; s.insert(s.end(), h2, h2 + 9);
    s.push_back(52); { char n[44] = "grnd_tex"; s.insert(s.end(), n, n + 44); }
    s.push_back(41); putf(s, 10.0f); putf(s, 100.0f); s.push_back(1); s.push_back(2); s.push_back(3);
    s.push_back(42); put32(s, 1298); putf(s, 1.0f); putf(s, 2.0f); putf(s, 3.0f); putf(s, 0.5f);
    s.push_back(0xFF);
    put32(d, uint32_t(s.size()));
    d.insert(d.end(), s.begin(), s.end());
    WorldDefinition w;
    std::string err;
    CHECK(parseWorldBin(d.data(), d.size(), w, &err));
    CHECK(w.terrainName == "test.gti" && w.groundTexture == "grnd_tex");
    CHECK(w.hasFog && near(w.fogFar, 100.0f) && w.fogRGB[2] == 3);
    CHECK(w.objects.size() == 1 && w.objects[0].type == 1298 && near(w.objects[0].z, 3.0f));
    CHECK(w.complete && w.stopOpcode == 0xFF);
}

int main() {
    testGti();
    testWorldBin();
    testLzss();
    testGzp();
    testGbs();
    testMath();
    testRig();
    testPlayer();
    testArmIk();
    testCollision();
    testWeapon();
    testEnemies();
    if (g_failures) {
        std::printf("%d test failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
