// Core text-effect primitives: color interpolation, the exponential ease-out, the outline
// stamping paths, the vertex-capture helper the animated effects build on, and the wave
// displacement pass.
//
// Render thread only. Every function here runs inside the ImGui draw pass: the drawing and
// vertex helpers through ApplyTextEffect in RendererEffects.cpp, EaseOutExpo also from the
// entrance animation in Renderer.cpp. None of them read or write game state. Positions are
// ImGui screen pixels: x grows right, y grows down.
//
// An outline is drawn as whole extra copies of the string, one AddText call per stamp, so
// its cost is the stamp count times the cost of the text itself. Each glow ring is one
// further full outline pass on top of that.
//
// Two guard conventions live side by side, and the split is deliberate. AddTextOutline4,
// DrawOutlineGlow, DrawDirectionalInnerOutline, ApplyWaveDisplacement and
// TextVertexSetup::Begin validate their arguments and return without drawing. DrawOutline
// and the DrawOutline*Internal stampers do not: their preconditions are mandatory, they are
// stated in TextEffects.hpp, and WithOutline and WithOutlineGlow inherit them.

#include "TextEffectsInternal.hpp"

namespace TextEffects
{

ImU32 LerpColorU32(ImU32 a, ImU32 b, float t)
{
    t = Saturate(t);

    // ImGui packs colors as ABGR on little-endian systems
    const int ar = (a >> IM_COL32_R_SHIFT) & 0xFF;
    const int ag = (a >> IM_COL32_G_SHIFT) & 0xFF;
    const int ab = (a >> IM_COL32_B_SHIFT) & 0xFF;
    const int aa = (a >> IM_COL32_A_SHIFT) & 0xFF;

    const int br = (b >> IM_COL32_R_SHIFT) & 0xFF;
    const int bg = (b >> IM_COL32_G_SHIFT) & 0xFF;
    const int bb = (b >> IM_COL32_B_SHIFT) & 0xFF;
    const int ba = (b >> IM_COL32_A_SHIFT) & 0xFF;

    // a + (b - a) * t per channel; the +0.5f rounds the cast to int
    const int rr = (int)(ar + (br - ar) * t + .5f);
    const int rg = (int)(ag + (bg - ag) * t + .5f);
    const int rb = (int)(ab + (bb - ab) * t + .5f);
    const int ra = (int)(aa + (ba - aa) * t + .5f);

    return IM_COL32(rr, rg, rb, ra);
}

// The t >= 1 branch returns exactly 1.0 rather than the formula's 0.99902, so a caller can
// compare the result against 1.0 to detect completion. Not constexpr: std::pow is not.
float EaseOutExpo(float t)
{
    t = Saturate(t);
    return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}

// 4 stamps on the cardinal axes, so 4 AddText calls per string.
void DrawOutline4Internal(ImDrawList* list,
                          ImFont* font,
                          float size,
                          const ImVec2& pos,
                          const char* text,
                          ImU32 outline,
                          float w)
{
    list->AddText(font, size, ImVec2(pos.x - w, pos.y), outline, text);
    list->AddText(font, size, ImVec2(pos.x + w, pos.y), outline, text);
    list->AddText(font, size, ImVec2(pos.x, pos.y - w), outline, text);
    list->AddText(font, size, ImVec2(pos.x, pos.y + w), outline, text);
}

// Stamp the text around a circle of radius w. Scaling the tap count with the
// circumference (~1 stamp / 2 px) keeps the contour an even ring at any width;
// a fixed tap count would read as a star with that many points. The half-step
// phase keeps taps off the cardinal axes, and the 8..24 clamp bounds the cost
// for wide outlines and long strings.
void DrawOutline8Internal(ImDrawList* list,
                          ImFont* font,
                          float size,
                          const ImVec2& pos,
                          const char* text,
                          ImU32 outline,
                          float w)
{
    const int n = std::clamp(static_cast<int>(std::ceil(TWO_PI * w * .5f)), 8, 24);
    const float step = TWO_PI / static_cast<float>(n);
    for (int i = 0; i < n; ++i)
    {
        const float a = (static_cast<float>(i) + .5f) * step;
        list->AddText(
            font, size, ImVec2(pos.x + std::cos(a) * w, pos.y + std::sin(a) * w), outline, text);
    }
}

// fastOutlines picks the 4-stamp variant; otherwise the ring.
void DrawOutlineInternal(ImDrawList* list,
                         ImFont* font,
                         float size,
                         const ImVec2& pos,
                         const char* text,
                         ImU32 outline,
                         float w,
                         bool fastOutlines)
{
    if (fastOutlines)
    {
        DrawOutline4Internal(list, font, size, pos, text, outline, w);
    }
    else
    {
        DrawOutline8Internal(list, font, size, pos, text, outline, w);
    }
}

void DrawOutlineGlow(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 glowColor,
                     float outlineWidth,
                     float glowScale,
                     float glowAlpha,
                     int rings,
                     bool fastOutlines)
{
    if (!list || !font || !text || !text[0] || glowAlpha <= .0f || rings <= 0)
    {
        return;
    }

    // RGB is fixed; alpha is modulated per ring
    const int gr = (glowColor >> IM_COL32_R_SHIFT) & 0xFF;
    const int gg = (glowColor >> IM_COL32_G_SHIFT) & 0xFF;
    const int gb = (glowColor >> IM_COL32_B_SHIFT) & 0xFF;
    const int ga = (glowColor >> IM_COL32_A_SHIFT) & 0xFF;

    // Draw concentric rings from outermost (faintest) to innermost (brightest)
    for (int ring = rings; ring >= 1; --ring)
    {
        // Normalized ring position: 0 = innermost, 1 = outermost
        float ringT = (float)(ring - 1) / (float)(std::max)(rings - 1, 1);

        // Near-linear spacing growth so the few rings overlap into one
        // continuous halo instead of separating into distinct shells.
        float ringScale = glowScale * (1.0f + ringT * .6f);
        float ringOffset = outlineWidth * ringScale;

        // Gaussian alpha falloff, so the rings do not step visibly
        float ringAlphaFactor = std::exp(-2.5f * ringT * ringT);
        float finalAlpha = glowAlpha * ringAlphaFactor;
        int ringAlpha = std::clamp((int)(ga * finalAlpha + .5f), 0, 255);
        if (ringAlpha <= 0)
        {
            continue;
        }

        ImU32 ringColor = IM_COL32(gr, gg, gb, ringAlpha);
        DrawOutlineInternal(list, font, size, pos, text, ringColor, ringOffset, fastOutlines);
    }
}

// Public entry point for the internal stampers. No null guard: the stampers dereference list
// at once, so the caller must have validated list, font and text.
void DrawOutline(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 outline,
                 float w,
                 bool fastOutlines)
{
    DrawOutlineInternal(list, font, size, pos, text, outline, w, fastOutlines);
}

void DrawDirectionalInnerOutline(ImDrawList* list,
                                 ImFont* font,
                                 float size,
                                 const ImVec2& pos,
                                 const char* text,
                                 ImU32 outerColor,
                                 ImU32 tierColor,
                                 float outerWidth,
                                 float innerScale,
                                 float tintFactor,
                                 float alphaFactor,
                                 float lightAngleDeg,
                                 float lightBias,
                                 bool fastOutlines)
{
    if (!list || !font || !text || !text[0] || alphaFactor <= .0f)
    {
        return;
    }

    ImU32 baseColor = LerpColorU32(outerColor, tierColor, tintFactor);
    int ba = (baseColor >> IM_COL32_A_SHIFT) & 0xFF;
    ba = std::clamp((int)(ba * alphaFactor + .5f), 0, 255);
    baseColor = (baseColor & ~(0xFFu << IM_COL32_A_SHIFT)) | ((ImU32)ba << IM_COL32_A_SHIFT);

    float innerW = outerWidth * innerScale;
    if (innerW < .5f)
    {
        // A rim thinner than half a pixel lands inside the glyph instead of beside it, so
        // it reads as a dull film over the fill rather than as an edge.
        return;
    }

    if (lightBias <= .001f || fastOutlines)
    {
        // No light bias, or fast outlines requested: uniform inner outline
        DrawOutlineInternal(list, font, size, pos, text, baseColor, innerW, fastOutlines);
        return;
    }

    // Directional: 8 offsets, each width-scaled by its angle to the light
    float lightRad = lightAngleDeg * (3.14159265f / 180.0f);
    float lx = std::cos(lightRad);
    float ly = std::sin(lightRad);

    // 8 directions: E, W, N, S, NE, SE, NW, SW
    constexpr float dirs[][2] = {{1, 0},
                                 {-1, 0},
                                 {0, -1},
                                 {0, 1},
                                 {.70710678f, -.70710678f},
                                 {.70710678f, .70710678f},
                                 {-.70710678f, -.70710678f},
                                 {-.70710678f, .70710678f}};

    // Plain stamps rather than DrawOutlineInternal: every direction needs its own width, so
    // the fixed-radius ring stampers cannot express this.
    for (const auto& dir : dirs)
    {
        float dot = dir[0] * lx + dir[1] * ly;
        float scale = 1.0f - dot * lightBias;  // opposing the light = thicker
        float w = innerW * scale;
        float ox = dir[0] * w;
        float oy = dir[1] * w;
        list->AddText(font, size, ImVec2(pos.x + ox, pos.y + oy), baseColor, text);
    }
}

void AddTextOutline4(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 col,
                     ImU32 outline,
                     float w,
                     bool fastOutlines,
                     const OutlineGlowParams* glow)
{
    if (!list || !font || !text || !text[0])
    {
        return;
    }

    // Draw order: glow rings, then the outline, then the fill text on top
    if (glow && glow->enabled)
    {
        DrawOutlineGlow(list,
                        font,
                        size,
                        pos,
                        text,
                        glow->color,
                        w,
                        glow->scale,
                        glow->alpha,
                        glow->rings,
                        fastOutlines);
    }

    DrawOutlineInternal(list, font, size, pos, text, outline, w, fastOutlines);

    list->AddText(font, size, pos, col, text);
}

// Draws the string in white and hands the caller the vertex range it produced. The bounding
// box is measured from the emitted vertices, not from CalcTextSize, so it is the extent of
// the quads this run actually added.
bool TextVertexSetup::Begin(TextVertexSetup& out,
                            ImDrawList* list,
                            ImFont* font,
                            float size,
                            const ImVec2& pos,
                            const char* text)
{
    if (!list || !font || !text || !text[0])
    {
        return false;
    }

    out.list = list;
    out.vtxStart = list->VtxBuffer.Size;
    list->AddText(font, size, pos, IM_COL32_WHITE, text);
    out.vtxEnd = list->VtxBuffer.Size;

    if (out.vtxEnd <= out.vtxStart)
    {
        return false;
    }

    out.bbMin = ImVec2(FLT_MAX, FLT_MAX);
    out.bbMax = ImVec2(-FLT_MAX, -FLT_MAX);
    for (int i = out.vtxStart; i < out.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        out.bbMin.x = (std::min)(out.bbMin.x, p.x);
        out.bbMin.y = (std::min)(out.bbMin.y, p.y);
        out.bbMax.x = (std::max)(out.bbMax.x, p.x);
        out.bbMax.y = (std::max)(out.bbMax.y, p.y);
    }
    return true;
}

// tests/test_utils.cpp re-implements this function; mirror any change there.
ImVec4 HSVtoRGB(float h, float s, float v, float a)
{
    h = Frac(h);  // wrap hue to [0, 1)

    const float c = v * s;  // Chroma
    const float x = c * (1.0f - std::fabs(Frac(h * 6.0f) * 2.0f - 1.0f));
    const float m = v - c;  // Match value

    float r = 0, g = 0, b = 0;

    // Hue sextant
    const int i = (int)std::floor(h * 6.0f);
    switch (i % 6)
    {
        case 0:
            r = c;
            g = x;
            b = 0;
            break;  // Red to Yellow
        case 1:
            r = x;
            g = c;
            b = 0;
            break;  // Yellow to Green
        case 2:
            r = 0;
            g = c;
            b = x;
            break;  // Green to Cyan
        case 3:
            r = 0;
            g = x;
            b = c;
            break;  // Cyan to Blue
        case 4:
            r = x;
            g = 0;
            b = c;
            break;  // Blue to Magenta
        case 5:
            r = c;
            g = 0;
            b = x;
            break;  // Magenta to Red
    }

    return ImVec4(r + m, g + m, b + m, a);
}

void ApplyWaveDisplacement(ImDrawList* list,
                           int vtxStart,
                           int vtxEnd,
                           float bbMinX,
                           float bbWidth,
                           float amplitude,
                           float frequency,
                           float speed,
                           float time)
{
    if (!list || vtxEnd <= vtxStart || bbWidth < 1e-3f || amplitude < .01f)
    {
        return;
    }

    constexpr float TWO_PI = 6.28318530718f;

    // Y only, and each vertex is displaced from its own X. The UVs are untouched, so a glyph
    // quad shears vertically instead of translating as a rigid quad. The caller passes the
    // bounding box of the whole pass, so one wave runs across the string, not per glyph.
    for (int i = vtxStart; i < vtxEnd; ++i)
    {
        auto& vtx = list->VtxBuffer[i];
        float nx = (vtx.pos.x - bbMinX) / bbWidth;
        float wave = std::sin(nx * frequency * TWO_PI + time * speed * TWO_PI) * amplitude;
        vtx.pos.y += wave;
    }
}

}  // namespace TextEffects
