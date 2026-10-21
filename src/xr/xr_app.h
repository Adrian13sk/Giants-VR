// OpenXR instance / session / swapchain / frame-loop management for the Quest (GLES) build.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/math.h"
#include "render/gl.h"
#include "xr/xr_common.h"
#include "xr/xr_input.h"

namespace gvr {

struct EyeView {
    Pose pose;          // eye pose in tracking space
    float fovLeft = 0, fovRight = 0, fovUp = 0, fovDown = 0;  // radians
    int width = 0, height = 0;
};

struct XrFrameContext {
    XrTime predictedDisplayTime = 0;
    XrDuration predictedDisplayPeriod = 0;
    bool shouldRender = false;
    EyeView eyes[2];
    InputFrame input;
};

class XrApp {
public:
    struct Config {
        const char* appName = "GiantsVR";
        float preferredRefreshHz = 72.0f;
    };

    // Android: vm = JavaVM*, activity = jobject (NativeActivity clazz).
    bool initLoader(void* vm, void* activity);
    bool createInstance(void* vm, void* activity, const Config& cfg);
    bool createSession(EGLDisplay display, EGLConfig config, EGLContext context);
    void destroy();

    // Pump the OpenXR event queue. Sets exitRequested when the app should quit.
    void pollEvents(bool& exitRequested);
    bool sessionRunning() const { return sessionRunning_; }
    bool focused() const { return sessionState_ == XR_SESSION_STATE_FOCUSED; }

    // Frame loop: waitFrame + beginFrame + locate views + input sync.
    bool beginFrame(XrFrameContext& frame);
    // Bind the eye's current swapchain image as the GL draw framebuffer.
    enum class EyeBindResult { Ok, Skipped, Fatal };
    EyeBindResult bindEyeFramebuffer(int eye);
    void releaseEye(int eye);
    // Returns false if the session/instance was lost.
    bool endFrame(const XrFrameContext& frame);

    XrInput& input() { return input_; }
    XrInstance instance() const { return instance_; }
    const std::string& runtimeName() const { return runtimeName_; }

    XrReferenceSpaceType appSpaceType() const { return appSpaceType_; }
    float currentRefreshRate() const { return refreshRate_; }

private:
    struct Swapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        int width = 0, height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        std::vector<GLuint> framebuffers;
        GLuint depth = 0;
        bool acquired = false;
    };

    bool hasExtension(const char* name) const;
    bool createSwapchains();
    bool createReferenceSpace();
    void handleSessionStateChange(const XrEventDataSessionStateChanged& e, bool& exitRequested);
    void applyRefreshRate();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrReferenceSpaceType appSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    float refreshRate_ = 0.0f;
    std::vector<std::string> availableExtensions_;
    std::vector<const char*> enabledExtensions_;
    std::vector<XrViewConfigurationView> configViews_;
    std::vector<XrView> views_;
    Swapchain swapchains_[2];
    int64_t colorFormat_ = 0;
    std::string runtimeName_;
    Config config_;
    XrInput input_;
    std::vector<XrCompositionLayerProjectionView> layerViews_;
    PFN_xrRequestDisplayRefreshRateFB pfnRequestRefresh_ = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumerateRefresh_ = nullptr;
};

}  // namespace gvr
