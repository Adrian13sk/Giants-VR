// Mesa surfaceless EGL + GLES 3 context for the host tools.
#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "core/log.h"
#include "render/gl.h"

namespace gvr {
inline bool initEgl() {
    auto getPlatformDisplay =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (getPlatformDisplay)
        dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) {
        LOGE("eglInitialize failed 0x%x", eglGetError());
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfgAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, 0, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &n) || n < 1) {
        LOGW("No config with SURFACE_TYPE 0; trying any");
        const EGLint any[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE};
        if (!eglChooseConfig(dpy, any, &cfg, 1, &n) || n < 1) {
            LOGE("eglChooseConfig failed");
            return false;
        }
    }
    const EGLint ctxAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        LOGE("context creation failed 0x%x", eglGetError());
        return false;
    }
    LOGI("EGL %d.%d | GL: %s | %s | %s", maj, min, glGetString(GL_VENDOR), glGetString(GL_RENDERER),
         glGetString(GL_VERSION));
    return true;
}

}  // namespace gvr
