#pragma once

#include "PCH.h"
#include "Settings.h"

/**
 * @namespace TextEffects
 * @brief Collection of text rendering effects for ImGui.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Rendering
 *
 * Provides functions for rendering text with various visual effects using ImGui's
 * draw list API. Effects are achieved by manipulating per-vertex colors after
 * text is rendered to the draw list.
 *
 * ## :material-palette-swatch-variant: Effect Categories
 *
 * - **Gradients**: Horizontal, Vertical, Diagonal, Radial
 * - **Animated**: Shimmer, Pulse, RainbowWave, ConicRainbow
 * - **Complex**: Aurora, Sparkle, Plasma, Scanline
 * - **Utility**: Outline, Glow
 *
 * ## :material-sort-variant: Rendering Order
 *
 * 1. Glow (if enabled) - soft bloom behind text
 * 2. Shadow - offset dark copy
 * 3. Outline - 8-directional border
 * 4. Main text - with gradient/effect colors
 *
 * @see Settings::EffectType, Settings::EffectParams
 */
namespace TextEffects
{
    // ========== Utility Functions ==========

    /**
     * Clamp value to [0, 1] range.
     *
     * @paramx Input value to clamp.
     *
     * @return Value clamped to [0.0, 1.0].
     */
    float Saturate(float x);

    /**
     * Quintic smoothstep (smootherstep).
     *
     * Applies Ken Perlin's improved smoothstep with C2 continuity
     * (smooth first and second derivatives at boundaries).
     *
     * $$\text{smoothstep}(t) = 6t^5 - 15t^4 + 10t^3$$
     *
     * @paramt Normalized input value (clamped to [0, 1]).
     *
     * @return Smoothly interpolated value in [0, 1].
     */
    float SmoothStep(float t);

    /**
     * Linearly interpolate between two packed colors.
     *
     * Interpolates each RGBA channel independently using linear interpolation:
     * $$C_{out} = C_a + (C_b - C_a) \cdot t = C_a(1-t) + C_b \cdot t$$
     *
     * Where $t \in [0, 1]$, $t=0$ returns color $a$, $t=1$ returns color $b$.
     *
     * @parama First color (ImU32 packed ABGR format).
     * @paramb Second color (ImU32 packed ABGR format).
     * @paramt Interpolation factor [0, 1]. Values outside range are clamped.
     *
     * @return Interpolated color as ImU32.
     *
     * @see Saturate
     */
    ImU32 LerpColorU32(ImU32 a, ImU32 b, float t);

    // ========== Basic Effects ==========

    /**
     * Draw text with 8-directional outline.
     *
     * Renders text with a solid color and surrounding outline for readability.
     * The outline is drawn first in 8 directions (cardinal + diagonal), then
     * the main text on top.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramcol Main text color (ImU32).
     * @paramoutline Outline color (typically black for contrast).
     * @paramw Outline width in pixels.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 col, ImU32 outline, float w);

    // ========== Gradient Effects ==========

    /**
     * Draw text with horizontal gradient (no outline).
     *
     * Colors transition from left to right across the entire text width.
     * Each vertex is colored based on its X position within the text bounds.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramcolLeft Color at left edge of text.
     * @paramcolRight Color at right edge of text.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4Gradient
     */
    void AddTextHorizontalGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 colLeft, ImU32 colRight);

    /**
     * Draw text with horizontal gradient and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramcolLeft Left gradient color.
     * @paramcolRight Right gradient color.
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextHorizontalGradient, AddTextOutline4
     */
    void AddTextOutline4Gradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 colLeft, ImU32 colRight, ImU32 outline, float w);

    /**
     * Draw text with vertical gradient (no outline).
     *
     * Colors transition from top to bottom across the text height.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramtop Color at top of text.
     * @parambottom Color at bottom of text.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4VerticalGradient
     */
    void AddTextVerticalGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 top, ImU32 bottom);

    /**
     * Draw text with vertical gradient and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramtop Top gradient color.
     * @parambottom Bottom gradient color.
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4VerticalGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 top, ImU32 bottom, ImU32 outline, float w);

    /**
     * Draw text with diagonal gradient.
     *
     * Colors transition along an arbitrary direction vector.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parama Start color of gradient.
     * @paramb End color of gradient.
     * @paramdir Gradient direction vector (should be normalized).
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4DiagonalGradient
     */
    void AddTextDiagonalGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 a, ImU32 b, ImVec2 dir);

    /**
     * Draw text with diagonal gradient and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parama Start gradient color.
     * @paramb End gradient color.
     * @paramdir Gradient direction vector (normalized).
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4DiagonalGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 a, ImU32 b, ImVec2 dir, ImU32 outline, float w);

    /**
     * Draw text with radial gradient (center to edge).
     *
     * Colors radiate outward from the center of the text bounds.
     * The interpolation factor is computed from normalized distance:
     * $$t = \left(\frac{d}{r_{max}}\right)^\gamma$$
     *
     * Where $d$ is distance from center, $r_{max}$ is the maximum radius.
     * - $\gamma < 1$: faster falloff near center (more center color visible)
     * - $\gamma > 1$: slower falloff near center (more edge color visible)
     * - $\gamma = 1$: linear falloff
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramcolCenter Center color (at center point).
     * @paramcolEdge Edge color (at maximum radius).
     * @paramgamma Gamma exponent for falloff curve (1.0 = linear).
     * @paramoverrideCenter Optional custom center point (nullptr = auto).
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4RadialGradient
     */
    void AddTextRadialGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 colCenter, ImU32 colEdge,
        float gamma = 1.0f, ImVec2* overrideCenter = nullptr);

    /**
     * Draw text with radial gradient and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramcolCenter Center color.
     * @paramcolEdge Edge color.
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     * @paramgamma Gamma correction (1.0 = linear).
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4RadialGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 colCenter, ImU32 colEdge, ImU32 outline, float w,
        float gamma = 1.0f);

    // ========== Animated Effects ==========

    /**
     * Draw text with pulsing brightness.
     *
     * Gradient colors oscillate in brightness over time using sine wave modulation.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parama First gradient color.
     * @paramb Second gradient color.
     * @paramtime Current time in seconds (typically from ImGui::GetTime()).
     * @paramfreqHz Pulse frequency in Hz (cycles per second).
     * @paramamp Pulse amplitude [0, 1] (0 = no pulse, 1 = full range).
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4PulseGradient
     */
    void AddTextPulseGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 a, ImU32 b, float time, float freqHz, float amp);

    /**
     * Draw text with pulsing gradient and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parama First gradient color.
     * @paramb Second gradient color.
     * @paramtime Current time in seconds.
     * @paramfreqHz Pulse frequency in Hz.
     * @paramamp Pulse amplitude [0, 1].
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4PulseGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 a, ImU32 b, float time, float freqHz, float amp,
        ImU32 outline, float w);

    /**
     * Draw text with shimmer effect (moving highlight band).
     *
     * A bright highlight band sweeps across the text horizontally.
     * Band intensity uses a Gaussian-like profile via quintic smoothstep:
     * $$I(x) = \text{smoothstep}\left(1 - \frac{|x - x_{band}|}{w/2}\right) \cdot strength$$
     *
     * Where $x_{band} = phase \cdot width_{text}$ is the band center position.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseL Left base gradient color.
     * @parambaseR Right base gradient color.
     * @paramhighlight Highlight color for the shimmer band.
     * @paramphase01 Animation phase [0, 1] controlling band position.
     * @parambandWidth01 Width of band as fraction of text width [0, 1].
     * @paramstrength01 Highlight intensity [0, 1].
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4Shimmer, AddTextOutline4ChromaticShimmer
     */
    void AddTextShimmer(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 highlight,
        float phase01, float bandWidth01, float strength01 = 1.0f);

    /**
     * Draw text with shimmer effect and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseL Left base gradient color.
     * @parambaseR Right base gradient color.
     * @paramhighlight Highlight color for shimmer band.
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     * @paramphase01 Animation phase [0, 1].
     * @parambandWidth01 Band width as fraction [0, 1].
     * @paramstrength01 Highlight intensity [0, 1].
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4Shimmer(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 highlight, ImU32 outline, float w,
        float phase01, float bandWidth01, float strength01 = 1.0f);

    /**
     * Draw text with gradient base and shimmer (internal helper).
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseL Left base gradient color.
     * @parambaseR Right base gradient color.
     * @paramhighlight Shimmer highlight color.
     * @paramphase01 Animation phase [0, 1].
     * @parambandWidth01 Band width fraction [0, 1].
     * @paramstrength01 Highlight strength [0, 1].
     */
    void AddTextGradientShimmer(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 highlight,
        float phase01, float bandWidth01, float strength01);

    /**
     * Draw text with solid base and shimmer (internal helper).
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambase Solid base color.
     * @paramhighlight Shimmer highlight color.
     * @paramphase01 Animation phase [0, 1].
     * @parambandWidth01 Band width fraction [0, 1].
     * @paramstrength01 Highlight strength [0, 1].
     */
    void AddTextSolidShimmer(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 base, ImU32 highlight,
        float phase01, float bandWidth01, float strength01);

    /**
     * Draw text with chromatic aberration shimmer.
     *
     * High-quality shimmer with RGB channel separation creating a "ghosting" effect.
     * Renders multiple offset layers with color tinting for a premium look.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseL Left base color.
     * @parambaseR Right base color.
     * @paramhighlight Highlight color for shimmer band.
     * @paramoutline Outline color.
     * @paramoutlineW Outline width in pixels.
     * @paramphase01 Animation phase [0, 1].
     * @parambandWidth01 Highlight band width [0, 1].
     * @paramstrength01 Effect strength [0, 1].
     * @paramsplitPx Chromatic separation distance in pixels.
     * @paramghostAlphaMul Alpha multiplier for ghost layers [0, 1].
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4Shimmer
     */
    void AddTextOutline4ChromaticShimmer(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 highlight,
        ImU32 outline, float outlineW,
        float phase01, float bandWidth01, float strength01,
        float splitPx, float ghostAlphaMul);

    // ========== Rainbow Effects ==========

    /**
     * Draw text with animated rainbow wave (no outline).
     *
     * Colors cycle through the hue spectrum with a wave pattern.
     * Uses HSV color space for smooth transitions.
     *
     * Per-vertex hue is calculated as:
     * $$H = H_{base} + \frac{x - x_{min}}{x_{max} - x_{min}} \cdot H_{spread} + t \cdot speed$$
     *
     * HSV to RGB conversion follows standard formulas with $H \in [0,1]$
     * mapped to $[0°, 360°]$.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseHue Starting hue offset [0, 1] (0 = red, 0.33 = green, 0.66 = blue).
     * @paramhueSpread Hue variation across text width [0, 1].
     * @paramspeed Animation speed multiplier (hue shift per second).
     * @paramsaturation Color saturation [0, 1] (0 = grayscale, 1 = vivid).
     * @paramvalue Color brightness [0, 1] (0 = black, 1 = bright).
     * @paramalpha Transparency [0, 1].
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4RainbowWave
     */
    void AddTextRainbowWave(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        float baseHue, float hueSpread, float speed, float saturation, float value, float alpha);

    /**
     * Draw text with rainbow wave effect and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseHue Starting hue [0, 1].
     * @paramhueSpread Hue variation [0, 1].
     * @paramspeed Animation speed multiplier.
     * @paramsaturation Color saturation [0, 1].
     * @paramvalue Color brightness [0, 1].
     * @paramalpha Transparency [0, 1].
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     * @paramuseWhiteBase If true, draws white base layer for brightness boost.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4RainbowWave(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        float baseHue, float hueSpread, float speed, float saturation, float value,
        float alpha, ImU32 outline, float w, bool useWhiteBase = false);

    /**
     * Draw text with conic rainbow (circular hue rotation).
     *
     * Hue rotates around a center point, creating a circular rainbow pattern.
     * Per-vertex hue is computed from the angle to center:
     * $$H = \frac{\text{atan2}(y - c_y, x - c_x)}{2\pi} + H_{base} + t \cdot speed$$
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseHue Starting hue offset [0, 1].
     * @paramspeed Rotation speed (hue shift per second).
     * @paramsaturation Color saturation [0, 1].
     * @paramvalue Color brightness [0, 1].
     * @paramalpha Transparency [0, 1].
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4ConicRainbow
     */
    void AddTextConicRainbow(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        float baseHue, float speed, float saturation, float value, float alpha);

    /**
     * Draw text with conic rainbow and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseHue Starting hue [0, 1].
     * @paramspeed Rotation speed.
     * @paramsaturation Color saturation [0, 1].
     * @paramvalue Color brightness [0, 1].
     * @paramalpha Transparency [0, 1].
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     * @paramuseWhiteBase If true, draws white base for brightness.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4ConicRainbow(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        float baseHue, float speed, float saturation, float value, float alpha,
        ImU32 outline, float w, bool useWhiteBase = false);

    // ========== Complex Effects ==========

    /**
     * Draw text with animated aurora/northern lights effect.
     *
     * Creates flowing, wave-like color transitions reminiscent of aurora borealis.
     * Multiple sine waves combine to create organic, curtain-like movement.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramcolA First aurora color.
     * @paramcolB Second aurora color.
     * @paramspeed Animation speed multiplier.
     * @paramwaves Number of wave cycles across text width.
     * @paramintensity Color blend intensity.
     * @paramsway Horizontal sway amount for curtain effect.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4Aurora
     */
    void AddTextAurora(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 colA, ImU32 colB,
        float speed, float waves, float intensity, float sway);

    /**
     * Draw text with aurora effect and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramcolA First aurora color.
     * @paramcolB Second aurora color.
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     * @paramspeed Animation speed multiplier.
     * @paramwaves Number of wave cycles.
     * @paramintensity Color blend intensity.
     * @paramsway Horizontal sway amount.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4Aurora(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 colA, ImU32 colB, ImU32 outline, float w,
        float speed, float waves, float intensity, float sway);

    /**
     * Draw text with sparkle/glitter effect.
     *
     * Creates twinkling star highlights across the text surface.
     * Ideal for ice/frost, magical, or precious metal effects.
     *
     * Uses hash-based pseudo-random sparkle positions with sine-wave
     * brightness modulation at different phases per sparkle.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseL Left base gradient color.
     * @parambaseR Right base gradient color.
     * @paramsparkleColor Sparkle highlight color.
     * @paramdensity Sparkle density [0, 1] (higher = more sparkles).
     * @paramspeed Twinkle animation speed.
     * @paramintensity Sparkle brightness multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4Sparkle
     */
    void AddTextSparkle(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 sparkleColor,
        float density, float speed, float intensity);

    /**
     * Draw text with sparkle effect and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseL Left base gradient color.
     * @parambaseR Right base gradient color.
     * @paramsparkleColor Sparkle highlight color.
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     * @paramdensity Sparkle density [0, 1].
     * @paramspeed Twinkle animation speed.
     * @paramintensity Sparkle brightness multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4Sparkle(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 sparkleColor, ImU32 outline, float w,
        float density, float speed, float intensity);

    /**
     * Draw text with classic plasma effect.
     *
     * Creates demoscene-style plasma using overlapping sine waves.
     * Two wave patterns create interference for organic movement:
     * $$t = \frac{\sin(x \cdot f_1 + time \cdot speed) + \sin(y \cdot f_2 + time \cdot speed)}{2} \cdot 0.5 + 0.5$$
     *
     * The normalized value $t$ drives interpolation between the two colors.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramcolA First plasma color.
     * @paramcolB Second plasma color.
     * @paramfreq1 First sine wave frequency.
     * @paramfreq2 Second sine wave frequency.
     * @paramspeed Animation speed multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4Plasma
     */
    void AddTextPlasma(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 colA, ImU32 colB,
        float freq1, float freq2, float speed);

    /**
     * Draw text with plasma effect and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramcolA First plasma color.
     * @paramcolB Second plasma color.
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     * @paramfreq1 First sine wave frequency.
     * @paramfreq2 Second sine wave frequency.
     * @paramspeed Animation speed multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4Plasma(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 colA, ImU32 colB, ImU32 outline, float w,
        float freq1, float freq2, float speed);

    /**
     * Draw text with horizontal scanline effect.
     *
     * Creates a sweeping horizontal highlight bar like a scanner
     * or cyberpunk/Dwemer-style effect.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseL Left base gradient color.
     * @parambaseR Right base gradient color.
     * @paramscanColor Scanline highlight color.
     * @paramspeed Scan speed (cycles per second).
     * @paramwidth Scanline width [0, 1] as fraction of text height.
     * @paramintensity Scanline brightness multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4Scanline
     */
    void AddTextScanline(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 scanColor,
        float speed, float width, float intensity);

    /**
     * Draw text with scanline effect and outline.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @parambaseL Left base gradient color.
     * @parambaseR Right base gradient color.
     * @paramscanColor Scanline highlight color.
     * @paramoutline Outline color.
     * @paramw Outline width in pixels.
     * @paramspeed Scan speed (cycles per second).
     * @paramwidth Scanline width [0, 1].
     * @paramintensity Scanline brightness multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4Scanline(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 scanColor, ImU32 outline, float w,
        float speed, float width, float intensity);

    // ========== Glow Effect ==========

    /**
     * Draw soft glow/bloom effect behind text.
     *
     * Creates a blurred glow by drawing multiple offset copies of the text
     * in a circular pattern with decreasing alpha.
     *
     * Draws `samples` copies of text at positions around a circle of
     * radius `radius`, with alpha scaled by `intensity / samples`.
     *
     * @paramlist ImGui draw list to render to.
     * @paramfont Font to use for rendering.
     * @paramsize Font size in pixels.
     * @parampos Top-left position for text.
     * @paramtext Null-terminated UTF-8 string to render.
     * @paramglowColor Glow color (alpha will be modulated).
     * @paramradius Glow spread radius in pixels.
     * @paramintensity Glow brightness [0, 1].
     * @paramsamples Number of blur samples (8-16 recommended).
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     * @pre Should be called **before** drawing the main text.
     */
    void AddTextGlow(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 glowColor,
        float radius, float intensity, int samples);

    // ========== Side Ornaments ==========

    /**
     * Draw decorative ornaments on sides of a text region.
     *
     * Renders ornamental characters from the ornament font extending
     * from both sides, suitable for high-tier character names.
     *
     * @paramlist ImGui draw list to render to.
     * @paramcenter Center point of the text being decorated.
     * @paramtextWidth Width of the text in pixels.
     * @paramtextHeight Height of the text in pixels.
     * @paramcolor Ornament color (ImU32).
     * @paramalpha Opacity [0, 1].
     * @paramscale Size multiplier for ornament dimensions.
     * @paramspacing Distance from text edges in pixels.
     * @paramanimated If true, applies subtle pulse animation.
     * @paramtime Current time for animation (from ImGui::GetTime()).
     * @paramoutlineWidth Outline thickness matching text outline.
     * @paramenableGlow Enable glow effect matching text glow.
     * @paramglowRadius Glow spread radius in pixels.
     * @paramglowIntensity Glow brightness [0, 1].
     * @paramglowSamples Number of glow samples.
     *
     * @pre list != nullptr
     *
     * @see Settings::EnableOrnaments
     */
    void DrawSideOrnaments(ImDrawList* list, const ImVec2& center,
        float textWidth, float textHeight, ImU32 color, float alpha,
        float scale, float spacing, bool animated, float time,
        float outlineWidth, bool enableGlow, float glowRadius, float glowIntensity, int glowSamples,
        const std::string& leftOrnaments, const std::string& rightOrnaments,
        float ornamentScale = 1.0f, bool isSpecialTitle = false);

    // ========== Particle Aura ==========

    /**
     * Draw floating particle aura around a text region.
     *
     * Creates an aura of animated particles around the nameplate.
     * Multiple visual styles are available:
     *
     * | Style | Description |
     * |-------|-------------|
     * | Stars | Twinkling star points (classic) |
     * | Sparks | Fast, erratic fire-like sparks |
     * | Wisps | Slow, flowing ethereal wisps |
     * | Runes | Small magical rune symbols |
     * | Orbs | Soft glowing orbs |
     *
     * @paramlist ImGui draw list to render to.
     * @paramcenter Center of the particle region.
     * @paramradiusX Horizontal spread radius in pixels.
     * @paramradiusY Vertical spread radius in pixels.
     * @paramcolor Particle base color.
     * @paramalpha Maximum particle opacity [0, 1].
     * @paramstyle Particle visual style from Settings::ParticleStyle.
     * @paramparticleCount Number of particles to draw.
     * @paramparticleSize Base particle size in pixels.
     * @paramspeed Animation speed multiplier.
     * @paramtime Current time for animation.
     *
     * @pre list != nullptr
     *
     * @see Settings::ParticleStyle, Settings::EnableParticleAura
     */
    void DrawParticleAura(ImDrawList* list, const ImVec2& center,
        float radiusX, float radiusY, ImU32 color, float alpha,
        Settings::ParticleStyle style, int particleCount, float particleSize,
        float speed, float time, int styleIndex = 0, int enabledStyleCount = 1);
}
