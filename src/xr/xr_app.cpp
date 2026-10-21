#include "xr/xr_app.h"

#include <algorithm>
#include <cstring>

namespace gvr {

const char* xrResultName(XrInstance instance, XrResult r) {
    static thread_local char buf[XR_MAX_RESULT_STRING_SIZE];
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, r, buf))) return buf;
    std::snprintf(buf, sizeof(buf), "XrResult(%d)", static_cast<int>(r));
    return buf;
}

static Pose toPose(const XrPosef& p) {
    Pose r;
    r.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    r.position = {p.position.x, p.position.y, p.position.z};
    return r;
}

bool XrApp::hasExtension(const char* name) const {
    for (const auto& e : availableExtensions_)
        if (e == name) return true;
    return false;
}

bool XrApp::initLoader(void* vm, void* activity) {
#if defined(__ANDROID__)
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    XrResult r = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&initLoader));
    if (XR_FAILED(r) || !initLoader) {
        LOGE("xrInitializeLoaderKHR not available (%d)", static_cast<int>(r));
        return false;
    }
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = vm;
    li.applicationContext = activity;
    r = initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li));
    if (XR_FAILED(r)) {
        LOGE("xrInitializeLoaderKHR failed (%d)", static_cast<int>(r));
        return false;
    }
    LOGI("OpenXR loader initialised");
#else
    (void)vm; (void)activity;
#endif
    return true;
}

bool XrApp::createInstance(void* vm, void* activity, const Config& cfg) {
    config_ = cfg;
    uint32_t count = 0;
    XR_CHECK_RET(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), false);
    std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK_RET(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data()), false);
    for (const auto& p : props) {
        availableExtensions_.emplace_back(p.extensionName);
        LOGI("  runtime extension: %s v%u", p.extensionName, p.extensionVersion);
    }

    const char* required[] = {
#if defined(__ANDROID__)
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
#endif
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };
    for (const char* e : required) {
        if (!hasExtension(e)) {
            LOGE("Required OpenXR extension missing: %s", e);
            return false;
        }
        enabledExtensions_.push_back(e);
    }
    const char* optional[] = {
        XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
        XR_EXT_LOCAL_FLOOR_EXTENSION_NAME,
    };
    for (const char* e : optional)
        if (hasExtension(e)) enabledExtensions_.push_back(e);

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
#if defined(__ANDROID__)
    XrInstanceCreateInfoAndroidKHR aci{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    aci.applicationVM = vm;
    aci.applicationActivity = activity;
    ci.next = &aci;
#else
    (void)vm; (void)activity;
#endif
    std::strncpy(ci.applicationInfo.applicationName, cfg.appName, XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    std::strncpy(ci.applicationInfo.engineName, "GiantsVR", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 1;
    // Request 1.0 for maximum runtime compatibility; we use no 1.1-only core features.
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions_.size());
    ci.enabledExtensionNames = enabledExtensions_.data();
    XR_CHECK_RET(xrCreateInstance(&ci, &instance_), false);

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &ip))) {
        runtimeName_ = ip.runtimeName;
        LOGI("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
             XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    }

    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK_RET(xrGetSystem(instance_, &si, &systemId_), false);
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &sp))) {
        LOGI("System: %s, maxLayers %u, maxSwapchain %ux%u, orientTracking %d posTracking %d",
             sp.systemName, sp.graphicsProperties.maxLayerCount,
             sp.graphicsProperties.maxSwapchainImageWidth,
             sp.graphicsProperties.maxSwapchainImageHeight,
             sp.trackingProperties.orientationTracking, sp.trackingProperties.positionTracking);
    }

    count = 0;
    XR_CHECK_RET(xrEnumerateViewConfigurationViews(instance_, systemId_,
                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                                   &count, nullptr), false);
    configViews_.assign(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK_RET(xrEnumerateViewConfigurationViews(instance_, systemId_,
                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count,
                                                   &count, configViews_.data()), false);
    if (configViews_.size() != 2) {
        LOGE("Expected 2 stereo views, runtime reports %zu", configViews_.size());
        return false;
    }
    for (size_t i = 0; i < configViews_.size(); ++i)
        LOGI("View %zu recommended %ux%u (max %ux%u) samples %u", i,
             configViews_[i].recommendedImageRectWidth, configViews_[i].recommendedImageRectHeight,
             configViews_[i].maxImageRectWidth, configViews_[i].maxImageRectHeight,
             configViews_[i].recommendedSwapchainSampleCount);
    views_.assign(2, {XR_TYPE_VIEW});
    return true;
}

bool XrApp::createSession(EGLDisplay display, EGLConfig config, EGLContext context) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReqs = nullptr;
    XR_CHECK_RET(xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&getReqs)), false);
    XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK_RET(getReqs(instance_, systemId_, &reqs), false);
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const XrVersion have = XR_MAKE_VERSION(major, minor, 0);
    LOGI("GLES %d.%d; runtime requires >= %u.%u", major, minor,
         XR_VERSION_MAJOR(reqs.minApiVersionSupported), XR_VERSION_MINOR(reqs.minApiVersionSupported));
    if (have < reqs.minApiVersionSupported) {
        LOGE("GLES context version too low for the OpenXR runtime");
        return false;
    }

#if defined(__ANDROID__)
    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = display;
    binding.config = config;
    binding.context = context;
#endif
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = systemId_;
    XR_CHECK_RET(xrCreateSession(instance_, &sci, &session_), false);
    LOGI("OpenXR session created");

    if (!createReferenceSpace()) return false;
    if (!createSwapchains()) return false;
    if (!input_.init(instance_, session_)) return false;

    if (hasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
        xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnRequestRefresh_));
        xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnEnumerateRefresh_));
    }
    return true;
}

bool XrApp::createReferenceSpace() {
    uint32_t n = 0;
    XR_CHECK_RET(xrEnumerateReferenceSpaces(session_, 0, &n, nullptr), false);
    std::vector<XrReferenceSpaceType> types(n);
    XR_CHECK_RET(xrEnumerateReferenceSpaces(session_, n, &n, types.data()), false);
    auto has = [&](XrReferenceSpaceType t) {
        return std::find(types.begin(), types.end(), t) != types.end();
    };

    XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.poseInReferenceSpace.orientation.w = 1.0f;
    if (hasExtension(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME) && has(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT)) {
        appSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
    } else if (has(XR_REFERENCE_SPACE_TYPE_STAGE)) {
        appSpaceType_ = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else {
        // Seated fallback: put the origin 1.6 m below the initial head position so the
        // virtual floor is approximately where the user's floor is.
        appSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rci.poseInReferenceSpace.position.y = -1.6f;
    }
    rci.referenceSpaceType = appSpaceType_;
    XR_CHECK_RET(xrCreateReferenceSpace(session_, &rci, &appSpace_), false);
    LOGI("App reference space type: %d", static_cast<int>(appSpaceType_));

    XrReferenceSpaceCreateInfo vci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vci.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK_RET(xrCreateReferenceSpace(session_, &vci, &viewSpace_), false);
    return true;
}

bool XrApp::createSwapchains() {
    uint32_t n = 0;
    XR_CHECK_RET(xrEnumerateSwapchainFormats(session_, 0, &n, nullptr), false);
    std::vector<int64_t> formats(n);
    XR_CHECK_RET(xrEnumerateSwapchainFormats(session_, n, &n, formats.data()), false);
    // Prefer sRGB so the compositor receives correctly encoded colour.
    const int64_t preferred[] = {GL_SRGB8_ALPHA8, GL_RGBA8};
    colorFormat_ = 0;
    for (int64_t want : preferred) {
        if (std::find(formats.begin(), formats.end(), want) != formats.end()) {
            colorFormat_ = want;
            break;
        }
    }
    if (!colorFormat_) {
        colorFormat_ = formats.empty() ? GL_RGBA8 : formats[0];
        LOGW("No preferred swapchain format; using 0x%llx", static_cast<long long>(colorFormat_));
    }
    LOGI("Swapchain colour format 0x%llx", static_cast<long long>(colorFormat_));

    for (int eye = 0; eye < 2; ++eye) {
        Swapchain& sc = swapchains_[eye];
        sc.width = static_cast<int>(configViews_[eye].recommendedImageRectWidth);
        sc.height = static_cast<int>(configViews_[eye].recommendedImageRectHeight);
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = colorFormat_;
        ci.sampleCount = 1;
        ci.width = static_cast<uint32_t>(sc.width);
        ci.height = static_cast<uint32_t>(sc.height);
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XR_CHECK_RET(xrCreateSwapchain(session_, &ci, &sc.handle), false);

        uint32_t count = 0;
        XR_CHECK_RET(xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr), false);
        sc.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        XR_CHECK_RET(xrEnumerateSwapchainImages(
                         sc.handle, count, &count,
                         reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())), false);

        glGenRenderbuffers(1, &sc.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, sc.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, sc.width, sc.height);
        sc.framebuffers.resize(count);
        glGenFramebuffers(static_cast<GLsizei>(count), sc.framebuffers.data());
        for (uint32_t i = 0; i < count; ++i) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sc.framebuffers[i]);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   sc.images[i].image, 0);
            glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                      sc.depth);
            GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                LOGE("Eye %d framebuffer %u incomplete: 0x%x", eye, i, status);
                return false;
            }
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        LOGI("Eye %d swapchain %dx%d, %u images", eye, sc.width, sc.height, count);
    }
    layerViews_.assign(2, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    return checkGl("createSwapchains");
}

void XrApp::applyRefreshRate() {
    if (!pfnEnumerateRefresh_ || !pfnRequestRefresh_) return;
    uint32_t n = 0;
    if (XR_FAILED(pfnEnumerateRefresh_(session_, 0, &n, nullptr)) || n == 0) return;
    std::vector<float> rates(n);
    if (XR_FAILED(pfnEnumerateRefresh_(session_, n, &n, rates.data()))) return;
    float best = 0;
    for (float r : rates) {
        LOGI("  supported refresh rate %.1f Hz", r);
        if (std::fabs(r - config_.preferredRefreshHz) < std::fabs(best - config_.preferredRefreshHz))
            best = r;
    }
    if (best > 0 && XR_SUCCEEDED(pfnRequestRefresh_(session_, best))) {
        refreshRate_ = best;
        LOGI("Requested display refresh rate %.1f Hz", best);
    }
}

void XrApp::handleSessionStateChange(const XrEventDataSessionStateChanged& e, bool& exitRequested) {
    LOGI("Session state %d -> %d", static_cast<int>(sessionState_), static_cast<int>(e.state));
    sessionState_ = e.state;
    switch (e.state) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XrResult r = xrBeginSession(session_, &bi);
            if (XR_FAILED(r)) {
                LOGE("xrBeginSession failed: %s", xrResultName(instance_, r));
                exitRequested = true;
                return;
            }
            sessionRunning_ = true;
            applyRefreshRate();
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            XR_LOG_FAIL(xrEndSession(session_));
            sessionRunning_ = false;
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            sessionRunning_ = false;
            exitRequested = true;
            break;
        default:
            break;
    }
}

void XrApp::pollEvents(bool& exitRequested) {
    if (!instance_) return;
    for (;;) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult r = xrPollEvent(instance_, &ev);
        if (r != XR_SUCCESS) break;  // XR_EVENT_UNAVAILABLE or error
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOGW("Instance loss pending");
                exitRequested = true;
                return;
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                handleSessionStateChange(
                    *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev), exitRequested);
                break;
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                input_.refreshInteractionProfiles();
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                LOGI("Reference space change pending (user recentre)");
                ++recenterSerial_;
                break;
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                auto* rr = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&ev);
                refreshRate_ = rr->toDisplayRefreshRate;
                LOGI("Display refresh rate changed %.1f -> %.1f", rr->fromDisplayRefreshRate,
                     rr->toDisplayRefreshRate);
                break;
            }
            default:
                break;
        }
    }
}

bool XrApp::beginFrame(XrFrameContext& frame) {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XR_CHECK_RET(xrWaitFrame(session_, &wi, &fs), false);
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    XR_CHECK_RET(xrBeginFrame(session_, &bi), false);

    frame.predictedDisplayTime = fs.predictedDisplayTime;
    frame.predictedDisplayPeriod = fs.predictedDisplayPeriod;
    frame.shouldRender = fs.shouldRender == XR_TRUE;

    // Head pose (for gameplay) and input.
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    frame.input.headValid = false;
    if (XR_SUCCEEDED(xrLocateSpace(viewSpace_, appSpace_, fs.predictedDisplayTime, &head)) &&
        (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        frame.input.head = toPose(head.pose);
        frame.input.headValid = true;
    }
    input_.sync(fs.predictedDisplayTime, appSpace_, frame.input);
    frame.input.recenterSerial = recenterSerial_;

    if (!frame.shouldRender) return true;

    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = appSpace_;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    XrResult r = xrLocateViews(session_, &li, &vs, static_cast<uint32_t>(views_.size()), &n,
                               views_.data());
    const XrViewStateFlags need =
        XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_FAILED(r) || n != 2 || (vs.viewStateFlags & need) != need) {
        // Tracking lost: submit an empty frame rather than rendering with garbage poses.
        frame.shouldRender = false;
        return true;
    }
    for (int eye = 0; eye < 2; ++eye) {
        EyeView& ev = frame.eyes[eye];
        ev.pose = toPose(views_[eye].pose);
        ev.fovLeft = views_[eye].fov.angleLeft;
        ev.fovRight = views_[eye].fov.angleRight;
        ev.fovUp = views_[eye].fov.angleUp;
        ev.fovDown = views_[eye].fov.angleDown;
        ev.width = swapchains_[eye].width;
        ev.height = swapchains_[eye].height;
    }
    return true;
}

XrApp::EyeBindResult XrApp::bindEyeFramebuffer(int eye) {
    Swapchain& sc = swapchains_[eye];
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult r = xrAcquireSwapchainImage(sc.handle, &ai, &index);
    if (XR_FAILED(r)) {
        LOGE("xrAcquireSwapchainImage(eye %d) failed: %s", eye, xrResultName(instance_, r));
        return EyeBindResult::Skipped;
    }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    r = xrWaitSwapchainImage(sc.handle, &wi);
    if (XR_FAILED(r)) {
        // An acquired-but-unwaited image cannot be released (spec), so the swapchain is now
        // unusable: treat as fatal.
        LOGE("xrWaitSwapchainImage(eye %d) failed: %s", eye, xrResultName(instance_, r));
        return EyeBindResult::Fatal;
    }
    sc.acquired = true;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sc.framebuffers[index]);
    return EyeBindResult::Ok;
}

void XrApp::releaseEye(int eye) {
    Swapchain& sc = swapchains_[eye];
    if (!sc.acquired) return;
    // Depth is not needed after rendering; tell the tiler not to resolve it to memory.
    const GLenum discard[] = {GL_DEPTH_ATTACHMENT};
    glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, discard);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_LOG_FAIL(xrReleaseSwapchainImage(sc.handle, &ri));
    sc.acquired = false;
}

bool XrApp::endFrame(const XrFrameContext& frame) {
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {};
    uint32_t layerCount = 0;
    if (frame.shouldRender) {
        for (int eye = 0; eye < 2; ++eye) {
            XrCompositionLayerProjectionView& pv = layerViews_[eye];
            pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pv.pose = views_[eye].pose;
            pv.fov = views_[eye].fov;
            pv.subImage.swapchain = swapchains_[eye].handle;
            pv.subImage.imageRect.offset = {0, 0};
            pv.subImage.imageRect.extent = {swapchains_[eye].width, swapchains_[eye].height};
            pv.subImage.imageArrayIndex = 0;
        }
        layer.space = appSpace_;
        layer.viewCount = 2;
        layer.views = layerViews_.data();
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        layerCount = 1;
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = frame.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    XrResult r = xrEndFrame(session_, &ei);
    if (XR_FAILED(r)) {
        LOGW("xrEndFrame failed: %s", xrResultName(instance_, r));
        return !(r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST);
    }
    return true;
}

void XrApp::destroy() {
    input_.destroy();
    for (Swapchain& sc : swapchains_) {
        if (!sc.framebuffers.empty())
            glDeleteFramebuffers(static_cast<GLsizei>(sc.framebuffers.size()), sc.framebuffers.data());
        if (sc.depth) glDeleteRenderbuffers(1, &sc.depth);
        if (sc.handle) xrDestroySwapchain(sc.handle);
        sc = Swapchain{};
    }
    if (viewSpace_) xrDestroySpace(viewSpace_);
    if (appSpace_) xrDestroySpace(appSpace_);
    viewSpace_ = appSpace_ = XR_NULL_HANDLE;
    if (session_) xrDestroySession(session_);
    session_ = XR_NULL_HANDLE;
    if (instance_) xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
    LOGI("OpenXR shut down");
}

}  // namespace gvr
