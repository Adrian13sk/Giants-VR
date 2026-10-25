#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gvr {

bool parseWav(const uint8_t* d, size_t n, WavData& out, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return fail("not RIFF/WAVE");
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* pcm = nullptr;
    uint32_t pcmLen = 0;
    size_t p = 12;
    while (p + 8 <= n) {
        uint32_t len;
        std::memcpy(&len, d + p + 4, 4);
        const uint8_t* body = d + p + 8;
        if (p + 8 + len > n) len = uint32_t(n - p - 8);  // tolerate truncated last chunk
        if (std::memcmp(d + p, "fmt ", 4) == 0 && len >= 16) {
            std::memcpy(&fmt, body, 2);
            std::memcpy(&ch, body + 2, 2);
            std::memcpy(&rate, body + 4, 4);
            std::memcpy(&bits, body + 14, 2);
        } else if (std::memcmp(d + p, "data", 4) == 0) {
            pcm = body;
            pcmLen = len;
        }
        p += 8 + len + (len & 1);
    }
    if (fmt != 1) return fail("not PCM");
    if (ch < 1 || ch > 2 || (bits != 8 && bits != 16) || rate == 0 || !pcm) return fail("unsupported PCM layout");
    const uint32_t bytesPerFrame = ch * (bits / 8);
    const uint32_t frames = pcmLen / bytesPerFrame;
    out.rate = rate;
    out.channels = ch;
    out.mono.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        float acc = 0;
        for (uint32_t c = 0; c < ch; ++c) {
            const uint8_t* s = pcm + i * bytesPerFrame + c * (bits / 8);
            if (bits == 16) {
                int16_t v;
                std::memcpy(&v, s, 2);
                acc += float(v) / 32768.0f;
            } else {
                acc += (float(*s) - 128.0f) / 128.0f;
            }
        }
        out.mono[i] = acc / float(ch);
    }
    return true;
}

bool AudioMixer::addSound(const std::string& name, WavData&& wav) {
    if (wav.mono.empty() || wav.rate == 0) return false;
    std::lock_guard<std::mutex> lk(mu_);
    sounds_[name] = Sound{wav.rate, std::move(wav.mono)};
    return true;
}

bool AudioMixer::hasSound(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    return sounds_.count(name) != 0;
}

uint32_t AudioMixer::play(const std::string& name, const Vec3& pos, float volume, float pitch, bool loop,
                          bool positional) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sounds_.find(name);
    if (it == sounds_.end()) return 0;
    if (voices_.size() >= 48) {
        // Voice cap: drop the oldest non-looping voice.
        auto old = std::find_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.loop; });
        if (old == voices_.end()) return 0;
        voices_.erase(old);
    }
    Voice v{nextId_++, &it->second, 0.0, volume, std::max(0.05f, pitch), loop, positional, pos, 0, 0, false, 0, 0, 1, 1, 0, 0};
    voices_.push_back(v);
    return v.id;
}

void AudioMixer::stop(uint32_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [id](const Voice& v) { return v.id == id; }), voices_.end());
}

void AudioMixer::setPosition(uint32_t id, const Vec3& pos) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& v : voices_)
        if (v.id == id) v.pos = pos;
}

void AudioMixer::setVolume(uint32_t id, float volume) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& v : voices_)
        if (v.id == id) v.volume = volume;
}

bool AudioMixer::playing(uint32_t id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& v : voices_)
        if (v.id == id) return true;
    return false;
}

void AudioMixer::setListener(const Vec3& pos, const Quat& rot) {
    std::lock_guard<std::mutex> lk(mu_);
    listenerPos_ = pos;
    listenerRot_ = rot;
}

void AudioMixer::setMasterVolume(float v) {
    std::lock_guard<std::mutex> lk(mu_);
    master_ = v;
}

size_t AudioMixer::activeVoices() const {
    std::lock_guard<std::mutex> lk(mu_);
    return voices_.size();
}

void AudioMixer::targetGains(const Voice& v, float& l, float& r) const {
    float g = v.volume * master_;
    if (!v.positional) {
        l = r = g * 0.7071f;
        return;
    }
    const Vec3 d = v.pos - listenerPos_;
    const float dist = length(d);
    if (dist >= maxDistance) { l = r = 0; return; }
    // Inverse-distance rolloff beyond refDistance, faded to zero at maxDistance.
    g *= refDistance / std::max(refDistance, dist);
    g *= 1.0f - dist / maxDistance;
    // Pan: listener-space right component (listener looks down -Z, right is +X).
    float pan = 0;
    float behind = 0;
    if (dist > 1e-3f) {
        const Vec3 local = listenerRot_.conjugate().rotate(d * (1.0f / dist));
        pan = std::max(-1.0f, std::min(1.0f, local.x));
        behind = std::max(0.0f, local.z);  // +Z = behind the listener
    }
    g *= 1.0f - 0.15f * behind;  // sounds behind are a little quieter (and duller: targetCues)
    pan *= 0.8f;  // (the delay and head shadow carry part of the direction now)
    const float a = (pan + 1.0f) * 0.25f * kPi;  // equal-power
    l = g * std::cos(a);
    r = g * std::sin(a);
}

void AudioMixer::targetCues(const Voice& v, float& delayL, float& delayR, float& lpaL, float& lpaR) const {
    delayL = delayR = 0;
    lpaL = lpaR = 1.0f;
    if (!v.positional) return;
    const Vec3 d = v.pos - listenerPos_;
    const float dist = length(d);
    if (dist < 1e-3f) return;
    const Vec3 local = listenerRot_.conjugate().rotate(d * (1.0f / dist));
    const float side = std::max(-1.0f, std::min(1.0f, local.x)), behind = std::max(0.0f, local.z);
    // Interaural time difference: up to ~0.65 ms to the far ear (Woodworth's head, simplified).
    const float itd = 0.00065f * side;
    if (itd > 0) delayL = itd; else delayR = -itd;
    // Head shadow: the far ear rolls off from ~16 kHz (ahead) to ~3 kHz (fully to the side); behind,
    // both ears lose some top (~7 kHz).
    auto coef = [&](float fc) { return 1.0f - std::exp(-6.2831853f * fc / float(outRate_)); };
    const float farFc = 16000.0f - 13000.0f * std::fabs(side);
    const float backFc = 16000.0f - 9000.0f * behind;
    const float nearFc = backFc, farEarFc = std::min(farFc, backFc);
    if (side > 0) { lpaL = coef(farEarFc); lpaR = coef(nearFc); }
    else { lpaR = coef(farEarFc); lpaL = coef(nearFc); }
}

void AudioMixer::queueMusic(std::vector<float>&& stereo, uint32_t rate) {
    if (stereo.size() < 4 || rate == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    music_.push_back({std::move(stereo), rate});
}

size_t AudioMixer::musicQueued() const {
    std::lock_guard<std::mutex> lk(mu_);
    return music_.size();
}

void AudioMixer::clearMusic() {
    std::lock_guard<std::mutex> lk(mu_);
    music_.clear();
    musicCursor_ = 0;
}

void AudioMixer::setMusicVolume(float v) {
    std::lock_guard<std::mutex> lk(mu_);
    musicVolume_ = v;
}

uint64_t AudioMixer::musicFramesPlayed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return musicFrames_;
}

void AudioMixer::mix(float* out, uint32_t frames) {
    std::fill(out, out + size_t(frames) * 2, 0.0f);
    std::lock_guard<std::mutex> lk(mu_);
    // Music: seamless across queued buffers (linear interpolation, the next buffer supplies
    // the sample after the last one of the current buffer).
    for (uint32_t i = 0; i < frames && !music_.empty(); ++i) {
        const MusicBuf& b = music_.front();
        const size_t n = b.data.size() / 2;
        size_t i0 = size_t(musicCursor_);
        if (i0 >= n) {
            musicCursor_ -= double(n);
            music_.pop_front();
            --i;
            continue;
        }
        const float f = float(musicCursor_ - double(i0));
        float l1, r1;
        if (i0 + 1 < n) { l1 = b.data[2 * i0 + 2]; r1 = b.data[2 * i0 + 3]; }
        else if (music_.size() > 1) { l1 = music_[1].data[0]; r1 = music_[1].data[1]; }
        else { l1 = b.data[2 * i0]; r1 = b.data[2 * i0 + 1]; }
        const float g = musicVolume_ * master_;
        out[2 * i] += (b.data[2 * i0] + (l1 - b.data[2 * i0]) * f) * g;
        out[2 * i + 1] += (b.data[2 * i0 + 1] + (r1 - b.data[2 * i0 + 1]) * f) * g;
        musicCursor_ += double(b.rate) / double(outRate_);
        ++musicFrames_;
    }
    for (auto& v : voices_) {
        const Sound& s = *v.sound;
        const double step = double(s.rate) / double(outRate_) * double(v.pitch);
        float tl, tr;
        targetGains(v, tl, tr);
        float dL, dR, aL, aR;
        targetCues(v, dL, dR, aL, aR);
        if (!v.started) { v.gainL = tl; v.gainR = tr; v.delayL = dL; v.delayR = dR; v.started = true; }
        v.lpaL = aL;
        v.lpaR = aR;
        // Linear gain ramp over the block (no zipper noise when sources move).
        const float dl = (tl - v.gainL) / float(frames), dr = (tr - v.gainR) / float(frames);
        const size_t len = s.data.size();
        for (uint32_t i = 0; i < frames; ++i) {
            size_t i0 = size_t(v.cursor);
            if (i0 + 1 >= len) {
                if (!v.loop) { v.cursor = double(len); break; }
                v.cursor = std::fmod(v.cursor, double(len - 1));
                i0 = size_t(v.cursor);
            }
            // Each ear reads the source a little earlier by its delay (the far ear hears it later).
            auto at = [&](double c) {
                if (c < 0 && v.loop) c += double(len - 1);  // (a loop: the delayed ear reads the end of the previous lap)
                if (c <= 0) return s.data[0] * float(std::max(0.0, 1.0 + c));  // (fades in from silence)
                const size_t j = size_t(c);
                if (j + 1 >= len) return 0.0f;
                const float fr = float(c - double(j));
                return s.data[j] + (s.data[j + 1] - s.data[j]) * fr;
            };
            const double srcPerSec = double(s.rate) * double(v.pitch);
            const float sl = at(v.cursor - double(v.delayL) * srcPerSec), sr = at(v.cursor - double(v.delayR) * srcPerSec);
            v.lpL += v.lpaL * (sl - v.lpL);
            v.lpR += v.lpaR * (sr - v.lpR);
            v.gainL += dl;
            v.gainR += dr;
            out[2 * i] += v.lpL * v.gainL;
            out[2 * i + 1] += v.lpR * v.gainR;
            v.cursor += step;
        }
        v.gainL = tl;
        v.gainR = tr;
        // The delays glide to their targets block by block (no clicks as a source swings round).
        v.delayL += (dL - v.delayL) * 0.5f;
        v.delayR += (dR - v.delayR) * 0.5f;
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const Voice& v) { return !v.loop && v.cursor + 1.0 >= double(v.sound->data.size()); }),
                  voices_.end());
    // Soft clip so many simultaneous sounds do not hard-clip.
    for (uint32_t i = 0; i < frames * 2; ++i) out[i] = std::tanh(out[i]);
}

}  // namespace gvr
