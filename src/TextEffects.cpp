#include "TextEffects.h"
#include "ParticleTextures.h"
#include "Settings.h"

#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <utility>

#define TWO_PI  6.28318530718f
#define PI      3.14159265359f
#define INV_TWO_PI (1.0f / TWO_PI)

namespace TextEffects
{
    // Get byte length of UTF-8 character at position
    static size_t Utf8CharLen(const char *s)
    {
        if (!s || !*s)
            return 0;
        unsigned char c = (unsigned char)*s;
        if (c < 0x80)
            return 1;
        if (c < 0xE0)
            return 2;
        if (c < 0xF0)
            return 3;
        if (c < 0xF8)
            return 4;
        return 1;
    }

    // Extract UTF-8 characters from string into a vector of strings
    static std::vector<std::string> Utf8ToChars(const std::string &str)
    {
        std::vector<std::string> chars;
        const char *s = str.c_str();
        while (*s)
        {
            size_t len = Utf8CharLen(s);
            chars.emplace_back(s, len);
            s += len;
        }
        return chars;
    }

    float Saturate(float x)
    {
        // Clamp to [0, 1] range
        return std::clamp(x, 0.0f, 1.0f);
    }

    float SmoothStep(float t)
    {
        t = Saturate(t);
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    ImU32 LerpColorU32(ImU32 a, ImU32 b, float t)
    {
        // Linear interpolation between two packed RGBA colors
        t = Saturate(t);  // Ensure t is in [0, 1]

        // Extract color components from first color (a)
        // ImGui uses ABGR format on little-endian systems
        const int ar = (a >> IM_COL32_R_SHIFT) & 0xFF;
        const int ag = (a >> IM_COL32_G_SHIFT) & 0xFF;
        const int ab = (a >> IM_COL32_B_SHIFT) & 0xFF;
        const int aa = (a >> IM_COL32_A_SHIFT) & 0xFF;

        // Extract components from second color (b)
        const int br = (b >> IM_COL32_R_SHIFT) & 0xFF;
        const int bg = (b >> IM_COL32_G_SHIFT) & 0xFF;
        const int bb = (b >> IM_COL32_B_SHIFT) & 0xFF;
        const int ba = (b >> IM_COL32_A_SHIFT) & 0xFF;

        // Interpolate each component: a + (b - a) * t
        // Add 0.5f for proper rounding when converting to int
        const int rr = (int)(ar + (br - ar) * t + 0.5f);
        const int rg = (int)(ag + (bg - ag) * t + 0.5f);
        const int rb = (int)(ab + (bb - ab) * t + 0.5f);
        const int ra = (int)(aa + (ba - aa) * t + 0.5f);

        // Pack back into ImU32
        return IM_COL32(rr, rg, rb, ra);
    }

    // Fast 4-directional outline (4 draw calls)
    static inline void DrawOutline4Internal(ImDrawList *list, ImFont *font, float size,
                                            const ImVec2 &pos, const char *text, ImU32 outline, float w)
    {
        // Cardinal directions only - faster, slightly less smooth
        list->AddText(font, size, ImVec2(pos.x - w, pos.y), outline, text);
        list->AddText(font, size, ImVec2(pos.x + w, pos.y), outline, text);
        list->AddText(font, size, ImVec2(pos.x, pos.y - w), outline, text);
        list->AddText(font, size, ImVec2(pos.x, pos.y + w), outline, text);
    }

    // 8-directional outline (smoother, 8 draw calls)
    static inline void DrawOutline8Internal(ImDrawList *list, ImFont *font, float size,
                                            const ImVec2 &pos, const char *text, ImU32 outline, float w)
    {
        const float d = w * .70710678118f;  // Diagonal offset (w / sqrt(2))
        // Cardinal directions
        list->AddText(font, size, ImVec2(pos.x - w, pos.y), outline, text);
        list->AddText(font, size, ImVec2(pos.x + w, pos.y), outline, text);
        list->AddText(font, size, ImVec2(pos.x, pos.y - w), outline, text);
        list->AddText(font, size, ImVec2(pos.x, pos.y + w), outline, text);
        // Diagonal directions for smoother appearance
        list->AddText(font, size, ImVec2(pos.x - d, pos.y - d), outline, text);
        list->AddText(font, size, ImVec2(pos.x + d, pos.y - d), outline, text);
        list->AddText(font, size, ImVec2(pos.x - d, pos.y + d), outline, text);
        list->AddText(font, size, ImVec2(pos.x + d, pos.y + d), outline, text);
    }

    // Draw outline using Settings::FastOutlines to pick 4-dir or 8-dir
    static inline void DrawOutlineInternal(ImDrawList *list, ImFont *font, float size,
                                           const ImVec2 &pos, const char *text, ImU32 outline, float w)
    {
        if (Settings::FastOutlines)
        {
            DrawOutline4Internal(list, font, size, pos, text, outline, w);
        }
        else
        {
            DrawOutline8Internal(list, font, size, pos, text, outline, w);
        }
    }

    void AddTextOutline4(ImDrawList *list, ImFont *font, float size,
                         const ImVec2 &pos, const char *text, ImU32 col, ImU32 outline, float w)
    {
        // Draw 8-directional outline for smoother edges
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        // Draw main text on top
        list->AddText(font, size, pos, col, text);
    }

    void AddTextHorizontalGradient(ImDrawList *list, ImFont *font, float size,
                                   const ImVec2 &pos, const char *text, ImU32 colLeft, ImU32 colRight)
    {
        if (!list || !font || !text || !text[0])
        {
            return;
        }

        // First, add the text normally to get vertices in the buffer
        const int vtxStart = list->VtxBuffer.Size;
        list->AddText(font, size, pos, IM_COL32_WHITE, text);
        const int vtxEnd = list->VtxBuffer.Size;

        if (vtxEnd <= vtxStart)
        {
            return; // No vertices added
        }

        // Find the horizontal bounds of the text
        float minX = FLT_MAX;
        float maxX = -FLT_MAX;
        for (int i = vtxStart; i < vtxEnd; ++i)
        {
            const float x = list->VtxBuffer[i].pos.x;
            minX = (std::min)(minX, x);
            maxX = (std::max)(maxX, x);
        }

        const float denom = (maxX - minX);
        if (denom < 1e-3f)
        {
            // Text too narrow, just use left color
            for (int i = vtxStart; i < vtxEnd; ++i)
            {
                list->VtxBuffer[i].col = colLeft;
            }
            return;
        }

        // Recolor each vertex based on its X position
        // Left edge gets colLeft, right edge gets colRight, interpolated in between
        for (int i = vtxStart; i < vtxEnd; ++i)
        {
            const float x = list->VtxBuffer[i].pos.x;
            const float t = (x - minX) / denom;  // Normalize to [0, 1]
            list->VtxBuffer[i].col = LerpColorU32(colLeft, colRight, t);
        }
    }

    void AddTextOutline4Gradient(ImDrawList *list, ImFont *font, float size,
                                 const ImVec2 &pos, const char *text, ImU32 colLeft, ImU32 colRight, ImU32 outline, float w)
    {
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        // Fill pass, add text once, then rewrite its vertices to a gradient
        AddTextHorizontalGradient(list, font, size, pos, text, colLeft, colRight);
    }

    // Helper struct for text vertex manipulation
    struct TextVertexSetup
    {
        ImDrawList *list;
        int vtxStart;
        int vtxEnd;
        ImVec2 bbMin;
        ImVec2 bbMax;

        float width() const { return (std::max)(bbMax.x - bbMin.x, 1e-3f); }
        float height() const { return (std::max)(bbMax.y - bbMin.y, 1e-3f); }
        float normalizedX(float x) const { return (x - bbMin.x) / width(); }
        float normalizedY(float y) const { return (y - bbMin.y) / height(); }
        ImVec2 center() const { return ImVec2((bbMin.x + bbMax.x) * 0.5f, (bbMin.y + bbMax.y) * 0.5f); }

        // Validates params, adds text, computes bounds. Returns true if vertices were added.
        static bool Begin(TextVertexSetup &out, ImDrawList *list, ImFont *font, float size,
                          const ImVec2 &pos, const char *text)
        {
            if (!list || !font || !text || !text[0])
                return false;

            out.list = list;
            out.vtxStart = list->VtxBuffer.Size;
            list->AddText(font, size, pos, IM_COL32_WHITE, text);
            out.vtxEnd = list->VtxBuffer.Size;

            if (out.vtxEnd <= out.vtxStart)
                return false;

            // Compute bounding box
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
    };

    // Calculate AABB of vertex buffer range [vtxStart, vtxEnd)
    static inline void GetVtxBounds(ImDrawList *list, int vtxStart, int vtxEnd, ImVec2 &outMin, ImVec2 &outMax)
    {
        // Initialize to extremes so first vertex will replace them
        outMin = ImVec2(FLT_MAX, FLT_MAX);
        outMax = ImVec2(-FLT_MAX, -FLT_MAX);

        // Find the axis-aligned bounding box (AABB) of all vertices
        for (int i = vtxStart; i < vtxEnd; ++i)
        {
            const ImVec2 p = list->VtxBuffer[i].pos;
            outMin.x = (std::min)(outMin.x, p.x);  // Leftmost point
            outMin.y = (std::min)(outMin.y, p.y);  // Topmost point
            outMax.x = (std::max)(outMax.x, p.x);  // Rightmost point
            outMax.y = (std::max)(outMax.y, p.y);  // Bottommost point
        }
    }

    // Get fractional part of float
    static inline float Frac(float x) { return x - std::floor(x); }

    static inline ImVec4 HSVtoRGB(float h, float s, float v, float a)
    {
        // Convert HSV (Hue, Saturation, Value) to RGB
        // h = hue [0, 1], wraps around
        // s = saturation [0, 1], 0 = grayscale, 1 = full color
        // v = value [0, 1], 0 = black, 1 = bright
        // a = alpha [0, 1]

        h = Frac(h);  // Wrap hue to [0, 1]

        // HSV to RGB conversion using standard algorithm
        const float c = v * s;  // Chroma
        const float x = c * (1.0f - std::fabs(Frac(h * 6.0f) * 2.0f - 1.0f));
        const float m = v - c;  // Match value

        float r = 0, g = 0, b = 0;

        // Determine which of the 6 hue sextants we're in
        const int i = (int)std::floor(h * 6.0f);
        switch (i % 6)
        {
        case 0:
            r = c;
            g = x;
            b = 0;
            break; // Red to Yellow
        case 1:
            r = x;
            g = c;
            b = 0;
            break; // Yellow to Green
        case 2:
            r = 0;
            g = c;
            b = x;
            break; // Green to Cyan
        case 3:
            r = 0;
            g = x;
            b = c;
            break; // Cyan to Blue
        case 4:
            r = x;
            g = 0;
            b = c;
            break; // Blue to Magenta
        case 5:
            r = c;
            g = 0;
            b = x;
            break; // Magenta to Red
        }

        // Add match value to bring up to desired brightness
        return ImVec4(r + m, g + m, b + m, a);
    }

    void AddTextRainbowWave(ImDrawList *list, ImFont *font, float size,
                            const ImVec2 &pos, const char *text,
                            float baseHue, float hueSpread, float speed, float saturation, float value, float alpha)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const float time = (float)ImGui::GetTime();

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
            const float v = s.normalizedY(list->VtxBuffer[i].pos.y);

            // Calculate hue with smooth wave motion
            const float hue = baseHue + t * hueSpread + time * speed * 0.4f;

            // Add subtle vertical brightness gradient
            float vertBrightness = 1.0f + (1.0f - v) * 0.12f;

            // Add gentle shimmer wave that travels across text
            float shimmerPhase = t * 3.0f - time * speed * 0.8f;
            float shimmer = std::sin(shimmerPhase) * 0.5f + 0.5f;
            shimmer = shimmer * shimmer * 0.08f;  // Very subtle shimmer

            // Gentle saturation variation for depth
            float satVar = saturation * (0.97f + std::sin(t * 2.0f + time * 0.15f) * 0.03f);

            // Combine brightness modifiers
            float finalValue = value * vertBrightness + shimmer;
            finalValue = std::min(finalValue, 1.0f);

            const ImVec4 rgb = HSVtoRGB(hue, satVar, finalValue, alpha);
            list->VtxBuffer[i].col = ImGui::ColorConvertFloat4ToU32(rgb);
        }
    }

    void AddTextShimmer(ImDrawList *list, ImFont *font, float size,
                        const ImVec2 &pos, const char *text,
                        ImU32 baseL, ImU32 baseR, ImU32 highlight,
                        float phase01, float bandWidth01, float strength01)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const float bandHalf = (std::max)(bandWidth01 * 0.5f, 0.01f);

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
            const float v = s.normalizedY(list->VtxBuffer[i].pos.y);
            ImU32 base = LerpColorU32(baseL, baseR, t);

            const float d = std::abs(t - phase01);

            // Primary shimmer band with soft quintic falloff
            float h = (d < bandHalf) ? 1.0f - SmoothStep(d / bandHalf) : 0.0f;

            // Add vertical gradient to shimmer
            float verticalBoost = 1.0f + (1.0f - v) * 0.3f;
            h = h * strength01 * verticalBoost;

            // Secondary soft glow halo around the band
            float glow = std::exp(-d * d * 6.0f) * 0.2f * strength01;

            // Tertiary wide ambient glow for luxury feel
            float ambient = std::exp(-d * d * 2.0f) * 0.08f * strength01;

            // Edge highlight, subtle brightness at text edges
            float edgeDist = std::min(v, 1.0f - v) * 2.0f;  // 0 at edges, 1 at center
            float edgeGlow = (1.0f - edgeDist) * 0.1f * strength01 * (1.0f - d * 0.5f);

            h = Saturate(h + glow + ambient + edgeGlow);

            list->VtxBuffer[i].col = LerpColorU32(base, highlight, h);
        }
    }

    void AddTextOutline4Shimmer(ImDrawList *list, ImFont *font, float size,
                                const ImVec2 &pos, const char *text,
                                ImU32 baseL, ImU32 baseR, ImU32 highlight, ImU32 outline, float w,
                                float phase01, float bandWidth01, float strength01)
    {
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        AddTextShimmer(list, font, size, pos, text, baseL, baseR, highlight, phase01, bandWidth01, strength01);
    }

    void AddTextRadialGradient(ImDrawList *list, ImFont *font, float size,
                               const ImVec2 &pos, const char *text, ImU32 colCenter, ImU32 colEdge,
                               float gamma, ImVec2 *overrideCenter)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const ImVec2 center = overrideCenter ? *overrideCenter : s.center();

        // Calculate maximum radius to furthest corner
        auto dist2 = [&](const ImVec2 &p)
        {
            const float dx = p.x - center.x, dy = p.y - center.y;
            return dx * dx + dy * dy;
        };
        const float r2 = (std::max)({dist2(s.bbMin), dist2(ImVec2(s.bbMax.x, s.bbMin.y)),
                                     dist2(ImVec2(s.bbMin.x, s.bbMax.y)), dist2(s.bbMax)});
        const float invR = 1.0f / std::sqrt((std::max)(r2, 1e-6f));

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const ImVec2 p = list->VtxBuffer[i].pos;
            float t = Saturate(std::sqrt((p.x - center.x) * (p.x - center.x) +
                                         (p.y - center.y) * (p.y - center.y)) *
                               invR);
            if (gamma != 1.0f)
                t = std::pow(t, gamma);
            list->VtxBuffer[i].col = LerpColorU32(colCenter, colEdge, t);
        }
    }

    void AddTextOutline4RadialGradient(ImDrawList *list, ImFont *font, float size,
                                       const ImVec2 &pos, const char *text, ImU32 colCenter, ImU32 colEdge, ImU32 outline, float w,
                                       float gamma)
    {
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        AddTextRadialGradient(list, font, size, pos, text, colCenter, colEdge, gamma);
    }

    // Extract alpha channel [0-255] from packed color
    static inline int GetA(ImU32 c) { return (c >> IM_COL32_A_SHIFT) & 0xFF; }

    // Create new color with scaled alpha (preserves RGB)
    static inline ImU32 WithAlpha(ImU32 c, float mul)
    {
        // Extract RGBA components
        const int r = (c >> IM_COL32_R_SHIFT) & 0xFF;
        const int g = (c >> IM_COL32_G_SHIFT) & 0xFF;
        const int b = (c >> IM_COL32_B_SHIFT) & 0xFF;
        const int a = (c >> IM_COL32_A_SHIFT) & 0xFF;

        // Scale alpha and clamp to valid range
        const int na = (int)std::clamp(a * mul, 0.0f, 255.0f);

        // Repack color with new alpha
        return IM_COL32(r, g, b, na);
    }

    void AddTextGradientShimmer(ImDrawList *list, ImFont *font, float size,
                                const ImVec2 &pos, const char *text,
                                ImU32 baseL, ImU32 baseR, ImU32 highlight,
                                float phase01, float bandWidth01, float strength01)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const float sigma = (std::max)(bandWidth01, 1e-3f);
        const float inv2s2 = 1.0f / (2.0f * sigma * sigma);

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
            ImU32 base = LerpColorU32(baseL, baseR, t);

            const float d = t - phase01;
            float h = Saturate(std::exp(-(d * d) * inv2s2) * strength01);

            list->VtxBuffer[i].col = LerpColorU32(base, highlight, h);
        }
    }

    void AddTextSolidShimmer(ImDrawList *list, ImFont *font, float size,
                             const ImVec2 &pos, const char *text,
                             ImU32 base, ImU32 highlight,
                             float phase01, float bandWidth01, float strength01)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const float sigma = (std::max)(bandWidth01, 1e-3f);
        const float inv2s2 = 1.0f / (2.0f * sigma * sigma);

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
            const float d = t - phase01;
            float h = Saturate(std::exp(-(d * d) * inv2s2) * strength01);
            list->VtxBuffer[i].col = LerpColorU32(base, highlight, h);
        }
    }

    void AddTextOutline4ChromaticShimmer(ImDrawList *list, ImFont *font, float size,
                                         const ImVec2 &pos, const char *text,
                                         ImU32 baseL, ImU32 baseR, ImU32 highlight,
                                         ImU32 outline, float outlineW,
                                         float phase01, float bandWidth01, float strength01,
                                         float splitPx, float ghostAlphaMul = 0.35f)
    {
        // Extract alpha from base color so ghosts fade with distance
        const float baseA = (float)GetA(baseL) / 255.0f;
        const float gMul = ghostAlphaMul;

        // Create tinted ghost colors
        // These simulate chromatic aberration
        ImU32 ghostR = IM_COL32(255, 80, 80, (int)std::clamp(255.0f * baseA * gMul, 0.0f, 255.0f));  // Red ghost
        ImU32 ghostB = IM_COL32(80, 160, 255, (int)std::clamp(255.0f * baseA * gMul, 0.0f, 255.0f)); // Blue ghost

        // Highlight for ghosts
        ImU32 hiGhost = WithAlpha(highlight, gMul);

        // Layer 1: Draw ghost layers behind main text
        // Red ghost slightly to the left
        AddTextSolidShimmer(list, font, size, ImVec2(pos.x - splitPx, pos.y), text,
                            ghostR, hiGhost, Frac(phase01 + 0.02f), bandWidth01, strength01);

        // Blue ghost slightly to the right
        AddTextSolidShimmer(list, font, size, ImVec2(pos.x + splitPx, pos.y), text,
                            ghostB, hiGhost, Frac(phase01 + 0.07f), bandWidth01, strength01);

        // Layer 2: Draw 8-directional outline on main text
        DrawOutline8Internal(list, font, size, pos, text, outline, outlineW);

        // Layer 3: Draw main text with gradient and shimmer
        AddTextGradientShimmer(list, font, size, pos, text, baseL, baseR, highlight, phase01, bandWidth01, strength01);
    }

    void TextEffects::AddTextVerticalGradient(ImDrawList *list, ImFont *font, float size,
                                              const ImVec2 &pos, const char *text, ImU32 colTop, ImU32 colBottom)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const float t = s.normalizedY(list->VtxBuffer[i].pos.y);
            list->VtxBuffer[i].col = LerpColorU32(colTop, colBottom, t);
        }
    }

    void TextEffects::AddTextOutline4VerticalGradient(ImDrawList *list, ImFont *font, float size,
                                                      const ImVec2 &pos, const char *text, ImU32 colTop, ImU32 colBottom, ImU32 outline, float w)
    {
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        AddTextVerticalGradient(list, font, size, pos, text, colTop, colBottom);
    }

    // Scale RGB channels by multiplier
    static inline ImU32 ScaleRGB(ImU32 c, float mul)
    {
        mul = std::max(0.0f, mul);  // Prevent negative colors

        // Extract channels
        const int r = (c >> IM_COL32_R_SHIFT) & 0xFF;
        const int g = (c >> IM_COL32_G_SHIFT) & 0xFF;
        const int b = (c >> IM_COL32_B_SHIFT) & 0xFF;
        const int a = (c >> IM_COL32_A_SHIFT) & 0xFF;

        // Scale RGB and clamp to valid range
        const int nr = (int)std::clamp(r * mul, 0.0f, 255.0f);
        const int ng = (int)std::clamp(g * mul, 0.0f, 255.0f);
        const int nb = (int)std::clamp(b * mul, 0.0f, 255.0f);

        // Repack with original alpha
        return IM_COL32(nr, ng, nb, a);
    }

    void TextEffects::AddTextDiagonalGradient(ImDrawList *list, ImFont *font, float size,
                                              const ImVec2 &pos, const char *text, ImU32 a, ImU32 b, ImVec2 dir)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        // Normalize direction vector
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (len < 1e-3f)
            dir = ImVec2(1, 0);
        else
        {
            dir.x /= len;
            dir.y /= len;
        }

        // Project all vertices onto direction to find extent
        float minP = FLT_MAX, maxP = -FLT_MAX;
        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const ImVec2 p = list->VtxBuffer[i].pos;
            const float proj = p.x * dir.x + p.y * dir.y;
            minP = (std::min)(minP, proj);
            maxP = (std::max)(maxP, proj);
        }

        const float denom = (std::max)(maxP - minP, 1e-3f);

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const ImVec2 p = list->VtxBuffer[i].pos;
            const float t = (p.x * dir.x + p.y * dir.y - minP) / denom;
            list->VtxBuffer[i].col = LerpColorU32(a, b, t);
        }
    }

    void TextEffects::AddTextOutline4DiagonalGradient(ImDrawList *list, ImFont *font, float size,
                                                      const ImVec2 &pos, const char *text, ImU32 a, ImU32 b, ImVec2 dir, ImU32 outline, float w)
    {
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        AddTextDiagonalGradient(list, font, size, pos, text, a, b, dir);
    }

    void TextEffects::AddTextPulseGradient(ImDrawList *list, ImFont *font, float size,
                                           const ImVec2 &pos, const char *text, ImU32 a, ImU32 b, float time, float freqHz, float amp)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const float pulse = 1.0f + amp * std::sin(time * TWO_PI * freqHz);

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
            ImU32 base = LerpColorU32(a, b, t);
            list->VtxBuffer[i].col = ScaleRGB(base, pulse);
        }
    }

    void TextEffects::AddTextOutline4PulseGradient(ImDrawList *list, ImFont *font, float size,
                                                   const ImVec2 &pos, const char *text, ImU32 a, ImU32 b, float time, float freqHz, float amp, ImU32 outline, float w)
    {
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        AddTextPulseGradient(list, font, size, pos, text, a, b, time, freqHz, amp);
    }

    void TextEffects::AddTextConicRainbow(ImDrawList *list, ImFont *font, float size,
                                          const ImVec2 &pos, const char *text, float baseHue, float speed, float saturation, float value, float alpha)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const ImVec2 c = s.center();
        const float time = (float)ImGui::GetTime();

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const ImVec2 p = list->VtxBuffer[i].pos;
            const float ang = std::atan2(p.y - c.y, p.x - c.x);
            const float u = (ang + PI) * INV_TWO_PI;
            // Slower, more gradual rotation (0.3x speed)
            const float hue = baseHue + u + time * speed * 0.3f;
            list->VtxBuffer[i].col = ImGui::ColorConvertFloat4ToU32(HSVtoRGB(hue, saturation, value, alpha));
        }
    }

    void TextEffects::AddTextOutline4RainbowWave(ImDrawList *list, ImFont *font, float size,
                                                 const ImVec2 &pos, const char *text,
                                                 float baseHue, float hueSpread, float speed, float saturation, float value,
                                                 float alpha, ImU32 outline, float w, bool useWhiteBase)
    {
        // Always draw outline first
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        // Optionally draw white base layer
        if (useWhiteBase)
        {
            ImU32 whiteBase = IM_COL32(255, 255, 255, (int)(alpha * 255.0f));
            list->AddText(font, size, pos, whiteBase, text);
            // Draw rainbow overlay with reduced alpha
            AddTextRainbowWave(list, font, size, pos, text,
                               baseHue, hueSpread, speed, saturation, value, alpha * 0.35f);
        }
        else
        {
            // Draw rainbow directly
            AddTextRainbowWave(list, font, size, pos, text,
                               baseHue, hueSpread, speed, saturation, value, alpha);
        }
    }

    void TextEffects::AddTextOutline4ConicRainbow(ImDrawList *list, ImFont *font, float size,
                                                  const ImVec2 &pos, const char *text,
                                                  float baseHue, float speed, float saturation, float value, float alpha,
                                                  ImU32 outline, float w, bool useWhiteBase)
    {
        // Always draw outline first
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        // Optionally draw white base layer
        if (useWhiteBase)
        {
            ImU32 whiteBase = IM_COL32(255, 255, 255, (int)(alpha * 255.0f));
            list->AddText(font, size, pos, whiteBase, text);
            // Draw rainbow overlay with reduced alpha
            AddTextConicRainbow(list, font, size, pos, text,
                                baseHue, speed, saturation, value, alpha * 0.35f);
        }
        else
        {
            // Draw rainbow directly
            AddTextConicRainbow(list, font, size, pos, text,
                                baseHue, speed, saturation, value, alpha);
        }
    }

    // Integer hash for pseudo-random noise [0, 1)
    static inline float Hash(float x, float y)
    {
        size_t hash = static_cast<size_t>(static_cast<int>(x));
        hash ^= hash >> 16;
        hash *= 0x85ebca6b;
        hash ^= hash >> 13;
        hash *= 0xc2b2ae35;
        hash ^= hash >> 16;
        hash ^= static_cast<size_t>(static_cast<int>(y)) * 2654435761;
        return static_cast<float>(hash & 0xFFFFFF) / 16777216.0f; // [0, 1)
    }

    // 2D value noise with quintic interpolation
    static inline float ValueNoise(float x, float y)
    {
        // Get integer and fractional parts
        float ix = std::floor(x);
        float iy = std::floor(y);
        float fx = x - ix;
        float fy = y - iy;

        // Quintic interpolation curve for smoother results
        fx = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
        fy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

        // Sample four corners and interpolate
        float a = Hash(ix, iy);
        float b = Hash(ix + 1.0f, iy);
        float c = Hash(ix, iy + 1.0f);
        float d = Hash(ix + 1.0f, iy + 1.0f);

        // Bilinear interpolation
        float ab = a + (b - a) * fx;
        float cd = c + (d - c) * fx;
        return ab + (cd - ab) * fy;
    }

    // Fractal Brownian Motion
    static inline float FBMNoise(float x, float y, int octaves, float persistence = 0.5f)
    {
        float total = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float maxValue = 0.0f;

        for (int i = 0; i < octaves; i++)
        {
            total += ValueNoise(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= 2.0f;
        }

        return total / maxValue;
    }

    void TextEffects::AddTextAurora(ImDrawList *list, ImFont *font, float size,
                                    const ImVec2 &pos, const char *text,
                                    ImU32 colA, ImU32 colB,
                                    float speed, float waves, float intensity, float sway)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const float time = (float)ImGui::GetTime() * speed;

        // Create intermediate colors for richer aurora palette
        ImU32 colMid = LerpColorU32(colA, colB, 0.5f);
        // Add subtle brightness variation
        ImU32 colBright = LerpColorU32(colA, IM_COL32(255, 255, 255, 255), 0.25f);

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const ImVec2 p = list->VtxBuffer[i].pos;
            const float nx = s.normalizedX(p.x);
            const float ny = s.normalizedY(p.y);

            // Multiple flowing wave layers for organic aurora movement
            float wave1 = std::sin(nx * waves * TWO_PI + time * 1.2f + ny * 2.0f);
            float wave2 = std::sin(nx * waves * 0.7f * TWO_PI - time * 0.8f + ny * 1.5f) * 0.6f;
            float wave3 = std::sin(nx * waves * 1.3f * TWO_PI + time * 0.5f - ny * 1.0f) * 0.4f;

            // Vertical curtain effect
            float curtain = std::sin(ny * TWO_PI * 2.0f + time * 0.7f + nx * sway * 3.0f);
            curtain = curtain * 0.5f + 0.5f;  // Normalize to [0, 1]

            // Combine waves
            float combined = (wave1 + wave2 + wave3) / 2.0f;  // Range roughly [-1, 1]
            combined = combined * 0.5f + 0.5f;                // Normalize to [0, 1]

            // Add subtle shimmer
            float shimmer = std::sin(time * 4.0f + nx * 12.0f + ny * 8.0f) * 0.5f + 0.5f;
            shimmer = shimmer * shimmer * 0.15f; // Subtle sparkle

            // Horizontal sway effect
            float swayOffset = std::sin(ny * 3.0f + time * 1.5f) * sway;
            float swayedX = nx + swayOffset;
            float swayFactor = std::sin(swayedX * TWO_PI * waves + time) * 0.5f + 0.5f;

            // Blend all factors
            float t = Saturate((combined * 0.6f + curtain * 0.25f + swayFactor * 0.15f) * intensity + shimmer);

            // Three-color gradient for rich aurora appearance
            ImU32 finalColor;
            if (t < 0.4f)
            {
                finalColor = LerpColorU32(colA, colMid, t * 2.5f);
            }
            else if (t < 0.7f)
            {
                finalColor = LerpColorU32(colMid, colB, (t - 0.4f) * 3.33f);
            }
            else
            {
                finalColor = LerpColorU32(colB, colBright, (t - 0.7f) * 3.33f);
            }

            list->VtxBuffer[i].col = finalColor;
        }
    }

    void TextEffects::AddTextOutline4Aurora(ImDrawList *list, ImFont *font, float size,
                                            const ImVec2 &pos, const char *text,
                                            ImU32 colA, ImU32 colB, ImU32 outline, float w,
                                            float speed, float waves, float intensity, float sway)
    {
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        AddTextAurora(list, font, size, pos, text, colA, colB, speed, waves, intensity, sway);
    }

    void TextEffects::AddTextSparkle(ImDrawList *list, ImFont *font, float size,
                                     const ImVec2 &pos, const char *text,
                                     ImU32 baseL, ImU32 baseR, ImU32 sparkleColor,
                                     float density, float speed, float intensity)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const float time = (float)ImGui::GetTime();

        // Create color variations for richer sparkle
        ImU32 sparkleWhite = IM_COL32(255, 255, 255, 255);
        ImU32 sparkleTint = LerpColorU32(sparkleColor, sparkleWhite, 0.3f);

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const ImVec2 p = list->VtxBuffer[i].pos;
            const float nx = s.normalizedX(p.x);
            const float ny = s.normalizedY(p.y);

            ImU32 base = LerpColorU32(baseL, baseR, nx);
            float totalSparkle = 0.0f;
            float colorShift = 0.0f;  // For varying sparkle color

            // Layer 1: Large slow-twinkling stars
            float seed1 = Hash(std::floor(p.x * 0.06f), std::floor(p.y * 0.06f));
            if (seed1 > (1.0f - density * 0.4f))
            {
                float phase1 = seed1 * TWO_PI;
                float sparkleTime1 = time * speed * (0.6f + seed1 * 0.4f);
                float sparkle1 = std::sin(sparkleTime1 + phase1);
                sparkle1 = std::max(0.0f, sparkle1);
                sparkle1 = std::pow(sparkle1, 3.0f);

                // Star burst pattern
                float gridX = Frac(p.x * 0.06f);
                float gridY = Frac(p.y * 0.06f);
                float distFromCenter = std::sqrt((gridX - 0.5f) * (gridX - 0.5f) + (gridY - 0.5f) * (gridY - 0.5f));
                float starPattern = std::max(0.0f, 1.0f - distFromCenter * 3.0f);

                totalSparkle += sparkle1 * starPattern * 0.9f;
                colorShift += sparkle1 * 0.3f;
            }

            // Layer 2: Medium fast-twinkling sparkles
            float seed2 = Hash(std::floor(p.x * 0.12f) + 50.0f, std::floor(p.y * 0.12f) + 50.0f);
            if (seed2 > (1.0f - density * 0.7f))
            {
                float phase2 = seed2 * TWO_PI;
                float sparkleTime2 = time * speed * 1.8f * (0.8f + seed2 * 0.4f);
                float sparkle2 = std::sin(sparkleTime2 + phase2);
                sparkle2 = std::max(0.0f, sparkle2);
                sparkle2 = std::pow(sparkle2, 5.0f);
                totalSparkle += sparkle2 * 0.6f;
            }

            // Layer 3: Fine shimmer dust
            float seed3 = Hash(std::floor(p.x * 0.2f) + 100.0f, std::floor(p.y * 0.2f) + 100.0f);
            if (seed3 > (1.0f - density * 0.9f))
            {
                float phase3 = seed3 * TWO_PI;
                float sparkle3 = std::sin(time * speed * 2.5f + phase3);
                sparkle3 = std::max(0.0f, sparkle3);
                sparkle3 = std::pow(sparkle3, 8.0f);
                totalSparkle += sparkle3 * 0.35f;
            }

            // Layer 4: Rare brilliant flares
            float seed4 = Hash(std::floor(p.x * 0.04f) + 200.0f, std::floor(p.y * 0.04f) + 200.0f);
            if (seed4 > 0.93f)
            {
                float phase4 = seed4 * TWO_PI;
                float flare = std::sin(time * speed * 0.4f + phase4);
                flare = std::max(0.0f, flare);
                flare = std::pow(flare, 2.0f);
                totalSparkle += flare * 1.5f;
                colorShift += flare * 0.6f;  // Flares shift toward white
            }

            totalSparkle = Saturate(totalSparkle * intensity);
            colorShift = Saturate(colorShift);

            // Blend sparkle color with white based on intensity for brighter sparkles
            ImU32 finalSparkle = LerpColorU32(sparkleColor, sparkleTint, colorShift);
            list->VtxBuffer[i].col = LerpColorU32(base, finalSparkle, totalSparkle);
        }
    }

    void TextEffects::AddTextOutline4Sparkle(ImDrawList *list, ImFont *font, float size,
                                             const ImVec2 &pos, const char *text,
                                             ImU32 baseL, ImU32 baseR, ImU32 sparkleColor, ImU32 outline, float w,
                                             float density, float speed, float intensity)
    {
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        AddTextSparkle(list, font, size, pos, text, baseL, baseR, sparkleColor, density, speed, intensity);
    }

    void TextEffects::AddTextPlasma(ImDrawList *list, ImFont *font, float size,
                                    const ImVec2 &pos, const char *text,
                                    ImU32 colA, ImU32 colB,
                                    float freq1, float freq2, float speed)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const float time = (float)ImGui::GetTime() * speed;
        ImU32 colMid = LerpColorU32(colA, colB, 0.5f);

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const ImVec2 p = list->VtxBuffer[i].pos;
            const float nx = s.normalizedX(p.x);
            const float ny = s.normalizedY(p.y);

            // Enhanced plasma with more organic patterns
            float plasma = 0.0f;

            // Primary waves with varied phases
            plasma += std::sin(nx * freq1 * TWO_PI + time);
            plasma += std::sin(ny * freq2 * TWO_PI + time * 0.7f);

            // Diagonal waves
            plasma += std::sin((nx + ny) * (freq1 + freq2) * 0.5f * TWO_PI + time * 1.3f);
            plasma += std::sin((nx - ny) * freq1 * TWO_PI + time * 0.9f) * 0.5f;

            // Radial waves from offset centers for more organic look
            float cx1 = nx - 0.3f - std::sin(time * 0.3f) * 0.2f;
            float cy1 = ny - 0.5f - std::cos(time * 0.4f) * 0.15f;
            float dist1 = std::sqrt(cx1 * cx1 + cy1 * cy1);
            plasma += std::sin(dist1 * freq1 * TWO_PI * 2.0f - time * 1.2f);

            float cx2 = nx - 0.7f + std::cos(time * 0.35f) * 0.15f;
            float cy2 = ny - 0.5f + std::sin(time * 0.45f) * 0.2f;
            float dist2 = std::sqrt(cx2 * cx2 + cy2 * cy2);
            plasma += std::sin(dist2 * freq2 * TWO_PI * 1.5f + time * 0.8f) * 0.7f;

            // Normalize to [0, 1] with smoother transition
            plasma = (plasma + 5.2f) / 10.4f;
            plasma = SmoothStep(plasma); // Use quintic smoothing

            // Three-color gradient for richer appearance
            ImU32 finalColor;
            if (plasma < 0.5f)
            {
                finalColor = LerpColorU32(colA, colMid, plasma * 2.0f);
            }
            else
            {
                finalColor = LerpColorU32(colMid, colB, (plasma - 0.5f) * 2.0f);
            }

            list->VtxBuffer[i].col = finalColor;
        }
    }

    void TextEffects::AddTextOutline4Plasma(ImDrawList *list, ImFont *font, float size,
                                            const ImVec2 &pos, const char *text,
                                            ImU32 colA, ImU32 colB, ImU32 outline, float w,
                                            float freq1, float freq2, float speed)
    {
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        AddTextPlasma(list, font, size, pos, text, colA, colB, freq1, freq2, speed);
    }

    void TextEffects::AddTextScanline(ImDrawList *list, ImFont *font, float size,
                                      const ImVec2 &pos, const char *text,
                                      ImU32 baseL, ImU32 baseR, ImU32 scanColor,
                                      float speed, float scanWidth, float intensity)
    {
        TextVertexSetup s;
        if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
            return;

        const float time = (float)ImGui::GetTime();
        float phase1 = std::sin(time * speed * PI) * 0.5f + 0.5f;
        float phase2 = std::sin(time * speed * PI + 2.0f) * 0.5f + 0.5f;

        const float bandWidth = (std::max)(scanWidth, 0.05f);
        const float bandHalf = bandWidth * 0.5f;

        for (int i = s.vtxStart; i < s.vtxEnd; ++i)
        {
            const ImVec2 p = list->VtxBuffer[i].pos;
            const float nx = s.normalizedX(p.x);
            const float ny = s.normalizedY(p.y);

            // Base gradient color
            ImU32 base = LerpColorU32(baseL, baseR, nx);

            // Primary scanline with smooth quintic falloff
            float d1 = std::abs(ny - phase1);
            float scan1 = 0.0f;
            if (d1 < bandHalf)
            {
                scan1 = 1.0f - SmoothStep(d1 / bandHalf);
            }

            // Secondary scanline
            float d2 = std::abs(ny - phase2);
            float scan2 = 0.0f;
            if (d2 < bandHalf * 0.7f)
            {
                scan2 = (1.0f - SmoothStep(d2 / (bandHalf * 0.7f))) * 0.4f;
            }

            // Subtle horizontal scan lines
            float crtLines = std::sin(ny * s.height() * 0.5f) * 0.5f + 0.5f;
            crtLines = crtLines * 0.08f;  // Very subtle

            // Combine effects
            float totalScan = Saturate((scan1 + scan2) * intensity + crtLines);

            // Add slight glow around the main scanline
            float glow = std::exp(-d1 * d1 * 20.0f) * 0.15f * intensity;
            totalScan = Saturate(totalScan + glow);

            list->VtxBuffer[i].col = LerpColorU32(base, scanColor, totalScan);
        }
    }

    void TextEffects::AddTextOutline4Scanline(ImDrawList *list, ImFont *font, float size,
                                              const ImVec2 &pos, const char *text,
                                              ImU32 baseL, ImU32 baseR, ImU32 scanColor, ImU32 outline, float w,
                                              float speed, float width, float intensity)
    {
        DrawOutlineInternal(list, font, size, pos, text, outline, w);

        AddTextScanline(list, font, size, pos, text, baseL, baseR, scanColor, speed, width, intensity);
    }

    void TextEffects::AddTextGlow(ImDrawList *list, ImFont *font, float size,
                                  const ImVec2 &pos, const char *text, ImU32 glowColor,
                                  float radius, float intensity, int samples)
    {
        if (!list || !font || !text || !text[0] || radius <= 0.0f || intensity <= 0.01f)
            return;

        const int baseAlpha = (glowColor >> IM_COL32_A_SHIFT) & 0xFF;
        if (baseAlpha < 5)
            return;

        const int r = (glowColor >> IM_COL32_R_SHIFT) & 0xFF;
        const int g = (glowColor >> IM_COL32_G_SHIFT) & 0xFF;
        const int b = (glowColor >> IM_COL32_B_SHIFT) & 0xFF;

        // Multi-layer soft bloom for premium glow effect
        // Layer 1: Wide soft outer glow (largest, dimmest)
        // Layer 2: Medium glow
        // Layer 3: Tight inner glow (smallest, brightest)

        struct GlowLayer
        {
            float radiusMul;
            float alphaMul;
        };

        const GlowLayer layers[] = {
            {1.5f, 0.15f},  // Outer - wide and soft
            {1.0f, 0.25f},  // Middle
            {0.6f, 0.35f},  // Inner - tight and bright
        };

        const int numLayers = (samples > 8) ? 3 : (samples > 4) ? 2
                                                                : 1;

        for (int layer = 0; layer < numLayers; ++layer)
        {
            float layerRadius = radius * layers[layer].radiusMul;
            int layerAlpha = (int)(baseAlpha * intensity * layers[layer].alphaMul);
            layerAlpha = std::clamp(layerAlpha, 0, 255);
            if (layerAlpha < 3)
                continue;

            ImU32 col = IM_COL32(r, g, b, layerAlpha);

            // 8-directional samples for smooth circular glow
            const float offsets[8][2] = {
                {layerRadius, 0.0f},
                {-layerRadius, 0.0f},
                {0.0f, layerRadius},
                {0.0f, -layerRadius},
                {layerRadius * 0.707f, layerRadius * 0.707f},
                {-layerRadius * 0.707f, layerRadius * 0.707f},
                {layerRadius * 0.707f, -layerRadius * 0.707f},
                {-layerRadius * 0.707f, -layerRadius * 0.707f},
            };

            int numOffsets = (samples > 4) ? 8 : 4;
            for (int i = 0; i < numOffsets; ++i)
            {
                list->AddText(font, size,
                              ImVec2(pos.x + offsets[i][0], pos.y + offsets[i][1]),
                              col, text);
            }
        }
    }

    void DrawSideOrnaments(ImDrawList *list, const ImVec2 &center,
                           float textWidth, float textHeight, ImU32 color, float alpha,
                           float scale, float spacing, bool animated, float time,
                           float outlineWidth, bool enableGlow, float glowRadius, float glowIntensity, int glowSamples,
                           const std::string &leftOrnaments, const std::string &rightOrnaments,
                           float ornamentScale, bool isSpecialTitle)
    {
        if (!list || alpha <= 0.01f)
            return;
        if (leftOrnaments.empty() && rightOrnaments.empty())
            return;

        // Check if ornament font is configured
        if (Settings::OrnamentFontPath.empty())
        {
            return;
        }

        // Get ornament font (index 3)
        auto &io = ImGui::GetIO();
        if (io.Fonts->Fonts.Size < 4)
            return;
        ImFont *ornamentFont = io.Fonts->Fonts[3];
        if (!ornamentFont)
            return;

        // Extract color components
        int r = (color >> 0) & 0xFF;
        int g = (color >> 8) & 0xFF;
        int b = (color >> 16) & 0xFF;

        // Subtle animation
        float pulse = animated ? (0.92f + 0.08f * std::sin(time * 1.5f)) : 1.0f;

        // Alpha with pulse
        int baseAlpha = std::clamp((int)(alpha * pulse * 255.0f), 0, 255);
        int outlineAlpha = std::clamp((int)(alpha * 255.0f), 0, 255);

        ImU32 col = IM_COL32(r, g, b, baseAlpha);
        ImU32 colOutline = IM_COL32(0, 0, 0, outlineAlpha);

        // Calculate ornament size
        float sizeMultiplier = isSpecialTitle ? ornamentScale * 1.3f : ornamentScale;
        float ornamentSize = Settings::OrnamentFontSize * scale * sizeMultiplier;

        // Extra padding between ornaments and text
        float extraPadding = ornamentSize * 0.15f;
        float totalSpacing = spacing + extraPadding;

        // Outline offsets
        const float d = outlineWidth * 0.707f;
        const float outlineOffsets[8][2] = {
            {-outlineWidth, 0}, {outlineWidth, 0}, {0, -outlineWidth}, {0, outlineWidth}, {-d, -d}, {d, -d}, {-d, d}, {d, d}};

        // Helper lambda to draw a single ornament character (UTF-8 string) with glow and outline
        auto drawOrnamentChar = [&](const ImVec2 &pos, const std::string &ch)
        {
            const char *charStr = ch.c_str();

            // Glow pass
            if (enableGlow && glowRadius > 0.0f && glowIntensity > 0.0f)
            {
                int glowAlpha = std::clamp((int)(alpha * glowIntensity * 255.0f), 0, 255);
                const float layers[] = {1.5f, 1.0f, 0.6f};
                const float alphaMulti[] = {0.15f, 0.25f, 0.35f};

                for (int layer = 0; layer < 3; ++layer)
                {
                    float layerRadius = glowRadius * layers[layer];
                    int layerAlpha = std::clamp((int)(glowAlpha * alphaMulti[layer]), 0, 255);
                    if (layerAlpha < 3)
                        continue;
                    ImU32 layerCol = IM_COL32(r, g, b, layerAlpha);

                    const float glowOffsets[8][2] = {
                        {layerRadius, 0.0f},
                        {-layerRadius, 0.0f},
                        {0.0f, layerRadius},
                        {0.0f, -layerRadius},
                        {layerRadius * 0.707f, layerRadius * 0.707f},
                        {-layerRadius * 0.707f, layerRadius * 0.707f},
                        {layerRadius * 0.707f, -layerRadius * 0.707f},
                        {-layerRadius * 0.707f, -layerRadius * 0.707f},
                    };
                    for (int i = 0; i < 8; ++i)
                    {
                        list->AddText(ornamentFont, ornamentSize,
                                      ImVec2(pos.x + glowOffsets[i][0], pos.y + glowOffsets[i][1]),
                                      layerCol, charStr);
                    }
                }
            }

            // Outline pass
            for (int i = 0; i < 8; ++i)
            {
                list->AddText(ornamentFont, ornamentSize,
                              ImVec2(pos.x + outlineOffsets[i][0], pos.y + outlineOffsets[i][1]),
                              colOutline, charStr);
            }

            // Main character
            list->AddText(ornamentFont, ornamentSize, pos, col, charStr);
        };

        if (!leftOrnaments.empty())
        {
            auto leftChars = Utf8ToChars(leftOrnaments);
            float cursorX = center.x - textWidth * 0.5f - totalSpacing;
            for (int i = static_cast<int>(leftChars.size()) - 1; i >= 0; --i)
            {
                const std::string &ch = leftChars[i];
                ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, 0.0f, ch.c_str());
                cursorX -= charSize.x;
                ImVec2 charPos(cursorX, center.y - charSize.y * 0.5f);
                drawOrnamentChar(charPos, ch);
            }
        }

        if (!rightOrnaments.empty())
        {
            auto rightChars = Utf8ToChars(rightOrnaments);
            float cursorX = center.x + textWidth * 0.5f + totalSpacing;
            for (size_t i = 0; i < rightChars.size(); ++i)
            {
                const std::string &ch = rightChars[i];
                ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, 0.0f, ch.c_str());
                ImVec2 charPos(cursorX, center.y - charSize.y * 0.5f);
                drawOrnamentChar(charPos, ch);
                cursorX += charSize.x;
            }
        }
    }

    // Draw 4-pointed star with glow
    static void DrawStar4(ImDrawList *list, const ImVec2 &pos, float size, ImU32 color, ImU32 glowColor, float rotation = 0.0f)
    {
        const float innerRatio = 0.35f; // Inner radius as fraction of outer
        const float outerR = size;
        const float innerR = size * innerRatio;

        // 4 points = 8 vertices alternating outer/inner
        ImVec2 points[8];
        for (int i = 0; i < 8; ++i)
        {
            float angle = rotation + (float)i * 0.785398f; // 45 degrees = pi/4
            float radius = (i % 2 == 0) ? outerR : innerR;
            points[i] = ImVec2(pos.x + std::cos(angle) * radius, pos.y + std::sin(angle) * radius);
        }

        // Draw soft glow behind
        list->AddCircleFilled(pos, size * 1.8f, glowColor, 16);

        // Draw star shape
        list->AddConvexPolyFilled(points, 8, color);
    }

    // Draw 6-pointed star with glow
    static void DrawStar6(ImDrawList *list, const ImVec2 &pos, float size, ImU32 color, ImU32 glowColor, float rotation = 0.0f)
    {
        const float innerRatio = 0.45f;
        const float outerR = size;
        const float innerR = size * innerRatio;

        // 6 points = 12 vertices
        ImVec2 points[12];
        for (int i = 0; i < 12; ++i)
        {
            float angle = rotation + (float)i * 0.5235988f; // 30 degrees = pi/6
            float radius = (i % 2 == 0) ? outerR : innerR;
            points[i] = ImVec2(pos.x + std::cos(angle) * radius, pos.y + std::sin(angle) * radius);
        }

        // Draw soft glow layers
        list->AddCircleFilled(pos, size * 2.2f, glowColor, 16);
        list->AddCircleFilled(pos, size * 1.5f, glowColor, 16);

        // Draw star shape
        list->AddConvexPolyFilled(points, 12, color);
    }

    // Draw soft glowing orb with gradient layers
    static void DrawSoftOrb(ImDrawList *list, const ImVec2 &pos, float size, int r, int g, int b, int baseAlpha)
    {
        // Multiple layers for smooth gradient falloff
        const int layers = 5;
        for (int i = layers - 1; i >= 0; --i)
        {
            float t = (float)i / (float)(layers - 1);
            float radius = size * (0.4f + 0.6f * t);
            int layerAlpha = (int)(baseAlpha * (1.0f - t * 0.7f) * (1.0f - t * 0.3f));
            layerAlpha = std::clamp(layerAlpha, 0, 255);
            list->AddCircleFilled(pos, radius, IM_COL32(r, g, b, layerAlpha), 16);
        }
        // Bright core
        list->AddCircleFilled(pos, size * 0.25f, IM_COL32(255, 255, 255, baseAlpha / 2), 12);
    }

    // Draw ethereal wisp with flowing trail
    static void DrawWisp(ImDrawList *list, const ImVec2 &pos, float size, float angle, int r, int g, int b, int baseAlpha, float trailLength)
    {
        // Trail direction
        float trailAngle = angle + PI;
        float dx = std::cos(trailAngle);
        float dy = std::sin(trailAngle);

        // Draw trail segments with decreasing alpha
        const int trailSegments = 6;
        for (int i = trailSegments - 1; i >= 0; --i)
        {
            float t = (float)i / (float)trailSegments;
            float segX = pos.x + dx * size * trailLength * t;
            float segY = pos.y + dy * size * trailLength * t;
            float segSize = size * (1.0f - t * 0.6f);
            int segAlpha = (int)(baseAlpha * (1.0f - t * 0.8f));
            segAlpha = std::clamp(segAlpha, 0, 255);
            list->AddCircleFilled(ImVec2(segX, segY), segSize, IM_COL32(r, g, b, segAlpha), 12);
        }

        // Main wisp body with glow
        list->AddCircleFilled(pos, size * 1.6f, IM_COL32(r, g, b, baseAlpha / 4), 14);
        list->AddCircleFilled(pos, size, IM_COL32(r, g, b, baseAlpha), 12);
        list->AddCircleFilled(pos, size * 0.4f, IM_COL32(255, 255, 255, baseAlpha / 2), 8);
    }

    // Draw magical rune symbol with glow
    static void DrawRune(ImDrawList *list, const ImVec2 &pos, float size, int r, int g, int b, int baseAlpha, int runeType)
    {
        ImU32 glowCol = IM_COL32(r, g, b, baseAlpha / 3);
        ImU32 mainCol = IM_COL32(r, g, b, baseAlpha);
        ImU32 brightCol = IM_COL32(std::min(255, r + 50), std::min(255, g + 50), std::min(255, b + 50), baseAlpha);
        float thickness = size * 0.15f;

        // Draw glow behind
        list->AddCircleFilled(pos, size * 1.8f, IM_COL32(r, g, b, baseAlpha / 5), 16);

        // Different rune patterns based on type
        switch (runeType % 4)
        {
        case 0:
        {
            // Diamond with cross
            float s = size;
            // Outer diamond
            list->AddQuad(ImVec2(pos.x, pos.y - s), ImVec2(pos.x + s * 0.7f, pos.y),
                          ImVec2(pos.x, pos.y + s), ImVec2(pos.x - s * 0.7f, pos.y), mainCol, thickness);
            // Inner cross
            list->AddLine(ImVec2(pos.x, pos.y - s * 0.5f), ImVec2(pos.x, pos.y + s * 0.5f), brightCol, thickness * 0.8f);
            list->AddLine(ImVec2(pos.x - s * 0.35f, pos.y), ImVec2(pos.x + s * 0.35f, pos.y), brightCol, thickness * 0.8f);
            // Corner dots
            list->AddCircleFilled(ImVec2(pos.x, pos.y - s), size * 0.12f, brightCol, 8);
            list->AddCircleFilled(ImVec2(pos.x, pos.y + s), size * 0.12f, brightCol, 8);
            break;
        }
        case 1:
        {
            // Triangle with eye
            float s = size * 0.9f;
            ImVec2 p1(pos.x, pos.y - s);
            ImVec2 p2(pos.x - s * 0.866f, pos.y + s * 0.5f);
            ImVec2 p3(pos.x + s * 0.866f, pos.y + s * 0.5f);
            list->AddTriangle(p1, p2, p3, mainCol, thickness);
            // Inner circle (eye)
            list->AddCircle(pos, size * 0.35f, brightCol, 12, thickness * 0.7f);
            list->AddCircleFilled(pos, size * 0.15f, brightCol, 8);
            break;
        }
        case 2:
        {
            // Asterisk/star burst
            float s = size;
            for (int i = 0; i < 6; ++i)
            {
                float angle = (float)i * 0.5236f; // 30 degrees
                ImVec2 outer(pos.x + std::cos(angle) * s, pos.y + std::sin(angle) * s);
                list->AddLine(pos, outer, mainCol, thickness);
            }
            list->AddCircleFilled(pos, size * 0.2f, brightCol, 10);
            break;
        }
        case 3:
        {
            // Concentric circles with dots
            list->AddCircle(pos, size * 0.9f, mainCol, 14, thickness * 0.7f);
            list->AddCircle(pos, size * 0.5f, mainCol, 12, thickness * 0.6f);
            list->AddCircleFilled(pos, size * 0.2f, brightCol, 8);
            // Cardinal dots
            for (int i = 0; i < 4; ++i)
            {
                float angle = (float)i * 1.5708f; // 90 degrees
                ImVec2 dotPos(pos.x + std::cos(angle) * size * 0.7f, pos.y + std::sin(angle) * size * 0.7f);
                list->AddCircleFilled(dotPos, size * 0.1f, brightCol, 6);
            }
            break;
        }
        }
    }

    // Draw a spark with motion trail
    static void DrawSpark(ImDrawList *list, const ImVec2 &pos, float size, float angle, int r, int g, int b, int baseAlpha, float life)
    {
        // Spark gets more orange/yellow as it's "hotter"
        float heat = 1.0f - life;
        int sr = std::min(255, r + (int)(100 * heat));
        int sg = std::min(255, g + (int)(50 * heat));
        int sb = std::max(0, b - (int)(30 * heat));

        // Trail behind spark
        float trailAngle = angle + PI;
        const int trailSegs = 4;
        for (int i = trailSegs - 1; i >= 0; --i)
        {
            float t = (float)(i + 1) / (float)(trailSegs + 1);
            float trailX = pos.x + std::cos(trailAngle) * size * 3.0f * t;
            float trailY = pos.y + std::sin(trailAngle) * size * 3.0f * t;
            float segSize = size * (1.0f - t * 0.7f);
            int segAlpha = (int)(baseAlpha * (1.0f - t) * 0.6f);
            list->AddCircleFilled(ImVec2(trailX, trailY), segSize, IM_COL32(sr, sg, sb, segAlpha), 8);
        }

        // Main spark with glow
        list->AddCircleFilled(pos, size * 2.0f, IM_COL32(sr, sg, sb, baseAlpha / 4), 12);
        list->AddCircleFilled(pos, size, IM_COL32(sr, sg, sb, baseAlpha), 10);
        list->AddCircleFilled(pos, size * 0.4f, IM_COL32(255, 255, 220, baseAlpha), 8);
    }

    void DrawParticleAura(ImDrawList *list, const ImVec2 &center,
                          float radiusX, float radiusY, ImU32 color, float alpha,
                          Settings::ParticleStyle style, int particleCount, float particleSize,
                          float speed, float time, int styleIndex, int enabledStyleCount)
    {
        if (!list || alpha <= 0.05f || particleCount <= 0)
            return;

        // Check if we should use textured particles from folders
        int texStyleId = static_cast<int>(style);
        bool useTextures = Settings::UseParticleTextures && ParticleTextures::IsInitialized();
        int texCount = useTextures ? ParticleTextures::GetTextureCount(texStyleId) : 0;
        bool hasTextures = (texCount > 0);

        // Moderate alpha for visible but not overwhelming particles
        const float globalAlphaMult = 0.75f;
        alpha *= globalAlphaMult;

        // Reduce alpha when multiple styles overlap to prevent brightness pileup
        if (enabledStyleCount > 1)
            alpha /= std::sqrt((float)enabledStyleCount);

        int baseR = (color >> 0) & 0xFF;
        int baseG = (color >> 8) & 0xFF;
        int baseB = (color >> 16) & 0xFF;

        float timeScaled = time * speed;

        for (int i = 0; i < particleCount; ++i)
        {
            float phase = (float)i / (float)particleCount * TWO_PI + styleIndex * 2.399963f;
            float golden = (float)(i + styleIndex * 97) * 2.399963f;  // Golden angle, offset per style

            // Per-particle alpha variation (0.4 to 1.0 range)
            float alphaVariation = 0.4f + 0.6f * (0.5f + 0.5f * std::sin(golden * 1.7f + timeScaled * 0.3f));

            // Per-particle color variation - more vibrant hue shift and boosted saturation
            float hueShift = std::sin(golden * 2.3f + timeScaled * 0.25f) * 0.4f;  // +/- 40% hue shift for rainbow variety
            float satMod = 1.1f + 0.2f * std::sin(golden * 1.5f);                  // 110-130% saturation boost

            // Apply color variation
            int r = baseR, g = baseG, b = baseB;

            // Shift hue by rotating RGB values
            float hueAngle = hueShift * TWO_PI;
            float cosH = std::cos(hueAngle);
            float sinH = std::sin(hueAngle);

            // Simplified hue rotation matrix
            float newR = baseR * (0.213f + 0.787f * cosH - 0.213f * sinH) +
                         baseG * (0.213f - 0.213f * cosH + 0.143f * sinH) +
                         baseB * (0.213f - 0.213f * cosH - 0.928f * sinH);
            float newG = baseR * (0.715f - 0.715f * cosH - 0.715f * sinH) +
                         baseG * (0.715f + 0.285f * cosH + 0.140f * sinH) +
                         baseB * (0.715f - 0.715f * cosH + 0.283f * sinH);
            float newB = baseR * (0.072f - 0.072f * cosH + 0.928f * sinH) +
                         baseG * (0.072f - 0.072f * cosH - 0.283f * sinH) +
                         baseB * (0.072f + 0.928f * cosH + 0.072f * sinH);

            // Apply saturation modification
            float gray = 0.299f * newR + 0.587f * newG + 0.114f * newB;
            r = std::clamp((int)(gray + (newR - gray) * satMod), 0, 255);
            g = std::clamp((int)(gray + (newG - gray) * satMod), 0, 255);
            b = std::clamp((int)(gray + (newB - gray) * satMod), 0, 255);

            float x, y, finalAlpha, finalSize;

            switch (style)
            {
                case Settings::ParticleStyle::Stars:
                default:
                {
                    // Twinkling blue stars - dark to bright blue colors
                    float orbit = phase + timeScaled * 0.5f;
                    float radiusMod = 0.6f + 0.4f * std::sin(golden);
                    x = center.x + std::cos(orbit) * radiusX * radiusMod;
                    y = center.y + std::sin(orbit) * radiusY * radiusMod;

                    // Multi-frequency twinkle for more natural look
                    float twinkle1 = std::sin(timeScaled * 3.0f + golden * 3.0f);
                    float twinkle2 = std::sin(timeScaled * 5.0f + golden * 2.0f) * 0.3f;
                    float twinkle = 0.5f + 0.5f * (twinkle1 + twinkle2) / 1.3f;

                    if (twinkle < 0.2f)
                        continue;

                    finalAlpha = alpha * twinkle * alphaVariation;
                    finalSize = particleSize * (0.5f + 0.7f * twinkle);

                    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
                    int glowA = std::clamp((int)(finalAlpha * 60.0f), 0, 255);

                    // Blue star colors - vary from deep blue to bright cyan-white
                    // Brighter stars are more cyan/white, dimmer ones are deeper blue
                    float brightness = twinkle * 0.6f + 0.4f;
                    int sr = std::clamp((int)(80 + 175 * brightness * brightness), 0, 255); // Low red, increases with brightness
                    int sg = std::clamp((int)(120 + 135 * brightness), 0, 255);             // Medium green
                    int sb = std::clamp((int)(180 + 75 * brightness), 0, 255);              // High blue base

                    // Rotation based on time for sparkle effect
                    float rotation = timeScaled * 0.5f + golden;

                    // Use textured sprite if available
                    if (hasTextures)
                    {
                        ParticleTextures::DrawSpriteWithIndex(list, ImVec2(x, y), finalSize * 16.0f,
                                                            texStyleId, i, IM_COL32(sr, sg, sb, a), rotation);
                    }
                    else
                    {
                        // Fallback to shape-based rendering
                        // Alternate between 4-point and 6-point stars
                        if (i % 3 == 0)
                        {
                            DrawStar6(list, ImVec2(x, y), finalSize, IM_COL32(sr, sg, sb, a),
                                    IM_COL32(sr, sg, sb, glowA), rotation);
                        }
                        else
                        {
                            DrawStar4(list, ImVec2(x, y), finalSize * 0.9f, IM_COL32(sr, sg, sb, a),
                                    IM_COL32(sr, sg, sb, glowA), rotation);
                        }

                        // Extra bright white center flash at peak twinkle
                        if (twinkle > 0.85f)
                        {
                            float flashSize = finalSize * 0.3f * (twinkle - 0.85f) / 0.15f;
                            list->AddCircleFilled(ImVec2(x, y), flashSize, IM_COL32(220, 240, 255, a / 2), 8);
                        }
                    }
                    break;
                }

                case Settings::ParticleStyle::Sparks:
                {
                    // Yellowish sparks with trails - like fire sparks
                    float sparkTime = timeScaled * 2.0f + golden;
                    float sparkPhase = std::fmod(sparkTime, TWO_PI);
                    float life = sparkPhase / TWO_PI;

                    // Sparks shoot outward with slight curve
                    float dist = 0.2f + life * 0.8f;
                    float baseAngle = phase + std::sin(golden * 2.0f) * 0.5f;
                    float curveAngle = baseAngle + life * 0.3f * std::sin(golden);

                    x = center.x + std::cos(curveAngle) * radiusX * dist;
                    y = center.y + std::sin(curveAngle) * radiusY * dist - life * radiusY * 0.4f;

                    // Fade out with life, but also flicker
                    float flicker = 0.8f + 0.2f * std::sin(timeScaled * 15.0f + golden * 5.0f);
                    finalAlpha = alpha * (1.0f - life * life) * flicker * alphaVariation;
                    if (finalAlpha < 0.05f)
                        continue;

                    finalSize = particleSize * (1.0f - life * 0.4f);

                    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

                    // Yellowish spark colors - bright yellow to orange as they fade
                    float heatFade = 1.0f - life * 0.5f;                              // Cooler as they travel
                    int sr = std::clamp((int)(255 * heatFade), 180, 255);             // High red
                    int sg = std::clamp((int)(220 * heatFade - life * 80), 120, 220); // Medium-high green, fades
                    int sb = std::clamp((int)(80 - life * 60), 20, 80);               // Low blue

                    // Use textured sprite if available
                    if (hasTextures)
                    {
                        ParticleTextures::DrawSpriteWithIndex(list, ImVec2(x, y), finalSize * 16.0f,
                                                            texStyleId, i, IM_COL32(sr, sg, sb, a), curveAngle);
                    }
                    else
                    {
                        DrawSpark(list, ImVec2(x, y), finalSize, curveAngle, sr, sg, sb, a, life);
                    }
                    break;
                }

                case Settings::ParticleStyle::Wisps:
                {
                    // Ethereal flowing wisps with pale/blue tint
                    float wispTime = timeScaled * 0.3f;
                    float wave1 = std::sin(wispTime + golden) * 0.3f;
                    float wave2 = std::sin(wispTime * 1.7f + golden * 1.3f) * 0.15f;
                    float orbit = phase + wispTime + wave1 + wave2;

                    float radiusMod = 0.4f + 0.6f * std::sin(golden + wispTime * 0.5f);
                    x = center.x + std::cos(orbit) * radiusX * radiusMod;
                    y = center.y + std::sin(orbit * 0.7f) * radiusY * radiusMod;

                    // Gentle pulsing (alpha only, not size - keep pixel art consistent)
                    float pulse = 0.6f + 0.4f * std::sin(wispTime * 2.0f + golden * 2.0f);
                    finalAlpha = alpha * pulse * 0.7f * alphaVariation;
                    finalSize = particleSize; // Constant size for crisp pixel art

                    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

                    // Ethereal pale/blue tint - shift base color toward white/blue
                    int wr = std::min(255, r + 40);
                    int wg = std::min(255, g + 50);
                    int wb = std::min(255, b + 60);

                    // Movement angle for trail direction
                    float moveAngle = orbit + wave1 * 2.0f;
                    float trailLength = 1.5f + 0.5f * std::sin(golden);

                    // Use textured sprite if available
                    if (hasTextures)
                    {
                        ParticleTextures::DrawSpriteWithIndex(list, ImVec2(x, y), finalSize * 16.0f,
                                                            texStyleId, i, IM_COL32(wr, wg, wb, a), moveAngle);
                    }
                    else
                    {
                        DrawWisp(list, ImVec2(x, y), finalSize, moveAngle, wr, wg, wb, a, trailLength);
                    }
                    break;
                }

                case Settings::ParticleStyle::Runes:
                {
                    // Orbiting magical rune symbols
                    float runeOrbit = phase + timeScaled * 0.4f;
                    float wobble = std::sin(timeScaled + golden) * 0.1f;
                    float floatY = std::sin(timeScaled * 1.5f + golden * 2.0f) * radiusY * 0.08f;

                    x = center.x + std::cos(runeOrbit + wobble) * radiusX * 0.75f;
                    y = center.y + std::sin(runeOrbit + wobble) * radiusY * 0.45f + floatY;

                    // Pulsing glow
                    float pulse = 0.7f + 0.3f * std::sin(timeScaled * 2.0f + golden);
                    finalAlpha = alpha * pulse * alphaVariation;
                    finalSize = particleSize * 1.5f;

                    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

                    DrawRune(list, ImVec2(x, y), finalSize, r, g, b, a, i);
                    break;
                }

                case Settings::ParticleStyle::Orbs:
                {
                    // Soft glowing orbs with smooth gradients
                    float orbTime = timeScaled * 0.4f;
                    float orbit = phase + orbTime;

                    // Breathing/floating motion
                    float breathe = 0.85f + 0.15f * std::sin(orbTime * 1.5f + golden);
                    float floatY = std::sin(orbTime * 2.0f + golden * 1.5f) * radiusY * 0.1f;

                    float radiusMod = (0.45f + 0.45f * std::sin(golden)) * breathe;
                    x = center.x + std::cos(orbit) * radiusX * radiusMod;
                    y = center.y + std::sin(orbit * 0.8f) * radiusY * radiusMod + floatY;

                    // Gentle pulsing glow (alpha only, not size - keep pixel art consistent)
                    float glow = 0.65f + 0.35f * std::sin(orbTime * 2.0f + golden * 2.0f);
                    finalAlpha = alpha * glow * 0.6f * alphaVariation;
                    finalSize = particleSize; // Constant size for crisp pixel art

                    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

                    // Use textured sprite if available
                    if (hasTextures)
                    {
                        ParticleTextures::DrawSpriteWithIndex(list, ImVec2(x, y), finalSize * 16.0f,
                                                            texStyleId, i, IM_COL32(r, g, b, a), 0.0f);
                    }
                    else
                    {
                        DrawSoftOrb(list, ImVec2(x, y), finalSize, r, g, b, a);
                    }
                    break;
                }
            }
        }
    }
}
