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
// (hand or walk into it) to start it. Vimp meat you carry is stored when you walk past the meat
// drop-off. Not yet: building collision, enemies attacking the base, upgrades to stone, and the
// POP-UP BOMB the Smarties make once the base is complete.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "assets/skin_anim.h"
#include "core/log.h"
#include "game/cutscene.h"
#include "scene/asset_world.h"
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
constexpr float kPadRadius = 4.0f;     // m: standing on the Build Pad
constexpr float kDropRadius = 3.5f;    // m: the meat drop-off
constexpr int kWorkers = 6;            // ours: Borjoyzee's workforce

bool baseMission(const std::string& mission, bool reaper) {
    if (const char* f = std::getenv("GVR_BASE")) return *f == '1';
    return toLower(mission).rfind(reaper ? "r4a" : "5c", 0) == 0;
}
}  // namespace

void GameApp::initBase() {
    base_ = Base{};
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
    for (const auto& o : md.objects)
        if (!found && toLower(reg.modelName(o.type)) == "marker") {
            base_.centre = giantsToRuntime(o.x, o.y, o.z);
            found = true;
        }
    if (!found) base_.centre = assetWorld_->spawnPoint();
    float gy;
    if (assetWorld_->terrain().heightAt(base_.centre.x, base_.centre.z, gy)) base_.centre.y = gy;
    const Mat4 baseXf = Mat4::translation(base_.centre);
    ModelCache* mc = assetWorld_->models();
    for (int d = 0; d < kNumDefs; ++d) {
        std::string slots = kDefs[d].slot;
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
            if (sm->load(fs, *mc, renderer_, kDefs[d].model, kDefs[d].anim)) bl.model = std::move(sm);
            else bl.still = mc->get(std::string(kDefs[d].model) + ".gbs", kDefs[d].anim, true);  // static (wood_command)
            if (!bl.model && !bl.still) LOGW("Base: %s unavailable", kDefs[d].model);
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
            if (!base_.firepit->load(fs, *mc, renderer_, "Rp_soul_dropoff", "Rp_soul_dropoff.anm")) {
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
    base_.padAt = Vec3{base_.bldgs.front().xf.m[12], base_.bldgs.front().xf.m[13], base_.bldgs.front().xf.m[14]};
    base_.on = true;
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
    // Test hooks: GVR_BASE_FOOD=n stocks the store; GVR_BASE_ORDER=all orders everything at the start.
    if (const char* f = std::getenv("GVR_BASE_FOOD"); f && base_.traceT == 0.0f) base_.food = std::atoi(f);
    if (const char* o = std::getenv("GVR_BASE_ORDER"); o && !base_.autoOrdered) {
        base_.autoOrdered = true;
        if (std::string(o) == "all")
            for (int d : baseOffers()) { base_.food += kDefs[d].meat; orderBuilding(d); }
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
    int busy = 0;
    for (auto& b : base_.bldgs) {
        if (b.state != 1) continue;
        if (busy >= 2) break;  // two crews of three
        ++busy;
        b.progress = std::min(1.0f, b.progress + dt * base_.speed / std::max(1.0f, kDefs[b.def].seconds));
        b.fxT -= dt;
        if (b.fxT <= 0) {
            b.fxT = 0.35f + 0.3f * frand();
            const Vec3 at{b.xf.m[12] + (frand() - 0.5f) * 4.0f, b.xf.m[13] + 0.3f, b.xf.m[14] + (frand() - 0.5f) * 4.0f};
            smokePuff(FxTex::Dust, at, Vec3{0, 0.6f, 0}, 0.6f, 2.0f, 1.6f, 0.45f, 0.1f);
        }
        if (b.progress >= 1.0f) {
            b.state = 3;
            b.maxHp = b.hp = (kDefs[b.def].core ? 400.0f : 250.0f) * (b.level > 0 ? 1.6f : 1.0f);
            // Solid once standing: the building's own collision mesh (<model>_coll), else its model.
            ModelCache* mc = assetWorld_->models();
            const RenderModel* coll = mc->get(std::string(bldgModel(b.def, b.level)) + "_coll.gbs", "", false);
            if (!coll || coll->collisionTris.empty()) coll = mc->get(std::string(bldgModel(b.def, b.level)) + ".gbs", "", false);
            if (b.collision >= 0) {
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
            if (allOfKind) {
                // What a finished building gives (ours, after the game's descriptions): the Giftshop
                // "is full of weapons and other nifty stuff" - the shop counter opens in it; the
                // Workshop is "where your Smarties go for tools" - they build faster from then on.
                if (std::string(kDefs[b.def].key) == "giftshop1" && b.level > 0 && shopOn_) {
                    // "Even more weapons are available in this upgraded Gift Shop!": a pop-up bomb joins the counter.
                    shopItems_.push_back({3, shopAt_, false});
                    if (!shopInside_) shopAnchored_ = false;  // laid out afresh on the next visit
                } else if (std::string(kDefs[b.def].key) == "giftshop1") {
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
                sfx2D("pu_hlth", 0.9f, 0.8f);
                LOGI("Base: %s completed", bldgLabel(b.def, b.level));
            }
        }
    }
    // Test hook: GVR_BASE_UPGRADE=1 upgrades every kind as soon as it stands.
    if (static const bool autoUp = std::getenv("GVR_BASE_UPGRADE") != nullptr; autoUp)
        for (int d = 1; d < kNumDefs; ++d)
            if (baseKindState(d) == 3 && baseKindLevel(d) == 0 && upgradeFor(d)) { base_.food += upgradeFor(d)->meat; orderUpgrade(d); }
    updateRaids(dt);
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
    // Within arm's reach without stepping: up to four per row, two rows, 0.8 m out.
    for (size_t i = 0; i < offers.size(); ++i) {
        const size_t row = i / 4, col = i % 4;
        const float inRow = float(std::min<size_t>(4, offers.size() - row * 4));
        const float a = base_.menuYaw + (float(col) - 0.5f * (inRow - 1.0f)) * 0.55f;
        base_.items.push_back({offers[i], base_.menuAt + Vec3{-std::sin(a) * 0.8f, row == 0 ? 1.35f : 1.0f, -std::cos(a) * 0.8f}});
    }
    base_.touchCool = std::max(0.0f, base_.touchCool - dt);
    if (base_.touchCool > 0) return;
    Vec3 hands[2];
    int nh = 0;
    for (int h = 0; h < 2; ++h)
        if (in.hand[h].gripPoseValid) hands[nh++] = rig_.toWorld(in.hand[h].gripPose).position;
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
                base_.raiders.push_back({e.uid, r, i + 1});
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
                base_.raiders.push_back({e.uid, -1, 0});
                ++placed;
            }
            LOGI("Base raid %d: %d Reapers", base_.raids, placed);
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
        static const bool trace = std::getenv("GVR_BASE_TRACE") != nullptr;
        if (trace && int(base_.traceT) % 10 == 0 && int(base_.traceT - dt) % 10 != 0)
            LOGI("raider %u at (%.1f %.1f) home (%.1f %.1f) state %d aware %d", uid, e->pos.x, e->pos.z, e->home.x, e->home.z, int(e->state), int(e->aware));
        if (e->aware) continue;  // fighting the player
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
        for (auto& b : base_.bldgs) {
            if (b.state != 3 || b.def == 0) continue;
            const Vec3 bp{b.xf.m[12], b.xf.m[13], b.xf.m[14]};
            const Vec3 d{bp.x - e->pos.x, 0, bp.z - e->pos.z};
            if (d.x * d.x + d.z * d.z > 10.0f * 10.0f) continue;
            b.hp -= 6.0f * dt;
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
            if (b.hp <= 0) {
                b.state = 0;
                b.progress = 0;
                b.hp = 0;
                if (b.collision >= 0) assetWorld_->collisionMutable().setEnabled(b.collision, false);
                dustBurst(bp, 6.0f, 20);
                explode(bp + Vec3{0, 2, 0}, 3.0f, 0.0f, true);  // the noise and flash, no damage
                timmyLine_ = std::string(bldgLabel(b.def, b.level)) + " DESTROYED! REBUILD IT ON THE BUILD PAD.";
                timmyLineT_ = 4.0f;
                LOGI("Base: %s destroyed by raiders", bldgLabel(b.def, b.level));
            }
            break;
        }
    }
    base_.raiders.swap(alive);
}

void GameApp::renderBase(const Vec3& eye) {
    if (!base_.on) return;
    if (length(eye - base_.centre) > 260.0f) return;
    for (auto& b : base_.bldgs) {
        if (b.state == 0) continue;
        if (b.still) drawModel(b.still, b.xf, false);
        if (!b.model) continue;
        const float d = b.model->animDuration(0);
        b.model->pose(renderer_, d > 0 ? std::min(d, b.progress * d) : 0.0f);
        b.model->render(renderer_, b.xf);
    }
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
        std::snprintf(s, sizeof s, "%s  BUILDING... %d%%", bldgLabel(b.def, b.level), int(b.progress * 100.0f));
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
    if (base_.onPad) {
        char s[64];
        std::snprintf(s, sizeof s, "%s STORE %d   SMARTIES %d   TOUCH A SIGN TO BUILD", kRes, base_.food, kWorkers);
        label(base_.menuAt + Vec3{-std::sin(base_.menuYaw) * 1.2f, 1.8f, -std::cos(base_.menuYaw) * 1.2f}, s, {0.6f, 0.9f, 1.0f, 1.0f},
              0.05f);
    }
}

}  // namespace gvr
