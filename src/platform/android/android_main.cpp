// Quest entry point: NativeActivity + android_native_app_glue, EGL context, OpenXR frame loop.
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "core/log.h"
#include "audio/mixer.h"
#include "game/game_app.h"
#include "platform/android/audio_aaudio.h"
#include "platform/android/autotest.h"
#include "platform/android/crash_handler.h"
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
        const EGLint cfgAttrsWin[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                      EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
                                      EGL_SAMPLES, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                      EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_NONE};
        if ((!eglChooseConfig(display, cfgAttrsWin, &config, 1, &n) || n < 1) &&
            (!eglChooseConfig(display, cfgAttrs, &config, 1, &n) || n < 1)) {
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

// Wi-Fi drops broadcast packets to save power unless an app holds a multicast lock: without it the
// LAN games' beacons (game/sk_net.h) don't arrive. Held for the app's lifetime (released on exit).
void acquireMulticastLock(JNIEnv* env, jobject activity) {
    if (!env || !activity) return;
    auto clear = [env]() { if (env->ExceptionCheck()) { env->ExceptionClear(); return true; } return false; };
    jclass ctx = env->FindClass("android/content/Context");
    if (clear() || !ctx) return;
    jmethodID getSys = env->GetMethodID(ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring wifiName = env->NewStringUTF("wifi");
    jobject wifi = getSys ? env->CallObjectMethod(activity, getSys, wifiName) : nullptr;
    if (clear() || !wifi) { LOGW("Multicast lock: no Wi-Fi service"); return; }
    jclass wm = env->GetObjectClass(wifi);
    jmethodID create = env->GetMethodID(wm, "createMulticastLock", "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
    jstring tag = env->NewStringUTF("giantsvr");
    jobject lock = create ? env->CallObjectMethod(wifi, create, tag) : nullptr;
    if (clear() || !lock) { LOGW("Multicast lock: unavailable"); return; }
    jmethodID acquire = env->GetMethodID(env->GetObjectClass(lock), "acquire", "()V");
    if (acquire) env->CallVoidMethod(lock, acquire);
    if (clear()) { LOGW("Multicast lock: not granted"); return; }
    env->NewGlobalRef(lock);  // kept: the lock is held while the app runs
    LOGI("Multicast lock held (LAN games can be found)");
}

std::string dataDirectory(android_app* app) {
    // App-specific external storage needs no runtime permission:
    // /sdcard/Android/data/<package>/files/Bin  <- user copies the game's Bin folder here.
    std::string base = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    return base + "/Bin";
}

}  // namespace

// (2026-10-28) RECORD_AUDIO for Kabuto's mic roar: checked, and asked for once when the option is turned on.
bool hasPermission(JNIEnv* env, jobject activity, const char* perm) {
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    jstring s = env->NewStringUTF(perm);
    const jint r = m ? env->CallIntMethod(activity, m, s) : -1;
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(s);
    env->DeleteLocalRef(cls);
    return r == 0;
}

void requestPermission(JNIEnv* env, jobject activity, const char* perm) {
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, "requestPermissions", "([Ljava/lang/String;I)V");
    jclass strCls = env->FindClass("java/lang/String");
    jstring s = env->NewStringUTF(perm);
    jobjectArray arr = env->NewObjectArray(1, strCls, s);
    if (m) env->CallVoidMethod(activity, m, arr, 7);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(s);
    env->DeleteLocalRef(strCls);
    env->DeleteLocalRef(cls);
    LOGI("Microphone: asked for the RECORD_AUDIO permission");
}

void android_main(android_app* app) {
    LOGI("GiantsVR starting (build %s %s)", __DATE__, __TIME__);
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    acquireMulticastLock(env, app->activity->clazz);

    AppState state;
    app->userData = &state;
    app->onAppCmd = onAppCmd;

    EglState egl;
    gvr::XrApp xr;
    gvr::GameApp game;
    // A scripted test run (scripts/devtest.ps1 pushes files/autotest.txt): no save, no menu. Read
    // first: its settings (GVR_MULTIVIEW, ...) decide how the swapchains are made.
    gvr::AutoTest autotest;
    const std::string filesDir = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    if (!filesDir.empty()) gvr::installCrashHandler((filesDir + "/crash").c_str());  // (2026-10-28) files/crash/crash_<time>.txt
    const bool scripted = !filesDir.empty() && autotest.load(filesDir);
    if (scripted) autotest.applyEnvironment();
    bool ok = egl.init();
    ok = ok && xr.initLoader(app->activity->vm, app->activity->clazz);
    gvr::XrApp::Config cfg;
    ok = ok && xr.createInstance(app->activity->vm, app->activity->clazz, cfg);
    ok = ok && xr.createSession(egl.display, egl.config, egl.context);
    const std::string dataDir = dataDirectory(app);
    LOGI("Original data directory: %s", dataDir.c_str());
    if (scripted) {
        xr.setTestMode(true);
        if (autotest.finished()) ok = false;  // a bad script: reported, exit
        if (autotest.script().title) game.setMainMenu(true);  // (a menu test: from the main menu)
        ok = ok && game.init(dataDir, autotest.world());
    } else {
        // Progress checkpoint next to the Bin folder (app storage, no permission needed).
        if (!filesDir.empty()) game.setSavePath(filesDir + "/save.txt");
        game.setMainMenu(true);  // start at the main menu (title scene)
        ok = ok && game.init(dataDir);
    }
    if (!ok) LOGE("Initialisation failed; see log above. Exiting.");
    // Fixed foveated rendering: the Options setting (default medium); GVR_FFR=0..3 pins it in test runs.
    if (ok)
        if (const char* f = std::getenv("GVR_FFR")) game.setFoveationOverride(std::max(0, std::min(3, std::atoi(f))));
    int foveationApplied = -1;
    bool foveationDynamic = false;

    // Audio: AAudio stream -> software mixer with the original sound effects.
    gvr::AAudioOutput audioOut;
    std::unique_ptr<gvr::AudioMixer> mixer;
    if (ok) {
        const int32_t rate = audioOut.open();
        if (rate > 0) {
            mixer = std::make_unique<gvr::AudioMixer>(uint32_t(rate));
            mixer->setMasterVolume(0.85f);
            audioOut.attach(mixer.get());
            game.setAudio(mixer.get());
        }
    }
    bool audioRunning = false;
    gvr::AAudioInput micIn;  // (Kabuto's mic roar; opened on demand)

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

        // Audio follows the activity lifecycle (silent while paused / in the system menu).
        audioOut.poll();
        if (state.resumed && !audioRunning && audioOut.isOpen()) audioRunning = audioOut.start();
        if (!state.resumed && audioRunning) { audioOut.pause(); audioRunning = false; }
        if ((!state.resumed || !xr.sessionRunning()) && micIn.isOpen()) micIn.close();  // (paused / headset off: no mic)

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
        for (auto& h : frame.input.hand)
            if (h.handTracked && !game.wantsHandPointers()) {
                const bool m = h.menu;
                h = gvr::ControllerState{};
                h.menu = m;
            }
        if (game.foveationWanted() != foveationApplied || game.foveationDynamicWanted() != foveationDynamic) {  // changed in the menu (or first frame)
            foveationApplied = game.foveationWanted();
            foveationDynamic = game.foveationDynamicWanted();
            xr.setFoveation(foveationApplied, foveationDynamic);
            LOGI("Foveation: level %d%s", foveationApplied, foveationDynamic ? ", dynamic" : "");
        }
        if (autotest.active()) autotest.beginFrame(game, xr, frame, now);
        const double u0 = nowSeconds();
        // Kabuto's mic roar: the microphone open only while the option wants it (and never in a scripted test).
        if (!scripted) {
            static bool asked = false, granted = false;
            static double lastCheck = 0;
            if (game.micWanted()) {
                if (!granted && now - lastCheck > 1.0) {
                    lastCheck = now;
                    granted = hasPermission(env, app->activity->clazz, "android.permission.RECORD_AUDIO");
                    if (!granted && !asked) { asked = true; requestPermission(env, app->activity->clazz, "android.permission.RECORD_AUDIO"); }
                }
                // (a failed open, or a stream the device dropped, is retried every 3 s, not every frame)
                static double lastOpen = -10;
                if (micIn.failed()) micIn.close();
                if (granted && !micIn.isOpen() && now - lastOpen > 3.0) { lastOpen = now; micIn.open(); }
            } else if (micIn.isOpen()) {
                micIn.close();
            }
            game.setMicLevel(micIn.level(), granted);
        }
        game.update(dt, frame.input);
        if (autotest.active()) autotest.updateDone((nowSeconds() - u0) * 1000.0);
        for (const auto& hr : game.hapticRequests()) xr.input().vibrate(hr.hand, hr.amplitude, hr.seconds);
        game.hapticRequests().clear();

        bool eyesOk = true;
        const double r0 = nowSeconds();
        if (autotest.active() && frame.shouldRender) autotest.renderBegin();
        if (frame.shouldRender) {
            game.renderer().resetStats();
            game.renderer().gpuTimer().beginFrame();
            {
                const gvr::EyeView& l = frame.eyes[0];
                const gvr::EyeView& r = frame.eyes[1];
                game.renderShadows(gvr::EyeRenderInfo{l.pose, l.fovLeft, l.fovRight, l.fovUp, l.fovDown, l.width, l.height},
                                   gvr::EyeRenderInfo{r.pose, r.fovLeft, r.fovRight, r.fovUp, r.fovDown, r.width, r.height});
            }
            game.renderer().setMsaa(xr.multiview() && game.renderer().multiviewSupported() && xr.msaaSamples() > 1);
            if (xr.multiview() && game.renderer().multiviewSupported()) {
                // Both eyes in one pass (GL_OVR_multiview2).
                const auto bind = xr.bindStereoFramebuffer();
                if (bind != gvr::XrApp::EyeBindResult::Ok) {
                    eyesOk = false;
                    if (bind == gvr::XrApp::EyeBindResult::Fatal) exitRequested = true;
                } else {
                    const gvr::EyeView& l = frame.eyes[0];
                    const gvr::EyeView& r = frame.eyes[1];
                    game.renderStereo(gvr::EyeRenderInfo{l.pose, l.fovLeft, l.fovRight, l.fovUp, l.fovDown, l.width, l.height},
                                      gvr::EyeRenderInfo{r.pose, r.fovLeft, r.fovRight, r.fovUp, r.fovDown, r.width, r.height});
                    if (autotest.active()) autotest.afterEye(0, l.width, l.height, xr.eyeReadFramebuffer(0));
                    xr.releaseStereo();
                }
            } else {
                for (int eye = 0; eye < 2; ++eye) {
                    const auto bind = xr.bindEyeFramebuffer(eye);
                    if (bind != gvr::XrApp::EyeBindResult::Ok) {
                        eyesOk = false;
                        if (bind == gvr::XrApp::EyeBindResult::Fatal) exitRequested = true;
                        break;
                    }
                    const gvr::EyeView& ev = frame.eyes[eye];
                    gvr::EyeRenderInfo info{ev.pose, ev.fovLeft, ev.fovRight, ev.fovUp, ev.fovDown,
                                            ev.width, ev.height};
                    game.renderEye(info);
                    if (autotest.active()) autotest.afterEye(eye, ev.width, ev.height, xr.eyeReadFramebuffer(eye));
                    xr.releaseEye(eye);
                }
            }
        }
        // Never submit a projection layer referencing a swapchain without a released image.
        if (!eyesOk) {
            for (int eye = 0; eye < 2; ++eye) xr.releaseEye(eye);
            frame.shouldRender = false;
        }
        if (frame.shouldRender) game.renderer().gpuTimer().endFrame();
        if (autotest.active() && frame.shouldRender) autotest.renderEnd((nowSeconds() - r0) * 1000.0, game.renderer().drawCallsThisFrame());
        if (!xr.endFrame(frame)) exitRequested = true;
        if (autotest.active()) {
            autotest.endFrame(now);
            if (autotest.finished()) exitRequested = true;  // results written: done
        }

        ++frameCount;
        if (now - statsStart >= 5.0) {
            LOGI("perf: %.1f fps (app loop), %u draws last frame, refresh %.0f Hz",
                 frameCount / (now - statsStart), game.renderer().drawCallsThisFrame(),
                 xr.currentRefreshRate());
            frameCount = 0;
            statsStart = now;
        }
    }

    micIn.close();     // (its callback writes into micIn: closed before it goes - review 2026-10-28)
    audioOut.close();  // stop callbacks before the mixer and game go away
    game.setAudio(nullptr);
    mixer.reset();
    game.shutdown();
    xr.destroy();
    egl.destroy();
    app->activity->vm->DetachCurrentThread();
    LOGI("GiantsVR exited cleanly");
}
