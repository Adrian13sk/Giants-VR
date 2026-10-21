// Quest audio output: an AAudio low-latency float stereo stream whose data callback pulls
// from the platform-independent AudioMixer. Handles pause/resume and device disconnects
// (the stream is reopened from the main thread, never inside AAudio's callbacks).
#pragma once

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>

namespace gvr {

class AudioMixer;

class AAudioOutput {
public:
    // Opens the stream; returns the device sample rate (0 on failure).
    int32_t open();
    void attach(AudioMixer* mixer) { mixer_.store(mixer); }
    bool start();
    void pause();
    void close();
    // Call once per frame: reopens and restarts the stream after a disconnect.
    void poll();
    bool isOpen() const { return stream_ != nullptr; }

private:
    static aaudio_data_callback_result_t onData(AAudioStream* s, void* user, void* data, int32_t frames);
    static void onError(AAudioStream* s, void* user, aaudio_result_t error);
    AAudioStream* stream_ = nullptr;
    std::atomic<AudioMixer*> mixer_{nullptr};
    std::atomic<bool> needRestart_{false};
    bool running_ = false;
};

}  // namespace gvr
