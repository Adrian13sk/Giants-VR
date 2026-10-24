// The Meccaryn's guns beyond the handgun, read from weapondefs.bin (see assets/weapondefs.h):
//   item 3 shotgun (mc_shotgun_l0; first handed out in 1c, whose maps hold mc_pickup_shells),
//   item 5 machine gun (mc_machinegun_l0), item 6 sniper gun (mc_snipergun_l0; story3_bunker_3,
//   "Your new SNIPER GUN works great from a distance"). The rocket launcher is item 14
//   (mc_launcher_l0; story3_launcher_5). Each gun's damage, rate of fire and projectile speed come
//   from its weapon record, matched by its projectile effect (fx_shotgun_trail, fx_gun every 0.1 s,
//   fx_sniper_bullit), scaled against the handgun's record (fx_gun every 0.2 s) so our handgun's
//   hit stays the unit. Ammo: the item's key 45 (most carried) and 46 (given with it); a shells
//   pickup gives its ammo item's key 46. All key readings are ours (FORMATS.md).
// The missions' item lists are read as what Baz is handed at the start; he keeps what he had.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "assets/mission.h"
#include "audio/mixer.h"
#include "assets/skin_anim.h"
#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
struct GunSpec { const char* name; int item; int sel; const char* gbs; const char* anm; const char* fx; float interval; };
const GunSpec kSpecs[4] = {
    {"shotgun", 3, 4, "MC_shotgun_L0.gbs", "mc_shotgun.anm", "fx_shotgun_trail", -1.0f},
    {"machine gun", 5, 5, "MC_machinegun_L0.gbs", "mc_machinegun.anm", "fx_gun", 0.1f},
    {"sniper gun", 6, 6, "MC_snipergun_L0.gbs", "MC_snipergun.anm", "fx_sniper_bullit", -1.0f},
    {"Millenium mortar", 7, 7, "MC_mortar_L0.gbs", "MC_mortar.anm", "fx_lbomb", -1.0f},  // GText iconmortar
};
}  // namespace

std::vector<uint32_t> GameApp::missionItems(const std::string& mission) const {
    std::vector<uint8_t> d;
    MissionData m;
    if (!assetWorld_ || !assetWorld_->fs().read("wm_" + mission + ".bin", d) || !parseMission(d.data(), d.size(), m)) return {};
    return m.items;
}

void GameApp::initArsenal() {
    if (arsenalReady_ || !assetWorld_) return;
    arsenalReady_ = true;
    const AssetFS& fs = assetWorld_->fs();
    std::vector<uint8_t> d;
    std::string err;
    const bool defs = fs.read("weapondefs.bin", d) && parseWeaponDefs(d.data(), d.size(), weaponDefs_, &err);
    if (!defs) LOGW("weapondefs.bin: %s - the guns use built-in values", err.empty() ? "not found" : err.c_str());
    const WeaponDef* hand = defs ? weaponDefs_.weaponByFx("fx_gun", 0.2f) : nullptr;
    const float handDmg = hand && hand->damage > 0 ? hand->damage : 12.0f;
    ModelCache* mc = assetWorld_->models();
    for (int g = 0; g < kNumGuns; ++g) {
        const GunSpec& s = kSpecs[g];
        Gun& gun = guns_[g];
        const bool have = gun.have;
        const int ammo = gun.ammo;
        gun = Gun{};
        gun.have = have;
        gun.ammo = ammo;
        gun.name = s.name;
        gun.item = s.item;
        gun.sel = s.sel;
        const int layers[kNumGuns] = {kHudShotgun, kHudMachine, kHudSniper, kHudMortar};
        gun.hudLayer = layers[g];
        gun.model = mc->get(s.gbs, s.anm, /*dynamicLit=*/true);
        // Ref_handle / Ref_nozzle01 from the gun's .anm (game units, model frame).
        AnmAnimation a;
        std::vector<uint8_t> b;
        if (fs.read(s.anm, b) && parseAnm(b.data(), b.size(), a)) {
            std::vector<Mat4> wm;
            a.evaluate(0.0f, wm);
            auto at = [&](const char* node, Vec3& out) {
                for (size_t i = 0; i < a.nodes.size() && i < wm.size(); ++i)
                    if (toLower(a.nodes[i].name) == node) { out = {wm[i].m[12], wm[i].m[13], wm[i].m[14]}; return true; }
                return false;
            };
            if (!at("ref_handle", gun.handle) || !at("ref_nozzle01", gun.muzzle)) gun.model = nullptr;
        } else {
            gun.model = nullptr;
        }
        const WeaponDef* w = defs ? weaponDefs_.weaponByFx(s.fx, s.interval) : nullptr;
        if (w) {
            gun.damage = w->damage / handDmg;
            gun.interval = w->interval;
            gun.speed = w->speed * gGameUnitsToMeters;
            gun.range = std::min(600.0f, w->range * gGameUnitsToMeters);
        } else {  // the records' values, should the file be missing
            const float dm[4] = {40, 8, 80, 150}, iv[4] = {1.3f, 0.1f, 1.5f, 3.0f}, sp[4] = {900, 1500, 2500, 0}, rg[4] = {600, 600, 8000, 200};
            gun.damage = dm[g] / handDmg;
            gun.interval = iv[g];
            gun.speed = sp[g] * gGameUnitsToMeters;
            gun.range = std::min(600.0f, rg[g] * gGameUnitsToMeters);
        }
        if (const ItemDef* it = defs ? weaponDefs_.item(s.item) : nullptr) {
            if (it->maxAmmo > 0) gun.maxAmmo = it->maxAmmo;
            if (it->ammoPerPickup > 0) gun.startAmmo = it->ammoPerPickup;
            const ItemDef* am = it->ammoItem >= 0 ? weaponDefs_.item(it->ammoItem) : nullptr;
            if (am && am->ammoPerPickup > 0) gun.pickupAmmo = am->ammoPerPickup;
        } else {
            const int mx[4] = {35, 2500, 12, 10}, st[4] = {25, 800, 8, 6}, pk[4] = {2, 300, 3, 2};
            gun.maxAmmo = mx[g];
            gun.startAmmo = st[g];
            gun.pickupAmmo = pk[g];
        }
        if (gun.model)
            LOGI("Gun %s: %.2f x handgun damage, every %.2f s, %.0f m/s, %.0f m; ammo %d (max %d, pickup %d); model %.2f x %.2f x %.2f",
                 gun.name, gun.damage, gun.interval, gun.speed, gun.range, gun.startAmmo, gun.maxAmmo, gun.pickupAmmo,
                 gun.model->boundsMax.x - gun.model->boundsMin.x, gun.model->boundsMax.y - gun.model->boundsMin.y,
                 gun.model->boundsMax.z - gun.model->boundsMin.z);
        else
            LOGW("Gun %s: model or reference nodes missing - not available", gun.name);
    }
    shellsModel_ = mc->get("mc_pickup_shells.gbs", "", /*dynamicLit=*/true);
    lbombModel_ = mc->get("mc_lbomb.gbs", "MC_lbomb_spin2.anm", /*dynamicLit=*/true);
    grenadeModel_ = mc->get("MC_grenade_L0.gbs", "", /*dynamicLit=*/true);
    if (const ItemDef* it = defs ? weaponDefs_.item(8) : nullptr) {  // IconGrenade: most carried, given
        if (it->maxAmmo > 0) grenadeMax_ = it->maxAmmo;
        if (it->ammoPerPickup > 0) grenadeGive_ = it->ammoPerPickup;
    }
    if (std::getenv("GVR_ARSENAL"))  // test hook: every gun, full
        for (auto& g : guns_)
            if (g.model) { g.have = true; g.ammo = g.maxAmmo; }
    if (std::getenv("GVR_ARSENAL") && grenadeModel_) grenades_ = grenadeMax_;
}

void GameApp::resetArsenal() {
    for (auto& g : guns_) { g.have = false; g.ammo = 0; }
    grenades_ = 0;
    hasLauncher_ = false;
    weaponSel_ = 0;
}

void GameApp::grantItem(uint32_t item) {
    if (item == 14 && launcherModel_) { hasLauncher_ = true; return; }
    if (item == 8 && grenadeModel_) {  // grenades: topped up to what the item comes with
        grenades_ = std::min(grenadeMax_, std::max(grenades_, grenadeGive_));
        return;
    }
    for (auto& g : guns_)
        if (int(item) == g.item && g.model) {
            // Handed out: carried from now on, with at least the ammo it comes with.
            if (!g.have) LOGI("Got the %s (%d rounds)", g.name, g.startAmmo);
            g.have = true;
            g.ammo = std::min(g.maxAmmo, std::max(g.ammo, g.startAmmo));
        }
}

void GameApp::grantMissionItems(const std::vector<uint32_t>& items, const char* why) {
    initArsenal();
    for (uint32_t i : items) grantItem(i);
    std::string list = grenades_ > 0 ? " grenades/" + std::to_string(grenades_) : std::string();
    for (const auto& g : guns_)
        if (g.have) list += std::string(" ") + g.name + "/" + std::to_string(g.ammo);
    LOGI("Arsenal (%s):%s%s", why, list.empty() ? " handgun only" : list.c_str(), hasLauncher_ ? " + rocket launcher" : "");
}

int GameApp::gunOfSel(int sel) const {
    for (int g = 0; g < kNumGuns; ++g)
        if (guns_[g].sel == sel) return g;
    return -1;
}

bool GameApp::weaponOwned(int sel) const {
    switch (sel) {
        case 0: return true;
        case 1: return hasLauncher_ && launcherModel_;
        case 2: return hasBush();
        case 3: return popupBombs_ > 0;
        case 8: return grenades_ > 0 && grenadeModel_;
        default: {
            const int g = gunOfSel(sel);
            return g >= 0 && guns_[g].have && guns_[g].model;
        }
    }
}

const RenderModel* GameApp::heldModel() const {
    if (weaponSel_ == 1 && launcherModel_) return launcherModel_;
    const int g = gunOfSel(weaponSel_);
    if (g >= 0 && guns_[g].model) return guns_[g].model;
    return gunModel_;
}

void GameApp::heldRefs(Vec3& handle, Vec3& muzzle) const {
    const int g = gunOfSel(weaponSel_);
    if (weaponSel_ == 1 && launcherModel_) { handle = launcherHandle_; muzzle = launcherMuzzle_; }
    else if (g >= 0 && guns_[g].model) { handle = guns_[g].handle; muzzle = guns_[g].muzzle; }
    else { handle = gunHandle_; muzzle = gunMuzzle_; }
}

bool GameApp::fireGun(float dt, bool trigger, bool valid, const Vec3& muzzle, const Vec3& fwd) {
    const int g = gunOfSel(weaponSel_);
    gunCooldown_ -= dt;
    if (g < 0 || !guns_[g].model) {
        if (mgLoop_ && audio_) { audio_->stop(mgLoop_); mgLoop_ = 0; }
        gunTriggerHeld_ = trigger;
        return false;
    }
    Gun& gun = guns_[g];
    const int gh = rig_.turnHand();
    const bool pull = trigger && !gunTriggerHeld_;
    gunTriggerHeld_ = trigger;
    if (pull && gun.ammo <= 0) {
        sfx2D("me_outam", 0.8f);  // out of ammo (name read as OUT AMmo)
        haptics_.push_back({gh, 0.2f, 0.03f});
    }
    if (g == kGunMortar) {
        // The Millenium mortar lobs a bomb (mc_lbomb, weapon 1: every 3 s, range 200 units = 40 m): it
        // bounces once (fx_lbomb_bounce) and bursts on its next landing.
        weapon_.update(dt, false, muzzle, fwd);
        if (pull && valid && gun.ammo > 0 && gunCooldown_ <= 0) {
            Rocket r{muzzle, fwd * 20.0f + Vec3{0, 2.0f, 0}, 0.0f, 5};
            rockets_.push_back(r);
            --gun.ammo;
            gunCooldown_ = gun.interval;
            haptics_.push_back({gh, 1.0f, 0.15f});
            sfx("me_mtrsh", muzzle, 1.0f, 0.95f + 0.1f * frand());
            smokePuff(FxTex::Smoke, muzzle + fwd * 0.3f, fwd * 1.0f + Vec3{0, 0.5f, 0}, 0.3f, 1.4f, 1.0f, 0.5f, 0.2f);
            LOGI("Millenium bomb lobbed (%d left)", gun.ammo);
        }
        return true;
    }
    if (g == kGunShotgun) {
        // One shell per pull: it flies and bursts where it lands (mc_shotgun_shell, fx_explosion_shotgun).
        weapon_.update(dt, false, muzzle, fwd);
        if (pull && valid && gun.ammo > 0 && gunCooldown_ <= 0) {
            Rocket r{muzzle, fwd * std::min(gun.speed, 70.0f), 0.0f, 4};
            rockets_.push_back(r);
            --gun.ammo;
            gunCooldown_ = gun.interval;
            haptics_.push_back({gh, 0.8f, 0.1f});
            sfx("me_rsgsh", muzzle, 1.0f, 0.95f + 0.1f * frand());
            LOGI("Shotgun shell fired (%d left)", gun.ammo);
            smokePuff(FxTex::Smoke, muzzle + fwd * 0.2f, fwd * 1.5f, 0.15f, 0.9f, 0.6f, 0.5f, 0.15f);
        }
        return true;
    }
    // Machine gun (held) and sniper gun (one per pull): bolts with the record's damage and speed.
    weapon_.fireInterval = gun.interval;
    weapon_.projectileSpeed = std::max(70.0f, gun.speed);
    weapon_.range = gun.range;
    weapon_.shotDamage = gun.damage;
    const bool want = valid && gun.ammo > 0 && (g == kGunMachine ? trigger : pull);
    if (weapon_.update(dt, want, muzzle, fwd)) {
        --gun.ammo;
        if (g == kGunSniper) {
            haptics_.push_back({gh, 0.9f, 0.1f});
            sfx("me_sprsh", muzzle, 1.0f, 0.97f + 0.06f * frand());
        } else {
            haptics_.push_back({gh, 0.3f, 0.03f});
            if (!mgLoop_ && audio_) {
                sfx("me_mchst", muzzle, 0.8f);
                mgLoop_ = audio_->play2D("me_mchlp", 0.6f, 1.0f, true);
            }
        }
    }
    if (g == kGunMachine && mgLoop_ && audio_ && !(trigger && gun.ammo > 0 && valid)) {
        audio_->stop(mgLoop_);
        mgLoop_ = 0;
        sfx("me_mchen", muzzle, 0.8f);
    }
    // Back to the handgun's for its own shots.
    weapon_.fireInterval = 0.2f;
    weapon_.projectileSpeed = 70.0f;
    weapon_.range = 250.0f;
    weapon_.shotDamage = 1.0f;
    return true;
}

// Placed ammo pickups (mc_pickup_shells, type 647: the shotgun's) become live pickups.
void GameApp::ammoPickup(int objectType, const Vec3& at) {
    initArsenal();
    for (int g = 0; g < kNumGuns; ++g) {
        const ItemDef* it = weaponDefs_.item(guns_[g].item);
        const ItemDef* am = it && it->ammoItem >= 0 ? weaponDefs_.item(it->ammoItem) : nullptr;
        if (am && am->pickupType == objectType) {
            ammoPickups_.push_back({at, g, guns_[g].pickupAmmo, 0});
            return;
        }
    }
    if (objectType == 647) ammoPickups_.push_back({at, kGunShotgun, guns_[kGunShotgun].pickupAmmo, 0});  // without the file
}

void GameApp::updateAmmoPickups(float dt) {
    for (auto& p : ammoPickups_) {
        p.t += dt;
        const Vec3 d = p.pos - player_.position;
        Gun& g = guns_[p.gun];
        if (hero_ == Hero::Mecc && g.have && g.ammo < g.maxAmmo && d.x * d.x + d.z * d.z < 1.4f * 1.4f && std::fabs(d.y) < 2.2f) {
            g.ammo = std::min(g.maxAmmo, g.ammo + p.amount);
            p.t = -1e9f;
            pickupFlash_ = 1.0f;
            sfx2D("pu_ammo", 1.0f);
            haptics_.push_back({rig_.moveHand(), 0.5f, 0.08f});
            LOGI("Picked up %d rounds for the %s (%d)", p.amount, g.name, g.ammo);
        }
    }
    ammoPickups_.erase(std::remove_if(ammoPickups_.begin(), ammoPickups_.end(), [](const AmmoPickup& p) { return p.t < -1e8f; }),
                       ammoPickups_.end());
}

void GameApp::renderAmmoPickups() {
    if (!playMode_ || !shellsModel_) return;
    const Vec3 s = shellsModel_->boundsMax - shellsModel_->boundsMin;
    const float big = std::max(s.x, std::max(s.y, s.z));
    const float k = big > 1e-3f ? 0.5f / big : 1.0f;  // about half a metre: easy to spot
    for (const auto& p : ammoPickups_) {
        const float bob = 0.4f + 0.06f * std::sin(p.t * 2.5f);
        const Mat4 at = Mat4::translation(p.pos + Vec3{0, bob, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, p.t * 1.6f));
        drawModel(shellsModel_, at * Mat4::scale({k, k, k}) * Mat4::translation((shellsModel_->boundsMin + shellsModel_->boundsMax) * -0.5f), false);
    }
}

}  // namespace gvr
