// Logging shared by the Android runtime and the desktop/headless tools.
#pragma once

#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#define GVR_LOG_TAG "GiantsVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, GVR_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, GVR_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, GVR_LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) (std::fprintf(stdout, "[I] " __VA_ARGS__), std::fputc('\n', stdout))
#define LOGW(...) (std::fprintf(stderr, "[W] " __VA_ARGS__), std::fputc('\n', stderr))
#define LOGE(...) (std::fprintf(stderr, "[E] " __VA_ARGS__), std::fputc('\n', stderr))
#endif
