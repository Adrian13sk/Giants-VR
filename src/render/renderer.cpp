#include "render/renderer.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>

#include "core/log.h"

namespace gvr {

bool checkGl(const char* where) {
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) return true;
    while (err != GL_NO_ERROR) {
        LOGE("GL error 0x%04x at %s", err, where);
        err = glGetError();
    }
    return false;
}

void MeshData::append(const MeshData& other, const Mat4& t) {
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    for (Vertex v : other.vertices) {
        Vec4 p = t * Vec4{v.pos[0], v.pos[1], v.pos[2], 1.0f};
        Vec4 n = t * Vec4{v.normal[0], v.normal[1], v.normal[2], 0.0f};
        Vec3 nn = normalize(Vec3{n.x, n.y, n.z});
        v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
        v.normal[0] = nn.x; v.normal[1] = nn.y; v.normal[2] = nn.z;
        vertices.push_back(v);
    }
    for (uint32_t i : other.indices) indices.push_back(base + i);
}

namespace {

const char* kVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;
out vec4 vColor;
void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUv = aUv;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 300 es
precision mediump float;
in highp vec3 vWorldPos;
in vec3 vNormal;
in highp vec2 vUv;
in vec4 vColor;
uniform sampler2D uTex;
uniform vec4 uTint;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uAmbient;
uniform vec3 uFogColor;
uniform vec2 uFogRange;  // near, 1/(far-near)
uniform highp vec3 uEyePos;
uniform float uUnlit;
uniform float uAlphaRef;
uniform int uMaterial;        // 0 standard, 1 water, 2 sky
uniform highp float uTime;
uniform highp float uSeaLevel;
uniform vec3 uWaterColor;
uniform vec3 uSkyHorizon;
uniform vec3 uSkyZenith;
uniform highp sampler2DShadow uShadowMap;
uniform highp mat4 uShadowMat;
uniform float uShadowOn;
uniform highp float uShadowTexel;
out vec4 oColor;

float sunShadow(highp vec3 wp, vec3 n) {
    if (uShadowOn < 0.5) return 1.0;
    highp vec4 sp = uShadowMat * vec4(wp + n * 0.06, 1.0);
    highp vec3 c = sp.xyz / sp.w * 0.5 + 0.5;
    if (c.x <= 0.0 || c.x >= 1.0 || c.y <= 0.0 || c.y >= 1.0 || c.z >= 1.0) return 1.0;
    highp float z = c.z - 0.0012;
    float s = texture(uShadowMap, vec3(c.xy + vec2(-0.5, -0.5) * uShadowTexel, z));
    s += texture(uShadowMap, vec3(c.xy + vec2(0.5, -0.5) * uShadowTexel, z));
    s += texture(uShadowMap, vec3(c.xy + vec2(-0.5, 0.5) * uShadowTexel, z));
    s += texture(uShadowMap, vec3(c.xy + vec2(0.5, 0.5) * uShadowTexel, z));
    highp vec2 e = min(c.xy, 1.0 - c.xy);
    float edge = clamp(min(e.x, e.y) * 12.0, 0.0, 1.0);  // fades out toward the map's border
    return mix(1.0, s * 0.25, edge);
}

void main() {
    highp float d = length(vWorldPos - uEyePos);
    float fog = clamp(1.0 - (d - uFogRange.x) * uFogRange.y, 0.0, 1.0);
    if (uMaterial == 2) {
        // Sky band: clouds drift slowly; the sun's disc and glow sit where the light comes from.
        // Above the painted band the dome fades to the zenith colour (vertex alpha = band weight).
        vec4 b = texture(uTex, vUv + vec2(uTime * 0.0015, 0.0)) * uTint;
        b.rgb = mix(uSkyZenith, b.rgb, vColor.a);
        highp vec3 vd = normalize(vWorldPos - uEyePos);
        highp float sd = max(dot(vd, -normalize(uSunDir)), 0.0);  // highp: the disc edge is 1e-4 wide
        vec3 rgb = b.rgb + uSunColor * (smoothstep(0.9993, 0.9997, sd) * 6.0 + pow(sd, 48.0) * 0.45 + pow(sd, 6.0) * 0.12);
        oColor = vec4(rgb, 1.0);
        return;
    }
    if (uMaterial == 1) {
        // Water surface: four travelling waves give the normal; fresnel mixes the water body with
        // the reflected sky; the sun glints off the crests. Shallows show the tinted bed through it.
        highp vec2 p = vWorldPos.xz;
        highp float t = uTime;
        highp vec2 g = vec2(0.0);
        g += 0.110 * vec2(0.6, 0.8) * cos(dot(vec2(0.6, 0.8), p) * 0.9 + t * 1.2);
        g += 0.090 * vec2(-0.7, 0.5) * cos(dot(vec2(-0.7, 0.5), p) * 1.6 + t * 1.8);
        g += 0.070 * vec2(0.2, -0.9) * cos(dot(vec2(0.2, -0.9), p) * 2.9 + t * 2.6);
        g += 0.050 * vec2(-0.9, -0.3) * cos(dot(vec2(-0.9, -0.3), p) * 5.1 + t * 3.4);
        g *= clamp(1.0 - d / 160.0, 0.12, 1.0);  // calmer with distance (no moire on far waves)
        vec3 n = normalize(vec3(-g.x, 1.0, -g.y));
        vec3 v = normalize(uEyePos - vWorldPos);
        float fres = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
        vec3 r = reflect(-v, n);
        vec3 sky = mix(uSkyHorizon, uSkyZenith, clamp(r.y * 1.5, 0.0, 1.0));
        float sh = sunShadow(vWorldPos, vec3(0.0, 1.0, 0.0));
        float spec = pow(max(dot(r, -uSunDir), 0.0), 260.0) * sh;
        vec4 tex = 0.5 * texture(uTex, vUv + vec2(t * 0.010, t * 0.006)) + 0.5 * texture(uTex, vUv * 1.7 - vec2(t * 0.008, -t * 0.011));
        vec3 body = uWaterColor * (0.55 + 0.9 * tex.rgb) * (uAmbient + uSunColor * 0.5 * sh);
        vec3 rgb = mix(body, sky, fres) + uSunColor * spec * 4.0;
        float a = mix(0.70, 0.97, fres) * uTint.a;
        oColor = vec4(mix(uFogColor, rgb, fog), a);
        return;
    }
    vec4 base = texture(uTex, vUv) * vColor * uTint;
    if (base.a < uAlphaRef) discard;
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    float sh = sunShadow(vWorldPos, n);
    float ndl = max(dot(n, -uSunDir), 0.0);
    vec3 lit = base.rgb * (uAmbient + uSunColor * ndl * sh);
    // Baked lighting (lightmaps, vertex colours) still darkens under a moving shadow.
    vec3 rgb = mix(lit, base.rgb * mix(0.38, 1.0, sh), uUnlit);
    if (vWorldPos.y < uSeaLevel) {
        // Under water: the bed fades into the water colour with depth.
        float dep = uSeaLevel - vWorldPos.y;
        rgb = mix(rgb, uWaterColor * 0.8, clamp(0.25 + dep * 0.22, 0.0, 0.9));
    }
    oColor = vec4(mix(uFogColor, rgb, fog), base.a);
#ifdef DEBUG_SHADOW
    oColor = vec4(vec3(sh), 1.0);
#endif
}
)";

const char* kShadowVS = R"(#version 300 es
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
)";
const char* kShadowFS = R"(#version 300 es
precision mediump float;
void main() {}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("Shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

bool Renderer::init() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    std::string fsrc = kFragmentShader;
    if (std::getenv("GVR_DEBUG_SHADOW")) fsrc.insert(fsrc.find('\n') + 1, "#define DEBUG_SHADOW 1\n");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc.c_str());
    if (!vs || !fs) return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        LOGE("Program link failed: %s", log);
        return false;
    }
    uMVP_ = glGetUniformLocation(program_, "uMVP");
    uModel_ = glGetUniformLocation(program_, "uModel");
    uTint_ = glGetUniformLocation(program_, "uTint");
    uSunDir_ = glGetUniformLocation(program_, "uSunDir");
    uSunColor_ = glGetUniformLocation(program_, "uSunColor");
    uAmbient_ = glGetUniformLocation(program_, "uAmbient");
    uFogColor_ = glGetUniformLocation(program_, "uFogColor");
    uFogRange_ = glGetUniformLocation(program_, "uFogRange");
    uEyePos_ = glGetUniformLocation(program_, "uEyePos");
    uUnlit_ = glGetUniformLocation(program_, "uUnlit");
    uAlphaRef_ = glGetUniformLocation(program_, "uAlphaRef");
    uTex_ = glGetUniformLocation(program_, "uTex");
    uMaterial_ = glGetUniformLocation(program_, "uMaterial");
    uTime_ = glGetUniformLocation(program_, "uTime");
    uSeaLevel_ = glGetUniformLocation(program_, "uSeaLevel");
    uWaterColor_ = glGetUniformLocation(program_, "uWaterColor");
    uSkyHorizon_ = glGetUniformLocation(program_, "uSkyHorizon");
    uSkyZenith_ = glGetUniformLocation(program_, "uSkyZenith");
    uShadowMap_ = glGetUniformLocation(program_, "uShadowMap");
    uShadowMat_ = glGetUniformLocation(program_, "uShadowMat");
    uShadowOn_ = glGetUniformLocation(program_, "uShadowOn");
    uShadowTexel_ = glGetUniformLocation(program_, "uShadowTexel");
    {
        // A 1x1 depth texture bound to the shadow sampler while shadows are off (a sampler with
        // no complete texture is undefined).
        glGenTextures(1, &dummyShadowTex_);
        glBindTexture(GL_TEXTURE_2D, dummyShadowTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, 1, 1, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    const uint8_t white[4] = {255, 255, 255, 255};
    whiteTexture_ = 0;
    TextureId w = uploadTexture(1, 1, white, false, true);
    whiteTexture_ = textures_[w - 1].tex;
    meshes_.reserve(256);
    return checkGl("Renderer::init");
}

void Renderer::shutdown() {
    for (size_t i = 0; i < meshes_.size(); ++i) destroyMesh(static_cast<MeshId>(i + 1));
    for (size_t i = 0; i < textures_.size(); ++i) destroyTexture(static_cast<TextureId>(i + 1));
    meshes_.clear();
    textures_.clear();
    if (program_) glDeleteProgram(program_);
    program_ = 0;
}

MeshId Renderer::uploadMesh(const MeshData& data) {
    if (data.vertices.empty() || data.indices.empty()) return 0;
    GpuMesh m;
    glGenVertexArrays(1, &m.vao);
    glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, data.vertices.size() * sizeof(Vertex), data.vertices.data(),
                 GL_STATIC_DRAW);
    glGenBuffers(1, &m.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, data.indices.size() * sizeof(uint32_t),
                 data.indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, uv)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, color)));
    glBindVertexArray(0);
    m.indexCount = static_cast<GLsizei>(data.indices.size());
    m.vertexCount = data.vertices.size();
    m.live = true;
    meshes_.push_back(m);
    checkGl("uploadMesh");
    return static_cast<MeshId>(meshes_.size());
}

bool Renderer::updateMeshVertices(MeshId id, const std::vector<Vertex>& vertices) {
    if (id == 0 || id > meshes_.size()) return false;
    GpuMesh& m = meshes_[id - 1];
    if (!m.live || vertices.size() != m.vertexCount) return false;
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(Vertex), vertices.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void Renderer::destroyMesh(MeshId id) {
    if (id == 0 || id > meshes_.size()) return;
    GpuMesh& m = meshes_[id - 1];
    if (!m.live) return;
    glDeleteBuffers(1, &m.vbo);
    glDeleteBuffers(1, &m.ibo);
    glDeleteVertexArrays(1, &m.vao);
    m = GpuMesh{};
}

TextureId Renderer::uploadTexture(int width, int height, const uint8_t* rgba, bool mipmaps,
                                  bool repeat) {
    GpuTexture t;
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Textures are authored in sRGB; sample them as sRGB so lighting happens in linear space
    // and the sRGB framebuffer re-encodes on write.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba);
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    t.live = true;
    textures_.push_back(t);
    checkGl("uploadTexture");
    return static_cast<TextureId>(textures_.size());
}

void Renderer::destroyTexture(TextureId id) {
    if (id == 0 || id > textures_.size()) return;
    GpuTexture& t = textures_[id - 1];
    if (!t.live) return;
    glDeleteTextures(1, &t.tex);
    t = GpuTexture{};
}

void Renderer::beginView(const ViewParams& view, int w, int h, bool clear) {
    currentView_ = view;
    viewProj_ = view.projection * view.view;
    // Gribb-Hartmann plane extraction (column-major m[col*4+row]); far plane skipped (infinite).
    const float* m = viewProj_.m;
    auto row = [&](int r) { return Vec4{m[r], m[4 + r], m[8 + r], m[12 + r]}; };
    const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    auto add = [](Vec4 a, Vec4 b, float s) { return Vec4{a.x + s * b.x, a.y + s * b.y, a.z + s * b.z, a.w + s * b.w}; };
    planes_[0] = add(r3, r0, 1); planes_[1] = add(r3, r0, -1);
    planes_[2] = add(r3, r1, 1); planes_[3] = add(r3, r1, -1);
    planes_[4] = add(r3, r2, 1); planes_[5] = Vec4{0, 0, 0, 1};
    for (int i = 0; i < 5; ++i) {
        const float l = std::sqrt(planes_[i].x * planes_[i].x + planes_[i].y * planes_[i].y + planes_[i].z * planes_[i].z);
        if (l > 0) planes_[i] = {planes_[i].x / l, planes_[i].y / l, planes_[i].z / l, planes_[i].w / l};
    }
    glViewport(0, 0, w, h);
    glScissor(0, 0, w, h);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    if (clear) {
        glClearColor(env_.clearColor.x, env_.clearColor.y, env_.clearColor.z, 1.0f);
        glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    glUseProgram(program_);
    Vec3 sd = normalize(env_.sunDirection);
    glUniform3f(uSunDir_, sd.x, sd.y, sd.z);
    glUniform3f(uSunColor_, env_.sunColor.x, env_.sunColor.y, env_.sunColor.z);
    glUniform3f(uAmbient_, env_.ambientColor.x, env_.ambientColor.y, env_.ambientColor.z);
    glUniform3f(uFogColor_, env_.fogColor.x, env_.fogColor.y, env_.fogColor.z);
    const float fogSpan = env_.fogFar > env_.fogNear ? env_.fogFar - env_.fogNear : 1e9f;
    glUniform2f(uFogRange_, env_.fogNear, 1.0f / fogSpan);
    glUniform3f(uEyePos_, view.eyePosition.x, view.eyePosition.y, view.eyePosition.z);
    glUniform1i(uTex_, 0);
    glUniform1i(uMaterial_, 0);
    glUniform1f(uTime_, env_.time);
    glUniform1f(uSeaLevel_, env_.seaLevel);
    glUniform3f(uWaterColor_, env_.waterColor.x, env_.waterColor.y, env_.waterColor.z);
    glUniform3f(uSkyHorizon_, env_.skyHorizon.x, env_.skyHorizon.y, env_.skyHorizon.z);
    glUniform3f(uSkyZenith_, env_.skyZenith.x, env_.skyZenith.y, env_.skyZenith.z);
    glUniform1i(uShadowMap_, 1);
    glActiveTexture(GL_TEXTURE1);
    const bool sh = shadowsActive_ && shadowTex_;
    glBindTexture(GL_TEXTURE_2D, sh ? shadowTex_ : dummyShadowTex_);
    glUniform1f(uShadowOn_, sh ? 1.0f : 0.0f);
    glUniformMatrix4fv(uShadowMat_, 1, GL_FALSE, shadowMat_.m);
    glUniform1f(uShadowTexel_, shadowSize_ > 0 ? 1.0f / float(shadowSize_) : 0.0f);
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::draw(MeshId id, const Mat4& model, const DrawParams& p) {
    if (id == 0 || id > meshes_.size()) return;
    const GpuMesh& m = meshes_[id - 1];
    if (!m.live) return;
    Mat4 mvp = viewProj_ * model;
    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, model.m);
    glUniform4f(uTint_, p.tint.x, p.tint.y, p.tint.z, p.tint.w);
    glUniform1f(uUnlit_, p.unlit ? 1.0f : 0.0f);
    glUniform1i(uMaterial_, int(p.material));
    GLuint tex = whiteTexture_;
    if (p.texture != 0 && p.texture <= textures_.size() && textures_[p.texture - 1].live)
        tex = textures_[p.texture - 1].tex;
    glBindTexture(GL_TEXTURE_2D, tex);

    switch (p.blend) {
        case BlendMode::Opaque:
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
            glUniform1f(uAlphaRef_, -1.0f);
            break;
        case BlendMode::AlphaTest:
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
            glUniform1f(uAlphaRef_, 0.5f);
            break;
        case BlendMode::AlphaBlend:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glUniform1f(uAlphaRef_, 0.004f);
            break;
        case BlendMode::Additive:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
            glUniform1f(uAlphaRef_, 0.004f);
            break;
    }
    if (p.doubleSided) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);
    if (!p.depthWrite) glDepthMask(GL_FALSE);
    if (!p.depthTest) glDisable(GL_DEPTH_TEST);
    if (p.noFog) glUniform2f(uFogRange_, 1e9f, 0.0f);
    glBindVertexArray(m.vao);
    glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
    if (!p.depthTest) glEnable(GL_DEPTH_TEST);
    if (p.noFog) {
        const float fogSpan = env_.fogFar > env_.fogNear ? env_.fogFar - env_.fogNear : 1e9f;
        glUniform2f(uFogRange_, env_.fogNear, 1.0f / fogSpan);
    }
    ++drawCalls_;
}

bool Renderer::initShadows(int size) {
    GLuint vs = compile(GL_VERTEX_SHADER, kShadowVS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kShadowFS);
    if (!vs || !fs) return false;
    shadowProgram_ = glCreateProgram();
    glAttachShader(shadowProgram_, vs);
    glAttachShader(shadowProgram_, fs);
    glLinkProgram(shadowProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(shadowProgram_, GL_LINK_STATUS, &ok);
    if (!ok) { LOGE("Shadow program link failed"); return false; }
    uShadowMVP_ = glGetUniformLocation(shadowProgram_, "uMVP");
    glGenTextures(1, &shadowTex_);
    glBindTexture(GL_TEXTURE_2D, shadowTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &shadowFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex_, 0);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!complete) {
        LOGE("Shadow framebuffer incomplete");
        glDeleteFramebuffers(1, &shadowFbo_);
        shadowFbo_ = 0;
        return false;
    }
    shadowSize_ = size;
    LOGI("Sun shadows: %dx%d depth map", size, size);
    return checkGl("initShadows");
}

Mat4 Renderer::sunShadowMatrix(const Vec3& centre, float half) const {
    const Vec3 d = normalize(env_.sunDirection);  // direction the light travels
    // Light basis: forward = d, right/up perpendicular.
    const Vec3 upHint = std::fabs(d.y) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 rgt = normalize(cross(upHint, d));
    const Vec3 up = cross(d, rgt);
    // Snap the centre to whole shadow texels (no shimmering edges as the player moves).
    const float texel = shadowSize_ > 0 ? 2.0f * half / float(shadowSize_) : 0.05f;
    float cx = dot(centre, rgt), cy = dot(centre, up);
    cx = std::floor(cx / texel) * texel;
    cy = std::floor(cy / texel) * texel;
    const float cz = dot(centre, d);
    const float depth = 250.0f;
    // Rows: x = (p.rgt - cx)/half, y = (p.up - cy)/half, z maps [cz - depth/2, cz + depth/2] to [-1, 1].
    Mat4 m = Mat4::identity();
    m.m[0] = rgt.x / half; m.m[4] = rgt.y / half; m.m[8] = rgt.z / half; m.m[12] = -cx / half;
    m.m[1] = up.x / half;  m.m[5] = up.y / half;  m.m[9] = up.z / half;  m.m[13] = -cy / half;
    m.m[2] = d.x * 2.0f / depth; m.m[6] = d.y * 2.0f / depth; m.m[10] = d.z * 2.0f / depth; m.m[14] = -cz * 2.0f / depth;
    m.m[3] = 0; m.m[7] = 0; m.m[11] = 0; m.m[15] = 1;
    return m;
}

void Renderer::beginShadowPass(const Mat4& lightViewProj) {
    shadowMat_ = lightViewProj;
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glViewport(0, 0, shadowSize_, shadowSize_);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);  // thin and open meshes (leaves, planks) cast from both sides
    glClearDepthf(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.0f);
    glUseProgram(shadowProgram_);
}

void Renderer::drawShadow(MeshId id, const Mat4& model) {
    if (id == 0 || id > meshes_.size()) return;
    const GpuMesh& m = meshes_[id - 1];
    if (!m.live) return;
    const Mat4 mvp = shadowMat_ * model;
    glUniformMatrix4fv(uShadowMVP_, 1, GL_FALSE, mvp.m);
    glBindVertexArray(m.vao);
    glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
    ++drawCalls_;
}

void Renderer::endShadowPass() {
    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool Renderer::sphereVisible(const Vec3& c, float r) const {
    for (int i = 0; i < 5; ++i)
        if (planes_[i].x * c.x + planes_[i].y * c.y + planes_[i].z * c.z + planes_[i].w < -r) return false;
    return true;
}

void Renderer::endView() {
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

// ---------------------------------------------------------------------------------------------
namespace prim {
namespace {
Vertex mk(const Vec3& p, const Vec3& n, float u, float v, const uint8_t c[4]) {
    Vertex vx{};
    vx.pos[0] = p.x; vx.pos[1] = p.y; vx.pos[2] = p.z;
    vx.normal[0] = n.x; vx.normal[1] = n.y; vx.normal[2] = n.z;
    vx.uv[0] = u; vx.uv[1] = v;
    for (int i = 0; i < 4; ++i) vx.color[i] = c[i];
    return vx;
}
}  // namespace

MeshData box(const Vec3& h, const uint8_t c[4]) {
    MeshData m;
    const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : normals) {
        // Build two tangents for the face.
        Vec3 up = std::fabs(n.y) > 0.5f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        Vec3 t = cross(up, n);
        Vec3 b = cross(n, t);
        Vec3 center{n.x * h.x, n.y * h.y, n.z * h.z};
        Vec3 te{t.x * h.x, t.y * h.y, t.z * h.z};
        Vec3 be{b.x * h.x, b.y * h.y, b.z * h.z};
        uint32_t base = static_cast<uint32_t>(m.vertices.size());
        m.vertices.push_back(mk(center - te - be, n, 0, 0, c));
        m.vertices.push_back(mk(center + te - be, n, 1, 0, c));
        m.vertices.push_back(mk(center + te + be, n, 1, 1, c));
        m.vertices.push_back(mk(center - te + be, n, 0, 1, c));
        uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t i : idx) m.indices.push_back(base + i);
    }
    return m;
}

MeshData plane(float hs, int sub, float uvRepeat, const uint8_t c[4]) {
    MeshData m;
    const int n = sub + 1;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            float fx = -hs + 2 * hs * x / sub, fz = -hs + 2 * hs * z / sub;
            m.vertices.push_back(mk({fx, 0, fz}, {0, 1, 0}, uvRepeat * x / sub,
                                    uvRepeat * z / sub, c));
        }
    for (int z = 0; z < sub; ++z)
        for (int x = 0; x < sub; ++x) {
            uint32_t i0 = z * n + x, i1 = i0 + 1, i2 = i0 + n, i3 = i2 + 1;
            // CCW when viewed from +Y.
            m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    return m;
}

MeshData sphere(float r, int slices, int stacks, const uint8_t c[4]) {
    MeshData m;
    for (int i = 0; i <= stacks; ++i) {
        float v = static_cast<float>(i) / stacks;
        float phi = v * kPi;
        for (int j = 0; j <= slices; ++j) {
            float u = static_cast<float>(j) / slices;
            float th = u * 2 * kPi;
            Vec3 n{std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)};
            m.vertices.push_back(mk(n * r, n, u, v, c));
        }
    }
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j) {
            uint32_t a = i * (slices + 1) + j, b = a + slices + 1;
            m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    return m;
}

MeshData cylinder(float r, float hgt, int slices, const uint8_t c[4]) {
    MeshData m;
    for (int j = 0; j <= slices; ++j) {
        float th = 2 * kPi * j / slices;
        Vec3 n{std::cos(th), 0, std::sin(th)};
        m.vertices.push_back(mk({n.x * r, 0, n.z * r}, n, float(j) / slices, 0, c));
        m.vertices.push_back(mk({n.x * r, hgt, n.z * r}, n, float(j) / slices, 1, c));
    }
    for (int j = 0; j < slices; ++j) {
        uint32_t a = j * 2;
        m.indices.insert(m.indices.end(), {a, a + 1, a + 2, a + 2, a + 1, a + 3});
    }
    // Caps
    for (int cap = 0; cap < 2; ++cap) {
        float y = cap ? hgt : 0;
        Vec3 n{0, cap ? 1.0f : -1.0f, 0};
        uint32_t center = static_cast<uint32_t>(m.vertices.size());
        m.vertices.push_back(mk({0, y, 0}, n, 0.5f, 0.5f, c));
        for (int j = 0; j <= slices; ++j) {
            float th = 2 * kPi * j / slices;
            m.vertices.push_back(mk({std::cos(th) * r, y, std::sin(th) * r}, n, 0, 0, c));
        }
        for (int j = 0; j < slices; ++j) {
            uint32_t a = center + 1 + j;
            if (cap) m.indices.insert(m.indices.end(), {center, a + 1, a});
            else m.indices.insert(m.indices.end(), {center, a, a + 1});
        }
    }
    return m;
}

}  // namespace prim
}  // namespace gvr
