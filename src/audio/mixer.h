// Software audio mixer (platform-independent, unit-tested).
//
// Sounds are the original game's WAV files (PCM 16-bit, mostly mono 22.05 kHz), converted to
// float mono at load. Voices are positional (distance attenuation + equal-power stereo pan
// relative to the listener's head) or head-relative (2D). The platform layer (AAudio on Quest,
// a WAV writer in the headless harness) pulls interleaved stereo float frames with mix().
// Thread-safe: game-thread calls and the audio callback share one short critical section.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math.h"

namespace gvr {

struct WavData {
    uint32_t rate = 0;
    uint16_t channels = 0;
    std::vector<float> mono;  // downmixed to mono, -1..1
};
// Parses a RIFF/WAVE PCM file (8/16-bit, mono/stereo). False on anything else.
bool parseWav(const uint8_t* data, size_t size, WavData& out, std::string* error = nullptr);

class AudioMixer {
public:
    explicit AudioMixer(uint32_t outputRate = 48000) : outRate_(outputRate) {}
    uint32_t outputRate() const { return outRate_; }

    // Registers a decoded sound under `name` (lower-case). Returns false if empty.
    bool addSound(const std::string& name, WavData&& wav);
    bool hasSound(const std::string& name) const;

    // Starts a voice; returns its id (0 = failed). Positional voices use world coordinates.
    uint32_t play(const std::string& name, const Vec3& pos, float volume = 1.0f, float pitch = 1.0f,
                  bool loop = false, bool positional = true);
    uint32_t play2D(const std::string& name, float volume = 1.0f, float pitch = 1.0f, bool loop = false) {
        return play(name, Vec3{0, 0, 0}, volume, pitch, loop, false);
    }
    void stop(uint32_t id);
    void setPosition(uint32_t id, const Vec3& pos);
    void setVolume(uint32_t id, float volume);
    bool playing(uint32_t id) const;
    void setListener(const Vec3& pos, const Quat& orientation);
    void setMasterVolume(float v);

    // Music: interleaved stereo buffers (any rate) played back to back without gaps.
    void queueMusic(std::vector<float>&& stereo, uint32_t rate);
    size_t musicQueued() const;     // buffers waiting or playing
    void clearMusic();
    void setMusicVolume(float v);
    uint64_t musicFramesPlayed() const;  // output frames of music mixed so far

    // Mixes `frames` stereo frames (interleaved L,R) into out (overwrites).
    void mix(float* out, uint32_t frames);
    size_t activeVoices() const;

    float refDistance = 2.0f;   // m: full volume within this distance
    float maxDistance = 90.0f;  // m: silent beyond

private:
    struct Sound { uint32_t rate; std::vector<float> data; };
    struct Voice {
        uint32_t id;
        const Sound* sound;
        double cursor;  // source frame position
        float volume, pitch;
        bool loop, positional;
        Vec3 pos;
        float gainL, gainR;  // smoothed toward the target to avoid zipper noise
        bool started;
    };
    void targetGains(const Voice& v, float& l, float& r) const;
    mutable std::mutex mu_;
    uint32_t outRate_;
    std::unordered_map<std::string, Sound> sounds_;
    std::vector<Voice> voices_;
    uint32_t nextId_ = 1;
    Vec3 listenerPos_;
    Quat listenerRot_;
    float master_ = 1.0f;
    struct MusicBuf { std::vector<float> data; uint32_t rate; };
    std::deque<MusicBuf> music_;
    double musicCursor_ = 0;  // source frame position in music_.front()
    float musicVolume_ = 0.5f;
    uint64_t musicFrames_ = 0;
};

}  // namespace gvr
