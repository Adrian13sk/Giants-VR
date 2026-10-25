// Headless desktop harness: runs the same GameApp/renderer code as the Quest build on Mesa
// (EGL surfaceless + GLES 3), with a scripted head/controller pose, and writes a side-by-side
// stereo PNG. Used to verify rendering and original-asset loading without a headset.
//
// usage: headless_render [--data DIR] [--world Story1] [--out file.png] [--size W H] [--frames N]
//                        [--move X Y] [--turn X] [--head-yaw DEG] [--no-test-scene]
//                        [--at GAME_X GAME_Y]  (teleport to game coordinates)
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>

#include "audio/mixer.h"
#include <cstring>
#include <string>
#include <vector>

#include "assets/image.h"
#include "core/log.h"
#include "core/prof.h"
#include "scene/asset_world.h"
#include "game/game_app.h"
#include "game/test_script.h"
#include <fstream>
#include <sstream>
#include "scene/model_cache.h"
#include "render/gl.h"
#include "egl_util.h"

using namespace gvr;

int main(int argc, char** argv) {
    std::string dataDir = "", out = "stereo.png", world = "Story1";
    float atX = 0, atY = 0, pitchDeg = -8.6f, headHeight = 1.65f;
    bool useAt = false;
    bool holdBrief = false;
    int w = 720, h = 792, frames = 1;
    float moveX = 0, moveY = 0, turnX = 0, headYawDeg = 0, lookX = 0, lookY = 0;
    bool useLook = false, wantBrief = false;
    bool testScene = true, freeCam = false, firstPerson = true, jet = false;
    float gripAmount = 0.0f, handsFwd = 0.35f, handsUp = 1.2f;
    bool mainMenu = false;
    int completeAt = -1, vimpMeat = -1, completeEvery = -1;
    int syFrom = -1, syTo = -1;
    float syVal = 0;
    int ltrigFrom = -1, ltrigTo = -1, yFrom = -1, yTo = -1;
    int gripFrom = -1, gripTo = -1, handPathFrom = -1, handPathTo = -1, stickFrom = -1, stickTo = -1, trigFrom = -1, trigTo = -1;
    float handDx = 0, handDy = 0, handDz = 0;
    std::string savePath;
    int menuAt = -1, sxFrom = -1, sxTo = -1;
    float sxVal = 0;
    int xFrom = -1, xTo = -1, lPathFrom = -1, lPathTo = -1, hurtAt = -1;
    float lDx = 0, lDy = 0, lDz = 0, hurtAmount = 0, hurtYaw = 0;  // right hand moves by this (tracking metres) over the path frames
    int jetFrom = -1, jetTo = -1;
    bool autoAim = false, viewBody = false, viewCorpse = false;
    int viewFrames = 0, killShots = 0, killPlayerAt = -1, killCause = 0;
    bool killDemo = false;
    int trigPulse = 0;
    int lgripFrom = -1, lgripTo = -1;
    float spawnReaper = 0;
    float blastX = 0, blastY = 0, blastH = 1;
    int blastN = 0, blastFrame = -1;
    std::string audioOut, scriptPath, videoDir;
    int videoEvery = 3, videoFrom = 0, vw = 1280, vh = 720, videoN = 0;
    GLuint vTex = 0, vDepth = 0, vFbo = 0;
    float triggerAmount = 0.0f, aimPitchDeg = -17.2f, aimYawDeg = 0.0f;
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
        else if (a == "--brief") wantBrief = true;  // show the first mission's briefing (skipped by default)
        else if (a == "--look-at") { lookX = std::atof(next()); lookY = std::atof(next()); useLook = true; }  // game x y (with --at)
        else if (a == "--no-test-scene") testScene = false;
        else if (a == "--hold-brief") holdBrief = true;  // hold the left stick down (mission briefing overlay)
        else if (a == "--world") world = next();
        else if (a == "--script") scriptPath = next();  // a device test script (devtest/tests/*.txt), run at 72 fps
        else if (a == "--pitch") pitchDeg = std::atof(next());
        else if (a == "--height") headHeight = std::atof(next());
        else if (a == "--at") { atX = std::atof(next()); atY = std::atof(next()); useAt = true; }
        else if (a == "--free") freeCam = true;
        else if (a == "--main-menu") mainMenu = true;     // start at the main menu
        else if (a == "--complete-at") completeAt = std::atoi(next());  // mark the mission complete at this frame
        else if (a == "--complete-every") completeEvery = std::atoi(next());  // ...and every N frames (campaign soak test)
        else if (a == "--vimp-meat") vimpMeat = std::atoi(next());  // carrying this much Vimp meat from the start
        else if (a == "--stick-y-window") { syFrom = std::atoi(next()); syTo = std::atoi(next()); syVal = std::atof(next()); }  // right stick y            // free camera instead of the Meccaryn
        else if (a == "--first-person") firstPerson = true;
        else if (a == "--third-person") firstPerson = false;
        else if (a == "--jet") jet = true;                 // hold A (jetpack)
        else if (a == "--jet-window") { jetFrom = std::atoi(next()); jetTo = std::atoi(next()); }  // hold A for frames [from, to)
        else if (a == "--grip") gripAmount = std::atof(next());  // both grips 0..1
        else if (a == "--stick-up-window") { stickFrom = std::atoi(next()); stickTo = std::atoi(next()); }  // right stick forward
        else if (a == "--trigger-window") { trigFrom = std::atoi(next()); trigTo = std::atoi(next()); }
        else if (a == "--lgrip-window") { lgripFrom = std::atoi(next()); lgripTo = std::atoi(next()); }  // left grip held
        else if (a == "--trigger-pulse") trigPulse = std::atoi(next());  // inside the trigger window: pulled for N frames, released for N
        else if (a == "--ltrigger-window") { ltrigFrom = std::atoi(next()); ltrigTo = std::atoi(next()); }  // left trigger
        else if (a == "--y-window") { yFrom = std::atoi(next()); yTo = std::atoi(next()); }  // Y (left secondary)
        else if (a == "--grip-window") { gripFrom = std::atoi(next()); gripTo = std::atoi(next()); }  // right grip for frames [from, to)
        else if (a == "--hand-path") { handPathFrom = std::atoi(next()); handPathTo = std::atoi(next());
                                       handDx = std::atof(next()); handDy = std::atof(next()); handDz = std::atof(next()); }
        else if (a == "--save") savePath = next();
        else if (a == "--menu-at") menuAt = std::atoi(next());
        else if (a == "--stick-x-window") { sxFrom = std::atoi(next()); sxTo = std::atoi(next()); sxVal = std::atof(next()); }  // right stick x  // press the left menu button at this frame  // checkpoint file (continue from it / write it)
        else if (a == "--x-window") { xFrom = std::atoi(next()); xTo = std::atoi(next()); }  // X (left primary) held for frames [from, to)
        else if (a == "--lhand-path") { lPathFrom = std::atoi(next()); lPathTo = std::atoi(next());
                                        lDx = std::atof(next()); lDy = std::atof(next()); lDz = std::atof(next()); }
        else if (a == "--hurt") { hurtAt = std::atoi(next()); hurtAmount = std::atof(next()); hurtYaw = std::atof(next()); }  // frame, damage, from (deg, + right)
        else if (a == "--trigger") triggerAmount = std::atof(next());  // right trigger (fires in play mode)
        else if (a == "--aim-pitch") aimPitchDeg = std::atof(next());  // controller pitch (deg)
        else if (a == "--aim-yaw") aimYawDeg = std::atof(next());      // controller yaw (deg, + = left)
        else if (a == "--auto-aim") autoAim = true;  // test only: point the right controller at the nearest enemy
        else if (a == "--view-body") viewBody = true; // test only: after the frames, stand 4 m from an enemy body
        else if (a == "--spawn-reaper") spawnReaper = float(std::atof(next()));  // a live Reaper this many metres ahead at the start
        else if (a == "--kill-demo") { killDemo = true; killShots = std::atoi(next()); }  // a Reaper killed 5 m ahead at the start (then N shots at the body)
        else if (a == "--kill-player") { killPlayerAt = std::atoi(next()); killCause = std::atoi(next()); }  // frame, cause (0 shot 1 bitten 2 drowned 3 blast)
        else if (a == "--blast-at") { blastX = std::atof(next()); blastY = std::atof(next()); blastH = std::atof(next()); blastN = std::atoi(next()); blastFrame = std::atoi(next()); }  // N rocket blasts at game x y, h m up, at frame F
        else if (a == "--view-corpse") { viewCorpse = true; viewFrames = std::atoi(next()); }  // then N more frames 3 m from the nearest body
        else if (a == "--audio-out") audioOut = next();  // write the mixed game audio (48 kHz stereo WAV)
        else if (a == "--video") { videoDir = next(); videoEvery = std::max(1, std::atoi(next())); }  // spectator frames (PNG) every N frames
        else if (a == "--video-from") videoFrom = std::atoi(next());
        else if (a == "--video-size") { vw = std::atoi(next()); vh = std::atoi(next()); }
        else if (a == "--hands") { handsFwd = std::atof(next()); handsUp = std::atof(next()); }  // controller offset (m)
    }
    TestScript script;
    const bool useScript = !scriptPath.empty();
    if (useScript) {
        std::ifstream sf(scriptPath);
        std::stringstream ss;
        ss << sf.rdbuf();
        std::string err;
        if (!sf || !script.parse(ss.str(), err)) {
            LOGE("script %s: %s", scriptPath.c_str(), sf ? err.c_str() : "can't read");
            return 4;
        }
        world = script.world;
        if (!script.mission.empty()) setenv("GVR_MISSION", script.mission.c_str(), 1);
        for (const auto& e : script.env) setenv(e.first.c_str(), e.second.c_str(), 1);
        frames = int(script.duration * 72.0f);
        firstPerson = script.firstPerson;
        pitchDeg = script.pitchDeg;
        aimYawDeg = script.aimYawDeg;
        aimPitchDeg = script.aimPitchDeg;
        if (script.hasAt) { useAt = true; atX = script.atX; atY = script.atY; }
        if (script.hasLook) { useLook = true; lookX = script.lookX; lookY = script.lookY; }
        LOGI("script %s: %s / %s, %.0f s", script.name.c_str(), world.c_str(), script.mission.c_str(), script.duration);
    }
    if (!initEgl()) return 2;

    GameApp app;
    if (!savePath.empty()) app.setSavePath(savePath);
    if (mainMenu) app.setMainMenu(true);
    if (!app.init(dataDir, world)) return 3;
    // Test only: force the Ripper death type (GVR_BURST_CHANCE=1 -> always burst into gore).
    if (const char* bc = std::getenv("GVR_BURST_CHANCE"))
        const_cast<CreatureDirector&>(app.creatureDirector()).params.burstChance = float(std::atof(bc));
    AudioMixer mixer(48000);
    std::vector<float> audio;
    if (!audioOut.empty()) app.setAudio(&mixer);
    LOGI("original assets loaded: %s", app.hasOriginalAssets() ? "yes" : "no");

    // Scripted tracking-space poses, approximating a standing Quest 2 user.
    InputFrame in;
    in.headValid = true;
    // --look-at: face a game-coordinate point from the --at position (runtime x = -game x).
    if (useLook) headYawDeg = std::atan2(lookX - atX, -(lookY - atY)) * 180.0f / kPi;
    if (!wantBrief) setenv("GVR_NO_BRIEF", "1", 1);
    in.head.position = {0, headHeight, 0};
    in.head.orientation = Quat::fromAxisAngle({0, 1, 0}, (useLook ? 0.0f : headYawDeg) * kPi / 180.0f) *
                          Quat::fromAxisAngle({1, 0, 0}, pitchDeg * kPi / 180.0f);
    for (int hnd = 0; hnd < 2; ++hnd) {
        ControllerState& c = in.hand[hnd];
        c.active = c.gripPoseValid = c.aimPoseValid = true;
        float side = hnd == kLeft ? -1.0f : 1.0f;
        c.gripPose.position = {0.2f * side, handsUp, -handsFwd};
        c.squeeze = gripAmount;
        c.gripPose.orientation = Quat::fromAxisAngle({0, 1, 0}, aimYawDeg * kPi / 180.0f) *
                                 Quat::fromAxisAngle({1, 0, 0}, aimPitchDeg * kPi / 180.0f);
        c.aimPose = c.gripPose;
    }
    in.hand[kRight].trigger = 0.0f;  // applied only during the simulated frames (below)
    in.hand[kLeft].primary = false;  // X in the free camera skips to the next mission
    in.hand[kRight].primary = jet;
    in.hand[kLeft].thumbstick = {moveX, moveY};
    in.hand[kRight].thumbstick = {turnX, 0};
    app.rig().comfort.snapTurn = false;

    // Disable the engineering scene if requested (left stick click toggles it).
    if (!testScene) app.setShowTestScene(false);
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
    if (useScript && script.briefing) app.debugReloadWithBriefing(in);
    in.hand[kRight].trigger = triggerAmount;
    in.hand[kLeft].thumbstickClick = holdBrief;
    if (spawnReaper > 0) {
        const Pose hw = app.rig().toWorld(in.head);
        Vec3 f = hw.orientation.rotate(Vec3{0, 0, -1});
        f.y = 0;
        f = normalize(f);
        const int i = app.debugSpawnReaperAt(hw.position + f * spawnReaper);
        std::printf("spawned Reaper %d %.0f m ahead\n", i, double(spawnReaper));
    }
    if (killDemo) {
        const Pose hw = app.rig().toWorld(in.head);
        Vec3 f = hw.orientation.rotate(Vec3{0, 0, -1});
        f.y = 0;
        f = normalize(f);
        app.debugKillReaperAt(hw.position + f * 5.0f, f);
        for (int k = 0; k < 90; ++k) app.update(1.0f / 72.0f, in);  // falls
        app.debugShootCorpses(killShots);
        app.debugDumpBlood();
    }
    double updSum = 0, updMax = 0;  // CPU time of GameApp::update (host; relative costs only)
    int updN = 0, updMaxF = -1;
    // GVR_REALTIME=1: frames paced at 72 Hz of wall time (two harnesses playing over the network).
    const bool realtime = std::getenv("GVR_REALTIME") != nullptr;
    const auto pace0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        if (realtime) {
            const auto due = pace0 + std::chrono::microseconds(int64_t(f) * 1000000 / 72);
            while (std::chrono::steady_clock::now() < due) {}
        }
        if (jetFrom >= 0) in.hand[kRight].primary = f >= jetFrom && f < jetTo;
        if (gripFrom >= 0) in.hand[kRight].squeeze = f >= gripFrom && f < gripTo ? 1.0f : 0.0f;
        if (lgripFrom >= 0) in.hand[kLeft].squeeze = f >= lgripFrom && f < lgripTo ? 1.0f : 0.0f;
        if (stickFrom >= 0) in.hand[kRight].thumbstick.y = f >= stickFrom && f < stickTo ? 1.0f : 0.0f;
        if (trigFrom >= 0) in.hand[kRight].trigger = f >= trigFrom && f < trigTo && (trigPulse <= 0 || ((f - trigFrom) / trigPulse) % 2 == 0) ? 1.0f : 0.0f;
        if (xFrom >= 0) in.hand[kLeft].primary = f >= xFrom && f < xTo;
        if (ltrigFrom >= 0) in.hand[kLeft].trigger = f >= ltrigFrom && f < ltrigTo ? 1.0f : 0.0f;
        if (yFrom >= 0) in.hand[kLeft].secondary = f >= yFrom && f < yTo;
        in.hand[kLeft].menu = menuAt >= 0 && f >= menuAt && f < menuAt + 2;
        if (sxFrom >= 0) in.hand[kRight].thumbstick.x = f >= sxFrom && f < sxTo ? sxVal : 0.0f;
        if (syFrom >= 0) in.hand[kRight].thumbstick.y = f >= syFrom && f < syTo ? syVal : 0.0f;
        if (lPathFrom >= 0 && f >= lPathFrom && f < lPathTo) {
            const float k = 1.0f / float(lPathTo - lPathFrom);
            in.hand[kLeft].gripPose.position = in.hand[kLeft].gripPose.position + Vec3{lDx, lDy, lDz} * k;
            in.hand[kLeft].aimPose.position = in.hand[kLeft].aimPose.position + Vec3{lDx, lDy, lDz} * k;
        }
        if (f == hurtAt) app.debugHurt(hurtAmount, hurtYaw);
        if (f == completeAt || (completeEvery > 0 && f > 0 && f % completeEvery == 0)) app.debugCompleteMission();
        if (f == 0 && vimpMeat >= 0) app.debugSetVimpMeat(vimpMeat);
        if (handPathFrom >= 0 && f >= handPathFrom && f < handPathTo) {
            const float k = 1.0f / float(handPathTo - handPathFrom);
            in.hand[kRight].gripPose.position = in.hand[kRight].gripPose.position + Vec3{handDx, handDy, handDz} * k;
            in.hand[kRight].aimPose.position = in.hand[kRight].aimPose.position + Vec3{handDx, handDy, handDz} * k;
        }
        Vec3 chest;
        if (autoAim && app.nearestEnemy(chest)) {
            // World target -> tracking-space direction from the right controller.
            const Pose& rigPose = app.rig().worldFromTracking;
            const Vec3 ctrlWorld = app.rig().toWorld(in.hand[kRight].aimPose).position;
            const Vec3 d = rigPose.orientation.conjugate().rotate(normalize(chest - ctrlWorld));
            const float yaw = std::atan2(-d.x, -d.z), pitch = std::asin(std::max(-1.0f, std::min(1.0f, d.y)));
            const Quat q = Quat::fromAxisAngle({0, 1, 0}, yaw) * Quat::fromAxisAngle({1, 0, 0}, pitch);
            in.hand[kRight].gripPose.orientation = in.hand[kRight].aimPose.orientation = q;
        }
        if (useScript) {
            const float t = float(f) / 72.0f, tp = float(f - 1) / 72.0f;
            const Pose head = in.head;
            script.inputAt(t, in);
            in.head = head;  // (the harness's own head pose: --look-at already turned the rig)
            for (const auto* b : script.blastsIn(tp, t))
                for (int k = 0; k < b->n; ++k) app.debugBlast(b->x, b->y, b->h);
            for (const auto* e : script.reapersIn(tp, t)) {
                if (e->kill) app.debugBlastReapers();
                else app.debugReapersAhead(e->n, e->dist);
            }
            for (const auto* s : script.shotsIn(tp, t)) LOGI("script: shot '%s' at %.1f s (device only)", s->label.c_str(), s->t);
        }
        if (f == killPlayerAt) app.debugKillPlayer(killCause);
        if (f == blastFrame) {
            for (int b = 0; b < blastN; ++b) app.debugBlast(blastX, blastY, blastH);
            LOGI("blast x%d: %zu objects destroyed", blastN, app.destroyedCount());
        }
        const auto u0 = std::chrono::steady_clock::now();
        app.update(1.0f / 72.0f, in);
        const double ums = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count();
        if (f >= 30) { updSum += ums; ++updN; if (ums > updMax) { updMax = ums; updMaxF = f; } }
        if (!videoDir.empty() && f >= videoFrom && (f - videoFrom) % videoEvery == 0) {
            // A spectator view from the head (16:9, ~92 x 57 degrees) for progress videos.
            if (!vFbo) {
                glGenTextures(1, &vTex);
                glBindTexture(GL_TEXTURE_2D, vTex);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, vw, vh);
                glGenRenderbuffers(1, &vDepth);
                glBindRenderbuffer(GL_RENDERBUFFER, vDepth);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, vw, vh);
                glGenFramebuffers(1, &vFbo);
                glBindFramebuffer(GL_FRAMEBUFFER, vFbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vTex, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, vDepth);
            }
            EyeRenderInfo ve{};
            ve.trackingPose = in.head;
            const float hx = 0.80f, hy = hx * float(vh) / float(vw);
            ve.fovLeft = -hx; ve.fovRight = hx; ve.fovUp = hy; ve.fovDown = -hy;
            ve.width = vw; ve.height = vh;
            if (videoN % 40 == 0) {
                const Pose hw = app.rig().toWorld(in.head);
                const Vec3 fw = hw.orientation.rotate({0, 0, -1});
                LOGI("video frame %d (sim frame %d): eye (%.1f %.1f %.1f) forward (%.2f %.2f %.2f) scale %.2f", videoN, f, hw.position.x,
                     hw.position.y, hw.position.z, fw.x, fw.y, fw.z, app.rig().comfort.worldScale);
            }
            app.renderShadows(ve, ve);
            glBindFramebuffer(GL_FRAMEBUFFER, vFbo);
            app.renderEye(ve);
            std::vector<uint8_t> px(size_t(vw) * vh * 4);
            glReadPixels(0, 0, vw, vh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            char name[64];
            std::snprintf(name, sizeof name, "/f%05d.png", videoN++);
            writePng(videoDir + name, vw, vh, px.data(), true);
        }
        if (std::getenv("GVR_PLAYER_TRACE") && f % 12 == 0) {
            const auto& p = app.player();
            LOGI("trace f%d player (%.2f %.2f %.2f) game (%.0f %.0f) ground %d swim %d anim %d", f, p.position.x, p.position.y, p.position.z,
                 -p.position.x / gGameUnitsToMeters, p.position.z / gGameUnitsToMeters, int(p.onGround), int(p.swimming()), int(p.anim()));
        }
        if (!audioOut.empty()) {
            const size_t n = audio.size();
            audio.resize(n + 2 * 667);
            mixer.mix(audio.data() + n, 667);
        }
    }
    if (!audioOut.empty()) {
        FILE* f = std::fopen(audioOut.c_str(), "wb");
        if (f) {
            const uint32_t frames = uint32_t(audio.size() / 2), bytes = frames * 4;
            auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
            auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
            std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16);
            u16(1); u16(2); u32(48000); u32(48000 * 4); u16(4); u16(16); std::fwrite("data", 1, 4, f); u32(bytes);
            for (float v : audio) { const int16_t s = int16_t(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f); u16(uint16_t(s)); }
            std::fclose(f);
            LOGI("wrote %s (%.1f s of audio)", audioOut.c_str(), frames / 48000.0f);
        }
    }
    Vec3 body;
    if (viewCorpse && app.nearestCorpse(body)) {
        const Vec3 at = body + Vec3{2.2f, 0, 2.2f};
        app.teleportPlayer(at, std::atan2(-(body.x - at.x), -(body.z - at.z)));
        in.hand[kRight].trigger = 0;
        in.hand[kLeft].thumbstick = {0, 0};
        for (int f = 0; f < viewFrames; ++f) app.update(1.0f / 72.0f, in);
        LOGI("corpse view: %zu blood decals, %zu cleaners", app.bloodDecals(), app.cleanerCount());
    }
    if (viewBody && app.anyEnemy(body)) {
        const Vec3 at = body + Vec3{0, 0, 4.0f};
        app.teleportPlayer(at, 0.0f);
        in.hand[kRight].trigger = 0;
        app.update(1.0f / 72.0f, in);
    }
    {
        const Pose hw = app.rig().toWorld(in.head);
        const Vec3 f = hw.orientation.rotate(Vec3{0, 0, -1});
        LOGI("view: eye (%.1f %.1f %.1f) = game (%.0f %.0f %.0f), forward (%.2f %.2f %.2f)", hw.position.x, hw.position.y,
             hw.position.z, -hw.position.x / gGameUnitsToMeters, hw.position.z / gGameUnitsToMeters,
             hw.position.y / gGameUnitsToMeters, f.x, f.y, f.z);
    }
    if (app.playMode()) {
        const auto& p = app.player();
        LOGI("weapon: %zu shots, %zu hits, last hit at %.1f m", app.weaponShots(), app.weaponHits(), app.weaponLastHitDistance());
        LOGI("player: health %.0f, injections %d", app.playerHealth(), app.syringeCount());
        Vec3 ne;
        const float nd = app.nearestEnemy(ne) ? length(ne - p.position - Vec3{0, 1.2f, 0}) : -1.0f;
        const Vec3 rel = ne - p.position;
        LOGI("combat: %zu/%zu Reapers alive, %zu creatures alive (%zu characters), nearest %.1f m (yaw %.0f deg), "
             "player health %.0f", app.enemiesAlive(), app.enemyCount(), app.creaturesAlive(), app.characterCount(), nd,
             std::atan2(-rel.x, -rel.z) * 180.0f / kPi, app.playerHealth());
        if (std::getenv("GVR_CREATURE_DUMP"))
            for (const auto& c : app.creatureDirector().creatures)
                LOGI("  creature state %d clip %d pos (%.1f %.1f %.1f) dist %.1f speed %.2f spitWait %.1f blocked %.2f detour %.2f",
                     int(c.state), int(c.clip), c.pos.x, c.pos.y, c.pos.z, length(c.pos - p.position), c.speed, c.spitWait,
                     c.blockedTime, c.detour);
        LOGI("creatures: %zu spits launched, %zu spit hits, %zu gore pieces", app.spitsLaunched(), app.spitHits(),
             app.gibCount());
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

    gvr::profReport();
    if (updN > 0) LOGI("update: %.2f ms average, %.2f ms worst (frame %d) over %d frames", updSum / updN, updMax, updMaxF, updN);
    // Quest 2-like asymmetric FOVs (radians), mirrored between eyes; IPD 64 mm.
    const float ipd = 0.064f;
    std::vector<uint8_t> sbs(size_t(w) * 2 * h * 4);
    std::vector<uint8_t> eyePx(size_t(w) * h * 4);
    EyeRenderInfo eyes[2];
    for (int eye = 0; eye < 2; ++eye) {
        EyeRenderInfo& ei = eyes[eye];
        ei = EyeRenderInfo{};
        ei.trackingPose.orientation = in.head.orientation;
        ei.trackingPose.position = in.head.transformPoint({(eye == 0 ? -0.5f : 0.5f) * ipd, 0, 0});
        if (eye == 0) { ei.fovLeft = -0.942f; ei.fovRight = 0.698f; }
        else { ei.fovLeft = -0.698f; ei.fovRight = 0.942f; }
        ei.fovUp = 0.733f;
        ei.fovDown = -0.837f;
        ei.width = w;
        ei.height = h;
    }
    app.renderShadows(eyes[0], eyes[1]);
    auto texMultiview = reinterpret_cast<PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC>(eglGetProcAddress("glFramebufferTextureMultiviewOVR"));
    if (app.renderer().multiviewSupported() && texMultiview) {
        // Both eyes in one pass, as on the Quest: a two-layer colour + depth target.
        GLuint arr = 0, darr = 0, mvFbo = 0, layerFbo = 0;
        glGenTextures(1, &arr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_SRGB8_ALPHA8, w, h, 2);
        glGenTextures(1, &darr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, darr);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH_COMPONENT24, w, h, 2);
        glGenFramebuffers(1, &mvFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, mvFbo);
        texMultiview(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arr, 0, 0, 2);
        texMultiview(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, darr, 0, 0, 2);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("multiview FBO incomplete");
            return 4;
        }
        app.renderer().resetStats();
        app.renderStereo(eyes[0], eyes[1]);
        LOGI("both eyes (multiview): %u draw calls (world objects %u, %u parts)", app.renderer().drawCallsThisFrame(),
             app.assetWorld() ? app.assetWorld()->statObjects() : 0u, app.assetWorld() ? app.assetWorld()->statParts() : 0u);
        glGenFramebuffers(1, &layerFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, layerFbo);
        for (int eye = 0; eye < 2; ++eye) {
            glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arr, 0, eye);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, eyePx.data());
            if (!checkGl("readback")) return 5;
            for (int y = 0; y < h; ++y)
                std::memcpy(&sbs[(size_t(y) * w * 2 + size_t(eye) * w) * 4], &eyePx[size_t(y) * w * 4], size_t(w) * 4);
        }
    } else {
        for (int eye = 0; eye < 2; ++eye) {
            const EyeRenderInfo& ei = eyes[eye];
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            app.renderer().resetStats();
            app.renderEye(ei);
            LOGI("eye %d: %u draw calls (world objects %u, %u parts)", eye, app.renderer().drawCallsThisFrame(),
                 app.assetWorld() ? app.assetWorld()->statObjects() : 0u, app.assetWorld() ? app.assetWorld()->statParts() : 0u);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, eyePx.data());
            if (!checkGl("readback")) return 5;
            for (int y = 0; y < h; ++y)
                std::memcpy(&sbs[(size_t(y) * w * 2 + size_t(eye) * w) * 4], &eyePx[size_t(y) * w * 4], size_t(w) * 4);
        }
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
