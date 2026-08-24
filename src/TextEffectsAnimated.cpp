// Time-driven text effects: shimmer, ember, breathe, mote, wander, eclipse, pulse and
// electric, plus the static top-edge shine overlay.
//
// Render thread only. Every function here runs inside the ImGui draw pass, reached through
// ApplyTextEffect in RendererEffects.cpp, and touches no game state.
//
// Shared conventions (the same ones TextEffectsComplex.cpp lists):
//  - TextVertexSetup::Begin draws the string in white and records the vertex range plus one
//    bounding box for the whole string. The normalized coordinates each effect uses (t or nx
//    across, v or ny down) are measured inside that box, so v = 0 is the top of the tallest
//    glyph, not the top of the glyph a vertex belongs to.
//  - Color is written per vertex, and ImGui emits 4 vertices per drawn glyph, so each field
//    is evaluated at glyph-quad corners and interpolated across the quad. Blank and clipped
//    characters emit no vertices, which is why a per-glyph index is (i - vtxStart) / 4 over
//    drawn glyphs, not over string positions.
//  - Every effect is stateless: the animation comes from ImGui::GetTime(), the caller's
//    phase01, and hashes. Nothing is carried between frames.
//  - A bright pole taken from the separate highlight color passes through WithAlphaFrom,
//    because that color carries the low effectAlpha. Poles built with DeepShade or
//    HotHighlight from baseL/baseR already keep the fill alpha.

#include "TextEffectsInternal.hpp"

namespace TextEffects
{

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
                    float strength01)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // The band supplies its own contrast: the resting face sits below the base
    // color and the band core pushes past the highlight toward white. A smaller
    // amplitude cap stacks with the strength band and disappears.
    static constexpr float kAmplitudeCap = .85f;
    static constexpr float kFaceShade = .22f;  // resting dip below base
    // bandHalf is a half-width in normalized text width, so the smoothstep core
    // spans 2 * .85 * bandWidth01: 17% to 37% of the string at the shipped
    // bandWidth01 of .10 to .22. The .01 floor keeps a degenerate width visible.
    const float bandHalf = (std::max)(bandWidth01 * .85f, .01f);

    const ImU32 deepL = DeepShade(baseL, .60f, 1.25f);
    const ImU32 deepR = DeepShade(baseR, .60f, 1.25f);
    // Adopt the fill's alpha: the raw highlight carries the low effectAlpha,
    // which would make the band transparent where it brightens.
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .85f), baseL);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        // Wrap-around distance, so the band exits right and re-enters left
        const float d =
            (std::min)(std::abs(t - phase01),
                       (std::min)(std::abs(t - phase01 + 1.0f), std::abs(t - phase01 - 1.0f)));

        // Primary band, quintic falloff
        float h = (d < bandHalf) ? 1.0f - SmoothStep(d / bandHalf) : .0f;

        // Bias toward the top of the string box; v spans the whole string, not one glyph
        float verticalBoost = 1.0f + (1.0f - v) * .12f;
        h = h * strength01 * verticalBoost;

        // Secondary halo around the band
        float glow = std::exp(-d * d * 6.0f) * .18f * strength01;

        // Tertiary wide ambient glow
        float ambient = std::exp(-d * d * 2.0f) * .06f * strength01;

        h = Saturate((h + glow + ambient) * kAmplitudeCap);

        // Resting face dips toward the deep shade and is released as the band
        // approaches, so the sweep carries a real luminance swing.
        ImU32 face = LerpColorU32(base, deep, kFaceShade * strength01 * (1.0f - h));
        list->VtxBuffer[i].col = LerpColorU32(face, hot, h);
    }
}

void AddTextEmber(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 colA,
                  ImU32 colB,
                  float speed,
                  float intensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime() * speed;

    // Both poles are needed: charcoal troughs below the base color and molten
    // peaks above it. A small warm tint under a low cap moves pixels only a few
    // percent and is invisible.
    static constexpr float kAmplitudeCap = .70f;

    // Charcoal pole: deep, warm-shifted shade of the base pair.
    const ImU32 charcoalL = DeepShade(colA, .45f, 1.45f);
    const ImU32 charcoalR = DeepShade(colB, .45f, 1.45f);
    // Molten pole: fixed amber-white, independent of the tier hue.
    const int aA = (colA >> IM_COL32_A_SHIFT) & 0xFF;
    const ImU32 molten = IM_COL32(255, 214, 140, aA);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        ImU32 base = LerpColorU32(colA, colB, nx);
        ImU32 charcoal = LerpColorU32(charcoalL, charcoalR, nx);

        // Three sine layers for the ember flicker
        float n1 = std::sin(nx * 5.0f + time * 1.2f + ny * 3.0f) * .5f + .5f;
        float n2 = std::sin(nx * 8.0f - time * 2.0f + ny * 5.0f) * .5f + .5f;
        float n3 = std::sin(nx * 12.0f + time * 3.5f - ny * 2.0f) * .5f + .5f;

        // Weighted mix
        float ember = n1 * .5f + n2 * .3f + n3 * .2f;

        // Vertical heat gradient over the whole string box: the bottom runs hotter.
        float heatGrad = .80f + .20f * ny;

        // Per-character flicker, added on top of the three sine layers above,
        // which already run at 1.2x, 2.0x and 3.5x. At 1.5x and the shipped speeds
        // of .28 to .35 this term runs at .07 to .08 Hz: a breathing coal, not a strobe.
        float charFlicker = std::sin(time * 1.5f + nx * 20.0f) * .5f + .5f;
        charFlicker = charFlicker * charFlicker * .12f;

        // Signed heat field: below the midline the glyph cools toward charcoal,
        // above it heats toward molten. The midline sits at .32 because at the
        // low end of the strength band the field never crosses .45, which would
        // leave whole tiers with no molten phase at all.
        float heat = Saturate(ember * heatGrad + charFlicker) * intensity;
        float signedHeat = (heat - .32f) * 2.0f * kAmplitudeCap;

        if (signedHeat >= .0f)
        {
            float up = SmoothStep(Saturate(signedHeat));
            list->VtxBuffer[i].col = LerpColorU32(base, molten, up);
        }
        else
        {
            float down = SmoothStep(Saturate(-signedHeat));
            list->VtxBuffer[i].col = LerpColorU32(base, charcoal, down * .8f);
        }
    }
}

// Golden-ratio scrambling hash for deterministic [0, 1) values from an integer seed.
// Used by Mote, Wander and Electric to pick a stable glyph, per-glyph phase offsets
// and per-cycle jitter, without the FBM / Hash helpers in TextEffectsComplex.cpp.
static inline float SubtleHash01(size_t seed)
{
    seed ^= seed >> 16;
    seed *= 0x9e3779b97f4a7c15ULL;
    seed ^= seed >> 13;
    seed *= 0xc2b2ae35ULL;
    seed ^= seed >> 16;
    return static_cast<float>(seed & 0xFFFFFF) / 16777216.0f;
}

void AddTextBreathe(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 baseL,
                    ImU32 baseR,
                    float speed,
                    float amplitude)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Uniform swell across the whole text. Bright bases have no upward headroom,
    // so the cycle dips into a deep shade and lifts back through the base to a
    // slight overshoot: an asymmetric swing, a little deeper on the dip than on
    // the lift, that reads on any palette.
    const float time = (float)ImGui::GetTime();
    const float wave = std::sin(TWO_PI * time * speed);

    const ImU32 deepL = DeepShade(baseL, .55f, 1.30f);
    const ImU32 deepR = DeepShade(baseR, .55f, 1.30f);
    const ImU32 hotL = HotHighlight(baseL, .55f);
    const ImU32 hotR = HotHighlight(baseR, .55f);
    // amplitude [0,1] is the fraction of the full deep-to-hot range used. The dip
    // and lift gains stay near-balanced: a mostly-darkening cycle reads as nothing
    // happening, and the lift toward the hot pole is what registers as a swell.
    const float dip = Saturate(amplitude * 2.0f) * Saturate(-wave);
    const float lift = Saturate(amplitude * 1.7f) * Saturate(wave);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        if (dip > .0f)
        {
            ImU32 deep = LerpColorU32(deepL, deepR, t);
            list->VtxBuffer[i].col = LerpColorU32(base, deep, SmoothStep(dip));
        }
        else
        {
            ImU32 hot = LerpColorU32(hotL, hotR, t);
            list->VtxBuffer[i].col = LerpColorU32(base, hot, SmoothStep(lift));
        }
    }
}

void AddTextMote(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 baseL,
                 ImU32 baseR,
                 ImU32 moteColor,
                 float period,
                 float peakAlpha)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // One mote per period: periodIdx picks a stable glyph, phase drives a single
    // bell envelope. The mote must land on a glyph quad. A position hashed over
    // the whole bounding box would often fall in a space, a gap, or the empty
    // band above the x-height and recolor nothing.
    const float time = (float)ImGui::GetTime();
    const float safePeriod = (std::max)(period, .1f);
    const float periodIdx = std::floor(time / safePeriod);
    const float phase = Saturate((time / safePeriod) - periodIdx);

    // Single bell: peaks at 1.0 mid-period and spans the whole period, so the
    // glint holds above half peak for about 70% of it, roughly 2 s at the shipped
    // period of 3 s. It fades in and out; it never pops.
    const float envelope = 4.0f * phase * (1.0f - phase);

    // Land on ink: pick a glyph, center the mote on its quad (4 verts/glyph).
    // Blank and clipped characters emit no quad, so charCount and litChar count
    // drawn glyphs, not string positions.
    const int charCount = (s.vtxEnd - s.vtxStart) / 4;
    if (charCount <= 0)
    {
        return;
    }
    const size_t seed = static_cast<size_t>(periodIdx);
    const int litChar =
        static_cast<int>(SubtleHash01(seed * 0x1f1f1f1fULL + 1ULL) * (float)charCount) % charCount;
    float moteX = .0f, moteY = .0f;
    for (int k = 0; k < 4; ++k)
    {
        const ImVec2& q = list->VtxBuffer[s.vtxStart + litChar * 4 + k].pos;
        moteX += q.x * .25f;
        moteY += q.y * .25f;
    }

    // Radius scaled to text height so the glint covers roughly one glyph and a
    // bit more, wide enough to be visible when it fires.
    const float moteRadius = (std::max)(s.height() * .55f, 1.0f);
    const float radius2 = moteRadius * moteRadius;

    // The glint must stay opaque, so it adopts the fill's alpha rather than the
    // highlight's low effectAlpha, which would dim the glyph as it brightens.
    const ImU32 glint = WithAlphaFrom(moteColor, baseL);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float t = s.normalizedX(p.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);

        const float dx = p.x - moteX;
        const float dy = p.y - moteY;
        const float d2 = dx * dx + dy * dy;
        // Gaussian falloff; exp(-(d^2) / (r^2 / 3)) keeps the mote tight.
        const float falloff = std::exp(-d2 * 3.0f / radius2);

        const float intensity = Saturate(peakAlpha * envelope * falloff);
        list->VtxBuffer[i].col = LerpColorU32(base, glint, intensity);
    }
}

void AddTextWander(ImDrawList* list,
                   ImFont* font,
                   float size,
                   const ImVec2& pos,
                   const char* text,
                   ImU32 baseL,
                   ImU32 baseR,
                   float speed,
                   float amplitude,
                   float spread)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Per-glyph cycle: each glyph gets its own phase, so no single point pulls
    // focus. Four vertices per glyph is the ImGui glyph-quad convention. Each
    // glyph swings around its base color, dipping into a deep shade on one half
    // of its cycle and lifting toward a hot highlight on the other; a dip-only
    // swing never brightens and reads as flat text. spread scales the per-glyph
    // offset: 0 puts every glyph in phase, so the string swells as one the way
    // AddTextBreathe does, and 1 spreads the glyphs over a full turn.
    const float time = (float)ImGui::GetTime();
    const float clampedSpread = (std::max)(spread, .0f);

    const ImU32 deepL = DeepShade(baseL, .58f, 1.30f);
    const ImU32 deepR = DeepShade(baseR, .58f, 1.30f);
    const ImU32 hotL = HotHighlight(baseL, .50f);
    const ImU32 hotR = HotHighlight(baseR, .50f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);

        const int charIdx = (i - s.vtxStart) / 4;
        const float phaseOffset =
            SubtleHash01(static_cast<size_t>(charIdx) + 1ULL) * TWO_PI * clampedSpread;

        // Signed swing per glyph; amplitude scales how far it travels.
        const float wave = std::sin(TWO_PI * time * speed + phaseOffset);
        if (wave >= .0f)
        {
            ImU32 deep = LerpColorU32(deepL, deepR, t);
            const float dip = wave * Saturate(amplitude * 2.2f);
            list->VtxBuffer[i].col = LerpColorU32(base, deep, SmoothStep(dip));
        }
        else
        {
            ImU32 hot = LerpColorU32(hotL, hotR, t);
            const float lift = -wave * Saturate(amplitude * 1.8f);
            list->VtxBuffer[i].col = LerpColorU32(base, hot, SmoothStep(lift));
        }
    }
}

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
                    float strength01)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Inverse shimmer: a soft shadow crosses the text with a hot rim on its
    // leading edge. Darkening always has headroom on bright bases, so this
    // reads on every tier palette.
    const float bandHalf = (std::max)(bandWidth01 * .8f, .01f);
    const ImU32 deepL = DeepShade(baseL, .40f, 1.40f);
    const ImU32 deepR = DeepShade(baseR, .40f, 1.40f);
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .90f), baseL);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        // Signed wrap-around offset from the band center in [-.5, .5).
        float d = t - phase01;
        d -= std::floor(d + .5f);

        // Shadow body: smooth dark bell over the band.
        float shadow = std::exp(-(d * d) / (bandHalf * bandHalf * .5f));

        // Hot rim on the leading edge only, just ahead of the shadow. The caller
        // advances phase01 with time, so the band travels toward +t and a vertex
        // with d > 0 is one the shadow has not reached yet.
        float rimD = d - bandHalf * 1.1f;
        float rim = std::exp(-(rimD * rimD) / (bandHalf * bandHalf * .06f));

        ImU32 shadowed = LerpColorU32(base, deep, Saturate(shadow * .70f * strength01));
        list->VtxBuffer[i].col = LerpColorU32(shadowed, hot, Saturate(rim * .85f * strength01));
    }
}

void AddTextPulse(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 baseL,
                  ImU32 baseR,
                  ImU32 highlight,
                  float rateHz,
                  float amplitude)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Two-beat cycle: one beat, a softer second, then a long rest. The glow
    // blooms from the center of the text outward and the rest state sits
    // slightly shaded, so both beats carry a real swing.
    //
    // envelope over one cycle, which lasts 1 / rateHz seconds:
    //
    //   phase   0     .12       .34                                1
    //           |______|_________|_________________________________|
    //                beat 1   beat 2 (55%)      rest
    //
    // The envelope is above ~0.1 only up to phase .47, so about half of every
    // cycle is rest. That gap is what makes the pair of beats readable.
    const float time = (float)ImGui::GetTime();
    const float safeRate = (std::max)(rateHz, .05f);
    const float ph = Frac(time * safeRate);

    auto beat = [](float ph, float center, float width)
    {
        const float d = (ph - center) / width;
        return std::exp(-d * d);
    };
    // Beat widths .09/.10 are in cycle phase, so their duration scales with
    // rateHz: at the shipped .15-.16 Hz each beat is about 1 s wide at half
    // maximum. Halving the widths keeps the same cadence but cuts that to about
    // .5 s, which reads as staccato blinking instead of a swell and release.
    const float envelope = Saturate(beat(ph, .12f, .09f) + .55f * beat(ph, .34f, .10f));

    const ImU32 deepL = DeepShade(baseL, .62f, 1.25f);
    const ImU32 deepR = DeepShade(baseR, .62f, 1.25f);
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .70f), baseL);
    const float amp = Saturate(amplitude);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        // Horizontal falloff from the text center: the beat keeps full strength
        // mid-string and 55% at the two ends, so it reads as blooming outward.
        const float centerDist = std::abs(t - .5f) * 2.0f;
        const float bloom = 1.0f - centerDist * .45f;

        ImU32 rest = LerpColorU32(base, deep, .18f * amp * (1.0f - envelope));
        list->VtxBuffer[i].col = LerpColorU32(rest, hot, Saturate(envelope * bloom * amp));
    }
}

void AddTextElectric(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 baseL,
                     ImU32 baseR,
                     ImU32 highlight,
                     float rateHz,
                     float intensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Every cycle a sharp bright front sweeps the text left to right over the
    // first 38% of the period, jittered by a per-cycle hash so no two strikes
    // trace the same path. The rest of the cycle holds a faintly shaded face
    // with faint static. Stateless: the pattern comes from the hash and the
    // clock only.
    const float time = (float)ImGui::GetTime();
    const float safeRate = (std::max)(rateHz, .02f);
    const float cyc = time * safeRate;
    const float cycIdx = std::floor(cyc);
    const float ph = cyc - cycIdx;

    const ImU32 deepL = DeepShade(baseL, .60f, 1.25f);
    const ImU32 deepR = DeepShade(baseR, .60f, 1.25f);
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .95f), baseL);

    // The strike occupies the first 38% of the cycle: a deliberately slow sweep.
    const bool striking = ph < .38f;
    const float frontX = striking ? (ph / .38f) * 1.2f - .1f : -1.0f;

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        // Faint per-glyph static. It is applied on every frame but re-rolled only
        // once per cycle, so between strikes the face holds still and changes only
        // when the cycle index advances.
        const int charIdx = (i - s.vtxStart) / 4;
        const float staticFlick = SubtleHash01(static_cast<size_t>(charIdx) * 31ULL +
                                               static_cast<size_t>(cycIdx) * 131ULL) *
                                  .10f;
        ImU32 col = LerpColorU32(base, deep, .10f + staticFlick);

        if (striking)
        {
            // Jittered vertical path: the front is not a straight bar.
            const float jitter = (SubtleHash01(static_cast<size_t>(cycIdx) * 977ULL +
                                               static_cast<size_t>(v * 7.0f) * 53ULL) -
                                  .5f) *
                                 .08f;
            const float d = t - (frontX + jitter);
            // White-hot core with a warm afterglow trailing behind. The 300
            // coefficient puts the core half-width at 1/sqrt(300), about 6% of the
            // string, so a glyph glows for roughly .3 s as the front passes at the
            // shipped .12-.13 Hz. A larger coefficient narrows that to a flash.
            const float core = std::exp(-d * d * 300.0f);
            const float tail = (d < .0f) ? std::exp(d * 9.0f) * .35f : .0f;
            col = LerpColorU32(col, hot, Saturate((core + tail) * intensity));
        }

        list->VtxBuffer[i].col = col;
    }
}

void AddTextShineOverlay(ImDrawList* list,
                         ImFont* font,
                         float size,
                         const ImVec2& pos,
                         const char* text,
                         float intensity,
                         float falloff,
                         ImU32 shineColor)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // An overlay pass, not a recolor: this draws its own copy of the string and
    // replaces every vertex with shineColor, so it must run after the fill. RGB
    // is fixed and only alpha is modulated per vertex. The call site composites
    // the copy alpha-over; it does not push additive blending.
    const int sr = (shineColor >> IM_COL32_R_SHIFT) & 0xFF;
    const int sg = (shineColor >> IM_COL32_G_SHIFT) & 0xFF;
    const int sb = (shineColor >> IM_COL32_B_SHIFT) & 0xFF;
    const float shapedFalloff = (std::max)(0.5f, falloff) * 3.4f;
    // With the shipped INI a scale of .10 gives ~0.5/255 alpha, which rounds to
    // zero; .40 puts a visible rim on the cap height.
    const float intensityScale = .40f;

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);

        // Keep the shine tight to the top edge instead of washing over the entire
        // glyph face. topBand reaches 0 at v = 1/2.2, so only the top ~45% of the
        // string box is lit, and a short glyph, whose quad starts further down that
        // box, takes less of the band than a tall one.
        const float topBand = Saturate(1.0f - v * 2.2f);
        float shine = std::pow(topBand, shapedFalloff) * intensity * intensityScale;

        // Slight horizontal variation
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        float h = 2.0f * t - 1.0f;
        float hMod = 1.0f - .45f * h * h;
        shine *= hMod;
        shine = std::pow(Saturate(shine), 1.35f);

        const int a = (int)std::clamp(shine * 255.0f, .0f, 255.0f);
        list->VtxBuffer[i].col = IM_COL32(sr, sg, sb, a);
    }
}

}  // namespace TextEffects
