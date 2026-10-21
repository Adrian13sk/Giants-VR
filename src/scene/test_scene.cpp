#include "scene/test_scene.h"

#include <vector>

namespace gvr {

bool TestScene::init(Renderer& r) {
    const uint8_t white[4] = {255, 255, 255, 255};
    floor_ = r.uploadMesh(prim::plane(100.0f, 20, 100.0f, white));  // 1 m checker squares
    marker_ = r.uploadMesh(prim::box({0.15f, 0.5f, 0.15f}, white));
    pillar_ = r.uploadMesh(prim::cylinder(0.3f, 3.0f, 24, white));
    sphere_ = r.uploadMesh(prim::sphere(1.0f, 24, 12, white));
    box_ = r.uploadMesh(prim::box({0.5f, 0.5f, 0.5f}, white));
    ray_ = r.uploadMesh(prim::box({0.004f, 0.004f, 0.5f}, white));
    giant_ = r.uploadMesh(prim::cylinder(2.0f, 30.0f, 32, white));

    // Procedural checker texture: 64x64 with two 32px cells, so one texture repeat = 2 cells.
    std::vector<uint8_t> px(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            bool c = ((x / 32) ^ (y / 32)) & 1;
            uint8_t v = c ? 200 : 120;
            bool line = (x % 32 == 0) || (y % 32 == 0);
            uint8_t* p = &px[(y * 64 + x) * 4];
            p[0] = line ? 60 : v;
            p[1] = line ? 60 : static_cast<uint8_t>(v + 10);
            p[2] = line ? 60 : v;
            p[3] = 255;
        }
    checker_ = r.uploadTexture(64, 64, px.data(), true, true);
    return floor_ && marker_ && pillar_ && sphere_ && box_ && ray_ && giant_ && checker_;
}

void TestScene::render(Renderer& r, const VrRig& rig, const InputFrame& in) {
    // Floor: 200 m x 200 m, texture repeat 100 -> 2 m per texture -> 1 m checker cells.
    DrawParams fp;
    fp.texture = checker_;
    r.draw(floor_, Mat4::identity(), fp);

    // Distance markers along -Z (red), +X (green), -X (blue) at known distances.
    const float dists[] = {1, 2, 3, 5, 10, 20, 50};
    for (float d : dists) {
        float h = 0.5f;
        DrawParams red; red.tint = {0.9f, 0.2f, 0.2f, 1};
        DrawParams grn; grn.tint = {0.2f, 0.8f, 0.3f, 1};
        DrawParams blu; blu.tint = {0.25f, 0.35f, 0.95f, 1};
        r.draw(marker_, Mat4::translation({0, h, -d}), red);
        r.draw(marker_, Mat4::translation({d, h, 0}), grn);
        r.draw(marker_, Mat4::translation({-d, h, 0}), blu);
    }
    // Ring of pillars at 8 m radius.
    for (int i = 0; i < 12; ++i) {
        float a = 2 * kPi * i / 12;
        DrawParams pp; pp.tint = {0.8f, 0.75f, 0.6f, 1};
        r.draw(pillar_, Mat4::translation({8 * std::cos(a), 0, 8 * std::sin(a)}), pp);
    }
    // A 30 m "giant-scale" column 40 m away, for judging large-character scale.
    DrawParams gp; gp.tint = {0.6f, 0.5f, 0.75f, 1};
    r.draw(giant_, Mat4::translation({25, 0, -32}), gp);
    // A 1 m reference cube at eye height 2 m in front.
    DrawParams cp; cp.tint = {0.95f, 0.8f, 0.2f, 1};
    r.draw(box_, Mat4::trs({0, 1.5f, -2.5f}, Quat::fromAxisAngle({0, 1, 0}, 0.6f), {0.4f, 0.4f, 0.4f}), cp);

    // Controllers: grip box (colour = trigger/squeeze), aim ray, button indicator spheres.
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        if (c.gripPoseValid) {
            Pose g = rig.toWorld(c.gripPose);
            DrawParams dp;
            dp.tint = {0.2f + 0.8f * c.trigger, 0.2f + 0.8f * c.squeeze, h == kLeft ? 0.9f : 0.3f, 1};
            r.draw(box_, Mat4::trs(g.position, g.orientation, {0.05f, 0.05f, 0.12f}), dp);
            // Button indicators above the controller.
            const bool buttons[4] = {c.primary, c.secondary, c.thumbstickClick, c.menu};
            for (int b = 0; b < 4; ++b) {
                Vec3 off{-0.03f + 0.02f * b, 0.05f, 0.0f};
                DrawParams bp;
                bp.unlit = true;
                bp.tint = buttons[b] ? Vec4{1, 1, 0.2f, 1} : Vec4{0.15f, 0.15f, 0.15f, 1};
                r.draw(sphere_, Mat4::trs(g.transformPoint(off), g.orientation, {0.007f, 0.007f, 0.007f}), bp);
            }
            // Thumbstick deflection indicator.
            Vec3 so{c.thumbstick.x * 0.03f, 0.075f, -c.thumbstick.y * 0.03f};
            DrawParams sp; sp.unlit = true; sp.tint = {0.2f, 0.9f, 1.0f, 1};
            r.draw(sphere_, Mat4::trs(g.transformPoint(so), g.orientation, {0.006f, 0.006f, 0.006f}), sp);
        }
        if (c.aimPoseValid) {
            Pose a = rig.toWorld(c.aimPose);
            DrawParams rp;
            rp.unlit = true;
            rp.tint = c.trigger > 0.5f ? Vec4{1, 0.3f, 0.2f, 1} : Vec4{0.9f, 0.9f, 0.9f, 1};
            // Ray box is 1 m long centred on origin; shift it 0.5 m forward along -Z.
            Mat4 m = Mat4::fromPose(a) * Mat4::translation({0, 0, -0.5f}) * Mat4::scale({1, 1, 1});
            r.draw(ray_, m, rp);
        }
    }
}

}  // namespace gvr
