// GameApp, rework: cutscenes on a cinema screen (user: "All cutscenes need fixing ... actors
// misplacement, black screen, objects placement"). The original shots were staged for one camera:
// its pitch (a flyover looking down on the island, a shot looking up at Kabuto), its framing and its
// cuts. Seen from inside the scene with a level, free head, many showed sky, a floor or the empty side
// of a set the artists never dressed. Here each frame of the shot is rendered from the shot camera
// itself - position, direction, roll - into a 4:3 picture (4x MSAA) and shown on a large screen
// standing in a dark room in front of you; the screen stays put through the cuts (it is fixed in the
// room, not to your head). Options -> CUTSCENES: CINEMA SCREEN (default) or AROUND YOU (the earlier
// immersive view). GVR_CUT_VIEW=around/screen overrides (tests). Our own design.
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "game/cutscene.h"
#include "game/game_app.h"
#include "scene/asset_world.h"
#include "scene/terrain.h"

namespace gvr {

namespace {
// The picture: 4:3 at 75 degrees across (the data carries no field of view).
constexpr int kScreenW = 1024, kScreenH = 768;
constexpr float kHalfFovX = 0.6545f;  // 37.5 degrees
// The screen in the room: 3.6 m wide, 3.2 m ahead of where you stood when it appeared.
constexpr float kScreenWidth = 3.6f, kScreenDist = 3.2f;
}  // namespace

bool GameApp::cinemaActive() const {
    if (!cutsceneActive()) return false;
    static const char* force = std::getenv("GVR_CUT_VIEW");
    if (force && std::strcmp(force, "around") == 0) return false;
    const bool want = (force && std::strcmp(force, "screen") == 0) || settings_.cutsceneScreen;
    return want && renderer_.screenReady();
}

void GameApp::renderCinema() {
    static const char* force = std::getenv("GVR_CUT_VIEW");
    const bool want = cutsceneActive() && !(force && std::strcmp(force, "around") == 0) &&
                      ((force && std::strcmp(force, "screen") == 0) || settings_.cutsceneScreen);
    if (!want) {
        cinemaAnchored_ = false;
        return;
    }
    if (!cinemaInit_) {
        cinemaInit_ = true;
        renderer_.initScreen(kScreenW, kScreenH);
        cinemaBatch_.init(renderer_, 1);
    }
    if (!renderer_.screenReady()) return;
    // The shot camera now, exactly where the shot puts it: a camera standing inside a cliff sees out
    // through the back of the terrain, as in the original (Kstory5_End_18's sits in the rock - lifting
    // it onto the top, as the immersive view must, showed the ground instead of the cast).
    const Vec3 eye = cutscene_->eyeNow();
    const Vec3 f = normalize(cutscene_->camForwardNow());
    Vec3 up = cutscene_->camUpNow();
    const Vec3 z = f * -1.0f;
    Vec3 x = cross(up, z);
    if (length(x) < 1e-4f) x = cross(Vec3{0, 0, 1}, z);
    x = normalize(x);
    const Vec3 y = cross(z, x);
    ViewParams cv;
    Mat4 vm = Mat4::identity();
    vm.m[0] = x.x; vm.m[4] = x.y; vm.m[8] = x.z;
    vm.m[1] = y.x; vm.m[5] = y.y; vm.m[9] = y.z;
    vm.m[2] = z.x; vm.m[6] = z.y; vm.m[10] = z.z;
    vm.m[12] = -dot(x, eye); vm.m[13] = -dot(y, eye); vm.m[14] = -dot(z, eye);
    cv.view = vm;
    cv.eyePosition = eye;
    const float hy = std::atan(std::tan(kHalfFovX) * float(kScreenH) / float(kScreenW));
    cv.projection = Mat4::projectionFov(-kHalfFovX, kHalfFovX, hy, -hy, 0.1f, 0.0f);
    const std::vector<int> cutTypes = cutscene_->actorTypes();
    renderer_.beginScreenPass(cv);
    renderWorld(cv, true, cutTypes);
    cutscene_->render(false);  // (the shot's own camera: what is near it was in its picture)
    renderer_.endScreenPass();
    static int logged = 0;
    if (logged < 2) {
        ++logged;
        LOGI("Cinema: shot %s from (%.1f %.1f %.1f) looking (%.2f %.2f %.2f)", cutscene_->shotName().c_str(), eye.x, eye.y, eye.z, f.x, f.y, f.z);
    }
}

void GameApp::renderCinemaRoom(const ViewParams& v) {
    (void)v;
    if (!renderer_.screenReady()) return;
    // Where the screen stands, in the room (tracking space): ahead of the head as it was when the
    // cutscene began, level, at eye height.
    if (!cinemaAnchored_ && lastInput_.headValid) {
        const Vec3 hf = lastInput_.head.orientation.rotate(Vec3{0, 0, -1});
        const float yaw = std::atan2(-hf.x, -hf.z);
        cinemaAnchor_.orientation = Quat::fromAxisAngle({0, 1, 0}, yaw);
        // (GVR_CINEMA_FILL, reviews: close enough to fill a spectator frame)
        static const float dist = std::getenv("GVR_CINEMA_FILL") ? 1.75f : kScreenDist;
        cinemaAnchor_.position = lastInput_.head.position + Vec3{-std::sin(yaw), 0, -std::cos(yaw)} * dist + Vec3{0, -0.05f, 0};
        cinemaAnchored_ = true;
    }
    if (!cinemaAnchored_) {
        cinemaAnchor_.orientation = Quat{0, 0, 0, 1};
        cinemaAnchor_.position = Vec3{0, 1.6f, -kScreenDist};
    }
    const float w = kScreenWidth, h = kScreenWidth * float(kScreenH) / float(kScreenW);
    auto room = [&](const Vec3& local) {
        Pose p;
        p.position = cinemaAnchor_.position + cinemaAnchor_.orientation.rotate(local);
        p.orientation = cinemaAnchor_.orientation;
        return rig_.toWorld(p);
    };
    const Pose centre = room(Vec3{0, 0, 0});
    {
        const Pose sub = room(Vec3{0, -0.5f * h - 0.22f, 0.05f});
        cinemaSubXf_ = Mat4::translation(sub.position) * Mat4::rotation(sub.orientation) * Mat4::scale({1.9f, 1.9f, 1.9f});
        cinemaSubValid_ = true;
    }
    // A dark frame round the picture and a dim floor, so the room has a shape.
    if (text_.ready()) {
        text_.drawPanel(Mat4::translation(room(Vec3{0, 0, -0.03f}).position) * Mat4::rotation(centre.orientation), w + 0.16f, h + 0.16f,
                        {0.05f, 0.05f, 0.06f, 1.0f}, true);
        Pose floor;
        floor.position = Vec3{lastInput_.head.position.x, 0.0f, lastInput_.head.position.z};
        floor.orientation = cinemaAnchor_.orientation * Quat::fromAxisAngle({1, 0, 0}, -1.5708f);
        const Pose fw = rig_.toWorld(floor);
        text_.drawPanel(Mat4::translation(fw.position) * Mat4::rotation(fw.orientation), 9.0f, 9.0f, {0.025f, 0.025f, 0.03f, 1.0f}, true);
    }
    const Vec3 c[4] = {room(Vec3{-0.5f * w, -0.5f * h, 0}).position, room(Vec3{0.5f * w, -0.5f * h, 0}).position,
                       room(Vec3{0.5f * w, 0.5f * h, 0}).position, room(Vec3{-0.5f * w, 0.5f * h, 0}).position};
    cinemaBatch_.clear();
    static const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};  // (the picture's v = 0 is its bottom)
    const uint8_t white[4] = {255, 255, 255, 255};
    cinemaBatch_.add(c, uv, white);
    cinemaBatch_.upload(renderer_);
    DrawParams dp;
    dp.texture = renderer_.screenTexture();
    dp.unlit = true;
    dp.noFog = true;  // (emissive: the picture as rendered)
    const float k = 1.0f - std::max(0.0f, std::min(1.0f, cutscene_->blackout()));  // the shot's fades: on the picture
    dp.tint = {k, k, k, 1.0f};
    cinemaBatch_.draw(renderer_, dp);
}

}  // namespace gvr
