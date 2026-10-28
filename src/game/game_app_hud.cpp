// GameApp: the HUD (original Meccaryn HUD textures) and the health injections. Split from
// game_app.cpp; same class.
#include "game/game_app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/arm_ik.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

// ---------------------------------------------------------------------------------------------
// HUD

void GameApp::initHud(ModelCache* mc) {
    if (hudReady_) return;
    // Layer -> texture (drawn in layer order, over everything).
    const char* names[kHudLayers] = {nullptr,          "st_mec_hbar",    "st_mec_fuel",     "st_mec_energy",
                                     "st_mec_jetpack", "st_mec_wpslot",  "st_mec_gun",      "st_mec_launcher",
                                     "st_rp_sword",    "st_rp_firewall", "st_mec_syringe",  "st_vimpmeat",     "st_mec_flare",   "st_mec_bush",    "st_mec_numbers", nullptr,
                                     "st_mec_pop",   // the pop-up bomb (w_menus.gzp)
                                     "st_mec_shotgun", "st_mec_machine", "st_mec_sniper", "st_mec_mortar", "st_mec_grenade", "st_mec_mine",
                                     "i_repair",        // (the Repair Gun's inventory icon: no st_ art for it)
                                     "i_mecc_turret"};  // (the portable turret's)
    const size_t caps[kHudLayers] = {10, 160, 160, 4, 4, 8, 8, 8, 4, 4, 8, 4, 4, 4, 64, 48, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    for (int k = 0; k < kHudLayers; ++k) {
        hudTex_[k] = names[k] ? mc->texture(names[k]) : whiteTex_;
        if (!hudTex_[k]) hudTex_[k] = whiteTex_;
        hudBatch_[k].init(renderer_, caps[k]);
    }
    {
        // The backing plates (layer 0): a soft-edged rounded rectangle rather than a hard-edged
        // box (rework; it read like a debug panel at the wrist).
        const int N = 64;
        std::vector<uint8_t> px(size_t(N) * N * 4, 255);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const float dx = std::max(0.0f, std::fabs(float(x) + 0.5f - N * 0.5f) - (N * 0.5f - 14.0f));
                const float dy = std::max(0.0f, std::fabs(float(y) + 0.5f - N * 0.5f) - (N * 0.5f - 14.0f));
                const float d = std::sqrt(dx * dx + dy * dy);  // distance into the 14-px rounded border
                px[size_t(y * N + x) * 4 + 3] = uint8_t(255.0f * std::clamp((13.0f - d) / 9.0f, 0.0f, 1.0f));
            }
        if (const TextureId t = renderer_.uploadTexture(N, N, px.data(), true, false)) hudTex_[0] = t;
    }
    // The gauges' insides, measured from the outline art: the heart's lobes join below the cusp
    // (seed in the middle), the jetpack arrow's head opens into its stem (seed in the stem).
    Image img;
    if (mc->image("st_mec_energy", img)) heartShape_ = gaugeInterior(img, img.width / 2, img.height / 2);
    if (mc->image("st_mec_jetpack", img)) jetShape_ = gaugeInterior(img, img.width * 31 / 64, img.height / 2);
    LOGI("HUD: heart gauge %zu spans, jetpack gauge %zu spans", heartShape_.size(), jetShape_.size());
    // The digit strip is proportional (st_mec_numbers: "0" 14 px wide, "1" 8 px...): the cells are
    // measured from the empty columns between the glyphs; equal tenths cut glyphs in half.
    for (int d = 0; d <= 10; ++d) digitU_[d] = float(d) / 10.0f;
    if (mc->image("st_mec_numbers", img) && img.width > 10) {
        std::vector<int> starts, ends;
        bool in = false;
        for (int x = 0; x < img.width; ++x) {
            bool any = false;
            for (int y = 0; y < img.height && !any; ++y) any = img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4 + 3] > 40;
            if (any && !in) starts.push_back(x);
            if (!any && in) ends.push_back(x);
            in = any;
        }
        if (in) ends.push_back(img.width);
        if (starts.size() == 10 && ends.size() == 10) {
            for (int d = 0; d < 10; ++d) { digitU0_[d] = float(starts[size_t(d)]) / float(img.width); digitU1_[d] = float(ends[size_t(d)]) / float(img.width); }
            digitsMeasured_ = true;
        }
        LOGI("HUD digits: %zu glyphs measured", starts.size());
    }
    hudReady_ = true;
}

void GameApp::hudRect(int layer, const HudFrame& f, float x, float y, float w, float h, const uint8_t rgba[4], float u0, float v0,
                      float u1, float v1) {
    const Vec3 c = f.o + f.r * x + f.u * y, hr = f.r * (0.5f * w), hu = f.u * (0.5f * h);
    const Vec3 q[4] = {c - hr - hu, c + hr - hu, c + hr + hu, c - hr + hu};
    const float uv[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
    hudBatch_[layer].add(q, uv, rgba);
}

// A gauge filled from the bottom to `level` (0..1): the inside spans below the level with the
// fill texture's lit half, then the outline over it.
void GameApp::hudGauge(int fillLayer, int outlineLayer, const std::vector<GaugeSpan>& shape, const HudFrame& f, float x, float y,
                       float w, float h, float level, const uint8_t fill[4], const uint8_t outline[4]) {
    if (!shape.empty() && level > 0.0f) {
        float top = 1.0f, bottom = 0.0f;
        for (const auto& s : shape) { top = std::min(top, s.v0); bottom = std::max(bottom, s.v1); }
        const float cut = bottom - std::min(1.0f, level) * (bottom - top);
        for (const auto& s : shape) {
            if (s.v1 <= cut) continue;
            const float v0 = std::max(s.v0, cut), v1 = s.v1;
            const float cy = y + (0.5f - 0.5f * (v0 + v1)) * h, cx = x + (0.5f * (s.x0 + s.x1) - 0.5f) * w;
            // The fill textures: transparent / dark top half, the coloured gradient below.
            const float t0 = (v0 - top) / (bottom - top), t1 = (v1 - top) / (bottom - top);
            hudRect(fillLayer, f, cx, cy, (s.x1 - s.x0) * w, (v1 - v0) * h, fill, 0.2f, 0.52f + 0.46f * t0, 0.8f,
                    0.52f + 0.46f * t1);
        }
    }
    hudRect(outlineLayer, f, x, y, w, h, outline);
}

void GameApp::hudDigits(const HudFrame& f, float x, float y, float h, int value, const uint8_t rgba[4]) {
    // st_mec_numbers: "0123456789" in ten equal cells across the strip (left-aligned at x).
    const std::string s = std::to_string(std::max(0, value));
    float cx = x;
    for (size_t i = 0; i < s.size(); ++i) {
        const int d = s[i] - '0';
        const float u0 = digitsMeasured_ ? digitU0_[d] : float(d) / 10.0f, u1 = digitsMeasured_ ? digitU1_[d] : float(d + 1) / 10.0f;
        // Glyph width from its share of the 128 x 32 strip (aspect 4:1), a little gap between.
        const float w = h * (u1 - u0) * 4.0f;
        hudRect(kHudNumbers, f, cx + 0.5f * w, y, w, h, rgba, u0, 0.0f, u1, 1.0f);
        cx += w + h * 0.08f;
    }
}

// The status panel, laid out in metres (FP size; TP scales the frame).
void GameApp::hudStatus(const HudFrame& f) {
    const float t = float(time_);
    const float hp = std::max(0.0f, playerHealth_) / 100.0f;
    const uint8_t plate[4] = {0, 0, 0, 80};
    hudRect(kHudPlate, f, 0.006f, 0.006f, 0.118f, 0.122f, plate);
    // Health: the heart, pulsing red when low, flashing green while an injection heals.
    const bool low = hp < 0.3f;
    const float pulse = low ? 0.5f + 0.5f * std::sin(t * 9.0f) : 0.0f;
    uint8_t fill[4] = {255, 255, 255, 235};
    if (healFlash_ > 0) { fill[0] = uint8_t(255 - 180 * std::min(1.0f, healFlash_)); fill[2] = fill[0]; }
    uint8_t heart[4] = {255, uint8_t(255 - 150 * pulse), uint8_t(255 - 150 * pulse), 255};
    hudGauge(kHudHealthFill, kHudEnergy, heartShape_, f, -0.034f, 0.0f, 0.030f, 0.084f, hp, fill, heart);
    // Jetpack fuel (once he has it): flashes red while overheated.
    if (player_.jetpackAvailable) {
        uint8_t fuel[4] = {255, 255, 255, 235};
        if (player_.overheated()) { const bool on = std::fmod(t, 0.4f) < 0.2f; fuel[1] = fuel[2] = on ? 60 : 255; }
        const uint8_t outline[4] = {255, 255, 255, 255};
        hudGauge(kHudFuelFill, kHudJetGauge, jetShape_, f, -0.002f, 0.0f, 0.030f, 0.084f, player_.fuel(), fuel, outline);
    }
    if (hero_ == Hero::Kabuto) {
        // His adrenaline in the jetpack gauge's place: red, flashing when ready, draining while on.
        const bool ready = kbAdren_ >= 1.0f && kbAdrenT_ <= 0 && kbGrow_ >= 0.999f;
        const bool on = kbAdrenT_ > 0 || (ready && std::fmod(t, 0.6f) < 0.3f);
        const uint8_t ad[4] = {255, uint8_t(on ? 40 : 120), uint8_t(on ? 30 : 90), 235};
        const uint8_t outline[4] = {255, 200, 200, 255};
        hudGauge(kHudFuelFill, kHudJetGauge, jetShape_, f, -0.002f, 0.0f, 0.030f, 0.084f, kbAdren_, ad, outline);
        if (kbOffspringCharges_ > 0) {  // Offspring to lay
            const uint8_t nc[4] = {255, 230, 120, 255};
            hudDigits(f, 0.046f, 0.012f, 0.018f, kbOffspringCharges_, nc);
        }
    }
    const bool giant = hero_ == Hero::Kabuto;  // the giant: no injections or weapons (he heals by eating)
    // Injections carried: the syringe icon and the count (pops when one is picked up).
    const float pop = 1.0f + 0.5f * std::max(0.0f, pickupFlash_);
    const uint8_t on[4] = {255, 255, 255, 255}, dim[4] = {255, 255, 255, 90};
    if (!giant) hudRect(kHudSyringe, f, 0.032f, 0.018f, 0.030f * pop, 0.030f * pop, syringes_ > 0 ? on : dim);
    const uint8_t num[4] = {255, 255, 255, uint8_t(syringes_ > 0 ? 255 : 110)};
    if (!giant) hudDigits(f, 0.046f, 0.012f, 0.018f, syringes_, num);
    // Flares carried (once there are any), right of the injections.
    if (flares_ > 0 && !giant) {
        const float fp = 1.0f + 0.5f * std::max(0.0f, flareFlash_);
        hudRect(kHudFlare, f, 0.066f, 0.018f, 0.026f * fp, 0.026f * fp, on);
        hudDigits(f, 0.078f, 0.012f, 0.018f, flares_, num);
    }
    // Vimp meat carried (once there is any, or a mission wants it): gold when there's enough.
    bool wantMeat = vimpMeat_ > 0;
    for (const auto& o : objectives_) wantMeat |= o.kind == 4 && !o.done;
    if (wantMeat && !giant) {
        const float mp = 1.0f + 0.5f * std::max(0.0f, meatFlash_);
        const float mx = flares_ > 0 ? 0.034f : 0.0f;  // after the flares
        hudRect(kHudPlate, f, 0.098f + mx, 0.018f, 0.062f, 0.036f, plate);
        hudRect(kHudMeat, f, 0.080f + mx, 0.018f, 0.028f * mp, 0.028f * mp, on);
        const bool enough = vimpMeat_ >= kVimpMeatNeeded;
        const uint8_t mc[4] = {255, uint8_t(enough ? 205 : 255), uint8_t(enough ? 25 : 255), 255};
        hudDigits(f, 0.094f + mx, 0.012f, 0.018f, vimpMeat_, mc);
    }
    // The weapon in hand.
    if (giant) {
    } else if (hero_ == Hero::Delphi) {
        hudRect(delphiWeapon_ == kDelphiMine ? kHudMine : kHudSword, f, 0.040f, -0.024f, 0.036f, 0.036f, on);
        if (delphiWeapon_ == kDelphiMine || (delphiBow() && bows_[delphiWeapon_].ammo >= 0)) {  // arrows / mines left (no bow icon in the data's HUD set)
            const int n = delphiWeapon_ == kDelphiMine ? mines_ : bows_[delphiWeapon_].ammo;
            const uint8_t nc[4] = {255, uint8_t(n > 0 ? 255 : 80), uint8_t(n > 0 ? 255 : 80), 255};
            hudDigits(f, 0.026f, -0.046f, 0.012f, n, nc);
        }
        // Her fire bolt where the jetpack gauge would be: lit when ready, dim while it recharges.
        const float ready = boltCooldown_ <= 0 ? 1.0f : 0.0f;
        const uint8_t fire[4] = {255, 255, 255, uint8_t(ready > 0 ? 255 : 90)};
        hudRect(kHudFire, f, -0.002f, 0.0f, 0.03f, 0.03f, fire);
    } else {
        const bool launcher = weaponSel_ == 1 && launcherModel_;
        const int g = gunOfSel(weaponSel_);
        (void)launcher;
        (void)g;
        hudRect(selHudLayer(weaponSel_), f, 0.040f, -0.024f, 0.036f, 0.036f, on);
        const int rounds = weaponSel_ == 3 || weaponSel_ == 11 ? -1 : selCount(weaponSel_);
        if (rounds >= 0) {  // rounds left, under the weapon
            const uint8_t nc[4] = {255, uint8_t(rounds > 0 ? 255 : 80), uint8_t(rounds > 0 ? 255 : 80), 255};
            hudDigits(f, 0.026f, -0.046f, 0.012f, rounds, nc);
        }
    }
    // Mission objectives along the top: one pip each, gold when done.
    const size_t n = objectives_.size();
    for (size_t i = 0; i < n && n <= 16; ++i) {
        const float px = 0.006f + (float(i) - 0.5f * float(n - 1)) * 0.012f;
        const uint8_t back[4] = {0, 0, 0, 150};
        hudRect(kHudMarks, f, px, 0.058f, 0.009f, 0.009f, back);
        const uint8_t gold[4] = {255, 205, 25, 245}, grey[4] = {128, 128, 128, 160};
        hudRect(kHudMarks, f, px, 0.058f, 0.006f, 0.006f, objectives_[i].done ? gold : grey);
    }
}

// The weapon strip (st_mec_wpslot, four slots) that drops in at the top of the view when the
// weapon is switched, the chosen slot popping, then fades: as the original's weapon select.
void GameApp::hudWeaponStrip() {
    const float t = weaponHudT_;
    if (t > 1.9f || !subInit_) return;
    const float in = std::min(1.0f, t / 0.15f), out = t > 1.4f ? std::max(0.0f, 1.0f - (t - 1.4f) / 0.5f) : 1.0f;
    const float a = in * out;
    const float drop = 0.035f * (1.0f - in) * (1.0f - in);
    HudFrame f;
    f.o = rig_.toWorld(lastInput_.head).position + subRot_.rotate(Vec3{0, 0.17f + drop, -0.9f});
    f.r = subRot_.rotate(Vec3{1, 0, 0});
    f.u = subRot_.rotate(Vec3{0, 1, 0});
    const float W = 0.30f * settings_.hudScale(), H = W / 4.0f, cw = W / 4.0f;
    const uint8_t back[4] = {0, 0, 0, uint8_t(120 * a)}, strip[4] = {255, 255, 255, uint8_t(255 * a)};
    // One cell per thing carried, in the switching order (the injections, not a weapon, after the
    // guns); a count under those that run out. The first four sit on the original strip art
    // (st_mec_wpslot, four slots), more continue on plates.
    struct Cell { int layer; bool chosen; int count; };
    Cell cells[kNumSel + 1];  // (every slot and the injections)
    int n = 0;
    auto add = [&](int layer, bool chosen, int count) { if (n < kNumSel + 1) cells[n++] = {layer, chosen, count}; };
    add(kHudGun, weaponSel_ == 0, -1);
    for (int g = 0; g < kNumGuns; ++g)
        if (guns_[g].have && guns_[g].model) add(guns_[g].hudLayer, weaponSel_ == guns_[g].sel, guns_[g].ammo);
    if (grenades_ > 0) add(kHudGrenade, weaponSel_ == 8, grenades_);
    if (mines_ > 0) add(kHudMine, weaponSel_ == 9, mines_);
    if (hasLauncher_ && launcherModel_) add(kHudLauncher, weaponSel_ == 1, launcherAmmo_ + launcherHoming_);
    if (hasRepairGun_ && repairModel_) add(kHudRepair, weaponSel_ == 10, -1);
    if (weaponOwned(11)) add(kHudTurret, weaponSel_ == 11, turretRounds_);
    if (syringes_ > 0) add(kHudSyringe, false, syringes_);
    if (hasBush()) add(kHudBush, weaponSel_ == 2, -1);
    if (popupBombs_ > 0) add(kHudPop, weaponSel_ == 3, popupBombs_);
    const float x0 = -0.5f * cw * float(n);
    const int onArt = std::min(n, 4);
    hudRect(kHudPlate, f, 0, 0, cw * float(n) + 0.01f, H + 0.01f, back);
    hudRect(kHudWpSlot, f, x0 + 0.5f * cw * float(onArt), 0, cw * float(onArt), H, strip, 0, 0, float(onArt) / 4.0f, 1);
    const float popK = 1.0f + 0.35f * std::max(0.0f, 1.0f - t / 0.3f);
    for (int i = 0; i < n; ++i) {
        const Cell& c = cells[i];
        const float cx = x0 + (float(i) + 0.5f) * cw;
        if (c.chosen) {
            const uint8_t glow[4] = {140, 255, 140, uint8_t(110 * a)};
            hudRect(kHudMarks, f, cx, 0, cw - 0.006f, H - 0.006f, glow);
        }
        const float sz = H * 0.82f * (c.chosen ? popK : 0.85f);
        const uint8_t col[4] = {255, 255, 255, uint8_t((c.chosen ? 255 : 130) * a)};
        hudRect(c.layer, f, cx + 0.004f, 0.004f, sz, sz, col);
        if (c.count >= 0) {
            const uint8_t nc[4] = {255, uint8_t(c.count > 0 ? 255 : 80), uint8_t(c.count > 0 ? 255 : 80), uint8_t(255 * a)};
            hudDigits(f, cx - cw / 2.0f + 0.006f, -H * 0.33f, H * 0.3f, c.count, nc);
        }
    }
}

// The weapon wheel's cells: what he carries, in the switching order.
void GameApp::buildWheelCells() {
    wheelCells_.clear();
    for (int sel : kWeaponOrder)
        if (weaponOwned(sel)) wheelCells_.push_back({sel, selHudLayer(sel), selCount(sel)});
}

int GameApp::selHudLayer(int sel) const {
    const int g = gunOfSel(sel);
    if (g >= 0) return guns_[g].hudLayer;
    switch (sel) {
        case 1: return kHudLauncher;
        case 2: return kHudBush;
        case 3: return kHudPop;
        case 8: return kHudGrenade;
        case 9: return kHudMine;
        case 10: return kHudRepair;
        case 11: return kHudTurret;
        default: return kHudGun;
    }
}

int GameApp::selCount(int sel) const {
    const int g = gunOfSel(sel);
    if (g >= 0) return guns_[g].ammo;
    switch (sel) {
        case 1: return launcherAmmo_ + launcherHoming_;
        case 3: return popupBombs_;
        case 8: return grenades_;
        case 9: return mines_;
        case 11: return turretRounds_;
        default: return -1;
    }
}

// The wheel: a ring of icons (radius 11 cm) round where the gun hand was, the one the hand moves toward lit.
void GameApp::hudWeaponWheel() {
    if (!wheelOpen_ || wheelCells_.empty() || hero_ != Hero::Mecc) return;  // (Delphi's: renderDelphiWheel)
    wheelToWorld();
    HudFrame f;
    f.o = wheelCentre_;
    f.r = wheelR_;
    f.u = wheelU_;
    const uint8_t back[4] = {0, 0, 0, 160};
    hudRect(kHudPlate, f, 0, 0, 0.32f, 0.32f, back);
    // A dot where the hand points the choice (its move since the wheel opened, at the wheel's scale).
    float x = 0, y = 0;
    if (wheelHandMove(lastInput_.hand[rig_.turnHand()], x, y)) {
        const float len = std::sqrt(x * x + y * y);
        if (len > 0.11f) { x *= 0.11f / len; y *= 0.11f / len; }
        const uint8_t dot_[4] = {200, 230, 255, 220};
        hudRect(kHudMarks, f, x, y, 0.012f, 0.012f, dot_);
    }
    const int n = int(wheelCells_.size());
    for (int i = 0; i < n; ++i) {
        const float a = 6.2832f * float(i) / float(n);
        const float cx = 0.11f * std::sin(a), cy = 0.11f * std::cos(a);
        const WheelCell& c = wheelCells_[size_t(i)];
        const bool hot = i == wheelHover_, cur = c.sel == weaponSel_;
        if (hot || cur) {
            const uint8_t glow[4] = {uint8_t(hot ? 140 : 90), 255, uint8_t(hot ? 140 : 90), uint8_t(hot ? 170 : 70)};
            hudRect(kHudMarks, f, cx, cy, 0.062f, 0.062f, glow);
        }
        const float sz = hot ? 0.058f : 0.046f;
        const uint8_t col[4] = {255, 255, 255, uint8_t(hot ? 255 : 190)};
        hudRect(c.layer, f, cx, cy, sz, sz, col);
        if (c.count >= 0) {
            const uint8_t nc[4] = {255, uint8_t(c.count > 0 ? 255 : 80), uint8_t(c.count > 0 ? 255 : 80), 230};
            hudDigits(f, cx - 0.02f, cy - 0.034f, 0.014f, c.count, nc);
        }
    }
}

// A red wedge on the side the last hit came from, around the middle of the view.
void GameApp::hudHitMarker() {
    if (hitDirTimer_ <= 0 || !lastInput_.headValid) return;
    const Pose head = rig_.toWorld(lastInput_.head);
    const Vec3 fwd = head.orientation.rotate(Vec3{0, 0, -1}), right = head.orientation.rotate(Vec3{1, 0, 0}),
               up = head.orientation.rotate(Vec3{0, 1, 0});
    const Vec3 d = hitFrom_ - head.position;
    const float ang = std::atan2(dot(d, right), dot(d, fwd));  // 0 = ahead, +pi/2 = right
    const Vec3 dir = right * std::sin(ang) + up * std::cos(ang), side = right * std::cos(ang) - up * std::sin(ang);
    const Vec3 c = head.position + fwd * 0.6f;
    const float r0 = 0.10f, r1 = 0.15f, w0 = 0.010f, w1 = 0.032f;
    const Vec3 q[4] = {c + dir * r0 - side * w0, c + dir * r0 + side * w0, c + dir * r1 + side * w1, c + dir * r1 - side * w1};
    const uint8_t red[4] = {255, 20, 0, uint8_t(250 * std::min(1.0f, hitDirTimer_))};
    hudBatch_[kHudMarks].add(q, red);
}

void GameApp::buildHud() {
    if (!hudReady_) return;
    for (auto& b : hudBatch_) b.clear();
    const bool show = playMode_ && !cutsceneActive() && !dying_ && load_.phase == LoadPhase::Idle && !menuOpen_;
    if (show) {
        scopeCross();
        const ControllerState& lc = lastInput_.hand[rig_.moveHand()];
        if (firstPerson_ && lc.gripPoseValid) {
            // Floating over the move-hand forearm (behind the hand, so the fingers don't cover
            // it), facing the eyes: raise the wrist to read it.
            const Pose g = rig_.toWorld(lc.gripPose);
            HudFrame f;
            f.o = g.position + g.orientation.rotate(Vec3{0, 0.02f, 0.07f}) + Vec3{0, 0.10f, 0};
            f.r = headWorldRot_.rotate(Vec3{settings_.hudScale(), 0, 0});
            f.u = headWorldRot_.rotate(Vec3{0, settings_.hudScale(), 0});
            hudStatus(f);
        } else if (!firstPerson_ && subInit_) {
            // Third person: head-locked at the lower left of the view.
            const float k = 2.2f * settings_.hudScale();
            HudFrame f;
            f.o = rig_.toWorld(lastInput_.head).position + subRot_.rotate(Vec3{-0.33f, -0.24f, -0.9f});
            f.r = subRot_.rotate(Vec3{k, 0, 0});
            f.u = subRot_.rotate(Vec3{0, k, 0});
            hudStatus(f);
        }
        hudWeaponStrip();
        hudWeaponWheel();
        hudHitMarker();
    }
    if (settings_.showFps && subInit_ && lastInput_.headValid) {
        // Frame rate (smoothed), small, at the top left of the view.
        HudFrame f;
        f.o = rig_.toWorld(lastInput_.head).position + subRot_.rotate(Vec3{-0.3f, 0.26f, -0.9f});
        f.r = subRot_.rotate(Vec3{1, 0, 0});
        f.u = subRot_.rotate(Vec3{0, 1, 0});
        const int fps = int(fpsSmooth_ + 0.5f);
        const uint8_t col[4] = {uint8_t(fps >= 70 ? 120 : 255), uint8_t(fps >= 70 ? 255 : (fps >= 60 ? 220 : 80)), 120, 230};
        const uint8_t back[4] = {0, 0, 0, 150};
        hudRect(kHudPlate, f, 0.035f, 0, 0.085f, 0.06f, back);
        hudDigits(f, 0, 0, 0.05f, fps, col);
    }
    for (auto& b : hudBatch_) b.upload(renderer_);
}

// Draws the HUD batches (built once per frame in buildHud) and the full-view flashes.
void GameApp::renderHud() {
    if (hudReady_) {
        DrawParams dp;
        dp.blend = BlendMode::AlphaBlend;
        dp.unlit = true;
        dp.noFog = true;
        dp.depthWrite = false;
        dp.depthTest = false;
        dp.doubleSided = true;
        for (int k = 0; k < kHudLayers; ++k) {
            dp.texture = hudTex_[k];
            hudBatch_[k].draw(renderer_, dp);
        }
    }
    if (!impactModel_ || impactModel_->parts.empty() || !whiteTex_) return;
    const MeshId quad = impactModel_->parts[0].mesh;  // 0.2 m square in the XY plane
    auto flash = [&](const Vec4& color) {
        DrawParams dp;
        dp.texture = whiteTex_;
        dp.blend = BlendMode::AlphaBlend;
        dp.unlit = true;
        dp.noFog = true;
        dp.depthWrite = false;
        dp.depthTest = false;
        dp.doubleSided = true;
        dp.tint = color;
        const Vec3 fwd = headWorldRot_.rotate(Vec3{0, 0, -1});
        const Vec3 eye = renderer_.currentView().eyePosition;
        renderer_.draw(quad, Mat4::translation(eye + fwd * 0.12f) * Mat4::rotation(headWorldRot_) * Mat4::scale({4, 4, 1}), dp);
    };
    if (damageFlash_ > 0 && settings_.classicLook) flash({1.0f, 0.05f, 0.0f, std::min(0.5f, 0.35f * damageFlash_)});  // (rework: a red rim instead, renderVignette)
    if (healFlash_ > 0) flash({0.2f, 1.0f, 0.3f, std::min(0.2f, 0.18f * healFlash_)});
}

// ---------------------------------------------------------------------------------------------
// Health injections

void GameApp::initPickups(ModelCache* mc) {
    healthPickups_.clear();
    ammoPickups_.clear();
    enemyDead_.clear();
    syringeOut_ = false;
    injectT_ = -1;
    healPending_ = 0;
    pickupModel_ = mc->get("MC_pickup_health.gbs", "", /*dynamicLit=*/true);
    syringeModel_ = mc->get("mc_syringe.gbs", "", /*dynamicLit=*/true);
    if (pickupModel_) {
        const Vec3 s = pickupModel_->boundsMax - pickupModel_->boundsMin;
        const float big = std::max(s.x, std::max(s.y, s.z));
        pickupScale_ = big > 1e-3f ? 0.7f / big : 1.0f;  // about 70 cm: easy to spot
        LOGI("Health pickup model %.2f x %.2f x %.2f m", s.x, s.y, s.z);
    }
    if (syringeModel_) {
        // Its long axis is the needle's; the needle end is the thinner one (fewer vertices there
        // is not known here, so the direction is fixed by the measured model: see the log).
        const Vec3 mn = syringeModel_->boundsMin, mx = syringeModel_->boundsMax, s = mx - mn;
        syringeCentre_ = (mn + mx) * 0.5f;
        if (s.x >= s.y && s.x >= s.z) syringeAxis_ = {1, 0, 0};
        else if (s.y >= s.z) syringeAxis_ = {0, 1, 0};
        else syringeAxis_ = {0, 0, 1};
        const float len = std::max(s.x, std::max(s.y, s.z));
        syringeScale_ = len > 1e-3f ? 0.2f / len : 1.0f;  // a 20 cm syringe in the hand
        LOGI("Syringe model %.2f x %.2f x %.2f m", s.x, s.y, s.z);
    }
    if (!assetWorld_) return;
    auto& objs = assetWorld_->worldObjects();
    if (const char* dump = std::getenv("GVR_DUMP_OBJECTS"))  // test hook: list objects whose name contains this
        for (const auto& o : objs)
            if (o.name.find(dump) != std::string::npos)
                LOGI("object %s at game (%.0f, %.0f, %.0f) r %.1f m yaw-ish %.2f", o.name.c_str(), -o.placement.m[12] / gGameUnitsToMeters,
                     o.placement.m[14] / gGameUnitsToMeters, o.placement.m[13] / gGameUnitsToMeters, o.radius,
                     std::atan2(o.placement.m[8], o.placement.m[10]));
    // Placed health pickups (object class 37, mc_pickup_health) become live pickups.
    const ObjectRegistry& reg = assetWorld_->registry();
    for (auto& o : objs) {
        const ObjectType* t = reg.type(o.type);
        const bool health = o.name.find("pickup_health") != std::string::npos || (t && t->objectClass == 37);
        if (!health || o.hidden) continue;
        o.hidden = true;
        if (o.collision >= 0) assetWorld_->collisionMutable().setEnabled(o.collision, false);
        Vec3 at{o.placement.m[12], o.placement.m[13], o.placement.m[14]};
        float gy;
        if (assetWorld_->terrain().heightAt(at.x, at.z, gy)) at.y = std::max(at.y, gy);
        // Ammo (mc_pickup_shells / _clips / _rockets) is not health: weapondefs' ammo items name
        // these pickups (item 37 shells -> type 647).
        if (o.name.find("pickup_health") == std::string::npos && o.name.find("pickup_") != std::string::npos && ammoPickup(int(o.type), at)) {
            LOGI("Ammo pickup (%s) at game (%.0f, %.0f)", o.name.c_str(), -at.x / gGameUnitsToMeters, at.z / gGameUnitsToMeters);
            continue;
        }
        healthPickups_.push_back({at, 0});
        LOGI("Health pickup at game (%.0f, %.0f)", -at.x / gGameUnitsToMeters, at.z / gGameUnitsToMeters);
    }
    if (const char* n = std::getenv("GVR_SYRINGES")) syringes_ = std::max(0, std::min(kMaxSyringes, std::atoi(n)));  // test hook
    LOGI("Health pickups on the map: %zu, ammo pickups: %zu", healthPickups_.size(), ammoPickups_.size());
}

void GameApp::dropHealth(const Vec3& at) {
    Vec3 p = at;
    float gy;
    if (assetWorld_ && assetWorld_->terrain().heightAt(p.x, p.z, gy)) p.y = std::max(p.y, gy);
    healthPickups_.push_back({p, 0});
}

void GameApp::injectHealth() {
    if (syringes_ <= 0) return;
    --syringes_;
    healPending_ += kSyringeHeal;
    healFlash_ = 1.0f;
    sfx2D("me_syrng", 1.0f);
    for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.7f, 0.15f});
    LOGI("Health injection: %d left, healing %.0f", syringes_, kSyringeHeal);
}

void GameApp::updatePickups(float dt, const InputFrame& in) {
    pickupFlash_ = std::max(0.0f, pickupFlash_ - dt * 3.0f);
    healFlash_ = std::max(0.0f, healFlash_ - dt * 1.5f);
    hitDirTimer_ = std::max(0.0f, hitDirTimer_ - dt);
    weaponHudT_ += dt;
    // An injection heals over a second.
    if (healPending_ > 0 && playerHealth_ > 0) {
        const float h = std::min(healPending_, kSyringeHeal * dt);
        healPending_ -= h;
        playerHealth_ = std::min(100.0f, playerHealth_ + h);
        if (playerHealth_ >= 100.0f) healPending_ = 0;
    }
    // Some of the Reapers killed drop an injection, some ammo for a weapon carried (both ours: 35 %,
    // then 30 % of the rest).
    if (enemyDead_.size() < enemies_.enemies.size()) enemyDead_.resize(enemies_.enemies.size(), 0);
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const bool dead = enemies_.enemies[i].state == EnemyState::Dead;
        if (dead && !enemyDead_[i]) {
            ++recentKills_;
            static const float ammoChance = std::getenv("GVR_AMMO_DROP") ? float(std::atof(std::getenv("GVR_AMMO_DROP"))) : 0.3f;  // test hook
            if (frand() < 0.35f) dropHealth(enemies_.enemies[i].pos + Vec3{0.6f, 0, 0.3f});
            else if (frand() < ammoChance) dropAmmo(enemies_.enemies[i].pos + Vec3{-0.5f, 0, 0.4f});
        }
        enemyDead_[i] = dead;
    }
    // Walk over one to pick it up (if there's room in the inventory).
    for (auto& p : healthPickups_) {
        const float before = p.t;
        p.t += dt;
        // A green shimmer rising from it, to be seen from afar.
        if (std::floor(p.t * 5.0f) != std::floor(before * 5.0f) && length(p.pos - player_.position) < 80.0f)
            smokePuff(FxTex::FireGreen, p.pos + Vec3{0, 0.35f, 0}, Vec3{0, 0.6f, 0}, 0.7f, 0.25f, 0.7f, 0.55f, 0.3f);
        const Vec3 d = p.pos - player_.position;
        if (syringes_ < kMaxSyringes && ((d.x * d.x + d.z * d.z < 1.4f * 1.4f && std::fabs(d.y) < 2.2f) || handReach(p.pos, 0.45f))) {
            ++syringes_;
            p.t = -1e9f;
            pickupFlash_ = 1.0f;
            sfx2D("pu_hlth", 1.0f);
            haptics_.push_back({rig_.moveHand(), 0.5f, 0.08f});
            LOGI("Picked up a health injection (%d)", syringes_);
        }
    }
    healthPickups_.erase(std::remove_if(healthPickups_.begin(), healthPickups_.end(), [](const HealthPickup& p) { return p.t < -1e8f; }),
                         healthPickups_.end());
    updateAmmoPickups(dt);
    // Using one: X (the move hand's lower face button).
    const int mh = rig_.moveHand();
    const bool press = injectEdge_.pressed(in.hand[mh].primary) && hero_ != Hero::Kabuto;  // his X eats
    syringeValid_ = false;
    if (firstPerson_) {
        injectT_ = -1;
        if (press && (syringeOut_ || syringes_ > 0)) {
            syringeOut_ = !syringeOut_;
            syringeOutT_ = 0;
            haptics_.push_back({mh, 0.3f, 0.05f});
        }
        if (syringes_ <= 0) syringeOut_ = false;
        const ControllerState& c = in.hand[mh];
        if (syringeOut_ && syringeModel_ && c.gripPoseValid) {
            syringeOutT_ += dt;
            // In the hand, needle along the controller's pointing direction.
            const Pose aim = c.aimPoseValid ? rig_.toWorld(c.aimPose) : rig_.toWorld(c.gripPose);
            const Pose grip = rig_.toWorld(c.gripPose);
            const Quat orient = aim.orientation * rotationBetween(syringeAxis_, Vec3{0, 0, -1});
            const float k = syringeScale_;
            syringeXf_ = Mat4::translation(grip.position + aim.orientation.rotate(Vec3{0, 0, -0.03f})) * Mat4::rotation(orient) *
                         Mat4::scale({k, k, k}) * Mat4::translation(syringeCentre_ * -1.0f);
            syringeValid_ = true;
            // Push it into the body: the needle tip reaches the chest / belly (below the head,
            // inside the torso's radius).
            const Vec3 tip = grip.position + aim.orientation.rotate(Vec3{0, 0, -0.03f - 0.085f});
            if (in.headValid && syringeOutT_ > 0.25f) {
                const Pose head = rig_.toWorld(in.head);
                const Vec3 hf = head.orientation.rotate(Vec3{0, 0, -1});
                Vec3 flat{hf.x, 0, hf.z};
                flat = length(flat) > 1e-3f ? normalize(flat) : Vec3{0, 0, -1};
                const Vec3 chestTop = head.position + Vec3{0, -0.22f, 0} + flat * 0.04f;
                const float drop = chestTop.y - tip.y;
                const Vec3 dxz{tip.x - chestTop.x, 0, tip.z - chestTop.z};
                // (2026-10-28) ...or into the other forearm (the controller's back end runs along it): a jab in the arm.
                bool arm = false;
                const ControllerState& oc = in.hand[mh == kLeft ? kRight : kLeft];
                if (oc.gripPoseValid) {
                    const Pose og = rig_.toWorld(oc.gripPose);
                    const Vec3 a = og.position, b = og.position + og.orientation.rotate(Vec3{0, 0, 0.26f * rig_.comfort.worldScale});
                    const Vec3 ab = b - a;
                    const float t = std::max(0.0f, std::min(1.0f, dot(tip - a, ab) / std::max(1e-6f, dot(ab, ab))));
                    arm = length(tip - (a + ab * t)) < 0.09f * rig_.comfort.worldScale && t > 0.15f;  // (a forearm ~5 cm round, and some slack)
                    static const bool tr = std::getenv("GVR_SYRINGE_TRACE") != nullptr;
                    static int n = 0;
                    if (tr && n++ % 12 == 0) LOGI("syringe trace: tip (%.2f %.2f %.2f), forearm %.2f m away at %.2f along", tip.x, tip.y, tip.z, length(tip - (a + ab * t)), t);
                }
                if ((drop > 0.0f && drop < 0.55f && length(dxz) < 0.17f) || arm) {
                    if (arm) LOGI("Injection into the forearm");
                    injectHealth();
                    syringeOut_ = false;
                }
            }
        }
    } else {
        syringeOut_ = false;
        // Third person: the Meccaryn's injection animation; the dose goes in half way through.
        if (press && syringes_ > 0 && injectT_ < 0 && player_.onGround && !player_.swimming() && playerModel_) {
            const float dur = std::max(0.8f, playerModel_->animDuration(clip_[int(PlayerAnim::Inject)]));
            player_.startAction(PlayerAnim::Inject, dur);
            LOGI("Injection animation (%.2f s)", dur);
            injectT_ = 0;
            injectHealed_ = false;
        }
        if (injectT_ >= 0) {
            injectT_ += dt;
            if (!player_.inAction()) {
                injectT_ = -1;
            } else {
                if (!injectHealed_ && injectT_ > 0.55f * std::max(0.8f, playerModel_->animDuration(clip_[int(PlayerAnim::Inject)]))) {
                    injectHealed_ = true;
                    injectHealth();
                }
                // The syringe in his left hand meanwhile.
                Mat4 ref;
                if (syringeModel_ && playerModel_->nodeWorld("ref_lhand", ref)) {
                    const Vec4 hp = bodyXf_ * Vec4{-ref.m[12] * gGameUnitsToMeters, ref.m[14] * gGameUnitsToMeters,
                                                   ref.m[13] * gGameUnitsToMeters, 1.0f};
                    const Quat body = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
                    const float k = syringeScale_ * 1.3f;
                    syringeXf_ = Mat4::translation({hp.x, hp.y, hp.z}) * Mat4::rotation(body * rotationBetween(syringeAxis_, Vec3{0, -0.6f, -0.8f})) *
                                 Mat4::scale({k, k, k}) * Mat4::translation(syringeCentre_ * -1.0f);
                    syringeValid_ = true;
                }
            }
        }
    }
}

void GameApp::renderPickups() {
    if (!playMode_) return;
    if (pickupModel_)
        for (const auto& p : healthPickups_) {
            const float bob = 0.45f + 0.07f * std::sin(p.t * 2.5f);
            const Mat4 at = Mat4::translation(p.pos + Vec3{0, bob, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, p.t * 1.6f));
            const Mat4 centre = Mat4::translation((pickupModel_->boundsMin + pickupModel_->boundsMax) * -0.5f);
            const float k = pickupScale_;
            drawModel(pickupModel_, at * Mat4::scale({k, k, k}) * centre, false);
            // A soft green-white glow shell so it can be spotted from a distance.
            drawModel(pickupModel_, at * Mat4::scale(Vec3{k, k, k} * 1.2f) * centre, true, 0.25f + 0.1f * std::sin(p.t * 5.0f));
        }
    if (syringeValid_ && syringeModel_ && !dying_) drawModel(syringeModel_, syringeXf_, false);
    renderAmmoPickups();
}

void GameApp::renderGunCounter() {
    // Delphi: the arrows left by the bow's grip (bows with a count; the plain bow's arrows are endless).
    if (hero_ == Hero::Delphi && firstPerson_ && bowValid_ && delphiBow() && bows_[delphiWeapon_].ammo >= 0 && !menuOpen_ && text_.ready() &&
        lastInput_.headValid) {
        const int n = bows_[delphiWeapon_].ammo;
        const Vec3 eye = rig_.toWorld(lastInput_.head).position;
        const Vec3 at = bowGrip_ + Vec3{0, 0.07f, 0} - bowFwd_ * 0.04f;
        const Vec3 d = eye - at;
        if (length(d) < 1.2f && length(d) > 0.08f) {
            const Quat face = Quat::fromAxisAngle({0, 1, 0}, std::atan2(d.x, d.z)) *
                              Quat::fromAxisAngle({1, 0, 0}, std::asin(std::max(-1.0f, std::min(1.0f, d.y / length(d)))));
            char s[16];
            std::snprintf(s, sizeof s, "%d", n);
            float w = 0, h = 0;
            if (const MeshId tm = text_.mesh(s, 0.02f, 0.2f, &w, &h)) {
                const Mat4 xf = Mat4::translation(at) * Mat4::rotation(face);
                text_.drawPanel(xf, w + 0.012f, h + 0.006f, {0, 0, 0, 0.45f}, true);
                text_.draw(tm, xf * Mat4::translation({0, 0, 0.001f}), n == 0 ? Vec4{1.0f, 0.3f, 0.25f, 1.0f} : Vec4{0.75f, 1.0f, 0.85f, 1.0f}, true);
                static bool said = false;
                if (!said) { said = true; LOGI("Bow counter shown: %d arrows", n); }
            }
        }
        return;
    }
    if (!gunCounterValid_ || !firstPerson_ || hero_ != Hero::Mecc || !gunValid_ || menuOpen_ || !text_.ready() || !lastInput_.headValid) return;
    const bool launcher = weaponSel_ == 1 && launcherModel_;
    const int g = gunOfSel(weaponSel_);
    const int rounds = g >= 0 ? guns_[g].ammo : (launcher ? launcherAmmo_ + launcherHoming_ : -1);
    if (rounds < 0) return;  // (the handgun: no count)
    // On top of the gun model, over the hand that holds it (its handle), a little forward (model -Z is the muzzle).
    const RenderModel* gm = g >= 0 ? guns_[g].model : (launcher ? launcherModel_ : nullptr);
    Vec3 at = gunCounterAt_;
    if (gm) {
        const Vec3 lp{gunHandleLocal_.x, gm->boundsMax.y, gunHandleLocal_.z - 0.08f * (gm->boundsMax.z - gm->boundsMin.z)};
        const float* m = gunXf_.m;
        at = Vec3{m[0] * lp.x + m[4] * lp.y + m[8] * lp.z + m[12], m[1] * lp.x + m[5] * lp.y + m[9] * lp.z + m[13],
                  m[2] * lp.x + m[6] * lp.y + m[10] * lp.z + m[14]};
        at = at + Vec3{0, 0.025f, 0};
    }
    const Vec3 eye = rig_.toWorld(lastInput_.head).position;
    const Vec3 d = eye - at;
    if (length(d) > 1.2f || length(d) < 0.08f) return;
    // Facing the eye, upright.
    const Quat face = Quat::fromAxisAngle({0, 1, 0}, std::atan2(d.x, d.z)) * Quat::fromAxisAngle({1, 0, 0}, std::asin(std::max(-1.0f, std::min(1.0f, d.y / length(d)))));
    char s[16];
    std::snprintf(s, sizeof s, "%d", rounds);
    float w = 0, h = 0;
    const MeshId tm = text_.mesh(s, 0.02f, 0.2f, &w, &h);
    if (!tm) return;
    const Mat4 xf = Mat4::translation(at) * Mat4::rotation(face);
    text_.drawPanel(xf, w + 0.012f, h + 0.006f, {0, 0, 0, 0.45f}, true);
    const Vec4 col = rounds == 0 ? Vec4{1.0f, 0.3f, 0.25f, 1.0f} : (rounds <= 5 ? Vec4{1.0f, 0.8f, 0.3f, 1.0f} : Vec4{0.75f, 1.0f, 0.85f, 1.0f});
    text_.draw(tm, xf * Mat4::translation({0, 0, 0.001f}), col, true);
    static bool said = false;
    if (!said) { said = true; LOGI("Gun counter shown: %d rounds", rounds); }
}

bool GameApp::wristWanted(Vec3& wrist, Vec3& toEye, Vec3& eye) const {
    const int mh = rig_.moveHand();
    const InputFrame& in = lastInput_;
    const ControllerState& c = in.hand[mh];
    bool want = false;
    if (playMode_ && firstPerson_ && hero_ != Hero::Kabuto && c.gripPoseValid && in.headValid && !menuOpen_) {
        const Pose head = rig_.toWorld(in.head), grip = rig_.toWorld(c.gripPose);
        eye = head.position;
        wrist = grip.position;
        const Vec3 hf = head.orientation.rotate(Vec3{0, 0, -1});
        const Vec3 d = wrist - eye;
        const float dist = length(d);
        const Vec3 arm = grip.orientation.rotate(Vec3{0, 0, -1});  // the forearm's line (the controller points along it)
        const float s = rig_.comfort.worldScale > 0 ? 1.0f / rig_.comfort.worldScale : 1.0f;
        // Near the face (15-50 cm), in front of the eyes, below them, the forearm across the view.
        want = dist * s > 0.15f && dist * s < 0.5f && dot(normalize(d), hf) > 0.7f && d.y < 0.05f && std::fabs(dot(arm, hf)) < 0.55f;
        toEye = dist > 1e-4f ? d * (-1.0f / dist) : Vec3{0, 0, 1};
    }
    return want;
}

// (2026-10-28) The hold timer advances in update(), with the frame's step: it was advanced while drawing at a fixed
// 1/72 s - at 36 fps the 0.25 s hold took half a second, and the PC harness (drawing only some frames) missed it.
void GameApp::updateWristPanel(float dt) {
    Vec3 wrist, toEye, eye;
    const bool want = wristWanted(wrist, toEye, eye);
    wristT_ = want ? wristT_ + dt : std::max(0.0f, wristT_ - 3.0f * dt);
    const bool show = wristT_ > 0.25f;
    if (show && !wristShown_) LOGI("Wrist display shown (health %.0f)", playerHealth_);
    wristShown_ = show;
}

void GameApp::renderWristPanel() {
    Vec3 wrist, toEye, eye;
    const bool want = wristWanted(wrist, toEye, eye) && text_.ready();
    const bool show = wristShown_ && want;
    if (!show) return;
    // The lines.
    std::string ammo = "AMMO -";
    if (hero_ == Hero::Mecc) {
        const int g = gunOfSel(weaponSel_);
        if (g >= 0) ammo = std::string(guns_[g].name) + " " + std::to_string(guns_[g].ammo);
        else if (weaponSel_ == 1) ammo = "LAUNCHER " + std::to_string(launcherAmmo_) + (launcherHoming_ > 0 ? " +" + std::to_string(launcherHoming_) + " HOMING" : "");
        else if (weaponSel_ == 8) ammo = "GRENADES " + std::to_string(grenades_);
        else if (weaponSel_ == 9) ammo = "MINES " + std::to_string(mines_);
        else if (weaponSel_ == 10) ammo = "REPAIR GUN";
        else if (weaponSel_ == 2) ammo = "BUSH PACK";
        else if (weaponSel_ == 3) ammo = "POP-UP BOMB " + std::to_string(popupBombs_);
        else ammo = "HANDGUN";
    } else if (hero_ == Hero::Delphi) {
        if (delphiBow()) ammo = std::string(bows_[delphiWeapon_].name) + (bows_[delphiWeapon_].ammo >= 0 ? " " + std::to_string(bows_[delphiWeapon_].ammo) : "");
        else ammo = delphiWeapon_ == kDelphiMine ? "MINES " + std::to_string(mines_) : "SWORD";
    }
    for (char& ch : ammo) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    std::string obj = "NO OBJECTIVE";
    float best = 1e18f;
    for (const auto& o : objectives_) {
        if (o.done) continue;
        Vec3 p = (o.boundsMin + o.boundsMax) * 0.5f;
        if (o.item >= 0 && size_t(o.item) < destruct_.items.size()) p = destruct_.items[size_t(o.item)].base;
        best = std::min(best, length(Vec3{p.x - player_.position.x, 0, p.z - player_.position.z}));
    }
    if (best < 1e17f) obj = "OBJECTIVE " + std::to_string(int(best + 0.5f)) + " M";
    if (missionComplete_) obj = "MISSION COMPLETE";
    const std::string text = "HEALTH " + std::to_string(int(playerHealth_ + 0.5f)) + "\n" + ammo + "\n" +
                             (hero_ == Hero::Mecc ? "INJECTIONS " + std::to_string(syringes_) + "  FLARES " + std::to_string(flares_) + "\n" : std::string()) + obj;
    float w = 0, h = 0;
    const float k = rig_.comfort.worldScale;
    const MeshId m = text_.mesh(text, 0.015f * k, 0.3f * k, &w, &h);
    if (!m) return;
    // Over the wrist, facing the eyes, upright.
    const Vec3 at = wrist + Vec3{0, 0.07f * k, 0} + toEye * (0.02f * k);
    const Vec3 fwd = normalize(Vec3{toEye.x, 0, toEye.z} + Vec3{0, 0, 1e-4f});
    const Quat q = Quat::fromAxisAngle({0, 1, 0}, std::atan2(fwd.x, fwd.z));
    const float fade = std::min(1.0f, (wristT_ - 0.25f) / 0.15f);
    const Mat4 xf = Mat4::translation(at) * Mat4::rotation(q);
    static int dbg = 0;
    if (std::getenv("GVR_WRIST_TRACE") && dbg++ % 30 == 0)
        LOGI("wrist trace: eye (%.2f %.2f %.2f) wrist (%.2f %.2f %.2f) panel (%.2f %.2f %.2f) %.3f x %.3f m, fade %.2f, mesh %u", eye.x, eye.y, eye.z,
             wrist.x, wrist.y, wrist.z, at.x, at.y, at.z, w, h, fade, unsigned(m));
    text_.drawPanel(xf * Mat4::translation({0, 0, -0.002f}), w + 0.02f * k, h + 0.016f * k, {0.02f, 0.04f, 0.05f, 0.75f * fade});
    text_.draw(m, xf, {playerHealth_ < 35.0f ? 1.0f : 0.75f, playerHealth_ < 35.0f ? 0.45f : 0.95f, 0.6f, fade});
}

}  // namespace gvr
