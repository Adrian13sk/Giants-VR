// Unit tests that need no proprietary data (synthetic inputs only). Asset tests that require
// the user's original files are run with gzptool validate (see TEST_REPORT.md).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "assets/flick.h"
#include "assets/gbs.h"
#include "assets/gzp.h"
#include "assets/gti.h"
#include "assets/world_bin.h"
#include "assets/mission.h"
#include "assets/object_defs.h"
#include "assets/weapondefs.h"
#include "assets/fx_defs.h"
#include "game/fx_player.h"
#include "game/skirmish.h"
#include "game/sk_net.h"
#include "scene/model_cache.h"
#include "assets/image.h"
#include "core/math.h"
#include "game/test_script.h"
#include "game/arm_ik.h"
#include "scene/collision.h"
#include "game/player.h"
#include "game/weapon.h"
#include "game/enemy.h"
#include "game/creature.h"
#include "game/smartie.h"
#include "game/gore.h"
#include "game/destruction.h"
#include "game/particles.h"
#include "game/hud_gauge.h"
#include "game/villager.h"
#include "game/vimp.h"
#include "game/jetski.h"
#include "game/save_game.h"
#include "game/settings.h"
#include "audio/mixer.h"
#include "audio/music.h"
#include "assets/gtext.h"
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
    // Dry jetpack held in the air: sputters, braking the fall (no hard landing) and forward speed.
    for (int held = 1; held >= 0; --held) {
        PlayerController jp;
        jp.groundHeight = [](float, float) { return 0.0f; };
        jp.teleport({0, 0, 0}, 0.0f);
        PlayerInput ji;
        ji.jet = true;
        ji.move = {0, 1};
        float maxImpact = 0;
        bool sputtered = false;
        for (int i = 0; i < 72 * 30 && !(i > 72 && jp.onGround); ++i) {
            if (jp.fuel() < 0.02f && !held) ji.jet = false;  // let go just before it runs dry
            jp.update(1.0f / 72.0f, ji);
            sputtered = sputtered || jp.sputtering();
            maxImpact = std::max(maxImpact, jp.takeImpact());
        }
        CHECK(jp.onGround);
        if (held) CHECK(jp.overheated() && sputtered && maxImpact > 0 && maxImpact < jp.params.hardLandSpeed);
        else CHECK(!sputtered && maxImpact > jp.params.stunSpeed);
    }
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
    // Jetpack: horizontal speed builds up gradually; fuel runs out (5 s), overheats, refills on the ground.
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
    CHECK(jp.thrusting() && jp.fuel() < 1.0f - 3.0f / jp.params.fuelSeconds + 0.05f);
    for (int i = 0; i < int(72 * (jp.params.fuelSeconds - 3.0f)) + 20; ++i) jp.update(1.0f / 72.0f, j);
    CHECK(!jp.thrusting() && jp.fuel() == 0.0f && jp.overheated() && jp.takeOverheat());  // burnt dry: overheated
    // Falling without thrust: no air control, air friction slows him.
    const float hs0 = jp.horizontalSpeed();
    for (int i = 0; i < 72; ++i) jp.update(1.0f / 72.0f, j);
    CHECK(jp.horizontalSpeed() < hs0 * 0.85f);
    j.jet = false;
    float impact = 0;
    bool stunned = false;
    for (int i = 0; i < 72 * 12; ++i) { jp.update(1.0f / 72.0f, j); impact = std::max(impact, jp.takeImpact()); stunned |= jp.stunned(); }
    CHECK(impact > jp.params.stunSpeed && stunned);  // a hard landing from that height
    CHECK(jp.fuel() > 0.99f && jp.onGround && !jp.overheated());  // refilled on the ground
    // Landing clips: a 1 m drop keeps the ground clips (no air clip, no landing); a 4 m drop plays
    // the soft landing once (held on its last frame, not looped), without a stun.
    for (int k = 0; k < 2; ++k) {
        PlayerController dp;
        dp.groundHeight = [](float, float) { return 0.0f; };
        dp.oneShot[int(PlayerAnim::Land)] = 0.6f;
        dp.teleport({0, 0, 0}, 0.0f);
        dp.position.y = k == 0 ? 1.0f : 4.0f;
        dp.onGround = false;
        PlayerInput none;
        bool air = false, landed = false, stun = false;
        float maxLandTime = 0;
        for (int i = 0; i < 72 * 2; ++i) {
            dp.update(1.0f / 72.0f, none);
            air |= dp.anim() == PlayerAnim::Hover || dp.anim() == PlayerAnim::Fall;
            landed |= dp.anim() == PlayerAnim::Land;
            stun |= dp.stunned();
            if (dp.anim() == PlayerAnim::Land) maxLandTime = std::max(maxLandTime, dp.animTime());
        }
        if (k == 0) CHECK(!air && !landed && !stun);
        else { CHECK(air && landed && !stun && maxLandTime < 0.6f); if (!(air && landed && !stun)) std::printf("air %d landed %d stun %d t %.2f\n", air, landed, stun, maxLandTime); }
    }
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
    d.spawn({0, 0, -70}, kPi);  // facing the player (+Z)
    std::vector<int> shots;
    const Vec3 player{0, 0, 0};
    const float dt = 1.0f / 72.0f;
    d.update(dt, player, true, shots);
    CHECK(d.enemies[0].state == EnemyState::Idle);  // 70 m: out of sight
    d.enemies[0].pos.z = -40;
    d.enemies[0].home = d.enemies[0].pos;
    d.enemies[0].patrolWait = 10.0f;  // standing still, looking this way
    // Notices the player in front of it, takes a moment (alert scan), then runs.
    for (int i = 0; i < 36 && !d.enemies[0].aware; ++i) d.update(dt, player, true, shots);
    d.update(dt, player, true, shots);
    CHECK(d.enemies[0].aware && d.enemies[0].state == EnemyState::Idle && d.enemies[0].clip == EnemyClip::Alert);
    for (int i = 0; i < 72 * 2 && d.enemies[0].state == EnemyState::Idle; ++i) d.update(dt, player, true, shots);
    d.update(dt, player, true, shots);
    CHECK(d.enemies[0].state == EnemyState::Advance && d.enemies[0].clip == EnemyClip::Run);  // far: runs
    {
        // Facing away and not close: doesn't notice the player; patrols near home instead.
        EnemyDirector p;
        p.groundHeight = d.groundHeight;
        for (auto& c : p.clipDuration) c = 0.6f;
        p.spawn({0, 0, -30}, 0.0f);
        p.enemies[0].patrolWait = 100.0f;  // standing guard, back to the player
        for (int i = 0; i < 72 * 3; ++i) p.update(dt, player, true, shots);
        CHECK(!p.enemies[0].aware && p.enemies[0].state == EnemyState::Idle);
        // With nobody about, its patrol stays near its post.
        EnemyDirector q;
        q.groundHeight = d.groundHeight;
        for (auto& c : q.clipDuration) c = 0.6f;
        q.spawn({0, 0, -30}, 0.0f);
        bool walked = false;
        for (int i = 0; i < 72 * 30; ++i) {
            q.update(dt, Vec3{0, 0, 500}, true, shots);
            walked |= q.enemies[0].clip == EnemyClip::Patrol;
            CHECK(length(q.enemies[0].pos - Vec3{0, 0, -30}) <= q.params.patrolRadius + 0.5f);
        }
        CHECK(walked && q.enemies[0].state == EnemyState::Idle);
        // Shot at: aware at once.
        p.damage(0, 10.0f);
        CHECK(p.enemies[0].aware);
    }
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
    {
        // Cliff: a wall of 60-degree ground between the enemy and the player blocks it; it
        // never climbs (stays low) and shoots when in range instead.
        EnemyDirector c;
        c.groundHeight = [](float, float z) { return z > -20.0f && z < -10.0f ? (z + 20.0f) * 1.73f : (z >= -10.0f ? 17.3f : 0.0f); };
        for (auto& cd : c.clipDuration) cd = 0.6f;
        c.spawn({0, 0, -40}, 0.0f);
        std::vector<int> sh;
        const Vec3 top{0, 17.3f, 0};
        float maxY = 0;
        for (int i = 0; i < 72 * 20; ++i) { c.update(1.0f / 72.0f, top, true, sh); maxY = std::max(maxY, c.enemies[0].pos.y); }
        CHECK(maxY < 1.5f);  // never walked up the cliff
    }
    for (int i = 0; i < 72 * 20; ++i) d.update(dt, player, true, shots);
    CHECK(d.enemies.empty());
}

static std::vector<uint8_t> makeWav(uint32_t rate, const std::vector<int16_t>& samples) {
    std::vector<uint8_t> d;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (8 * i))); };
    auto u16 = [&](uint16_t v) { d.push_back(uint8_t(v)); d.push_back(uint8_t(v >> 8)); };
    d.insert(d.end(), {'R', 'I', 'F', 'F'}); u32(36 + uint32_t(samples.size()) * 2);
    d.insert(d.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); u32(16);
    u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    d.insert(d.end(), {'d', 'a', 't', 'a'}); u32(uint32_t(samples.size()) * 2);
    for (int16_t s : samples) u16(uint16_t(s));
    return d;
}

static void testAudio() {
    std::vector<int16_t> tone(2205);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = int16_t(16000 * std::sin(float(i) * 0.3f));
    const auto bytes = makeWav(22050, tone);
    WavData w;
    CHECK(parseWav(bytes.data(), bytes.size(), w) && w.rate == 22050 && w.mono.size() == 2205);
    CHECK(near(w.mono[1], 16000.0f * std::sin(0.3f) / 32768.0f, 1e-4f));
    AudioMixer m(48000);
    WavData w2 = w;
    CHECK(m.addSound("tone", std::move(w2)));
    m.setListener({0, 0, 0}, Quat{});  // looking down -Z, right = +X
    std::vector<float> buf(2 * 480);
    auto energy = [&](int ch) { float e = 0; for (size_t i = 0; i < 480; ++i) e += buf[2 * i + ch] * buf[2 * i + ch]; return e; };
    // A source on the right is louder in the right channel.
    uint32_t id = m.play("tone", {5, 0, 0});
    m.mix(buf.data(), 480);
    CHECK(energy(1) > 10.0f * energy(0));
    m.stop(id);
    // Far away: silent. Head-relative (2D): centred.
    m.play("tone", {0, 0, -500});
    m.mix(buf.data(), 480);
    CHECK(energy(0) + energy(1) < 1e-9f);
    m.stop(2);
    uint32_t c = m.play2D("tone");
    m.mix(buf.data(), 480);
    CHECK(near(energy(0), energy(1), 1e-4f) && energy(0) > 0.1f);
    // One-shot voices end (0.1 s at 22.05 kHz = 0.1 s at 48 kHz); loops keep playing.
    for (int i = 0; i < 20; ++i) m.mix(buf.data(), 480);
    CHECK(!m.playing(c));
    uint32_t l = m.play2D("tone", 1.0f, 1.0f, true);
    for (int i = 0; i < 20; ++i) m.mix(buf.data(), 480);
    CHECK(m.playing(l) && m.activeVoices() == 1);
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
    // Sizes fixed against the data (B5): 50 = op + u16; 35 = 16-char name; 44 = 32-char name;
    // 108-110 = two 32-char names (music set pairs in w_Story2). An object after them must parse.
    s.push_back(50); s.push_back(8); s.push_back(0);
    s.push_back(35); { char n[16] = "c"; s.insert(s.end(), n, n + 16); }
    s.push_back(44); { char n[32] = "kabuto"; s.insert(s.end(), n, n + 32); }
    s.push_back(110); { char n[64] = "mwb"; std::memcpy(n + 32, "mwbend", 7); s.insert(s.end(), n, n + 64); }
    s.push_back(42); put32(s, 7); putf(s, 4.0f); putf(s, 5.0f); putf(s, 6.0f); putf(s, 0.0f);
    s.push_back(0xFF);
    put32(d, uint32_t(s.size()));
    d.insert(d.end(), s.begin(), s.end());
    WorldDefinition w;
    std::string err;
    CHECK(parseWorldBin(d.data(), d.size(), w, &err));
    CHECK(w.terrainName == "test.gti" && w.groundTexture == "grnd_tex");
    CHECK(w.hasFog && near(w.fogFar, 100.0f) && w.fogRGB[2] == 3);
    CHECK(w.objects.size() == 2 && w.objects[0].type == 1298 && near(w.objects[0].z, 3.0f) && w.objects[1].type == 7);
    CHECK(w.complete && w.stopOpcode == 0xFF);
}

// Byte-builder helpers for synthetic files.
struct Bytes {
    std::vector<uint8_t> v;
    Bytes& u8(uint8_t x) { v.push_back(x); return *this; }
    Bytes& u32(uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); return *this; }
    Bytes& f32(float f) { uint32_t x; std::memcpy(&x, &f, 4); return u32(x); }
    Bytes& str(const char* s, size_t pad = 0) {
        size_t k = std::strlen(s);
        v.insert(v.end(), s, s + k);
        for (size_t i = k; i < std::max(pad, k + 1); ++i) v.push_back(0);
        return *this;
    }
};

static void testMission() {
    using namespace gvr;
    Bytes b;
    b.u32(4).u8(1);
    b.u8(0x0B).u32(2).u32(0).u32(1).u32(0);                     // setup: Mecc
    b.u8(0x03).u32(73).f32(-100).f32(200).f32(10).f32(45);      // object
    b.u8(0x09).f32(1.5f).u8(0x05).u8(3).u8(0x14).str("flick_a", 32);
    b.u8(0x16).u32(254).u8(255).u8(0).u8(0x09).f32(0.6f).u8(0x17);  // mounted child, pop
    b.u8(0x04).u32(992).f32(1).f32(2).f32(3).f32(10).f32(2).f32(6);  // tilted object
    b.u8(0x1E).u32(2).u32(7).u32(8);                            // counted list on current object
    b.u8(0x15).str("start_1", 32).u8(0x02);
    MissionData m;
    std::string err;
    CHECK(parseMission(b.v.data(), b.v.size(), m, &err));
    CHECK(m.objects.size() == 3);
    CHECK(m.setup[0] == 2 && m.startFlick == "start_1");
    if (m.objects.size() == 3) {
        CHECK(m.objects[0].type == 73 && m.objects[0].x == -100 && m.objects[0].yaw == 45);
        CHECK(m.objects[0].scale == 1.5f && m.objects[0].group == 3 && m.objects[0].flicks.size() == 1);
        CHECK(m.objects[1].parent == 0 && m.objects[1].type == 254 && m.objects[1].scale == 0.6f);
        CHECK(m.objects[2].pitch == 2 && m.objects[2].roll == 6);
    }
    // Truncation, unknown ops and trailing data are rejected.
    CHECK(!parseMission(b.v.data(), b.v.size() - 1, m));
    Bytes bad;
    bad.u32(4).u8(0x7E).u8(0x02);
    CHECK(!parseMission(bad.v.data(), bad.v.size(), m));
    b.u8(0);
    CHECK(!parseMission(b.v.data(), b.v.size(), m));
}

static void testObjectDefs() {
    using namespace gvr;
    // Definition blob: 2 meshes, 0 x, 1 group (2 LODs, anims in slots 2 and 1), 2 types.
    Bytes blob;
    const size_t hdr = 4 + 8 + 4 + 4 + 4 + 4 + 8;  // table sizes, filled below
    std::vector<uint32_t> offs;
    Bytes body;
    auto at = [&]() { return uint32_t(hdr + body.v.size()); };
    const uint32_t m0 = at(); body.u32(500).str("tree_l0");
    const uint32_t m1 = at(); body.u32(501).str("tree_l1");
    const uint32_t g0 = at();
    body.u32(40).u8(0x51).u32(500).f32(60).f32(0).u8(0x51).u32(501).f32(60).f32(100);
    body.u8(0x52).u8(2).str("Tree_Walk.anm").u8(0x52).u8(1).str("tree_idle.anm");
    body.u8(0x7F).u32(5).str("a").str("b").str("c").str("d").str("e").u32(0xFFFFFFFF).u8(0x55).u8(0xFF);
    const uint32_t t0 = at(); body.u32(102).u8(0x01).u32(40).u8(0x13).u8(1).u8(0x1E).u8(60).u8(0x17).f32(2.2f).u8(0xFF);
    const uint32_t t1 = at(); body.u32(210).u8(0x13).u8(0x2D).u8(0x2F).u8(0x01).u32(40).u8(0xFF);
    blob.u32(2).u32(m0).u32(m1).u32(0).u32(1).u32(g0).u32(2).u32(t0).u32(t1);
    CHECK(blob.v.size() == hdr);
    blob.v.insert(blob.v.end(), body.v.begin(), body.v.end());
    Bytes file;
    file.u32(0x1A0002E5);
    const uint32_t sect = 32;
    for (int i = 0; i < 7; ++i) file.u32(i == 3 ? sect : 0);
    file.u32(uint32_t(blob.v.size()));
    file.v.insert(file.v.end(), blob.v.begin(), blob.v.end());
    ObjectRegistry reg;
    std::string err;
    CHECK(reg.addDefinitionFile(file.v.data(), file.v.size(), &err));
    CHECK(reg.typeCount() == 2 && reg.meshCount() == 2 && reg.groupCount() == 1);
    CHECK(reg.modelName(102) == "tree_l0" && reg.modelName(210) == "tree_l0");
    CHECK(reg.modelAnim(102) == "tree_idle.anm" && reg.anim(102, 2) == "tree_walk.anm");
    const ObjectType* t = reg.type(102);
    CHECK(t && t->objectClass == 60 && std::fabs(t->scale - 2.2f) < 1e-6f);
    // A corrupt group op is reported, not read past.
    file.v[32 + 4 + hdr + (g0 - hdr) + 4] = 0x99;
    ObjectRegistry r2;
    CHECK(!r2.addDefinitionFile(file.v.data(), file.v.size()));
}

static void testBurrows() {
    using namespace gvr;
    auto flat = [](float, float) { return 0.0f; };
    const Vec3 shot{0, 0, -1};
    std::vector<CreatureEvent> ev;
    // Buried: can't be shot or hit (the ground takes it), lies still while the player is far.
    CreatureDirector b;
    b.groundHeight = flat;
    for (int c = 0; c < int(CreatureClip::Count); ++c) b.clipDuration[c] = 0.6f;
    b.spawn({0, 0, -60}, 0, 1.0f, CreatureSize::Medium, 40.0f, true);
    b.bury(0);
    CHECK(CreatureDirector::underground(b.creatures[0]));
    float t; Vec3 n; int idx = -1;
    CHECK(!b.raycast({0, 0.5f, 0}, {0, 0, -1}, 100, t, n, idx));
    CHECK(!b.damage(0, 100.0f, shot) && b.creatures[0].health > 0);
    for (int f = 0; f < 120; ++f) b.update(1.0f / 60, Vec3{0, 0, 0}, true, ev);
    // (the blocked hit disturbed it: it tunnels at the player)
    CHECK(b.creatures[0].state == CreatureState::Tunnel);
    // Tunnels fast and bursts out right by the player, biting on the way up.
    bool emerged = false, struck = false;
    float when = 0;
    for (int f = 0; f < 900 && !struck; ++f) {
        b.update(1.0f / 60, Vec3{0, 0, 0}, true, ev);
        for (const auto& e : ev) {
            if (e.kind == CreatureEvent::Emerge && !emerged) { emerged = true; when = f / 60.0f; }
            if (e.kind == CreatureEvent::Strike) struck = true;
        }
    }
    CHECK(emerged && struck);
    CHECK(when < 10.0f);  // ~58 m at 8.5 m/s
    CHECK(length(b.creatures[0].pos) < 4.0f);
    // A large one ploughs along slower underground.
    CreatureDirector l;
    l.groundHeight = flat;
    l.spawn({0, 0, -60}, 0, 1.0f, CreatureSize::Large, 90.0f, true);
    l.bury(0);
    l.disturb({0, 0, -60}, 5.0f);
    for (int f = 0; f < 120; ++f) l.update(1.0f / 60, Vec3{0, 0, 0}, true, ev);
    CHECK(l.creatures[0].state == CreatureState::Tunnel);
    CHECK(length(l.creatures[0].pos - Vec3{0, 0, -60}) < 0.6f * (length(b.creatures[0].pos - Vec3{0, 0, -60})));
    // Far from the player and undisturbed, a buried one stays put.
    CreatureDirector q;
    q.groundHeight = flat;
    q.spawn({0, 0, -80}, 0, 1.0f, CreatureSize::Small, 20.0f, false);
    q.bury(0);
    for (int f = 0; f < 300; ++f) q.update(1.0f / 60, Vec3{0, 0, 0}, true, ev);
    CHECK(q.creatures[0].state == CreatureState::Buried && length(q.creatures[0].pos - Vec3{0, 0, -80}) < 1e-3f);
    // Not into the sea: tunnelling stops at the shore.
    CreatureDirector s;
    s.groundHeight = [](float, float z) { return z > -20.0f ? -2.0f : 0.0f; };
    s.spawn({0, 0, -40}, 0, 1.0f, CreatureSize::Medium, 40.0f, true);
    s.bury(0);
    s.disturb({0, 0, -40}, 5.0f);
    for (int f = 0; f < 300; ++f) s.update(1.0f / 60, Vec3{0, 0, 0}, true, ev);
    CHECK(s.creatures[0].pos.z < -19.0f);
    // A small one latches on now and then when it bites, then jumps off.
    bool latched = false, off = false;
    for (int trial = 0; trial < 20 && !(latched && off); ++trial) {
        CreatureDirector m;
        m.groundHeight = flat;
        for (int c = 0; c < int(CreatureClip::Count); ++c) m.clipDuration[c] = 0.6f;
        m.params.latchChance = 1.0f;
        m.spawn({0, 0, -3}, 0, 1.0f, CreatureSize::Small, 20.0f, false);
        for (int f = 0; f < 1200; ++f) {
            m.update(1.0f / 60, Vec3{0, 0, 0}, true, ev);
            for (const auto& e : ev) {
                if (e.kind == CreatureEvent::LatchOn) latched = true;
                if (e.kind == CreatureEvent::LatchOff) off = true;
            }
        }
    }
    CHECK(latched && off);
    std::printf("burrows ok\n");
}

static void testCreatures() {
    using namespace gvr;
    CreatureDirector cd;
    cd.groundHeight = [](float, float) { return 0.0f; };
    for (int c = 0; c < int(CreatureClip::Count); ++c) cd.clipDuration[c] = 0.6f;
    const int i = cd.spawn({0, 0, -15}, kPi, 1.0f, CreatureSize::Small, 20.0f, false);  // facing the player
    std::vector<CreatureEvent> ev;
    const Vec3 player{0, 0, 0};
    bool alerted = false, struck = false;
    float damage = 0;
    for (int f = 0; f < 600; ++f) {
        cd.update(1.0f / 60, player, true, ev);
        for (const auto& e : ev) {
            if (e.kind == CreatureEvent::Alert) alerted = true;
            if (e.kind == CreatureEvent::Strike) { struck = true; damage += e.damage; }
        }
    }
    CHECK(alerted);
    CHECK(struck && damage > 0);
    CHECK(std::fabs(cd.creatures[size_t(i)].pos.z) < 2.5f);  // closed in on the player
    // Out of sight: stays idle.
    CreatureDirector far;
    far.groundHeight = cd.groundHeight;
    far.spawn({0, 0, -60}, 0.0f, 1.0f, CreatureSize::Medium, 40.0f, true);
    for (int f = 0; f < 600; ++f) far.update(1.0f / 60, player, true, ev);
    CHECK(far.creatures[0].state == CreatureState::Idle && length(far.creatures[0].pos - Vec3{0, 0, -60}) <= far.params.patrolRadius + 0.5f);
    // Behind it, not close enough to be heard: it doesn't notice (sniffs about instead).
    CreatureDirector behind;
    behind.groundHeight = cd.groundHeight;
    behind.spawn({0, 0, -20}, 0.0f, 1.0f, CreatureSize::Small, 20.0f, false);
    for (int f = 0; f < 600; ++f) behind.update(1.0f / 60, player, true, ev);
    CHECK(behind.creatures[0].state == CreatureState::Idle);
    // Hit points come from the type: 20 (small) dies to one 25-damage bolt, 90 (large) takes four.
    const Vec3 shot{0, 0, -1};
    CHECK(cd.damage(i, 25.0f, shot));
    CHECK(cd.aliveCount() == 0 && cd.creatures[size_t(i)].clip == CreatureClip::Die);
    CreatureDirector big;
    big.spawn({0, 0, 0}, 0, 2.3f, CreatureSize::Large, 90.0f, true);
    CHECK(!big.damage(0, 25.0f, shot) && !big.damage(0, 25.0f, shot) && !big.damage(0, 25.0f, shot));
    std::vector<CreatureEvent> dev;
    CHECK(big.damage(0, 25.0f, shot, &dev) && !dev.empty() && dev.back().kind == CreatureEvent::Die);
    // Raycast hits the living capsule and passes over the dead.
    CreatureDirector rc;
    rc.spawn({0, 0, -10}, 0, 1.0f, CreatureSize::Medium, 40.0f, true);
    float t; Vec3 n; int idx = -1;
    CHECK(rc.raycast({0, 0.5f, 0}, {0, 0, -1}, 50, t, n, idx) && idx == 0 && std::fabs(t - 9.55f) < 0.1f);
    rc.damage(0, 100, shot);
    CHECK(!rc.raycast({0, 0.5f, 0}, {0, 0, -1}, 50, t, n, idx));
    // A cliff between creature and player blocks the chase.
    CreatureDirector cl;
    cl.groundHeight = [](float, float z) { return z > -8.0f ? 6.0f : 0.0f; };  // player on a 6 m ledge
    cl.spawn({0, 0, -12}, 0, 1.0f, CreatureSize::Small, 20.0f, false);
    for (int f = 0; f < 300; ++f) cl.update(1.0f / 60, Vec3{0, 6, 0}, true, ev);
    CHECK(cl.creatures[0].pos.z < -7.5f && cl.creatures[0].pos.y < 0.1f);
    // A medium Ripper at mid-range spits; the ball lobs over and bursts at the player.
    CreatureDirector sp;
    sp.groundHeight = [](float, float) { return 0.0f; };
    for (int c = 0; c < int(CreatureClip::Count); ++c) sp.clipDuration[c] = 0.8f;
    sp.spawn({0, 0, -24}, kPi, 1.0f, CreatureSize::Medium, 40.0f, true);
    sp.params.lungeMax = 0;  // isolate the spit
    bool launched = false, burst = false;
    float spitDmg = 0;
    for (int f = 0; f < 60 * 12 && !burst; ++f) {
        sp.update(1.0f / 60, player, true, ev);
        for (const auto& e : ev) {
            if (e.kind == CreatureEvent::SpitLaunch) launched = true;
            if (e.kind == CreatureEvent::SpitBurst && launched) { burst = true; spitDmg = e.damage; }
        }
    }
    CHECK(launched && burst);
    CHECK(spitDmg >= 0.0f && spitDmg <= sp.params.spitDamage[1]);
    // Small Rippers never spit.
    CreatureDirector sm;
    sm.groundHeight = sp.groundHeight;
    sm.spawn({0, 0, -15}, 0, 1.0f, CreatureSize::Small, 20.0f, true);
    bool smallSpat = false;
    for (int f = 0; f < 600; ++f) {
        sm.update(1.0f / 60, player, true, ev);
        for (const auto& e : ev) smallSpat |= e.kind == CreatureEvent::SpitLaunch;
    }
    CHECK(!smallSpat);
    // Steering: a wall between creature and player is walked around, not pushed against.
    CollisionWorld wall;
    {
        std::vector<Vec3> tris;
        auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) { tris.insert(tris.end(), {a, b, c, a, c, d}); };
        quad({-3, 0, -6}, {3, 0, -6}, {3, 3, -6}, {-3, 3, -6});
        quad({-3, 0, -6.4f}, {-3, 3, -6.4f}, {3, 3, -6.4f}, {3, 0, -6.4f});
        wall.addObject(tris);
    }
    CreatureDirector wa;
    wa.groundHeight = sp.groundHeight;
    wa.collision = &wall;
    wa.params.lungeMax = 0;
    wa.params.hearRange = 20.0f;  // knows where the player is, but the wall is in the way
    wa.spawn({0, 0, -12}, 0, 1.0f, CreatureSize::Small, 20.0f, false);
    bool reached = false;
    for (int f = 0; f < 60 * 15 && !reached; ++f) {
        wa.update(1.0f / 60, player, true, ev);
        for (const auto& e : ev) reached |= e.kind == CreatureEvent::Swing;
    }
    CHECK(reached);
}

static void testMusic() {
    using namespace gvr;
    // Songs: amb (a1,a2,a3 loop), bat (b1 -> chain batloop), batloop (b2,b3 loop), trn song (t1, b3).
    // Sets: bat -> {batloop, batx}. Pairs: a2 -> t1 (into "bat" via a variant), a1 -> b2.
    struct Song { const char* name; std::vector<const char*> segs; int end; const char* next; };
    std::vector<Song> songs = {{"amb", {"a1", "a2", "a3"}, 1, nullptr},
                               {"bat", {"b1"}, 2, "batloop"},
                               {"batloop", {"b2", "b3"}, 0, nullptr},
                               {"batx", {"t1", "b4"}, 1, nullptr}};
    std::vector<std::pair<const char*, const char*>> pairs = {{"a2", "t1"}, {"a1", "b2"}};
    std::vector<std::pair<const char*, const char*>> sets = {{"bat", "batloop"}, {"bat", "batx"}};
    Bytes b;
    b.u32(uint32_t(songs.size())).u32(uint32_t(pairs.size())).u32(uint32_t(sets.size()));
    const size_t offAt = b.v.size();
    for (size_t i = 0; i < songs.size(); ++i) b.u32(0);
    auto n16 = [&](const char* s) { b.str(s, 16); };
    for (auto& p : pairs) { n16(p.first); n16(p.second); }
    for (auto& p : sets) { n16(p.first); n16(p.second); }
    for (size_t i = 0; i < songs.size(); ++i) {
        const uint32_t o = uint32_t(b.v.size());
        std::memcpy(&b.v[offAt + 4 * i], &o, 4);
        n16(songs[i].name);
        for (auto* sg : songs[i].segs) n16(sg);
        b.u8(uint8_t(songs[i].end));
        if (songs[i].next) n16(songs[i].next); else { b.u8(0).u8(0).u8(0); }
    }
    MusicSet ms;
    std::string err;
    CHECK(ms.parse(b.v.data(), b.v.size(), &err));
    CHECK(ms.songCount() == 4 && ms.transitionCount() == 2);
    CHECK(ms.setOfSong("batloop") == "bat" && ms.setOfSong("batx") == "bat" && ms.setOfSong("amb") == "amb");
    CHECK(ms.transition("a2", "bat") == "t1" && ms.transition("a1", "bat") == "b2");
    MusicDirector md(ms);
    md.request("amb");
    CHECK(md.next() == "a1" && md.next() == "a2" && md.next() == "a3" && md.next() == "a1");  // loops
    md.next();                 // a2 queued
    md.request("bat");
    CHECK(md.next() == "t1");  // transition from a2 enters the variant song at its bridge
    CHECK(md.next() == "b4");
    md.request("amb");
    CHECK(md.next() == "a1");  // no transition from b4: the ambient song starts over
    md.request("bat");
    CHECK(md.next() == "b2" && md.next() == "b3" && md.next() == "b2");  // a1 -> b2, batloop loops
    MusicDirector m2(ms);
    m2.request("bat");
    CHECK(m2.next() == "b1" && m2.next() == "b2");  // the set's own song chains into its loop
    // Mixer: two queued buffers play back to back without a gap.
    AudioMixer mix(100);
    mix.setMasterVolume(1.0f);
    mix.setMusicVolume(1.0f);
    mix.queueMusic(std::vector<float>(2 * 50, 0.25f), 100);
    mix.queueMusic(std::vector<float>(2 * 50, 0.25f), 100);
    std::vector<float> out(2 * 120);
    mix.mix(out.data(), 120);
    bool continuous = true;
    for (int i = 0; i < 100; ++i) continuous &= std::fabs(out[2 * i] - std::tanh(0.25f)) < 1e-4f;
    CHECK(continuous);
    CHECK(std::fabs(out[2 * 110]) < 1e-6f && mix.musicQueued() == 0);
    std::vector<uint8_t> raw = {0x00, 0x40, 0x00, 0xC0};  // +0.5, -0.5
    auto f = decodeMusicRaw(raw.data(), raw.size());
    CHECK(f.size() == 2 && std::fabs(f[0] - 0.5f) < 1e-6f && std::fabs(f[1] + 0.5f) < 1e-6f);
}

static void testAffineInverse() {
    using namespace gvr;
    const Mat4 m = Mat4::trs({1, 2, 3}, Quat::fromAxisAngle({0.3f, 1, 0.2f}, 0.7f), {2, -1.5f, 0.5f});
    const Mat4 p = m * m.affineInverse();
    for (int k = 0; k < 16; ++k) CHECK(std::fabs(p.m[k] - Mat4::identity().m[k]) < 1e-4f);
}

static void testRescue() {
    using namespace gvr;
    RescueDirector r;
    // Land: y = 1 for x < 10, sea floor (-2) beyond. Pad: x in [4, 6], z in [-1, 1].
    r.groundHeight = [](float x, float) { return x < 10.0f ? 1.0f : -2.0f; };
    r.hasPad = true;
    r.padMin = {4, 0, -1};
    r.padMax = {6, 2, 1};
    const int a = r.add({0, 1, 0}, 0, 1.2f);
    std::vector<SmartieEvent> ev;
    Vec3 hands[2] = {{0.3f, 2.9f, 0.2f}, {50, 0, 0}};  // right hand above the Smartie's head (reach-down assist)
    float grip[2] = {1.0f, 0.0f};
    const float dt = 1.0f / 72.0f;
    r.update(dt, hands, grip, {0, 1, 3}, ev);
    CHECK(r.smarties[size_t(a)].state == SmartieState::Carried && r.holding(0));
    // Carry it over the pad and let go: it falls onto the pad and is saved.
    for (int f = 0; f < 72; ++f) { hands[0].x = 0.3f + 4.7f * float(f) / 71.0f; r.update(dt, hands, grip, {0, 1, 3}, ev); }
    grip[0] = 0.0f;
    hands[0] = {5.0f, 2.9f, 0.2f};  // held still, then let go
    for (int f = 0; f < 120; ++f) r.update(dt, hands, grip, {0, 1, 3}, ev);
    CHECK(r.smarties[size_t(a)].state == SmartieState::Saved);
    CHECK(r.savedCount() == 1);
    // A second Smartie dropped in the water returns to where it was stranded.
    const int b = r.add({1, 1, 2}, 0, 1.2f);
    hands[0] = {1.0f, 2.0f, 2.0f};
    grip[0] = 1.0f;
    r.update(dt, hands, grip, {0, 1, 3}, ev);
    CHECK(r.smarties[size_t(b)].state == SmartieState::Carried);
    hands[0] = {12.0f, 2.0f, 2.0f};
    r.update(dt, hands, grip, {0, 1, 3}, ev);
    grip[0] = 0.0f;
    for (int f = 0; f < 72 * 32; ++f) { hands[0] = {12.0f, 2.0f, 2.0f}; r.update(dt, hands, grip, {0, 1, 3}, ev); }
    bool splashed = false, returned = false;
    for (const auto& e : ev) {
        splashed |= e.kind == SmartieEvent::Splashed && e.index == b;
        returned |= e.kind == SmartieEvent::Returned && e.index == b;
    }
    CHECK(splashed && returned);
    CHECK(r.smarties[size_t(b)].state == SmartieState::Idle && length(r.smarties[size_t(b)].pos - Vec3{1, 1, 2}) < 1e-4f);
    // A hanging Smartie over the water: it holds on for a while, slips, lets go, falls in, is
    // eaten, and hangs there again later; grabbing it off the hang point works.
    {
        RescueDirector h;
        h.groundHeight = [](float, float) { return -5.0f; };  // all sea
        h.holdMin = h.holdMax = 10.0f;
        h.warnTime = 3.0f;
        h.respawnTime = 5.0f;
        const int k = h.add({0, -5, 0}, 0, 1.2f);
        h.setHang(k, {0, 20, 0});
        CHECK(h.smarties[size_t(k)].state == SmartieState::Hanging);
        std::vector<SmartieEvent> he;
        Vec3 hh[2] = {{50, 0, 0}, {50, 0, 0}};
        float hg[2] = {0, 0};
        bool slipped = false, letGo = false, died = false, back = false;
        for (int f = 0; f < 72 * 30; ++f) {
            h.update(dt, hh, hg, {0, 0, 10}, he);
            for (const auto& e : he) {
                slipped |= e.kind == SmartieEvent::Slipping;
                letGo |= e.kind == SmartieEvent::LetGo;
                died |= e.kind == SmartieEvent::Died;
                back |= e.kind == SmartieEvent::Returned;
            }
            he.clear();
        }
        CHECK(slipped && letGo && died && back);
        CHECK(h.smarties[size_t(k)].state == SmartieState::Hanging);
        hh[0] = {0.2f, 19.5f, 0.1f};  // a hand on its body, just below its hands
        hg[0] = 1.0f;
        h.update(dt, hh, hg, {0, 0, 10}, he);
        CHECK(h.smarties[size_t(k)].state == SmartieState::Carried);
    }
    // Out of reach: nothing is grabbed.
    hands[0] = {3.0f, 2.0f, 2.0f};
    grip[0] = 1.0f;
    r.update(dt, hands, grip, {0, 1, 3}, ev);
    CHECK(!r.holding(0));
}

static void testGameText() {
    using namespace gvr;
    // n=2, keys "A_1\0b\0", values "Hello\0World\0"
    std::vector<uint8_t> d;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (8 * i))); };
    const std::string keys("A_1\0b\0", 6), vals("Hello\0World\0", 12);
    u32(2); u32(uint32_t(keys.size())); u32(uint32_t(vals.size()));
    u32(0); u32(0); u32(4); u32(6);
    d.insert(d.end(), keys.begin(), keys.end());
    d.insert(d.end(), vals.begin(), vals.end());
    GameText gt;
    CHECK(gt.parse(d.data(), d.size()));
    CHECK(gt.size() == 2);
    CHECK(gt.get("a_1") == "Hello" && gt.get("B") == "World" && gt.get("c").empty());
    d.resize(d.size() - 3);  // truncated value block must be rejected, not read past the end
    GameText bad;
    std::string err;
    CHECK(!bad.parse(d.data(), d.size(), &err));
}

static void testGore() {
    using namespace gvr;
    // Blood: drops fly, land as splats on the ground and as clouds on the water; splats fade.
    BloodSystem b;
    b.groundHeight = [](float x, float) { return x < 5.0f ? 1.0f : -3.0f; };  // land, then sea
    b.seaLevel = 0.0f;
    b.burst({0, 2, 0}, {0, 1, 0}, 10, 3.0f, 0.1f);
    CHECK(b.drops.size() == 10 && b.sprays.size() == 1);
    for (int i = 0; i < 300; ++i) b.update(1.0f / 60);
    CHECK(b.drops.empty() && b.sprays.empty() && b.decals.size() == 10);
    bool onGround = true;
    for (const auto& d : b.decals) onGround &= !d.water && std::fabs(d.pos.y - 1.0f) < 1e-3f;
    CHECK(onGround);
    b.pool({8, -3, 0}, 1.0f);
    CHECK(b.decals.back().water && std::fabs(b.decals.back().pos.y) < 0.05f);
    for (int i = 0; i < 60 * 70; ++i) b.update(1.0f / 60);
    CHECK(b.decals.empty());  // all faded out
    for (int i = 0; i < 200; ++i) b.pool({0, 1, 0}, 1.0f);
    CHECK(b.decals.size() == b.maxDecals);

    // Cleaners: come to a body, eat it (it is reported eaten), then go.
    CleanerDirector c;
    c.groundHeight = [](float, float) { return 0.0f; };
    c.call(7, {10, 0, 0}, 2, 0.0f);
    CHECK(c.cleaners.size() == 2 && c.busyWith(7));
    float eaten = 0;
    bool started = false, gone = false;
    std::vector<std::pair<uint32_t, float>> e;
    std::vector<CleanerEvent> ev;
    for (int i = 0; i < 60 * 60 && !gone; ++i) {
        e.clear();
        ev.clear();
        c.update(1.0f / 60, [&](uint32_t id, Vec3& at) { at = {10, 0, 0}; return id == 7 && eaten < 1.0f; }, e, ev);
        for (const auto& pr : e) if (pr.first == 7) eaten += pr.second;
        for (const auto& x : ev) { started |= x.kind == CleanerEvent::StartEat; gone |= x.kind == CleanerEvent::Gone; }
    }
    CHECK(started && eaten >= 1.0f && gone);
}

static void testDestruction() {
    using namespace gvr;
    DestructionDirector d;
    std::vector<DestructEvent> ev;
    const int house = d.add(0, DestructKind::House, {0, 0, 0}, 6, 4, 330);
    const int tree = d.add(1, DestructKind::Tree, {10, 0, 0}, 8, 2, 60);
    // Bullets barely scratch a house; blasts take it down through the stages.
    for (int i = 0; i < 20; ++i) d.damage(house, 25, false, {0, 0, 20}, ev);  // 150 of 330
    CHECK(d.items[house].stage == 1 && d.items[house].alive());
    bool collapse = false;
    for (int i = 0; i < 4 && !collapse; ++i) collapse = d.damage(house, 120, true, {0, 0, 20}, ev);
    CHECK(collapse && d.items[house].stage == 3);
    CHECK(std::fabs(d.items[house].fallDir.z + 1.0f) < 1e-3f);  // falls away from the blast
    // A rocket fells a tree; it lies down after its fall time.
    ev.clear();
    CHECK(d.damage(tree, 120, true, {12, 0, 0}, ev));
    bool fallen = false, down = false;
    for (int i = 0; i < 300; ++i) {
        ev.clear();
        d.update(1.0f / 60, ev);
        for (const auto& e : ev) { fallen |= e.kind == DestructEvent::Fallen && e.index == tree; down |= e.kind == DestructEvent::Down && e.index == tree; }
    }
    CHECK(fallen && down && d.items[tree].stage == 4);
    CHECK(DestructionDirector::fallAngle(0, 2) == 0.0f && std::fabs(DestructionDirector::fallAngle(2, 2) - 1.5708f) < 1e-3f);
    // Particles age out.
    ParticleSystem ps;
    Particle p;
    p.life = 0.5f;
    ps.emit(p);
    for (int i = 0; i < 40; ++i) ps.update(1.0f / 60);
    CHECK(ps.parts.empty());
}

static void testHudGauge() {
    // A ring outline (walls between radius 10 and 12) on a 32x32 image.
    Image img;
    img.width = img.height = 32;
    img.rgba.assign(32 * 32 * 4, 0);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float r = std::sqrt(float((x - 16) * (x - 16) + (y - 16) * (y - 16)));
            if (r >= 10.0f && r <= 12.0f) img.rgba[size_t(y * 32 + x) * 4 + 3] = 255;
        }
    const auto spans = gaugeInterior(img, 16, 16, 2);
    CHECK(!spans.empty());
    float top = 1, bottom = 0;
    for (const auto& s : spans) {
        CHECK(s.x0 < s.x1 && s.v0 < s.v1);
        CHECK(s.x0 >= 5.0f / 32 && s.x1 <= 27.0f / 32);  // inside the ring (plus the 1 px overlap)
        top = std::min(top, s.v0);
        bottom = std::max(bottom, s.v1);
    }
    CHECK(top >= 5.0f / 32 && bottom <= 27.0f / 32);
    CHECK(gaugeInterior(img, 2, 2, 2).empty());    // outside: leaks to the border
    CHECK(gaugeInterior(img, 16, 5, 2).empty());   // on the wall
    // The player's one-shot action (injection) plays, slows walking, and ends.
    PlayerController pc;
    pc.groundHeight = [](float, float) { return 0.0f; };
    pc.teleport({0, 0, 0}, 0);
    pc.startAction(PlayerAnim::Inject, 0.5f);
    PlayerInput in;
    in.move = {0, 1};
    pc.update(0.1f, in);
    CHECK(pc.anim() == PlayerAnim::Inject && pc.inAction());
    CHECK(pc.horizontalSpeed() < 0.5f * pc.params.runSpeed);
    for (int i = 0; i < 10; ++i) pc.update(0.1f, in);
    CHECK(!pc.inAction() && pc.anim() == PlayerAnim::Run);
    // Ripper hit box: a long low body, turned 90 degrees; shots along its length hit its flank.
    CreatureDirector rb;
    rb.params.hasBox = true;
    rb.params.boxMin = {-0.6f, 0.0f, -1.0f};
    rb.params.boxMax = {0.6f, 1.0f, 1.0f};
    rb.spawn({0, 0, 0}, 0.5f * kPi, 1.0f, CreatureSize::Medium, 40.0f, false);
    float rt = 0;
    Vec3 rn;
    int ri = -1;
    // Turned 90 deg, the body's length lies along x: a ray down +z 0.9 m off-centre in x hits.
    CHECK(rb.raycast({0.9f, 0.5f, -5.0f}, {0, 0, 1}, 50.0f, rt, rn, ri) && ri == 0 && near(rt, 4.4f, 1e-3f) && rn.z < -0.9f);
    CHECK(!rb.raycast({0.0f, 1.3f, -5.0f}, {0, 0, 1}, 50.0f, rt, rn, ri));   // over its back
    CHECK(!rb.raycast({1.2f, 0.5f, -5.0f}, {0, 0, 1}, 50.0f, rt, rn, ri));   // past its tail
    // Burn overlays: coverage grows light < medium < heavy; embers exist; burn level follows hp.
    std::vector<uint8_t> ct[3], emb;
    makeBurnTextures(64, 7u, ct, emb);
    double cov[3] = {};
    for (int k = 0; k < 3; ++k)
        for (size_t i = 3; i < ct[k].size(); i += 4) cov[k] += ct[k][i];
    CHECK(cov[0] > 0 && cov[0] < cov[1] && cov[1] < cov[2]);
    size_t hot = 0;
    for (size_t i = 0; i < emb.size(); i += 4) hot += emb[i] > 128;
    CHECK(hot > 0 && hot < emb.size() / 4 / 2);
    Destructible bd;
    bd.hp = 25; bd.maxHp = 100;
    CHECK(near(burnLevel(bd), 0.75f, 1e-4f));
    bd.stage = 3;
    CHECK(burnLevel(bd) == 1.0f);
    std::printf("hud gauge / player action / hit box / burn ok\n");
}

static void testRace() {
    // A square course: start at the origin, gates at the corners, back to the start.
    RaceDirector r;
    const std::vector<Vec3> gates{{0, 0, -100}, {100, 0, -100}, {100, 0, 0}};
    const Vec3 finish{0, 0, 0};
    std::vector<std::vector<Vec3>> lines{{finish, gates[0], gates[1], gates[2], finish}};
    r.setup(gates, lines, 1, &finish);
    CHECK(!r.started() && r.racers.size() == 1 && near(r.racers[0].length, 400.0f));
    // Countdown, then drive the course at 25 m/s (the rival cruises slower).
    std::vector<Vec3> route{finish, gates[0], gates[1], gates[2], finish};
    Vec3 p = finish;
    size_t leg = 0;
    float t = 0;
    while (t < 60.0f && !r.playerFinished) {
        const float dt = 1.0f / 72.0f;
        t += dt;
        if (r.started() && leg + 1 < route.size()) {
            Vec3 d = route[leg + 1] - p;
            const float l = length(d);
            if (l < 25.0f * dt) { p = route[leg + 1]; ++leg; }
            else p = p + d * (25.0f * dt / l);
        }
        r.update(dt, p);
    }
    CHECK(r.playerFinished && r.playerPlace == 1 && r.missed == 0 && r.nextGate == 3);
    // Skipping gates: three misses lose it.
    RaceDirector r2;
    const std::vector<Vec3> g2{{0, 0, -50}, {0, 0, -100}, {0, 0, -150}, {0, 0, -200}, {0, 0, -250}};
    r2.setup(g2, {}, 0, nullptr);
    for (int f = 0; f < 72 * 4; ++f) r2.update(1.0f / 72.0f, Vec3{0, 0, 0});
    r2.update(1.0f / 72.0f, Vec3{0, 0, -150});  // jumped to gate 3: gates 1 and 2 missed
    CHECK(r2.missed == 2 && !r2.lost && r2.nextGate == 3);
    r2.update(1.0f / 72.0f, Vec3{40, 0, -250});  // past gate 4 but wide of it - not counted
    r2.update(1.0f / 72.0f, Vec3{0, 0, -250});   // gate 5 taken, gate 4 missed: the third
    CHECK(r2.lost);
    // Jetski handling: full throttle reaches top speed; land ahead stops it.
    JetskiState s;
    JetskiParams jp;
    auto sea = [](float, float) { return -5.0f; };
    for (int f = 0; f < 72 * 5; ++f) stepJetski(s, jp, 1.0f, 0.0f, false, 1.0f / 72.0f, sea, 0.0f);
    CHECK(near(s.speed, jp.maxSpeed) && s.pos.z < -60.0f && near(s.pos.y, 0.0f));
    auto wall = [](float, float z) { return z < -10.0f ? 30.0f : -5.0f; };
    JetskiState s2;
    bool bumped = false;
    for (int f = 0; f < 72 * 3; ++f) bumped |= stepJetski(s2, jp, 1.0f, 0.0f, false, 1.0f / 72.0f, wall, 0.0f);
    CHECK(bumped && s2.pos.z > -10.5f);
    // Stick right turns right (toward +X when facing -Z).
    JetskiState s3;
    s3.speed = 15.0f;
    for (int f = 0; f < 72; ++f) stepJetski(s3, jp, 1.0f, 1.0f, false, 1.0f / 72.0f, sea, 0.0f);
    CHECK(s3.pos.x > 3.0f);
}

static void testVimps() {
    VimpDirector d;
    d.groundHeight = [](float, float) { return 0.0f; };
    const int h = d.addHerd({0, 0, 0});
    for (int k = 0; k < 4; ++k) d.add(h, {float(k) * 2.0f, 0, 0}, 0.0f);
    // Calm: they graze where they are.
    for (int f = 0; f < 72; ++f) d.update(1.0f / 72.0f, {}, {});
    CHECK(d.vimps.size() == 4 && d.aliveCount() == 4);
    // The player walks up from +X: the herd stampedes away (toward -X).
    for (int f = 0; f < 72 * 3; ++f) d.update(1.0f / 72.0f, {Vec3{12, 0, 0}}, {});
    float mx = 0;
    for (const auto& v : d.vimps) mx += v.pos.x;
    CHECK(mx / 4.0f < -8.0f);
    CHECK(d.herds[0].calm < 5.0f);
    // 20 hp: a 25-damage shot kills, a 10 only hurts.
    CHECK(!d.damage(0, 10.0f, {5, 0, 0}) && d.vimps[0].state == VimpState::Hit);
    CHECK(d.damage(1, 25.0f, {5, 0, 0}) && d.vimps[1].state == VimpState::Dead && d.aliveCount() == 3);
    // Carcasses go after a while.
    for (int f = 0; f < 72 * 25; ++f) d.update(1.0f / 72.0f, {}, {});
    CHECK(d.vimps.size() == 3);
}

static void testVillagers() {
    VillagerDirector vd;
    vd.groundHeight = [](float, float) { return 0.0f; };
    vd.add({0, 0, 0}, 0.0f);
    const Vec3 far{500, 0, 500};
    std::vector<Vec3> none;
    // Calm: they go about their day (acts and strolls), staying near home.
    bool walked = false;
    for (int i = 0; i < 72 * 120; ++i) {
        vd.update(1.0f / 72.0f, far, none, none);
        walked |= vd.villagers[0].state == VillagerState::Walk;
        CHECK(length(vd.villagers[0].pos - vd.villagers[0].home) < vd.params.homeRadius + 0.5f);
    }
    CHECK(walked);
    // A Ripper close by: they run away from it, then cower, and calm down after it's gone.
    const Vec3 start = vd.villagers[0].pos;
    std::vector<Vec3> threat{start + Vec3{3, 0, 0}};
    vd.update(1.0f / 72.0f, far, threat, none);
    CHECK(vd.villagers[0].state == VillagerState::Flee && vd.villagers[0].clip == VillagerClip::Run);
    for (int i = 0; i < 72 * 3; ++i) vd.update(1.0f / 72.0f, far, threat, none);
    CHECK(vd.villagers[0].pos.x < start.x);  // away from the threat (on +x)
    for (int i = 0; i < 72 * 8; ++i) vd.update(1.0f / 72.0f, far, threat, none);
    CHECK(vd.villagers[0].state == VillagerState::Cower);
    for (int i = 0; i < 72 * 8; ++i) vd.update(1.0f / 72.0f, far, none, none);
    CHECK(vd.villagers[0].state != VillagerState::Cower && vd.villagers[0].state != VillagerState::Flee);
    // The Meccaryn comes close: a wave.
    bool waved = false;
    for (int i = 0; i < 72 * 3 && !waved; ++i) {
        vd.update(1.0f / 72.0f, vd.villagers[0].pos + Vec3{1.5f, 0, 0}, none, none);
        waved = vd.villagers[0].state == VillagerState::Wave;
    }
    CHECK(waved);
    std::printf("villagers ok\n");
}

static void testSaveGame() {
    const std::string path = "gvr_test_save.txt";
    SaveGame a;
    a.world = "Story2";
    a.mission = "2b_1_savewife";
    a.syringes = 4;
    a.launcher = true;
    a.guns[0] = true;
    a.ammo[0] = 17;
    a.guns[2] = true;  // the sniper, out of ammo
    a.bows[0] = true;  // the plain bow: unlimited arrows
    a.bows[2] = true;
    a.arrows[2] = 5;
    a.bows[4] = true;  // the power-up bow
    a.arrows[4] = 2;
    a.rockets = 7;
    a.mines = 3;
    CHECK(writeSaveGame(path, a));
    SaveGame b;
    CHECK(loadSaveGame(path, b) && b.world == a.world && b.mission == a.mission && b.syringes == 4 && b.launcher);
    CHECK(b.guns[0] && b.ammo[0] == 17 && !b.guns[1] && b.guns[2] && b.ammo[2] == 0);
    CHECK(b.bows[0] && b.arrows[0] == -1 && !b.bows[1] && b.bows[2] && b.arrows[2] == 5);
    CHECK(b.bows[4] && b.arrows[4] == 2 && !b.bows[3] && b.rockets == 7 && b.mines == 3);
    {  // a damaged / foreign file is not a save
        FILE* f = std::fopen(path.c_str(), "w");
        std::fputs("world=Story1\n", f);
        std::fclose(f);
    }
    SaveGame c;
    CHECK(!loadSaveGame(path, c));
    std::remove(path.c_str());
    CHECK(!loadSaveGame(path, c));
    UserSettings us;
    us.turn = 3; us.leftHanded = true; us.thirdPerson = true; us.hudSize = 2; us.volume = 4; us.music = 0; us.subtitles = false; us.foveation = 3;
    CHECK(writeSettings(path, us));
    UserSettings ul;
    CHECK(loadSettings(path, ul) && ul.turn == 3 && ul.leftHanded && ul.thirdPerson && ul.hudSize == 2 && ul.volume == 4 &&
          ul.music == 0 && !ul.subtitles && ul.foveation == 3 && near(ul.hudScale(), 1.3f, 1e-6f));
    CHECK(UserSettings{}.foveation == 2);  // an old settings file without the key: medium
    std::remove(path.c_str());
    std::printf("save game / settings ok\n");
}

static void testFlickVisibility() {
    FlickShot s;
    s.visibility.push_back({0.0f, 6, false});  // story1_jetpack_6: the rider hidden at the start
    s.visibility.push_back({0.87f, 6, true});  // shown when he climbs on
    s.visibility.push_back({0.87f, 2, false}); // the standing Timmy goes
    CHECK(!s.actorShownAt(6, 0.0f) && !s.actorShownAt(6, 0.5f) && s.actorShownAt(6, 1.0f));
    CHECK(s.actorShownAt(2, 0.5f) && !s.actorShownAt(2, 1.0f) && s.actorShownAt(3, 1.0f));
}

static void testFlickEffect() {
    // header, op 0x27 {slot 0, "fx_a", mode 1, actor 4, "ref_nozzle"}, op 5 (yield), strings
    std::vector<uint32_t> w = {3, 0, 0, 0, 0, 0, 0x27, 7, 0, 0, 1, 4, 0, 5, 2};
    const uint32_t strOff = uint32_t(w.size() * 4);
    w[9] = strOff;
    w[12] = strOff + 8;
    std::vector<uint8_t> d(w.size() * 4);
    std::memcpy(d.data(), w.data(), d.size());
    const char strs[] = "fx_a\0\0\0\0ref_nozzle\0\0";
    d.insert(d.end(), strs, strs + sizeof(strs) - 1);
    const uint32_t total = uint32_t(d.size());
    std::memcpy(d.data() + 4, &total, 4);
    FlickShot s;
    std::string err;
    CHECK(parseFlick(d.data(), d.size(), s, &err));
    CHECK(s.effects.size() == 1 && s.effects[0].name == "fx_a" && s.effects[0].node == "ref_nozzle" &&
          s.effects[0].actor == 4 && s.effects[0].mode == 1 && s.effects[0].time == 0.0f);
}

static void testTestScript() {
    TestScript ts;
    std::string err;
    CHECK(ts.parse("name village  # a comment\nworld Story1\nmission 1a_x\nenv GVR_CUTSCENE none\nview third\nat 100 200\nlook 100 300\n"
                   "duration 12\nmove 0 1 2 4\nhold trigger_r 3 5\nhand_r 0 0 -1 6 8\nblast 1 2 3 4 5\nshot 6 town\n", err));
    CHECK(ts.name == "village" && ts.world == "Story1" && ts.mission == "1a_x" && !ts.firstPerson && ts.duration == 12.0f);
    CHECK(ts.env.size() == 1 && ts.env[0].first == "GVR_CUTSCENE");
    // Facing +game y = runtime +z: yaw pi.
    CHECK(std::fabs(std::fabs(ts.startYaw()) - 3.14159265f) < 1e-3f);
    InputFrame in;
    ts.inputAt(1.0f, in);
    CHECK(in.hand[kLeft].thumbstick.y == 0.0f && in.hand[kRight].trigger == 0.0f);
    ts.inputAt(3.5f, in);
    CHECK(in.hand[kLeft].thumbstick.y == 1.0f && in.hand[kRight].trigger == 1.0f);
    ts.inputAt(7.0f, in);  // half way along the hand path
    CHECK(std::fabs(in.hand[kRight].gripPose.position.z - (-0.35f - 0.5f)) < 1e-4f);
    ts.inputAt(9.0f, in);  // and it stays there
    CHECK(std::fabs(in.hand[kRight].gripPose.position.z - (-1.35f)) < 1e-4f);
    CHECK(ts.blastsIn(4.9f, 5.0f).size() == 1 && ts.blastsIn(5.0f, 5.1f).empty());
    {
        TestScript r;
        std::string e2;
        CHECK(r.parse("reapers 6 8 2 5\n", e2));
        CHECK(r.reapersIn(1.9f, 2.0f).size() == 1 && !r.reapersIn(1.9f, 2.0f)[0]->kill && r.reapersIn(1.9f, 2.0f)[0]->n == 6);
        CHECK(r.reapersIn(4.9f, 5.0f).size() == 1 && r.reapersIn(4.9f, 5.0f)[0]->kill);
        CHECK(!r.parse("reapers 6 8 5 2\n", e2));  // killed before they come
    }
    CHECK(ts.shotsIn(5.99f, 6.01f).size() == 1);
    const Pose l = ts.eyePose(0), r = ts.eyePose(1);
    CHECK(std::fabs(length(r.position - l.position) - 0.064f) < 1e-4f);
    TestScript bad;
    CHECK(!bad.parse("hold nosuchbutton 1 2\n", err) && err.find("line 1") == 0);
}

static void testWeaponDefs() {
    // Synthetic file: one weapon {id 0, proj 793, flag 57, dmg 80, every 1.5 s, speed 2500, range 8000,
    // fx "fx_s"}, end; the item record {36=6, 39=787, 47=1, 54=2, 48="IconS", 37 flag, 36=10, 47=3}.
    auto f = [](float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; };
    std::vector<uint32_t> w = {1, 0,
                               1, 3, 0, 3, 3, 793, 57, 2, 10, 3, f(80.0f), 13, 3, f(1.5f), 24, 3, f(2500.0f), 34, 3, f(8000.0f), 22, 3, 0,
                               9, 6, f(60.0f), 0, f(30.0f), f(70.0f), 2, 2,
                               36, 3, 6, 39, 3, 787, 47, 3, 1, 54, 3, 2, 45, 3, 0xFFFFFFFFu, 48, 3, 0, 37, 2, 36, 3, 10, 47, 3, 3};
    const uint32_t strOff = uint32_t(w.size() * 4);
    w[24] = strOff;      // "fx_s"
    w[w.size() - 9] = strOff + 5;   // "IconS"
    std::vector<uint8_t> d(w.size() * 4);
    std::memcpy(d.data(), w.data(), d.size());
    const char strs[] = "fx_s\0IconS\0";
    d.insert(d.end(), strs, strs + sizeof(strs) - 1);
    const uint32_t total = uint32_t(d.size());
    std::memcpy(d.data() + 4, &total, 4);
    WeaponDefs defs;
    std::string err;
    CHECK(parseWeaponDefs(d.data(), d.size(), defs, &err));
    CHECK(defs.weapons.size() == 1 && defs.items.size() == 2);
    const WeaponDef* s = defs.weapon(0);
    CHECK(s && s->projectileType == 793 && s->damage == 80.0f && s->interval == 1.5f && s->speed == 2500.0f && s->range == 8000.0f &&
          s->fxProjectile == "fx_s" && s->splash[0] == 60.0f && s->splash[3] == 70.0f);
    CHECK(defs.weaponByFx("fx_s", 1.5f) == s && !defs.weaponByFx("fx_s", 0.2f));
    const ItemDef* i = defs.item(6);
    CHECK(i && i->objectType == 787 && i->category == 1 && i->race == 2 && i->maxAmmo == -1 && i->icon == "IconS");
    CHECK(defs.item(10) && defs.item(10)->category == 3 && !defs.item(7));
    d[0] = 2;
    CHECK(!parseWeaponDefs(d.data(), d.size(), defs, &err));
}

static void testSoundTable() {
    // 0x1A0002E5 file whose section 2 holds two sounds: 7 -> "A_B.wav", 9 -> "c.wav".
    std::vector<uint8_t> d(32 + 20 + 2 * 40, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); };
    put(0, 0x1A0002E5u);
    const uint32_t sec = 32;
    put(4 + 2 * 4, sec);
    put(sec, 0xFFFFFFFFu);
    put(sec + 4, 0x33221102u);
    put(sec + 8, 2);
    put(sec + 12, 40);
    const uint32_t pool = uint32_t(d.size()) - sec;
    const char names[] = "A_B.wav\0c.wav\0";
    d.insert(d.end(), names, names + sizeof(names) - 1);
    put(sec + 16, uint32_t(d.size()) - sec);
    put(sec + 20, 7); put(sec + 20 + 36, pool);
    put(sec + 60, 9); put(sec + 60 + 36, pool + 8);
    ObjectRegistry reg;
    std::string err;
    CHECK(reg.addSoundTable(d.data(), d.size(), &err));
    CHECK(reg.soundCount() == 2 && reg.soundName(7) == "a_b" && reg.soundName(9) == "c" && reg.soundName(8).empty());
}

static void testFxLibrary() {
    // Two effects: "a" = {03 type 794, 06, 1b 2.0, 07, 02}; "b" = {0a 1 "a" 0, 2b 5 6, 02}.
    std::vector<uint8_t> d;
    auto u32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) d.push_back(uint8_t(v >> (8 * k))); };
    auto f32 = [&](float f) { uint32_t v; std::memcpy(&v, &f, 4); u32(v); };
    auto str = [&](const char* s) { while (*s) d.push_back(uint8_t(*s++)); d.push_back(0); };
    u32(1); u32(0); u32(2);
    str("a"); const size_t offA = d.size(); u32(0);
    str("B"); const size_t offB = d.size(); u32(0);
    const size_t startA = d.size();
    d.push_back(0x03); u32(794); d.push_back(0x06); d.push_back(0x1b); f32(2.0f); d.push_back(0x07); d.push_back(0x02);
    const size_t startB = d.size();
    d.push_back(0x0a); u32(1); str("a"); u32(0); d.push_back(0x2b); u32(5); u32(6); d.push_back(0x02);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); };
    put(offA, uint32_t(startA - 12));
    put(offB, uint32_t(startB - 12));
    put(4, uint32_t(d.size() - 12));
    FxLibrary lib;
    std::string err;
    CHECK(parseFxLibrary(d.data(), d.size(), lib, &err));
    const FxDef* a = lib.find("A");
    const FxDef* b = lib.find("b");
    CHECK(a && a->ops.size() == 5 && a->ops[0].op == 0x03 && a->ops[0].u[0] == 794 && a->ops[2].f[0] == 2.0f);
    CHECK(b && b->ops.size() == 3 && b->ops[0].s == "a" && b->ops[0].u.size() == 2 && b->ops[1].u[1] == 6);
    // A wrong argument length desyncs the stream and is caught.
    d[startA + 6] = 0x55;  // the 1b op becomes 0x55 (no arguments): its float bytes then read as ops
    CHECK(!parseFxLibrary(d.data(), d.size(), lib, &err));
}

static void testFxAttach() {
    // "r" = {0b 0.05, 24 800 821 0, 06, 21, 0a 0 "f" 0, 20 colours, 22 2 0.001, 25 1.0, 07, 02} (a rocket's trail),
    // "f" = {03 820, 06, 04 3 0 0.001, 07, 02} (the flame riding it).
    std::vector<uint8_t> d;
    auto u32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) d.push_back(uint8_t(v >> (8 * k))); };
    auto f32 = [&](float f) { uint32_t v; std::memcpy(&v, &f, 4); u32(v); };
    auto str = [&](const char* s) { while (*s) d.push_back(uint8_t(*s++)); d.push_back(0); };
    u32(1); u32(0); u32(2);
    str("r"); const size_t offR = d.size(); u32(0);
    str("f"); const size_t offF = d.size(); u32(0);
    const size_t startR = d.size();
    d.push_back(0x0b); f32(0.05f);
    d.push_back(0x24); u32(800); u32(821); u32(0);
    d.push_back(0x06); d.push_back(0x21);
    d.push_back(0x0a); u32(0); str("f"); u32(0);
    d.push_back(0x20); for (float c : {0.4f, 0.8f, 0.9f, 0.3f, 0.7f, 0.8f}) f32(c);
    d.push_back(0x22); f32(2.0f); f32(0.001f);
    d.push_back(0x25); f32(1.0f);
    d.push_back(0x07); d.push_back(0x02);
    const size_t startF = d.size();
    d.push_back(0x03); u32(820); d.push_back(0x06); d.push_back(0x04); f32(3.0f); f32(0.0f); f32(0.001f); d.push_back(0x07); d.push_back(0x02);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); };
    put(offR, uint32_t(startR - 12));
    put(offF, uint32_t(startF - 12));
    put(4, uint32_t(d.size() - 12));
    FxLibrary lib;
    std::string err;
    CHECK(parseFxLibrary(d.data(), d.size(), lib, &err));
    FxPlayer fx;
    fx.init(&lib, nullptr, nullptr, nullptr);
    // Played free, an object-riding emitter has nothing to ride: nothing starts.
    fx.play("r", {0, 0, 0});
    fx.update(0.1f);
    CHECK(fx.particles.empty());
    const uint32_t h = fx.attach("r", {0, 0, 0});
    CHECK(h != 0 && fx.particles.size() == 1 && fx.particles[0].attached && fx.particles[0].streak && fx.particles[0].ref);
    CHECK(std::fabs(fx.particles[0].sw0 - 2.0f * gGameUnitsToMeters) < 1e-5f && fx.particles[0].sc1[2] == 0.8f);
    // Flying 4 s (longer than the flame's 3 s life): the flame rides it all the way; the streak follows.
    Vec3 at{0, 0, 0};
    for (int i = 0; i < 240; ++i) {
        at.z -= 0.5f;
        CHECK(fx.moveAttached(h, at, Quat{}));
        fx.update(1.0f / 60.0f);
    }
    const FxPlayer::Particle* root = nullptr;
    const FxPlayer::Particle* flame = nullptr;
    for (const auto& p : fx.particles) (p.id == h ? root : flame) = &p;
    CHECK(root && flame && length(flame->pos - at) < 1e-4f);
    CHECK(root->streakPts.size() >= 40 && root->streakPts.front().age <= 1.0f + 1e-3f);
    // Landed: the flame goes out at once, the streak fades over its second, then nothing is left.
    fx.detach(h);
    for (int i = 0; i < 12; ++i) fx.update(1.0f / 60.0f);
    CHECK(fx.particles.size() == 1 && !fx.particles[0].attached && !fx.moveAttached(h, at, Quat{}));
    for (int i = 0; i < 70; ++i) fx.update(1.0f / 60.0f);
    CHECK(fx.particles.empty());
    // One the caller stops moving (say, its projectile was cleared) ends by itself.
    const uint32_t h2 = fx.attach("r", at);
    for (int i = 0; i < 120; ++i) fx.update(1.0f / 60.0f);
    CHECK(h2 != 0 && fx.particles.empty());
}

static void testSkirmish() {
    // Two Meccaryn teams on flat ground, bases 200 m apart, no walls.
    Skirmish sk;
    sk.groundAt = [](float, float) { return 0.0f; };
    std::vector<Vec3> markers;
    for (int k = 0; k < 4; ++k) { markers.push_back({-100.0f + 5.0f * float(k), 0, 10}); markers.push_back({100.0f + 5.0f * float(k), 0, 10}); }
    markers.push_back({0, 0, 500});  // far from both: not a spawn
    sk.setup({SkHero::Mecc, SkHero::Mecc}, {{-100, 0, 0}, {100, 0, 0}}, markers);
    CHECK(sk.teams.size() == 2 && sk.teams[0].spawns.size() == 4 && sk.teams[1].spawns.size() == 4 && sk.teams[0].name == "Blue");
    PlayerController proto;
    for (int k = 0; k < 3; ++k) { sk.addPlayer("b" + std::to_string(k), 0, true, false, proto); sk.addPlayer("g" + std::to_string(k), 1, true, false, proto); }
    CHECK(sk.players.size() == 6 && sk.players[0].role == SkPlayer::Attack && sk.players[2].role == SkPlayer::Defend);
    CHECK(length(sk.players[0].body.position - Vec3{-100, 0, 0}) < 30.0f);
    // A capture by hand: a Blue player carrying Green's flag home.
    SkPlayer& b0 = sk.players[0];
    sk.flags[1].carrier = b0.id;
    sk.flags[1].atHome = false;
    b0.carrying = 1;
    b0.body.position = sk.teams[0].base;
    sk.events.clear();
    sk.update(0.016f);
    bool captured = false;
    for (const auto& e : sk.events) captured = captured || (e.kind == SkEvent::FlagCaptured && e.a == b0.id && e.team == 1);
    CHECK(captured && sk.teams[0].captures == 1 && sk.flags[1].atHome && b0.carrying < 0);
    // Killing a carrier drops the flag where he fell; his own team's touch returns it.
    SkPlayer& g0 = sk.players[1];
    g0.body.position = Vec3{0, 0, 0};
    sk.flags[0].carrier = g0.id;
    sk.flags[0].atHome = false;
    g0.carrying = 0;
    sk.damage(g0.id, 500.0f, b0.id, Vec3{});
    CHECK(!g0.alive && sk.flags[0].carrier < 0 && !sk.flags[0].atHome && b0.kills == 1 && sk.teams[0].kills == 1);
    // Friendly fire is off: a mate's shot does nothing.
    const float hp = sk.players[2].hp;
    sk.damage(sk.players[2].id, 50.0f, b0.id, Vec3{});
    CHECK(sk.players[2].hp == hp);
    // Played out: the bots fight and run for flags; someone wins within 20 minutes of game time.
    sk.rules.captureLimit = 3;
    sk.rules.killLimit = 20;
    for (int i = 0; i < 72 * 1200 && sk.winner < 0; ++i) { sk.update(1.0f / 72.0f); sk.events.clear(); }
    int kills = 0;
    for (const auto& p : sk.players) kills += p.kills;
    CHECK(sk.winner >= 0 && kills > 0);
    std::printf("skirmish: winner %s after %.0f s, captures %d-%d, kills %d-%d\n", sk.teams[size_t(std::max(0, sk.winner))].name.c_str(), sk.time,
                sk.teams[0].captures, sk.teams[1].captures, sk.teams[0].kills, sk.teams[1].kills);
}

static Vec3 flat3(const Vec3& v) { return Vec3{v.x, 0, v.z}; }

static void testSkNav() {
    // A sea (surface 0) with a round island 0.5 m high, its beach shelving into the sea down to a
    // floor 20 m down.
    auto ground = [](float x, float z) {
        const float r = std::sqrt(x * x + z * z);
        return std::max(-20.0f, r < 40.0f ? 0.5f : 0.5f - 0.3f * (r - 40.0f));
    };
    SkNav nav;
    nav.build(-100, -100, 100, 100, 4.0f, ground, 0.0f);
    std::vector<Vec3> way;
    // A swimmer climbs out of the sea onto the beach (the step is surface to surface, not from the floor).
    CHECK(nav.path({-80, 0, 0}, {0, 0.5f, 0}, SkNav::Swimmer, way, 1 << 30) && length(flat3(way.back() - Vec3{0, 0.5f, 0})) < 0.5f);
    // A turned building: its footprint is blocked, not its axis-aligned bounds; a goal inside it is
    // still reached (a flag among buildings); the cells beside it stay open.
    const Mat4 xf = Mat4::translation({0, 0.5f, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, 0.7854f));
    nav.blockBox(xf, {-12, 0, -4}, {12, 5, 4}, 0.0f);
    CHECK(nav.blocked({0, 0, 0}) && nav.blocked({6, 0, -6}) && !nav.blocked({8, 0, 8}) && !nav.blocked({-8, 0, -8}));
    CHECK(nav.blocked({8, 0, -4}) && !nav.blocked({8, 0, 0}));
    CHECK(nav.path({-30, 0.5f, 30}, {8, 0.5f, -4}, SkNav::Walker, way, 1 << 30) && length(flat3(way.back() - Vec3{8, 0, -4})) < 0.5f);
    for (const Vec3& p : way) CHECK(!nav.blocked(p) || length(flat3(p - Vec3{8, 0, -4})) < 0.5f);  // (never through it)
    // Learnt: after bots stuck three times going into a cell, routes go round it.
    for (int k = 0; k < 3; ++k) nav.penalize({20, 0.5f, -20}, SkNav::Walker);
    CHECK(nav.path({-30, 0.5f, 30}, {20, 0.5f, -20}, SkNav::Walker, way, 1 << 30));
    CHECK(length(flat3(way.back() - Vec3{20, 0.5f, -20})) < 0.5f);  // (still the goal: it's the last cell)
    for (int k = 0; k < 3; ++k) nav.penalize({24, 0.5f, -24}, SkNav::Walker);
    CHECK(nav.path({28, 0.5f, -28}, {28, 0.5f, 28}, SkNav::Walker, way, 1 << 30));
    bool through = false;
    for (const Vec3& p : way) through = through || length(flat3(p - Vec3{24, 0.5f, -24})) < 1.0f;
    CHECK(!through);
}

static void testSkNet() {
    // A host on localhost with one bot a side, and a joiner, exchanging packets as two games would.
    Skirmish host;
    host.groundAt = [](float, float) { return 0.0f; };
    host.setup({SkHero::Mecc, SkHero::Mecc}, {{-60, 0, 0}, {60, 0, 0}}, {});
    host.rules.killLimit = 0;
    PlayerController proto;
    const int me = host.addPlayer("HOST", 0, false, true, proto);
    const int bot = host.addPlayer("GREEN 1", 1, true, false, proto);
    host.players[size_t(bot)].body.position = Vec3{55, 0, 30};  // out of the way
    SkNetHost net;
    CHECK(net.start("M_TEST", &host, 0));
    int remoteId = -1;
    net.addRemote = [&](const std::string& name, int team) {
        remoteId = host.addPlayer(name, team < 0 ? 1 : team, false, false, proto);
        host.players[size_t(remoteId)].remote = true;
        return remoteId;
    };
    const NetAddr where{0x7F000001u, net.port()};
    Skirmish mirror;
    mirror.groundAt = host.groundAt;
    mirror.setup({SkHero::Mecc, SkHero::Mecc}, {{-60, 0, 0}, {60, 0, 0}}, {});
    mirror.mirror = true;
    SkNetClient cl;
    CHECK(cl.join(where, "JOINER", 1));
    SkNetSelf self;
    self.pos = Vec3{40, 0, 0};
    float t = 0;
    auto step = [&](int frames) {
        for (int i = 0; i < frames; ++i, t += 1.0f / 60.0f) {
            host.update(1.0f / 60.0f);
            net.update(1.0f / 60.0f);
            host.events.clear();
            cl.update(1.0f / 60.0f, self, mirror);
            mirror.update(1.0f / 60.0f);
        }
    };
    step(60);
    CHECK(cl.state == SkNetClient::State::Joined && cl.myId == remoteId && cl.myTeam == 1 && net.clients() == 1);
    // The joiner sees the host's players where the host has them.
    step(30);
    CHECK(mirror.players.size() == host.players.size());
    CHECK(length(mirror.players[size_t(me)].body.position - host.players[size_t(me)].body.position) < 1.0f);
    // Its state moves its player on the host.
    CHECK(length(host.players[size_t(remoteId)].body.position - Vec3{40, 0, 0}) < 0.01f);
    // A hit it lands counts on the host (once, however often it's resent).
    const float hp0 = host.players[size_t(me)].hp;
    cl.hit(me, 30.0f, Vec3{});
    step(30);
    bool hurtHost = false;  // the host's own player: a HurtLocal for the host's game
    (void)hp0;
    host.damage(me, 0.0f, -1, Vec3{});
    // Damage to the joiner's player reaches the joiner as an event.
    host.damage(remoteId, 12.0f, me, Vec3{1, 2, 3});
    net.update(0.0f);
    host.events.clear();
    step(30);
    bool hurt = false;
    for (const auto& e : cl.events) hurt = hurt || (e.kind == SkEvent::HurtLocal && e.b == remoteId && e.amount == 12.0f);
    CHECK(hurt);
    (void)hurtHost;
    // Its death, reported, is a kill for whoever hit it last.
    self.alive = false;
    self.lastAttacker = me;
    step(30);
    CHECK(!host.players[size_t(remoteId)].alive && host.players[size_t(me)].kills == 1 && host.teams[0].kills == 1);
    // ...and the joiner hears about it.
    bool killMsg = false;
    for (const auto& e : cl.events) killMsg = killMsg || (e.kind == SkEvent::Kill && e.b == remoteId && e.a == me);
    CHECK(killMsg);
    // A backlog of more news than one snapshot carries (12) all arrives, in order.
    cl.events.clear();
    for (int k = 0; k < 30; ++k) {
        SkEvent e;
        e.kind = SkEvent::FlagReturned;
        e.team = 0;
        e.amount = float(k);
        host.events.push_back(e);
    }
    net.update(0.0f);
    host.events.clear();
    step(60);
    std::vector<float> got;
    for (const auto& e : cl.events)
        if (e.kind == SkEvent::FlagReturned) got.push_back(e.amount);
    CHECK(got.size() == 30);
    for (size_t k = 0; k < got.size(); ++k) CHECK(got[k] == float(k));
    // It leaves: its player is gone (not respawned by a new round, not drawn), and the next joiner
    // takes its place.
    net.removeRemote = [&](int id) { host.removePlayer(id); };
    const size_t before = host.players.size();
    cl.leave();
    step(30);
    CHECK(net.clients() == 0 && host.players[size_t(remoteId)].gone && !host.players[size_t(remoteId)].alive);
    host.spawn(remoteId);
    CHECK(!host.players[size_t(remoteId)].alive);
    const int left = remoteId;
    SkNetClient cl2;
    Skirmish mirror2;
    mirror2.groundAt = host.groundAt;
    mirror2.setup({SkHero::Mecc, SkHero::Mecc}, {{-60, 0, 0}, {60, 0, 0}}, {});
    mirror2.mirror = true;
    CHECK(cl2.join(where, "AGAIN", 0));
    for (int i = 0; i < 60; ++i) {
        host.update(1.0f / 60.0f);
        net.update(1.0f / 60.0f);
        host.events.clear();
        cl2.update(1.0f / 60.0f, self, mirror2);
    }
    CHECK(cl2.state == SkNetClient::State::Joined && cl2.myId == left && host.players.size() == before && !host.players[size_t(left)].gone);
    cl2.leave();
    net.stop();
}

// Texture upscaling (rework): a smooth ramp stays a smooth, monotonic ramp at twice the size (a tap
// offset bug once made every other column come from the wrong place - blocky ground).
static void testUpscale() {
    Image img;
    img.width = img.height = 16;
    img.rgba.resize(16 * 16 * 4);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            uint8_t* p = &img.rgba[size_t(y * 16 + x) * 4];
            const float t = std::sin(float(x) / 16.0f * 6.2831853f);  // (periodic: the upscale wraps)
            p[0] = p[1] = p[2] = uint8_t(128 + 100 * t);
            p[3] = 255;
        }
    upscaleSharpen(img);
    CHECK(img.width == 32 && img.height == 32);
    // Between neighbouring outputs the change follows the sine's slope: no zigzag.
    int reversals = 0;
    for (int x = 1; x + 1 < 32; ++x) {
        const int a = img.rgba[size_t(5 * 32 + x - 1) * 4], b = img.rgba[size_t(5 * 32 + x) * 4], c = img.rgba[size_t(5 * 32 + x + 1) * 4];
        if ((b - a) * (c - b) < -4) ++reversals;  // a turn that isn't the sine's own peak or trough
    }
    CHECK(reversals <= 2);
    // Output 2x+1 sits at source x + 0.25: close to source x for a smooth ramp.
    const int src = 128 + int(100 * std::sin(4.25f / 16.0f * 6.2831853f));
    CHECK(std::abs(int(img.rgba[size_t(5 * 32 + 9) * 4]) - src) < 12);
}

int main() {
    testUpscale();
    testSkirmish();
    testSkNav();
    testSkNet();
    testSoundTable();
    testFxLibrary();
    testFxAttach();
    testWeaponDefs();
    testTestScript();
    testFlickVisibility();
    testFlickEffect();
    testGameText();
    testRescue();
    testGore();
    testDestruction();
    testHudGauge();
    testVillagers();
    testVimps();
    testRace();
    testSaveGame();
    testAffineInverse();
    testGti();
    testWorldBin();
    testMission();
    testObjectDefs();
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
    testCreatures();
    testBurrows();
    testAudio();
    testMusic();
    if (g_failures) {
        std::printf("%d test failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
