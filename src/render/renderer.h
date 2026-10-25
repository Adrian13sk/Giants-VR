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
    bool depthTest = true;  // false: drawn over everything (overlays such as subtitles)
    // Material: Standard (lit/unlit textured), Water (animated surface: waves, fresnel sky
    // reflection, sun glints), Sky (drifting clouds, sun disc and glow).
    enum class Material : uint8_t { Standard, Water, Sky, Terrain } material = Material::Standard;
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
    float seaLevel = -1e9f;                      // water surface height; below it surfaces tint toward waterColor
    Vec3 waterColor{0.020f, 0.095f, 0.110f};     // linear
    Vec3 skyHorizon{0.55f, 0.70f, 0.90f}, skyZenith{0.25f, 0.40f, 0.70f};  // linear, for water reflections
    float time = 0;                              // seconds (waves, cloud drift)
    // Colour grade: saturation, contrast, warm/cool split, exposure (1 = neutral). Livelier than
    // the raw textures (user feedback after device test 19: "washed out, boring").
    Vec4 grade{1.25f, 1.06f, 0.08f, 1.08f};
    // Time of day (game layer): the painted sky's tint, how dark it is (stars), and the light on
    // baked surfaces (lightmaps and vertex colours carry daylight; this scales them at dusk/night).
    Vec3 skyTint{1, 1, 1};
    float night = 0;
    Vec3 bakedLight{1, 1, 1};
    // One point light from the game (the jetpack's plasma): colour 0 = off.
    Vec3 pointLightPos{0, 0, 0}, pointLightColor{0, 0, 0};
    float pointLightRadius = 6.0f;
};

class Renderer {
public:
    bool init();
    void shutdown();

    MeshId uploadMesh(const MeshData& data);
    void destroyMesh(MeshId id);
    // Replaces all vertices of a mesh (same count as at upload); used for CPU skinning.
    bool updateMeshVertices(MeshId id, const std::vector<Vertex>& vertices);
    // Replaces vertices [first, first + count) only.
    bool updateMeshVertexRange(MeshId id, size_t first, const Vertex* vertices, size_t count);
    // RGBA8 pixels, rows top-to-bottom.
    TextureId uploadTexture(int width, int height, const uint8_t* rgba, bool mipmaps, bool repeat);
    void destroyTexture(TextureId id);

    EnvironmentParams& environment() { return env_; }

    // Begin rendering a view into the currently bound framebuffer.
    void beginView(const ViewParams& view, int viewportWidth, int viewportHeight, bool clear = true);
    // Both eyes in one pass (GL_OVR_multiview2): the bound framebuffer has two layers (a texture
    // array), each draw goes to both with its eye's view. currentView() is then the point between
    // the eyes (for the game's distance checks); culling keeps what either eye sees.
    bool multiviewSupported() const { return programMV_ != 0; }
    void beginStereoView(const ViewParams views[2], int viewportWidth, int viewportHeight, bool clear = true);
    bool stereo() const { return stereo_; }
    void draw(MeshId mesh, const Mat4& model, const DrawParams& params = {});
    void endView();
    // Sun shadows: one depth map around the player, rendered once per frame before the eyes.
    bool initShadows(int size);
    void beginShadowPass(const Mat4& lightViewProj);
    void drawShadow(MeshId mesh, const Mat4& model);
    void endShadowPass();
    void setShadowsActive(bool on) { shadowsActive_ = on; }
    // Terrain layers (Material::Terrain): the world's ground texture is the draw's own; these add
    // rock (slopes), sand (beach band and sea bed), a second grass/earth, a second rock and a
    // tileable noise texture (R: patches, G: fine grain) that breaks the blends up. detail=false:
    // the cheap path (ground + rock by slope + sand band, fewer samples) for the low quality levels.
    void setTerrainLayers(TextureId rock, TextureId sand, TextureId grass2, TextureId rock2, TextureId noise) {
        terrainTex_[0] = rock; terrainTex_[1] = sand; terrainTex_[2] = grass2; terrainTex_[3] = rock2; terrainTex_[4] = noise;
    }
    void setTerrainDetail(bool on) { terrainDetail_ = on; }
    bool shadowsReady() const { return shadowFbo_ != 0; }
    // Orthographic sun view around `centre` (half-size in metres), snapped to shadow texels.
    Mat4 sunShadowMatrix(const Vec3& centre, float halfSize) const;
    // Planar water reflection: the world mirrored in the sea surface, rendered once per frame
    // (from between the eyes, wide enough for both) into its own colour + depth target. The water
    // shader corrects the lookup per eye with the reflection's depth.
    bool initReflection(int width, int height);
    bool reflectionReady() const { return reflFbo_ != 0; }
    // `view`: the mirrored view (view matrix * mirror), eyePosition = mirrored eye.
    void beginReflectionPass(const ViewParams& view, float nearZ);
    void endReflectionPass();
    void setReflectionActive(bool on) { reflActive_ = on; }
    bool inReflectionPass() const { return inReflection_; }
    // A gun scope's picture: the world from the scope, rendered into a small square texture that is
    // then drawn on the scope's eyepiece like any other texture (scopeTexture()).
    bool initScope(int size);
    bool scopeReady() const { return scopeFbo_ != 0; }
    void beginScopePass(const ViewParams& view);
    void endScopePass();
    TextureId scopeTexture() const { return scopeTex_; }
    // Water depth below the sea surface over a rectangle of the world (R8: depth / 24 m).
    void setWaterDepthMap(int w, int h, const uint8_t* depth, float x0, float z0, float width, float depthZ);
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
    // The main program's uniform locations; one set per program (mono, multiview).
    struct Uniforms {
        GLint uMVP_ = -1, uModel_ = -1, uTint_ = -1, uSunDir_ = -1, uSunColor_ = -1, uAmbient_ = -1;
        GLint uFogColor_ = -1, uFogRange_ = -1, uEyePos_ = -1, uUnlit_ = -1, uAlphaRef_ = -1, uTex_ = -1;
        GLint uMaterial_ = -1, uTime_ = -1, uSeaLevel_ = -1, uWaterColor_ = -1, uSkyHorizon_ = -1, uSkyZenith_ = -1;
        GLint uShadowMap_ = -1, uShadowMat_ = -1, uShadowOn_ = -1, uShadowTexel_ = -1;
        GLint uClipY_ = -1, uGrade_ = -1, uReflTex_ = -1, uReflDepth_ = -1, uReflMat_ = -1, uReflEye_ = -1, uReflNear_ = -1,
              uReflOn_ = -1, uWaterDepth_ = -1, uWaterDepthRect_ = -1, uWaterDepthOn_ = -1;
        GLint uTerr_[5] = {-1, -1, -1, -1, -1}, uTerrDetail_ = -1, uSkyTint_ = -1, uNight_ = -1, uBakedLight_ = -1, uPointPos_ = -1, uPointColor_ = -1;
        GLint uViewProj_ = -1, uEyePosV_ = -1;  // multiview: both eyes' view-projection and position
    };
    GLuint linkMain(bool multiview, Uniforms& u);
    void beginViews(const ViewParams* views, int n, int w, int h, bool clear);
    Uniforms mono_, multi_;
    Uniforms* U_ = &mono_;
    GLuint programMV_ = 0;
    bool stereo_ = false;
    float anisotropy_ = 0;
    TextureId terrainTex_[5] = {};
    bool terrainDetail_ = true;
    GLuint reflFbo_ = 0, reflColor_ = 0, reflDepth_ = 0, waterDepthTex_ = 0;
    int reflW_ = 0, reflH_ = 0;
    GLuint scopeFbo_ = 0, scopeDepth_ = 0;
    TextureId scopeTex_ = 0;
    int scopeSize_ = 0;
    bool reflActive_ = false, inReflection_ = false;
    Mat4 reflMat_ = Mat4::identity();
    Vec3 reflEye_;
    float reflNear_ = 0.1f;
    float waterRect_[4] = {0, 0, 0, 0};
    GLuint shadowProgram_ = 0, shadowFbo_ = 0, shadowTex_ = 0, dummyShadowTex_ = 0;
    GLint uShadowMVP_ = -1;
    int shadowSize_ = 0;
    bool shadowsActive_ = false;
    Mat4 shadowMat_ = Mat4::identity();
    GLuint whiteTexture_ = 0;
    std::vector<GpuMesh> meshes_;
    std::vector<GpuTexture> textures_;
    EnvironmentParams env_;
    ViewParams currentView_{};
    Mat4 viewProj_ = Mat4::identity();
    uint32_t drawCalls_ = 0;
    uint32_t culled_ = 0;
    Vec4 planes_[2][6];
    int planeSets_ = 1;
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
