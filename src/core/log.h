// Logging shared by the Android runtime and the desktop/headless tools.
#pragma once

#include <cstdio>

namespace gvr {
// (2026-10-28) The last log lines, kept in memory for a crash report (Android: crash_handler.cpp).
// Copies up to `max` lines, oldest first, into `out` (each line '\n'-terminated); returns the bytes written.
size_t recentLogLines(char* out, size_t outSize, int max);
}  // namespace gvr

#if defined(__ANDROID__)
#include <android/log.h>
#define GVR_LOG_TAG "GiantsVR"
namespace gvr {
void logPrint(int prio, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
}
#define LOGI(...) ::gvr::logPrint(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) ::gvr::logPrint(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) ::gvr::logPrint(ANDROID_LOG_ERROR, __VA_ARGS__)
#else
#define LOGI(...) (std::fprintf(stdout, "[I] " __VA_ARGS__), std::fputc('\n', stdout))
#define LOGW(...) (std::fprintf(stderr, "[W] " __VA_ARGS__), std::fputc('\n', stderr))
#define LOGE(...) (std::fprintf(stderr, "[E] " __VA_ARGS__), std::fputc('\n', stderr))
#endif
