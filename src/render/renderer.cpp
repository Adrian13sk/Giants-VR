#include "render/renderer.h"

#include <cstddef>

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
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;
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
out vec4 oColor;
void main() {
    vec4 base = texture(uTex, vUv) * vColor * uTint;
    if (base.a < uAlphaRef) discard;
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    float ndl = max(dot(n, -uSunDir), 0.0);
    vec3 lit = base.rgb * (uAmbient + uSunColor * ndl);
    vec3 rgb = mix(lit, base.rgb, uUnlit);
    highp float d = length(vWorldPos - uEyePos);
    float fog = clamp(1.0 - (d - uFogRange.x) * uFogRange.y, 0.0, 1.0);
    oColor = vec4(mix(uFogColor, rgb, fog), base.a);
}
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
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
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
    m.live = true;
    meshes_.push_back(m);
    checkGl("uploadMesh");
    return static_cast<MeshId>(meshes_.size());
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
    if (p.noFog) glUniform2f(uFogRange_, 1e9f, 0.0f);
    glBindVertexArray(m.vao);
    glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
    if (p.noFog) {
        const float fogSpan = env_.fogFar > env_.fogNear ? env_.fogFar - env_.fogNear : 1e9f;
        glUniform2f(uFogRange_, env_.fogNear, 1.0f / fogSpan);
    }
    ++drawCalls_;
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
