// Static model inspection tool: renders original GBS models in isolation from four directions
// (front, side, above, below) as one contact-sheet row per model.
// usage: modelsheet <BinDir> <out.png> <model.gbs>[@<anim.anm>]...
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "assets/image.h"
#include "egl_util.h"
#include "scene/model_cache.h"

using namespace gvr;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: modelsheet <Bin> <out.png> <model.gbs>...\n"); return 2; }
    if (!initEgl()) return 2;
    Renderer r;
    if (!r.init()) return 3;
    r.environment().fogNear = 1e5f;
    r.environment().fogFar = 2e5f;
    r.environment().clearColor = {0.25f, 0.27f, 0.3f};
    AssetFS fs;
    fs.mountAllArchives(argv[1]);
    ModelCache cache(fs, r);
    const int W = 320, H = 320, cols = 4, rows = argc - 3;
    std::vector<uint8_t> sheet(size_t(W) * cols * H * rows * 4);
    GLuint tex, depth, fbo;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex); glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, W, H);
    glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    std::vector<uint8_t> px(size_t(W) * H * 4);
    for (int row = 0; row < rows; ++row) {
        // "<model.gbs>@<anim.anm>" bakes a skinned object in its default skin pose.
        std::string arg = argv[3 + row], anim;
        if (auto at = arg.find('@'); at != std::string::npos) { anim = arg.substr(at + 1); arg.resize(at); }
        const RenderModel* m = cache.get(arg, anim);
        if (!m) continue;
        const Vec3 centre = (m->boundsMin + m->boundsMax) * 0.5f;
        const float size = std::max({m->boundsMax.x - m->boundsMin.x, m->boundsMax.y - m->boundsMin.y,
                                     m->boundsMax.z - m->boundsMin.z});
        for (int c = 0; c < cols; ++c) {
            // 0 front (-Z side), 1 side (+X), 2 from above (pitched down), 3 from below.
            Quat q = Quat::fromAxisAngle({0, 1, 0}, c == 1 ? kPi / 2 : kPi / 5);
            if (c == 2) q = q * Quat::fromAxisAngle({1, 0, 0}, -0.9f);
            if (c == 3) q = q * Quat::fromAxisAngle({1, 0, 0}, 0.7f);
            Pose eye{q, centre + q.rotate(Vec3{0, 0, size * 1.3f})};
            ViewParams v;
            v.view = Mat4::fromPose(eye.inverse());
            v.projection = Mat4::projectionFov(-0.5f, 0.5f, 0.5f, -0.5f, 0.05f, 0.0f);
            v.eyePosition = eye.position;
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            r.beginView(v, W, H, true);
            for (const auto& part : m->parts) {
                DrawParams dp;
                dp.texture = part.texture;
                dp.blend = part.blend;
                dp.unlit = !part.lit;
                r.draw(part.mesh, Mat4::identity(), dp);
            }
            r.endView();
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < H; ++y)
                std::memcpy(&sheet[((size_t(row) * H + (H - 1 - y)) * W * cols + size_t(c) * W) * 4],
                            &px[size_t(y) * W * 4], size_t(W) * 4);
        }
    }
    writePng(argv[2], W * cols, H * rows, sheet.data(), false);
    std::printf("wrote %s\n", argv[2]);
    return 0;
}
