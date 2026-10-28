#include "render/gpu_timer.h"

#include <EGL/egl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "render/gl.h"

namespace gvr {
namespace {
// GL_EXT_disjoint_timer_query (not in the GLES 3 headers).
constexpr GLenum kTimestamp = 0x8E28, kCounterBits = 0x8864, kQueryResult = 0x8866, kQueryResultAvailable = 0x8867,
                 kGpuDisjoint = 0x8FBB;
using PfnGenQueries = void (*)(GLsizei, GLuint*);
using PfnQueryCounter = void (*)(GLuint, GLenum);
using PfnGetQueryiv = void (*)(GLenum, GLenum, GLint*);
using PfnGetQueryObjectuiv = void (*)(GLuint, GLenum, GLuint*);
using PfnGetQueryObjectui64v = void (*)(GLuint, GLenum, uint64_t*);
PfnGenQueries genQueries = nullptr;
PfnQueryCounter queryCounter = nullptr;
PfnGetQueryObjectuiv getQueryuiv = nullptr;
PfnGetQueryObjectui64v getQueryui64v = nullptr;

const char* kNames[] = {"shadow", "haze", "reflection", "scope", "screen", "frame", "eyes"};
constexpr int kFrameIdx = int(GpuPass::Count), kEyesIdx = int(GpuPass::Count) + 1;

double wallSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

bool GpuPassTimer::init() {
    enabled_ = false;
    const char* want = std::getenv("GVR_GPU_PASSES");
    if (!want || want[0] == '0') return false;
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!ext || !std::strstr(ext, "GL_EXT_disjoint_timer_query")) {
        LOGW("GPU pass timer: no GL_EXT_disjoint_timer_query");
        return false;
    }
    genQueries = reinterpret_cast<PfnGenQueries>(eglGetProcAddress("glGenQueriesEXT"));
    queryCounter = reinterpret_cast<PfnQueryCounter>(eglGetProcAddress("glQueryCounterEXT"));
    auto getQueryiv = reinterpret_cast<PfnGetQueryiv>(eglGetProcAddress("glGetQueryivEXT"));
    getQueryuiv = reinterpret_cast<PfnGetQueryObjectuiv>(eglGetProcAddress("glGetQueryObjectuivEXT"));
    getQueryui64v = reinterpret_cast<PfnGetQueryObjectui64v>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
    if (!genQueries || !queryCounter || !getQueryiv || !getQueryuiv || !getQueryui64v) {
        LOGW("GPU pass timer: timer query functions missing");
        return false;
    }
    GLint bits = 0;
    getQueryiv(kTimestamp, kCounterBits, &bits);
    while (glGetError() != GL_NO_ERROR) {}
    if (bits <= 0) {
        LOGW("GPU pass timer: the GPU has no timestamp counter (%d bits)", bits);
        return false;
    }
    for (auto& s : queries_) genQueries(kMarks, s);
    enabled_ = true;
    lastLog_ = wallSeconds();
    LOGI("GPU pass timer: on (%d-bit timestamps)", bits);
    return true;
}

void GpuPassTimer::mark(int id) {
    if (!enabled_ || !inFrame_) return;
    if (count_[slot_] >= (id == kMarks - 1 ? kMarks : kMarks - 1)) return;  // (the last query is kept for the frame's end)
    const int n = count_[slot_]++;
    ids_[slot_][n] = id;
    queryCounter(queries_[slot_][n], kTimestamp);
}

void GpuPassTimer::beginFrame() {
    if (!enabled_) return;
    poll();
    inFrame_ = false;
    for (int k = 0; k < kSlots; ++k) {  // a free slot, or none: this frame goes untimed
        const int s = (slot_ + k) % kSlots;
        if (!busy_[s]) { slot_ = s; inFrame_ = true; break; }
    }
    if (!inFrame_) return;
    count_[slot_] = 0;
    mark(0);
}

void GpuPassTimer::endFrame() {
    if (!enabled_ || !inFrame_) return;
    mark(kMarks - 1);
    busy_[slot_] = true;
    inFrame_ = false;
    slot_ = (slot_ + 1) % kSlots;
}

void GpuPassTimer::poll() {
    GLint disjoint = 0;
    glGetIntegerv(kGpuDisjoint, &disjoint);
    for (int s = 0; s < kSlots; ++s) {
        if (!busy_[s]) continue;
        const int n = count_[s];
        GLuint avail = 0;
        getQueryuiv(queries_[s][n - 1], kQueryResultAvailable, &avail);  // (the last mark lands last)
        if (!avail) continue;
        busy_[s] = false;
        if (disjoint || n < 2) continue;
        uint64_t t[kMarks] = {};
        for (int i = 0; i < n; ++i) getQueryui64v(queries_[s][i], kQueryResult, &t[i]);
        const double frameMs = double(t[n - 1] - t[0]) * 1e-6;
        if (!(frameMs > 0.0 && frameMs < 200.0)) continue;  // (implausible: dropped, as the frame timer does)
        double pass[int(GpuPass::Count)] = {};
        uint64_t open[int(GpuPass::Count)] = {};
        double inPasses = 0;
        for (int i = 1; i < n - 1; ++i) {
            const int p = (ids_[s][i] - 1) / 2;
            if (p < 0 || p >= int(GpuPass::Count)) continue;
            if (ids_[s][i] % 2 == 1) open[p] = t[i];
            else if (open[p] && t[i] >= open[p]) {
                const double ms = double(t[i] - open[p]) * 1e-6;
                pass[p] += ms;
                inPasses += ms;
                open[p] = 0;
            }
        }
        for (int p = 0; p < int(GpuPass::Count); ++p) { total_[p] += pass[p]; win_[p] += pass[p]; }
        total_[kFrameIdx] += frameMs;
        win_[kFrameIdx] += frameMs;
        const double eyes = frameMs > inPasses ? frameMs - inPasses : 0.0;
        total_[kEyesIdx] += eyes;
        win_[kEyesIdx] += eyes;
        ++frames_;
        ++winFrames_;
    }
    const double now = wallSeconds();
    if (now - lastLog_ >= 5.0 && winFrames_ > 0) {
        char buf[256];
        int o = std::snprintf(buf, sizeof buf, "GPU passes (ms, mean of %d frames):", winFrames_);
        for (int i = 0; i < int(GpuPass::Count) + 2 && o < int(sizeof buf); ++i)
            o += std::snprintf(buf + o, sizeof buf - size_t(o), " %s %.2f", kNames[i], win_[i] / winFrames_);
        LOGI("%s", buf);
        for (double& w : win_) w = 0;
        winFrames_ = 0;
        lastLog_ = now;
    }
}

std::string GpuPassTimer::summary() const {
    if (frames_ == 0) return "n/a";
    std::string s;
    char buf[48];
    const int order[] = {kFrameIdx, 0, 1, 2, 3, 4, kEyesIdx};
    for (int i : order) {
        std::snprintf(buf, sizeof buf, "%s%s %.2f", s.empty() ? "" : " ", kNames[i], total_[i] / frames_);
        s += buf;
    }
    std::snprintf(buf, sizeof buf, " (mean ms of %d frames)", frames_);
    return s + buf;
}

}  // namespace gvr
