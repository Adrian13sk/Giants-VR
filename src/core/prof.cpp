#include "core/prof.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "core/log.h"

namespace gvr {

namespace {
struct Stat { double sum = 0, worst = 0; long n = 0; };
std::map<std::string, Stat>& table() { static std::map<std::string, Stat> t; return t; }
}  // namespace

bool profEnabled() {
    static const bool on = std::getenv("GVR_PROFILE") != nullptr;
    return on;
}

void profAdd(const char* name, double ms) {
    Stat& s = table()[name];
    s.sum += ms;
    s.worst = std::max(s.worst, ms);
    ++s.n;
}

void profReport() {
    if (!profEnabled()) return;
    for (const auto& [name, s] : table())
        if (s.n > 0) LOGI("prof %-22s %7.3f ms avg %7.3f ms worst (%ld)", name.c_str(), s.sum / double(s.n), s.worst, s.n);
}

}  // namespace gvr
