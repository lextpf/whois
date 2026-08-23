#pragma once

#include "PCH.hpp"
#include "Settings.hpp"

#include <algorithm>

/**
 * @namespace TextEffects
 * @brief Collection of text rendering effects for ImGui.
 * @author Alex (https://github.com/lextpf)
 * @ingroup TextEffects
 *
 * Most effects render text through ImGui's draw list, then rewrite the per-vertex colors of
 * the glyph quads that call just emitted. The stamp-based passes are the exceptions:
 * AddTextOutline4, AddTextGlow, AddTextSoftShadow and the DrawOutline family emit extra
 * offset copies of the string and never touch the vertex buffer.
 *
 * Every function here runs on the render thread, inside an ImGui frame. None of them read or
 * write game state; they only append to the draw list they are given.
 *
 * The animation clock is not uniform. An effect that takes a speed, period or rate argument
 * samples ImGui::GetTime() itself. AddTextShimmer and AddTextEclipse instead take phase01
 * from the caller and hold still when the caller does not advance it. ApplyWaveDisplacement
 * and DrawParticleAura take an explicit time value in seconds.
 *
 * Every `AddText*` entry point starts with the same five arguments - draw list, font, font
 * size in pixels, top-left position, and a null-terminated UTF-8 string - and returns
 * without drawing when the draw list or the font is null, or when the string is null or
 * empty. DrawOutline is the exception: it has no null guard, so its preconditions are
 * mandatory, and WithOutline and WithOutlineGlow inherit them.
 *
 * ## :material-palette-swatch-variant: Effect Categories
 *
 * - **Gradients**: Horizontal, Vertical, Diagonal, Radial
 * - **Animated**: Shimmer, Ember, Aurora, Breathe, Mote, Wander, Eclipse, Pulse, Electric
 * - **Complex**: Sparkle, Enchant, Frost, Drift
 * - **Utility**: Outline, Glow, Soft shadow, Shine overlay, Wave displacement
 *
 * ## :material-sort-variant: Rendering Order
 *
 * The caller emits passes 1 and 2. Passes 3 to 7 run inside the renderer's
 * ApplyTextEffect. Every pass marked optional is skipped when its feature is off.
 *
 * 1. Glow (optional) - soft bloom behind text
 * 2. Shadow (optional) - one offset copy, or a feathered disc when SoftShadow is on
 * 3. Directional inner outline (optional) - drawn below the fill, so only its rim survives
 * 4. Outline glow rings (optional)
 * 5. Outer outline - 4 cardinal stamps (FastOutlines) or a ring of 8 to 24 stamps
 * 6. Main text fill - with gradient/effect colors
 * 7. Text-alpha shaping, top-edge shine overlay, wave displacement (each optional)
 *
 * @see Settings::EffectType, Settings::EffectParams
 */
namespace TextEffects
{
/**
 * @brief Clamp a value to the 0 to 1 range.
 *
 * @param x Input value.
 * @return Value clamped to [0, 1].
 */
constexpr float Saturate(float x)
{
    return std::clamp(x, .0f, 1.0f);
}

/**
 * @brief Quintic smoothstep, with continuous first and second derivatives at the ends.
 *
 * $$\text{smoothstep}(t) = 6t^5 - 15t^4 + 10t^3$$
 *
 * @param t Input value, clamped to [0, 1].
 * @return Interpolated value in [0, 1].
 */
constexpr float SmoothStep(float t)
{
    t = Saturate(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/**
 * @brief Cubic ease-out: fast start, gentle deceleration to the target.
 *
 * $$\text{easeOutCubic}(t) = 1 - (1 - t)^3$$
 *
 * Front-loads the motion, so a value arrives at its target with weight rather than with a
 * symmetric or linear feel. Used for the entrance reveal.
 *
 * @param t Input value, clamped to [0, 1].
 * @return Eased value in [0, 1].
 */
constexpr float EaseOutCubic(float t)
{
    t = Saturate(t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/**
 * @brief Cubic ease-in: gentle start, accelerating away from the origin.
 *
 * $$\text{easeInCubic}(t) = t^3$$
 *
 * Mirror of EaseOutCubic. Used for the exit, which starts slowly and accelerates as the
 * label leaves.
 *
 * @param t Input value, clamped to [0, 1].
 * @return Eased value in [0, 1].
 *
 * @see EaseOutCubic
 */
constexpr float EaseInCubic(float t)
{
    t = Saturate(t);
    return t * t * t;
}

/**
 * @brief Exponential ease-out: very fast start, long flattening tail.
 *
 * $$\text{easeOutExpo}(t) = \begin{cases} 1 & t \ge 1 \\ 1 - 2^{-10t} & t < 1 \end{cases}$$
 *
 * Sharper than EaseOutCubic. Used to settle a positional offset into place: most of the
 * travel happens immediately, then the final pixels ease in slowly. The @p t >= 1 branch
 * returns exactly 1.0 instead of the formula's 0.99902, so a caller can test the result
 * for completion exactly. Not constexpr, because it uses std::pow.
 *
 * @param t Input value, clamped to [0, 1].
 * @return Eased value in [0, 1].
 *
 * @see EaseOutCubic
 */
float EaseOutExpo(float t);

/**
 * @brief Linearly interpolate two packed colors, channel by channel.
 *
 * $$C_{out} = C_a + (C_b - C_a) \cdot t = C_a(1-t) + C_b \cdot t$$
 *
 * Alpha is interpolated with the three color channels, so blending toward a color that
 * carries a different alpha also changes the result's opacity. Each channel is rounded to
 * the nearest integer.
 *
 * @param a First color, ImU32 packed ABGR.
 * @param b Second color, ImU32 packed ABGR.
 * @param t Interpolation factor, clamped to [0, 1]; 0 returns `a`, 1 returns `b`.
 * @return Interpolated color as ImU32.
 *
 * @see Saturate
 */
ImU32 LerpColorU32(ImU32 a, ImU32 b, float t);

/**
 * @struct OutlineGlowParams
 * @brief Parameters for the white halo behind text outlines.
 */
struct OutlineGlowParams
{
    bool enabled = false;  ///< Master gate; false skips the glow rings
    /// Glow color, typically white. RGB is used as-is; the alpha channel is the base value
    /// that the per-ring falloff scales.
    ImU32 color = 0;
    /// Innermost ring radius, as a multiple of the outline width. Outer rings grow to
    /// 1.6 times this, so the rings overlap into one halo instead of separate shells.
    float scale = 1.6f;
    /// Peak ring opacity, as a multiplier of the glow color's alpha. The innermost ring
    /// reaches it; each ring further out falls off by exp(-2.5 * t * t), with t running
    /// from 0 at the innermost ring to 1 at the outermost.
    float alpha = .20f;
    int rings = 2;  ///< Number of concentric rings; the INI clamps the value to 1-3
};

/**
 * @brief Stamp an outline around a text position.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param outline Outline color.
 * @param w Outline width in pixels.
 * @param fastOutlines If true, stamps the 4 cardinal offsets; otherwise stamps a ring of
 *                     8 to 24 taps, the count scaled to the circumference of `w`.
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
 *
 * @warning The preconditions are mandatory. This call chain has no null guard and
 *          dereferences @p list at once, unlike AddTextOutline4 and the AddText*
 *          effects, which return on a null argument.
 */
void DrawOutline(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 outline,
                 float w,
                 bool fastOutlines);

/**
 * @struct ShineParams
 * @brief Parameters for the static top-edge shine overlay.
 */
struct ShineParams
{
    /// Gates the top-edge overlay only, and the overlay also needs intensity above 0.
    /// innerTextAlpha and textGlowAlpha are applied whenever a non-null ShineParams is
    /// passed, including when this is false.
    bool enabled = false;
    /// Strength knob in [0, 1], not the resulting peak alpha. The value is scaled by
    /// 0.40, attenuated toward the left and right edges, then shaped by pow(x, 1.35)
    /// before it becomes the vertex alpha.
    float intensity = .35f;
    /// Vertical falloff knob. The applied exponent is max(0.5, falloff) * 3.4, so the
    /// default 2.0 shapes with an exponent of 6.8.
    float falloff = 2.0f;
    /// Text-glow amount in [0, 1]; 0 is off. It fades the bright fill by up to 20% and
    /// brightens it by up to 30%. It applies only to vertices the renderer classifies
    /// as text body (luminance >= 0.22, brightest channel >= 80, and alpha at or above
    /// the per-batch body threshold).
    float textGlowAlpha = .0f;
    float innerTextAlpha = 1.0f;  ///< Text body alpha multiplier 0-1 (applied after effects)
};

/**
 * @struct WaveParams
 * @brief Parameters for the wave displacement effect.
 */
struct WaveParams
{
    bool enabled = false;    ///< Master gate; false skips the displacement pass
    float amplitude = 1.5f;  ///< Peak Y displacement in pixels
    float frequency = 3.0f;  ///< Cycles across the text width
    float speed = 1.0f;      ///< Phase travel in cycles per second
    float time = .0f;        ///< Caller-supplied clock in seconds (ImGui::GetTime())
};

/**
 * @brief Apply sine-wave Y displacement to a vertex range.
 *
 * Call after the text is rendered and vertex-colored by an effect.
 *
 * Only the Y coordinate changes, and each vertex is displaced from its own X. The texture
 * coordinates are left alone, so a glyph quad shears vertically instead of translating as
 * a rigid quad.
 *
 * @param list       ImGui draw list that holds the vertices.
 * @param vtxStart   First vertex index to displace.
 * @param vtxEnd     One past the last vertex index to displace.
 * @param bbMinX     Left edge of the range's bounding box, in screen pixels.
 * @param bbWidth    Width of that bounding box, in screen pixels.
 * @param amplitude  Peak Y displacement in pixels.
 * @param frequency  Cycles across `bbWidth`.
 * @param speed      Phase travel in cycles per second.
 * @param time       Caller clock in seconds (ImGui::GetTime()).
 *
 * @note No-op when @p list is null, when the vertex range is empty, when
 *       @p bbWidth < 1e-3, or when @p amplitude < 0.01.
 */
void ApplyWaveDisplacement(ImDrawList* list,
                           int vtxStart,
                           int vtxEnd,
                           float bbMinX,
                           float bbWidth,
                           float amplitude,
                           float frequency,
                           float speed,
                           float time);

/**
 * @brief Draw a directional inner outline tinted toward the tier color.
 *
 * The renderer draws it below the outline glow, the outer outline and the fill, so only
 * the rim survives. That keeps its sub-pixel stamps from filming over animated effects.
 *
 * Two paths exist. When @p lightBias is 0.001 or less, or when @p fastOutlines is true,
 * the call draws one uniform outline of width outerWidth * innerScale. Otherwise it stamps
 * 8 fixed directions (E, W, N, S and the four diagonals) and scales each stamp's offset by
 * 1 - dot(direction, light) * lightBias, so stamps that oppose the light sit further out
 * and the rim reads as lit from one side.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param outerColor Outer outline color; the rim color starts from it.
 * @param tierColor Color the rim is tinted toward.
 * @param outerWidth Outer outline width in pixels.
 * @param innerScale Inner rim width as a fraction of `outerWidth`.
 * @param tintFactor Blend from @p outerColor to `tierColor`; 0 keeps the outline color,
 *                   1 uses the tier color.
 * @param alphaFactor Multiplier applied to the blended color's alpha.
 * @param lightAngleDeg Light direction in degrees, measured with +x right and +y down, so
 *                      315 points up and to the right.
 * @param lightBias Directional width variation; 0 gives a uniform rim.
 * @param fastOutlines If true, forces the uniform path with the 4-stamp outline.
 *
 * @note Returns without drawing when `list`, @p font or @p text is null, when the string
 *       is empty, when @p alphaFactor is 0 or less, or when outerWidth * innerScale is
 *       below 0.5 px.
 */
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
                                 bool fastOutlines);

/**
 * @brief Draw concentric glow rings behind the text outline.
 *
 * Rings are stamped from the outermost and faintest ring inward. Each ring is one full
 * outline pass, so the cost is rings times the cost of DrawOutline.
 *
 * For ring index $k$ counted from the innermost, with $n$ rings and
 * $\tau = k / \max(n - 1,\ 1)$:
 *
 * $$r_k = w \cdot glowScale \cdot (1 + 0.6\,\tau), \qquad
 *   a_k = A_{glow} \cdot glowAlpha \cdot e^{-2.5\,\tau^2}$$
 *
 * Here $w$ is outlineWidth and $A_{glow}$ is the alpha channel of glowColor. The Gaussian
 * alpha term and the near-linear radius growth make the few rings read as one halo instead
 * of separate shells.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param glowColor Ring color; RGB is used as-is, its alpha is the base for the falloff.
 * @param outlineWidth Outline width in pixels; every ring radius is a multiple of it.
 * @param glowScale Innermost ring radius, as a multiple of `outlineWidth`. The outermost
 *                  ring reaches 1.6 times that value.
 * @param glowAlpha Peak ring opacity, as a multiplier of the glow color's alpha.
 * @param rings Number of concentric rings.
 * @param fastOutlines If true, every ring uses the 4-stamp outline; otherwise the ring of
 *                     8 to 24 taps.
 *
 * @note Unlike DrawOutline, this call is guarded: it returns without drawing when `list`,
 *       @p font or @p text is null, when the string is empty, when @p glowAlpha is 0 or
 *       less, or when @p rings is 0 or less. A single ring whose solved alpha rounds to 0
 *       is skipped, and the remaining rings still draw.
 */
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
                     bool fastOutlines);

/**
 * @brief Draw an outline, then delegate the fill to any effect function.
 *
 * @code
 * WithOutline<AddTextHorizontalGradient>(list, font, size, pos, text,
 *                                        outline, w, fastOutlines, colLeft, colRight);
 * @endcode
 *
 * @tparam EffectFn Pointer to the non-outline effect function.
 * @tparam Args     Additional
 * effect-specific argument types.
 *
 * @param list          ImGui draw list.
 * @param font Font
 * used to draw the text.
 * @param size          Font size, in pixels.
 * @param pos Top-left text
 * position.
 * @param text          Null-terminated UTF-8 text.
 * @param outline       Outline
 * color.
 * @param w             Outline width, in pixels.
 * @param fastOutlines  Whether to use
 * the four-stamp outline path.
 * @param args          Arguments forwarded to the effect function.

 * *
 * @warning DrawOutline runs first, so this template inherits its mandatory preconditions:
 *
 * `list`, @p font and @p text must all be non-null.
 */
template <auto EffectFn, typename... Args>
inline void WithOutline(ImDrawList* list,
                        ImFont* font,
                        float size,
                        const ImVec2& pos,
                        const char* text,
                        ImU32 outline,
                        float w,
                        bool fastOutlines,
                        Args&&... args)
{
    static_assert(
        std::is_invocable_v<decltype(EffectFn),
                            ImDrawList*,
                            ImFont*,
                            float,
                            const ImVec2&,
                            const char*,
                            Args...>,
        "EffectFn must accept (ImDrawList*, ImFont*, float, const ImVec2&, const char*, Args...)");
    DrawOutline(list, font, size, pos, text, outline, w, fastOutlines);
    EffectFn(list, font, size, pos, text, std::forward<Args>(args)...);
}

/**
 * @struct DualOutlineParams
 * @brief Parameters for the dual-tone directional inner outline.

 * *
 * `Renderer::ApplyTextEffect` is the only consumer. It forwards these fields to
 *
 * `DrawDirectionalInnerOutline`.
 */
struct DualOutlineParams
{
    bool enabled = false;     ///< Master gate; false skips the inner outline pass
    ImU32 tierColor = 0;      ///< Tier color to blend toward
    float innerScale = .5f;   ///< Inner outline width as fraction of outer
    float tintFactor = .3f;   ///< Blend toward tier color (0=outline, 1=tier)
    float alphaFactor = .5f;  ///< Multiplier on the rim alpha; 0 or less skips the pass
    /// Light direction in degrees, measured with +x right and +y down, so 315 points up
    /// and to the right. Stamps that oppose the light are pushed further out.
    float lightAngle = 315.f;
    /// Directional width variation. A value of 0.001 or less selects the uniform rim, as
    /// does FastOutlines.
    float lightBias = .15f;
};

/**
 * @brief WithOutline variant that also draws outline glow rings behind the outline.
 *
 * A null @p glow pointer, or one whose enabled flag is false, skips the rings and leaves
 * the
 * outline and the fill unchanged.
 *
 * @tparam EffectFn Pointer to the non-outline effect
 * function.
 * @tparam Args     Additional effect-specific argument types.
 *
 * @param list ImGui
 * draw list.
 * @param font          Font used to draw the text.
 * @param size          Font size,
 * in pixels.
 * @param pos           Top-left text position.
 * @param text Null-terminated UTF-8
 * text.
 * @param outline       Outline color.
 * @param w             Outline width, in pixels.
 *
 * @param fastOutlines  Whether to use the four-stamp outline path.
 * @param glow          Optional
 * outline-glow parameters.
 * @param args          Arguments forwarded to the effect function.
 *

 * * @warning DrawOutline runs after the rings, so this template inherits its mandatory
 *
 * preconditions: `list`, @p font and @p text must all be non-null.
 */
template <auto EffectFn, typename... Args>
inline void WithOutlineGlow(ImDrawList* list,
                            ImFont* font,
                            float size,
                            const ImVec2& pos,
                            const char* text,
                            ImU32 outline,
                            float w,
                            bool fastOutlines,
                            const OutlineGlowParams* glow,
                            Args&&... args)
{
    static_assert(
        std::is_invocable_v<decltype(EffectFn),
                            ImDrawList*,
                            ImFont*,
                            float,
                            const ImVec2&,
                            const char*,
                            Args...>,
        "EffectFn must accept (ImDrawList*, ImFont*, float, const ImVec2&, const char*, Args...)");
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
    DrawOutline(list, font, size, pos, text, outline, w, fastOutlines);
    EffectFn(list, font, size, pos, text, std::forward<Args>(args)...);
}

/**
 * @brief Draw text in a solid color with a surrounding outline for readability.
 *
 * The `4` in the name is not a limit: @p fastOutlines picks between the 4-stamp outline
 * and the smooth outline ring.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param col Main text color (ImU32).
 * @param outline Outline color (typically black for contrast).
 * @param w Outline width in pixels.
 * @param fastOutlines If true, stamps the 4 cardinal offsets; otherwise stamps a ring of
 *                     8 to 24 taps, with the tap count scaled to the circumference.
 * @param glow Optional outline glow parameters. A null pointer, or one whose enabled flag
 *             is false, draws no glow. The rings are drawn before the outline.
 */
void AddTextOutline4(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 col,
                     ImU32 outline,
                     float w,
                     bool fastOutlines,
                     const OutlineGlowParams* glow = nullptr);

/**
 * @brief Draw text with a left-to-right gradient and no outline.
 *
 * Each vertex is colored from its X position within the text bounds.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colLeft Color at left edge of text. A string narrower than 1e-3 px is filled with
 *                this color alone, because the interpolation has no range.
 * @param colRight Color at right edge of text.
 */
void AddTextHorizontalGradient(ImDrawList* list,
                               ImFont* font,
                               float size,
                               const ImVec2& pos,
                               const char* text,
                               ImU32 colLeft,
                               ImU32 colRight);

/**
 * @brief Draw text with a top-to-bottom gradient and no outline.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param top Color at top of text.
 * @param bottom Color at bottom of text.
 */
void AddTextVerticalGradient(ImDrawList* list,
                             ImFont* font,
                             float size,
                             const ImVec2& pos,
                             const char* text,
                             ImU32 top,
                             ImU32 bottom);

/**
 * @brief Draw text with a gradient along an arbitrary direction vector.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param a Start color of gradient.
 * @param b End color of gradient.
 * @param dir Gradient direction; normalized internally. A near-zero vector
 *            (|dir| < 1e-3) falls back to (1, 0), which gives a horizontal gradient.
 */
void AddTextDiagonalGradient(ImDrawList* list,
                             ImFont* font,
                             float size,
                             const ImVec2& pos,
                             const char* text,
                             ImU32 a,
                             ImU32 b,
                             ImVec2 dir);

/**
 * @brief Draw text with a radial gradient from the center of the text bounds outward.
 *
 * $$t = \left(\frac{d}{r_{max}}\right)^\gamma$$
 *
 * Where $d$ is the distance from the center, $r_{max}$ is the distance from the center to
 * the furthest corner of the text bounding box, and $t$ is the blend factor toward
 * `colEdge`.
 * - $\gamma < 1$: raises $t$, so the edge color reaches further inward
 * - $\gamma > 1$: lowers $t$, so the center color holds further outward
 * - $\gamma = 1$: linear falloff
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colCenter Center color (at center point).
 * @param colEdge Edge color (at maximum radius).
 * @param gamma Gamma exponent for the falloff curve; exactly 1.0 is linear and skips the
 *              pow call.
 * @param overrideCenter Optional center point, in screen pixels. Null uses the center of
 *                       the text bounding box. The center may sit outside that box; the
 *                       ratio $d / r_{max}$ is clamped to [0, 1] before the exponent, so
 *                       the far side saturates at the edge color instead of overshooting.
 */
void AddTextRadialGradient(ImDrawList* list,
                           ImFont* font,
                           float size,
                           const ImVec2& pos,
                           const char* text,
                           ImU32 colCenter,
                           ImU32 colEdge,
                           float gamma = 1.0f,
                           ImVec2* overrideCenter = nullptr);

/**
 * @brief Draw text with a flickering ember heat effect.
 *
 * Overlapping sine-noise layers with per-character phase variation drive the flicker.
 * Bright spots blow through to a fixed amber-white (255, 214, 140) that adopts colA's
 * alpha, so embers read as fire on any tier hue; troughs cool toward a deep charcoal
 * shade of the base pair. A vertical heat gradient makes the bottom of the text hotter.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colA Left base gradient color; the molten highlight adopts its alpha.
 * @param colB Right base gradient color.
 * @param speed Flicker animation speed multiplier.
 * @param intensity Scales the heat field, in the range 0 to 1. The field is signed around
 *                  0.32, so a low value does not neutralize the effect: it holds the text
 *                  at the charcoal pole.
 */
void AddTextEmber(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 colA,
                  ImU32 colB,
                  float speed,
                  float intensity);

/**
 * @brief Draw text with a highlight band that sweeps across it horizontally.
 *
 * With $t$ the vertex position normalized across the text width, $v$ the vertex position
 * normalized down the text height, and $p$ the band phase, the band uses a wrapped
 * distance, so it exits at the right and re-enters at the left:
 *
 * $$d = \min(|t - p|,\ |t - p + 1|,\ |t - p - 1|)$$
 *
 * The band half-width is $w_{half} = \max(0.85 \cdot bandWidth01,\ 0.01)$. A quintic
 * smoothstep core, $1 - \text{smoothstep}(d / w_{half})$, is scaled by $strength01$ and by
 * a vertical bias $1 + 0.12(1 - v)$ that favors the top of the string bounding box, then
 * summed with two exponential halos ($0.18\,e^{-6d^2}$ and $0.06\,e^{-2d^2}$, both also
 * scaled by $strength01$). The sum is scaled by 0.85 and then clamped to the 0 to 1 range,
 * so a core hit at full strength still reaches 1.
 *
 * The resting face is not the base color: it sits up to 22% toward a deep jewel shade and
 * is released as the band arrives, so the sweep has contrast to travel against.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param highlight Highlight color for the shimmer band; it adopts the fill's alpha.
 * @param phase01 Animation phase in [0, 1] controlling band position.
 * @param bandWidth01 Band half-width as a fraction of text width; scaled by 0.85.
 * @param strength01 Highlight intensity [0, 1].
 */
void AddTextShimmer(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 baseL,
                    ImU32 baseR,
                    ImU32 highlight,
                    float phase01,
                    float bandWidth01,
                    float strength01 = 1.0f);

/**
 * @brief Draw text with animated aurora color transitions.
 *
 * Multiple combined sine waves give the color flow a curtain-like movement.
 *
 * The curtain travels wider than the base pair. One scalar drives a four-stop ramp with
 * smoothstep handoffs: a deep dusk shade of the midpoint of colA and colB at 0, colA at
 * 1/3, colB at 2/3, and a white-hot crest derived from colB at 1. Tier palettes pair
 * same-family colors, so a curtain confined to colA and colB would show almost no travel.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colA First aurora color; the 1/3 stop of the ramp.
 * @param colB Second aurora color; the 2/3 stop, and the source of the white crest.
 * @param speed Animation speed multiplier.
 * @param waves Number of wave cycles across text width.
 * @param intensity Color blend intensity. It scales the ramp position, so a low value holds
 *                  the text near the dusk pole instead of neutralizing the effect.
 * @param sway Horizontal sway amount for curtain effect.
 */
void AddTextAurora(ImDrawList* list,
                   ImFont* font,
                   float size,
                   const ImVec2& pos,
                   const char* text,
                   ImU32 colA,
                   ImU32 colB,
                   float speed,
                   float waves,
                   float intensity,
                   float sway);

/**
 * @brief Draw text with twinkling star highlights across its surface.
 *
 * Sparkle positions come from a hash of the vertex position quantized to an integer grid;
 * each sparkle modulates brightness on a sine wave at its own phase. Four layers stack at
 * different grid scales and rates: large slow stars with a radial burst shape, medium
 * sparkles, fine dust, and rare bright flares. The resting face sits 12% toward a deeper
 * shade of the base pair, so every glint has contrast to flash against.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param sparkleColor Sparkle highlight color. It adopts the fill's alpha, so a glint does
 *                     not make the glyph transparent where it brightens.
 * @param density Sparkle density [0, 1] (higher = more sparkles). It lowers the hash
 *                threshold of the first three layers only; the rare flare layer uses a
 *                fixed threshold.
 * @param speed Twinkle animation speed.
 * @param intensity Sparkle brightness multiplier; the summed glint is capped at 0.95.
 */
void AddTextSparkle(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 baseL,
                    ImU32 baseR,
                    ImU32 sparkleColor,
                    float density,
                    float speed,
                    float intensity);

/**
 * @brief Draw text with a flowing energy pattern driven by fractal Brownian motion.
 *
 * Two offset noise layers combine, which keeps the flow non-repeating.
 *
 * The combined field drives a three-stop luminance weave: a deep shade of the midpoint of
 * colA and colB in the folds, then colA, then that midpoint in the body. Where the field
 * crests above 0.52 a white-hot filament derived from colB is blended on top.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colA First energy color; the middle stop of the weave.
 * @param colB Second energy color. The weave never reaches it as a fill; it contributes
 *             through the colA/colB midpoint and as the source of the filament.
 * @param speed Animation speed multiplier.
 * @param scale Noise scale (higher = finer detail).
 * @param intensity Color blend intensity [0, 1]. It scales the weave position and the
 *                  filament together, so 0 holds the text at the deep fold shade.
 */
void AddTextEnchant(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 colA,
                    ImU32 colB,
                    float speed,
                    float scale,
                    float intensity);

/**
 * @brief Draw text with a crystalline frost pattern plus sparkle flashes.
 *
 * A creeping frost overlay from hash-based crystalline noise combines with twinkling
 * sparkle highlights.
 *
 * The glaze and the flashes both target a fixed icy blue-white (220, 235, 255) that adopts
 * colA's alpha, so frost reads as ice on any tier hue. colA and colB remain the left and
 * right ends of the base gradient underneath. The glaze blends at most 50% toward the ice
 * color and the flash at most a further 90%, so the base pair always shows through.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colA Left end of the base gradient; the ice color adopts its alpha.
 * @param colB Right end of the base gradient.
 * @param density Frost coverage [0, 1] (higher = more ice). It also lowers the threshold of
 *                the medium sparkle layer; the rare flash layer uses a fixed threshold.
 * @param speed Animation speed multiplier.
 * @param sparkleIntensity Brightness of sparkle flashes [0, 1].
 */
void AddTextFrost(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 colA,
                  ImU32 colB,
                  float density,
                  float speed,
                  float sparkleIntensity);

/**
 * @brief Draw text with a slow uniform brightness pulse.
 *
 * A shared sine lerps the whole text between a deep jewel shade (the dip) and a hot
 * highlight (the lift) around the base gradient, so saturation and hue move with the
 * brightness. The dip gain is slightly the larger of the two, because a bright base has
 * little upward headroom, but the two stay close: a cycle that only darkens reads as
 * nothing happening, and the lift is what registers as a swell. The default amplitude is
 * 0.16, floored at 0.10, and the default speed of 0.25 Hz gives one cycle every four
 * seconds.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param speed Pulse frequency in Hz.
 * @param amplitude Fraction of the deep-shade to hot-highlight range travelled, with
 *                  about 2x gain on the dip and 1.7x on the lift (default 0.16,
 *                  floored at 0.10).
 */
void AddTextBreathe(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 baseL,
                    ImU32 baseR,
                    float speed,
                    float amplitude);

/**
 * @brief Draw text with a slow uniform hue wander.
 *
 * Each vertex color is converted to HSV, its hue shifted by a shared sinusoidal offset,
 * then converted back. A saturation swell of +/-18% and a value swell of +/-7% ride the
 * same cycle at interleaved phase offsets (+0.25 and +0.60 of a turn), so low-chroma tier
 * palettes still read. The default speed of 0.08 Hz gives one cycle every ~12 seconds.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param speed Drift frequency in Hz.
 * @param hueRangeDeg Peak hue deviation in degrees, applied as +/-hueRangeDeg
 *                    (default 26).
 */
void AddTextDrift(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 baseL,
                  ImU32 baseR,
                  float speed,
                  float hueRangeDeg);

/**
 * @brief Draw text with a single rare twinkling mote.
 *
 * Once per `period` seconds a glyph is chosen by hashing the period index, and the mote is
 * centred on that glyph's quad, so it always lands on ink. A bell-curve envelope fades the
 * mote in and out within that period, and a Gaussian radial falloff keeps the bright spot
 * roughly one glyph wide. Exactly one mote is visible at a time.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param moteColor Color of the twinkle itself (usually highlight); it adopts the fill's
 *                  alpha, so the glyph does not thin out where it brightens.
 * @param period Seconds between twinkles; floored at 0.1 s.
 * @param peakAlpha Peak intensity of the mote blend in [0, 1].
 */
void AddTextMote(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 baseL,
                 ImU32 baseR,
                 ImU32 moteColor,
                 float period,
                 float peakAlpha);

/**
 * @brief Draw text with per-character asynchronous breathing.
 *
 * Same brightness pulse as AddTextBreathe, but each glyph carries its own phase offset
 * hashed from its character index, so no single point commands attention.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param speed Pulse frequency in Hz.
 * @param amplitude Brightness swing per glyph in [0, 1].
 * @param spread Phase desync; clamped to >= 0 only (0 = all glyphs in phase,
 *               1 = one full turn of desync; values above 1 wrap).
 *
 * @note Per-glyph grouping assumes ImGui's 4-vertices-per-glyph quad layout, as do
 *       AddTextMote and AddTextElectric.
 */
void AddTextWander(ImDrawList* list,
                   ImFont* font,
                   float size,
                   const ImVec2& pos,
                   const char* text,
                   ImU32 baseL,
                   ImU32 baseR,
                   float speed,
                   float amplitude,
                   float spread);

/**
 * @brief Draw text with a sweeping shadow band and a hot leading rim.
 *
 * The inverse of AddTextShimmer: a soft darkening crosses the text with a thin white-hot
 * rim just ahead of it. Darkening always has headroom, so this also reads on bright tier
 * palettes.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param highlight Rim color (usually tier highlight); it adopts the fill's alpha.
 * @param phase01 Band position in [0, 1) (wraps).
 * @param bandWidth01 Band half-width as a fraction of text width; scaled by 0.8.
 * @param strength01 Overall effect strength in [0, 1].
 */
void AddTextEclipse(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 baseL,
                    ImU32 baseR,
                    ImU32 highlight,
                    float phase01,
                    float bandWidth01,
                    float strength01);

/**
 * @brief Draw text with a two-beat heartbeat glow.
 *
 * One beat, a softer second beat, then a long rest. The glow blooms from the text center
 * outward, and the rest state sits slightly shaded so both beats have swing to work with.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param highlight Bright pole of the beat (usually tier highlight); it adopts the
 *                  fill's alpha.
 * @param rateHz Heartbeat cycle frequency in Hz; floored at 0.05.
 * @param amplitude Swing depth in [0, 1].
 */
void AddTextPulse(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 baseL,
                  ImU32 baseR,
                  ImU32 highlight,
                  float rateHz,
                  float amplitude);

/**
 * @brief Draw text with rare crackling arc sweeps.
 *
 * Each cycle a jittered white-hot front crosses the text left to right during the first
 * 38% of the cycle; between strikes the face rests calm with faint per-glyph static.
 * Stateless: the pattern comes from a hash and the clock only.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param highlight Bright pole of the strike (usually tier highlight); it adopts the
 *                  fill's alpha.
 * @param rateHz Strike cycle frequency in Hz, floored at 0.02 (0.18 = a strike every
 *               ~5.5 s, of which the first ~2.1 s is the sweep).
 * @param intensity Strike brightness in [0, 1].
 */
void AddTextElectric(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 baseL,
                     ImU32 baseR,
                     ImU32 highlight,
                     float rateHz,
                     float intensity);

/**
 * @brief Draw a static top-edge shine over rendered text.
 *
 * The function brightens vertices
 * in approximately the top 45 percent of the full text
 * bounding box. It measures the band across
 * the complete string, not each glyph. Short glyphs
 * can sit below the band and receive no shine.
 * The call site uses alpha-over compositing and
 * does not enable additive blending.
 *
 * @param
 * list        ImGui draw list that contains the rendered text.
 * @param font        Font used to
 * render the text.
 * @param size        Font size, in pixels.
 * @param pos         Top-left text
 * position.
 * @param text        Null-terminated UTF-8 text.
 * @param intensity   Strength from
 * zero through one. The function scales it by 0.40,
 *                    attenuates it near the
 * horizontal edges, and shapes it with
 *                    `pow(x, 1.35)`.
 * @param falloff
 * Vertical falloff control. The exponent is
 *                    `max(0.5, falloff) * 3.4`.
 *
 * @param shineColor  Highlight tint.
 */
void AddTextShineOverlay(ImDrawList* list,
                         ImFont* font,
                         float size,
                         const ImVec2& pos,
                         const char* text,
                         float intensity,
                         float falloff,
                         ImU32 shineColor);

/**
 * @brief Draw a soft multi-layer bloom behind text.
 *
 * Offset copies of the text at several radii, each with its own alpha multiplier, give a
 * smooth falloff over up to three concentric layers:
 *
 * - **Outer** (1.5x radius, lowest alpha): wide ambient glow
 * - **Middle** (1.0x radius, medium alpha): primary bloom
 * - **Inner** (0.6x radius, highest alpha): bright core halo
 *
 * `samples` selects quality, not a copy count. It is quantized to three levels, so any
 * value above 8 gives the same output:
 *
 * | samples   | Layers drawn         | Copies per layer | AddText calls |
 * |-----------|----------------------|------------------|---------------|
 * | 4 or less | Outer                | 4                | 4             |
 * | 5 to 8    | Outer, Middle        | 8                | 16            |
 * | above 8   | Outer, Middle, Inner | 8                | 24            |
 *
 * The per-copy alpha is solved for additive accumulation, so the copies of one layer sum
 * to that layer's target peak. The call sites push additive blending around this call.
 * Under alpha-over blending the result is dimmer than intended.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param glowColor Glow color (alpha will be modulated).
 * @param radius Glow spread radius in pixels.
 * @param intensity Glow brightness [0, 1].
 * @param samples Quality selector; only the ranges <= 4, 5-8 and > 8 differ.
 *
 * @pre Should be called **before** drawing the main text.
 *
 * @note No-op when `list`, @p font or @p text is null, when the string is empty, when
 *       @p radius is 0 or less, when @p intensity is 0.01 or less, or when @p glowColor's
 *       alpha is below 5. A layer whose solved per-copy alpha rounds below 1 is skipped.
 */
void AddTextGlow(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 glowColor,
                 float radius,
                 float intensity,
                 int samples);

/**
 * @brief Draw a soft, directional drop-shadow behind text, with no background plate.
 *
 * Distributes `samples` offset copies of the text within a disc of radius `softness`,
 * centered at `pos + dir * distance`: one central tap plus a golden-angle spiral, so the
 * disc is filled evenly rather than left as a hollow ring. Each copy contributes a fraction
 * of the target opacity, so the accumulation darkens toward the center and feathers at the
 * edges. This gives depth a single hard offset cannot, while staying inside the text layer.
 * The per-sample alpha is solved for alpha-over compositing, unlike AddTextGlow.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position of the (un-offset) text.
 * @param text Null-terminated UTF-8 string to render.
 * @param shadowColor Shadow tint; RGB is used as-is, alpha is the per-frame fade.
 * @param dirX Shadow cast direction X (cos of the angle; +x = right).
 * @param dirY Shadow cast direction Y (sin of the angle; +y = down).
 * @param distance Offset distance along the direction in pixels.
 * @param softness Feather/blur radius of the disc in pixels.
 * @param opacity Master opacity multiplier [0, 1] applied to the shadow alpha.
 * @param samples Number of feather samples (1-24, clamped); higher is smoother.
 *
 * @pre Should be called **before** drawing the outline and main text.
 *
 * @note Collapses to a single crisp offset copy when @p samples <= 1 or
 *       @p softness <= 0.01; in that collapsed path the copy is dropped as well when its
 *       alpha rounds below 3. No-op when @p opacity <= 0.01, when @p shadowColor's alpha
 *       is below 4, or when the feathered path's solved per-sample alpha rounds below 1.
 */
void AddTextSoftShadow(ImDrawList* list,
                       ImFont* font,
                       float size,
                       const ImVec2& pos,
                       const char* text,
                       ImU32 shadowColor,
                       float dirX,
                       float dirY,
                       float distance,
                       float softness,
                       float opacity,
                       int samples);

/**
 * @struct ParticleAuraParams
 * @brief Parameters for `DrawParticleAura`.
 */
struct ParticleAuraParams
{
    ImDrawList* list;  ///< ImGui draw list to render to
    ImVec2 center;     ///< Center of particle region
    float radiusX;     ///< Horizontal spread radius in pixels
    float radiusY;     ///< Vertical spread radius in pixels
    ImU32 color;       ///< Particle base color
    /// Per-style particle opacity ceiling [0, 1]. An aura configured at or below 0.05
    /// renders nothing at all. When enabledStyleCount > 1 this value is divided by
    /// 1 + 0.05 * (enabledStyleCount - 1), so it is not the absolute maximum.
    float alpha;
    Settings::ParticleStyle style;  ///< Particle visual style
    int particleCount;              ///< Particle count; 0 or less renders nothing
    float particleSize;             ///< Base particle size in pixels
    float speed;                    ///< Animation speed multiplier applied to time
    float time;                     ///< Animation clock in seconds
    int styleIndex = 0;             ///< Index among enabled styles; picks this style's band
    int enabledStyleCount = 1;      ///< Total enabled particle styles; divides alpha
    /// Use texture sprites instead of procedural shapes. Procedural shapes are used anyway
    /// when ParticleTextures is not initialized or the style has no texture.
    bool useParticleTextures = true;
    /// Blend mode: 0 = Additive, 1 = Screen, 2 = Alpha. A sprite-expansion style whose
    /// motion recipe pins a blend mode overrides this value.
    int blendMode = 0;
    ImU32 colorSecondary = 0;     ///< Optional second gradient color (0 = use primary only)
    float depthStrength = .7f;    ///< Scales the 3D depth read (size/alpha/parallax)
    float colorWarmth = .5f;      ///< Scales warm/cool depth temperature mix + apex pulse
    float glowStrength = .35f;    ///< Additive backlight halo alpha multiplier (0 disables)
    float glowSize = 2.2f;        ///< Halo radius as a multiple of the crisp sprite size
    float shineThreshold = .84f;  ///< Sine threshold for the rare specular glint
};

/**
 * @brief Draw an aura of animated particles around a text region.
 *
 * Ten styles carry a dedicated renderer with its own distribution and motion model:
 *
 * | Style         | Motion                                                       |
 * |---------------|--------------------------------------------------------------|
 * | Firefly       | Tilted orbit + incommensurate wander, blinking               |
 * | Dust          | Slow tilted orbit + tiny drift, twinkling                    |
 * | Mote          | Tilted orbit + Brownian wander, breathing glow               |
 * | Wisp          | Tilted orbit + serpentine weave, tangent echo trail          |
 * | Spark         | Tilted orbit + periodic outward ember flare, tangent trail   |
 * | Leaf          | Tilted orbit + in/out radius drift, fluttering tumble        |
 * | CherryBlossom | Tilted orbit + gentle bob, slow continuous spin              |
 * | Snow          | Broad, slow orbit with gentle flake bob (dormant, see below) |
 * | Smoke         | Rises, expands, dissolves (non-orbiting)                     |
 * | Aurora        | Horizontal flowing light curtain (non-orbiting)              |
 *
 * The eight orbiting entries share a tilted elliptical ring (SampleOrbit): each particle
 * circles the plate at its own radius, elevation band, speed and phase, with front-of-ring
 * particles drawn larger/brighter and back-of-ring particles smaller/dimmer for a 3D read.
 * Smoke and Aurora are the deliberate non-orbiting exceptions. Snow now reaches the ring
 * through the shared Orbit archetype below; its dedicated renderer is kept only as the
 * fallback for a missing motion recipe and does not run.
 *
 * Per-particle variation comes from independent deterministic hash streams, so motion is
 * organic yet stable across frames. When several styles render together each occupies its
 * own radial band; a style rendering alone may fill the whole region.
 *
 * The remaining sprite styles share five parameterized motion archetypes instead of bespoke
 * renderers. A sixth archetype, also named Zap, is declared but no style selects it - the
 * Zap style itself uses Orbit. See StyleMotionSpec in TextEffectsParticle.cpp.
 *
 * | Archetype | Styles                                                       |
 * |-----------|--------------------------------------------------------------|
 * | Orbit     | Arcane, Enchant, Gem, Hex, Curse, Void, Vortex, Fairy, Runes |
 * | Orbit     | Bat (swoops), Butterfly (bob), Constellation + Moon, Planet  |
 * | Orbit     | Pollen (wide sway), Pixiedust (sprinkle), Ash (flutter),     |
 * | Orbit     | Zap, Snow, Coin (spinning), Ink                              |
 * | Rise      | Bubble (pops at top), Heart, Soul, Steam, Zzz, Ember         |
 * | Fall      | Confetti                                                     |
 * | Flow      | Wind (mid band), Fog + Sand (lower band)                     |
 * | Twinkle   | Glitter (anchored sparkle field)                             |
 *
 * A sprite whose texture is a horizontal flipbook strip animates on a stateless clock
 * (per-particle phase, speed-coupled cadence). The frame count is detected from the image
 * dimensions - width divided by height, when the width is an exact multiple of the height
 * - so it is a property of the art, not a fixed number, and there is no filename
 * convention that marks a strip. A single-frame sprite holds frame 0.
 *
 * @param params Particle aura parameters.
 *
 * @note Returns without drawing when params.list is null, when params.alpha is 0.05 or
 *       less, or when params.particleCount is 0 or less. A style with neither a dedicated
 *       renderer nor a motion recipe falls back to the firefly wanderer.
 *
 * @see ParticleAuraParams, Settings::ParticleStyle, Settings::Particle
 */
void DrawParticleAura(const ParticleAuraParams& params);
}  // namespace TextEffects
