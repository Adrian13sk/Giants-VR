// Cutscene playback (flk_*.bin shots) in VR.
//
// Presentation chosen with the user for comfort: each shot is watched from a fixed viewpoint
// (the shot camera's position at the start of the shot, looking toward its target; head look is
// free), with a short fade between shots and subtitles in the game's font. Actors are the
// original models, posed by their own performance trees from the shot's director animation
// (an actor named X is the node table parented to "X_mover", or the table containing node X).
// Shots chain through the "next shot" name in each file until the cutscene ends.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "assets/flick.h"
#include "assets/gzp.h"
#include "assets/object_defs.h"
#include "render/renderer.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

// Game-frame (Z-up, game units) matrix -> runtime frame (Y-up, metres): C * m * C^-1.
Mat4 gameMatrixToRuntime(const Mat4& m);

class CutscenePlayer {
public:
    // Ground height (runtime metres) under (x, z) for feet near height y: the terrain, or the top
    // of a structure (walkway, rock) at most a little above y. Keeps actors on our ground.
    std::function<float(float x, float y, float z)> groundHeight;
    CutscenePlayer(const AssetFS& fs, ModelCache& models, Renderer& r, const ObjectRegistry& reg)
        : fs_(fs), models_(models), r_(r), reg_(reg) {}
    // Starts the cutscene at `firstShot` (e.g. "story1_jetpack_1"). False if it can't load.
    bool start(const std::string& firstShot);
    void stop() { active_ = false; actors_.clear(); }
    bool active() const { return active_; }
    void update(float dt);
    // nearCull: leave out actors within 1 m of the eye (the immersive view; not the cinema camera).
    void render(bool nearCull = true);
    // True once per new shot: where to put the viewer's eyes and which way to face.
    bool takeNewShot(Vec3& eye, float& yaw);
    // Where the shot camera is now (runtime frame). Shots whose camera travels with the action
    // (e.g. alongside the flying lander) carry the viewer along; orientation stays fixed.
    Vec3 eyeNow() const { return eyeNow_; }
    // What the shot camera looks at: its target node, else the middle of the shot's actors, else
    // 8 m ahead of it.
    Vec3 targetNow() const { return targetNow_; }
    // The shot camera's own view direction and up now (pitch and roll included; cinema screen).
    Vec3 camForwardNow() const { return camFwd_; }
    Vec3 camUpNow() const { return camUp_; }
    // Soundtrack stream to start (once), e.g. "f_jetpack"; empty if none pending.
    std::string takeStream();
    // Timed streams (op 0x2C: sound effects, voice clips) due since the last call.
    std::vector<std::string> takeCues();
    // Effects (op 0x27) due since the last call: name and world position of the actor node.
    struct FxEvent { std::string name; Vec3 pos; int mode; };
    std::vector<FxEvent> takeEffects();
    // The subtitle line(s) due now: the latest whose time has come (each holds until the next).
    std::vector<std::string> subtitles() const;
    float blackout() const;      // 0 (clear) .. 1 (black), for the fades between shots
    float shotTime() const { return t_; }
    float shotDuration() const { return duration_; }
    const std::string& shotName() const { return shotName_; }
    size_t shotsPlayed() const { return shotsPlayed_; }
    // Object types appearing as actors in the current shot (the game hides placed duplicates).
    std::vector<int> actorTypes() const;

private:
    struct Actor {
        FlickActor info;
        std::unique_ptr<SkinnedModel> skinned;
        int clip = -1;
        const RenderModel* model = nullptr;
        int mover = -1;   // director node carrying the actor's tree (or -1)
        int node = -1;    // director node for a static actor
        int table = -1;   // director node table driving the actor
        int parent = -1;  // attached (op 3): index into actors_ of the carrier
        std::string parentNode, align;  // carrier bone; own node aligned to it ("ref_handle")
        // Static (.gbs) props: reference nodes from the model's .anm at rest (game frame), for
        // the alignment node and effect points ("ref_nozzle").
        std::vector<std::pair<std::string, Mat4>> refs;
        bool refNode(const std::string& name, Mat4& out) const {
            for (const auto& r : refs)
                if (r.first == name) { out = r.second; return true; }
            return false;
        }
        Mat4 xf = Mat4::identity();  // runtime transform this frame
        bool visible = true;
        mutable bool nearLogged = false;
        bool fixed = false;  // placed in the world (op 0x20): xf set once
        float groundOffset = 0;  // ground-following actors: vertical correction for this shot (m)
        bool grounded = false;   // groundOffset applies (otherwise only sunk feet are lifted)
    };
    // Ground-following actors (op 0x22): the lowest their feet come over the whole shot is put on
    // our ground, so standing actors neither float nor sink while hops and jumps keep their arc.
    void groundActors();
    bool loadShot(const std::string& name);
    void updateProps(float t);
    const AssetFS& fs_;
    ModelCache& models_;
    Renderer& r_;
    const ObjectRegistry& reg_;
    bool active_ = false, newShot_ = false;
    FlickShot shot_;
    std::string shotName_, pendingStream_, streamStarted_;
    std::vector<Actor> actors_;
    std::vector<Mat4> world_;
    float t_ = 0, duration_ = 0;
    float startVis_ = 1.0f;  // screen visibility when the shot began (the script's fades carry over)
    float scriptVisibility(float t) const;  // the shot's op 0x1F fades at t (1 picture, 0 black)
    size_t shotsPlayed_ = 0;
    size_t cueNext_ = 0;
    size_t fxNext_ = 0;
    std::vector<FxEvent> dueFx_;
    std::vector<std::string> dueCues_;
    Vec3 eye_, eyeNow_, eyeOverride_, targetNow_;
    Vec3 camFwd_{0, 0, -1}, camUp_{0, 1, 0};
    int tgtNode_ = -1;
    bool hasEyeOverride_ = false;
    int camNode_ = -1;
    float yaw_ = 0;
};

}  // namespace gvr
