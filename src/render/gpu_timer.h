// GPU time per render pass (2026-10-28), from GL_EXT_disjoint_timer_query timestamps (glQueryCounterEXT).
// Marks sit only at the edges of whole framebuffer passes - the sun's shadow map, the sky haze cube, the water
// reflection, the sniper scope, the cinema screen - and at the frame's start and end. On a tiling GPU (the
// Quest's Adreno) a mark in the middle of a pass would split nothing useful, so what is left of the frame is the
// eyes' own pass ("eyes": terrain, objects, foliage, water, particles, post together); its parts are still priced
// by switching them off (GVR_LOOK_OFF).
// Off unless enabled (GVR_GPU_PASSES=1; scripted device runs turn it on). Results arrive some frames later;
// summary() gives the mean per pass, and a log line every 5 s says the same.
#pragma once

#include <cstdint>
#include <string>

namespace gvr {

enum class GpuPass { Shadow, Haze, Reflection, Scope, Screen, Count };

class GpuPassTimer {
public:
    // Needs a current GL context. False (and every call a no-op) without timestamp support.
    bool init();
    bool enabled() const { return enabled_; }
    void beginFrame();
    void endFrame();
    void begin(GpuPass p) { mark(int(p) * 2 + 1); }
    void end(GpuPass p) { mark(int(p) * 2 + 2); }
    // "frame 12.3 shadow 1.1 haze 0.2 reflection 2.0 scope 0.0 screen 0.0 eyes 9.0 (mean ms of N frames)".
    std::string summary() const;
    int framesTimed() const { return frames_; }

private:
    static constexpr int kSlots = 6;        // frames in flight
    static constexpr int kMarks = 2 + 2 * int(GpuPass::Count) * 2;  // (a pass may run twice a frame)
    void mark(int id);
    void poll();

    bool enabled_ = false, inFrame_ = false;
    unsigned queries_[kSlots][kMarks] = {};
    int ids_[kSlots][kMarks] = {};
    int count_[kSlots] = {};
    bool busy_[kSlots] = {};
    int slot_ = 0;
    // Totals since the start (ms) and in the current 5 s window.
    double total_[int(GpuPass::Count) + 2] = {};  // passes, then frame, then eyes
    int frames_ = 0;
    double win_[int(GpuPass::Count) + 2] = {};
    int winFrames_ = 0;
    double lastLog_ = 0;
};

}  // namespace gvr
