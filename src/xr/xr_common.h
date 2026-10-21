// OpenXR include configuration for the Android GLES build.
#pragma once

#if defined(__ANDROID__)
#include <jni.h>
#include <EGL/egl.h>
#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "core/log.h"

namespace gvr {

const char* xrResultName(XrInstance instance, XrResult r);

// Evaluate an OpenXR call; log and return `ret` from the enclosing function on failure.
#define XR_CHECK_RET(call, ret)                                                         \
    do {                                                                                \
        XrResult _r = (call);                                                           \
        if (XR_FAILED(_r)) {                                                            \
            LOGE("%s failed: %s (%d) [%s:%d]", #call, ::gvr::xrResultName(instance_, _r), \
                 static_cast<int>(_r), __FILE__, __LINE__);                             \
            return ret;                                                                 \
        }                                                                               \
    } while (0)

#define XR_LOG_FAIL(call)                                                               \
    do {                                                                                \
        XrResult _r = (call);                                                           \
        if (XR_FAILED(_r))                                                              \
            LOGW("%s failed: %s (%d)", #call, ::gvr::xrResultName(instance_, _r),         \
                 static_cast<int>(_r));                                                 \
    } while (0)

}  // namespace gvr
