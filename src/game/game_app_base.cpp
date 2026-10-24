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
const BldgDef kDefs[] = {
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
constexpr int kNumDefs = int(sizeof(kDefs) / sizeof(kDefs[0]));
constexpr float kPadRadius = 4.0f;     // m: standing on the Build Pad
constexpr float kDropRadius = 3.5f;    // m: the meat drop-off
constexpr int kWorkers = 6;            // ours: Borjoyzee's workforce

bool baseMission(const std::string& mission) {
    if (const char* f = std::getenv("GVR_BASE")) return *f == '1';
    return toLower(mission).rfind("5c", 0) == 0;
}
}  // namespace

void GameApp::initBase() {
    base_ = Base{};
    if (!assetWorld_ || hero_ != Hero::Mecc || !baseMission(assetWorld_->missionName())) return;
    const AssetFS& fs = assetWorld_->fs();
    std::vector<uint8_t> b;
    AnmAnimation tmpl;
    if (!fs.read("Mecc_base_template_1.anm", b) || !parseAnm(b.data(), b.size(), tmpl)) {
        LOGW("Base: Mecc_base_template_1.anm unavailable");
        return;
    }
    std::vector<Mat4> nodes;
    tmpl.evaluate(0.0f, nodes);
    auto slot = [&](const std::string& name, Mat4& out) {
        for (size_t i = 0; i < tmpl.nodes.size() && i < nodes.size(); ++i)
            if (toLower(tmpl.nodes[i].name) == toLower(name)) { out = nodes[i]; return true; }
        return false;
    };
    // Centre: Borjoyzee's start (the base's leader stands at the site), else the player start.
    const MissionData& md = assetWorld_->mission();
    const ObjectRegistry& reg = assetWorld_->registry();
    bool found = false;
    for (const auto& o : md.objects)
        if (toLower(reg.modelName(o.type)).find("borjoyzee") != std::string::npos) {
            base_.centre = giantsToRuntime(o.x, o.y, o.z);
            found = true;
            break;
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
    if (slot("Firepit", m)) {
        base_.firepit = std::make_unique<SkinnedModel>();
        if (!base_.firepit->load(fs, *mc, renderer_, "Sm_firepit", "SM_firepit.anm")) base_.firepit.reset();
        base_.firepitXf = baseXf * gameMatrixToRuntime(m);
    }
    if (slot("meat_dropoff", m)) {
        base_.dropXf = baseXf * gameMatrixToRuntime(m);
        base_.dropModel = mc->get("sm_dropoff.gbs", "sm_dropoff.anm", true);
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

void GameApp::orderBuilding(int d) {
    const int cost = kDefs[d].meat;
    if (base_.food < cost) {
        timmyLine_ = std::string(kDefs[d].label) + ": NEEDS " + std::to_string(cost) + " VIMP MEAT IN THE STORE (" +
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
    LOGI("Base: ordered %s (%d meat, store %d)", kDefs[d].label, cost, base_.food);
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
        timmyLine_ = "VIMP MEAT STORED: " + std::to_string(base_.food);
        timmyLineT_ = 3.0f;
        vimpMeat_ = 0;
        sfx2D("pu_meat1", 1.0f, 0.8f);
    }
    // Construction: one building at a time per crew; the whole workforce on the oldest order.
    static const bool trace = std::getenv("GVR_BASE_TRACE") != nullptr;
    if (trace && int(base_.traceT + dt) != int(base_.traceT))
        for (const auto& b : base_.bldgs)
            if (b.state == 1) LOGI("base-trace t=%.0f %s progress %.2f", base_.traceT, kDefs[b.def].label, b.progress);
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
            // Solid once standing: the building's own collision mesh (<model>_coll), else its model.
            ModelCache* mc = assetWorld_->models();
            const RenderModel* coll = mc->get(std::string(kDefs[b.def].model) + "_coll.gbs", "", false);
            if (!coll || coll->collisionTris.empty()) coll = mc->get(std::string(kDefs[b.def].model) + ".gbs", "", false);
            if (coll && !coll->collisionTris.empty()) {
                std::vector<Vec3> tris;
                tris.reserve(coll->collisionTris.size());
                for (const Vec3& v : coll->collisionTris) {
                    const Vec4 q = b.xf * Vec4{v.x, v.y, v.z, 1.0f};
                    tris.push_back({q.x, q.y, q.z});
                }
                b.collision = assetWorld_->collisionMutable().addObject(tris);
                Vec3 mn = tris[0], mx = tris[0];
                for (const Vec3& t : tris) { mn = Vec3{std::min(mn.x, t.x), std::min(mn.y, t.y), std::min(mn.z, t.z)}; mx = Vec3{std::max(mx.x, t.x), std::max(mx.y, t.y), std::max(mx.z, t.z)}; }
                LOGI("Base: %s solid (%zu triangles, box (%.1f %.1f %.1f)-(%.1f %.1f %.1f), site (%.1f %.1f %.1f))", kDefs[b.def].label,
                     tris.size() / 3, mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, b.xf.m[12], b.xf.m[13], b.xf.m[14]);
            }
            bool allOfKind = true;
            for (const auto& o : base_.bldgs) allOfKind = allOfKind && (o.def != b.def || o.state == 3);
            if (allOfKind) {
                // What a finished building gives (ours, after the game's descriptions): the Giftshop
                // "is full of weapons and other nifty stuff" - the shop counter opens in it; the
                // Workshop is "where your Smarties go for tools" - they build faster from then on.
                if (std::string(kDefs[b.def].key) == "giftshop1") {
                    // The counter stands at its front (toward the base's middle): the building is solid.
                    const Vec3 site{b.xf.m[12], b.xf.m[13], b.xf.m[14]};
                    Vec3 dir = base_.centre - site;
                    dir.y = 0;
                    Vec3 at = site + normalize(dir) * std::min(8.0f, length(dir) * 0.5f);
                    float gy;
                    if (assetWorld_->terrain().heightAt(at.x, at.z, gy)) at.y = gy;
                    openShopAt(at);
                }
                if (std::string(kDefs[b.def].key) == "mcworkshop") base_.speed = 1.5f;
                timmyLine_ = std::string(kDefs[b.def].label) + " - COMPLETED!";
                timmyLineT_ = 3.5f;
                sfx2D("pu_hlth", 0.9f, 0.8f);
                LOGI("Base: %s completed", kDefs[b.def].label);
            }
        }
    }
    if (!base_.complete) {
        bool all = true;
        for (const auto& b : base_.bldgs) all = all && (!kDefs[b.def].core || b.state == 3);
        if (all) {
            base_.complete = true;
            givePopupBomb(1, "BASE COMPLETE! YOUR SMARTIES MADE YOU A POP-UP BOMB.");
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
            timmyLine_ = "OUT OF FOOD!!! BRING VIMP MEAT TO THE DROP-OFF.";
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
        if (!touch || baseKindState(it.def) != 0) continue;
        base_.touchCool = 0.8f;
        orderBuilding(it.def);
        break;
    }
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
        std::snprintf(s, sizeof s, "%s  BUILDING... %d%%", kDefs[b.def].label, int(b.progress * 100.0f));
        label(Vec3{b.xf.m[12], b.xf.m[13] + 6.0f, b.xf.m[14]}, s, {1.0f, 0.85f, 0.3f, 1.0f}, 0.35f);
    }
    if (length(eye - base_.dropAt) < 40.0f) {
        char s[64];
        std::snprintf(s, sizeof s, "MEAT DROP-OFF  STORE %d", base_.food);
        label(base_.dropAt + Vec3{0, 2.6f, 0}, s, base_.food > 0 ? Vec4{0.6f, 1.0f, 0.6f, 1.0f} : Vec4{1.0f, 0.4f, 0.3f, 1.0f}, 0.18f);
    }
    if (length(eye - base_.padAt) < 40.0f && !base_.onPad) {
        label(base_.padAt + Vec3{0, 3.0f, 0}, base_.complete ? "BUILD PAD - BASE COMPLETE" : "BUILD PAD - STAND HERE TO BUILD",
              {0.6f, 0.9f, 1.0f, 1.0f}, 0.18f);
    }
    // The menu: a card per building you can order, at arm's length round the pad.
    for (const auto& it : base_.items) {
        const BldgDef& d = kDefs[it.def];
        const int st = baseKindState(it.def);
        const bool afford = st == 0 && base_.food >= d.meat;
        char s[64];
        if (st == 0) std::snprintf(s, sizeof s, "%s: %d MEAT", d.label, d.meat);
        else std::snprintf(s, sizeof s, "%s: %s", d.label, st == 3 ? "COMPLETED!" : "BUILDING...");
        const float bob = 0.03f * std::sin(float(time_) * 2.0f + it.pos.x);
        label(it.pos + Vec3{0, bob, 0}, s, afford ? Vec4{0.6f, 1.0f, 0.6f, 1.0f} : Vec4{0.75f, 0.75f, 0.75f, 1.0f}, 0.045f, 0.3f);
    }
    if (base_.onPad) {
        char s[64];
        std::snprintf(s, sizeof s, "VIMP MEAT STORE %d   SMARTIES %d   TOUCH A SIGN TO BUILD", base_.food, kWorkers);
        label(base_.menuAt + Vec3{-std::sin(base_.menuYaw) * 1.2f, 1.8f, -std::cos(base_.menuYaw) * 1.2f}, s, {0.6f, 0.9f, 1.0f, 1.0f},
              0.05f);
    }
}

}  // namespace gvr
