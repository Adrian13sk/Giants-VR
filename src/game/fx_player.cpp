#include "game/fx_player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "assets/gzp.h"
#include "assets/object_defs.h"
#include "core/log.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
float frnd() { return float(std::rand()) / float(RAND_MAX); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
Vec3 g2r(float x, float y, float z) { return Vec3{-x, z, y} * gGameUnitsToMeters; }
Vec3 randomDir() {
    for (;;) {
        const Vec3 v{frnd() * 2 - 1, frnd() * 2 - 1, frnd() * 2 - 1};
        const float l = length(v);
        if (l > 0.05f && l <= 1.0f) return v * (1.0f / l);
    }
}
}  // namespace

float FxPlayer::Particle::alpha() const {
    float a = 1.0f;
    if (fadeIn > 1e-4f && age < fadeIn) a = age / fadeIn;
    if (fadeOut > 1e-4f && age > life - fadeOut) a = std::min(a, std::max(0.0f, (life - age) / fadeOut));
    return std::max(0.0f, std::min(1.0f, a));
}

float FxPlayer::Particle::size() const { return lerp(size0, size1, life > 0 ? std::min(1.0f, age / life) : 1.0f); }

void FxPlayer::init(const FxLibrary* lib, const ObjectRegistry* reg, ModelCache* models, const AssetFS* fs) {
    lib_ = lib;
    reg_ = reg;
    models_ = models;
    fs_ = fs;
    types_.clear();
    clear();
}

const FxPlayer::TypeInfo& FxPlayer::typeInfo(uint32_t type) {
    auto it = types_.find(type);
    if (it != types_.end()) return it->second;
    TypeInfo ti;
    const std::string name = reg_ ? reg_->modelName(type) : std::string();
    if (!name.empty() && models_) {
        ti.model = models_->get(name + ".gbs", "", /*dynamicLit=*/true);
        ti.anim = reg_->modelAnim(type);
        if (ti.model) {
            for (const auto& p : ti.model->parts) ti.additive = ti.additive || p.glass;  // efx flag bit 0: glowing
            ti.billboard = ti.model->boundsMax.z - ti.model->boundsMin.z < 1e-3f;      // a flat quad: faces the eye
        }
    }
    return types_[type] = ti;
}

FxPlayer::Shape* FxPlayer::shape(const std::string& anim) {
    auto it = shapes_.find(anim);
    if (it != shapes_.end()) return it->second->ok ? it->second.get() : nullptr;
    auto s = std::make_unique<Shape>();
    std::vector<uint8_t> b;
    if (fs_ && !anim.empty() && fs_->read(anim, b) && parseAnm(b.data(), b.size(), s->anim)) {
        s->ok = true;
        for (int n = 1; n <= 40; ++n) {
            char a[24], c[24];
            std::snprintf(a, sizeof a, "ref_debri%02d", n);
            std::snprintf(c, sizeof c, "ref_debri_%02d", n);
            auto f = s->anim.byName.find(a);
            if (f == s->anim.byName.end()) f = s->anim.byName.find(c);
            s->refNode.push_back(f == s->anim.byName.end() ? -1 : f->second);
        }
    }
    Shape* r = s->ok ? s.get() : nullptr;
    shapes_[anim] = std::move(s);
    return r;
}

const FxPlayer::Particle* FxPlayer::byId(uint32_t id) const {
    for (const auto& p : particles)
        if (p.id == id) return &p;
    return nullptr;
}

bool FxPlayer::play(const std::string& name, const Vec3& at, const Quat& rot, float scale) {
    if (!lib_) return false;
    const FxDef* def = lib_->find(name);
    if (!def) return false;
    run(def, 0, at, rot, scale, 0, -1);
    return true;
}

void FxPlayer::run(const FxDef* def, size_t from, const Vec3& at, const Quat& rot, float scale, uint32_t parent, int node) {
    Particle* cur = nullptr;
    bool inBlock = false;
    float sizeLo = 1, sizeHi = 1;
    for (size_t i = from; i < def->ops.size(); ++i) {
        const FxOp& op = def->ops[i];
        auto f = [&](size_t k) { return k < op.f.size() ? op.f[k] : 0.0f; };
        switch (op.op) {
            case 0x0b:  // wait, then go on
                if (!inBlock && f(0) > 0) {
                    pending_.push_back({def, i + 1, f(0), at, rot, scale, parent, node});
                    return;
                }
                break;
            case 0x2b: case 0x2c:
                if (soundName && !op.u.empty() && op.u[0]) {
                    const std::string s = soundName(op.u[0]);
                    if (!s.empty() && op.op == 0x2b) sounds.push_back({s, at});
                }
                break;
            case 0x03: case 0x38: {
                if (particles.size() >= kMaxParticles || op.u.empty()) { cur = nullptr; break; }
                const TypeInfo& ti = typeInfo(op.u[0]);
                Particle p;
                p.id = nextId_++;
                p.model = ti.model;
                p.additive = ti.additive;
                p.billboard = ti.billboard;
                p.pos = at;
                p.rot = rot;
                p.life = 1.0f;
                p.size0 = p.size1 = scale;
                p.parent = parent;
                p.node = node;
                if (!ti.anim.empty()) p.shape = shape(ti.anim);
                particles.push_back(p);
                cur = &particles.back();
                sizeLo = sizeHi = 1;
                break;
            }
            case 0x06: inBlock = true; break;
            case 0x07: inBlock = false; cur = nullptr; break;
            default: break;
        }
        if (!cur || !inBlock) continue;
        Particle& p = *cur;
        switch (op.op) {
            case 0x04: p.life = std::max(0.02f, f(0)); p.fadeIn = f(1); p.fadeOut = f(2); break;
            case 0x08: p.size0 = f(0) * gGameUnitsToMeters * scale; p.size1 = f(1) * gGameUnitsToMeters * scale; break;
            case 0x09:
                p.size0 = lerp(f(0), f(1), frnd()) * gGameUnitsToMeters * scale;
                p.size1 = lerp(f(2), f(3), frnd()) * gGameUnitsToMeters * scale;
                break;
            case 0x35:
                p.size0 = lerp(f(0), f(1), frnd()) * gGameUnitsToMeters * scale;
                p.size1 = lerp(f(2), f(3), frnd()) * gGameUnitsToMeters * scale;
                break;
            case 0x1b: p.shapeScale = f(0) * scale; if (!p.billboard) p.size0 = p.size1 = f(0) * scale; break;
            case 0x1c: { sizeLo = f(0); sizeHi = f(1); const float k = lerp(sizeLo, sizeHi, frnd()); p.size0 *= k; p.size1 *= k; break; }
            case 0x12: p.vel = p.vel + randomDir() * (lerp(f(0), f(1), frnd()) * gGameUnitsToMeters * scale); break;
            case 0x0e: p.gravity = f(0); break;
            case 0x0f: p.gravity = lerp(f(0), f(1), frnd()); break;
            case 0x19: p.bounce = f(0); break;
            case 0x33: {
                const float d2r = 3.14159265f / 180.0f;
                p.rot = p.rot * Quat::fromAxisAngle({1, 0, 0}, lerp(f(0), f(1), frnd()) * d2r) *
                        Quat::fromAxisAngle({0, 1, 0}, lerp(f(2), f(3), frnd()) * d2r) * Quat::fromAxisAngle({0, 0, 1}, lerp(f(4), f(5), frnd()) * d2r);
                break;
            }
            case 0x13: p.spinAxis = randomDir(); p.spin = lerp(f(0), f(1), frnd()) * 6.2832f * 30.0f; break;
            case 0x21: p.ref = true; break;
            case 0x0a: case 0x10: case 0x70:
                // A child effect riding this shape's point n.
                if (lib_ && !op.s.empty() && !op.u.empty())
                    if (const FxDef* c = lib_->find(op.s)) {
                        const uint32_t pid = p.id;
                        const int n = int(op.u[0]);
                        // Started after this emitter's block (its parameters complete): queued with no wait.
                        pending_.push_back({c, 0, 0.0f, p.pos, p.rot, scale, pid, n});
                    }
                break;
            case 0x5b:
                if (lib_ && !op.s.empty())
                    if (const FxDef* c = lib_->find(op.s)) trails_.push_back({p.id, c, std::max(0.02f, f(0)), 0.0f});
                break;
            default: break;
        }
    }
}

void FxPlayer::update(float dt) {
    // Effects whose wait is over (and children queued this frame).
    std::vector<Pending> due;
    for (auto& w : pending_) {
        w.wait -= dt;
        if (w.wait <= 0) due.push_back(w);
    }
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Pending& w) { return w.wait <= 0; }), pending_.end());
    for (const auto& w : due) run(w.def, w.op, w.at, w.rot, w.scale, w.parent, w.node);
    // Reference shapes: their points this frame (game units -> runtime, in the effect's frame).
    std::vector<Mat4> world;
    for (auto& p : particles) {
        p.points.clear();
        if (!p.shape) continue;
        const float t = std::fmod(p.age, std::max(0.05f, p.shape->anim.durationSeconds()));
        p.shape->anim.evaluate(t, world);
        for (int n : p.shape->refNode) {
            if (n < 0 || size_t(n) >= world.size()) { p.points.push_back(p.pos); continue; }
            const Mat4& m = world[size_t(n)];
            p.points.push_back(p.pos + p.rot.rotate(g2r(m.m[12], m.m[13], m.m[14]) * p.shapeScale));
        }
    }
    for (auto& p : particles) {
        p.age += dt;
        if (p.parent) {
            const Particle* par = byId(p.parent);
            if (par && p.node >= 1 && size_t(p.node) <= par->points.size()) {
                const Vec3 np = par->points[size_t(p.node - 1)] + p.offset;
                if (dt > 1e-4f && p.age > dt) p.vel = (np - p.pos) * (1.0f / dt);  // kept when the shape ends
                p.pos = np;
                continue;
            }
            p.parent = 0;  // the shape is gone: carry on free, with the speed it had
        }
        p.vel.y -= 9.8f * p.gravity * dt;
        p.pos += p.vel * dt;
        if (p.spin != 0) p.rot = Quat::fromAxisAngle(p.spinAxis, p.spin * dt) * p.rot;
        float gy;
        if (p.bounce > 0 && groundAt && groundAt(p.pos.x, p.pos.z, gy) && p.pos.y < gy) {
            p.pos.y = gy;
            p.vel = Vec3{p.vel.x * 0.6f, -p.vel.y * p.bounce, p.vel.z * 0.6f};
            if (std::fabs(p.vel.y) < 0.3f) { p.vel = {0, 0, 0}; p.gravity = 0; p.spin = 0; }
        }
    }
    // Trails.
    for (auto& tr : trails_) {
        const Particle* p = byId(tr.particle);
        if (!p) { tr.every = -1; continue; }
        tr.t += dt;
        while (tr.t >= tr.every && particles.size() < kMaxParticles) {
            tr.t -= tr.every;
            run(tr.effect, 0, p->pos, Quat{}, 1.0f, 0, -1);
            p = byId(tr.particle);  // the vector may have grown
            if (!p) break;
        }
    }
    trails_.erase(std::remove_if(trails_.begin(), trails_.end(), [](const Trail& t) { return t.every < 0; }), trails_.end());
    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.age >= p.life; }), particles.end());
}

}  // namespace gvr
