// Text in the game's own font (giantfont_eng.tga from the user's archives): 256x128, four rows
// of 32-pixel glyphs from '!' (33) to '_' (95), proportional, upper case only. Glyph extents are
// measured at load from the alpha channel (columns without coverage separate glyphs), so no
// font data is hard-coded. Lower case is drawn as upper case, as the original font has none.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "assets/gzp.h"
#include "render/renderer.h"

namespace gvr {

class TextRenderer {
public:
    bool init(const AssetFS& fs, Renderer& r);
    bool ready() const { return texture_ != 0; }
    // Mesh for `text` (cached): centred on the origin in the XY plane, lines `lineHeight`
    // metres tall, wrapped at `maxWidth` metres; '\n' and "\t" in the source are line breaks /
    // spaces. Returns 0 if unavailable. *width / *height receive the block size.
    MeshId mesh(const std::string& text, float lineHeight, float maxWidth, float* width = nullptr,
                float* height = nullptr);
    // depth: hidden behind the world (name tags) instead of drawn over everything.
    void draw(MeshId m, const Mat4& xf, const Vec4& color = {1, 1, 1, 1}, bool depth = false);
    // A flat panel (w x h metres, centred) behind text, for readability.
    void drawPanel(const Mat4& xf, float w, float h, const Vec4& color, bool depth = false);
    // A picture on a flat panel (w x h metres, centred), drawn over everything like the panel.
    void drawImage(const Mat4& xf, float w, float h, TextureId tex, const Vec4& color = {1, 1, 1, 1}, bool depth = false);
    // Glyph count found in the font (63 expected); for tests/logs.
    size_t glyphCount() const { return glyphs_.size(); }
    // Measures glyph extents in an RGBA font image (exposed for tests).
    static std::vector<std::pair<int, int>> measureRow(const uint8_t* rgba, int w, int h, int row, int rowHeight);

private:
    struct Glyph { float u0, v0, u1, v1; float aspect; };  // aspect = width / rowHeight
    std::unordered_map<char, Glyph> glyphs_;
    struct Cached { MeshId mesh; float w, h; };
    std::unordered_map<std::string, Cached> cache_;
    Renderer* r_ = nullptr;
    TextureId texture_ = 0;
    TextureId white_ = 0;
    MeshId quad_ = 0;
};

}  // namespace gvr
