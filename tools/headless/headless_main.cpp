// Headless desktop harness: runs the same GameApp/renderer code as the Quest build on Mesa
// (EGL surfaceless + GLES 3), with a scripted head/controller pose, and writes a side-by-side
// stereo PNG. Used to verify rendering and original-asset loading without a headset.
//
// usage: headless_render [--data DIR] [--world Story1] [--out file.png] [--size W H] [--frames N]
//                        [--move X Y] [--turn X] [--head-yaw DEG] [--no-test-scene]
//                        [--at GAME_X GAME_Y]  (teleport to game coordinates)
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "assets/image.h"
#include "core/log.h"
#include "game/game_app.h"
#include "scene/model_cache.h"
#include "render/gl.h"
#include "egl_util.h"

using namespace gvr;

int main(int argc, char** argv) {
    std::string dataDir = "", out = "stereo.png", world = "Story1";
    float atX = 0, atY = 0, pitchDeg = -8.6f, headHeight = 1.65f;
    bool useAt = false;
    int w = 720, h = 792, frames = 1;
    float moveX = 0, moveY = 0, turnX = 0, headYawDeg = 0;
    bool testScene = true, freeCam = false, firstPerson = false, jet = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--data") dataDir = next();
        else if (a == "--out") out = next();
        else if (a == "--size") { w = std::atoi(next()); h = std::atoi(next()); }
        else if (a == "--frames") frames = std::atoi(next());
        else if (a == "--move") { moveX = std::atof(next()); moveY = std::atof(next()); }
        else if (a == "--turn") turnX = std::atof(next());
        else if (a == "--head-yaw") headYawDeg = std::atof(next());
        else if (a == "--no-test-scene") testScene = false;
        else if (a == "--world") world = next();
        else if (a == "--pitch") pitchDeg = std::atof(next());
        else if (a == "--height") headHeight = std::atof(next());
        else if (a == "--at") { atX = std::atof(next()); atY = std::atof(next()); useAt = true; }
        else if (a == "--free") freeCam = true;            // free camera instead of the Meccaryn
        else if (a == "--first-person") firstPerson = true;
        else if (a == "--jet") jet = true;                 // hold A (jetpack)
    }
    if (!initEgl()) return 2;

    GameApp app;
    if (!app.init(dataDir, world)) return 3;
    LOGI("original assets loaded: %s", app.hasOriginalAssets() ? "yes" : "no");

    // Scripted tracking-space poses, approximating a standing Quest 2 user.
    InputFrame in;
    in.headValid = true;
    in.head.position = {0, headHeight, 0};
    in.head.orientation = Quat::fromAxisAngle({0, 1, 0}, headYawDeg * kPi / 180.0f) *
                          Quat::fromAxisAngle({1, 0, 0}, pitchDeg * kPi / 180.0f);
    for (int hnd = 0; hnd < 2; ++hnd) {
        ControllerState& c = in.hand[hnd];
        c.active = c.gripPoseValid = c.aimPoseValid = true;
        float side = hnd == kLeft ? -1.0f : 1.0f;
        c.gripPose.position = {0.2f * side, 1.2f, -0.35f};
        c.gripPose.orientation = Quat::fromAxisAngle({1, 0, 0}, -0.3f);
        c.aimPose = c.gripPose;
    }
    in.hand[kRight].trigger = 1.0f;
    in.hand[kLeft].primary = true;
    in.hand[kRight].primary = jet;
    in.hand[kLeft].thumbstick = {moveX, moveY};
    in.hand[kRight].thumbstick = {turnX, 0};
    app.rig().comfort.snapTurn = false;

    // Disable the engineering scene if requested (left stick click toggles it).
    if (!testScene) {
        InputFrame t = in;
        t.hand[kLeft].thumbstickClick = true;
        app.update(0.0f, t);
    }
    app.update(0.0f, in);  // consume the automatic spawn
    if (freeCam) app.setPlayMode(false, in);
    app.setFirstPerson(firstPerson);
    if (useAt) {
        // Teleport to game coordinates (units, Z-up) facing the scripted head yaw.
        const float yaw = headYawDeg * kPi / 180.0f;
        if (app.playMode()) app.teleportPlayer(giantsToRuntime(atX, atY, 0.0f), yaw);
        else app.rig().teleportHead(in, giantsToRuntime(atX, atY, 0.0f), yaw);
        app.update(0.0f, in);  // ground following sets the height
    }
    for (int f = 0; f < frames; ++f) app.update(1.0f / 72.0f, in);
    if (app.playMode()) {
        const auto& p = app.player();
        LOGI("player at (%.2f %.2f %.2f) yaw %.1f deg, %s, anim %d t=%.2f, speed %.2f m/s", p.position.x, p.position.y,
             p.position.z, p.yaw * 180.0f / kPi, p.onGround ? "on ground" : "airborne", int(p.anim()), p.animTime(),
             p.horizontalSpeed());
    }

    // Offscreen sRGB targets, like the OpenXR swapchain.
    GLuint tex = 0, depth = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, w, h);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("FBO incomplete");
        return 4;
    }

    // Quest 2-like asymmetric FOVs (radians), mirrored between eyes; IPD 64 mm.
    const float ipd = 0.064f;
    std::vector<uint8_t> sbs(size_t(w) * 2 * h * 4);
    std::vector<uint8_t> eyePx(size_t(w) * h * 4);
    for (int eye = 0; eye < 2; ++eye) {
        EyeRenderInfo ei{};
        ei.trackingPose.orientation = in.head.orientation;
        ei.trackingPose.position = in.head.transformPoint({(eye == 0 ? -0.5f : 0.5f) * ipd, 0, 0});
        if (eye == 0) { ei.fovLeft = -0.942f; ei.fovRight = 0.698f; }
        else { ei.fovLeft = -0.698f; ei.fovRight = 0.942f; }
        ei.fovUp = 0.733f;
        ei.fovDown = -0.837f;
        ei.width = w;
        ei.height = h;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        app.renderer().resetStats();
        app.renderEye(ei);
        LOGI("eye %d: %u draw calls", eye, app.renderer().drawCallsThisFrame());
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, eyePx.data());
        if (!checkGl("readback")) return 5;
        for (int y = 0; y < h; ++y)
            std::memcpy(&sbs[(size_t(y) * w * 2 + size_t(eye) * w) * 4], &eyePx[size_t(y) * w * 4], size_t(w) * 4);
    }
    if (!writePng(out, w * 2, h, sbs.data(), true)) {
        LOGE("PNG write failed");
        return 6;
    }
    Pose hw = app.rig().toWorld(in.head);
    LOGI("head world position after %d frames: %.3f %.3f %.3f yaw %.1f deg", frames, hw.position.x,
         hw.position.y, hw.position.z, yawOf(hw.orientation) * 180.0f / kPi);
    LOGI("wrote %s (%dx%d)", out.c_str(), w * 2, h);
    app.shutdown();
    return 0;
}
