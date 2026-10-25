// Delphi's bows. From the data: weapondefs items 44 IconRBBow (object rp_bow), 45 IconRBRPG (rp_bow_rpg,
// ammo item 55: 35 most, 25 given), 46 IconRBSniper (rp_bow_sniper, ammo 57: 15 / 8); r1_smartiegrab
// hands out 45, r4a 44 45 46. The RPG bow's weapon record is fxb_bow_rpg (40 damage every 1.3 s, a
// small blast), the sniper bow's fxb_bow_sniper (80 every 1.5 s, 2500 speed). The plain bow has no
// effect name to match it by: ours 30 every 1.0 s (the fxb-less bullet records 30 / 46 read the same).
// Sounds by name: SR_RBW*/SR_RPG*/SR_SBW* (PL pull, SH shot), SR_STORB/STORP/STOSB sword -> bow,
// SR_BTOSW bow -> sword. The models: the riser at the origin, the limbs curving back toward the
// archer (+Y, game) to tips at (0, 2.1, +-3.2); no string in the mesh - drawn here.
// Ours, for VR: right stick forward switches sword -> bows -> sword. Draw with the other hand (grab
// near the string, pull back, let go: the arrow flies from your hand through the bow) or hold the
// trigger of the bow hand (third person: always) and let go. Power grows with the draw.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/cutscene.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
struct BowSpec { const char* name; int item; const char* gbs; const char* fx; float dmg, interval; const char* pull; const char* shot; const char* draw; };
const BowSpec kBows[5] = {
    {"BOW", 44, "rp_bow.gbs", nullptr, 30.0f, 1.0f, "sr_rbwpl", "sr_rbwsh", "sr_storb"},
    {"RPG BOW", 45, "rp_bow_rpg.gbs", "fxb_bow_rpg", 40.0f, 1.3f, "sr_rpgpl", "sr_rpgsh", "sr_storp"},
    {"SNIPER BOW", 46, "rp_bow_sniper.gbs", "fxb_bow_sniper", 80.0f, 1.5f, "sr_sbwpl", "sr_sbwsh", "sr_stosb"},
    // Items 47 IconRBHoming (rp_bow_homing; ammo 56) and 48 IconRBPowerup (rp_bow_powerup; ammo 58). No mission
    // hands them out; the Spellshop does ("spells and weapons made just for you", ours: game_app_shop.cpp).
    // Their records by effect: fx_homing (50 every 1.5 s), fxb_bow_screamer (50 every 1.5 s, key 7 = 8: read
    // as the power-up bow's 8-arrow fan - ours spreads 5).
    {"HOMING BOW", 47, "rp_bow_homing.gbs", "fx_homing", 50.0f, 1.5f, "sr_hbwpl", "sr_hbwsh", "sr_stohb"},
    {"POWER-UP BOW", 48, "rp_bow_powerup.gbs", "fxb_bow_screamer", 50.0f, 1.5f, "sr_pbwpl", "sr_pbwsh", "sr_stopb"},
};
}  // namespace

void GameApp::initBows() {
    if (bowsReady_ || !assetWorld_) return;
    bowsReady_ = true;
    ModelCache* mc = assetWorld_->models();
    initArsenal();  // weaponDefs_
    const WeaponDef* hand = weaponDefs_.weaponByFx("fx_gun", 0.2f);
    const float handDmg = hand && hand->damage > 0 ? hand->damage : 12.0f;
    for (int b = 0; b < kNumBows; ++b) {
        Bow& bow = bows_[b];
        const bool have = bow.have;
        const int ammo = bow.ammo;
        bow = Bow{};
        bow.have = have;
        bow.ammo = ammo;
        bow.name = kBows[b].name;
        bow.item = kBows[b].item;
        bow.model = mc->get(kBows[b].gbs, "rp_bow.anm", /*dynamicLit=*/true);
        float dmg = kBows[b].dmg, iv = kBows[b].interval;
        if (const WeaponDef* w = kBows[b].fx ? weaponDefs_.weaponByFx(kBows[b].fx) : nullptr) { dmg = w->damage; iv = w->interval; }
        bow.damage = dmg / handDmg;
        bow.interval = iv;
        if (const ItemDef* it = weaponDefs_.item(bow.item)) {
            if (it->ammoItem >= 0) {
                bow.maxAmmo = it->maxAmmo > 0 ? it->maxAmmo : 30;
                bow.startAmmo = it->ammoPerPickup > 0 ? it->ammoPerPickup : 10;
            }
        } else if (b != kBowPlain) {
            const int mx[5] = {-1, 35, 15, 10, 10}, st[5] = {0, 25, 8, 6, 3};
            bow.maxAmmo = mx[b];
            bow.startAmmo = st[b];
        }
        if (bow.maxAmmo < 0) bow.ammo = -1;
        LOGI("Bow %s: %s, %.2f x handgun damage at full draw, every %.1f s, arrows %s", bow.name, bow.model ? "ok" : "missing", bow.damage,
             bow.interval, bow.maxAmmo < 0 ? "unlimited" : (std::to_string(bow.startAmmo) + " (max " + std::to_string(bow.maxAmmo) + ")").c_str());
    }
    arrowModel_ = mc->get("rparrow.gbs");
    if (std::getenv("GVR_BOWS"))  // test hook: every bow, full
        for (auto& b : bows_)
            if (b.model) { b.have = true; if (b.maxAmmo > 0) b.ammo = b.maxAmmo; }
}

bool GameApp::grantBowItem(uint32_t item) {
    initBows();
    for (auto& b : bows_)
        if (int(item) == b.item && b.model) {
            if (!b.have) LOGI("Got the %s", b.name);
            b.have = true;
            if (b.maxAmmo > 0) b.ammo = std::min(b.maxAmmo, std::max(b.ammo, b.startAmmo));
            return true;
        }
    return false;
}

void GameApp::updateBow(float dt, const InputFrame& in) {
    bowValid_ = false;
    bowLinesShown_ = false;
    if (!playerModel_) return;
    initBows();
    // Her arrows fly as the gun's bolts do (updateWeapon, which runs only for the Meccaryn, moves them there).
    weapon_.update(dt, false, Vec3{}, Vec3{0, 0, -1});
    const int gh = rig_.turnHand(), mh = rig_.moveHand();
    bowCooldown_ -= dt;
    static bool hookDone = false;  // test hook: GVR_DELPHI_WEAPON=n starts with bow n in hand
    if (!hookDone && playMode_) {
        hookDone = true;
        if (const char* w = std::getenv("GVR_DELPHI_WEAPON")) delphiWeapon_ = std::max(-1, std::min(int(kDelphiMine), std::atoi(w)));
    }
    // Right stick forward: sword -> each bow she has -> her mines (if she carries any) -> sword.
    const float sy = in.hand[gh].thumbstick.y;
    if (sy < 0.3f) bowSwitchArmed_ = true;
    if (bowSwitchArmed_ && sy > 0.75f && !dying_) {
        bowSwitchArmed_ = false;
        int next = delphiWeapon_;
        for (int k = 0; k < kNumBows + 2; ++k) {
            next = next + 1 > kDelphiMine ? -1 : next + 1;
            if (next < 0 || (next == kDelphiMine && mines_ > 0 && rpMineModel_) || (next < kNumBows && bows_[next].have && bows_[next].model)) break;
        }
        if (next != delphiWeapon_) {
            delphiWeapon_ = next;
            bowDrawing_ = false;
            bowDraw_ = 0;
            sfx2D(next < 0 ? "sr_btosw" : kBows[next].draw, 0.8f);
            haptics_.push_back({gh, 0.5f, 0.06f});
            const Bow* b = next >= 0 && next < kNumBows ? &bows_[next] : nullptr;
            timmyLine_ = next == kDelphiMine ? "MINES (" + std::to_string(mines_) + ") - TRIGGER SETS ONE DOWN"
                       : b ? std::string(b->name) + (b->ammo >= 0 ? " (" + std::to_string(b->ammo) + ")" : "") +
                                 (firstPerson_ ? " - BOW IN YOUR LEFT HAND: GRIP THE STRING WITH YOUR RIGHT, PULL BACK, LET GO" : " - HOLD THE TRIGGER, LET GO")
                           : "SWORD";
            timmyLineT_ = 3.0f;
            LOGI("Delphi: %s", next == kDelphiMine ? "mines" : b ? b->name : "sword");
        }
    }
    if (delphiBow() && !(bows_[delphiWeapon_].have && bows_[delphiWeapon_].model)) delphiWeapon_ = -1;
    if (delphiWeapon_ == kDelphiMine && !(mines_ > 0 && rpMineModel_)) delphiWeapon_ = -1;
    if (!delphiBow() || dying_) { bowDrawing_ = false; return; }
    Bow& bow = bows_[delphiWeapon_];
    // Rework (user: "it shall be held in the left hand and the string pulled by the right hand"): first
    // person, the bow is in the move hand (left; right when left-handed) and the turn hand draws the
    // string - the arrow flies only when a drawn string is let go. (The stick still switches weapons.)
    const int bowHand = firstPerson_ ? mh : gh, drawHand = firstPerson_ ? gh : mh;
    // Where the bow is: first person the bow hand's controller (riser in the fist, limbs vertical,
    // the back of the bow where it points); third person her right hand.
    Vec3 aimDir;
    if (firstPerson_) {
        const ControllerState& c = in.hand[bowHand];
        if (!c.gripPoseValid) return;
        const Pose grip = rig_.toWorld(c.gripPose);
        const Pose aim = c.aimPoseValid ? rig_.toWorld(c.aimPose) : grip;
        const float k = bowScale_;
        bowXf_ = Mat4::translation(grip.position) * Mat4::rotation(aim.orientation) * Mat4::scale({k, k, k});
        aimDir = aim.orientation.rotate(Vec3{0, 0, -1});
    } else {
        Mat4 ref;
        if (!playerModel_->nodeWorld("ref_rhand", ref)) return;
        bowXf_ = bodyXf_ * gameMatrixToRuntime(ref);
        const Vec3 at{bowXf_.m[12], bowXf_.m[13], bowXf_.m[14]};
        aimDir = tpAiming_ ? normalize(aimTarget_ - at) : Quat::fromAxisAngle({0, 1, 0}, player_.yaw).rotate(normalize(Vec3{0, 0.03f, -1}));
    }
    bowValid_ = true;
    auto pt = [&](float gx, float gy, float gz) {
        const Vec3 r = giantsToRuntime(gx, gy, gz);
        const Vec4 w = bowXf_ * Vec4{r.x, r.y, r.z, 1.0f};
        return Vec3{w.x, w.y, w.z};
    };
    bowGrip_ = pt(0, 0, 0);
    bowTips_[0] = pt(0, 2.1f, 3.2f);
    bowTips_[1] = pt(0, 2.1f, -3.2f);
    bowNockRest_ = pt(0, 2.1f, 0);
    bowFwd_ = firstPerson_ ? normalize(bowGrip_ - bowNockRest_) : aimDir;
    const float maxPull = 0.6f;  // m of draw for full power
    bool release = false;
    Vec3 nockAt = bowNockRest_;
    // Drawing by hand (first person): the other hand's grip near the string.
    const ControllerState& oc = in.hand[drawHand];
    const bool ogrip = oc.squeeze > 0.7f && oc.gripPoseValid && firstPerson_;
    const Vec3 ohand = oc.gripPoseValid ? rig_.toWorld(oc.gripPose).position : Vec3{};
    if (ogrip && !bowDrawing_ && !bowGripWas_)
        LOGI("Bow: other hand gripped %.2f m from the string%s", length(ohand - bowNockRest_), length(ohand - bowNockRest_) < 0.25f ? " - drawing" : "");
    bowGripWas_ = ogrip;
    if (!bowDrawing_ && ogrip && length(ohand - bowNockRest_) < 0.25f && bowCooldown_ <= 0) {
        bowDrawing_ = true;
        bowByHand_ = true;
        sfx(kBows[delphiWeapon_].pull, bowNockRest_, 0.7f);
    }
    if (bowDrawing_ && bowByHand_) {
        const float pulled = dot(ohand - bowNockRest_, bowFwd_ * -1.0f);
        bowDraw_ = std::max(0.0f, std::min(1.0f, pulled / maxPull));
        nockAt = bowNockRest_ - bowFwd_ * (bowDraw_ * maxPull);
        if (bowDraw_ > 0.2f && frand() < dt * 20.0f) haptics_.push_back({drawHand, 0.15f + 0.3f * bowDraw_, 0.02f});
        if (!ogrip) release = true;
    }
    // Drawing with the trigger: third person only (first person, the string is drawn by hand).
    const bool trig = in.hand[gh].trigger > 0.6f && !firstPerson_;
    if (!bowDrawing_ && trig && bowCooldown_ <= 0 && !bowTrigWas_) {
        bowDrawing_ = true;
        bowByHand_ = false;
        sfx(kBows[delphiWeapon_].pull, bowNockRest_, 0.7f);
    }
    if (bowDrawing_ && !bowByHand_) {
        bowDraw_ = std::min(1.0f, bowDraw_ + dt / 0.7f);
        nockAt = bowNockRest_ - bowFwd_ * (bowDraw_ * maxPull * 0.7f);
        if (!trig) release = true;
    }
    bowTrigWas_ = trig;
    bowNock_ = nockAt;
    bowLinesShown_ = true;
    if (!release) return;
    bowDrawing_ = false;
    const float draw = bowDraw_;
    bowDraw_ = 0;
    if (draw < 0.15f) return;  // let go without a pull: nothing
    if (bow.ammo == 0) {
        sfx2D("me_outam", 0.6f, 1.3f);
        timmyLine_ = std::string(bow.name) + ": NO ARROWS";
        timmyLineT_ = 2.0f;
        return;
    }
    // The arrow leaves the nock along the bow's line (by hand: from the hand through the riser).
    const Vec3 dir = bowByHand_ ? normalize(bowGrip_ - nockAt) : bowFwd_;
    const Vec3 from = bowGrip_ + dir * 0.15f;
    const float power = 0.35f + 0.65f * draw;
    if (delphiWeapon_ == kBowRpg) {
        Rocket r{from, dir * (25.0f + 35.0f * draw), 0.0f, 7, 0, bow.damage * power};
        rockets_.push_back(r);
    } else if (delphiWeapon_ == kBowHoming) {
        // A homing arrow (kind 8): turns toward the nearest foe ahead of it (updateRockets).
        Rocket r{from, dir * (20.0f + 25.0f * draw), 0.0f, 8, 0, bow.damage * power};
        rockets_.push_back(r);
    } else if (delphiWeapon_ == kBowPowerup) {
        // A fan of five arrows, each a little lighter.
        const Vec3 side = normalize(cross(dir, Vec3{0, 1, 0}));
        weapon_.shotDamage = bow.damage * power * 0.6f;
        weapon_.projectileSpeed = 25.0f + 50.0f * draw;
        weapon_.shotStyle = 3;
        for (int k = -2; k <= 2; ++k) weapon_.spawnShot(from, normalize(dir + side * (0.07f * float(k))));
        weapon_.shotStyle = 0;
        weapon_.shotDamage = 1.0f;
        weapon_.projectileSpeed = 70.0f;
    } else {
        weapon_.shotDamage = bow.damage * power;
        weapon_.projectileSpeed = delphiWeapon_ == kBowSniper ? 40.0f + 160.0f * draw : 25.0f + 50.0f * draw;
        weapon_.range = delphiWeapon_ == kBowSniper ? 600.0f : 250.0f;
        weapon_.shotStyle = 3;
        weapon_.spawnShot(from, dir);
        weapon_.shotStyle = 0;
        weapon_.shotDamage = 1.0f;
        weapon_.projectileSpeed = 70.0f;
        weapon_.range = 250.0f;
    }
    if (bow.ammo > 0) --bow.ammo;
    bowCooldown_ = bow.interval * 0.6f;
    sfx(kBows[delphiWeapon_].shot, from, 1.0f, 0.95f + 0.1f * frand());
    haptics_.push_back({bowHand, 0.4f + 0.5f * draw, 0.08f});
    if (bowByHand_) haptics_.push_back({drawHand, 0.6f, 0.05f});
    LOGI("Arrow from the %s at %.0f%% draw (%s) from (%.1f %.1f %.1f) dir (%.2f %.2f %.2f)", bow.name, double(draw * 100.0f),
         bow.ammo >= 0 ? std::to_string(bow.ammo).c_str() : "unlimited", from.x, from.y, from.z, dir.x, dir.y, dir.z);
}

// The string (tip - nock - tip) and, while drawn, the nocked arrow: thin quads facing the eyes,
// built once per frame.
void GameApp::buildBowLines() {
    if (!bowLinesInit_) { bowLinesInit_ = true; bowLines_.init(renderer_, 4); }
    bowLines_.clear();
    if (bowValid_ && bowLinesShown_ && hero_ == Hero::Delphi && !riding_ && lastInput_.headValid) {
        const Vec3 eye = rig_.toWorld(lastInput_.head).position;
        auto line = [&](const Vec3& a, const Vec3& b, float w, const uint8_t c[4]) {
            const Vec3 d = b - a;
            Vec3 side = cross(d, eye - (a + b) * 0.5f);
            if (length(side) < 1e-6f) return;
            side = normalize(side) * (0.5f * w);
            const Vec3 q[4] = {a - side, b - side, b + side, a + side};
            bowLines_.add(q, c);
        };
        const uint8_t string[4] = {225, 220, 200, 255}, shaft[4] = {150, 110, 70, 255};
        line(bowTips_[0], bowNock_, 0.004f, string);
        line(bowNock_, bowTips_[1], 0.004f, string);
        if (bowDrawing_) line(bowNock_, bowGrip_ + bowFwd_ * 0.12f, 0.008f, shaft);
    }
    bowLines_.upload(renderer_);
}

void GameApp::renderBow() {
    if (!bowValid_ || !delphiBow() || !playMode_ || dying_ || riding_ || hero_ != Hero::Delphi) return;
    drawModel(bows_[delphiWeapon_].model, bowXf_, false);
    if (bowLinesInit_) {
        DrawParams dp;
        dp.unlit = true;
        dp.doubleSided = true;
        bowLines_.draw(renderer_, dp);
    }
}

}  // namespace gvr
