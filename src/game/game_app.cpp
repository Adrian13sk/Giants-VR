#include "game/game_app.h"

#include "core/log.h"
#include "scene/asset_world.h"

namespace gvr {

GameApp::GameApp() = default;
GameApp::~GameApp() = default;

bool GameApp::init(const std::string& dataDir, const std::string& worldName) {
    if (!renderer_.init()) {
        LOGE("Renderer init failed");
        return false;
    }
    if (!testScene_.init(renderer_)) {
        LOGE("Test scene init failed");
        return false;
    }
    assetWorld_ = std::make_unique<AssetWorld>();
    if (!assetWorld_->init(dataDir, renderer_, worldName)) {
        LOGW("Original assets unavailable: %s", assetWorld_->status().c_str());
    }
    if (assetWorld_->hasTerrain()) {
        showTestScene_ = false;
        needSpawn_ = true;
        AssetWorld* w = assetWorld_.get();
        // Walk on the original terrain; keep the current height when off the heightfield.
        rig_.groundHeight = [w, this](float x, float z) {
            float y;
            return w->terrain().heightAt(x, z, y) ? y : rig_.worldFromTracking.position.y;
        };
    } else {
        rig_.groundHeight = [](float, float) { return 0.0f; };
    }
    return true;
}

bool GameApp::hasOriginalAssets() const { return assetWorld_ && assetWorld_->loaded(); }

void GameApp::update(float dt, const InputFrame& in) {
    time_ += dt;
    if (needSpawn_ && in.headValid) {
        rig_.teleportHead(in, assetWorld_->spawnPoint(), assetWorld_->spawnYaw());
        needSpawn_ = false;
    }
    // Y (B when left-handed) toggles the debug fly/noclip mode. Haptics: double pulse = on.
    const int mh = rig_.moveHand();
    if (flyEdge_.pressed(in.hand[mh].secondary)) {
        rig_.flyMode = !rig_.flyMode;
        LOGI("Fly/noclip mode %s", rig_.flyMode ? "ON" : "OFF");
        haptics_.push_back({mh, rig_.flyMode ? 0.9f : 0.3f, rig_.flyMode ? 0.15f : 0.05f});
    }
    rig_.update(dt, in);
    if (assetWorld_) assetWorld_->update(dt);

    // Haptic confirmation on trigger press (validates the haptics path on device).
    for (int h = 0; h < 2; ++h)
        if (triggerEdge_[h].pressed(in.hand[h].trigger > 0.6f))
            haptics_.push_back({h, 0.5f, 0.05f});
    // Left thumbstick click toggles the engineering test geometry.
    if (modeEdge_.pressed(in.hand[kLeft].thumbstickClick)) showTestScene_ = !showTestScene_;
    lastInput_ = in;
}

void GameApp::renderEye(const EyeRenderInfo& eye) {
    Pose eyeWorld = rig_.toWorld(eye.trackingPose);
    ViewParams v;
    v.view = Mat4::fromPose(eyeWorld.inverse());
    v.projection = Mat4::projectionFov(eye.fovLeft, eye.fovRight, eye.fovUp, eye.fovDown, 0.05f, 0.0f);
    v.eyePosition = eyeWorld.position;
    renderer_.beginView(v, eye.width, eye.height, true);
    if (assetWorld_ && assetWorld_->loaded()) assetWorld_->render(renderer_);
    if (showTestScene_ || !hasOriginalAssets()) testScene_.render(renderer_, rig_, lastInput_);
    else testScene_.renderControllers(renderer_, rig_, lastInput_);
    renderer_.endView();
}

void GameApp::shutdown() {
    assetWorld_.reset();
    renderer_.shutdown();
}

}  // namespace gvr
