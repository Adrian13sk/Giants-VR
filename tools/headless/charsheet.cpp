// Character inspection tool: renders one original character (GBS+SKN+ANM) in isolation at
// several animation times from the front and the side, as a contact sheet PNG.
// usage: charsheet <BinDir> <modelBase> <anim.anm> <out.png> [columns]
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "assets/image.h"
#include "egl_util.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

using namespace gvr;

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: charsheet <Bin> <base> <anim> <out.png> [cols]\n"); return 2; }
    const int cols = argc > 5 ? std::atoi(argv[5]) : 6;
    if (!initEgl()) return 2;
    Renderer r;
    if (!r.init()) return 3;
    r.environment().fogNear = 1e5f;
    r.environment().fogFar = 2e5f;
    r.environment().clearColor = {0.25f, 0.27f, 0.3f};
    AssetFS fs;
    fs.mountAllArchives(argv[1]);
    ModelCache cache(fs, r);
    SkinnedModel m;
    if (!m.load(fs, cache, r, argv[2], argv[3])) return 4;
    const int W = 256, H = 320;
    std::vector<uint8_t> sheet(size_t(W) * cols * H * 2 * 4);
    GLuint tex, depth, fbo;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex); glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, W, H);
    glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    std::vector<uint8_t> px(size_t(W) * H * 4);
    const float dur = std::max(0.01f, m.animDuration());
    for (int c = 0; c < cols; ++c) {
        const float t = dur * c / cols;
        m.pose(r, t);
        Vec3 mn, mx;
        m.posedBounds(mn, mx);
        const Vec3 centre = (mn + mx) * 0.5f;
        const float size = std::max({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z});
        for (int view = 0; view < 2; ++view) {
            // view 0: from the model's -Z side (front in the runtime frame); view 1: from +X.
            const float yaw = view == 0 ? 0.0f : kPi / 2;
            Quat q = Quat::fromAxisAngle({0, 1, 0}, yaw);
            Pose eye{q, centre + q.rotate(Vec3{0, 0, size * 1.6f})};
            ViewParams v;
            v.view = Mat4::fromPose(eye.inverse());
            v.projection = Mat4::projectionFov(-0.45f, 0.45f, 0.56f, -0.56f, 0.05f, 0.0f);
            v.eyePosition = eye.position;
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            r.beginView(v, W, H, true);
            m.render(r, Mat4::identity());
            r.endView();
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < H; ++y)
                std::memcpy(&sheet[((size_t(view) * H + (H - 1 - y)) * W * cols + size_t(c) * W) * 4],
                            &px[size_t(y) * W * 4], size_t(W) * 4);
        }
    }
    writePng(argv[4], W * cols, H * 2, sheet.data(), false);
    std::printf("wrote %s (anim %.2fs)\n", argv[4], dur);
    return 0;
}
