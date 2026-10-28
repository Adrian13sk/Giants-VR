// Billboard particles (renderer-independent): smoke, dust, fire and the trails of rockets and
// Ripper spit, drawn by the game layer with the original effect textures (EFX_smoke_black,
// EFX_smoke_Mgrey, b_smoke, b_dust, EFX_fire_trail / Bfire / Gfire).
#pragma once

#include <cstdint>
#include <vector>

#include "core/math.h"

namespace gvr {

enum class FxTex : int { Smoke = 0, SmokeBlack, SmokeGrey, Dust, Fire, FireBlue, FireGreen, Drip, Spray, Count };  // Drip: water drops (additive), Spray: splash spray

struct Particle {
    Vec3 pos, vel;
    float age = 0, life = 1;
    float size0 = 0.3f, size1 = 1.0f;  // metres, start -> end
    float alpha = 1;                   // peak opacity
    float angle = 0, spin = 0;         // billboard roll
    float drag = 0;                    // 1/s velocity decay
    float rise = 0;                    // m/s^2 buoyancy (smoke) or negative for gravity
    FxTex tex = FxTex::Smoke;
    bool additive = false;             // fire glows; smoke and dust blend
    uint32_t seed = 0;                 // (set by emit: stable per particle - which ones a crowded frame leaves out)
    float size() const;
    float opacity() const;             // fades in quickly, out over the second half
};

class ParticleSystem {
public:
    size_t maxParticles = 600;
    uint32_t nextSeed = 1;
    std::vector<Particle> parts;
    void emit(const Particle& p);
    void update(float dt);
};

}  // namespace gvr
