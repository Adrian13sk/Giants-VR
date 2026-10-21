// Quest entry point: NativeActivity + android_native_app_glue, EGL context, OpenXR frame loop.
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "core/log.h"
#include "game/game_app.h"
#include "render/gl.h"
#include "xr/xr_app.h"

namespace {

struct EglState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;  // tiny pbuffer; we never render to it

    bool init() {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
            LOGE("eglInitialize failed: 0x%x", eglGetError());
            return false;
        }
        const EGLint cfgAttrs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                   EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
                                   EGL_SAMPLES, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                   EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
        EGLint n = 0;
        if (!eglChooseConfig(display, cfgAttrs, &config, 1, &n) || n < 1) {
            LOGE("eglChooseConfig failed: 0x%x", eglGetError());
            return false;
        }
        const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttrs);
        if (context == EGL_NO_CONTEXT) {
            LOGE("eglCreateContext failed: 0x%x", eglGetError());
            return false;
        }
        const EGLint pbAttrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, pbAttrs);
        if (surface == EGL_NO_SURFACE) {
            LOGE("eglCreatePbufferSurface failed: 0x%x", eglGetError());
            return false;
        }
        if (!eglMakeCurrent(display, surface, surface, context)) {
            LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
            return false;
        }
        LOGI("GL: %s | %s | %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
             glGetString(GL_VERSION));
        return true;
    }
    void destroy() {
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
            if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
            eglTerminate(display);
        }
        display = EGL_NO_DISPLAY;
    }
};

struct AppState {
    bool resumed = false;
    bool hasWindow = false;
};

void onAppCmd(android_app* app, int32_t cmd) {
    auto* st = static_cast<AppState*>(app->userData);
    switch (cmd) {
        case APP_CMD_RESUME: st->resumed = true; LOGI("APP_CMD_RESUME"); break;
        case APP_CMD_PAUSE: st->resumed = false; LOGI("APP_CMD_PAUSE"); break;
        case APP_CMD_INIT_WINDOW: st->hasWindow = true; LOGI("APP_CMD_INIT_WINDOW"); break;
        case APP_CMD_TERM_WINDOW: st->hasWindow = false; LOGI("APP_CMD_TERM_WINDOW"); break;
        case APP_CMD_DESTROY: LOGI("APP_CMD_DESTROY"); break;
        default: break;
    }
}

double nowSeconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

std::string dataDirectory(android_app* app) {
    // App-specific external storage needs no runtime permission:
    // /sdcard/Android/data/<package>/files/Bin  <- user copies the game's Bin folder here.
    std::string base = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    return base + "/Bin";
}

}  // namespace

void android_main(android_app* app) {
    LOGI("GiantsVR starting (build %s %s)", __DATE__, __TIME__);
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    AppState state;
    app->userData = &state;
    app->onAppCmd = onAppCmd;

    EglState egl;
    gvr::XrApp xr;
    gvr::GameApp game;
    bool ok = egl.init();
    ok = ok && xr.initLoader(app->activity->vm, app->activity->clazz);
    gvr::XrApp::Config cfg;
    ok = ok && xr.createInstance(app->activity->vm, app->activity->clazz, cfg);
    ok = ok && xr.createSession(egl.display, egl.config, egl.context);
    const std::string dataDir = dataDirectory(app);
    LOGI("Original data directory: %s", dataDir.c_str());
    ok = ok && game.init(dataDir);
    if (!ok) LOGE("Initialisation failed; see log above. Exiting.");

    bool exitRequested = !ok;
    bool finishRequested = false;
    double last = nowSeconds();
    uint64_t frameCount = 0;
    double statsStart = last;
    while (!app->destroyRequested) {
        // Block on Android events only when the XR session is idle and the app is paused.
        for (;;) {
            int events = 0;
            android_poll_source* source = nullptr;
            const int timeout = (!state.resumed && !xr.sessionRunning() && !exitRequested) ? -1 : 0;
            int id = ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source));
            if (id < 0) break;
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
        }
        if (app->destroyRequested) break;
        if (exitRequested) {
            if (!finishRequested) {
                ANativeActivity_finish(app->activity);
                finishRequested = true;
            }
            // Keep pumping events until the system destroys us.
            usleep(10000);
            continue;
        }

        xr.pollEvents(exitRequested);
        if (!xr.sessionRunning()) {
            usleep(10000);
            continue;
        }

        gvr::XrFrameContext frame;
        if (!xr.beginFrame(frame)) {
            exitRequested = true;
            continue;
        }
        const double now = nowSeconds();
        float dt = static_cast<float>(now - last);
        last = now;
        if (dt > 0.1f) dt = 0.1f;  // clamp after suspension
        if (!xr.focused()) {
            // Not focused (system menu / guardian): keep rendering, ignore gameplay input.
            for (auto& h : frame.input.hand) { h.thumbstick = {}; h.trigger = h.squeeze = 0; }
        }
        game.update(dt, frame.input);
        for (const auto& hr : game.hapticRequests()) xr.input().vibrate(hr.hand, hr.amplitude, hr.seconds);
        game.hapticRequests().clear();

        if (frame.shouldRender) {
            game.renderer().resetStats();
            for (int eye = 0; eye < 2; ++eye) {
                if (!xr.bindEyeFramebuffer(eye)) continue;
                const gvr::EyeView& ev = frame.eyes[eye];
                gvr::EyeRenderInfo info{ev.pose, ev.fovLeft, ev.fovRight, ev.fovUp, ev.fovDown,
                                        ev.width, ev.height};
                game.renderEye(info);
                xr.releaseEye(eye);
            }
        }
        xr.endFrame(frame);

        ++frameCount;
        if (now - statsStart >= 5.0) {
            LOGI("perf: %.1f fps (app loop), %u draws last frame, refresh %.0f Hz",
                 frameCount / (now - statsStart), game.renderer().drawCallsThisFrame(),
                 xr.currentRefreshRate());
            frameCount = 0;
            statsStart = now;
        }
    }

    game.shutdown();
    xr.destroy();
    egl.destroy();
    app->activity->vm->DetachCurrentThread();
    LOGI("GiantsVR exited cleanly");
}
