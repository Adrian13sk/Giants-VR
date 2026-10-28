#include "platform/android/audio_aaudio.h"

#include <cmath>
#include <cstring>

#include "audio/mixer.h"
#include "core/log.h"

namespace gvr {

int32_t AAudioOutput::open() {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
        LOGE("AAudio: createStreamBuilder failed");
        return 0;
    }
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, &AAudioOutput::onData, this);
    AAudioStreamBuilder_setErrorCallback(b, &AAudioOutput::onError, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !stream_) {
        LOGE("AAudio: openStream failed: %s", AAudio_convertResultToText(r));
        stream_ = nullptr;
        return 0;
    }
    const int32_t rate = AAudioStream_getSampleRate(stream_);
    LOGI("AAudio: %d Hz, %d ch, burst %d frames, buffer %d frames", rate, AAudioStream_getChannelCount(stream_),
         AAudioStream_getFramesPerBurst(stream_), AAudioStream_getBufferSizeInFrames(stream_));
    return rate;
}

bool AAudioOutput::start() {
    if (!stream_ || running_) return running_;
    const aaudio_result_t r = AAudioStream_requestStart(stream_);
    running_ = r == AAUDIO_OK;
    if (!running_) LOGW("AAudio: requestStart failed: %s", AAudio_convertResultToText(r));
    return running_;
}

void AAudioOutput::pause() {
    if (!stream_ || !running_) return;
    AAudioStream_requestPause(stream_);
    running_ = false;
}

void AAudioOutput::close() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
    running_ = false;
}

void AAudioOutput::poll() {
    if (!needRestart_.exchange(false)) return;
    LOGW("AAudio: stream disconnected; reopening");
    const bool wasRunning = running_;
    close();
    if (open() > 0 && wasRunning) start();
}

aaudio_data_callback_result_t AAudioOutput::onData(AAudioStream*, void* user, void* data, int32_t frames) {
    auto* self = static_cast<AAudioOutput*>(user);
    float* out = static_cast<float*>(data);
    AudioMixer* m = self->mixer_.load();
    if (m) m->mix(out, uint32_t(frames));
    else std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioOutput::onError(AAudioStream*, void* user, aaudio_result_t error) {
    // Called on an AAudio thread: only flag; poll() reopens on the main thread.
    if (error == AAUDIO_ERROR_DISCONNECTED) static_cast<AAudioOutput*>(user)->needRestart_.store(true);
}

bool AAudioInput::open() {
    if (stream_) return true;
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_GENERIC);
    AAudioStreamBuilder_setDataCallback(b, &AAudioInput::onData, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !stream_) {
        LOGW("Microphone: openStream failed: %s", AAudio_convertResultToText(r));
        stream_ = nullptr;
        return false;
    }
    if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
        AAudioStream_close(stream_);
        stream_ = nullptr;
        LOGW("Microphone: requestStart failed");
        return false;
    }
    LOGI("Microphone: %d Hz input open (Kabuto's roar)", AAudioStream_getSampleRate(stream_));
    return true;
}

void AAudioInput::close() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
    level_.store(0.0f);
    LOGI("Microphone: closed");
}

aaudio_data_callback_result_t AAudioInput::onData(AAudioStream*, void* user, void* data, int32_t frames) {
    auto* self = static_cast<AAudioInput*>(user);
    const float* x = static_cast<const float*>(data);
    double sum = 0;
    for (int32_t i = 0; i < frames; ++i) sum += double(x[i]) * double(x[i]);
    const float rms = frames > 0 ? float(std::sqrt(sum / double(frames))) : 0.0f;
    const float prev = self->level_.load();
    self->level_.store(rms > prev ? rms : prev * 0.9f + rms * 0.1f);  // (fast up, slow down)
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

}  // namespace gvr
