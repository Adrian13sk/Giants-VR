// (2026-10-29) Delphi's TELEPORT: weapondefs item 24 (IconRSTeleport, GText "Teleport"; no HUD art of its own in the
// data's st_rp_ set). How it is cast in the original is native code. Ours, beside FOLLOW: the casting hand points
// at open ground 3-25 m away with the trigger held - steadily for 0.3 s, a blue flame showing the spot - and is
// pulled back toward her as FOLLOW's catch is: she pulls herself there, in a blink (the fade and move of Options ->
// MOVEMENT: TELEPORT). Where FOLLOW has a Reaper in the aim, FOLLOW wins. Only where she could stand (level enough,
// not deep water, room overhead), on foot, not in races; 5 s between. Reaper story 3 on, and practice.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/game_app.h"

namespace gvr {

namespace {
constexpr float kRtMin = 3.0f, kRtMax = 25.0f;  // m
constexpr float kRtCool = 5.0f;
constexpr float kRtPull = 1.2f;                 // m/s, as FOLLOW's catch
}  // namespace

bool GameApp::rsTeleportOwned() const {
    if (hero_ != Hero::Delphi || !assetWorld_) return false;
    const std::string w = toLower(worldName_);
    return practice_ || w.rfind("rstory3", 0) == 0 || w == "rstory4" || w == "rstory5" || std::getenv("GVR_RS_TELEPORT") != nullptr;
}

// Where the aim meets ground she could stand on, within reach.
bool GameApp::rsTeleportAim(const Vec3& from, const Vec3& dir, Vec3& spot) const {
    float t = 0;
    Vec3 n;
    if (!worldRay_ || !worldRay_(from, dir, kRtMax + 2.0f, t, n)) return false;
    spot = from + dir * t;
    const float H = player_.height;
    const float flat = length(Vec3{spot.x - player_.position.x, 0, spot.z - player_.position.z});
    if (n.y < 0.75f || flat < kRtMin || flat > kRtMax) return false;
    if (player_.seaLevel > -1e8f && spot.y < player_.seaLevel - 0.6f * H) return false;  // (deep water)
    float tu = 0;
    Vec3 nu;
    if (worldRay_(spot + Vec3{0, 0.1f, 0}, Vec3{0, 1, 0}, 0.95f * H, tu, nu)) return false;  // (no room to stand)
    return true;
}

bool GameApp::updateRsTeleport(float dt, float pull, float pullDist, const Vec3& from, const Vec3& dir, int hand) {
    if (!rsTeleportOwned() || raceMode_ || riding_ || telPhase_ != 0) {
        rtAimT_ = 0;
        return false;
    }
    Vec3 spot;
    const bool ok = rsTeleportAim(from, dir, spot);
    static const bool trace = std::getenv("GVR_SPELL_TRACE") != nullptr;
    if (trace && int(time_ * 4) != int((time_ - dt) * 4)) {
        float t = 0;
        Vec3 n;
        const bool hit = worldRay_ && worldRay_(from, dir, 60.0f, t, n);
        LOGI("spell trace: TELEPORT aim %s - %s %.1f m, normal y %.2f, aimed %.2f s", ok ? "ok" : "no", hit ? "ground at" : "nothing within", double(t), double(n.y), double(rtAimT_));
    }
    // The spot taken from the steady aim (the pull itself drags the aim off it).
    if (ok && (rtAimT_ <= 0 || length(spot - rtSpot_) < 1.5f)) {
        if (rtAimT_ <= 0) rtSpot_ = spot;
        rtAimT_ += dt;
        rtSpotOk_ = true;
    } else if (pull < 1.0f) {
        rtAimT_ = 0;
        rtSpotOk_ = false;
    }
    if (rtSpotOk_ && rtAimT_ > 0.15f && rtCool_ <= 0 && frand() < dt * 20.0f)
        smokePuff(FxTex::FireBlue, rtSpot_ + Vec3{frand() - 0.5f, 0.1f, frand() - 0.5f} * 0.6f, Vec3{0, 0.8f, 0}, 0.2f, 0.4f, 0.5f, 0.8f, 0.0f);
    if (!rtSpotOk_ || rtAimT_ < 0.3f || pull < kRtPull || pullDist < 0.12f || rtCool_ > 0 || !player_.onGround) return false;
    const float d = length(Vec3{rtSpot_.x - player_.position.x, 0, rtSpot_.z - player_.position.z});
    telPending_ = rtSpot_;
    telPhase_ = 1;  // (the blink: game_app_teleport.cpp)
    spellGestureCast_ = true;  // (letting go: not a fire bolt)
    spellCharge_ = 0;
    rtCool_ = kRtCool;
    rtAimT_ = 0;
    rtSpotOk_ = false;
    endCloak("she cast");
    sfx("sr_spfsh", player_.position + Vec3{0, 1.0f, 0}, 0.9f, 0.7f);
    haptics_.push_back({hand, 0.8f, 0.15f});
    LOGI("Delphi casts TELEPORT: %.1f m (hand pulled at %.1f m/s)", double(d), double(pull));
    return true;
}

}  // namespace gvr
