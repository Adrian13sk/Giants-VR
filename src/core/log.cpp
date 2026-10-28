// The log's in-memory tail (Android): every line also goes into a fixed ring that a crash report can
// copy out without allocating (crash_handler.cpp). On the host the macros print directly.
#include "core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstring>

namespace gvr {
namespace {
constexpr int kLines = 160;
constexpr int kLineLen = 240;
char gRing[kLines][kLineLen];
std::atomic<unsigned> gNext{0};
}  // namespace

#if defined(__ANDROID__)
void logPrint(int prio, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    __android_log_write(prio, GVR_LOG_TAG, buf);
    const unsigned i = gNext.fetch_add(1) % kLines;
    const char lv = prio == ANDROID_LOG_ERROR ? 'E' : prio == ANDROID_LOG_WARN ? 'W' : 'I';
    gRing[i][0] = lv;
    gRing[i][1] = ' ';
    std::strncpy(gRing[i] + 2, buf, kLineLen - 3);
    gRing[i][kLineLen - 1] = 0;
}
#endif

size_t recentLogLines(char* out, size_t outSize, int max) {
    const unsigned n = gNext.load();
    const unsigned count = n < unsigned(kLines) ? n : unsigned(kLines);
    const unsigned take = count < unsigned(max) ? count : unsigned(max);
    size_t w = 0;
    for (unsigned k = n - take; k < n && w + 2 < outSize; ++k) {
        const char* line = gRing[k % kLines];
        for (size_t j = 0; line[j] && j < size_t(kLineLen) && w + 2 < outSize; ++j) out[w++] = line[j];
        out[w++] = '\n';
    }
    if (w < outSize) out[w] = 0;
    return w;
}

}  // namespace gvr
