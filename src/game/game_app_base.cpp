// Base building (5c_securebase, "The Battle!": "1. Gather SMARTIES and VIMP MEAT. 2. BUILD A BASE.").
//
// From the data: the layout is Mecc_base_template_1.anm (xx_mecc_base.gzp) - one node per building
// slot round base_center (command_center, workshop, giftshop, Gate, Wall_A1..C3, Firepit,
// meat_dropoff, Repair01/02, teleport) - and the buildings are the archive's skinned models
// (Wood_pub, Wood_shop, Wood_gift, Fun_house, Wood_gate, Wood_wall, wood_command) with their
// "_build" clips, which assemble the building. Names, the "Build Pad" ("where you stand to direct
// base building"), the Pub ("keep it stocked with Vimp Meat"), the states (Ready To Build /
// Building... / Completed!) and "OUT OF FOOD!!!" are the game's own text (GTextEnglish basebldg*,
// bldgdescr*, basestate*, basehint*).
//
// Ours (the original's base logic is native code, not decoded): the costs in meat, the build
// times, which template slot holds the Pub and the Party House (Repair01/02 - the template has no
// slot by those names), the fixed workforce, the base centre at Borjoyzee's start, and the VR
// menu - a ring of the buildings you can order, floating round you on the Build Pad; touch one
// (hand or walk into it) to start it - or touch its site on the table map, the base in miniature
// to your left on the pad (2026-10-28). Vimp meat you carry is stored when you walk past the meat
// drop-off. Not yet: building collision, enemies attacking the base, upgrades to stone, and the
// POP-UP BOMB the Smarties make once the base is complete.
#include "game/game_app.h"
#include "assets/gtext.h"
#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "assets/skin_anim.h"
#include "core/log.h"
#include "game/cutscene.h"
#include "scene/asset_world.h"
#include "game/arm_ik.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
struct BldgDef {
    const char* key;     // GText basebldg<key>
    const char* label;
    const char* model;   // skinned model in xx_mecc_base
    const char* anim;    // its build clip
    const char* slot;    // template node
    int meat;            // ours
    float seconds;       // ours
    bool core;           // counts toward "base complete"
};
// The Build Pad comes first and stands from the start.
const BldgDef kMecc[] = {
    {"mccomhut", "BUILD PAD", "wood_command", "wood_command.anm", "command_center", 0, 0.0f, false},
    {"pub", "PUB", "Wood_pub", "Wood_pub_build.anm", "Repair01", 4, 18.0f, true},
    {"mcworkshop", "WORKSHOP", "Wood_shop", "wood_shop.anm", "workshop", 6, 20.0f, true},
    {"giftshop1", "GIFTSHOP", "Wood_gift", "wood_gift.anm", "giftshop", 8, 22.0f, true},
    {"mcpartyhouse", "PARTY HOUSE", "Fun_house", "fun_house_build.anm", "Repair02", 10, 25.0f, true},
    {"woodgate", "WOOD GATE", "Wood_gate", "Wood_gate_build.anm", "Gate", 4, 14.0f, false},
    {"woodwalla", "WOOD WALL A", "Wood_wall", "wood_wall_build.anm", "Wall_A1|Wall_A2|Wall_A3", 3, 12.0f, false},
    {"woodwallb", "WOOD WALL B", "Wood_wall", "wood_wall_build.anm", "Wall_B1|Wall_B2", 2, 10.0f, false},
    {"woodwallc", "WOOD WALL C", "Wood_wall", "wood_wall_build.anm", "Wall_C1|Wall_C2|Wall_C3", 3, 12.0f, false},
    // The Bulldog Turret (GTextEnglish iconphmeccturret; s_mecc_turret in xx_mecc_base). The template
    // has no turret slots: two sites are ours (placeTurretSites), and so is what it does.
    {"turret", "BULLDOG TURRET", "s_mecc_turret", "s_mecc_turret.anm", "", 5, 15.0f, false},
    // (2026-10-29) The Gyrocopter Pad (GText basephmeccgyropad / iconphmeccvehlaunch; s_mecc_gpad): on the template's
    // free "teleport" slot (ours). Built, it has a gyrocopter standing on it ("Gyropad ready!") - game_app_gyro.cpp.
    {"gyropad", "GYROCOPTER PAD", "s_mecc_gpad", "s_mecc_gpad.anm", "teleport", 8, 20.0f, false},
};

// Delphi's base (r4a, "Sappho's Fleet"): RP_base_template_1 and xx_reaper_base.gzp. Names from
// GTextEnglish (basebldgrpcomhut "Build Pad", rpworkshop "Spell School", spellshop1, extractor
// "Persuader" - "where your Smarties get magic power ... so long as you keep feeding it souls",
// rppartyhouse, rpgenerator, weakwall "Forcefield", weakgate "Reaper Gate"). The template has no
// Persuader slot: it and the Party House take Repair01/02, the Generator "teleport" (ours).
const BldgDef kReaper[] = {
    {"rpcomhut", "BUILD PAD", "RP_command", "RP_command.anm", "command_center", 0, 0.0f, false},
    {"extractor", "PERSUADER", "Extractor_1", "Extractor_1.anm", "Repair01", 4, 18.0f, true},
    {"rpworkshop", "SPELL SCHOOL", "RP_workshop", "RP_workshop.anm", "rp_workshop", 6, 20.0f, true},
    {"spellshop1", "SPELLSHOP", "spellshop_1", "spellshop_1.anm", "spellshop", 8, 22.0f, true},
    {"rppartyhouse", "PARTY HOUSE", "RP_Fun_house", "RP_Fun_house_build.anm", "Repair02", 10, 25.0f, true},
    {"rpgenerator", "SHIELD GENERATOR", "RP_generator", "RP_generator.anm", "teleport", 6, 18.0f, false},
    {"weakgate", "REAPER GATE", "magic_gate1", "magic_gate1.anm", "Gate", 4, 14.0f, false},
    {"weakwall", "FORCEFIELDS", "magic_wall1", "magic_wall1.anm", "Wall_A1|Wall_B1|Wall_C1", 6, 16.0f, false},
    {"turret", "HELLFIRE TURRET", "s_reaper_turret", "s_reaper_turret.anm", "", 5, 15.0f, false},  // iconphrpturret
};
// The race chosen in initBase.
const BldgDef* kDefs = kMecc;
int kNumDefs = int(sizeof(kMecc) / sizeof(kMecc[0]));
const char* kRes = "VIMP MEAT";

// Second-level buildings (the archives' stone / level-2 models with their own build clips; names
// from GTextEnglish basebldgstonewalla.., giftshop2, spellshop2, magicwall "Forcefield 2", magicgate
// "Reaper Gate 2"). Costs ours; the Pub and Party Houses have no second level in the data.
struct Upgrade { const char* key; const char* label; const char* model; const char* anim; int meat; };
const Upgrade kUpgrades[] = {
    // The Build Pad's own upgrade (basebldgmccomcenter "Comm Center", stone_command); what it does is
    // ours: it warns you where a raid is coming from. (The Reapers' base has no Comm Center model.)
    {"mccomhut", "COMM CENTER", "stone_command", "stone_command.anm", 8},
    {"mcworkshop", "STONE WORKSHOP", "stone_shop", "Stone_shop.anm", 8},
    {"giftshop1", "GIFTSHOP 2", "stone_gift", "stone_gift.anm", 10},
    {"woodgate", "STONE GATE", "Stone_gate", "stone_gate_build.anm", 6},
    {"woodwalla", "STONE WALL A", "Stone_wall", "Stone_wall_build.anm", 5},
    {"woodwallb", "STONE WALL B", "Stone_wall", "Stone_wall_build.anm", 4},
    {"woodwallc", "STONE WALL C", "Stone_wall", "Stone_wall_build.anm", 5},
    {"extractor", "PERSUADER 2", "Extractor_2", "Extractor_2.anm", 8},
    {"spellshop1", "SPELLSHOP 2", "spellshop_2", "spellshop_2.anm", 10},
    {"weakgate", "REAPER GATE 2", "magic_gate2", "magic_gate2.anm", 6},
    {"weakwall", "FORCEFIELDS 2", "magic_wall2", "magic_wall2.anm", 10},
};
const Upgrade* upgradeFor(int d) {
    for (const auto& u : kUpgrades)
        if (std::string(u.key) == kDefs[d].key) return &u;
    return nullptr;
}
const char* bldgLabel(int d, int level) {
    const Upgrade* u = level > 0 ? upgradeFor(d) : nullptr;
    return u ? u->label : kDefs[d].label;
}
const char* bldgModel(int d, int level) {
    const Upgrade* u = level > 0 ? upgradeFor(d) : nullptr;
    return u ? u->model : kDefs[d].model;
}
const char* bldgAnim(int d, int level) {
    const Upgrade* u = level > 0 ? upgradeFor(d) : nullptr;
    return u ? u->anim : kDefs[d].anim;
}
bool isKey(int d, const char* key) { return std::string(kDefs[d].key) == key; }
bool isGate(int d) { return std::string(kDefs[d].key).find("gate") != std::string::npos; }
constexpr float kPadRadius = 4.0f;     // m: standing on the Build Pad
constexpr float kDropRadius = 3.5f;    // m: the meat drop-off

bool baseMission(const std::string& mission, bool reaper) {
    if (const char* f = std::getenv("GVR_BASE")) return *f == '1';
    // The missions whose briefings say "BUILD A BASE": 5c (Meccaryn); r4a and the Reaper battles
    // rstory2_battle_1 / rstory5_battle_2 (Delphi).
    const std::string m = toLower(mission);
    if (!reaper) return m.rfind("5c", 0) == 0;
    return m.rfind("r4a", 0) == 0 || m.rfind("rstory2_battle", 0) == 0 || m.rfind("rstory5_battle", 0) == 0;
}
}  // namespace

// A finished base for a skirmish side (game_app_skirmish.cpp): the race's template buildings standing
// at `at` (its position and yaw), solid. No walls or gates: a closed ring would shut the side's own
// flag holder in (the original opens gates for its side - not done here; ours).
// The Party House "holds special items": the pop-up bomb for a Meccaryn, the power-up bow for Delphi,
// on the shop's counter (ours). Before any shop has opened they wait for it (openShopAt).
void GameApp::addPartyGoods() {
    if (!base_.partyGoods || !shopOn_) return;
    const int kind = base_.reaper ? 7 : 3;
    if (kind == 7) {
        initBows();
        if (bows_[kBowPowerup].have) return;
    }
    for (const auto& it : shopItems_)
        if (it.kind == kind && !it.taken) return;  // (already on the counter)
    shopItems_.push_back({kind, shopAt_, false});
    if (!shopInside_) shopAnchored_ = false;
    LOGI("Base: the Party House's goods are in the shop");
}

int GameApp::standingBase(bool reaper, const Mat4& at, std::vector<SkBuilding>& out) {
    if (!assetWorld_) return 0;
    const BldgDef* defs = reaper ? kReaper : kMecc;
    const int n = reaper ? int(sizeof(kReaper) / sizeof(kReaper[0])) : int(sizeof(kMecc) / sizeof(kMecc[0]));
    const AssetFS& fs = assetWorld_->fs();
    std::vector<uint8_t> b;
    AnmAnimation tmpl;
    if (!fs.read(reaper ? "RP_base_template_1.anm" : "Mecc_base_template_1.anm", b) || !parseAnm(b.data(), b.size(), tmpl)) return 0;
    std::vector<Mat4> nodes;
    tmpl.evaluate(0.0f, nodes);
    ModelCache* mc = assetWorld_->models();
    int placed = 0;
    for (int d = 0; d < n; ++d) {
        const std::string key = defs[d].key;
        if (key.find("wall") != std::string::npos || key.find("gate") != std::string::npos) continue;
        std::string slots = defs[d].slot;
        size_t s0 = 0;
        while (s0 <= slots.size()) {
            const size_t s1 = std::min(slots.find('|', s0), slots.size());
            const std::string sn = toLower(slots.substr(s0, s1 - s0));
            s0 = s1 + 1;
            int node = -1;
            for (size_t i = 0; i < tmpl.nodes.size() && i < nodes.size(); ++i)
                if (toLower(tmpl.nodes[i].name) == sn) node = int(i);
            if (node < 0) continue;
            SkBuilding bl;
            bl.pad = d == 0;
            bl.xf = at * gameMatrixToRuntime(nodes[size_t(node)]);
            float gy;
            if (assetWorld_->terrain().heightAt(bl.xf.m[12], bl.xf.m[14], gy)) bl.xf.m[13] = gy;
            auto sm = std::make_unique<SkinnedModel>();
            if (fs.exists(std::string(defs[d].model) + ".skn") && sm->load(fs, *mc, renderer_, defs[d].model, defs[d].anim)) {
                sm->pose(renderer_, sm->animDuration() * 0.999f);  // built: the end of its build clip
                bl.model = std::move(sm);
            } else {
                bl.still = mc->get(std::string(defs[d].model) + ".gbs", defs[d].anim, true);
            }
            if (!bl.model && !bl.still) continue;
            // Solid: its collision mesh (<model>_coll), else its model.
            const RenderModel* coll = mc->get(std::string(defs[d].model) + "_coll.gbs", "", false);
            if (!coll || coll->collisionTris.empty()) coll = mc->get(std::string(defs[d].model) + ".gbs", "", false);
            if (coll && !coll->collisionTris.empty()) {
                std::vector<Vec3> tris;
                tris.reserve(coll->collisionTris.size());
                for (const Vec3& v : coll->collisionTris) {
                    const Vec4 q = bl.xf * Vec4{v.x, v.y, v.z, 1.0f};
                    tris.push_back({q.x, q.y, q.z});
                }
                assetWorld_->collisionMutable().addObject(tris);
            }
            out.push_back(std::move(bl));
            ++placed;
        }
    }
    return placed;
}

void GameApp::initBase() {
    base_ = Base{};
    enemies_.params.patrolSpeed = EnemyParams{}.patrolSpeed;  // the base missions raise it (below)
    if (!assetWorld_ || (hero_ != Hero::Mecc && hero_ != Hero::Delphi)) return;
    const bool reaper = hero_ == Hero::Delphi;
    if (!baseMission(assetWorld_->missionName(), reaper)) return;
    kDefs = reaper ? kReaper : kMecc;
    kNumDefs = reaper ? int(sizeof(kReaper) / sizeof(kReaper[0])) : int(sizeof(kMecc) / sizeof(kMecc[0]));
    kRes = reaper ? "VIMP SOULS" : "VIMP MEAT";
    base_.reaper = reaper;
    const AssetFS& fs = assetWorld_->fs();
    std::vector<uint8_t> b;
    AnmAnimation tmpl;
    const char* tmplName = reaper ? "RP_base_template_1.anm" : "Mecc_base_template_1.anm";
    if (!fs.read(tmplName, b) || !parseAnm(b.data(), b.size(), tmpl)) {
        LOGW("Base: %s unavailable", tmplName);
        return;
    }
    std::vector<Mat4> nodes;
    tmpl.evaluate(0.0f, nodes);
    auto slot = [&](const std::string& name, Mat4& out) {
        for (size_t i = 0; i < tmpl.nodes.size() && i < nodes.size(); ++i)
            if (toLower(tmpl.nodes[i].name) == toLower(name)) { out = nodes[i]; return true; }
        return false;
    };
    // Centre: Borjoyzee's start (5c: the base's leader stands at the site), else the mission's
    // marker (r4a has one, id 2, and no Borjoyzee), else the player start. Ours.
    const MissionData& md = assetWorld_->mission();
    const ObjectRegistry& reg = assetWorld_->registry();
    bool found = false;
    for (const auto& o : md.objects)
        if (toLower(reg.modelName(o.type)).find("borjoyzee") != std::string::npos) {
            base_.centre = giantsToRuntime(o.x, o.y, o.z);
            found = true;
            break;
        }
    if (!found) {
        // No Borjoyzee (the Reaper missions): candidates are the mission's marker and the middle
        // of where its army paths end (they end round the base: 5c's ring Borjoyzee, r4a's its
        // marker). Pick the one with the most open, dry, level ground within 45 m - rstory2's
        // marker sits against a cliff over water, its paths end 130 m south (ours).
        std::vector<Vec3> cands;
        for (const auto& o : md.objects)
            if (toLower(reg.modelName(o.type)) == "marker") cands.push_back(giantsToRuntime(o.x, o.y, o.z));
        std::vector<std::pair<int, std::vector<Vec3>>> paths;
        for (const auto& o : md.objects) {
            if (toLower(reg.modelName(o.type)) != "army_spline") continue;
            auto it = std::find_if(paths.begin(), paths.end(), [&](const auto& pp) { return pp.first == o.group; });
            if (it == paths.end()) { paths.push_back({o.group, {}}); it = paths.end() - 1; }
            it->second.push_back(giantsToRuntime(o.x, o.y, o.z));
        }
        if (!paths.empty()) {
            // The ends' middle, ignoring outliers (paths that end by the barracks): the median end
            // and the mean of the ends within 250 m of it.
            std::vector<Vec3> ends;
            for (const auto& pp : paths) ends.push_back(pp.second.back());
            std::vector<float> xs, zs;
            for (const auto& e : ends) { xs.push_back(e.x); zs.push_back(e.z); }
            std::nth_element(xs.begin(), xs.begin() + long(xs.size() / 2), xs.end());
            std::nth_element(zs.begin(), zs.begin() + long(zs.size() / 2), zs.end());
            const Vec3 med{xs[xs.size() / 2], 0, zs[zs.size() / 2]};
            Vec3 sum;
            int n = 0;
            for (const auto& e : ends)
                if (length(Vec3{e.x - med.x, 0, e.z - med.z}) < 250.0f) { sum = sum + e; ++n; }
            if (n) cands.push_back(sum * (1.0f / float(n)));
        }
        const Terrain& terr = assetWorld_->terrain();
        auto score = [&](const Vec3& c) {
            float h0;
            if (!terr.heightAt(c.x, c.z, h0) || h0 < player_.seaLevel - 0.5f) return -1;
            int good = 0;
            for (int r = 1; r <= 3; ++r)
                for (int a = 0; a < 12; ++a) {
                    const float ang = float(a) * 0.5236f, rad = 15.0f * float(r);
                    float h;
                    if (terr.heightAt(c.x + std::cos(ang) * rad, c.z + std::sin(ang) * rad, h) && h > player_.seaLevel - 0.5f &&
                        std::fabs(h - h0) < 3.0f)
                        ++good;
                }
            return good;
        };
        int best = -2;
        for (const auto& c : cands) {
            const int sc = score(c);
            LOGI("Base: centre candidate (%.1f %.1f) open ground %d/36", c.x, c.z, sc);
            if (sc > best) { best = sc; base_.centre = c; found = true; }
        }
    }
    if (!found) base_.centre = assetWorld_->spawnPoint();
    float gy;
    if (assetWorld_->terrain().heightAt(base_.centre.x, base_.centre.z, gy)) base_.centre.y = gy;
    const Mat4 baseXf = Mat4::translation(base_.centre);
    ModelCache* mc = assetWorld_->models();
    for (int d = 0; d < kNumDefs; ++d) {
        std::string slots = kDefs[d].slot;
        if (slots.empty()) { placeTurretSites(d, baseXf); continue; }
        size_t s0 = 0;
        while (s0 <= slots.size()) {
            const size_t s1 = std::min(slots.find('|', s0), slots.size());
            const std::string sn = slots.substr(s0, s1 - s0);
            s0 = s1 + 1;
            Mat4 m;
            if (!slot(sn, m)) { LOGW("Base: template has no slot %s", sn.c_str()); continue; }
            Base::Bldg bl;
            bl.def = d;
            bl.xf = baseXf * gameMatrixToRuntime(m);
            Vec3 p{bl.xf.m[12], bl.xf.m[13], bl.xf.m[14]};
            if (assetWorld_->terrain().heightAt(p.x, p.z, gy)) bl.xf.m[13] = gy;
            auto sm = std::make_unique<SkinnedModel>();
            if (fs.exists(std::string(kDefs[d].model) + ".skn") && sm->load(fs, *mc, renderer_, kDefs[d].model, kDefs[d].anim)) bl.model = std::move(sm);
            else bl.still = mc->get(std::string(kDefs[d].model) + ".gbs", kDefs[d].anim, true);  // static (wood_command)
            if (!bl.model && !bl.still) LOGW("Base: %s unavailable", kDefs[d].model);
            if (isKey(d, "gyropad") && bl.model && bl.model->restMin().y > 0.05f)  // (its mesh starts 2.5 m above its origin: on the ground)
                bl.xf.m[13] -= bl.model->restMin().y;
            if (d == 0) { bl.state = 3; bl.progress = 1.0f; }
            base_.bldgs.push_back(std::move(bl));
        }
    }
    // Props that stand from the start: the fire pit and the meat drop-off.
    Mat4 m;
    if (!reaper && slot("Firepit", m)) {
        base_.firepit = std::make_unique<SkinnedModel>();
        if (!base_.firepit->load(fs, *mc, renderer_, "Sm_firepit", "SM_firepit.anm")) base_.firepit.reset();
        base_.firepitXf = baseXf * gameMatrixToRuntime(m);
    }
    if (slot(reaper ? "soul_dropoff" : "meat_dropoff", m)) {
        base_.dropXf = baseXf * gameMatrixToRuntime(m);
        if (reaper) {
            base_.firepit = std::make_unique<SkinnedModel>();  // the soul drop-off is animated: drawn like the fire pit
            if (!fs.exists("Rp_soul_dropoff.skn") || !base_.firepit->load(fs, *mc, renderer_, "Rp_soul_dropoff", "Rp_soul_dropoff.anm")) {
                base_.firepit.reset();  // no skin: the static model with its clip's rest pose
                base_.dropModel = mc->get("Rp_soul_dropoff.gbs", "Rp_soul_dropoff.anm", true);
            }
            base_.firepitXf = base_.dropXf;
        } else {
            base_.dropModel = mc->get("sm_dropoff.gbs", "sm_dropoff.anm", true);
        }
    }
    base_.dropAt = Vec3{base_.dropXf.m[12], base_.dropXf.m[13], base_.dropXf.m[14]};
    if (assetWorld_->terrain().heightAt(base_.dropAt.x, base_.dropAt.z, gy)) base_.dropAt.y = base_.dropXf.m[13] = gy;
    // The drop-off has to be walkable to (Delphi can't fly): if its slot lies on a rock well above
    // or below the base's middle, it moves in toward the middle until the ground is level with it (ours).
    for (int k = 0; k < 20 && std::fabs(base_.dropAt.y - base_.centre.y) > 2.0f; ++k) {
        base_.dropAt = base_.dropAt + (Vec3{base_.centre.x, base_.dropAt.y, base_.centre.z} - base_.dropAt) * 0.12f;
        if (assetWorld_->terrain().heightAt(base_.dropAt.x, base_.dropAt.z, gy)) base_.dropAt.y = gy;
        base_.dropXf.m[12] = base_.dropAt.x;
        base_.dropXf.m[13] = base_.dropAt.y;
        base_.dropXf.m[14] = base_.dropAt.z;
        if (base_.reaper) base_.firepitXf = base_.dropXf;
    }
    base_.padAt = Vec3{base_.bldgs.front().xf.m[12], base_.bldgs.front().xf.m[13], base_.bldgs.front().xf.m[14]};
    base_.on = true;
    // Raiders march briskly in the base missions (the walk at 1.1 m/s took minutes along the army paths).
    enemies_.params.patrolSpeed = 2.4f;
    initCrew();
    initLeader();
    LOGI("Base: centre (%.1f %.1f %.1f), %zu building sites, pad (%.1f %.1f %.1f), drop-off (%.1f %.1f %.1f)", base_.centre.x,
         base_.centre.y, base_.centre.z, base_.bldgs.size(), base_.padAt.x, base_.padAt.y, base_.padAt.z, base_.dropAt.x,
         base_.dropAt.y, base_.dropAt.z);
}

// A kind's state over all its sites: 0 not ordered, 1 building, 3 all complete.
int GameApp::baseKindState(int d) const {
    bool any = false, all = true, ordered = false;
    for (const auto& b : base_.bldgs) {
        if (b.def != d) continue;
        any = true;
        all = all && b.state == 3;
        ordered = ordered || b.state != 0;
    }
    return !any ? 3 : all ? 3 : ordered ? 1 : 0;
}

// The kinds the menu offers: each not-yet-started building once (all slots of a wall together).
std::vector<int> GameApp::baseOffers() const {
    std::vector<int> v;
    for (int d = 1; d < kNumDefs; ++d) {
        bool open = false;
        for (const auto& b : base_.bldgs) open = open || (b.def == d && b.state == 0);
        if (open) v.push_back(d);
    }
    return v;
}

int GameApp::baseKindLevel(int d) const {
    int lv = 99;
    for (const auto& b : base_.bldgs)
        if (b.def == d) lv = std::min(lv, b.level);
    return lv == 99 ? 0 : lv;
}

// Upgrading: every site of the kind is rebuilt as its second-level model (its own build clip).
void GameApp::orderUpgrade(int d) {
    const Upgrade* u = upgradeFor(d);
    if (!u) return;
    if (base_.food < u->meat) {
        timmyLine_ = std::string(u->label) + ": NEEDS " + std::to_string(u->meat) + " " + kRes + " IN THE STORE (" +
                     std::to_string(base_.food) + ")";
        timmyLineT_ = 3.0f;
        sfx2D("ph_selct", 0.5f, 0.6f);
        return;
    }
    const AssetFS& fs = assetWorld_->fs();
    ModelCache* mc = assetWorld_->models();
    int n = 0;
    for (auto& b : base_.bldgs) {
        if (b.def != d || b.state != 3 || b.level != 0) continue;
        auto sm = std::make_unique<SkinnedModel>();
        const RenderModel* still = nullptr;
        if (!sm->load(fs, *mc, renderer_, u->model, u->anim)) {
            sm.reset();
            still = mc->get(std::string(u->model) + ".gbs", u->anim, true);
            if (!still) { LOGW("Base: %s unavailable", u->model); continue; }
        }
        if (b.collision >= 0) assetWorld_->collisionMutable().setEnabled(b.collision, false);
        b.collision = -1;  // the new building's own collision once it stands
        b.model = std::move(sm);
        b.still = still;
        b.posedT = -1;
        b.level = 1;
        b.state = 1;
        b.progress = 0;
        ++n;
    }
    if (!n) return;
    base_.food -= u->meat;
    timmyLine_ = std::string(u->label) + " - BUILDING...";
    timmyLineT_ = 3.0f;
    sfx2D("ph_selct", 1.0f, 1.2f);
    for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.5f, 0.1f});
    LOGI("Base: upgrade to %s ordered (%d sites, %d %s, store %d)", u->label, n, u->meat, kRes, base_.food);
}

void GameApp::orderBuilding(int d) {
    const int cost = kDefs[d].meat;
    if (base_.food < cost) {
        timmyLine_ = std::string(kDefs[d].label) + ": NEEDS " + std::to_string(cost) + " " + kRes + " IN THE STORE (" +
                     std::to_string(base_.food) + ")";
        timmyLineT_ = 3.0f;
        sfx2D("ph_selct", 0.5f, 0.6f);
        return;
    }
    base_.food -= cost;
    for (auto& b : base_.bldgs)
        if (b.def == d && b.state == 0) b.state = 1;
    timmyLine_ = std::string(kDefs[d].label) + " - BUILDING...";
    timmyLineT_ = 3.0f;
    sfx2D("ph_selct", 1.0f);
    for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.5f, 0.1f});
    LOGI("Base: ordered %s (%d %s, store %d)", kDefs[d].label, cost, kRes, base_.food);
}

void GameApp::updateBase(float dt, const InputFrame& in) {
    if (!base_.on || !playMode_) return;
    if (base_.failT > 0) {  // lost: the mission starts again from its briefing
        base_.failT -= dt;
        timmyLineT_ = std::max(timmyLineT_, 1.0f);
        if (base_.failT <= 0) { LOGI("Base: restarting %s", assetWorld_->missionName().c_str()); beginLoading(assetWorld_->missionName(), in); }
        return;
    }
    updateLeader(dt);
    // Test hooks: GVR_BASE_FOOD=n stocks the store; GVR_BASE_ORDER=all orders everything at the start.
    if (const char* f = std::getenv("GVR_BASE_FOOD"); f && base_.traceT == 0.0f) base_.food = std::atoi(f);
    if (const char* o = std::getenv("GVR_BASE_ORDER"); o && !base_.autoOrdered) {
        base_.autoOrdered = true;
        if (std::string(o) == "all")
            for (int d : baseOffers()) { base_.food += kDefs[d].meat; orderBuilding(d); }
        else  // (a building's key: that one only)
            for (int d : baseOffers())
                if (isKey(d, o)) { base_.food += kDefs[d].meat; orderBuilding(d); }
    }
    const Vec3 p = player_.position;
    auto flat = [](const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); };
    // Meat into the store.
    if (vimpMeat_ > 0 && flat(p, base_.dropAt) < kDropRadius) {
        base_.food += vimpMeat_;
        LOGI("Base: %d Vimp meat stored (%d)", vimpMeat_, base_.food);
        timmyLine_ = std::string(kRes) + " STORED: " + std::to_string(base_.food);
        timmyLineT_ = 3.0f;
        vimpMeat_ = 0;
        sfx2D("pu_meat1", 1.0f, 0.8f);
    }
    // Construction: one building at a time per crew; the whole workforce on the oldest order.
    static const bool trace = std::getenv("GVR_BASE_TRACE") != nullptr;
    if (trace && int(base_.traceT + dt) != int(base_.traceT))
        for (const auto& b : base_.bldgs)
            if (b.state == 1) LOGI("base-trace t=%.0f %s progress %.2f", base_.traceT, bldgLabel(b.def, b.level), b.progress);
    base_.traceT += dt;
    updateCrew(dt);
    poseCrew();
    int busy = 0;
    const int crews = crewSites();
    // The Pub (Meccaryn) / Persuader (Reaper): once it has stood, its loss stops every other building
    // until it is rebuilt ("Can't build - Repair the Pub!!!" / "... the Persuader!!!"); a standing
    // Persuader with souls in the store speeds the work and eats one every 30 s.
    const char* foodKey = base_.reaper ? "extractor" : "pub";
    bool foodBuilt = false, foodStands = false, persuading = false;
    for (const auto& b : base_.bldgs)
        if (std::string(kDefs[b.def].key) == foodKey) {
            foodBuilt = foodBuilt || base_.rewarded.count(b.def) > 0;
            foodStands = foodStands || b.state == 3;
        }
    const bool foodDown = foodBuilt && !foodStands;
    if (base_.reaper && foodStands) {
        for (const auto& b : base_.bldgs) persuading = persuading || b.state == 1;
        persuading = persuading && base_.food > 0;
        if (persuading && (base_.soulT += dt) >= 30.0f) {
            base_.soulT = 0;
            --base_.food;
            LOGI("Base: the Persuader ate a soul (%d left)", base_.food);
        }
    }
    base_.blockSayT = std::max(0.0f, base_.blockSayT - dt);
    for (auto& b : base_.bldgs) {
        if (b.state != 1) continue;
        if (busy >= crews) break;  // two Smarties per building going up (game_app_basecrew.cpp)
        if (foodDown && std::string(kDefs[b.def].key) != foodKey) {
            if (base_.blockSayT <= 0) {
                base_.blockSayT = 15.0f;
                smartieSay(base_.reaper ? "YAN" : "BORJOYZEE", base_.reaper ? "basehintextrdestroyed" : "basehintpubdestroyed");
            }
            continue;
        }
        ++busy;
        // Work stops while Reapers are at the site (the crew cowers).
        bool threat = false;
        for (const auto& e : enemies_.enemies)
            if (e.state != EnemyState::Dead && length(Vec3{e.pos.x - b.xf.m[12], 0, e.pos.z - b.xf.m[14]}) < 15.0f) { threat = true; break; }
        if (threat) continue;
        b.progress = std::min(1.0f, b.progress + dt * base_.speed * (persuading ? 1.25f : 1.0f) / std::max(1.0f, kDefs[b.def].seconds));
        b.fxT -= dt;
        if (b.fxT <= 0) {
            b.fxT = 0.35f + 0.3f * frand();
            const Vec3 at{b.xf.m[12] + (frand() - 0.5f) * 4.0f, b.xf.m[13] + 0.3f, b.xf.m[14] + (frand() - 0.5f) * 4.0f};
            smokePuff(FxTex::Dust, at, Vec3{0, 0.6f, 0}, 0.6f, 2.0f, 1.6f, 0.45f, 0.1f);
        }
        if (b.progress >= 1.0f) {
            b.state = 3;
            // Its gyrocopter, on the pad's top. (2026-10-29 review) Rebuilt after a wreck: the parked one
            // had come down through the wreck onto the ground inside the new pad (it couldn't be boarded
            // again) - it goes back on top; one in the air just gets its landing pad back.
            const Vec3 padPos{b.xf.m[12], b.xf.m[13], b.xf.m[14]};
            const bool parkedHere = gyro_.on && !gyro_.riding && length(Vec3{gyro_.pos.x - padPos.x, 0, gyro_.pos.z - padPos.z}) < 4.0f;
            if (isKey(b.def, "gyropad") && (!gyro_.on || parkedHere || gyro_.riding)) {
                Vec3 mn{0, 0, 0}, mx{0, 0.4f, 0};
                if (b.model) { mn = b.model->restMin(); mx = b.model->restMax(); }
                else if (b.still) { mn = b.still->boundsMin; mx = b.still->boundsMax; }
                float ty = b.xf.m[13];
                assetWorld_->terrain().heightAt(b.xf.m[12], b.xf.m[14], ty);
                const float lift = std::max(0.0f, b.xf.m[13] + mx.y - ty);
                if (gyro_.riding) {
                    gyro_.padAt = Vec3{padPos.x, ty, padPos.z};
                    gyro_.padY = lift > 0 ? ty + lift : -1e9f;
                } else {
                    const bool first = !gyro_.on;
                    placeGyro(padPos, parkedHere ? gyro_.yaw : std::atan2(-b.xf.m[8], -b.xf.m[10]), lift);
                    if (first) smartieSay("SMARTIE", "smchgyropaddone");
                }
            }
            b.maxHp = b.hp = (kDefs[b.def].core ? 400.0f : 250.0f) * (b.level > 0 ? 1.6f : 1.0f);
            // Solid once standing: the building's own collision mesh (<model>_coll), else its model.
            ModelCache* mc = assetWorld_->models();
            const std::string collName = std::string(bldgModel(b.def, b.level)) + "_coll.gbs";
            const RenderModel* coll = assetWorld_->fs().exists(collName) ? mc->get(collName, "", false) : nullptr;
            if (!coll || coll->collisionTris.empty()) coll = mc->get(std::string(bldgModel(b.def, b.level)) + ".gbs", "", false);
            // A gate "allows you to quickly enter and exit your base": solid while shut, it opens as
            // you come up to it (updateGates). The Build Pad / Comm Center stays walkable (its menu).
            if (isGate(b.def) && b.model && b.openClip < 0) {
                b.openClip = b.model->addAnimation(assetWorld_->fs(), std::string(bldgModel(b.def, b.level)) + "_open.anm");
                b.openT = 0;
                b.posedOpen = -1;
            }
            // Its damaged look (D_<model> in its rest pose: the same building scorched and broken; the
            // Pub's is its wreckage), shown below half health.
            {
                const std::string dn = std::string("D_") + bldgModel(b.def, b.level) + ".gbs";
                b.damaged = assetWorld_->fs().exists(dn) ? mc->get(dn, bldgAnim(b.def, b.level), true) : nullptr;
            }
            if (b.def == 0) {
                LOGI("Base: %s stands (walkable)", bldgLabel(b.def, b.level));
            } else if (b.collision >= 0) {
                assetWorld_->collisionMutable().setEnabled(b.collision, true);  // rebuilt after a raid
            } else if (coll && !coll->collisionTris.empty()) {
                std::vector<Vec3> tris;
                tris.reserve(coll->collisionTris.size());
                for (const Vec3& v : coll->collisionTris) {
                    const Vec4 q = b.xf * Vec4{v.x, v.y, v.z, 1.0f};
                    tris.push_back({q.x, q.y, q.z});
                }
                b.collision = assetWorld_->collisionMutable().addObject(tris);
                Vec3 mn = tris[0], mx = tris[0];
                for (const Vec3& t : tris) { mn = Vec3{std::min(mn.x, t.x), std::min(mn.y, t.y), std::min(mn.z, t.z)}; mx = Vec3{std::max(mx.x, t.x), std::max(mx.y, t.y), std::max(mx.z, t.z)}; }
                LOGI("Base: %s solid (%zu triangles, box (%.1f %.1f %.1f)-(%.1f %.1f %.1f), site (%.1f %.1f %.1f))", bldgLabel(b.def, b.level),
                     tris.size() / 3, mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, b.xf.m[12], b.xf.m[13], b.xf.m[14]);
            }
            bool allOfKind = true;
            for (const auto& o : base_.bldgs) allOfKind = allOfKind && (o.def != b.def || o.state == 3);
            auto rw = base_.rewarded.find(b.def);
            const bool rebuilt = rw != base_.rewarded.end() && rw->second >= b.level;
            if (allOfKind && rebuilt) {
                // Rebuilt after a raid: standing again, no second reward (shop items, counters).
                timmyLine_ = std::string(bldgLabel(b.def, b.level)) + " - REBUILT";
                timmyLineT_ = 3.0f;
                sfx2D("pu_hlth", 0.7f, 0.8f);
                LOGI("Base: %s rebuilt", bldgLabel(b.def, b.level));
            } else if (allOfKind) {
                base_.rewarded[b.def] = b.level;
                // What a finished building gives (ours, after the game's descriptions): the Giftshop
                // "is full of weapons and other nifty stuff" - the shop counter opens in it; the
                // Workshop is "where your Smarties go for tools" - they build faster from then on.
                if (std::string(kDefs[b.def].key) == "giftshop1" && b.level > 0 && shopOn_) {
                    // "Even more weapons are available in this upgraded Gift Shop!": a pop-up bomb joins the counter.
                    shopItems_.push_back({3, shopAt_, false});
                    if (!shopInside_) shopAnchored_ = false;  // laid out afresh on the next visit
                } else if (std::string(kDefs[b.def].key) == "spellshop1" && b.level > 0 && shopOn_) {
                    // "More spells and weapons in the upgraded Spell Shop": the power-up bow joins (ours).
                    initBows();
                    if (!bows_[kBowPowerup].have) shopItems_.push_back({7, shopAt_, false});
                    if (!shopInside_) shopAnchored_ = false;
                } else if (std::string(kDefs[b.def].key) == "giftshop1" || std::string(kDefs[b.def].key) == "spellshop1") {
                    // The counter stands at its front (toward the base's middle): the building is solid.
                    const Vec3 site{b.xf.m[12], b.xf.m[13], b.xf.m[14]};
                    Vec3 dir = base_.centre - site;
                    dir.y = 0;
                    Vec3 at = site + normalize(dir) * std::min(8.0f, length(dir) * 0.5f);
                    float gy;
                    if (assetWorld_->terrain().heightAt(at.x, at.z, gy)) at.y = gy;
                    openShopAt(at);
                }
                if (std::string(kDefs[b.def].key) == "mcworkshop" || std::string(kDefs[b.def].key) == "rpworkshop") base_.speed = 1.5f;
                timmyLine_ = std::string(bldgLabel(b.def, b.level)) + " - COMPLETED!";
                timmyLineT_ = 3.5f;
                if (std::string(kDefs[b.def].key) == "mcpartyhouse" || std::string(kDefs[b.def].key) == "rppartyhouse") {
                    base_.partyGoods = true;
                    addPartyGoods();
                    // ("PARTY HOUSE COMPLETE! Check your shop for new weapons!" - its own line)
                    smartieSay(base_.reaper ? "YAN" : "BORJOYZEE", base_.reaper ? "basechatrpphousedone" : "basechatmcphousedone");
                }
                sfx2D("pu_hlth", 0.9f, 0.8f);
                LOGI("Base: %s completed", bldgLabel(b.def, b.level));
            }
        }
    }
    // Test hook: GVR_BASE_UPGRADE=1 upgrades every kind as soon as it stands.
    if (static const bool autoUp = std::getenv("GVR_BASE_UPGRADE") != nullptr; autoUp)
        for (int d = 0; d < kNumDefs; ++d)
            if (baseKindState(d) == 3 && baseKindLevel(d) == 0 && upgradeFor(d)) { base_.food += upgradeFor(d)->meat; orderUpgrade(d); }
    // Test hook: GVR_BASE_WRECK=<s> wrecks every standing building (not the pad) once, at that time;
    // GVR_BASE_DAMAGE=<s> takes them to 40 % health instead (their damaged look).
    if (static const char* wr = std::getenv("GVR_BASE_WRECK"); wr && base_.traceT >= float(std::atof(wr)) && base_.traceT - dt < float(std::atof(wr)))
        for (auto& b : base_.bldgs)
            if (b.def != 0 && b.state == 3) wreckBuilding(b);
    if (static const char* dm = std::getenv("GVR_BASE_DAMAGE"); dm && base_.traceT >= float(std::atof(dm)) && base_.traceT - dt < float(std::atof(dm)))
        for (auto& b : base_.bldgs)
            if (b.def != 0 && b.state == 3) { b.hp = 0.4f * b.maxHp; b.hitT = float(time_); }
    updateRaids(dt);
    updateGates(dt);
    updateTurrets(dt);
    if (!base_.complete) {
        bool all = true;
        for (const auto& b : base_.bldgs) all = all && (!kDefs[b.def].core || b.state == 3);
        if (all) {
            base_.complete = true;
            if (base_.reaper) {
                // r4a: "Your Smarties will create the TORNADO SPELL for you once your BASE is complete."
                timmyLine_ = "BASE COMPLETE! YOUR SMARTIES TAUGHT YOU THE TORNADO SPELL - HOLD THE CAST LONGER.";
                timmyLineT_ = 6.0f;
            } else {
                givePopupBomb(1, "BASE COMPLETE! YOUR SMARTIES MADE YOU A POP-UP BOMB.");
            }
            LOGI("Base: complete");
        }
    }
    // The Build Pad: the menu opens while you stand on it.
    const bool onPad = flat(p, base_.padAt) < kPadRadius && std::fabs(p.y - base_.padAt.y) < 4.0f;
    if (onPad && !base_.onPad) {
        const float yaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : player_.yaw;
        base_.menuAt = p;
        base_.menuYaw = yaw;
        sfx2D("ph_selct", 0.8f, 1.2f);
        if (base_.food == 0 && !baseOffers().empty()) {
            timmyLine_ = std::string("OUT OF FOOD!!! BRING ") + kRes + " TO THE DROP-OFF.";
            timmyLineT_ = 4.0f;
        }
    }
    base_.onPad = onPad;
    base_.items.clear();
    if (!onPad) return;
    // Every kind keeps its place (ordered ones show their state), so signs never slide under the hand.
    std::vector<int> offers;
    for (int d = 1; d < kNumDefs; ++d) offers.push_back(d);
    if (upgradeFor(0)) offers.push_back(0);  // the pad's own upgrade (Comm Center), last
    // Within arm's reach without stepping: up to four per row, three rows, 0.8 m out.
    for (size_t i = 0; i < offers.size(); ++i) {
        const size_t row = i / 4, col = i % 4;
        const float inRow = float(std::min<size_t>(4, offers.size() - row * 4));
        const float a = base_.menuYaw + (float(col) - 0.5f * (inRow - 1.0f)) * 0.55f;
        base_.items.push_back({offers[i], base_.menuAt + Vec3{-std::sin(a) * 0.8f, 1.35f - 0.33f * float(row), -std::cos(a) * 0.8f}});
    }
    // The table map, to the left of the signs: the base's sites within 0.28 m of its middle.
    {
        const float ta = base_.menuYaw + 1.25f;
        base_.tableAt = base_.menuAt + Vec3{-std::sin(ta) * 0.62f, 0.92f, -std::cos(ta) * 0.62f};
        float reach = 12.0f;
        for (const auto& b : base_.bldgs) reach = std::max(reach, flat(Vec3{b.xf.m[12], 0, b.xf.m[14]}, base_.centre) + 6.0f);
        base_.tableScale = 0.28f / reach;

    }
    base_.touchCool = std::max(0.0f, base_.touchCool - dt);
    if (base_.touchCool > 0) return;
    Vec3 hands[2];
    int nh = 0;
    for (int h = 0; h < 2; ++h)
        if (in.hand[h].gripPoseValid) hands[nh++] = rig_.toWorld(in.hand[h].gripPose).position;
    // Test hook GVR_BASE_TABLE=<label>: from 2 s on the pad, the right hand is put on that kind's first free
    // site on the table (the reach itself is the player's; this checks the map and the order).
    if (static const char* tt = std::getenv("GVR_BASE_TABLE"); tt) {
        static float onPadT = 0;
        onPadT += dt;
        if (onPadT > 2.0f)
            for (const auto& b : base_.bldgs)
                if (b.state == 0 && b.def > 0 && std::string(kDefs[b.def].label) == tt) {
                    hands[0] = baseTablePoint(Vec3{b.xf.m[12], b.xf.m[13], b.xf.m[14]});
                    if (nh == 0) nh = 1;
                    break;
                }
    }
    // A free site touched on the table: that building ordered (all of a kind's sites go up together).
    for (const auto& b : base_.bldgs) {
        if (b.state != 0 || b.def == 0) continue;
        const Vec3 at = baseTablePoint(Vec3{b.xf.m[12], b.xf.m[13], b.xf.m[14]});
        bool touch = false;
        for (int k = 0; k < nh; ++k)
            touch = touch || (flat(hands[k], at) < 0.035f && hands[k].y > at.y - 0.03f && hands[k].y < at.y + 0.07f);
        if (!touch) continue;
        base_.touchCool = 0.8f;
        LOGI("Base: %s touched on the table map", kDefs[b.def].label);
        orderBuilding(b.def);
        return;
    }
    for (const auto& it : base_.items) {
        bool touch = false;
        for (int k = 0; k < nh; ++k) touch = touch || length(hands[k] - it.pos) < 0.16f;
        if (!touch) continue;
        const int st = baseKindState(it.def);
        if (st == 0) { base_.touchCool = 0.8f; orderBuilding(it.def); }
        else if (st == 3 && upgradeFor(it.def) && baseKindLevel(it.def) == 0) { base_.touchCool = 0.8f; orderUpgrade(it.def); }
        else continue;
        break;
    }
}

// Raids (ours; the game's own lines: smchbaseattack1-4 "Help! Our base is under attack!",
// basehintpubdestroyed "Can't build - Repair the Pub!!!"). Once something stands, every 80 s while
// the enemy army barracks stands, a few Reapers come down the mission's army paths (army_spline, the
// last ~60 m of each; else 45-60 m out on the barracks' side) and march on the base; near a standing building they wreck it (6 hp/s each) unless the player draws them off.
// A wrecked building falls back to its site and can be ordered again.
void GameApp::updateRaids(float dt) {
    if (!assetWorld_ || enemyPool_.empty() || std::getenv("GVR_NO_RAIDS")) return;
    // The barracks: where the raids come from, and while it stands they keep coming.
    bool barracks = false;
    for (const auto& d : destruct_.items) {
        if (d.object < 0 || size_t(d.object) >= assetWorld_->worldObjects().size()) continue;
        if (assetWorld_->worldObjects()[size_t(d.object)].name != "guard_army_barracks") continue;
        if (!base_.haveRaidFrom) { base_.raidFrom = d.base; base_.haveRaidFrom = true; }
        barracks = barracks || d.alive();
    }
    // The data's army paths: type army_spline, one path per id (op 5), points in file order from the
    // barracks to the base (5c: 18 paths from about (700, 1500) to the base's surroundings; ids 10 and
    // 11 loop by the barracks). Read: paths whose last point lies within 150 m of the base.
    if (!base_.routesRead) {
        base_.routesRead = true;
        const MissionData& md = assetWorld_->mission();
        const ObjectRegistry& reg = assetWorld_->registry();
        std::vector<std::pair<int, std::vector<Vec3>>> paths;
        for (const auto& o : md.objects) {
            if (toLower(reg.modelName(o.type)) != "army_spline") continue;
            auto it = std::find_if(paths.begin(), paths.end(), [&](const auto& pp) { return pp.first == o.group; });
            if (it == paths.end()) { paths.push_back({o.group, {}}); it = paths.end() - 1; }
            it->second.push_back(giantsToRuntime(o.x, o.y, o.z));
        }
        for (auto& pp : paths) {
            if (pp.second.size() < 2) continue;
            const Vec3 e = pp.second.back() - base_.centre;
            if (e.x * e.x + e.z * e.z < 150.0f * 150.0f) base_.routes.push_back(std::move(pp.second));
        }
        LOGI("Base: %zu army paths lead to the base (of %zu)", base_.routes.size(), paths.size());
    }
    bool standing = false;
    for (const auto& b : base_.bldgs) standing = standing || (b.def != 0 && b.state == 3);
    if (base_.raidT < 0) {
        if (standing) base_.raidT = std::getenv("GVR_RAID_SOON") ? 2.0f : 60.0f;  // the first a minute after the first building
    } else if (barracks || !base_.haveRaidFrom) {
        base_.raidT -= dt;
        if (base_.raidT <= 0 && standing) {
            base_.raidT = 80.0f;
            ++base_.raids;
            Vec3 dir = base_.haveRaidFrom ? base_.raidFrom - base_.centre : Vec3{frand() - 0.5f, 0, frand() - 0.5f};
            dir.y = 0;
            dir = normalize(dir + Vec3{1e-3f, 0, 0});
            const int n = 2 + std::min(2, base_.raids / 2);
            int placed = 0;
            // Along an army path: start where about 60 m of it remains (the march from the barracks
            // itself would take minutes at a walk), follow its points to the end, then the buildings.
            for (int k = 0; k < n && !base_.routes.empty(); ++k) {
                const int r = int(frand() * float(base_.routes.size())) % int(base_.routes.size());
                const auto& pts = base_.routes[size_t(r)];
                size_t i = pts.size() - 1;
                float left = 0;
                while (i > 0 && left < 60.0f) { left += length(Vec3{pts[i].x - pts[i - 1].x, 0, pts[i].z - pts[i - 1].z}); --i; }
                Vec3 p = pts[i] + Vec3{(frand() - 0.5f) * 3.0f, 0, (frand() - 0.5f) * 3.0f};
                float h;
                if (!assetWorld_->terrain().heightAt(p.x, p.z, h) || h < player_.seaLevel - 0.8f) continue;
                const Vec3 to = pts[std::min(i + 1, pts.size() - 1)] - p;
                const int ei = enemies_.spawn(Vec3{p.x, h, p.z}, std::atan2(-to.x, -to.z));
                if (ei < 0) continue;
                Enemy& e = enemies_.enemies[size_t(ei)];
                e.home = pts[std::min(i + 1, pts.size() - 1)];
                Base::Raider rd{e.uid, r, i + 1, r, k == 0 && base_.leader && base_.carrier == 0, false, pts.front()};
                rd.from = i;
                base_.raiders.push_back(rd);
                LOGI("Raider %u on army path %d at (%.1f %.1f %.1f)", e.uid, r, p.x, h, p.z);
                ++placed;
            }
            for (int attempt = 0; attempt < 30 && placed < n; ++attempt) {  // no paths (or none usable): from the barracks' side
                const float side = (frand() - 0.5f) * 0.8f;
                const Vec3 d2{dir.x * std::cos(side) - dir.z * std::sin(side), 0, dir.x * std::sin(side) + dir.z * std::cos(side)};
                const Vec3 p = base_.centre + d2 * (45.0f + frand() * 15.0f);
                float h;
                if (!assetWorld_->terrain().heightAt(p.x, p.z, h) || h < player_.seaLevel - 0.8f) continue;  // wading depth at most (Delphi's base sits in a lagoon)
                const int ei = enemies_.spawn(Vec3{p.x, h, p.z}, std::atan2(d2.x, d2.z));
                if (ei < 0) continue;
                Enemy& e = enemies_.enemies[size_t(ei)];
                e.home = base_.centre;  // retargeted below to a standing building
                base_.raiders.push_back({e.uid, -1, 0, -1, placed == 0 && base_.leader && base_.carrier == 0, false, Vec3{p.x, h, p.z} + d2 * 40.0f});
                ++placed;
            }
            LOGI("Base raid %d: %d Reapers", base_.raids, placed);
            // A standing Comm Center tells you where they come from, relative to where you face (ours).
            if (placed > 0 && base_.bldgs.front().state == 3 && base_.bldgs.front().level > 0) {
                Vec3 from;
                int k = 0;
                for (size_t i = base_.raiders.size() - size_t(placed); i < base_.raiders.size(); ++i)
                    for (const auto& e : enemies_.enemies)
                        if (e.uid == base_.raiders[i].uid) { from = from + e.pos; ++k; }
                if (k) {
                    from = from * (1.0f / float(k)) - player_.position;
                    const float yawHead = lastInput_.headValid ? yawOf(rig_.toWorld(lastInput_.head).orientation) : player_.yaw;
                    float rel = std::atan2(-from.x, -from.z) - yawHead;
                    while (rel > kPi) rel -= 2 * kPi;
                    while (rel < -kPi) rel += 2 * kPi;
                    const char* side = std::fabs(rel) < 0.8f ? "AHEAD OF YOU" : std::fabs(rel) > 2.35f ? "BEHIND YOU" : rel > 0 ? "ON YOUR LEFT" : "ON YOUR RIGHT";
                    timmyLine_ = "COMM CENTER: " + std::to_string(placed) + " REAPERS COMING - " + side + "!";
                    LOGI("Base: %s", timmyLine_.c_str());
                    timmyLineT_ = 5.0f;
                    for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.6f, 0.15f});
                    sfx2D("ph_selct", 1.0f, 0.7f);
                }
            }
        }
    }
    // March the raiders on the base (the patrol walk, kept going) and let them wreck what they reach.
    base_.attackSayT = std::max(0.0f, base_.attackSayT - dt);
    std::vector<Base::Raider> alive;
    for (Base::Raider rd : base_.raiders) {
        const uint32_t uid = rd.uid;
        Enemy* e = nullptr;
        for (auto& c : enemies_.enemies) if (c.uid == uid) e = &c;
        if (!e || e->state == EnemyState::Dead || e->remove) {
            LOGI("Raider %u gone (%s)", uid, e ? "dead" : "removed");
            continue;
        }
        if (e->aware || rd.back || rd.route >= 0) e->siege = false;
        // Stuck (ground too steep for the Reaper AI, or a rock in the way - the army paths cross
        // ridges): after 3 s without progress while marching, it gets past - 10 m on toward its
        // goal, or a step aside if the player is watching from close by (ours).
        if (!e->aware && !e->siege) {  // (a raider firing at a building stands still on purpose)
            if (length(Vec3{e->pos.x - rd.lastPos.x, 0, e->pos.z - rd.lastPos.z}) > 1.0f) {
                rd.lastPos = e->pos;
                rd.stuckT = 0;
            } else if ((rd.stuckT += dt) > 3.0f) {
                rd.stuckT = 0;
                Vec3 g{e->patrolTo.x - e->pos.x, 0, e->patrolTo.z - e->pos.z};
                const float gd = std::sqrt(g.x * g.x + g.z * g.z);
                Vec3 to = e->pos;
                if (length(e->pos - player_.position) > 30.0f && gd > 0.5f) to = e->pos + g * (std::min(10.0f, gd) / gd);
                else to = e->pos + Vec3{(frand() - 0.5f) * 8.0f, 0, (frand() - 0.5f) * 8.0f};
                float gy;
                if (assetWorld_->terrain().heightAt(to.x, to.z, gy) && gy > player_.seaLevel - 0.8f) {
                    e->pos = Vec3{to.x, gy, to.z};
                    rd.lastPos = e->pos;
                }
            }
        }
        static const bool trace = std::getenv("GVR_BASE_TRACE") != nullptr;
        if (trace && int(base_.traceT) % 10 == 0 && int(base_.traceT - dt) % 10 != 0)
            LOGI("raider %u at (%.1f %.1f) home (%.1f %.1f) state %d aware %d route %d next %zu snatch %d back %d siege %d", uid, e->pos.x, e->pos.z,
                 e->home.x, e->home.z, int(e->state), int(e->aware), rd.route, rd.next, int(rd.snatch), int(rd.back), int(e->siege));
        // Carrying the leader off: back the way it came, to the path's start (the barracks' side).
        if (rd.back) {
            alive.push_back(rd);
            if (e->aware) continue;  // stopped to fight; still holds him
            Vec3 goal = rd.escape;
            if (rd.route >= 0) {
                const auto& pts = base_.routes[size_t(rd.route)];
                while (rd.next > rd.from && length(Vec3{pts[rd.next].x - e->pos.x, 0, pts[rd.next].z - e->pos.z}) < 4.0f) --rd.next;
                goal = pts[rd.next];
                alive.back().next = rd.next;
            }
            const Vec3 g{goal.x - e->pos.x, 0, goal.z - e->pos.z};
            // Lost once he's carried back to where the raid set out (the enemy's side of the path).
            if ((rd.route < 0 || rd.next <= rd.from) && g.x * g.x + g.z * g.z < 4.0f * 4.0f && base_.failT < 0) {
                base_.failT = 6.0f;
                timmyLine_ = "MISSION FAILED - " + base_.leaderName + " WAS TAKEN TO THE ENEMY BASE!";
                timmyLineT_ = 6.0f;
                LOGI("Base: %s carried off - mission failed", base_.leaderName.c_str());
            }
            e->home = e->patrolTo = goal;
            e->patrolWait = 0;
            e->stateTime = 0;
            continue;
        }
        // Still on its army path: walk point to point.
        if (rd.route >= 0 && !e->aware) {
            const auto& pts = base_.routes[size_t(rd.route)];
            while (rd.next < pts.size()) {
                const Vec3 g{pts[rd.next].x - e->pos.x, 0, pts[rd.next].z - e->pos.z};
                if (g.x * g.x + g.z * g.z > 4.0f * 4.0f) break;
                ++rd.next;
            }
            if (rd.next < pts.size()) {
                e->home = e->patrolTo = pts[rd.next];
                e->patrolWait = 0;
                e->stateTime = 0;
                alive.push_back(rd);
                continue;
            }
            rd.route = -1;  // arrived: on to the buildings
        }
        alive.push_back(rd);
        if (e->aware) continue;  // fighting the player
        // The snatcher goes for the leader; when it reaches him it picks him up and turns back.
        if (rd.snatch && base_.leader && base_.carrier == 0) {
            const Vec3 g{base_.leaderPos.x - e->pos.x, 0, base_.leaderPos.z - e->pos.z};
            if (g.x * g.x + g.z * g.z < 2.0f * 2.0f) {
                base_.carrier = uid;
                alive.back().back = true;
                alive.back().route = rd.route0;
                if (rd.route0 >= 0) alive.back().next = base_.routes[size_t(rd.route0)].size() - 1;
                timmyLine_ = base_.leaderName + " HAS BEEN KIDNAPPED! STOP THEM BEFORE THEY REACH THEIR BASE!";
                timmyLineT_ = 5.0f;
                for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.8f, 0.3f});
                LOGI("Base: %s kidnapped by raider %u", base_.leaderName.c_str(), uid);
                continue;
            }
            e->home = e->patrolTo = base_.leaderPos;
            e->patrolWait = 0;
            e->stateTime = 0;
            continue;
        }
        // Head for a standing building (the nearest to the raider); the base's middle if none.
        {
            float best = 1e18f;
            Vec3 tgt = base_.centre;
            for (const auto& b : base_.bldgs) {
                if (b.state != 3 || b.def == 0) continue;
                const Vec3 bp{b.xf.m[12], b.xf.m[13], b.xf.m[14]};
                const float d2 = (bp.x - e->pos.x) * (bp.x - e->pos.x) + (bp.z - e->pos.z) * (bp.z - e->pos.z);
                if (d2 < best) { best = d2; tgt = bp; }
            }
            e->home = tgt;
        }
        const Vec3 g{e->home.x - e->pos.x, 0, e->home.z - e->pos.z};
        if (g.x * g.x + g.z * g.z > 6.0f * 6.0f) {
            e->patrolTo = e->home;
            e->patrolWait = 0;
            e->stateTime = 0;
        }
        e->siege = false;
        for (auto& b : base_.bldgs) {
            if (b.state != 3 || b.def == 0) continue;
            // A standing Shield Generator halves the damage (ours).
            bool shielded = false;
            for (const auto& g : base_.bldgs) shielded = shielded || (g.state == 3 && std::string(kDefs[g.def].key) == "rpgenerator");
            const Vec3 bp{b.xf.m[12], b.xf.m[13], b.xf.m[14]};
            const Vec3 d{bp.x - e->pos.x, 0, bp.z - e->pos.z};
            if (d.x * d.x + d.z * d.z > 10.0f * 10.0f) continue;
            e->siege = true;  // stands and fires at it (enemy.cpp)
            e->siegeAt = bp + Vec3{0, 2.0f, 0};
            b.hp -= 6.0f * dt * (shielded ? 0.5f : 1.0f);
            b.hitT = float(time_);
            if (frand() < dt * 2.0f) {
                smokePuff(FxTex::Fire, bp + Vec3{(frand() - 0.5f) * 4, 1.0f + frand() * 3, (frand() - 0.5f) * 4}, Vec3{0, 1, 0}, 0.6f, 1.6f, 0.6f, 0.9f, 0.4f);
                smokePuff(FxTex::SmokeBlack, bp + Vec3{(frand() - 0.5f) * 4, 2.0f + frand() * 3, (frand() - 0.5f) * 4}, Vec3{0, 1.2f, 0}, 1.0f, 3.5f, 2.5f, 0.6f, 0.3f);
            }
            if (base_.attackSayT <= 0) {
                base_.attackSayT = 12.0f;
                static const char* lines[] = {"smchbaseattack1", "smchbaseattack2", "smchbaseattack3", "smchbaseattack4"};
                static int line = 0;
                smartieSay(base_.reaper ? "YAN" : "BORJOYZEE", lines[line++ % 4]);
            }
            if (b.hp <= 0) wreckBuilding(b);
            break;
        }
    }
    base_.raiders.swap(alive);
}

// Two turret sites (ours: the template has none): flanking the side the raids come from (the army
// paths' ends, else the barracks, else the player's start), just outside the buildings, on ground
// level with the base and clear of the other sites.
void GameApp::placeTurretSites(int d, const Mat4& baseXf) {
    (void)baseXf;
    const MissionData& md = assetWorld_->mission();
    const ObjectRegistry& reg = assetWorld_->registry();
    // Each path's first and last point (per path id): the paths that end near the base point back
    // to where the raids set out.
    std::vector<std::pair<int, std::pair<Vec3, Vec3>>> paths;
    for (const auto& o : md.objects) {
        if (toLower(reg.modelName(o.type)) != "army_spline") continue;
        auto it = std::find_if(paths.begin(), paths.end(), [&](const auto& e) { return e.first == o.group; });
        const Vec3 p = giantsToRuntime(o.x, o.y, o.z);
        if (it == paths.end()) paths.push_back({o.group, {p, p}});
        else it->second.second = p;
    }
    Vec3 dir;
    for (const auto& pp : paths) {
        const Vec3 e{pp.second.second.x - base_.centre.x, 0, pp.second.second.z - base_.centre.z};
        const Vec3 f{pp.second.first.x - base_.centre.x, 0, pp.second.first.z - base_.centre.z};
        if (e.x * e.x + e.z * e.z < 150.0f * 150.0f && f.x * f.x + f.z * f.z > 1.0f) dir = dir + normalize(f);
    }
    if (dir.x * dir.x + dir.z * dir.z < 1.0f)
        for (const auto& w : assetWorld_->worldObjects())
            if (w.name == "guard_army_barracks") { dir = Vec3{w.centre.x - base_.centre.x, 0, w.centre.z - base_.centre.z}; break; }
    if (dir.x * dir.x + dir.z * dir.z < 1.0f) dir = assetWorld_->spawnPoint() - base_.centre;
    dir.y = 0;
    if (dir.x * dir.x + dir.z * dir.z < 1e-4f) dir = Vec3{1, 0, 0};
    dir = normalize(dir);
    float reach = 0;
    for (const auto& b : base_.bldgs) reach = std::max(reach, length(Vec3{b.xf.m[12] - base_.centre.x, 0, b.xf.m[14] - base_.centre.z}));
    const float radius = std::max(12.0f, std::min(40.0f, reach + 6.0f));
    const AssetFS& fs = assetWorld_->fs();
    ModelCache* mc = assetWorld_->models();
    const float a0 = std::atan2(dir.x, dir.z);
    for (int side = -1; side <= 1; side += 2) {
        bool placed = false;
        // (2026-10-29: r4a's base, with the Gyrocopter Pad's site, had no level ground on one side within
        // 1.65 rad and 8 m: the search goes on round to 2.85 rad and 16 m out before giving up.)
        for (int k = 0; k < 16 && !placed; ++k) {
            const float a = a0 + float(side) * (0.6f + 0.15f * float(k));
            for (float r = radius; r <= radius + 16.0f && !placed; r += 4.0f) {
                Vec3 p = base_.centre + Vec3{std::sin(a) * r, 0, std::cos(a) * r};
                float h;
                if (!assetWorld_->terrain().heightAt(p.x, p.z, h) || h < player_.seaLevel - 0.3f || std::fabs(h - base_.centre.y) > 4.0f) continue;
                bool clear = true;
                for (const auto& o : base_.bldgs) clear = clear && length(Vec3{o.xf.m[12] - p.x, 0, o.xf.m[14] - p.z}) > 7.0f;
                if (!clear) continue;
                p.y = h;
                Base::Bldg bl;
                bl.def = d;
                // Facing out, toward where the raids come from.
                bl.xf = Mat4::translation(p) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, std::atan2(-(p.x - base_.centre.x), -(p.z - base_.centre.z)) + kPi));
                auto sm = std::make_unique<SkinnedModel>();
                if (!sm->load(fs, *mc, renderer_, kDefs[d].model, kDefs[d].anim)) { LOGW("Base: %s unavailable", kDefs[d].model); return; }
                // The head (node "center" and what hangs from it) turns about its own pivot to aimYaw.
                const size_t idx = base_.bldgs.size();
                sm->setPoseModifier([this, idx](const AnmAnimation& clip, const std::vector<std::vector<int>>& children, std::vector<Mat4>& world) {
                    if (idx >= base_.bldgs.size()) return;
                    int c = -1;
                    for (size_t i = 0; i < clip.nodes.size(); ++i)
                        if (toLower(clip.nodes[i].name) == "center") { c = int(i); break; }
                    if (c < 0 || size_t(c) >= world.size()) return;
                    const Vec3 pv{world[size_t(c)].m[12], world[size_t(c)].m[13], world[size_t(c)].m[14]};
                    const Mat4 rot = Mat4::translation(pv) * Mat4::rotation(Quat::fromAxisAngle({0, 0, 1}, base_.bldgs[idx].aimYaw)) * Mat4::translation(pv * -1.0f);
                    std::vector<int> stack{c};
                    while (!stack.empty()) {
                        const int n = stack.back();
                        stack.pop_back();
                        world[size_t(n)] = rot * world[size_t(n)];
                        if (size_t(n) < children.size()) for (int ch : children[size_t(n)]) stack.push_back(ch);
                    }
                });
                bl.model = std::move(sm);
                base_.bldgs.push_back(std::move(bl));
                placed = true;
                LOGI("Base: %s site at (%.1f %.1f %.1f)", kDefs[d].label, p.x, p.y, p.z);
            }
        }
        if (!placed) LOGW("Base: no level ground for a %s on side %d", kDefs[d].label, side);
    }
}

// A raided building falls: noise and dust, and its site is empty again (ordered again from the Build Pad).
void GameApp::wreckBuilding(Base::Bldg& b) {
    const Vec3 bp{b.xf.m[12], b.xf.m[13], b.xf.m[14]};
    if (isKey(b.def, "gyropad")) gyro_.padY = -1e9f;  // (its gyrocopter comes down off the wreck: updateGyroParked)
    b.state = 0;
    b.progress = 0;
    b.hp = 0;
    b.openClip = -1;  // (a rebuilt gate loads its clip again)
    b.openT = 0;
    if (b.collision >= 0) assetWorld_->collisionMutable().setEnabled(b.collision, false);
    b.damaged = nullptr;
    dustBurst(bp, 6.0f, 20);
    explode(bp + Vec3{0, 2, 0}, 3.0f, 0.0f, true);  // the noise and flash, no damage
    timmyLine_ = std::string(bldgLabel(b.def, b.level)) + " DESTROYED! REBUILD IT ON THE BUILD PAD.";
    timmyLineT_ = 4.0f;
    LOGI("Base: %s destroyed by raiders", bldgLabel(b.def, b.level));
}

// Gates open while you're within 8 m (their _open clip, 1.6-3.3 s) and shut behind you; shut
// (less than a third open) they are solid like the walls.
void GameApp::updateGates(float dt) {
    for (auto& b : base_.bldgs) {
        if (b.state != 3 || b.openClip < 0 || !b.model) continue;
        const float dur = b.model->animDuration(b.openClip);
        const Vec3 site{b.xf.m[12], b.xf.m[13], b.xf.m[14]};
        const bool near = length(Vec3{player_.position.x - site.x, 0, player_.position.z - site.z}) < 8.0f &&
                          std::fabs(player_.position.y - site.y) < 5.0f;
        const float was = b.openT;
        b.openT = std::max(0.0f, std::min(dur * 0.999f, b.openT + (near ? dt : -dt)));
        if (was == 0.0f && b.openT > 0.0f) { sfx("ph_enter", site, 0.8f, 0.8f); LOGI("Base: %s opens", bldgLabel(b.def, b.level)); }
        if (was > 0.0f && b.openT == 0.0f) LOGI("Base: %s shut", bldgLabel(b.def, b.level));
        if (b.collision >= 0) assetWorld_->collisionMutable().setEnabled(b.collision, b.openT < dur * 0.33f);
    }
}

// Turrets (ours): a standing turret turns its head to the nearest Reaper within 45 m that it can
// see and fires a round every 0.7 s once lined up (18 damage: about five to drop a Reaper).
void GameApp::updateTurrets(float dt) {
    for (auto& b : base_.bldgs) {
        if (b.state != 3 || !isKey(b.def, "turret")) continue;
        const Vec3 site{b.xf.m[12], b.xf.m[13], b.xf.m[14]};
        const Vec3 muzzle = site + Vec3{0, 3.0f, 0};
        const Enemy* tgt = nullptr;
        float best = 45.0f * 45.0f;
        for (const auto& e : enemies_.enemies) {
            if (e.state == EnemyState::Dead || e.remove) continue;
            const Vec3 v = e.pos + Vec3{0, 1.1f, 0} - muzzle;
            const float d2 = dot(v, v);
            if (d2 >= best) continue;
            const float d = std::sqrt(d2);
            float t;
            Vec3 n;
            const Vec3 dirv = v * (1.0f / std::max(d, 1e-3f));
            if (worldRay_ && d > 3.0f && worldRay_(muzzle + dirv * 2.0f, dirv, d - 3.0f, t, n)) continue;  // behind a wall or a hill
            best = d2;
            tgt = &e;
        }
        b.fireT = std::max(0.0f, b.fireT - dt);
        if (!tgt) { b.target = 0; continue; }
        b.target = tgt->uid;
        // The head's yaw in the model's own (game, Z-up) frame: its barrel points along -Y there.
        const Mat4 inv = affineInverse(b.xf);
        const Vec4 lv = inv * Vec4{tgt->pos.x - site.x, 0, tgt->pos.z - site.z, 0};
        const float want = std::atan2(-lv.x, -lv.z);  // runtime local (-Z forward) = game -Y
        float diff = want - b.aimYaw;
        while (diff > kPi) diff -= 2 * kPi;
        while (diff < -kPi) diff += 2 * kPi;
        b.aimYaw += std::max(-2.5f * dt, std::min(2.5f * dt, diff));
        if (std::fabs(diff) < 0.15f && b.fireT <= 0) {
            b.fireT = 0.7f;
            const Vec3 aim = tgt->pos + Vec3{0, 1.1f, 0} + Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} * 0.4f;
            const Vec3 dv = normalize(aim - muzzle);
            base_.turretShots.push_back({muzzle + dv * 1.5f, dv * 70.0f, 1.0f, tgt->uid, 18.0f});
            sfx(base_.reaper ? "me_rtfr1" : "me_gtfr1", muzzle, 0.8f, 0.95f + 0.1f * frand());
        }
    }
    stepTurretShots(base_.turretShots, dt, "Base: a turret");
}

// Turret rounds (a base turret's or Baz's own): a hit within 0.8 m of the target's body.
void GameApp::stepTurretShots(std::vector<Base::TurretShot>& shots, float dt, const char* who) {
    for (size_t i = 0; i < shots.size();) {
        auto& t = shots[i];
        const Vec3 from = t.pos;
        t.pos = t.pos + t.vel * dt;
        t.life -= dt;
        bool done = t.life <= 0;
        // A Ripper (the portable turret's: the id carries kTurretCreature).
        for (size_t k = 0; k < creatures_.creatures.size() && !done && (t.target & kTurretCreature); ++k) {
            const Creature& c = creatures_.creatures[k];
            if ((c.uid | kTurretCreature) != t.target || c.state == CreatureState::Dead) continue;
            const Vec3 m = c.pos + Vec3{0, 0.5f * c.scale, 0};
            const Vec3 seg = t.pos - from;
            const float sl = std::max(1e-6f, dot(seg, seg));
            const float u = std::max(0.0f, std::min(1.0f, dot(m - from, seg) / sl));
            if (length(from + seg * u - m) > 0.6f * std::max(1.0f, c.scale)) break;
            const uint32_t uid = c.uid;
            if (thrownHitCreature(int(k), t.damage, from + seg * u, normalize(t.vel))) LOGI("%s shot Ripper %u", who, uid);
            done = true;
        }
        for (size_t k = 0; k < enemies_.enemies.size() && !done && !(t.target & kTurretCreature); ++k) {
            const Enemy& e = enemies_.enemies[k];
            if (e.uid != t.target || e.state == EnemyState::Dead) continue;
            const Vec3 c = e.pos + Vec3{0, 1.1f, 0};
            // Closest approach of this step's segment to the body.
            const Vec3 seg = t.pos - from;
            const float sl = std::max(1e-6f, dot(seg, seg));
            const float u = std::max(0.0f, std::min(1.0f, dot(c - from, seg) / sl));
            if (length(from + seg * u - c) > 0.8f) break;
            const Vec3 hit = from + seg * u;
            const bool killed = enemies_.damage(int(k), t.damage);
            if (killed && e.objective >= 0) completeObjective(e.objective);
            if (killed) LOGI("%s shot Reaper %u", who, e.uid);
            bleed(hit, normalize(t.vel), killed, 1.0f);
            if (killed) blood_.pool(e.pos, 1.3f);
            sfx(killed ? "rpman_die2" : "ex_bdys1", hit, killed ? 1.0f : 0.35f);
            done = true;
        }
        if (done) { shots[i] = shots.back(); shots.pop_back(); }
        else ++i;
    }
}

void GameApp::drawTurretShots(const std::vector<Base::TurretShot>& shots, const Vec4& tint) {
    if (!boltModel_) return;
    for (const auto& t : shots) {
        const Quat r = rotationBetween({0, 0, -1}, t.vel);
        for (const auto& part : boltModel_->parts) {
            DrawParams dp;
            dp.texture = part.texture;
            dp.blend = BlendMode::Additive;
            dp.unlit = true;
            dp.depthWrite = false;
            dp.doubleSided = true;
            dp.tint = tint;
            renderer_.draw(part.mesh, Mat4::translation(t.pos) * Mat4::rotation(r) * Mat4::scale({0.35f, 0.35f, 1.2f}), dp);
        }
    }
}

void GameApp::renderBase(const Vec3& eye) {
    if (!base_.on) return;
    renderCrew(eye, false);  // includes Smarties on their way back from the hatches
    if (length(eye - base_.centre) > 260.0f) return;
    for (auto& b : base_.bldgs) {
        if (b.state == 0) continue;
        if (b.still) drawModel(b.still, b.xf, false);
        if (!b.model) continue;
        // Below half health: the damaged model (a gate shows it only while shut).
        if (b.state == 3 && b.damaged && b.hp < 0.5f * b.maxHp && b.openT <= 0.0f) {
            drawModel(b.damaged, b.xf, false);
            continue;
        }
        SkinnedModel* m = b.model.get();
        if (b.state == 3 && b.openClip >= 0) {  // a gate: its open clip
            if (std::fabs(b.openT - b.posedOpen) > 1e-3f) {
                m->poseBlend(renderer_, b.openClip, b.openT, b.openClip, b.openT, 0.0f);
                b.posedOpen = b.openT;
            }
            m->render(renderer_, b.xf);
            continue;
        }
        const float d = b.model->animDuration(0);
        if (isKey(b.def, "turret") && b.state == 3 && std::fabs(b.aimYaw - b.posedYaw) > 0.01f) b.posedT = -1;  // re-aimed
        // Skinned on the CPU: only re-posed while it is going up (a standing building keeps its pose).
        const float pt = d > 0 ? std::min(d, b.progress * d) : 0.0f;
        if (std::fabs(pt - b.posedT) > 1e-3f) { b.posedYaw = b.aimYaw; m->pose(renderer_, pt); b.posedT = pt; }
        m->render(renderer_, b.xf);
    }
    // Turret rounds: hot streaks (the Bulldog's orange, the Hellfire's red).
    drawTurretShots(base_.turretShots, base_.reaper ? Vec4{2.6f, 0.6f, 0.3f, 1.0f} : Vec4{2.4f, 1.5f, 0.5f, 1.0f});
    if (base_.firepit) {
        const float d = base_.firepit->animDuration(0);
        base_.firepit->pose(renderer_, d > 0 ? std::fmod(float(time_), d) : 0.0f);
        base_.firepit->render(renderer_, base_.firepitXf);
    }
    if (base_.dropModel) drawModel(base_.dropModel, base_.dropXf, false);
    if (!text_.ready()) return;
    const Pose head = rig_.toWorld(lastInput_.head);
    const Quat face = Quat::fromAxisAngle({0, 1, 0}, yawOf(head.orientation));
    auto label = [&](const Vec3& at, const std::string& s, const Vec4& col, float size, float wrap = 20.0f) {
        float w = 0, h = 0;
        const MeshId tm = text_.mesh(s, size, wrap, &w, &h);
        if (!tm) return;
        const Mat4 xf = Mat4::translation(at) * Mat4::rotation(face);
        text_.drawPanel(xf, w + 0.06f, h + 0.03f, {0, 0, 0, 0.5f}, true);
        text_.draw(tm, xf * Mat4::translation({0, 0, 0.002f}), col, true);
    };
    // Signs over the sites under construction, and the store at the drop-off.
    for (const auto& b : base_.bldgs) {
        if (b.state != 1 || length(eye - Vec3{b.xf.m[12], b.xf.m[13], b.xf.m[14]}) > 60.0f) continue;
        char s[64];
        bool worked = false;
        for (const auto& w : base_.crew) worked = worked || (w.joined && w.site >= 0 && &base_.bldgs[size_t(w.site)] == &b);
        if (worked) std::snprintf(s, sizeof s, "%s  BUILDING... %d%%", bldgLabel(b.def, b.level), int(b.progress * 100.0f));
        else std::snprintf(s, sizeof s, "%s  AWAITING MORE SMARTIES", bldgLabel(b.def, b.level));
        label(Vec3{b.xf.m[12], b.xf.m[13] + 6.0f, b.xf.m[14]}, s, {1.0f, 0.85f, 0.3f, 1.0f}, 0.35f);
    }
    for (const auto& b : base_.bldgs) {
        if (b.state != 3 || float(time_) - b.hitT > 6.0f || length(eye - Vec3{b.xf.m[12], b.xf.m[13], b.xf.m[14]}) > 80.0f) continue;
        char s[64];
        std::snprintf(s, sizeof s, "%s UNDER ATTACK %d%%", bldgLabel(b.def, b.level), int(100.0f * b.hp / std::max(1.0f, b.maxHp)));
        label(Vec3{b.xf.m[12], b.xf.m[13] + 6.0f, b.xf.m[14]}, s, {1.0f, 0.35f, 0.25f, 1.0f}, 0.35f);
    }
    if (length(eye - base_.dropAt) < 40.0f) {
        char s[64];
        std::snprintf(s, sizeof s, "%s DROP-OFF  STORE %d", base_.reaper ? "SOUL" : "MEAT", base_.food);
        label(base_.dropAt + Vec3{0, 2.6f, 0}, s, base_.food > 0 ? Vec4{0.6f, 1.0f, 0.6f, 1.0f} : Vec4{1.0f, 0.4f, 0.3f, 1.0f}, 0.18f);
    }
    if (length(eye - base_.padAt) < 40.0f && !base_.onPad) {
        label(base_.padAt + Vec3{0, 3.0f, 0}, base_.complete ? "BUILD PAD - BASE COMPLETE" : "BUILD PAD - STAND HERE TO BUILD",
              {0.6f, 0.9f, 1.0f, 1.0f}, 0.18f);
    }
    // The menu: a card per building you can order, at arm's length round the pad.
    for (const auto& it : base_.items) {
        const BldgDef& d = kDefs[it.def];
        const int st = baseKindState(it.def), lv = baseKindLevel(it.def);
        const Upgrade* up = upgradeFor(it.def);
        const bool canUp = st == 3 && up && lv == 0;
        const bool afford = (st == 0 && base_.food >= d.meat) || (canUp && base_.food >= up->meat);
        char s[80];
        if (st == 0) std::snprintf(s, sizeof s, "%s: %d %s", bldgLabel(it.def, lv), d.meat, base_.reaper ? "SOULS" : "MEAT");
        else if (canUp) std::snprintf(s, sizeof s, "UPGRADE TO %s: %d %s", up->label, up->meat, base_.reaper ? "SOULS" : "MEAT");
        else std::snprintf(s, sizeof s, "%s: %s", bldgLabel(it.def, lv), st == 3 ? "COMPLETED!" : "BUILDING...");
        const float bob = 0.03f * std::sin(float(time_) * 2.0f + it.pos.x);
        label(it.pos + Vec3{0, bob, 0}, s, afford ? Vec4{0.6f, 1.0f, 0.6f, 1.0f} : Vec4{0.75f, 0.75f, 0.75f, 1.0f}, 0.045f, 0.3f);
    }
    if (base_.onPad) renderBaseTable();
    if (base_.onPad) {
        char s[64];
        std::snprintf(s, sizeof s, "%s STORE %d   SMARTIES %d   TOUCH A SIGN TO BUILD", kRes, base_.food, crewCount());
        label(base_.menuAt + Vec3{-std::sin(base_.menuYaw) * 1.2f, 1.8f, -std::cos(base_.menuYaw) * 1.2f}, s, {0.6f, 0.9f, 1.0f, 1.0f},
              0.05f);
    }
}

// The table map (2026-10-28, ours - the original directs building from the pad by its menu only): the base in
// miniature on a dark glass disc. The buildings standing or going up are the real models drawn small (in their
// current pose: a site under construction shows its scaffold growing); a free site is a small square - green if
// the store can pay for it - with its name; touching it orders it (updateBase). Dots: you (blue), the Smarties'
// leader (gold), Reapers about (red).
void GameApp::renderBaseTable() {
    if (base_.tableScale <= 0 || !text_.ready()) return;
    const float s = base_.tableScale;
    auto flat = [](const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); };
    const Quat flatUp = Quat::fromAxisAngle({1, 0, 0}, -1.5707963f);
    // (the rim first: the panels don't write depth, so the later one lies on top)
    text_.drawPanel(Mat4::translation(base_.tableAt + Vec3{0, -0.002f, 0}) * Mat4::rotation(flatUp), 0.67f, 0.67f, {0.4f, 0.75f, 1.0f, 0.5f}, true);
    text_.drawPanel(Mat4::translation(base_.tableAt) * Mat4::rotation(flatUp), 0.64f, 0.64f, {0.02f, 0.06f, 0.1f, 0.92f}, true);
    const Mat4 toTable = Mat4::translation(base_.tableAt + Vec3{0, 0.006f, 0}) * Mat4::scale({s, s, s}) * Mat4::translation(base_.centre * -1.0f);
    const Pose head = rig_.toWorld(lastInput_.head);
    const Quat face = Quat::fromAxisAngle({0, 1, 0}, yawOf(head.orientation));
    auto tag = [&](const Vec3& at, const char* str, const Vec4& col) {
        float w = 0, h = 0;
        const MeshId tm = text_.mesh(str, 0.014f, 0.2f, &w, &h);
        if (!tm) return;
        text_.draw(tm, Mat4::translation(at) * Mat4::rotation(face), col, true);
    };
    auto dot = [&](const Vec3& world, float size, const Vec4& col) {
        text_.drawPanel(Mat4::translation(baseTablePoint(world) + Vec3{0, 0.004f, 0}) * Mat4::rotation(flatUp), size, size, col, true);
    };
    for (const auto& b : base_.bldgs) {
        const Vec3 site{b.xf.m[12], b.xf.m[13], b.xf.m[14]};
        if (b.state == 0) {
            if (b.def == 0) continue;
            const bool afford = base_.food >= kDefs[b.def].meat;
            dot(site, 0.03f, afford ? Vec4{0.45f, 1.0f, 0.5f, 0.9f} : Vec4{0.6f, 0.6f, 0.6f, 0.7f});
            tag(baseTablePoint(site) + Vec3{0, 0.03f, 0}, kDefs[b.def].label, afford ? Vec4{0.6f, 1.0f, 0.6f, 1.0f} : Vec4{0.75f, 0.75f, 0.75f, 1.0f});
            continue;
        }
        const Mat4 xf = toTable * b.xf;
        if (b.still) drawModel(b.still, xf, false);
        if (b.model) {
            if (b.state == 1) b.model->renderGhost(renderer_, xf, {0.5f, 0.8f, 1.0f});
            else b.model->render(renderer_, xf);
        }
        if (b.state == 3 && float(time_) - b.hitT < 6.0f) dot(site, 0.04f, {1.0f, 0.3f, 0.2f, 0.6f});  // under attack
    }
    for (const auto& e : enemies_.enemies)
        if (e.state != EnemyState::Dead && flat(e.pos, base_.centre) * s < 0.31f) dot(e.pos, 0.014f, {1.0f, 0.2f, 0.15f, 1.0f});
    if (base_.leader && flat(base_.leaderPos, base_.centre) * s < 0.31f) dot(base_.leaderPos, 0.016f, {1.0f, 0.85f, 0.2f, 1.0f});
    dot(player_.position, 0.016f, {0.3f, 0.7f, 1.0f, 1.0f});
    static bool logged = false;
    if (!logged) { logged = true; LOGI("Base: table map shown (%.4f m of table per m, %zu sites)", s, base_.bldgs.size()); }
}

// (2026-10-28) The Repair Gun's beam: aimed at a base building within 50 m (the data's range, 50 game units read
// as metres here - ours), held: +50 every 1.5 s (the data's -50 damage per 1.5 s), shown as green fire along the beam
// and the original repair ring (EFX_repair_ring) on the building.
void GameApp::stopRepairLoop() {
    if (!repairLoop_) return;
    if (audio_) audio_->stop(repairLoop_);
    repairLoop_ = 0;
    sfx2D("me_repnd", 0.6f);
}

void GameApp::updateRepairGun(float dt, bool trigger, const Vec3& muzzle, const Vec3& fwd) {
    repairOn_ = false;
    if (!trigger) { repairTarget_ = -1; repairTick_ = 0; stopRepairLoop(); return; }
    if (!repairLoop_ && audio_) {  // (the data's repair gun sounds: start, then its loop while the beam is on)
        sfx2D("me_repst", 0.7f);
        repairLoop_ = audio_->play2D("me_replp", 0.5f, 1.0f, true);
    }
    int best = -1;
    float bestT = 50.0f;
    Vec3 hit = muzzle + fwd * 50.0f;
    for (size_t i = 0; i < base_.bldgs.size(); ++i) {
        const Base::Bldg& b = base_.bldgs[i];
        if (b.state == 0) continue;
        Vec3 mn, mx;
        if (b.model) { mn = b.model->restMin(); mx = b.model->restMax(); }
        else if (b.still) { mn = b.still->boundsMin; mx = b.still->boundsMax; }
        else continue;
        const Vec4 cw = b.xf * Vec4{0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z), 1};
        const Vec3 c{cw.x, cw.y, cw.z};
        const float r = 0.35f * length(mx - mn);
        const float t = dot(c - muzzle, fwd);
        if (t < 0 || t > bestT + r) continue;
        if (length(muzzle + fwd * t - c) > r) continue;
        best = int(i);
        bestT = std::max(0.0f, t - 0.5f * r);
        hit = muzzle + fwd * bestT;
    }
    repairOn_ = true;
    repairTo_ = hit;
    if (frand() < dt * 40.0f) {  // green fire along the beam
        const float u = frand();
        smokePuff(FxTex::FireGreen, muzzle + (hit - muzzle) * u, Vec3{0, 0.4f, 0}, 0.18f, 0.05f, 0.3f, 1.0f, 0.0f);
    }
    // Test hook GVR_REPAIR_AUTO: the nearest damaged standing building, wherever the gun points (scripted runs).
    static const bool autoAim = std::getenv("GVR_REPAIR_AUTO") != nullptr;
    if (autoAim) {
        float bd = 1e9f;
        for (size_t i = 0; i < base_.bldgs.size(); ++i) {
            const Base::Bldg& b = base_.bldgs[i];
            const Vec3 c{b.xf.m[12], b.xf.m[13] + 2.0f, b.xf.m[14]};
            if (b.state == 3 && b.hp < b.maxHp && length(c - muzzle) < bd) { bd = length(c - muzzle); best = int(i); hit = c; }
        }
        repairTo_ = hit;
    }
    if (best != repairTarget_) {
        repairTarget_ = best;
        repairTick_ = 0;
        if (best >= 0) {
            const Base::Bldg& b = base_.bldgs[size_t(best)];
            if (b.state == 1) {  // the game's own line
                const std::string t = gameText_ ? gameText_->get("hintwaitrepair") : std::string();
                timmyLine_ = t.empty() ? "YOU CANNOT REPAIR A BUILDING UNTIL IT IS FULLY BUILT!" : t;
                timmyLineT_ = 3.0f;
            }
        }
    }
    if (best < 0) return;
    Base::Bldg& b = base_.bldgs[size_t(best)];
    if (b.state != 3 || b.hp >= b.maxHp) return;
    repairTick_ += dt;
    b.hp = std::min(b.maxHp, b.hp + (50.0f / 1.5f) * dt);
    if (repairTick_ >= 1.5f) {
        repairTick_ -= 1.5f;
        haptics_.push_back({rig_.turnHand(), 0.3f, 0.05f});
        LOGI("Repair gun: %s %.0f / %.0f", bldgLabel(b.def, b.level), b.hp, b.maxHp);
        if (b.hp >= b.maxHp) {
            timmyLine_ = std::string(bldgLabel(b.def, b.level)) + " REPAIRED";
            timmyLineT_ = 2.5f;
        }
    }
}

void GameApp::renderRepairBeam() {
    if (!repairOn_ || !repairRing_ || hero_ != Hero::Mecc || weaponSel_ != 10) return;
    const float t = float(time_);
    const Vec3 eye = renderer_.currentView().eyePosition;
    const Vec3 d = eye - repairTo_;
    const float yaw = std::atan2(d.x, d.z);
    for (int k = 0; k < 2; ++k)
        drawModel(repairRing_, Mat4::translation(repairTo_) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, yaw) * Quat::fromAxisAngle({0, 0, 1}, t * (2.0f + float(k)))) *
                                   Mat4::scale(Vec3{1, 1, 1} * (repairTarget_ >= 0 ? 8.0f + 3.0f * float(k) : 3.0f)),  // (the model is 0.28 m across)
                  true, 0.9f);
}

}  // namespace gvr
