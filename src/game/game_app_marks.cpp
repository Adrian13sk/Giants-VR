// GameApp, rework: marks left on the ground. Explosions char it (the original's blastmark_1, a
// blackened burst, darker with the blast's size), Kabuto's feet press his footprint into it
// (Kabuto_footprint.tga from his level archive - unused by us before). Each mark is a small grid
// that follows the terrain (a flat quad on uneven ground sank into it or floated), fades in fast,
// lies, then fades away. Our own design on the original art.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "core/log.h"
#include "game/game_app.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr size_t kMaxMarks = 48;
}

void GameApp::addGroundMark(int kind, const Vec3& at, float size, float angle, float life, float width) {
    static const bool trace = std::getenv("GVR_MARK_TRACE") != nullptr;
    if (trace) LOGI("mark %d at (%.1f %.1f %.1f) size %.1f%s", kind, at.x, at.y, at.z, size, enemies_.groundHeight ? "" : " - no ground");
    if (!enemies_.groundHeight) return;
    const float g = enemies_.groundHeight(at.x, at.z);
    if (g < player_.seaLevel - 0.1f) return;  // (under water: none)
    if (at.y > g + 0.6f * size + 1.5f) return;  // (a blast in the air)
    if (marks_.size() >= kMaxMarks) marks_.erase(marks_.begin());
    GroundMark m;
    m.kind = kind;
    m.pos = Vec3{at.x, g, at.z};
    m.size = size;
    m.angle = angle;
    m.life = life;
    m.width = width;
    marks_.push_back(m);
}

void GameApp::updateGroundMarks(float dt) {
    if (marksWorld_ != assetWorld_.get()) { marksWorld_ = assetWorld_.get(); marks_.clear(); }
    // Kabuto's footprints: one per step of his run cycle (two per cycle, as the footstep sounds), the
    // left foot's mirrored; not on structures or in water.
    const PlayerAnim pa = player_.anim();
    if (hero_ == Hero::Kabuto && playMode_ && player_.onGround && playerModel_ &&
        (pa == PlayerAnim::Run || pa == PlayerAnim::Strafe || pa == PlayerAnim::Turbo)) {
        const float dur = std::max(0.1f, playerModel_->animDuration(clip_[int(pa)]));
        const float half = std::floor(player_.animTime() / dur * 2.0f);
        if (half != footPhase_) {
            footPhase_ = half;
            const bool wade = player_.position.y < player_.seaLevel - 0.05f;
            // (2026-10-28) Each footfall in the hands, heavier the bigger he is; the stepping side's hand most.
            {
                const int sideHand = (int(half) & 1) ? kRight : kLeft;
                const float a = (0.2f + 0.35f * kbGrow_) * (wade ? 0.6f : 1.0f);
                haptics_.push_back({sideHand, a, 0.07f});
                haptics_.push_back({1 - sideHand, 0.6f * a, 0.05f});
                static int n = 0;
                if (++n == 4) LOGI("Kabuto's footfalls felt in the hands (%.2f)", a);
            }
            if (!wade && !player_.onObject()) {
                const float side = (int(half) & 1) ? 1.0f : -1.0f;
                const Quat r = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
                const Vec3 foot = player_.position + r.rotate(Vec3{0.12f * side * player_.height, 0, -0.05f * player_.height});
                addGroundMark(1, foot, 0.2f * player_.height, -player_.yaw, 45.0f, 0.5f * side);
            }
        }
    } else {
        footPhase_ = -1;
    }
    for (auto& m : marks_) m.age += dt;
    marks_.erase(std::remove_if(marks_.begin(), marks_.end(), [](const GroundMark& m) { return m.age >= m.life; }), marks_.end());
}

void GameApp::buildGroundMarks(const Vec3& eye, ModelCache* mc) {
    if (!marksReady_ && mc) {
        marksReady_ = true;
        markTex_[0] = mc->texture("blastmark_1");
        markTex_[1] = mc->texture("Kabuto_footprint");
        markBatch_[0].init(renderer_, 40 * 9);
        markBatch_[1].init(renderer_, 40 * 9);
        markGlow_.init(renderer_, 12 * 9);
        arrowBatch_.init(renderer_, 30 * 4);
        {
            // The embers' light: a soft white disc with a speckled middle (made here; tinted by the
            // vertex colour - the game's glow textures are coloured).
            const int N = 64;
            std::vector<uint8_t> px(size_t(N * N * 4));
            unsigned r = 12345u;
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x) {
                    r = r * 1664525u + 1013904223u;
                    const float dx = (float(x) + 0.5f) / N * 2.0f - 1.0f, dy = (float(y) + 0.5f) / N * 2.0f - 1.0f;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    const float speck = 0.55f + 0.45f * float((r >> 8) & 0xFF) / 255.0f;
                    const float v = std::max(0.0f, 1.0f - d / 0.7f);
                    const uint8_t c = uint8_t(255.0f * std::min(1.0f, v * v * speck * 1.3f));
                    uint8_t* p = &px[size_t(y * N + x) * 4];
                    p[0] = p[1] = p[2] = c;
                    p[3] = 255;
                }
            markGlowTex_ = renderer_.uploadTexture(N, N, px.data(), true, false);
        }
        LOGI("Ground marks: scorch %s, footprint %s", markTex_[0] ? "ok" : "missing", markTex_[1] ? "ok" : "missing");
    }
    if (!marksReady_) return;
    for (auto& b : markBatch_) b.clear();
    markGlow_.clear();
    if (!enemies_.groundHeight) return;
    for (const auto& m : marks_) {
        const float far = m.kind == 1 ? 260.0f : 120.0f;  // (a giant's footprints are seen from his height)
        if (length(m.pos - eye) > far + m.size) continue;
        QuadBatch& b = markBatch_[m.kind == 1 ? 1 : 0];
        if (b.count() + 9 > b.capacity()) continue;
        // Fade in over 0.15 s, out over the last third of its life.
        const float fin = std::min(1.0f, m.age / 0.15f), fout = std::min(1.0f, (m.life - m.age) / (0.33f * m.life));
        const float a = fin * fout * (m.kind == 1 ? 0.8f : 0.92f);
        if (a <= 0.01f) continue;
        const uint8_t shade = m.kind == 1 ? 120 : 255;
        const uint8_t col[4] = {shade, shade, shade, uint8_t(a * 255.0f)};
        const float ca = std::cos(m.angle), sa = std::sin(m.angle), h = 0.5f * m.size;
        const Vec3 ux = Vec3{ca * h, 0, sa * h} * m.width, vz{-sa * h, 0, ca * h};
        // A 3x3 grid of cells, each corner on the ground (+4 cm, more for a big one).
        const float lift = 0.04f + 0.01f * m.size;
        auto corner = [&](int i, int j) {
            const float s = -1.0f + 2.0f * float(i) / 3.0f, t = -1.0f + 2.0f * float(j) / 3.0f;
            Vec3 p = m.pos + ux * s + vz * t;
            p.y = enemies_.groundHeight(p.x, p.z) + lift;
            return p;
        };
        // A fresh scorch glows: embers in the burnt ground for the first few seconds (additive).
        const float ember = m.kind == 0 ? std::max(0.0f, 1.0f - m.age / 4.0f) : 0.0f;
        const float flick = 0.8f + 0.2f * std::sin(float(time_) * 9.0f + m.angle * 7.0f);
        const uint8_t eg[4] = {uint8_t(255.0f * ember * flick), uint8_t(110.0f * ember * ember * flick), uint8_t(25.0f * ember * ember), 255};
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                const Vec3 q[4] = {corner(i, j), corner(i + 1, j), corner(i + 1, j + 1), corner(i, j + 1)};
                const float uv[4][2] = {{float(i) / 3.0f, float(j) / 3.0f}, {float(i + 1) / 3.0f, float(j) / 3.0f},
                                        {float(i + 1) / 3.0f, float(j + 1) / 3.0f}, {float(i) / 3.0f, float(j + 1) / 3.0f}};
                b.add(q, uv, col);
                if (ember > 0.02f && !markGlow_.full()) markGlow_.add(q, uv, eg);
            }
    }
    // Arrows that stuck (game_app_casings.cpp): a dark shaft back from where each went in, teal
    // fletching near its end, as two crossed quads (the original's arrow is only a glowing streak).
    arrowBatch_.clear();
    for (const auto& a : stuckArrows_) {
        if (length(a.pos - eye) > 60.0f || arrowBatch_.count() + 4 > arrowBatch_.capacity()) continue;
        const float fade = std::max(0.0f, std::min(1.0f, (20.0f - a.age) / 4.0f));
        Vec3 s1 = cross(a.dir, Vec3{0, 1, 0});
        s1 = length(s1) > 1e-3f ? normalize(s1) : Vec3{1, 0, 0};
        const Vec3 s2 = normalize(cross(a.dir, s1));
        const Vec3 head = a.pos, tail = a.pos - a.dir * 0.75f, fl = a.pos - a.dir * 0.55f;
        const uint8_t shaft[4] = {70, 48, 30, uint8_t(255.0f * fade)}, feather[4] = {40, 170, 160, uint8_t(255.0f * fade)};
        for (const Vec3& s : {s1, s2}) {
            const Vec3 q[4] = {head - s * 0.016f, head + s * 0.016f, tail + s * 0.016f, tail - s * 0.016f};
            arrowBatch_.add(q, shaft);
            const Vec3 f[4] = {fl - s * 0.06f, fl + s * 0.06f, tail + s * 0.06f, tail - s * 0.06f};
            arrowBatch_.add(f, feather);
        }
    }
    arrowBatch_.upload(renderer_);
    for (auto& b : markBatch_) b.upload(renderer_);
    markGlow_.upload(renderer_);
    static const bool trace = std::getenv("GVR_MARK_TRACE") != nullptr;
    static int tk = 0;
    if (trace && (++tk % 60 == 1)) LOGI("marks: %zu (%zu scorch cells, %zu footprint cells), first age %.2f", marks_.size(), markBatch_[0].count(), markBatch_[1].count(), marks_.empty() ? -1.0f : marks_[0].age);
}

void GameApp::renderGroundMarks() {
    if (!marksReady_ || renderer_.inReflectionPass()) return;
    DrawParams dp;
    dp.blend = BlendMode::AlphaBlend;
    dp.unlit = true;  // (the marks are dark; as the blood decals)
    dp.depthWrite = false;
    dp.doubleSided = true;
    for (int k = 0; k < 2; ++k) {
        if (!markTex_[k] || markBatch_[k].count() == 0) continue;
        dp.texture = markTex_[k];
        static const bool dbg = std::getenv("GVR_MARK_DEBUG") != nullptr;
        if (dbg) dp.blend = BlendMode::Opaque;
        markBatch_[k].draw(renderer_, dp);
    }
    if (arrowBatch_.count()) {
        DrawParams ap;
        ap.blend = BlendMode::AlphaBlend;
        ap.unlit = true;
        ap.doubleSided = true;
        arrowBatch_.draw(renderer_, ap);
    }
    if (markGlowTex_ && markGlow_.count()) {
        // (additive: the scorch texture is near black, so the embers' glow is a soft round light
        // over the middle of the burn, in the vertex colour)
        DrawParams gp = dp;
        gp.blend = BlendMode::Additive;
        gp.texture = markGlowTex_;
        gp.noFog = true;
        markGlow_.draw(renderer_, gp);
    }
}

}  // namespace gvr
