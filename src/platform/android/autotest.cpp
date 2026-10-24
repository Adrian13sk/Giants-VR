#include "platform/android/autotest.h"

#include <EGL/egl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "assets/image.h"
#include "core/log.h"
#include "game/game_app.h"
#include "scene/model_cache.h"
#include "xr/xr_app.h"

namespace gvr {

namespace {
// GL_EXT_disjoint_timer_query (not in the GLES 3 headers).
constexpr GLenum kTimeElapsed = 0x88BF, kQueryResult = 0x8866, kQueryResultAvailable = 0x8867, kGpuDisjoint = 0x8FBB;
using PfnGenQueries = void (*)(GLsizei, GLuint*);
using PfnBeginQuery = void (*)(GLenum, GLuint);
using PfnEndQuery = void (*)(GLenum);
using PfnGetQueryObjectuiv = void (*)(GLuint, GLenum, GLuint*);
using PfnGetQueryObjectui64v = void (*)(GLuint, GLenum, uint64_t*);
PfnGenQueries genQueries = nullptr;
PfnBeginQuery beginQuery = nullptr;
PfnEndQuery endQuery = nullptr;
PfnGetQueryObjectuiv getQueryuiv = nullptr;
PfnGetQueryObjectui64v getQueryui64v = nullptr;

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

float percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * float(v.size() - 1) + 0.5f))];
}

long procStatusKb(const char* key) {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind(key, 0) == 0) return std::atol(line.c_str() + std::strlen(key));
    return -1;
}
}  // namespace

bool AutoTest::load(const std::string& filesDir) {
    const std::string path = filesDir + "/autotest.txt";
    const std::string text = readFile(path);
    if (text.empty()) return false;
    std::remove(path.c_str());  // consumed: a crash doesn't loop
    out_ = filesDir + "/autotest_out";
    mkdir(out_.c_str(), 0775);
    std::remove((out_ + "/done").c_str());
    std::remove((out_ + "/result.txt").c_str());
    std::remove((out_ + "/frames.csv").c_str());
    if (!script_.parse(text, error_)) {
        LOGE("autotest: bad script: %s", error_.c_str());
        // Report it and stop at once.
        std::ofstream r(out_ + "/result.txt");
        r << "status script_error\nerror " << error_ << "\n";
        std::ofstream(out_ + "/done") << "script_error\n";
        finished_ = true;
        active_ = true;
        return true;
    }
    active_ = true;
    LOGI("autotest: running '%s' (%s / %s, %.0f s)", script_.name.c_str(), script_.world.c_str(),
         script_.mission.empty() ? "first mission" : script_.mission.c_str(), script_.duration);
    return true;
}

void AutoTest::applyEnvironment() const {
    if (!script_.mission.empty()) setenv("GVR_MISSION", script_.mission.c_str(), 1);
    setenv("GVR_NO_BRIEF", "1", 1);
    for (const auto& e : script_.env) setenv(e.first.c_str(), e.second.c_str(), 1);
}

void AutoTest::initTimers() {
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!ext || !std::strstr(ext, "GL_EXT_disjoint_timer_query")) {
        LOGW("autotest: no GL_EXT_disjoint_timer_query: no GPU times");
        return;
    }
    genQueries = reinterpret_cast<PfnGenQueries>(eglGetProcAddress("glGenQueriesEXT"));
    beginQuery = reinterpret_cast<PfnBeginQuery>(eglGetProcAddress("glBeginQueryEXT"));
    endQuery = reinterpret_cast<PfnEndQuery>(eglGetProcAddress("glEndQueryEXT"));
    getQueryuiv = reinterpret_cast<PfnGetQueryObjectuiv>(eglGetProcAddress("glGetQueryObjectuivEXT"));
    getQueryui64v = reinterpret_cast<PfnGetQueryObjectui64v>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
    if (!genQueries || !beginQuery || !endQuery || !getQueryuiv || !getQueryui64v) return;
    genQueries(kQueries, queries_);
    timers_ = true;
}

void AutoTest::beginFrame(GameApp& game, XrApp& xr, XrFrameContext& frame, double now) {
    game_ = &game;
    if (!started_) {
        // First frame: place the player as the script says (as the harness does).
        started_ = true;
        t0_ = now;
        lastNow_ = now;
        initTimers();
        InputFrame in;
        script_.inputAt(0.0f, in);
        game.update(0.0f, in);  // consume the automatic spawn
        game.setFirstPerson(script_.firstPerson);
        if (script_.briefing) game.debugReloadWithBriefing(in);
        else if (script_.hasAt) {
            const float yaw = script_.startYaw();
            if (game.playMode()) game.teleportPlayer(giantsToRuntime(script_.atX, script_.atY, 0.0f), yaw);
            game.update(0.0f, in);
        }
    }
    prevT_ = t_;
    t_ = float(now - t0_);
    script_.inputAt(t_, frame.input);
    const Pose eyes[2] = {script_.eyePose(0), script_.eyePose(1)};
    xr.overrideEyePoses(frame, eyes);
    for (const auto* b : script_.blastsIn(prevT_, t_))
        for (int k = 0; k < b->n; ++k) game.debugBlast(b->x, b->y, b->h);
    for (const auto* s : script_.shotsIn(prevT_, t_)) pendingShots_.push_back(s->label);
}

void AutoTest::renderBegin() {
    if (!timers_) return;
    pollTimers();
    if (qBusy_[qNext_]) return;  // all in flight: skip this frame's GPU time
    beginQuery(kTimeElapsed, queries_[qNext_]);
}

void AutoTest::renderEnd(double cpuMs, unsigned drawCalls) {
    if (timers_ && !qBusy_[qNext_]) {
        endQuery(kTimeElapsed);
        qBusy_[qNext_] = true;
        qFrame_[qNext_] = frameNo_;
        qNext_ = (qNext_ + 1) % kQueries;
    }
    Sample s;
    s.t = t_;
    s.dtMs = float((t_ - prevT_) * 1000.0);
    s.updMs = float(updMs_);
    s.cpuMs = float(cpuMs);
    s.gpuMs = -1;
    s.draws = drawCalls;
    s.skip = skip_ > 0;
    if (skip_ > 0) --skip_;
    samples_.push_back(s);
    gpuByFrame_.push_back(-1);
    ++frameNo_;
}

void AutoTest::pollTimers() {
    GLint disjoint = 0;
    glGetIntegerv(kGpuDisjoint, &disjoint);
    for (int q = 0; q < kQueries; ++q) {
        if (!qBusy_[q]) continue;
        GLuint avail = 0;
        getQueryuiv(queries_[q], kQueryResultAvailable, &avail);
        if (!avail) continue;
        uint64_t ns = 0;
        getQueryui64v(queries_[q], kQueryResult, &ns);
        qBusy_[q] = false;
        // (Adreno returned nonsense for some queries: keep only plausible times.)
        if (!disjoint && ns < 200000000ull && size_t(qFrame_[q]) < gpuByFrame_.size()) gpuByFrame_[size_t(qFrame_[q])] = float(ns * 1e-6);
    }
}

void AutoTest::afterEye(int eye, int width, int height) {
    if (eye != 0 || pendingShots_.empty() || width <= 0 || height <= 0) return;
    // The eye image as rendered (sRGB-encoded values), halved for size.
    std::vector<uint8_t> px(size_t(width) * size_t(height) * 4);
    GLint drawFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(drawFbo));
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const int w2 = width / 2, h2 = height / 2;
    std::vector<uint8_t> half(size_t(w2) * size_t(h2) * 4);
    for (int y = 0; y < h2; ++y)
        for (int x = 0; x < w2; ++x)
            for (int c = 0; c < 4; ++c) {
                const size_t i0 = (size_t(2 * y) * width + 2 * x) * 4 + c, i1 = i0 + 4, i2 = i0 + size_t(width) * 4, i3 = i2 + 4;
                half[(size_t(y) * w2 + x) * 4 + c] = uint8_t((px[i0] + px[i1] + px[i2] + px[i3] + 2) / 4);
            }
    for (size_t i = 3; i < half.size(); i += 4) half[i] = 255;
    for (const auto& label : pendingShots_) {
        const std::string path = out_ + "/" + script_.name + "_" + label + ".png";
        if (writePng(path, w2, h2, half.data(), true)) LOGI("autotest: screenshot %s", path.c_str());
        else LOGE("autotest: screenshot %s failed", path.c_str());
    }
    pendingShots_.clear();
    skip_ = 5;
}

void AutoTest::endFrame(double now) {
    lastNow_ = now;
    if (!finished_ && started_ && t_ >= script_.duration) {
        // Let the last GPU queries land.
        if (timers_) {
            glFinish();
            pollTimers();
        }
        writeResults();
        finished_ = true;
    }
}

void AutoTest::writeResults() {
    for (size_t i = 0; i < samples_.size() && i < gpuByFrame_.size(); ++i) samples_[i].gpuMs = gpuByFrame_[i];
    std::vector<float> dt, upd, cpu, gpu, draws;
    int over = 0;
    for (const auto& s : samples_) {
        if (s.skip || s.t < script_.perfFrom || s.t > script_.perfEnd()) continue;
        dt.push_back(s.dtMs);
        upd.push_back(s.updMs);
        cpu.push_back(s.cpuMs);
        if (s.gpuMs >= 0) gpu.push_back(s.gpuMs);
        draws.push_back(float(s.draws));
        if (s.dtMs > 1.5f * 1000.0f / 72.0f) ++over;
    }
    auto mean = [](const std::vector<float>& v) {
        double a = 0;
        for (float x : v) a += x;
        return v.empty() ? 0.0 : a / double(v.size());
    };
    std::ofstream r(out_ + "/result.txt");
    char buf[512];
    r << "status ok\nname " << script_.name << "\nworld " << script_.world << "\nmission " << script_.mission << "\n";
    std::snprintf(buf, sizeof(buf), "window %.1f-%.1f s, %zu frames (screenshot stalls left out)\n", script_.perfFrom, script_.perfEnd(), dt.size());
    r << buf;
    const double meanDt = mean(dt);
    std::snprintf(buf, sizeof(buf), "fps %.1f\nframe_ms mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f\nslow_frames %d (> 1.5 x 13.9 ms)\n",
                  meanDt > 0 ? 1000.0 / meanDt : 0.0, meanDt, percentile(dt, 0.5f), percentile(dt, 0.95f), percentile(dt, 0.99f),
                  percentile(dt, 1.0f), over);
    r << buf;
    std::snprintf(buf, sizeof(buf), "update_ms mean %.2f p95 %.2f max %.2f\nrender_cpu_ms mean %.2f p95 %.2f max %.2f\n", mean(upd),
                  percentile(upd, 0.95f), percentile(upd, 1.0f), mean(cpu), percentile(cpu, 0.95f), percentile(cpu, 1.0f));
    r << buf;
    if (!gpu.empty())
        std::snprintf(buf, sizeof(buf), "gpu_ms mean %.2f p95 %.2f max %.2f (%zu frames timed)\n", mean(gpu), percentile(gpu, 0.95f),
                      percentile(gpu, 1.0f), gpu.size());
    else
        std::snprintf(buf, sizeof(buf), "gpu_ms n/a\n");
    r << buf;
    if (game_)
        r << "mission_complete " << (game_->missionComplete() ? 1 : 0) << "\nplayer_health " << int(game_->playerHealth())
          << "\nreapers_alive " << game_->enemiesAlive() << "\n";
    std::snprintf(buf, sizeof(buf), "draws mean %.0f max %.0f\nmemory_kb rss %ld peak %ld\n", mean(draws), percentile(draws, 1.0f),
                  procStatusKb("VmRSS:"), procStatusKb("VmHWM:"));
    r << buf;
    r.close();
    std::ofstream csv(out_ + "/frames.csv");
    csv << "t,dt_ms,update_ms,render_cpu_ms,gpu_ms,draws,screenshot_stall\n";
    for (const auto& s : samples_) {
        std::snprintf(buf, sizeof(buf), "%.3f,%.2f,%.2f,%.2f,%.2f,%u,%d\n", s.t, s.dtMs, s.updMs, s.cpuMs, s.gpuMs, s.draws, int(s.skip));
        csv << buf;
    }
    csv.close();
    std::ofstream(out_ + "/done") << "ok\n";
    LOGI("autotest: '%s' finished: %.1f fps, see %s", script_.name.c_str(), meanDt > 0 ? 1000.0 / meanDt : 0.0, out_.c_str());
}

}  // namespace gvr
