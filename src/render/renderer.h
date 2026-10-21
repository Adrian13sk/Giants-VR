// OpenGL ES 3.0 forward renderer shared by the Quest runtime and the headless desktop harness.
// It renders into whatever framebuffer is bound by the caller (an OpenXR swapchain image on
// Quest, an offscreen FBO in the harness).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/math.h"
#include "render/gl.h"

namespace gvr {

struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
    uint8_t color[4];
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;  // triangle list

    void append(const MeshData& other, const Mat4& transform);
};

using MeshId = uint32_t;     // 0 = invalid
using TextureId = uint32_t;  // 0 = no texture (white)

enum class BlendMode : uint8_t { Opaque, AlphaTest, AlphaBlend, Additive };

struct DrawParams {
    Vec4 tint{1, 1, 1, 1};
    TextureId texture = 0;
    BlendMode blend = BlendMode::Opaque;
    bool unlit = false;
    bool doubleSided = false;
    bool noFog = false;
    bool depthWrite = true;
};

struct ViewParams {
    Mat4 view;        // world -> eye
    Mat4 projection;  // eye -> clip
    Vec3 eyePosition; // world space
};

struct EnvironmentParams {
    Vec3 sunDirection{-0.4f, -1.0f, -0.3f};  // direction light travels (world)
    Vec3 sunColor{1.0f, 0.96f, 0.88f};
    Vec3 ambientColor{0.50f, 0.52f, 0.58f};  // raised after device test 1 ("a bit dark")
    Vec3 fogColor{0.55f, 0.70f, 0.90f};
    float fogNear = 60.0f;      // linear fog start (metres), like the original D3D fog
    float fogFar = 600.0f;      // linear fog end (metres)
    Vec3 clearColor{0.55f, 0.70f, 0.90f};
};

class Renderer {
public:
    bool init();
    void shutdown();

    MeshId uploadMesh(const MeshData& data);
    void destroyMesh(MeshId id);
    // Replaces all vertices of a mesh (same count as at upload); used for CPU skinning.
    bool updateMeshVertices(MeshId id, const std::vector<Vertex>& vertices);
    // RGBA8 pixels, rows top-to-bottom.
    TextureId uploadTexture(int width, int height, const uint8_t* rgba, bool mipmaps, bool repeat);
    void destroyTexture(TextureId id);

    EnvironmentParams& environment() { return env_; }

    // Begin rendering a view into the currently bound framebuffer.
    void beginView(const ViewParams& view, int viewportWidth, int viewportHeight, bool clear = true);
    void draw(MeshId mesh, const Mat4& model, const DrawParams& params = {});
    void endView();
    const ViewParams& currentView() const { return currentView_; }
    // Bounding-sphere test against the current view frustum (world space).
    bool sphereVisible(const Vec3& centre, float radius) const;
    uint32_t culledThisFrame() const { return culled_; }
    void countCulled() { ++culled_; }

    uint32_t drawCallsThisFrame() const { return drawCalls_; }
    void resetStats() { drawCalls_ = 0; culled_ = 0; }

private:
    struct GpuMesh {
        GLuint vao = 0, vbo = 0, ibo = 0;
        GLsizei indexCount = 0;
        size_t vertexCount = 0;
        bool live = false;
    };
    struct GpuTexture {
        GLuint tex = 0;
        bool live = false;
    };

    GLuint program_ = 0;
    GLint uMVP_ = -1, uModel_ = -1, uTint_ = -1, uSunDir_ = -1, uSunColor_ = -1, uAmbient_ = -1;
    GLint uFogColor_ = -1, uFogRange_ = -1, uEyePos_ = -1, uUnlit_ = -1, uAlphaRef_ = -1, uTex_ = -1;
    GLuint whiteTexture_ = 0;
    std::vector<GpuMesh> meshes_;
    std::vector<GpuTexture> textures_;
    EnvironmentParams env_;
    ViewParams currentView_{};
    Mat4 viewProj_ = Mat4::identity();
    uint32_t drawCalls_ = 0;
    uint32_t culled_ = 0;
    Vec4 planes_[6];
};

// Procedural geometry helpers (used for the engineering test scene and debug visualisation;
// never presented as original game content).
namespace prim {
MeshData box(const Vec3& halfExtents, const uint8_t rgba[4]);
MeshData plane(float halfSize, int subdivisions, float uvRepeat, const uint8_t rgba[4]);
MeshData sphere(float radius, int slices, int stacks, const uint8_t rgba[4]);
MeshData cylinder(float radius, float height, int slices, const uint8_t rgba[4]);
}  // namespace prim

}  // namespace gvr
