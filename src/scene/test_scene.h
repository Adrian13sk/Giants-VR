// Engineering test scene (M3-M5): floor grid, distance markers and controller visualisation.
// Synthetic content only - it is NOT original Giants: Citizen Kabuto content.
#pragma once

#include "game/input_state.h"
#include "game/vr_rig.h"
#include "render/renderer.h"

namespace gvr {

class TestScene {
public:
    bool init(Renderer& r);
    void render(Renderer& r, const VrRig& rig, const InputFrame& input);
    // Controller indicators only (used on top of original-content scenes).
    void renderControllers(Renderer& r, const VrRig& rig, const InputFrame& input);
    void renderPointers(Renderer& r, const VrRig& rig, const InputFrame& input);

private:
    MeshId floor_ = 0, marker_ = 0, pillar_ = 0, sphere_ = 0, box_ = 0, ray_ = 0, giant_ = 0;
    TextureId checker_ = 0;
};

}  // namespace gvr
