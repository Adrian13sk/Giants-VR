// Platform-independent application core. The Quest runtime (OpenXR) and the headless desktop
// harness both drive this class: update() once per frame, renderEye() once per eye.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "game/input_state.h"
#include "game/vr_rig.h"
#include "render/renderer.h"
#include "scene/test_scene.h"

namespace gvr {

struct EyeRenderInfo {
    Pose trackingPose;  // eye pose in tracking space
    float fovLeft, fovRight, fovUp, fovDown;
    int width, height;
};

struct HapticRequest {
    int hand;
    float amplitude;
    float seconds;
};

class AssetWorld;  // original-asset scene (defined in scene/asset_world.h)

class GameApp {
public:
    GameApp();
    ~GameApp();
    // GL context must be current. dataDir: directory holding the user's original game files
    // (the contents of the game's "Bin" folder). Missing data is not fatal.
    bool init(const std::string& dataDir, const std::string& worldName = "Story1");
    void update(float dt, const InputFrame& input);
    void renderEye(const EyeRenderInfo& eye);
    void shutdown();

    std::vector<HapticRequest>& hapticRequests() { return haptics_; }
    VrRig& rig() { return rig_; }
    Renderer& renderer() { return renderer_; }
    bool hasOriginalAssets() const;

private:
    Renderer renderer_;
    VrRig rig_;
    TestScene testScene_;
    std::unique_ptr<AssetWorld> assetWorld_;
    InputFrame lastInput_;
    std::vector<HapticRequest> haptics_;
    ButtonEdge triggerEdge_[2];
    ButtonEdge modeEdge_;
    ButtonEdge flyEdge_;
    bool showTestScene_ = true;
    bool needSpawn_ = false;
    double time_ = 0.0;
};

}  // namespace gvr
