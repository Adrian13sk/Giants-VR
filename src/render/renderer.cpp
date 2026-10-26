#include "render/renderer.h"

#include <cmath>
#include <cstring>
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
#ifdef MULTIVIEW
layout(num_views = 2) in;
uniform highp mat4 uViewProj[2];
uniform highp vec3 uEyePosV[2];
flat out highp vec3 vEyePos;
#else
uniform mat4 uMVP;
#endif
uniform mat4 uModel;
uniform highp vec2 uSway;  // wind: amplitude (m at the card's top), time (s)
uniform mediump int uMaterial;  // (4: rework grass - the blade's height fraction is in the normal's y)
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;
out vec4 vColor;
void main() {
    highp vec3 p = aPos;
    if (uSway.x > 0.0) {
        // Gusts travelling across the field: two waves; only the top of the card moves.
        highp float ph = uSway.y * 1.7 + p.x * 0.35 + p.z * 0.27;
        float k = (uMaterial == 4 ? aNormal.y * aNormal.y : 1.0 - aUv.y) * uSway.x;
        p.x += k * (sin(ph) * 0.8 + sin(ph * 2.3 + 1.3) * 0.35);
        p.z += k * (cos(ph * 0.9 + 0.4) * 0.5);
    }
    if (uSway.x < 0.0) {
        // Trees (rework): the whole palm bends in the wind, more the higher up (quadratic in the
        // height above its base, -uSway.x metres at 10 m); each tree's own phase from its position.
        highp vec3 w0 = (uModel * vec4(p, 1.0)).xyz;
        highp float h = clamp(w0.y - uModel[3].y, 0.0, 14.0);
        highp float ph = uSway.y * 1.1 + uModel[3].x * 0.07 + uModel[3].z * 0.05;
        highp float k = -uSway.x * h * h * 0.01;
        highp vec3 d = vec3(k * (sin(ph) * 0.7 + sin(ph * 2.6 + 1.1) * 0.3), 0.0, k * cos(ph * 0.8 + 0.5) * 0.45);
        // World offset -> model space (rotation and uniform scale: the transpose over the scale squared).
        p += transpose(mat3(uModel)) * d / max(dot(uModel[0].xyz, uModel[0].xyz), 1e-6);
    }
    vec4 wp = uModel * vec4(p, 1.0);
    vWorldPos = wp.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUv = aUv;
    vColor = aColor;
#ifdef MULTIVIEW
    vEyePos = uEyePosV[gl_ViewID_OVR];
    gl_Position = uViewProj[gl_ViewID_OVR] * wp;
#else
    gl_Position = uMVP * vec4(p, 1.0);
#endif
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
#ifdef MULTIVIEW
flat in highp vec3 vEyePos;  // this eye's position (the vertex shader picks it by view)
#define uEyePos vEyePos
#else
uniform highp vec3 uEyePos;
#endif
uniform float uUnlit;         // 0 lit, 1 baked (lightmaps / vertex colours), 2 emissive (effects, overlays)
uniform float uAlphaRef;
uniform mediump int uMaterial; // 0 standard, 1 water, 2 sky, 3 terrain, 4 grass
uniform highp float uTime;
uniform highp float uSeaLevel;
uniform highp float uClipY;   // reflection pass: nothing below the water surface
uniform highp vec4 uNearCut;  // first person: the own body is not drawn within w of xyz (the eyes)
uniform vec3 uWaterColor;
uniform vec3 uSkyHorizon;
uniform vec3 uSkyZenith;
uniform highp sampler2DShadow uShadowMap;
uniform highp mat4 uShadowMat;
uniform float uShadowOn;
uniform highp float uShadowTexel;
uniform vec4 uGrade;          // saturation, contrast, warmth, exposure
// Planar water reflection (one mirrored render per frame) and the water depth map.
uniform sampler2D uReflTex;
uniform highp sampler2D uReflDepth;
uniform highp mat4 uReflMat;
uniform highp vec3 uReflEye;
uniform highp float uReflNear;
uniform float uReflOn;
uniform sampler2D uWaterDepth;
uniform highp vec4 uWaterDepthRect;  // x0, z0, 1/width, 1/depth (world metres)
uniform float uWaterDepthOn;
uniform sampler2D uTerrRock;
uniform sampler2D uTerrSand;
uniform sampler2D uTerrGrass2;
uniform sampler2D uTerrRock2;
uniform sampler2D uTerrNoise;
uniform sampler2D uGrassTex;  // rework grass (material 4): the blades' shape and shading
uniform float uTerrDetail;
uniform vec3 uSkyTint;
uniform float uNight;
uniform vec3 uBakedLight;
uniform vec4 uPointPos;    // xyz position, w = 1 / radius^2
uniform vec3 uPointColor;  // 0: no point light
uniform sampler2D uSkyTex;  // rework: the world's sky band, for haze that dissolves into the sky behind
uniform float uSkyOn;
uniform float uLook;       // 0: the classic look; 1: the rework's (detail relief, specular, filmic tone, sun haze)
out vec4 oColor;

// Relief from the texture itself (bump mapping without tangents, Mikkelsen 2010): the colour's
// brightness taken as height; screen-space derivatives give the surface slope. h_m: metres of
// relief per unit of brightness.
vec3 bumpNormal(vec3 n, highp vec3 p, float h, float h_m) {
    highp vec3 dpdx = dFdx(p), dpdy = dFdy(p);
    float dhdx = dFdx(h) * h_m, dhdy = dFdy(h) * h_m;
    highp vec3 r1 = cross(dpdy, n), r2 = cross(n, dpdx);
    highp float det = dot(dpdx, r1);
    highp vec3 grad = sign(det) * (dhdx * r1 + dhdy * r2);
    highp vec3 b = abs(det) * n - grad;
    return dot(b, b) > 1e-20 ? normalize(vec3(b)) : n;
}

// Fog: the classic linear fog; the rework adds the sun's glow in the haze toward it.
vec3 fogMix(vec3 rgb, float fog, highp vec3 wp) {
    if (uLook < 0.5) return mix(uFogColor, rgb, fog);
    highp vec3 vd = normalize(wp - uEyePos);
    float sunward = pow(max(dot(vd, -normalize(uSunDir)), 0.0), 6.0);
    vec3 haze = uFogColor + uSunColor * sunward * 0.22;
    return mix(haze, rgb, fog);
}

// Terrain (material 3): the world's ground texture, varied with a second grass/earth in patches;
// rock on slopes (projected on the dominant side axis, so cliffs aren't stretched), two rocks
// mixed in large patches; a sand beach just above the sea and sand under it, wet below the
// waterline; fine grain up close. Weights come from slope, height above the sea and noise.
// Set by terrainColor for the relief in main(): how much rock, and its brightness (as height).
float gRockW = 0.0, gRockH = 0.0;
vec3 terrainColor(highp vec3 wp, vec3 n, vec2 uv, highp float d, float bias) {
    highp vec2 xz = wp.xz;
    vec2 nz = texture(uTerrNoise, xz * 0.006).rg;              // ~170 m: large patches
    float nzp = texture(uTerrNoise, xz * 0.031 + vec2(0.37, 0.71)).r;  // ~32 m: small patches
    vec3 grass = texture(uTex, uv, bias).rgb;  // (bias: grass blades take the ground's local average, not one texel)
    float slope = 1.0 - n.y;
    float rockW = smoothstep(0.26, 0.45, slope + (nzp - 0.5) * 0.14);
    highp float hs = wp.y - uSeaLevel;
    float sandW = (1.0 - smoothstep(0.8, 2.6 + nzp * 2.0, hs)) * (1.0 - smoothstep(0.32, 0.52, slope));
    vec3 col = grass;
    if (uTerrDetail > 0.5) {
        float gW = smoothstep(0.56, 0.74, nzp * 0.65 + nz.x * 0.55) * 0.85;  // noise sum averages ~0.6: about a third covered
        if (gW > 0.01) col = mix(col, texture(uTerrGrass2, xz * 0.085).rgb * vec3(1.06, 1.0, 0.86), gW);
        // Lusher / drier tint over the large patches.
        col *= mix(vec3(0.84, 1.08, 0.84), vec3(1.14, 1.03, 0.78), smoothstep(0.25, 0.75, nz.y));
    }
    if (sandW > 0.01) {
        vec3 sand = texture(uTerrSand, xz * 0.12).rgb;
        float l = dot(sand, vec3(0.299, 0.587, 0.114));
        sand = mix(vec3(l), sand, 0.45) * vec3(1.3, 1.22, 0.92);   // the desert sand paled to beach sand
        sand *= mix(1.0, 0.68, smoothstep(-0.1, 0.5, -hs));        // wet at and below the waterline
        col = mix(col, sand, sandW);
    }
    gRockW = 0.0;
    if (rockW > 0.01) {
        // The projection follows the true facet (screen-space derivatives), not the smoothed vertex
        // normal: on sheer terrain walls the smoothed normal still leans up, and the rock was smeared
        // into streaks (Rstory2's ice canyons).
        highp vec3 fn = cross(dFdx(wp), dFdy(wp));
        vec3 an = dot(fn, fn) > 1e-12 ? abs(normalize(vec3(fn))) : abs(n);
        highp vec2 ruv = an.x > an.z ? wp.zy : wp.xy;
        if (an.y > max(an.x, an.z)) ruv = xz;
        vec3 rock;
        if (uLook > 0.5 && uLook < 1.5) {
            // Rework (user: "the rocks textures" look awful): true triplanar (three projections
            // blended by the normal, no seams or smears where a cliff turns), strata bands along the
            // height warped by noise, a finer octave up close, and the rock's own texture as the relief
            // height (main(): gRockH), so cracks and ledges catch the light.
            vec3 w = an * an;
            w = w * w;
            w /= (w.x + w.y + w.z);
            highp vec3 q = wp * 0.11;
            rock = texture(uTerrRock, q.zy).rgb * w.x + texture(uTerrRock, q.xz).rgb * w.y + texture(uTerrRock, q.xy).rgb * w.z;
            // Anti-tiling: a second sample at about a quarter of the frequency, offset, blended in by
            // large-scale noise and more with distance - the 9 m repeat read as a grid on Kabuto's cliffs.
            highp vec3 q2 = wp * 0.027 + vec3(0.37, 0.61, 0.13);
            vec3 rockB = texture(uTerrRock, q2.zy).rgb * w.x + texture(uTerrRock, q2.xz).rgb * w.y + texture(uTerrRock, q2.xy).rgb * w.z;
            float mB = clamp(0.25 + 0.5 * smoothstep(0.3, 0.7, nz.x + 0.3 * (nzp - 0.5)) + 0.4 * smoothstep(25.0, 120.0, d), 0.0, 0.85);
            rock = mix(rock, rockB, mB);
            if (uTerrDetail > 0.5) rock = mix(rock, texture(uTerrRock2, ruv * 0.07).rgb * 1.1, smoothstep(0.42, 0.62, nz.x) * 0.6);
            // Strata: gently tilted layers (~9 m), warmer and cooler, lighter and darker.
            float band = sin(wp.y * 0.7 + nz.y * 5.0 + wp.x * 0.03) * 0.5 + 0.5;
            band = band * band * (3.0 - 2.0 * band);
            rock *= mix(vec3(0.78, 0.81, 0.86), vec3(1.2, 1.13, 1.03), band);  // (mostly light and dark: a strong warm cast yellowed Rstory2's ice)
            // Moss and earth caught on ledges and slopes that face up, in patches.
            float moss = smoothstep(0.3, 0.62, n.y) * smoothstep(0.35, 0.65, nzp + (nz.x - 0.5) * 0.4);
            rock = mix(rock, grass * vec3(0.9, 1.05, 0.75), moss * 0.75);  // strata: cooler and warmer layers
            float lr = dot(rock, vec3(0.299, 0.587, 0.114));
            float fine = 1.0;
            if (d < 30.0) {
                float f = dot(texture(uTerrRock, ruv * 0.47 + vec2(0.31, 0.17)).rgb, vec3(0.299, 0.587, 0.114));
                fine = mix(1.0, 0.7 + 0.6 * f / max(lr, 0.05), 0.5 * (1.0 - smoothstep(12.0, 30.0, d)));
            }
            rock *= clamp(fine, 0.6, 1.4);
            // Crevices darker, faces lighter: a contrast lift around the rock's own mean.
            rock = max(rock + (rock - vec3(lr)) * 0.15 + (lr - 0.3) * 0.25 * rock, vec3(0.0));
            gRockH = lr;
            gRockW = rockW;
        } else {
            rock = texture(uTerrRock, ruv * 0.11).rgb;
            if (uLook > 0.5) {
                // (the rework's fallback quality - Kabuto's worlds start there: one larger-scale sample
                // against the 9 m tiling grid)
                vec3 rockB = texture(uTerrRock, ruv * 0.027 + vec2(0.37, 0.61)).rgb;
                rock = mix(rock, rockB, clamp(0.3 + 0.45 * smoothstep(0.3, 0.7, nz.x) + 0.35 * smoothstep(25.0, 120.0, d), 0.0, 0.85));
            }
            if (uTerrDetail > 0.5) rock = mix(rock, texture(uTerrRock2, ruv * 0.07).rgb * 1.1, smoothstep(0.42, 0.62, nz.x) * 0.7);
        }
        col = mix(col, rock, rockW);
    }
    if (uTerrDetail > 0.5 && d < 30.0 && bias < 1.0) {  // (not for grass blades: their ground colour is an average)
        float g = texture(uTerrNoise, xz * 1.7).g;  // grain up close (fades out by 30 m)
        col *= mix(1.0, 0.82 + 0.36 * g, 1.0 - smoothstep(10.0, 30.0, d));
    }
    if (uLook > 0.5 && uLook < 1.5 && d < 60.0 && bias < 1.0) {
        // Rework: two more octaves of detail (clumps ~2.3 m, grit ~0.3 m) so the ground under your
        // feet isn't one blurred texel; fades with distance.
        float c1 = texture(uTerrNoise, xz * 0.43 + vec2(0.13, 0.57)).r;
        float c2 = texture(uTerrNoise, xz * 3.3 + vec2(0.71, 0.29)).g;
        float f = 1.0 - smoothstep(20.0, 60.0, d);
        col *= mix(1.0, (0.78 + 0.44 * c1) * (0.86 + 0.28 * c2), f);
    }
    return col;
}

float sunShadow(highp vec3 wp, vec3 n) {
    if (uShadowOn < 0.5) return 1.0;
    highp vec4 sp = uShadowMat * vec4(wp + n * 0.035, 1.0);
    highp vec3 c = sp.xyz / sp.w * 0.5 + 0.5;
    if (c.x <= 0.0 || c.x >= 1.0 || c.y <= 0.0 || c.y >= 1.0 || c.z >= 1.0) return 1.0;
    highp float z = c.z - 0.0005;
    highp vec2 e = min(c.xy, 1.0 - c.xy);
    float edge = clamp(min(e.x, e.y) * 12.0, 0.0, 1.0);  // fades out toward the map's border
    // Beyond 8 m one hardware-filtered tap is enough (texels there are far smaller than a pixel):
    // the Quest 2 measurements put the shadows at 1.4-1.8 ms of GPU time, mostly this sampling.
    highp vec3 dv = wp - uEyePos;
    if (dot(dv, dv) > 64.0) return mix(1.0, texture(uShadowMap, vec3(c.xy, z)), edge);
    // Up close, 5 bilinear PCF taps (hardware 2x2 each): soft edge about two texels wide.
    float s = texture(uShadowMap, vec3(c.xy, z)) * 2.0;
    s += texture(uShadowMap, vec3(c.xy + vec2(-0.75, -0.75) * uShadowTexel, z));
    s += texture(uShadowMap, vec3(c.xy + vec2(0.75, -0.75) * uShadowTexel, z));
    s += texture(uShadowMap, vec3(c.xy + vec2(-0.75, 0.75) * uShadowTexel, z));
    s += texture(uShadowMap, vec3(c.xy + vec2(0.75, 0.75) * uShadowTexel, z));
    return mix(1.0, s / 6.0, edge);
}

// Colour grade (in a gamma-2 space, so the curve acts on perceived brightness): saturation,
// an S-shaped contrast, warm highlights / cool shadows, exposure.
vec3 grade(vec3 c, float satScale) {
    if (uLook > 0.5) {
        // Rework: the painted textures' mid-tones as they were; only highlights roll off softly
        // (sun glints, bright sky) instead of clipping, by luminance so hues hold.
        float L = dot(c, vec3(0.2126, 0.7152, 0.0722));
        if (L > 0.6) {
            float Lc = 0.6 + (L - 0.6) / (1.0 + (L - 0.6) / 0.55);
            c *= Lc / L;
        }
    }
    vec3 g = sqrt(max(c * uGrade.w, vec3(0.0)));
    float l = dot(g, vec3(0.299, 0.587, 0.114));
    g = mix(vec3(l), g, mix(1.0, uGrade.x, satScale));
    g = clamp((g - 0.5) * uGrade.y + 0.5, 0.0, 1.2);
    g += (l - 0.45) * uGrade.z * vec3(1.0, 0.25, -0.9);
    g = max(g, vec3(0.0));
    return g * g;
}

// Rework haze: the sky band's own colour in that direction (just as the sky draws it), so distant
// ground dissolves into the sky behind it instead of a flat fog colour ending at a hard line.
vec3 skyHaze(highp vec3 vd) {
    highp float el = clamp(asin(clamp(vd.y, -1.0, 1.0)), -0.08, 0.75);
    highp vec2 suv = vec2(atan(vd.x, -vd.z) * 0.15915494 + uTime * 0.0015, mix(0.99, 0.01, (el + 0.08) / 0.83));
    vec3 b = textureLod(uSkyTex, suv, 2.0).rgb * uSkyTint;
    highp float sd = max(dot(vd, -normalize(uSunDir)), 0.0);
    return grade(b + uSunColor * (pow(sd, 48.0) * 0.45 + pow(sd, 6.0) * 0.12), 0.25);
}
vec3 fogOut(vec3 rgb, float fog, highp vec3 wp, float sat) {
    if (uLook < 0.5 || uLook > 1.5 || uSkyOn < 0.5) return grade(fogMix(rgb, fog, wp), sat);  // (the adaptive fallback, look 2: the flat haze - cheaper)
    return mix(skyHaze(normalize(wp - uEyePos)), grade(rgb, sat), fog);
}

void main() {
    if (vWorldPos.y < uClipY) discard;
    if (uNearCut.w > 0.0 && distance(vWorldPos, uNearCut.xyz) < uNearCut.w) discard;
    highp float d = length(vWorldPos - uEyePos);
    float fog = clamp(1.0 - (d - uFogRange.x) * uFogRange.y, 0.0, 1.0);
    if (uMaterial == 2) {
        // Sky band: clouds drift slowly; the sun's disc and glow sit where the light comes from.
        // Above the painted band the dome fades to the zenith colour (vertex alpha = band weight).
        vec4 b = texture(uTex, vUv + vec2(uTime * 0.0015, 0.0)) * uTint;
        b.rgb = mix(uSkyZenith, b.rgb * uSkyTint, vColor.a);
        highp vec3 vd = normalize(vWorldPos - uEyePos);
        if (uNight > 0.01) {
            // Stars: a hashed cell grid on the view direction, twinkling a little; above the horizon.
            highp vec3 cell = floor(vd * 420.0);
            highp float h = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
            float star = step(0.9975, h) * smoothstep(0.02, 0.2, vd.y) * (0.6 + 0.4 * sin(uTime * 3.0 + h * 400.0));
            b.rgb += vec3(star * uNight * 1.4);
        }
        highp float sd = max(dot(vd, -normalize(uSunDir)), 0.0);  // highp: the disc edge is 1e-4 wide
        vec3 rgb = b.rgb + uSunColor * (smoothstep(0.9993, 0.9997, sd) * 6.0 + pow(sd, 48.0) * 0.45 + pow(sd, 6.0) * 0.12);
        oColor = vec4(grade(rgb, 0.25), 1.0);  // the painted sky is already vivid
        return;
    }
    if (uMaterial == 1) {
        // Water: travelling waves and ripples give the normal; the reflection of the world (planar
        // reflection, corrected per eye with its depth) or the sky by fresnel; sun glints; the
        // depth below decides colour and clarity: clear turquoise shallows with foam at the shore,
        // dark deep water.
        highp vec2 p = vWorldPos.xz;
        highp float t = uTime;
        highp vec2 g = vec2(0.0);
        g += 0.100 * vec2(0.6, 0.8) * cos(dot(vec2(0.6, 0.8), p) * 0.9 + t * 1.2);
        g += 0.080 * vec2(-0.7, 0.5) * cos(dot(vec2(-0.7, 0.5), p) * 1.6 + t * 1.8);
        g += 0.060 * vec2(0.2, -0.9) * cos(dot(vec2(0.2, -0.9), p) * 2.9 + t * 2.6);
        g += 0.045 * vec2(-0.9, -0.3) * cos(dot(vec2(-0.9, -0.3), p) * 5.1 + t * 3.4);
        g += 0.030 * vec2(0.8, -0.55) * cos(dot(vec2(0.8, -0.55), p) * 9.3 + t * 4.7);
        g += 0.022 * vec2(-0.35, 0.94) * cos(dot(vec2(-0.35, 0.94), p) * 15.7 + t * 6.1);
        g *= clamp(1.0 - d / 160.0, 0.12, 1.0);  // calmer with distance (no moire on far waves)
        vec3 n = normalize(vec3(-g.x, 1.0, -g.y));
        highp vec3 v = normalize(uEyePos - vWorldPos);
        float ndv = max(dot(n, v), 0.0);
        float fres = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
        vec3 r = reflect(-v, n);
        vec3 refl = mix(uSkyHorizon, uSkyZenith, clamp(r.y * 1.5, 0.0, 1.0));
        if (uReflOn > 0.5) {
            highp vec4 cw = uReflMat * vec4(vWorldPos, 1.0);
            highp vec2 uv0 = cw.xy / cw.w * 0.5 + 0.5;
            highp float dz = texture(uReflDepth, uv0).r;
            // Distance to what is reflected (from the reflection camera's depth), then where that
            // point is for THIS eye's reflected ray: correct stereo depth for the reflection.
            highp float s0 = dz < 0.99999 ? length(vWorldPos - uReflEye) * max(0.0, uReflNear / (1.0 - dz) / cw.w - 1.0) : 2000.0;
            highp vec3 X = vWorldPos + normalize(reflect(-v, vec3(0.0, 1.0, 0.0))) * s0;
            highp vec4 cx = uReflMat * vec4(X, 1.0);
            highp vec2 uv = cx.xy / cx.w * 0.5 + 0.5 + g * 0.06;
            vec4 rc = texture(uReflTex, clamp(uv, vec2(0.002), vec2(0.998)));
            refl = mix(refl, min(rc.rgb, vec3(1.4)), rc.a);  // the sun disc reflected: bright, not a white blot
        }
        float sh = sunShadow(vWorldPos, vec3(0.0, 1.0, 0.0));
        float spec = pow(max(dot(r, -uSunDir), 0.0), 260.0) * sh;
        float depth = 30.0;
        if (uWaterDepthOn > 0.5) {
            highp vec2 wuv = (p - uWaterDepthRect.xy) * uWaterDepthRect.zw;
            if (wuv.x > 0.0 && wuv.y > 0.0 && wuv.x < 1.0 && wuv.y < 1.0) depth = texture(uWaterDepth, wuv).r * 24.0;
        }
        vec4 tex = 0.5 * texture(uTex, vUv + vec2(t * 0.010, t * 0.006)) + 0.5 * texture(uTex, vUv * 1.7 - vec2(t * 0.008, -t * 0.011));
        float deep = smoothstep(0.0, 6.0, depth);
        vec3 shallowCol = vec3(0.10, 0.36, 0.32);
        vec3 bodyCol = mix(shallowCol, uWaterColor, deep);
        vec3 body = bodyCol * (0.55 + 0.9 * tex.rgb) * (uAmbient + uSunColor * 0.6 * sh);
        // Stylised fresnel: never below ~22 %, so the world (and you) show in the water up close.
        float k = mix(0.2, 0.92, fres);
        vec3 rgb = mix(body, refl, k) + uSunColor * spec * 4.0;
        // Foam where the water meets the shore, broken up by the waves.
        float foamBand = 1.0 - smoothstep(0.02, 0.28, depth);
        float foamNoise = 0.5 + 0.5 * sin(p.x * 3.1 + t * 1.3 + 40.0 * g.x) * sin(p.y * 2.7 - t * 1.1);
        float foam = foamBand * smoothstep(0.45, 0.9, foamNoise + foamBand * 0.3) * (1.0 - smoothstep(30.0, 80.0, d));
        rgb = mix(rgb, vec3(0.85, 0.9, 0.9) * (uAmbient + uSunColor * sh), foam * 0.5);
        // Clarity: shallows let the bed show through; deep water is nearly opaque.
        float a = mix(mix(0.35, 0.93, smoothstep(0.0, 3.0, depth)), 1.0, max(fres, foam)) * uTint.a;
        oColor = vec4(fogOut(rgb, fog, vWorldPos, 0.6), a);
#ifdef DEBUG_REFL
        if (uReflOn > 0.5) oColor = vec4(texture(uReflTex, (uReflMat * vec4(vWorldPos, 1.0)).xy / (uReflMat * vec4(vWorldPos, 1.0)).w * 0.5 + 0.5).rgb, 1.0);
#endif
        return;
    }
    // (The terrain colour from one expression, as a select: called twice, or inside an if, Mesa's
    // llvmpipe - the PC test renderer - miscompiled the whole shader.)
    bool terr = uMaterial >= 3;
    vec4 base = terr ? vec4(terrainColor(vWorldPos, uMaterial >= 4 ? vec3(0.0, 1.0, 0.0) : normalize(vNormal), vUv, d, uMaterial == 4 ? 10.0 : (uMaterial == 5 ? 3.0 : 0.0)), 1.0)
                     : texture(uTex, vUv);
    if (uMaterial == 5) {
        // Rework stones: the ground's colour (dusty, a little paler), grained by the rock texture's
        // brightness (one projection along the dominant axis of the facet).
        vec3 an = abs(vNormal);
        highp vec2 q = an.x > an.z ? vWorldPos.zy : vWorldPos.xy;
        if (an.y > max(an.x, an.z)) q = vWorldPos.xz;
        float g = dot(texture(uTerrRock, q * 0.9).rgb, vec3(0.299, 0.587, 0.114));
        float gm = dot(textureLod(uTerrRock, vec2(0.5), 10.0).rgb, vec3(0.299, 0.587, 0.114));
        base.rgb = mix(base.rgb, vec3(dot(base.rgb, vec3(0.333))), 0.35) * clamp(0.75 + 0.6 * (g - gm) / max(gm, 0.05), 0.45, 1.5) * 1.05;
    }
    if (uMaterial == 4) {
        // Rework grass: the card's blade coordinates (x across, y up, z the tuft's tone) ride in the
        // normal; vUv is the ground's own texture coordinate under the tuft, so the blades take the
        // colour of the ground they grow from; the vertex colour is the ground's baked light there.
        vec3 bc = vNormal;
        vec4 bl = texture(uGrassTex, vec2(bc.x, 1.0 - bc.y));
        // Thin blades keep their coverage in the smaller mip levels, and the edge is sharpened to a
        // pixel (with alpha-to-coverage on the headset's MSAA: smooth, not stair-stepped).
        // (about the mip level, without log2: llvmpipe miscompiles exp2/log2 in this shader)
        highp vec2 tx = dFdx(bc.xy) * 128.0, ty = dFdy(bc.xy) * 128.0;
        float lod = clamp(sqrt(sqrt(max(max(dot(tx, tx), dot(ty, ty)), 1.0))) - 1.0, 0.0, 3.0);
        float a = bl.a * (1.0 + 0.3 * lod);
        a = clamp((a - 0.5) / max(fwidth(a), 1e-4) + 0.5, 0.0, 1.0);
        // Blades ~1.4x the ground's brightness on average (roots darker, tips lighter, each tuft its
        // own tone), and a little more saturated: grass catching the light over its earth.
        vec3 gc = base.rgb * bl.rgb * (5.2 + 2.6 * bc.z);
        float gl = dot(gc, vec3(0.299, 0.587, 0.114));
        base = vec4(max(mix(vec3(gl), gc, 1.3), 0.0), a);
    }
    base *= vColor * uTint;
    if (base.a < uAlphaRef) discard;
    if (uUnlit > 1.5) {  // emissive: effects and overlays keep their own colour
        oColor = vec4(base.rgb, base.a);
        return;
    }
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    if (uMaterial == 4) n = vec3(0.0, 1.0, 0.0);  // grass: lit like the ground
    float stoneK = uMaterial == 5 ? 1.0 : 0.0;  // stones: the facets lit by the sun as rock
    float lum = dot(base.rgb, vec3(0.299, 0.587, 0.114));
    vec3 ng = n;  // the geometric normal (shadow lookups)
    if (uLook > 0.5 && uLook < 1.5 && d < 45.0 && uMaterial < 4) {
        // Relief up close (fades out by 45 m: no shimmer far away). Terrain a little deeper.
        float k = (uMaterial == 3 ? 0.10 : 0.025) * (1.0 - smoothstep(25.0, 45.0, d));
        // Terrain: the relief of its detail noise (the ground textures are too soft to read from).
        float hgt = uMaterial == 3 ? texture(uTerrNoise, vWorldPos.xz * 0.43 + vec2(0.13, 0.57)).r * 0.6
                                     + texture(uTerrNoise, vWorldPos.xz * 1.7).g * 0.4 : lum;
        if (uMaterial == 3 && gRockW > 0.0) {  // rock: its own texture is the relief, deeper
            hgt = mix(hgt, gRockH * 2.2, gRockW);
            k *= 1.0 + 1.5 * gRockW;
        }
        n = bumpNormal(n, vWorldPos, hgt, k);
    }
    float sh = sunShadow(vWorldPos, ng);
    float ndl = max(dot(n, -uSunDir), 0.0);
    vec3 rgb;
    if (uUnlit > 0.5) {
        // Baked lighting (lightmaps, vertex colours) still darkens under a moving shadow, and the
        // moving sun brightens what faces it a little.
        // (Rock: the relief shows - ledges lit, cracks dark - around the same mean brightness.)
        rgb = base.rgb * mix(0.36, 1.0, sh) * mix(0.86 + 0.32 * ndl * sh, 0.55 + 0.9 * ndl * sh, uMaterial == 3 ? gRockW : stoneK) * uBakedLight;
    } else {
        // Sky light from above, darker bounce from below; warm sun; a faint rim of sky light.
        vec3 amb = uAmbient * mix(vec3(0.62, 0.58, 0.52), vec3(1.08, 1.1, 1.18), n.y * 0.5 + 0.5) * (uLook > 0.5 ? 1.25 : 1.0);
        highp vec3 v = normalize(uEyePos - vWorldPos);
        float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * 0.25;
        rgb = base.rgb * (amb + uSunColor * ndl * sh) + uSkyHorizon * rim * base.rgb;
    }
    if (uLook > 0.5 && uLook < 1.5 && uMaterial != 4) {
        // Sun highlights (Blinn-Phong): brighter texels read as smoother and shinier.
        highp vec3 vv = normalize(uEyePos - vWorldPos);
        vec3 hv = normalize(vv - uSunDir);
        float gloss = mix(12.0, 70.0, lum);
        float nh = clamp(dot(n, hv), 1e-4, 1.0);
        float facing = (dot(ng, -uSunDir) > 0.0 ? 1.0 : 0.0) * (uMaterial == 3 ? 0.35 : 1.0);  // ground: a soft sheen
        // nh^16 .. nh^64 by repeated squaring (cheaper than pow; and Mesa's llvmpipe, the PC test
        // renderer, miscompiled any exp2/pow here - the whole frame came out wrong).
        float p2 = nh * nh, p4 = p2 * p2, p8 = p4 * p4, p16 = p8 * p8, p32 = p16 * p16, p64 = p32 * p32;
        float pw = mix(p16, mix(p32, p64, max(lum * 2.0 - 1.0, 0.0)), min(lum * 2.0, 1.0));
        float sp = SPECEXPR;
        rgb += uSunColor * clamp(sp * (gloss + 8.0) * 0.025, 0.0, 0.8);
    }
    if (uPointColor.r + uPointColor.g + uPointColor.b > 0.0) {
        // The game's point light: smooth falloff to its radius, half-wrapped so it reaches round.
        highp vec3 lv = uPointPos.xyz - vWorldPos;
        float att = max(1.0 - dot(lv, lv) * uPointPos.w, 0.0);
        float wrap = max(dot(n, normalize(lv)) * 0.6 + 0.4, 0.0);
        rgb += (base.rgb + 0.12) * uPointColor * (att * att * wrap);  // (+0.12: shows on dark surfaces too)
    }
    if (vWorldPos.y < uSeaLevel) {
        // Under water: the bed fades into the water colour with depth.
        float dep = uSeaLevel - vWorldPos.y;
        rgb = mix(rgb, uWaterColor * 0.8, clamp(0.12 + dep * 0.2, 0.0, 0.9));
    }
    oColor = vec4(fogOut(rgb, fog, vWorldPos, 1.0), base.a);
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

GLuint Renderer::linkMain(bool multiview, Uniforms& u) {
    std::string vsrc = kVertexShader, fsrc = kFragmentShader;
    auto define = [](std::string& src, const char* line) { src.insert(src.find('\n') + 1, line); };
    if (multiview) {  // (#extension has to come before any declaration)
        define(vsrc, "#define MULTIVIEW 1\n#extension GL_OVR_multiview2 : require\n");
        define(fsrc, "#define MULTIVIEW 1\n");
    }
    if (std::getenv("GVR_DEBUG_SHADOW")) define(fsrc, "#define DEBUG_SHADOW 1\n");
    if (std::getenv("GVR_DEBUG_REFL")) define(fsrc, "#define DEBUG_REFL 1\n");
    if (const char* se = std::getenv("GVR_SPECEXPR")) define(fsrc, (std::string("#define SPECEXPR ") + se + "\n").c_str());
    else define(fsrc, "#define SPECEXPR (pw * (0.03 + 0.22 * lum * lum) * sh * facing)\n");
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc.c_str());
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc.c_str());
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOGE("Program link failed%s: %s", multiview ? " (multiview)" : "", log);
        glDeleteProgram(prog);
        return 0;
    }
    auto L = [prog](const char* n) { return glGetUniformLocation(prog, n); };
    u.uMVP_ = L("uMVP"); u.uModel_ = L("uModel"); u.uTint_ = L("uTint"); u.uSunDir_ = L("uSunDir");
    u.uSunColor_ = L("uSunColor"); u.uAmbient_ = L("uAmbient"); u.uFogColor_ = L("uFogColor"); u.uFogRange_ = L("uFogRange");
    u.uEyePos_ = L("uEyePos"); u.uUnlit_ = L("uUnlit"); u.uAlphaRef_ = L("uAlphaRef"); u.uTex_ = L("uTex");
    u.uMaterial_ = L("uMaterial"); u.uTime_ = L("uTime"); u.uSeaLevel_ = L("uSeaLevel"); u.uWaterColor_ = L("uWaterColor");
    u.uSkyHorizon_ = L("uSkyHorizon"); u.uSkyZenith_ = L("uSkyZenith"); u.uShadowMap_ = L("uShadowMap");
    u.uShadowMat_ = L("uShadowMat"); u.uShadowOn_ = L("uShadowOn"); u.uShadowTexel_ = L("uShadowTexel");
    u.uClipY_ = L("uClipY"); u.uNearCut_ = L("uNearCut"); u.uGrade_ = L("uGrade"); u.uReflTex_ = L("uReflTex"); u.uReflDepth_ = L("uReflDepth");
    u.uReflMat_ = L("uReflMat"); u.uReflEye_ = L("uReflEye"); u.uReflNear_ = L("uReflNear"); u.uReflOn_ = L("uReflOn");
    u.uWaterDepth_ = L("uWaterDepth"); u.uWaterDepthRect_ = L("uWaterDepthRect"); u.uWaterDepthOn_ = L("uWaterDepthOn");
    const char* tn[5] = {"uTerrRock", "uTerrSand", "uTerrGrass2", "uTerrRock2", "uTerrNoise"};
    for (int k = 0; k < 5; ++k) u.uTerr_[k] = L(tn[k]);
    u.uTerrDetail_ = L("uTerrDetail"); u.uSway_ = L("uSway"); u.uSkyTint_ = L("uSkyTint"); u.uNight_ = L("uNight"); u.uBakedLight_ = L("uBakedLight");
    u.uPointPos_ = L("uPointPos"); u.uPointColor_ = L("uPointColor");
    u.uSkyTex_ = L("uSkyTex"); u.uSkyOn_ = L("uSkyOn"); u.uGrassTex_ = L("uGrassTex");
    u.uViewProj_ = L("uViewProj"); u.uEyePosV_ = L("uEyePosV"); u.uLook_ = L("uLook");
    return prog;
}

bool Renderer::init() {
    program_ = linkMain(false, mono_);
    if (!program_) return false;
    U_ = &mono_;
    if (const char* lk = std::getenv("GVR_LOOK")) look_ = float(std::atoi(lk));  // 0: classic (comparisons)
    {
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        const bool mv = ext && std::strstr(ext, "GL_OVR_multiview2");
        // Both eyes in one pass where the GPU has it (GVR_MULTIVIEW=0: per eye, for comparisons).
        const char* env = std::getenv("GVR_MULTIVIEW");
        if (mv && !(env && env[0] == '0')) {
            programMV_ = linkMain(true, multi_);
            if (programMV_ && (multi_.uViewProj_ < 0 || multi_.uEyePosV_ < 0)) {
                LOGE("Multiview program lacks its view uniforms; per-eye rendering");
                glDeleteProgram(programMV_);
                programMV_ = 0;
            }
        }
        LOGI("GL_OVR_multiview2: %s; both eyes in one pass: %s", mv ? "yes" : "no", programMV_ ? "yes" : "no");
        // Anisotropic filtering where the GPU has it: ground and walls stay sharp at grazing angles.
        if (ext && std::strstr(ext, "GL_EXT_texture_filter_anisotropic")) {
            GLfloat mx = 0;
            glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &mx);
            anisotropy_ = std::min(4.0f, mx);
            LOGI("Anisotropic filtering: %.0fx", anisotropy_);
        }
    }
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
    if (programMV_) glDeleteProgram(programMV_);
    program_ = programMV_ = 0;
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

bool Renderer::updateMeshVertexRange(MeshId id, size_t first, const Vertex* vertices, size_t count) {
    if (id == 0 || id > meshes_.size()) return false;
    GpuMesh& m = meshes_[id - 1];
    if (!m.live || first + count > m.vertexCount || count == 0) return false;
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(first * sizeof(Vertex)), GLsizeiptr(count * sizeof(Vertex)), vertices);
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
        if (anisotropy_ > 1.0f) glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, anisotropy_);
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

void Renderer::beginView(const ViewParams& view, int w, int h, bool clear) { beginViews(&view, 1, w, h, clear); }

void Renderer::beginStereoView(const ViewParams views[2], int w, int h, bool clear) {
    if (!programMV_) {  // (callers check multiviewSupported(); never draw a mono program into layers)
        LOGE("beginStereoView without multiview");
        return;
    }
    beginViews(views, 2, w, h, clear);
}

void Renderer::beginViews(const ViewParams* views, int n, int w, int h, bool clear) {
    stereo_ = n == 2;
    U_ = stereo_ ? &multi_ : &mono_;
    currentView_ = views[0];
    Mat4 vp[2];
    for (int k = 0; k < n; ++k) vp[k] = views[k].projection * views[k].view;
    viewProj_ = vp[0];
    if (stereo_) currentView_.eyePosition = (views[0].eyePosition + views[1].eyePosition) * 0.5f;
    planeSets_ = n;
    for (int k = 0; k < n; ++k) {
        // Gribb-Hartmann plane extraction (column-major m[col*4+row]); far plane skipped (infinite).
        const float* m = vp[k].m;
        auto row = [&](int r) { return Vec4{m[r], m[4 + r], m[8 + r], m[12 + r]}; };
        const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        auto add = [](Vec4 a, Vec4 b, float s) { return Vec4{a.x + s * b.x, a.y + s * b.y, a.z + s * b.z, a.w + s * b.w}; };
        Vec4* pl = planes_[k];
        pl[0] = add(r3, r0, 1); pl[1] = add(r3, r0, -1);
        pl[2] = add(r3, r1, 1); pl[3] = add(r3, r1, -1);
        pl[4] = add(r3, r2, 1); pl[5] = Vec4{0, 0, 0, 1};
        for (int i = 0; i < 5; ++i) {
            const float l = std::sqrt(pl[i].x * pl[i].x + pl[i].y * pl[i].y + pl[i].z * pl[i].z);
            if (l > 0) pl[i] = {pl[i].x / l, pl[i].y / l, pl[i].z / l, pl[i].w / l};
        }
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
    glUseProgram(stereo_ ? programMV_ : program_);
    if (stereo_) {
        float m[32], e[6];
        for (int k = 0; k < 2; ++k) {
            std::memcpy(m + 16 * k, vp[k].m, sizeof(float) * 16);
            e[3 * k] = views[k].eyePosition.x; e[3 * k + 1] = views[k].eyePosition.y; e[3 * k + 2] = views[k].eyePosition.z;
        }
        glUniformMatrix4fv(U_->uViewProj_, 2, GL_FALSE, m);
        glUniform3fv(U_->uEyePosV_, 2, e);
    }
    Vec3 sd = normalize(env_.sunDirection);
    glUniform3f(U_->uSunDir_, sd.x, sd.y, sd.z);
    glUniform3f(U_->uSunColor_, env_.sunColor.x, env_.sunColor.y, env_.sunColor.z);
    glUniform3f(U_->uAmbient_, env_.ambientColor.x, env_.ambientColor.y, env_.ambientColor.z);
    glUniform3f(U_->uFogColor_, env_.fogColor.x, env_.fogColor.y, env_.fogColor.z);
    const float fogSpan = env_.fogFar > env_.fogNear ? env_.fogFar - env_.fogNear : 1e9f;
    glUniform2f(U_->uFogRange_, env_.fogNear, 1.0f / fogSpan);
    glUniform3f(U_->uEyePos_, views[0].eyePosition.x, views[0].eyePosition.y, views[0].eyePosition.z);
    glUniform1i(U_->uTex_, 0);
    glUniform1i(U_->uMaterial_, 0);
    glUniform1f(U_->uTime_, env_.time);
    glUniform1f(U_->uSeaLevel_, env_.seaLevel);
    glUniform3f(U_->uWaterColor_, env_.waterColor.x, env_.waterColor.y, env_.waterColor.z);
    glUniform3f(U_->uSkyHorizon_, env_.skyHorizon.x, env_.skyHorizon.y, env_.skyHorizon.z);
    glUniform3f(U_->uSkyZenith_, env_.skyZenith.x, env_.skyZenith.y, env_.skyZenith.z);
    glUniform1i(U_->uShadowMap_, 1);
    glActiveTexture(GL_TEXTURE1);
    const bool sh = shadowsActive_ && shadowTex_;
    glBindTexture(GL_TEXTURE_2D, sh ? shadowTex_ : dummyShadowTex_);
    glUniform1f(U_->uShadowOn_, sh ? 1.0f : 0.0f);
    glUniformMatrix4fv(U_->uShadowMat_, 1, GL_FALSE, shadowMat_.m);
    glUniform1f(U_->uShadowTexel_, shadowSize_ > 0 ? 1.0f / float(shadowSize_) : 0.0f);
    glUniform1f(U_->uClipY_, -1e9f);
    glUniform4f(U_->uNearCut_, 0, 0, 0, 0);
    glUniform4f(U_->uGrade_, env_.grade.x, env_.grade.y, env_.grade.z, env_.grade.w);
    glUniform3f(U_->uSkyTint_, env_.skyTint.x, env_.skyTint.y, env_.skyTint.z);
    glUniform1f(U_->uNight_, env_.night);
    glUniform3f(U_->uBakedLight_, env_.bakedLight.x, env_.bakedLight.y, env_.bakedLight.z);
    glUniform4f(U_->uPointPos_, env_.pointLightPos.x, env_.pointLightPos.y, env_.pointLightPos.z,
                1.0f / std::max(0.01f, env_.pointLightRadius * env_.pointLightRadius));
    glUniform3f(U_->uPointColor_, env_.pointLightColor.x, env_.pointLightColor.y, env_.pointLightColor.z);
    // Reflection (never sampled while rendering it) and water depth: units 2, 3, 4.
    const bool refl = reflActive_ && reflFbo_ && !inReflection_;
    glUniform1i(U_->uReflTex_, 2);
    glUniform1i(U_->uReflDepth_, 3);
    glUniform1i(U_->uWaterDepth_, 4);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, refl ? reflColor_ : whiteTexture_);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, refl ? reflDepth_ : whiteTexture_);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, waterDepthTex_ ? waterDepthTex_ : whiteTexture_);
    glUniform1f(U_->uReflOn_, refl ? 1.0f : 0.0f);
    glUniformMatrix4fv(U_->uReflMat_, 1, GL_FALSE, reflMat_.m);
    glUniform3f(U_->uReflEye_, reflEye_.x, reflEye_.y, reflEye_.z);
    glUniform1f(U_->uReflNear_, reflNear_);
    glUniform1f(U_->uWaterDepthOn_, waterDepthTex_ ? 1.0f : 0.0f);
    glUniform4f(U_->uWaterDepthRect_, waterRect_[0], waterRect_[1], waterRect_[2], waterRect_[3]);
    // Terrain layers: units 5..9.
    for (int k = 0; k < 5; ++k) {
        glUniform1i(U_->uTerr_[k], 5 + k);
        glActiveTexture(GLenum(GL_TEXTURE5 + k));
        GLuint t = whiteTexture_;
        if (terrainTex_[k] != 0 && terrainTex_[k] <= textures_.size() && textures_[terrainTex_[k] - 1].live) t = textures_[terrainTex_[k] - 1].tex;
        glBindTexture(GL_TEXTURE_2D, t);
    }
    glUniform1f(U_->uTerrDetail_, terrainDetail_ ? 1.0f : 0.0f);
    // The sky band for the haze: unit 10 (not while drawing the reflection - it has its own sky).
    {
        GLuint t = 0;
        if (skyTex_ != 0 && skyTex_ <= textures_.size() && textures_[skyTex_ - 1].live) t = textures_[skyTex_ - 1].tex;
        glUniform1i(U_->uSkyTex_, 10);
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_2D, t ? t : whiteTexture_);
        glUniform1f(U_->uSkyOn_, t && skyHaze_ ? 1.0f : 0.0f);
    }
    glUniform1f(U_->uLook_, look_);
    {  // the grass blades: unit 11
        GLuint t = 0;
        if (grassTex_ != 0 && grassTex_ <= textures_.size() && textures_[grassTex_ - 1].live) t = textures_[grassTex_ - 1].tex;
        glUniform1i(U_->uGrassTex_, 11);
        glActiveTexture(GL_TEXTURE11);
        glBindTexture(GL_TEXTURE_2D, t ? t : whiteTexture_);
    }
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::setNearCut(const Vec3& centre, float radius) {
    if (U_ && U_->uNearCut_ >= 0) glUniform4f(U_->uNearCut_, centre.x, centre.y, centre.z, radius);
}

void Renderer::draw(MeshId id, const Mat4& model, const DrawParams& p) {
    if (id == 0 || id > meshes_.size()) return;
    const GpuMesh& m = meshes_[id - 1];
    if (!m.live) return;
    if (!stereo_) {
        const Mat4 mvp = viewProj_ * model;
        glUniformMatrix4fv(U_->uMVP_, 1, GL_FALSE, mvp.m);
    }
    glUniformMatrix4fv(U_->uModel_, 1, GL_FALSE, model.m);
    glUniform4f(U_->uTint_, p.tint.x, p.tint.y, p.tint.z, p.tint.w);
    glUniform2f(U_->uSway_, p.sway, p.sway != 0 ? std::fmod(env_.time, 1000.0f) : 0.0f);
    // Light mode: lit, baked (unlit but shadowed), emissive (additive effects and overlays).
    const bool emissive = p.unlit && (p.blend == BlendMode::Additive || p.noFog || !p.depthTest);
    glUniform1f(U_->uUnlit_, emissive ? 2.0f : (p.unlit ? 1.0f : 0.0f));
    glUniform1i(U_->uMaterial_, int(p.material));
    GLuint tex = whiteTexture_;
    if (p.texture != 0 && p.texture <= textures_.size() && textures_[p.texture - 1].live)
        tex = textures_[p.texture - 1].tex;
    glBindTexture(GL_TEXTURE_2D, tex);
    // On the headset's MSAA every alpha-tested surface (palm fronds, shrubs, grilles) gets soft,
    // anti-aliased edges from alpha-to-coverage (rework: the shrub mounds' feathered rims were cut
    // into hard circles at 0.5); below 0.3 still dropped (stray low alpha in some textures).
    if (msaa_ && (p.alphaCoverage || p.blend == BlendMode::AlphaTest)) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);

    switch (p.blend) {
        case BlendMode::Opaque:
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
            glUniform1f(U_->uAlphaRef_, -1.0f);
            break;
        case BlendMode::AlphaTest:
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
            glUniform1f(U_->uAlphaRef_, msaa_ ? (p.alphaCoverage ? 0.02f : 0.3f) : 0.5f);
            break;
        case BlendMode::AlphaBlend:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glUniform1f(U_->uAlphaRef_, 0.004f);
            break;
        case BlendMode::Additive:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
            glUniform1f(U_->uAlphaRef_, 0.004f);
            break;
    }
    if (p.doubleSided) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);
    if (!p.depthWrite) glDepthMask(GL_FALSE);
    if (!p.depthTest) glDisable(GL_DEPTH_TEST);
    if (p.noFog) glUniform2f(U_->uFogRange_, 1e9f, 0.0f);
    glBindVertexArray(m.vao);
    glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
    if (!p.depthTest) glEnable(GL_DEPTH_TEST);
    if (p.noFog) {
        const float fogSpan = env_.fogFar > env_.fogNear ? env_.fogFar - env_.fogNear : 1e9f;
        glUniform2f(U_->uFogRange_, env_.fogNear, 1.0f / fogSpan);
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

bool Renderer::initReflection(int w, int h) {
    glGenTextures(1, &reflColor_);
    glBindTexture(GL_TEXTURE_2D, reflColor_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &reflDepth_);
    glBindTexture(GL_TEXTURE_2D, reflDepth_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &reflFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, reflFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, reflColor_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, reflDepth_, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!complete) {
        LOGE("Reflection framebuffer incomplete");
        glDeleteFramebuffers(1, &reflFbo_);
        reflFbo_ = 0;
        return false;
    }
    reflW_ = w;
    reflH_ = h;
    LOGI("Water reflections: %dx%d", w, h);
    return checkGl("initReflection");
}

bool Renderer::initScope(int size) {
    scopeTex_ = uploadTexture(size, size, nullptr, false, false);
    if (scopeTex_ == 0) return false;
    const GLuint color = textures_[scopeTex_ - 1].tex;
    glGenRenderbuffers(1, &scopeDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER, scopeDepth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
    glGenFramebuffers(1, &scopeFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, scopeFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, scopeDepth_);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (!complete) {
        LOGE("Scope framebuffer incomplete");
        glDeleteFramebuffers(1, &scopeFbo_);
        scopeFbo_ = 0;
        return false;
    }
    scopeSize_ = size;
    LOGI("Gun scope target: %dx%d", size, size);
    return checkGl("initScope");
}

bool Renderer::initScreen(int width, int height) {
    screenTex_ = uploadTexture(width, height, nullptr, false, false);
    if (screenTex_ == 0) return false;
    const GLuint color = textures_[screenTex_ - 1].tex;
    glGenFramebuffers(1, &screenFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, screenFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    // Drawn multisampled (4x), resolved into the texture by a blit.
    glGenRenderbuffers(1, &screenMsColor_);
    glBindRenderbuffer(GL_RENDERBUFFER, screenMsColor_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_SRGB8_ALPHA8, width, height);
    glGenRenderbuffers(1, &screenMsDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER, screenMsDepth_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT24, width, height);
    glGenFramebuffers(1, &screenMsFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, screenMsFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, screenMsColor_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, screenMsDepth_);
    complete = complete && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (!complete) {
        LOGE("Cinema screen framebuffer incomplete");
        glDeleteFramebuffers(1, &screenFbo_);
        glDeleteFramebuffers(1, &screenMsFbo_);
        screenFbo_ = screenMsFbo_ = 0;
        while (glGetError() != GL_NO_ERROR) {}
        return false;
    }
    screenW_ = width;
    screenH_ = height;
    LOGI("Cinema screen target: %dx%d, 4x MSAA", width, height);
    return checkGl("initScreen");
}

void Renderer::beginScreenPass(const ViewParams& view) {
    glBindFramebuffer(GL_FRAMEBUFFER, screenMsFbo_);
    beginView(view, screenW_, screenH_, false);
    glClearColor(env_.fogColor.x, env_.fogColor.y, env_.fogColor.z, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::endScreenPass() {
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, screenMsFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, screenFbo_);
    glBlitFramebuffer(0, 0, screenW_, screenH_, 0, 0, screenW_, screenH_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // (depth is not needed after the pass: tell a tiler not to store it)
    const GLenum discard[1] = {GL_DEPTH_ATTACHMENT};
    glBindFramebuffer(GL_FRAMEBUFFER, screenMsFbo_);
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, discard);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::beginScopePass(const ViewParams& view) {
    glBindFramebuffer(GL_FRAMEBUFFER, scopeFbo_);
    beginView(view, scopeSize_, scopeSize_, false);
    glClearColor(env_.fogColor.x, env_.fogColor.y, env_.fogColor.z, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::endScopePass() {
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::beginReflectionPass(const ViewParams& view, float nearZ) {
    inReflection_ = true;
    glBindFramebuffer(GL_FRAMEBUFFER, reflFbo_);
    beginView(view, reflW_, reflH_, false);
    glClearColor(env_.fogColor.x, env_.fogColor.y, env_.fogColor.z, 0.0f);  // alpha 0: nothing reflected (sky fallback)
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glFrontFace(GL_CW);  // the mirror flips the winding
    glUniform1f(U_->uClipY_, env_.seaLevel - 0.05f);
    reflMat_ = viewProj_;
    reflEye_ = view.eyePosition;
    reflNear_ = nearZ;
}

void Renderer::endReflectionPass() {
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glFrontFace(GL_CCW);
    glUniform1f(U_->uClipY_, -1e9f);
    // Only the colour is sampled later; depth too (the per-eye correction): keep both.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    inReflection_ = false;
}

void Renderer::setWaterDepthMap(int w, int h, const uint8_t* depth, float x0, float z0, float width, float depthZ) {
    if (!waterDepthTex_) glGenTextures(1, &waterDepthTex_);
    glBindTexture(GL_TEXTURE_2D, waterDepthTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, depth);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    waterRect_[0] = x0;
    waterRect_[1] = z0;
    waterRect_[2] = width > 0 ? 1.0f / width : 0.0f;
    waterRect_[3] = depthZ > 0 ? 1.0f / depthZ : 0.0f;
    checkGl("setWaterDepthMap");
}

bool Renderer::sphereVisible(const Vec3& c, float r) const {
    for (int k = 0; k < planeSets_; ++k) {  // seen by either eye
        bool in = true;
        for (int i = 0; i < 5 && in; ++i)
            if (planes_[k][i].x * c.x + planes_[k][i].y * c.y + planes_[k][i].z * c.z + planes_[k][i].w < -r) in = false;
        if (in) return true;
    }
    return false;
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
