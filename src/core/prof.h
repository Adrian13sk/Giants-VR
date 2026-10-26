// Opt-in section timing (GVR_PROFILE=1): GVR_PROF("name") times the rest of the scope; profReport()
// logs each section's average and worst milliseconds. Costs one branch when off.
#pragma once

#include <chrono>

namespace gvr {

bool profEnabled();
void profAdd(const char* name, double ms);
void profReport();

struct ProfScope {
    const char* name;
    std::chrono::steady_clock::time_point t0;
    bool on;
    explicit ProfScope(const char* n) : name(n), on(profEnabled()) {
        if (on) t0 = std::chrono::steady_clock::now();
    }
    ~ProfScope() {
        if (on) profAdd(name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
};

}  // namespace gvr

#define GVR_PROF_CAT2(a, b) a##b
#define GVR_PROF_CAT(a, b) GVR_PROF_CAT2(a, b)
#define GVR_PROF(name) ::gvr::ProfScope GVR_PROF_CAT(prof_, __LINE__)(name)
// A section that isn't a scope: GVR_PROF_BEGIN(id, "name") ... GVR_PROF_END(id).
#define GVR_PROF_BEGIN(id, name) auto* id##_scope = new ::gvr::ProfScope(name)
#define GVR_PROF_END(id) delete id##_scope
