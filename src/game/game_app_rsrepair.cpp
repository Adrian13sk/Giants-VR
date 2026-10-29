// (2026-10-29) Delphi's REPAIR spell: weapondefs item 73 (IconRSRepair, HUD art st_rp_repair) - the Reapers' repair
// as Baz's Repair Gun (item 19) is the Meccaryns'. How it is cast in the original is native code. Ours: with her
// base up (r4a and the battles that build one), the casting hand points at one of her damaged buildings within 50 m
// with the trigger held for half a second; her green fire then runs from the hand to it and mends it at the Repair
// Gun's rate (+50 every 1.5 s) while the trigger is held and the hand stays on a building. Casting it uses up the
// trigger pull (no fire wall on letting go).
#include <cstdlib>

#include "core/log.h"
#include "game/game_app.h"

namespace gvr {

bool GameApp::rsRepairOwned() const { return hero_ == Hero::Delphi && base_.on && base_.reaper; }

void GameApp::updateRsRepair(float dt, const InputFrame& in) {
    const int mh = rig_.moveHand();
    const ControllerState& c = in.hand[mh];
    const bool held = c.trigger > 0.4f;
    if (!held) rsBlocked_ = false;
    bool active = false;
    Vec3 from, d;
    if (rsRepairOwned() && held && !rsBlocked_ && c.aimPoseValid && firstPerson_ && !delphiBow() && !followUid_ && (!spellGestureCast_ || rsBeam_)) {
        const Pose aim = rig_.toWorld(c.aimPose);
        from = aim.position;
        d = aim.orientation.rotate(Vec3{0, 0, -1});
        Vec3 hit;
        int b = bldgInRay(from, d, 50.0f, hit);
        static const bool autoAim = std::getenv("GVR_REPAIR_AUTO") != nullptr;  // (test hook: the nearest damaged building)
        if (autoAim && !(rsBeam_ && b >= 0)) {
            float bd = 1e9f;
            for (size_t i = 0; i < base_.bldgs.size(); ++i) {
                const Base::Bldg& bl = base_.bldgs[i];
                const Vec3 cpos{bl.xf.m[12], bl.xf.m[13], bl.xf.m[14]};
                if (bl.state == 3 && bl.hp < bl.maxHp && length(cpos - from) < bd) { bd = length(cpos - from); b = int(i); }
            }
        }
        bool damaged = b >= 0 && base_.bldgs[size_t(b)].state == 3 && base_.bldgs[size_t(b)].hp < base_.bldgs[size_t(b)].maxHp;
        // (2026-10-29 review) Not through a Reaper / Ripper standing in front of it (she is aiming at him: a fire wall,
        // FOLLOW), and only when the pull starts on the building - a charge begun elsewhere is never taken over.
        if (damaged && !rsBeam_ && !autoAim) {
            float te = 0;
            Vec3 ne;
            int tgt = -1;
            if (weapon_.raycast && weapon_.raycast(from, d, length(hit - from), te, ne, tgt) && tgt >= 0) damaged = false;
        }
        if (damaged && !rsBeam_ && rsAimT_ <= 0 && spellCharge_ > 0.15f) { rsBlocked_ = true; damaged = false; }
        static const bool trace = std::getenv("GVR_SPELL_TRACE") != nullptr;
        if (trace && int(time_ * 2) != int((time_ - dt) * 2))
            LOGI("spell trace: REPAIR building %d%s, aimed %.2f s", b, damaged ? " (damaged)" : "", double(rsAimT_));
        if (rsBeam_) {
            active = b >= 0;
        } else if (damaged) {
            rsAimT_ += dt;
            active = rsAimT_ >= 0.5f;
            if (active) {
                sfx("sr_spfsh", from, 0.8f, 1.2f);
                haptics_.push_back({mh, 0.5f, 0.1f});
                LOGI("Delphi casts REPAIR on building %d (%.0f / %.0f)", b, double(base_.bldgs[size_t(b)].hp), double(base_.bldgs[size_t(b)].maxHp));
            }
        } else {
            rsAimT_ = 0;
        }
    } else {
        rsAimT_ = 0;
    }
    if (active) {
        rsBeam_ = true;
        spellGestureCast_ = true;  // (letting go: no fire wall)
        spellCharge_ = 0;
        updateRepairGun(dt, true, from, d, /*spell=*/true);
    } else if (rsBeam_) {
        rsBeam_ = false;
        updateRepairGun(dt, false, from, d, true);
        repairOn_ = false;
    }
}

}  // namespace gvr
