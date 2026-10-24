// On-device scripted test run (see game/test_script.h): loads <files>/autotest.txt, drives the
// game with it instead of the controllers, measures every frame (interval, CPU update and render
// time, GPU time from GL_EXT_disjoint_timer_query, draw calls), takes left-eye screenshots, and
// writes <files>/autotest_out/{result.txt,frames.csv,*.png,done}. The headset needn't be worn.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/test_script.h"
#include "render/gl.h"

namespace gvr {

class GameApp;
struct XrFrameContext;
class XrApp;

class AutoTest {
public:
    // Looks for the script in `filesDir`; consumes it (a crash won't rerun it) and prepares the
    // output folder. False when there's no test to run.
    bool load(const std::string& filesDir);
    bool active() const { return active_; }
    const TestScript& script() const { return script_; }
    std::string world() const { return script_.world; }

    // Before game.init: the script's environment (mission, test hooks).
    void applyEnvironment() const;
    // Every frame after xr.beginFrame: replace input and eye poses with the script's.
    void beginFrame(GameApp& game, XrApp& xr, XrFrameContext& frame, double now);
    // Timing around the parts of the frame.
    void updateDone(double ms) { updMs_ = ms; }
    void renderBegin();
    void renderEnd(double cpuMs, unsigned drawCalls);
    // After rendering an eye, before it is released: screenshots of the left eye.
    void afterEye(int eye, int width, int height);
    // True when the run is over (results written): the app should exit.
    bool finished() const { return finished_; }
    void endFrame(double now);

private:
    void writeResults();
    void initTimers();
    void pollTimers();

    TestScript script_;
    GameApp* game_ = nullptr;
    std::string out_;
    bool active_ = false, started_ = false, finished_ = false;
    double t0_ = 0, lastNow_ = 0;
    float t_ = 0, prevT_ = 0;
    std::vector<std::string> pendingShots_;
    // Per frame.
    struct Sample { float t, dtMs, updMs, cpuMs, gpuMs; unsigned draws; bool skip; };
    int skip_ = 0;  // frames after a screenshot (its read-back stalls): left out of the statistics
    std::vector<Sample> samples_;
    double updMs_ = 0;
    std::string error_;
    // GPU timing.
    bool timers_ = false;
    static constexpr int kQueries = 6;
    GLuint queries_[kQueries] = {};
    int qFrame_[kQueries] = {};
    bool qBusy_[kQueries] = {};
    int qNext_ = 0, frameNo_ = 0;
    std::vector<float> gpuByFrame_;
};

}  // namespace gvr
