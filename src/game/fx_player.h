// Plays the game's own effects from fx.bin (assets/fx_defs.h): each effect is a list of emitters,
// each an efx_* object (a textured quad, a debris mesh, or an invisible "reference shape" whose
// animation carries Ref_debriNN points outward); child effects ride those points.
// Renderer-independent: the game app draws `particles` and plays `sounds`.
//
// What the ops mean is read from the values (FORMATS.md, "fx.bin"); ours where noted:
//   0b t      wait t seconds before the next emitter starts
//   03 type   an emitter of that GameObj type; 06 ... 07 its parameters:
//     04 life, fadeIn, fadeOut (s)   08 size from, to (game units)   09 size from a..b to c..d
//     1b scale (reference shapes)    1c size factor range            12 speed range (units/s, random direction)
//     0e gravity (x 9.8 m/s^2; negative rises)   0f rise range (ours: like 0e, picked per particle)
//     19 bounce   33 start rotation ranges (degrees)   13 spin range (turns/s, ours)
//     21 the emitter is a reference shape (not drawn)
//     10 / 0a / 70 {n, "effect", 0}: that effect rides the shape's Ref_debri<n> point
//     5b {"effect", every s}: leaves that effect behind it (a trail)
//   2b {sound, sound}: a sound at the start (ids -> names by the caller)
//   24 {object type, emitter type, k}: an emitter riding a game object (a projectile's trail: FX_rocket =
//     mc_rocket + efx_ref_line_back). Started with attach(), the caller moves it every frame; k = -1
//     effects (foot dust, smoke) are self-contained and start like 03 with the emitter type.
//     In its block: 20 {colour from rgb, to rgb}, 22 {width from, to (game units)}, 25 {seconds}: a
//     streak left behind it fading over that time (ours, from the values); 26 read as its opacity
//     (provisional); 0a/10/70 with n = 0: a child effect riding the object itself (kept alive while
//     it flies); 27 {"effect", ...}: like 5b, every frame.
// Unknown ops are skipped. One instance draws at most kMaxParticles.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/fx_defs.h"
#include "assets/skin_anim.h"
#include "core/math.h"

namespace gvr {

class AssetFS;
class ObjectRegistry;
class ModelCache;
struct RenderModel;

class FxPlayer {
public:
    struct Shape;
    struct Particle {
        const RenderModel* model = nullptr;
        bool additive = false, billboard = true, ref = false;
        Vec3 pos, vel;
        Quat rot;
        Vec3 spinAxis{0, 1, 0};
        float spin = 0;                 // rad/s
        float age = 0, life = 1, fadeIn = 0, fadeOut = 0;
        float size0 = 1, size1 = 1;     // metres (quads) / scale (meshes)
        float gravity = 0, bounce = 0;
        uint32_t id = 0, parent = 0;    // riding the parent's Ref_debri<node> point (0: free)
        int node = -1;
        Vec3 offset;                    // from that point
        bool attached = false, fromAttach = false;  // moved by the caller (attach / moveAttached); dies when no longer moved
        bool parentAttached = false;    // riding an attached emitter's own position (n = 0)
        float unmoved = 0;              // seconds since the caller last moved it
        // The streak an attached emitter leaves (20/22/25/26).
        bool streak = false;
        float sc0[3] = {1, 1, 1}, sc1[3] = {1, 1, 1}, sw0 = 0, sw1 = 0, slife = 0, sopacity = 1;
        struct StreakPt { Vec3 pos; float age; };
        std::vector<StreakPt> streakPts;
        Shape* shape = nullptr;         // a reference shape: its animated points, this frame
        float shapeScale = 1;
        std::vector<Vec3> points;
        float alpha() const;
        float size() const;
    };
    std::vector<Particle> particles;
    struct Sound { std::string name; Vec3 pos; };
    std::vector<Sound> sounds;  // started this update (the caller plays and clears them)

    // Hooks: sound id -> name; ground height at (x, z) for bouncing debris.
    std::function<std::string(uint32_t)> soundName;
    std::function<bool(float, float, float&)> groundAt;

    void init(const FxLibrary* lib, const ObjectRegistry* reg, ModelCache* models, const AssetFS* fs);
    bool ready() const { return lib_ != nullptr; }
    // Starts an effect at `at` (runtime metres), oriented by `rot`, sized by `scale`. False if unknown.
    bool play(const std::string& name, const Vec3& at, const Quat& rot = Quat{}, float scale = 1.0f);
    // An effect riding a moving object (its 24 op): returns a handle (0 if the effect has none), moved
    // with moveAttached every frame (false once it is gone) and ended with detach; one not moved for
    // half a second ends by itself.
    uint32_t attach(const std::string& name, const Vec3& at, const Quat& rot = Quat{}, float scale = 1.0f);
    bool moveAttached(uint32_t handle, const Vec3& at, const Quat& rot);
    void detach(uint32_t handle);
    void update(float dt);
    void clear() { particles.clear(); pending_.clear(); sounds.clear(); trails_.clear(); }
    static constexpr size_t kMaxParticles = 600;

public:
    struct Shape {  // a reference shape's animation
        AnmAnimation anim;
        std::vector<int> refNode;  // Ref_debri<n> -> node index (n from 1; -1 missing)
        bool ok = false;
    };

private:
    struct Pending { const FxDef* def; size_t op; float wait; Vec3 at; Quat rot; float scale; uint32_t parent; int node; };
    struct TypeInfo { const RenderModel* model = nullptr; bool additive = false, billboard = true; std::string anim; };
    const FxLibrary* lib_ = nullptr;
    const ObjectRegistry* reg_ = nullptr;
    ModelCache* models_ = nullptr;
    const AssetFS* fs_ = nullptr;
    std::vector<Pending> pending_;
    std::unordered_map<uint32_t, TypeInfo> types_;
    std::unordered_map<std::string, std::unique_ptr<Shape>> shapes_;
    struct Trail { uint32_t particle; const FxDef* effect; float every, t; };
    std::vector<Trail> trails_;
    uint32_t nextId_ = 1;
    bool attaching_ = false;   // run() from attach(): waits before the 24 op are skipped
    uint32_t attachedRoot_ = 0;
    Particle* findMut(uint32_t id);
    const TypeInfo& typeInfo(uint32_t type);
    Shape* shape(const std::string& anim);
    // Runs a definition from op index `from` until it waits (0b) or ends; new emitters ride
    // (parent, node) when given.
    void run(const FxDef* def, size_t from, const Vec3& at, const Quat& rot, float scale, uint32_t parent, int node);
    const Particle* byId(uint32_t id) const;
};

}  // namespace gvr
