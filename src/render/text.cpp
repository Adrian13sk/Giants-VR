#include "render/text.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "assets/image.h"
#include "core/log.h"

namespace gvr {

std::vector<std::pair<int, int>> TextRenderer::measureRow(const uint8_t* rgba, int w, int h, int row, int rowHeight) {
    std::vector<std::pair<int, int>> runs;
    const int y0 = row * rowHeight, y1 = std::min(h, y0 + rowHeight);
    auto covered = [&](int x) {
        for (int y = y0; y < y1; ++y)
            if (rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] > 40) return true;
        return false;
    };
    for (int x = 0; x < w;) {
        if (!covered(x)) { ++x; continue; }
        const int s = x;
        while (x < w && covered(x)) ++x;
        runs.push_back({s, x});
    }
    return runs;
}

bool TextRenderer::init(const AssetFS& fs, Renderer& r) {
    r_ = &r;
    std::vector<uint8_t> bytes;
    Image img;
    if (!fs.read("giantfont_eng.tga", bytes) || !decodeImage(bytes.data(), bytes.size(), img) || img.width < 64) {
        LOGW("Text: giantfont_eng.tga unavailable");
        return false;
    }
    const int rowH = img.height / 4;
    char c = '!';
    for (int row = 0; row < 4 && c <= '_'; ++row)
        for (const auto& run : measureRow(img.rgba.data(), img.width, img.height, row, rowH)) {
            if (c > '_') break;
            Glyph g;
            g.u0 = float(run.first) / float(img.width);
            g.u1 = float(run.second) / float(img.width);
            g.v0 = float(row * rowH) / float(img.height);
            g.v1 = float((row + 1) * rowH) / float(img.height);
            g.aspect = float(run.second - run.first) / float(rowH);
            glyphs_[c++] = g;
        }
    if (glyphs_.size() < 60) {
        LOGW("Text: font layout unexpected (%zu glyphs)", glyphs_.size());
        glyphs_.clear();
        return false;
    }
    texture_ = r.uploadTexture(img.width, img.height, img.rgba.data(), false, false);
    const uint8_t white[4] = {255, 255, 255, 255};
    white_ = r.uploadTexture(1, 1, white, false, false);
    MeshData q;
    const float corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    for (const auto& p : corners) {
        Vertex v{};
        v.pos[0] = p[0];
        v.pos[1] = p[1];
        v.normal[2] = 1;
        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
        q.vertices.push_back(v);
    }
    q.indices = {0, 1, 2, 0, 2, 3};
    quad_ = r.uploadMesh(q);
    LOGI("Text: game font with %zu glyphs", glyphs_.size());
    return texture_ != 0;
}

MeshId TextRenderer::mesh(const std::string& textIn, float lineHeight, float maxWidth, float* width, float* height) {
    if (!ready()) return 0;
    char key[64];
    std::snprintf(key, sizeof key, "|%.3f|%.3f", lineHeight, maxWidth);
    const std::string ck = textIn + key;
    auto it = cache_.find(ck);
    if (it == cache_.end()) {
        // Normalise: "\n" (backslash-n in the data) and real newlines break lines; tabs = space.
        std::string text;
        for (size_t i = 0; i < textIn.size(); ++i) {
            if (textIn[i] == '\\' && i + 1 < textIn.size() && textIn[i + 1] == 'n') { text += '\n'; ++i; }
            else if (textIn[i] == '\t') text += ' ';
            else text += char(std::toupper(static_cast<unsigned char>(textIn[i])));
        }
        const float space = 0.3f * lineHeight, gap = 0.06f * lineHeight;
        auto advance = [&](char ch) {
            auto g = glyphs_.find(ch);
            return g == glyphs_.end() ? space : g->second.aspect * lineHeight + gap;
        };
        // Word wrap.
        std::vector<std::string> lines(1);
        float lineW = 0;
        std::string word;
        auto flushWord = [&]() {
            float ww = 0;
            for (char ch : word) ww += advance(ch);
            if (lineW > 0 && lineW + ww > maxWidth) { lines.emplace_back(); lineW = 0; }
            lines.back() += word;
            lineW += ww;
            word.clear();
        };
        for (char ch : text) {
            if (ch == '\n') { flushWord(); lines.emplace_back(); lineW = 0; }
            else if (ch == ' ') { flushWord(); if (lineW > 0) { lines.back() += ' '; lineW += space; } }
            else word += ch;
        }
        flushWord();
        MeshData md;
        float blockW = 0;
        std::vector<float> widths;
        for (const auto& l : lines) {
            float w = 0;
            for (char ch : l) w += advance(ch);
            widths.push_back(w);
            blockW = std::max(blockW, w);
        }
        const float blockH = lineHeight * float(lines.size());
        for (size_t li = 0; li < lines.size(); ++li) {
            float x = -0.5f * widths[li];
            const float y1 = 0.5f * blockH - lineHeight * float(li), y0 = y1 - lineHeight;
            for (char ch : lines[li]) {
                auto g = glyphs_.find(ch);
                if (g == glyphs_.end()) { x += space; continue; }
                const Glyph& gl = g->second;
                const float x1 = x + gl.aspect * lineHeight;
                const uint32_t base = uint32_t(md.vertices.size());
                const float px[4][4] = {{x, y0, gl.u0, gl.v1}, {x1, y0, gl.u1, gl.v1}, {x1, y1, gl.u1, gl.v0}, {x, y1, gl.u0, gl.v0}};
                for (const auto& p : px) {
                    Vertex v{};
                    v.pos[0] = p[0];
                    v.pos[1] = p[1];
                    v.normal[2] = 1;
                    v.uv[0] = p[2];
                    v.uv[1] = p[3];
                    v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
                    md.vertices.push_back(v);
                }
                for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) md.indices.push_back(base + k);
                x = x1 + gap;
            }
        }
        if (md.vertices.empty()) return 0;
        Cached c{r_->uploadMesh(md), blockW, blockH};
        if (cache_.size() > 64) {  // keep the cache small (subtitles change per shot)
            for (auto& e : cache_) r_->destroyMesh(e.second.mesh);
            cache_.clear();
        }
        it = cache_.emplace(ck, c).first;
    }
    if (width) *width = it->second.w;
    if (height) *height = it->second.h;
    return it->second.mesh;
}

void TextRenderer::draw(MeshId m, const Mat4& xf, const Vec4& color) {
    if (!m || !r_) return;
    DrawParams dp;
    dp.texture = texture_;
    dp.blend = BlendMode::AlphaBlend;
    dp.unlit = true;
    dp.noFog = true;
    dp.depthWrite = false;
    dp.depthTest = false;
    dp.doubleSided = true;
    dp.tint = color;
    r_->draw(m, xf, dp);
}

void TextRenderer::drawPanel(const Mat4& xf, float w, float h, const Vec4& color) {
    if (!quad_ || !r_) return;
    DrawParams dp;
    dp.texture = white_;
    dp.blend = BlendMode::AlphaBlend;
    dp.unlit = true;
    dp.noFog = true;
    dp.depthWrite = false;
    dp.depthTest = false;
    dp.doubleSided = true;
    dp.tint = color;
    r_->draw(quad_, xf * Mat4::scale({w, h, 1}), dp);
}

}  // namespace gvr
