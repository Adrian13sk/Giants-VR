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
    // Both eyes in one pass: one swapchain with two layers (GL_OVR_multiview). bindEyeFramebuffer /
    // releaseEye still work then (each eye's layer on its own), for a renderer without multiview.
    bool multiview() const { return multiview_; }
    int msaaSamples() const { return msaaSamples_; }  // the eye buffers' MSAA (1: none)
    EyeBindResult bindStereoFramebuffer();
    void releaseStereo();
    // A framebuffer to read one eye's finished image from (test screenshots); 0 if none bound.
    GLuint eyeReadFramebuffer(int eye) const;
    // Returns false if the session/instance was lost.
    bool endFrame(const XrFrameContext& frame);

    XrInput& input() { return input_; }
    XrInstance instance() const { return instance_; }
    const std::string& runtimeName() const { return runtimeName_; }

    XrReferenceSpaceType appSpaceType() const { return appSpaceType_; }
    float currentRefreshRate() const { return refreshRate_; }
    // Scripted test runs: render even without tracking (a headset lying on a desk), from eye poses
    // the test supplies (overrideEyePoses after beginFrame).
    void setTestMode(bool on) { testMode_ = on; }
    void overrideEyePoses(XrFrameContext& frame, const Pose eyes[2]);
    // Fixed foveated rendering (XR_FB_foveation): 0 off .. 3 high; the edges of each eye are
    // shaded at lower resolution. False when the runtime doesn't offer it.
    bool setFoveation(int level, bool dynamic);
    int foveation() const { return foveationLevel_; }

private:
    uint32_t recenterSerial_ = 0;
    bool testMode_ = false;
    int msaaSamples_ = 1;
    struct Swapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        int width = 0, height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        std::vector<GLuint> framebuffers;   // per image (multiview: both layers)
        std::vector<GLuint> layerFbos;      // multiview: per image and eye, one layer each
        GLuint depth = 0;                   // renderbuffer (per eye) or 2-layer texture (multiview)
        uint32_t index = 0;                 // the acquired image
        bool acquired = false;
    };

    bool hasExtension(const char* name) const;
    bool createSwapchains();
    bool createMultiviewSwapchain();
    EyeBindResult acquire(Swapchain& sc, int eye);
    Swapchain& swapchainFor(int eye) { return swapchains_[multiview_ ? 0 : eye]; }
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
    Swapchain swapchains_[2];  // per eye, or [0] only (multiview)
    bool multiview_ = false;
    int64_t colorFormat_ = 0;
    std::string runtimeName_;
    Config config_;
    XrInput input_;
    std::vector<XrCompositionLayerProjectionView> layerViews_;
    PFN_xrRequestDisplayRefreshRateFB pfnRequestRefresh_ = nullptr;
    PFN_xrCreateFoveationProfileFB pfnCreateFoveation_ = nullptr;
    PFN_xrDestroyFoveationProfileFB pfnDestroyFoveation_ = nullptr;
    PFN_xrUpdateSwapchainFB pfnUpdateSwapchain_ = nullptr;
    XrFoveationProfileFB foveationProfile_ = XR_NULL_HANDLE;
    int foveationLevel_ = -1;
    PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumerateRefresh_ = nullptr;
};

}  // namespace gvr
