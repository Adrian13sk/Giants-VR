// GameApp, rework: ambience and weather. The original's levels carry "a_..." markers from
// xx_ambient (a_shoreline, a_trees, a_high_winds, a_river, a_volcano, ... and one per level such as
// a_r1_light_rain, a_r2_snow, a_k5_storm) but its data has no ambient sound for them (they name
// zones; how the original used them is not known). Ours: procedural loops (audio/ambience.h) at the
// nearest markers - the surf along the shore, gusts of wind, rustling leaves, running water, a
// volcano's rumble - each voice heard from its marker's direction; and rain (light, or a storm) or
// snow falling round the player in the levels whose marker says so. Options -> AMBIENCE.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio/ambience.h"
#include "audio/mixer.h"
#include "core/log.h"
#include "game/game_app.h"
#include "scene/asset_world.h"

namespace gvr {

namespace {
constexpr int kKinds = 5;
// Per kind: hearing radius (m), voices at most, loudness.
constexpr float kRadius[kKinds] = {70.0f, 90.0f, 28.0f, 40.0f, 160.0f};
constexpr int kVoices[kKinds] = {3, 2, 2, 2, 1};
constexpr float kGain[kKinds] = {1.0f, 0.55f, 0.5f, 0.6f, 0.9f};
constexpr float kVirtual = 6.0f;  // voices sit this far off in their marker's direction (the mixer's own roll-off is steep)

int kindOf(const std::string& n, float& gain) {
    gain = 1.0f;
    if (n.rfind("a_shoreline", 0) == 0) return 0;
    if (n.rfind("a_m", 0) == 0 && n.find("beach") != std::string::npos) return 0;
    if (n == "a_high_winds_light") { gain = 0.55f; return 1; }
    if (n == "a_high_winds" || n == "a_wind_gusts" || n == "a_cold_wind_gusts") return 1;
    if (n == "a_trees" || n == "a_forest_trees" || n == "a_single_tree" || n == "a_big_leaf_tree" || n == "a_special_tree") {
        if (n == "a_single_tree") gain = 0.6f;
        return 2;
    }
    if (n == "a_river") return 3;
    if (n == "a_volcano" || n == "a_int_volcano") return 4;
    return -1;
}
}  // namespace

void GameApp::makeAmbienceSounds() {
    // The loops, made once while loading (about 70 s of mono audio at 22 kHz, ~0.1 s of work: not
    // in a frame of play) - when the mixer arrives, or at a level load.
    if (!audio_ || audio_->hasSound(ambienceName(AmbienceKind::Surf))) return;
    const AmbienceKind kinds[] = {AmbienceKind::Surf, AmbienceKind::Wind, AmbienceKind::Leaves, AmbienceKind::River, AmbienceKind::Rumble, AmbienceKind::Rain};
    for (AmbienceKind k : kinds) audio_->addSound(ambienceName(k), makeAmbience(k, 22050, 7));
    for (uint32_t v = 0; v < 3; ++v)  // three different claps of thunder
        audio_->addSound(std::string(ambienceName(AmbienceKind::Thunder)) + char('1' + v), makeAmbience(AmbienceKind::Thunder, 22050, 11 + v));
    LOGI("Ambience: loops made");
}

void GameApp::initAmbience(AssetWorld* w) {
    if (audio_) {
        for (const auto& v : ambVoices_) audio_->stop(v.id);
        if (ambRain_) audio_->stop(ambRain_);
    }
    ambVoices_.clear();
    ambRain_ = 0;
    ambRainVol_ = 0;
    weather_ = 0;
    weatherK_ = 0;
    strikeModel_ = nullptr;
    boltLoaded_ = false;
    lightningT_ = -1.0f;
    lightningNext_ = 5.0f;
    thunderDelay_ = 0;
    drops_.clear();
    dropSplashes_.clear();
    for (int k = 0; k < kKinds; ++k) { ambPts_[k].clear(); ambGain_[k].clear(); }
    makeAmbienceSounds();
    if (!w) return;
    int counts[kKinds] = {};
    for (const auto& m : w->ambientMarkers()) {
        float g;
        const int k = kindOf(m.name, g);
        if (k >= 0) { ambPts_[k].push_back(m.pos); ambGain_[k].push_back(g); ++counts[k]; }
        if (m.name == "a_r1_light_rain") weather_ = 1;
        else if (m.name == "a_k5_storm") weather_ = 2;
        else if (m.name == "a_r2_snow") weather_ = 3;
    }
    if (const char* e = std::getenv("GVR_WEATHER")) weather_ = std::atoi(e);  // (tests: 0 none, 1 rain, 2 storm, 3 snow)
    LOGI("Ambience: %d shore, %d wind, %d trees, %d water, %d volcano markers; weather %s", counts[0], counts[1], counts[2], counts[3], counts[4],
         weather_ == 1 ? "light rain" : (weather_ == 2 ? "storm" : (weather_ == 3 ? "snow" : "none")));
}

void GameApp::updateAmbience(float dt) {
    const int mode = settings_.ambience;
    const bool sounds = mode >= 1 && audio_ && playMode_ && !title_;
    const Vec3 ear = rig_.toWorld(lastInput_.head).position;
    const float scale = std::sqrt(std::max(1.0f, heroScale_));  // (Kabuto hears further)
    // Which markers sound now: per kind, the nearest few within reach.
    std::vector<AmbVoice> want;
    if (sounds && !cutsceneActive())
        for (int k = 0; k < kKinds; ++k) {
            std::vector<std::pair<float, size_t>> near;
            const float R = kRadius[k] * scale;
            for (size_t i = 0; i < ambPts_[k].size(); ++i) {
                const float d = length(ambPts_[k][i] - ear);
                if (d < R) near.push_back({d, i});
            }
            std::sort(near.begin(), near.end());
            for (int j = 0; j < kVoices[k] && j < int(near.size()); ++j) {
                AmbVoice v;
                v.kind = k;
                v.marker = near[size_t(j)].second;
                const float f = 1.0f - near[size_t(j)].first / R;
                v.target = kGain[k] * ambGain_[k][v.marker] * f * std::sqrt(f) * (j == 0 ? 1.0f : 0.7f);
                want.push_back(v);
            }
        }
    // Fade the playing ones toward their targets (0 if no longer wanted); start the new ones.
    for (auto& v : ambVoices_) {
        v.target = 0;
        for (const auto& w : want)
            if (w.kind == v.kind && w.marker == v.marker) v.target = w.target;
    }
    for (const auto& w : want) {
        bool have = false;
        for (const auto& v : ambVoices_) have |= v.kind == w.kind && v.marker == w.marker;
        if (have || !audio_) continue;
        AmbVoice v = w;
        v.vol = 0;
        const float pitch = 0.92f + 0.16f * frand();  // (two of the same loop never in step)
        v.id = audio_->play(ambienceName(AmbienceKind(w.kind)), ear, 0.0f, pitch, true, true);
        if (v.id) ambVoices_.push_back(v);
    }
    for (auto& v : ambVoices_) {
        v.vol += (v.target - v.vol) * std::min(1.0f, dt * 1.2f);
        const Vec3 m = ambPts_[v.kind][v.marker];
        Vec3 dir = m - ear;
        const float d = length(dir);
        // Close by, it surrounds you (less direction); further, it comes from the marker.
        dir = d > 0.1f ? dir * (1.0f / d) : Vec3{0, 0, -1};
        const float off = std::min(kVirtual, std::max(1.0f, d * 0.5f));
        audio_->setPosition(v.id, ear + dir * off);
        // (undoes the mixer's roll-off at `off`: none within refDistance, 1/d beyond, faded to 0 at maxDistance)
        const float rolloff = (audio_->refDistance / std::max(off, audio_->refDistance)) * (1.0f - off / audio_->maxDistance);
        audio_->setVolume(v.id, v.vol * 0.55f / std::max(0.05f, rolloff));
    }
    ambVoices_.erase(std::remove_if(ambVoices_.begin(), ambVoices_.end(),
                                    [this](const AmbVoice& v) {
                                        if (v.target > 0 || v.vol > 0.005f) return false;
                                        if (audio_) audio_->stop(v.id);
                                        return true;
                                    }),
                     ambVoices_.end());
    // Rain: all round (2D), louder in a storm.
    const bool rain = mode >= 2 && sounds && (weather_ == 1 || weather_ == 2) && !cutsceneActive();
    const float rainWant = rain ? (weather_ == 2 ? 0.55f : 0.3f) : 0.0f;
    if (audio_ && rainWant > 0 && !ambRain_) ambRain_ = audio_->play2D(ambienceName(AmbienceKind::Rain), 0.0f, 1.0f, true);
    ambRainVol_ += (rainWant - ambRainVol_) * std::min(1.0f, dt * 0.8f);
    if (audio_ && ambRain_) {
        audio_->setVolume(ambRain_, ambRainVol_);
        if (rainWant <= 0 && ambRainVol_ < 0.005f) { audio_->stop(ambRain_); ambRain_ = 0; }
    }
    static float trace = 0;
    static const bool ambTrace = std::getenv("GVR_AMB_TRACE") != nullptr;
    if (ambTrace && (trace += dt) > 1.0f) {
        trace = 0;
        std::string s;
        for (const auto& v : ambVoices_)
            s += std::string(" ") + ambienceName(AmbienceKind(v.kind)) + "@" + std::to_string(int(length(ambPts_[v.kind][v.marker] - ear))) + "m:" +
                 std::to_string(v.vol).substr(0, 4);
        LOGI("Ambience voices:%s%s", s.c_str(), ambRain_ ? " + rain" : "");
    }
    updateWeather(dt);
}

void GameApp::updateWeather(float dt) {
    const bool on = settings_.ambience >= 2 && weather_ != 0 && playMode_ && !title_ && assetWorld_;
    if (!on) { drops_.clear(); dropSplashes_.clear(); return; }
    const float s = std::max(1.0f, heroScale_);
    const Vec3 eye = rig_.toWorld(lastInput_.head).position;
    const bool snow = weather_ == 3;
    const size_t n = snow ? 900 : (weather_ == 2 ? 1100 : 600);
    const float R = (snow ? 11.0f : 13.0f) * std::sqrt(s), top = 7.0f * std::sqrt(s), bottom = 3.0f * std::sqrt(s);
    const Vec3 wind = snow ? Vec3{0.5f, 0, 0.25f} : (weather_ == 2 ? Vec3{3.0f, 0, 1.4f} : Vec3{1.0f, 0, 0.5f});
    auto spawn = [&](Drop& d, bool anywhere) {
        const float a = frand() * 6.2832f, r = R * std::sqrt(frand());
        d.pos = Vec3{eye.x + std::cos(a) * r, eye.y + (anywhere ? -bottom + frand() * (top + bottom) : top * (0.7f + 0.3f * frand())), eye.z + std::sin(a) * r};
        d.speed = snow ? 0.9f + 0.5f * frand() : (weather_ == 2 ? 11.0f : 8.5f) + 2.0f * frand();
        d.len = snow ? 0.05f + 0.035f * frand() : 0.35f + 0.25f * frand();
        d.phase = frand() * 6.2832f;
    };
    if (drops_.size() != n) {
        drops_.resize(n);
        for (auto& d : drops_) spawn(d, true);
    }
    auto ground = [&](const Vec3& p) {
        float g = player_.groundHeight ? player_.groundHeight(p.x, p.z) : -1e9f;
        return std::max(g, player_.seaLevel);
    };
    for (auto& d : drops_) {
        Vec3 v{wind.x, -d.speed, wind.z};
        if (snow) v = v + Vec3{std::sin(float(time_) * 0.9f + d.phase) * 0.35f, 0, std::cos(float(time_) * 0.7f + d.phase) * 0.35f};
        d.pos = d.pos + v * (dt * (snow ? 1.0f : std::sqrt(s)));
        // Kept round the player: wrapped across the cylinder as you move.
        const Vec3 h{d.pos.x - eye.x, 0, d.pos.z - eye.z};
        const float hl = length(h);
        if (hl > R) d.pos = Vec3{eye.x - h.x * (0.98f * R / hl) * 0.999f, d.pos.y, eye.z - h.z * (0.98f * R / hl) * 0.999f};
        if (d.pos.y < eye.y - bottom || d.pos.y < ground(d.pos)) {
            if (!snow && dropSplashes_.size() < 160 && length(d.pos - eye) < 7.0f * std::sqrt(s) && d.pos.y > eye.y - bottom)
                dropSplashes_.push_back({Vec3{d.pos.x, ground(d.pos) + 0.02f, d.pos.z}, 0.0f});
            spawn(d, false);
        }
    }
    for (auto& sp : dropSplashes_) sp.age += dt;
    dropSplashes_.erase(std::remove_if(dropSplashes_.begin(), dropSplashes_.end(), [](const DropSplash& sp) { return sp.age > 0.12f; }),
                        dropSplashes_.end());
}

void GameApp::renderWeather() {
    if (renderer_.inReflectionPass()) return;
    // The bolt itself while the sky flickers.
    if (lightningT_ >= 0 && lightningT_ < 0.35f && weather_ == 2) {
        if (!boltLoaded_ && assetWorld_ && assetWorld_->models()) {
            boltLoaded_ = true;
            strikeModel_ = assetWorld_->models()->get("EFX_lightning_1.gbs");
            if (!strikeModel_) LOGW("Weather: lightning bolt model unavailable");
        }
        if (strikeModel_) {
            const float t = lightningT_;
            const float f = t < 0.06f ? 1.0f : (t < 0.1f ? 0.2f : (t < 0.16f ? 0.9f : std::max(0.0f, 0.7f - (t - 0.16f) * 4.0f)));
            for (const auto& part : strikeModel_->parts) {
                if (part.textureName.find("lightning") == std::string::npos) continue;  // (the card, not its rain box)
                DrawParams dp;
                dp.texture = part.texture;
                dp.blend = BlendMode::Additive;
                dp.unlit = true;
                dp.noFog = true;
                dp.depthWrite = false;
                dp.doubleSided = true;
                dp.tint = {f, f, f * 1.1f, 1.0f};
                renderer_.draw(part.mesh, Mat4::translation(boltPos_) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, boltYaw_)), dp);
            }
        }
    }
    if (drops_.empty()) return;
    if (!weatherTex_) {
        weatherTex_ = glowTexture();
        weatherBatch_.init(renderer_, 1400);
    }
    const Vec3 eye = renderer_.currentView().eyePosition;
    const Quat q = headWorldRot_;
    const Vec3 right = q.rotate({1, 0, 0}), up = q.rotate({0, 1, 0});
    const bool snow = weather_ == 3;
    const EnvironmentParams& env = renderer_.environment();
    // Lit by the sky: brighter by day, dim at night.
    const Vec3 sky = env.ambientColor + env.sunColor * 0.35f;
    const float lum = std::min(1.0f, 0.35f + 0.65f * std::max(sky.x, std::max(sky.y, sky.z)));
    DrawParams dp;
    dp.texture = weatherTex_;
    dp.unlit = true;
    dp.blend = BlendMode::AlphaBlend;
    dp.depthWrite = false;
    dp.doubleSided = true;
    if (weatherBuilt_ == time_) { weatherBatch_.draw(renderer_, dp); return; }  // (built once a frame, drawn in every pass)
    weatherBuilt_ = time_;
    weatherBatch_.clear();
    const float s = std::sqrt(std::max(1.0f, heroScale_));
    for (const auto& d : drops_) {
        if (weatherBatch_.full()) break;
        if (snow) {
            const uint8_t c[4] = {uint8_t(240 * lum), uint8_t(244 * lum), uint8_t(250 * lum), 210};
            weatherBatch_.addBillboard(d.pos, d.len * s, right, up, d.phase, c);
            continue;
        }
        // A streak along its fall, turned to face the eye.
        const Vec3 dir = normalize(Vec3{weather_ == 2 ? 3.0f : 1.0f, -d.speed, weather_ == 2 ? 1.4f : 0.5f});
        const Vec3 a = d.pos, b = d.pos - dir * (d.len * s);
        Vec3 side = cross(dir, normalize(eye - a));
        const float sl = length(side);
        if (sl < 1e-3f) continue;
        side = side * (0.009f * s / sl);
        const Vec3 quad[4] = {b - side, b + side, a + side, a - side};
        const uint8_t c[4] = {uint8_t(185 * lum), uint8_t(195 * lum), uint8_t(210 * lum), uint8_t(weather_ == 2 ? 140 : 110)};
        weatherBatch_.add(quad, c);
    }
    // Splashes where drops land near you: tiny flashes on the ground and the water.
    for (const auto& sp : dropSplashes_) {
        if (weatherBatch_.full()) break;
        const uint8_t c[4] = {uint8_t(200 * lum), uint8_t(210 * lum), uint8_t(225 * lum), uint8_t(140 * (1.0f - sp.age / 0.12f))};
        weatherBatch_.addBillboard(sp.pos, (0.05f + sp.age * 0.6f) * s, right, up, 0.0f, c);
    }
    weatherBatch_.upload(renderer_);
    weatherBatch_.draw(renderer_, dp);
}

void GameApp::applyWeatherLight(float dt) {
    // Rain and snow: an overcast sky - the sun weaker, the haze greyer. A storm: lightning now and
    // then (a flicker of white light over everything), its thunder a moment later.
    const bool on = settings_.ambience >= 2 && weather_ != 0 && playMode_ && !title_ && !cutsceneActive();
    weatherK_ += ((on ? 1.0f : 0.0f) - weatherK_) * std::min(1.0f, dt * 0.5f);
    EnvironmentParams& env = renderer_.environment();
    const WeatherLit before{env.sunColor, env.ambientColor, env.fogColor, env.clearColor, env.skyHorizon, env.skyTint};
    if (weatherK_ > 0.001f) {
        const float sun = weather_ == 2 ? 0.6f : (weather_ == 1 ? 0.68f : 0.8f);
        const float k = weatherK_;
        env.sunColor = env.sunColor * (1.0f - (1.0f - sun) * k);
        const float g = (env.fogColor.x + env.fogColor.y + env.fogColor.z) / 3.0f;
        const float grey = weather_ == 2 ? 0.15f : 0.3f;
        env.fogColor = env.fogColor + (Vec3{g, g, g * 1.05f} - env.fogColor) * (grey * k);
        env.clearColor = env.fogColor;
        env.skyHorizon = env.fogColor;
    }
    if (on && weather_ == 2) {
        lightningNext_ -= dt;
        if (lightningNext_ <= 0) {
            lightningNext_ = 7.0f + 14.0f * frand();
            lightningT_ = 0.0f;
            thunderDelay_ = 0.4f + 2.6f * frand();
            // Where it strikes: far off, somewhere in front; the thunder's delay goes with the distance.
            const Pose head = rig_.toWorld(lastInput_.head);
            Vec3 f = head.orientation.rotate(Vec3{0, 0, -1});
            f.y = 0;
            f = length(f) > 1e-3f ? normalize(f) : Vec3{0, 0, -1};
            const float a = (frand() - 0.5f) * 2.0f, d = 150.0f + 900.0f * thunderDelay_ / 3.0f * 0.2f + 100.0f * frand();
            const Vec3 dir{f.x * std::cos(a) - f.z * std::sin(a), 0, f.x * std::sin(a) + f.z * std::cos(a)};
            boltPos_ = head.position + dir * d;
            float g = player_.seaLevel;
            if (assetWorld_) { float t; if (assetWorld_->terrain().heightAt(boltPos_.x, boltPos_.z, t)) g = std::max(g, t); }
            boltPos_.y = g;
            boltYaw_ = std::atan2(dir.x, dir.z);  // (the card faces back toward you)
            LOGI("Weather: lightning %.0f m away (thunder in %.1f s)", d, thunderDelay_);
        }
    }
    if (lightningT_ >= 0) {
        lightningT_ += dt;
        // Two or three flickers in a quarter of a second, then gone.
        const float t = lightningT_;
        const float f = t < 0.06f ? 1.0f : (t < 0.1f ? 0.25f : (t < 0.16f ? 0.8f : std::max(0.0f, 0.6f - (t - 0.16f) * 4.0f)));
        if (t > 0.4f) lightningT_ = -1.0f;
        env.ambientColor = env.ambientColor + Vec3{0.75f, 0.78f, 0.9f} * f;
        env.skyTint = env.skyTint * (1.0f + 1.6f * f);
        env.fogColor = env.fogColor + Vec3{0.25f, 0.26f, 0.32f} * f;
        env.clearColor = env.fogColor;
        env.skyHorizon = env.fogColor;
    }
    weatherIn_ = before;
    weatherOut_ = WeatherLit{env.sunColor, env.ambientColor, env.fogColor, env.clearColor, env.skyHorizon, env.skyTint};
    weatherLit_ = true;
    if (thunderDelay_ > 0) {
        thunderDelay_ -= dt;
        if (thunderDelay_ <= 0 && audio_) {
            const std::string name = std::string(ambienceName(AmbienceKind::Thunder)) + char('1' + int(frand() * 2.999f));
            audio_->play2D(name, 0.55f + 0.35f * frand(), 0.85f + 0.25f * frand());
        }
    }
}

void GameApp::undoWeatherLight() {
    // Puts back what applyWeatherLight changed last frame - each value only if nothing (a level
    // load) has written it since - so the world's base light is never captured rain-dimmed.
    if (!weatherLit_) return;
    weatherLit_ = false;
    EnvironmentParams& env = renderer_.environment();
    auto same = [](const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
    if (same(env.sunColor, weatherOut_.sun)) env.sunColor = weatherIn_.sun;
    if (same(env.ambientColor, weatherOut_.ambient)) env.ambientColor = weatherIn_.ambient;
    if (same(env.fogColor, weatherOut_.fog)) env.fogColor = weatherIn_.fog;
    if (same(env.clearColor, weatherOut_.clear)) env.clearColor = weatherIn_.clear;
    if (same(env.skyHorizon, weatherOut_.horizon)) env.skyHorizon = weatherIn_.horizon;
    if (same(env.skyTint, weatherOut_.skyTint)) env.skyTint = weatherIn_.skyTint;
}

void GameApp::updateTreeShake(float dt) {
    if (!assetWorld_ || !playMode_) return;
    struct Src { Vec3 p; float r, k; };
    std::vector<Src> src;
    if (hero_ == Hero::Kabuto && player_.horizontalSpeed() > 0.5f)
        src.push_back({player_.position, 1.3f * player_.height, std::min(1.0f, player_.horizontalSpeed() / 3.0f)});
    for (const auto& b : blasts_)
        if (b.age < 0.05f) src.push_back({b.pos, 6.0f + 4.0f * float(b.size), 1.0f});
    static const bool trace = std::getenv("GVR_SHAKE_TRACE") != nullptr;
    if (trace && !src.empty()) {
        int trees = 0;
        float nearest = 1e9f;
        for (const auto& o : assetWorld_->worldObjects())
            if (o.sway != 0) { ++trees; nearest = std::min(nearest, length(Vec3{o.centre.x - src[0].p.x, 0, o.centre.z - src[0].p.z})); }
        static float tt = 0;
        if ((tt += dt) > 1.0f) { tt = 0; LOGI("shake trace: %d swaying trees, nearest %.1f m (reach %.1f)", trees, nearest, src[0].r); }
    }
    if (src.empty() && treesShaking_ == 0) return;
    int shaking = 0;
    for (auto& o : assetWorld_->worldObjects()) {
        if (o.sway == 0) continue;
        if (o.shake > 0) o.shake = std::max(0.0f, o.shake - dt * 0.6f);
        for (const Src& s : src) {
            const float d = length(Vec3{o.centre.x - s.p.x, 0, o.centre.z - s.p.z});
            if (d < s.r) o.shake = std::max(o.shake, s.k * (1.0f - d / s.r));
        }
        shaking += o.shake > 0;
    }
    if (shaking > 0 && treesShaking_ == 0) LOGI("Trees: %d shaking", shaking);
    treesShaking_ = shaking;
}

void GameApp::updateSpeedWind(float dt) {
    if (!audio_) return;
    float speed = length(player_.velocity);
    if (riding_) speed = std::max(speed, std::fabs(ski_.speed));
    speed /= std::max(1.0f, std::sqrt(heroScale_));  // (a giant's stride is not a gale)
    const bool on = playMode_ && !title_ && !dying_ && !cutsceneActive() && !underwater_ && settings_.ambience >= 1;
    const float want = on ? std::min(1.0f, std::max(0.0f, (speed - 6.0f) / 16.0f)) * 0.7f : 0.0f;
    speedWindVol_ += (want - speedWindVol_) * std::min(1.0f, dt * 3.0f);
    if (speedWindVol_ > 0.01f && !speedWind_) speedWind_ = audio_->play2D(ambienceName(AmbienceKind::Wind), 0.0f, 1.35f, true);
    if (speedWind_) {
        audio_->setVolume(speedWind_, speedWindVol_);
        if (speedWindVol_ <= 0.01f && want <= 0) { audio_->stop(speedWind_); speedWind_ = 0; }
    }
}

}  // namespace gvr
