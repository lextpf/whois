// Unit tests for glyph utility functions using Google Test.
//
// Tests core math and color manipulation functions that are independent of the
// game runtime.
//
// The harness links no game code, so most helpers below are MIRRORS: the
// production logic is copied here, not included. A mirror goes stale silently,
// so change the copy in the same edit that changes the production function.
// NameFit.hpp and RenderConstants.hpp are runtime-free and are included for
// real.

#include <gtest/gtest.h>

#include "NameFit.hpp"
#include "RasterQuality.hpp"
#include "RenderConstants.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Re-implement functions under test: Saturate and SmoothStep mirror the
// constexpr helpers in src/TextEffects.hpp; LerpColorU32 mirrors
// src/TextEffectsCore.cpp.
// ============================================================================

float Saturate(float x)
{
    return std::clamp(x, 0.0f, 1.0f);
}

float SmoothStep(float t)
{
    t = Saturate(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// ImGui color format macros
#define IM_COL32_R_SHIFT 0
#define IM_COL32_G_SHIFT 8
#define IM_COL32_B_SHIFT 16
#define IM_COL32_A_SHIFT 24
#define IM_COL32(R, G, B, A)                                                             \
    (((unsigned int)(A) << IM_COL32_A_SHIFT) | ((unsigned int)(B) << IM_COL32_B_SHIFT) | \
     ((unsigned int)(G) << IM_COL32_G_SHIFT) | ((unsigned int)(R) << IM_COL32_R_SHIFT))

using ImU32 = unsigned int;

ImU32 LerpColorU32(ImU32 a, ImU32 b, float t)
{
    t = Saturate(t);
    const int ar = (a >> IM_COL32_R_SHIFT) & 0xFF;
    const int ag = (a >> IM_COL32_G_SHIFT) & 0xFF;
    const int ab = (a >> IM_COL32_B_SHIFT) & 0xFF;
    const int aa = (a >> IM_COL32_A_SHIFT) & 0xFF;
    const int br = (b >> IM_COL32_R_SHIFT) & 0xFF;
    const int bg = (b >> IM_COL32_G_SHIFT) & 0xFF;
    const int bb = (b >> IM_COL32_B_SHIFT) & 0xFF;
    const int ba = (b >> IM_COL32_A_SHIFT) & 0xFF;
    const int rr = (int)(ar + (br - ar) * t + 0.5f);
    const int rg = (int)(ag + (bg - ag) * t + 0.5f);
    const int rb = (int)(ab + (bb - ab) * t + 0.5f);
    const int ra = (int)(aa + (ba - aa) * t + 0.5f);
    return IM_COL32(rr, rg, rb, ra);
}

static inline float Frac(float x)
{
    return x - std::floor(x);
}

float LerpRange(float minVal, float maxVal, float t)
{
    t = Saturate(t);
    return minVal + (maxVal - minVal) * t;
}

bool IsPrimaryTextBody(int r, int g, int b, int a, int maxBatchAlpha)
{
    const float lum = (r * .299f + g * .587f + b * .114f) / 255.0f;
    const int maxCh = std::max({r, g, b});
    const int bodyAlphaThreshold = std::max(8, (maxBatchAlpha * 5 + 5) / 6);
    return lum >= .22f && maxCh >= 80 && a >= bodyAlphaThreshold;
}

struct ImVec4
{
    float x, y, z, w;
};

ImVec4 HSVtoRGB(float h, float s, float v, float a)
{
    h = Frac(h);
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(Frac(h * 6.0f) * 2.0f - 1.0f));
    const float m = v - c;
    float r = 0, g = 0, b = 0;
    const int i = (int)std::floor(h * 6.0f);
    switch (i % 6)
    {
        case 0:
            r = c;
            g = x;
            b = 0;
            break;
        case 1:
            r = x;
            g = c;
            b = 0;
            break;
        case 2:
            r = 0;
            g = c;
            b = x;
            break;
        case 3:
            r = 0;
            g = x;
            b = c;
            break;
        case 4:
            r = x;
            g = 0;
            b = c;
            break;
        case 5:
            r = c;
            g = 0;
            b = x;
            break;
    }
    return {r + m, g + m, b + m, a};
}

// ============================================================================
// Tests: fixed crisp-layer raster quality
// ============================================================================

TEST(RasterQualityTest, BadgeTextureSizesArePowersOfTwo)
{
    EXPECT_EQ(RasterQuality::STATUS_ICON_TEXTURE_SIZE, 256);
    EXPECT_EQ(RasterQuality::RANK_EMBLEM_TEXTURE_SIZE, 512);
    EXPECT_TRUE(RasterQuality::IsPowerOfTwo(RasterQuality::STATUS_ICON_TEXTURE_SIZE));
    EXPECT_TRUE(RasterQuality::IsPowerOfTwo(RasterQuality::RANK_EMBLEM_TEXTURE_SIZE));
}

TEST(RasterQualityTest, GlyphPaddingSupportsFontMipLimit)
{
    EXPECT_EQ(RasterQuality::FONT_GLYPH_PADDING, 8);
    EXPECT_EQ(RasterQuality::FONT_MIP_LIMIT, 3);
    EXPECT_GE(RasterQuality::FONT_GLYPH_PADDING, 1 << RasterQuality::FONT_MIP_LIMIT);
    EXPECT_TRUE(RasterQuality::FontPaddingSupportsMipLimit());
}

// ============================================================================
// Tests: Saturate
// ============================================================================

TEST(SaturateTest, ClampsAboveOne)
{
    EXPECT_FLOAT_EQ(Saturate(1.5f), 1.0f);
    EXPECT_FLOAT_EQ(Saturate(100.0f), 1.0f);
}

TEST(SaturateTest, ClampsBelowZero)
{
    EXPECT_FLOAT_EQ(Saturate(-0.5f), 0.0f);
    EXPECT_FLOAT_EQ(Saturate(-100.0f), 0.0f);
}

TEST(SaturateTest, PreservesValidRange)
{
    EXPECT_FLOAT_EQ(Saturate(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(Saturate(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(Saturate(1.0f), 1.0f);
}

// ============================================================================
// Tests: SmoothStep (quintic)
// ============================================================================

TEST(SmoothStepTest, ReturnsZeroAtZero)
{
    EXPECT_FLOAT_EQ(SmoothStep(0.0f), 0.0f);
}

TEST(SmoothStepTest, ReturnsOneAtOne)
{
    EXPECT_FLOAT_EQ(SmoothStep(1.0f), 1.0f);
}

TEST(SmoothStepTest, ReturnsHalfAtMidpoint)
{
    EXPECT_FLOAT_EQ(SmoothStep(0.5f), 0.5f);
}

TEST(SmoothStepTest, ClampsBelowZero)
{
    EXPECT_FLOAT_EQ(SmoothStep(-1.0f), 0.0f);
}

TEST(SmoothStepTest, ClampsAboveOne)
{
    EXPECT_FLOAT_EQ(SmoothStep(2.0f), 1.0f);
}

TEST(SmoothStepTest, HasZeroDerivativeAtEdges)
{
    // Test that the curve is smooth at boundaries by checking nearby values
    float eps = 0.001f;
    float at0 = SmoothStep(0.0f);
    float near0 = SmoothStep(eps);
    float slope0 = (near0 - at0) / eps;
    EXPECT_LT(slope0, 0.01f);  // Derivative should be ~0 at t=0

    float at1 = SmoothStep(1.0f);
    float near1 = SmoothStep(1.0f - eps);
    float slope1 = (at1 - near1) / eps;
    EXPECT_LT(slope1, 0.01f);  // Derivative should be ~0 at t=1
}

// ============================================================================
// Tests: LerpColorU32
// ============================================================================

TEST(LerpColorTest, ReturnsFirstAtZero)
{
    ImU32 a = IM_COL32(100, 150, 200, 255);
    ImU32 b = IM_COL32(200, 100, 50, 128);
    ImU32 result = LerpColorU32(a, b, 0.0f);
    EXPECT_EQ(result, a);
}

TEST(LerpColorTest, ReturnsSecondAtOne)
{
    ImU32 a = IM_COL32(100, 150, 200, 255);
    ImU32 b = IM_COL32(200, 100, 50, 128);
    ImU32 result = LerpColorU32(a, b, 1.0f);
    EXPECT_EQ(result, b);
}

TEST(LerpColorTest, InterpolatesAtHalf)
{
    ImU32 a = IM_COL32(0, 0, 0, 0);
    ImU32 b = IM_COL32(200, 100, 50, 128);
    ImU32 result = LerpColorU32(a, b, 0.5f);

    int r = (result >> IM_COL32_R_SHIFT) & 0xFF;
    int g = (result >> IM_COL32_G_SHIFT) & 0xFF;
    int bl = (result >> IM_COL32_B_SHIFT) & 0xFF;
    int al = (result >> IM_COL32_A_SHIFT) & 0xFF;

    EXPECT_EQ(r, 100);
    EXPECT_EQ(g, 50);
    EXPECT_EQ(bl, 25);
    EXPECT_EQ(al, 64);
}

TEST(LerpColorTest, ClampsT)
{
    ImU32 a = IM_COL32(100, 100, 100, 255);
    ImU32 b = IM_COL32(200, 200, 200, 255);

    ImU32 below = LerpColorU32(a, b, -1.0f);
    ImU32 above = LerpColorU32(a, b, 2.0f);

    EXPECT_EQ(below, a);
    EXPECT_EQ(above, b);
}

// ============================================================================
// Tests: HSVtoRGB
// ============================================================================

TEST(HSVtoRGBTest, RedAtHueZero)
{
    ImVec4 rgb = HSVtoRGB(0.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 1.0f, 0.01f);  // R = 1
    EXPECT_NEAR(rgb.y, 0.0f, 0.01f);  // G = 0
    EXPECT_NEAR(rgb.z, 0.0f, 0.01f);  // B = 0
}

TEST(HSVtoRGBTest, GreenAtHueThird)
{
    ImVec4 rgb = HSVtoRGB(1.0f / 3.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 0.0f, 0.01f);  // R = 0
    EXPECT_NEAR(rgb.y, 1.0f, 0.01f);  // G = 1
    EXPECT_NEAR(rgb.z, 0.0f, 0.01f);  // B = 0
}

TEST(HSVtoRGBTest, BlueAtHueTwoThirds)
{
    ImVec4 rgb = HSVtoRGB(2.0f / 3.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 0.0f, 0.01f);  // R = 0
    EXPECT_NEAR(rgb.y, 0.0f, 0.01f);  // G = 0
    EXPECT_NEAR(rgb.z, 1.0f, 0.01f);  // B = 1
}

TEST(HSVtoRGBTest, WhiteAtZeroSaturation)
{
    ImVec4 rgb = HSVtoRGB(0.5f, 0.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 1.0f, 0.01f);
    EXPECT_NEAR(rgb.y, 1.0f, 0.01f);
    EXPECT_NEAR(rgb.z, 1.0f, 0.01f);
}

TEST(HSVtoRGBTest, BlackAtZeroValue)
{
    ImVec4 rgb = HSVtoRGB(0.5f, 1.0f, 0.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 0.0f, 0.01f);
    EXPECT_NEAR(rgb.y, 0.0f, 0.01f);
    EXPECT_NEAR(rgb.z, 0.0f, 0.01f);
}

TEST(HSVtoRGBTest, PreservesAlpha)
{
    ImVec4 rgb = HSVtoRGB(0.0f, 1.0f, 1.0f, 0.5f);
    EXPECT_NEAR(rgb.w, 0.5f, 0.01f);
}

TEST(HSVtoRGBTest, WrapsHue)
{
    ImVec4 rgb1 = HSVtoRGB(0.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 rgb2 = HSVtoRGB(1.0f, 1.0f, 1.0f, 1.0f);  // Should wrap to same as 0
    ImVec4 rgb3 = HSVtoRGB(2.0f, 1.0f, 1.0f, 1.0f);  // Should also wrap

    EXPECT_NEAR(rgb1.x, rgb2.x, 0.01f);
    EXPECT_NEAR(rgb1.y, rgb2.y, 0.01f);
    EXPECT_NEAR(rgb1.z, rgb2.z, 0.01f);

    EXPECT_NEAR(rgb1.x, rgb3.x, 0.01f);
}

// ============================================================================
// Tests: Frac
// ============================================================================

TEST(FracTest, ReturnsDecimalPart)
{
    EXPECT_NEAR(Frac(1.25f), 0.25f, 0.0001f);
    EXPECT_NEAR(Frac(3.75f), 0.75f, 0.0001f);
}

TEST(FracTest, HandlesNegative)
{
    EXPECT_NEAR(Frac(-0.25f), 0.75f, 0.0001f);  // -0.25 - floor(-0.25) = -0.25 - (-1) = 0.75
}

TEST(FracTest, HandlesWholeNumbers)
{
    EXPECT_NEAR(Frac(5.0f), 0.0f, 0.0001f);
    EXPECT_NEAR(Frac(0.0f), 0.0f, 0.0001f);
}

// ============================================================================
// Tests: LerpRange
// ============================================================================

TEST(LerpRangeTest, ReturnsMinimumAtZero)
{
    EXPECT_FLOAT_EQ(LerpRange(0.2f, 0.6f, 0.0f), 0.2f);
}

TEST(LerpRangeTest, ReturnsMaximumAtOne)
{
    EXPECT_FLOAT_EQ(LerpRange(0.2f, 0.6f, 1.0f), 0.6f);
}

TEST(LerpRangeTest, InterpolatesMidpoint)
{
    EXPECT_NEAR(LerpRange(0.15f, 0.60f, 0.5f), 0.375f, 0.0001f);
}

// ============================================================================
// Tests: IsPrimaryTextBody
// ============================================================================

TEST(TextBodyHeuristicTest, IncludesMainFillAtCurrentBatchAlpha)
{
    EXPECT_TRUE(IsPrimaryTextBody(210, 180, 120, 96, 96));
}

TEST(TextBodyHeuristicTest, ExcludesLowAlphaSupportPass)
{
    EXPECT_FALSE(IsPrimaryTextBody(210, 180, 120, 48, 255));
}

TEST(TextBodyHeuristicTest, ExcludesDarkOutlinePass)
{
    EXPECT_FALSE(IsPrimaryTextBody(24, 24, 24, 255, 255));
}

// ============================================================================
// Re-implement particle distribution math (same logic as TextEffectsParticle.cpp
// and ParticleTextures.cpp)
// ============================================================================

uint32_t PMixU32(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return v;
}

float PTrait(int particleIndex, int styleIndex, uint32_t salt)
{
    uint32_t h = PMixU32(static_cast<uint32_t>(particleIndex) +
                         PMixU32(static_cast<uint32_t>(styleIndex) + PMixU32(salt)));
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

float AnnulusRadius(float bandFloor, float u)
{
    float f2 = bandFloor * bandFloor;
    return std::sqrt(f2 + (1.0f - f2) * u);
}

float PInterleavedGradientNoise(int x, int y)
{
    float f = 0.06711056f * static_cast<float>(x) + 0.00583715f * static_cast<float>(y);
    f -= std::floor(f);
    float v = 52.9829189f * f;
    return v - std::floor(v);
}

// ============================================================================
// Tests: PTrait (per-particle hash traits)
// ============================================================================

TEST(ParticleTraitTest, StaysInUnitRange)
{
    for (int i = 0; i < 1000; ++i)
    {
        for (uint32_t salt = 1; salt <= 9; ++salt)
        {
            float v = PTrait(i, i % 6, salt);
            EXPECT_GE(v, 0.0f);
            EXPECT_LT(v, 1.0f);
        }
    }
}

TEST(ParticleTraitTest, IsDeterministic)
{
    EXPECT_FLOAT_EQ(PTrait(17, 3, 4), PTrait(17, 3, 4));
    EXPECT_FLOAT_EQ(PTrait(0, 0, 1), PTrait(0, 0, 1));
}

TEST(ParticleTraitTest, MeanIsCentered)
{
    double sum = 0.0;
    const int n = 2000;
    for (int i = 0; i < n; ++i)
    {
        sum += PTrait(i, 2, 4);
    }
    EXPECT_NEAR(sum / n, 0.5, 0.05);
}

TEST(ParticleTraitTest, SaltStreamsAreDecorrelated)
{
    const int n = 1000;
    double sumA = 0.0, sumB = 0.0;
    for (int i = 0; i < n; ++i)
    {
        sumA += PTrait(i, 1, 2);
        sumB += PTrait(i, 1, 4);
    }
    const double meanA = sumA / n;
    const double meanB = sumB / n;
    double cov = 0.0, varA = 0.0, varB = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double da = PTrait(i, 1, 2) - meanA;
        const double db = PTrait(i, 1, 4) - meanB;
        cov += da * db;
        varA += da * da;
        varB += db * db;
    }
    const double pearson = cov / std::sqrt(varA * varB);
    EXPECT_LT(std::abs(pearson), 0.1);
}

// ============================================================================
// Tests: AnnulusRadius (area-uniform band sampling)
// ============================================================================

TEST(AnnulusRadiusTest, HitsBandEdges)
{
    EXPECT_NEAR(AnnulusRadius(0.58f, 0.0f), 0.58f, 1e-5f);
    EXPECT_NEAR(AnnulusRadius(0.58f, 1.0f), 1.0f, 1e-5f);
    EXPECT_NEAR(AnnulusRadius(0.30f, 0.0f), 0.30f, 1e-5f);
    EXPECT_NEAR(AnnulusRadius(0.30f, 1.0f), 1.0f, 1e-5f);
}

TEST(AnnulusRadiusTest, IsMonotonicInU)
{
    float prev = AnnulusRadius(0.45f, 0.0f);
    for (int i = 1; i <= 100; ++i)
    {
        float r = AnnulusRadius(0.45f, static_cast<float>(i) / 100.0f);
        EXPECT_GT(r, prev);
        prev = r;
    }
}

TEST(AnnulusRadiusTest, IsAreaUniform)
{
    // Equal density per unit area: the fraction of samples with r < m must
    // equal the annulus area ratio (m^2 - f^2) / (1 - f^2).
    const float f = 0.58f;
    const float m = 0.8f;
    const int n = 10000;
    int below = 0;
    for (int i = 0; i < n; ++i)
    {
        float u = (static_cast<float>(i) + 0.5f) / n;
        if (AnnulusRadius(f, u) < m)
        {
            ++below;
        }
    }
    const float expected = (m * m - f * f) / (1.0f - f * f);
    EXPECT_NEAR(static_cast<float>(below) / n, expected, 0.01f);
}

// ============================================================================
// Tests: PInterleavedGradientNoise (dither pattern)
// ============================================================================

TEST(InterleavedGradientNoiseTest, StaysInUnitRange)
{
    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            float v = PInterleavedGradientNoise(x, y);
            EXPECT_GE(v, 0.0f);
            EXPECT_LT(v, 1.0f);
        }
    }
}

TEST(InterleavedGradientNoiseTest, MeanIsCentered)
{
    double sum = 0.0;
    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            sum += PInterleavedGradientNoise(x, y);
        }
    }
    EXPECT_NEAR(sum / (64.0 * 64.0), 0.5, 0.05);
}

// ============================================================================
// Re-implement sprite flipbook + archetype timing math (same logic as
// ParticleTextures.cpp and TextEffectsParticle.cpp)
// ============================================================================

// Mirror of the ParticleTextures loader's strip frame detection: a strip is a
// horizontal row of square frames, so width must divide evenly by height and
// yield more than one frame; anything else loads as a 1-frame static.
int StripFrameCount(int width, int height, bool isStrip)
{
    if (isStrip && height > 0 && width % height == 0 && width / height > 1)
    {
        return width / height;
    }
    return 1;
}

// Mirror of TextEffectsParticle FlipFrame with the texture lookup replaced by
// a frame-count parameter (the lookup itself is D3D-bound; its input rule is
// covered by StripFrameCount above). Salt 13 = Trait::Flip.
int FlipFrame(
    int frames, int particleIndex, int styleIndex, float timeScaled, float speedVar, float fps)
{
    if (frames <= 1 || fps <= 0.0f)
    {
        return 0;
    }
    const float phase = PTrait(particleIndex, styleIndex, 13u) * static_cast<float>(frames);
    const float t = timeScaled * fps * speedVar + phase;
    return static_cast<int>(t) % frames;
}

// Mirror of RenderArchZapParticle's strike envelope: sharp eased attack over
// the first tenth of the cycle, exponential decay after.
float ZapEnvelope(float frac)
{
    return (frac < 0.10f) ? SmoothStep(frac / 0.10f) : std::exp(-(frac - 0.10f) * 7.0f);
}

// ============================================================================
// Tests: StripFrameCount (flipbook detection)
// ============================================================================

TEST(StripFrameCountTest, FourFrameStrip)
{
    EXPECT_EQ(StripFrameCount(64, 16, true), 4);
}

TEST(StripFrameCountTest, StaticSquareIsOneFrame)
{
    EXPECT_EQ(StripFrameCount(16, 16, false), 1);
}

TEST(StripFrameCountTest, RequiresStripNaming)
{
    // A wide texture NOT named *_strip.png must never be sliced (HD user
    // textures with non-square aspect would break otherwise).
    EXPECT_EQ(StripFrameCount(64, 16, false), 1);
    EXPECT_EQ(StripFrameCount(2048, 1024, false), 1);
}

TEST(StripFrameCountTest, NonDivisibleWidthFallsBackToStatic)
{
    EXPECT_EQ(StripFrameCount(60, 16, true), 1);
}

TEST(StripFrameCountTest, SquareStripIsStatic)
{
    EXPECT_EQ(StripFrameCount(16, 16, true), 1);
}

TEST(StripFrameCountTest, TallTextureIsStatic)
{
    EXPECT_EQ(StripFrameCount(16, 64, true), 1);
}

// ============================================================================
// Tests: FlipFrame (stateless flipbook clock)
// ============================================================================

TEST(FlipFrameTest, StaticSpriteHoldsFrameZero)
{
    for (float t = 0.0f; t < 20.0f; t += 0.37f)
    {
        EXPECT_EQ(FlipFrame(1, 5, 2, t, 1.0f, 4.0f), 0);
    }
}

TEST(FlipFrameTest, ZeroFpsHoldsFrameZero)
{
    EXPECT_EQ(FlipFrame(4, 5, 2, 12.34f, 1.0f, 0.0f), 0);
}

TEST(FlipFrameTest, StaysInRange)
{
    for (int i = 0; i < 50; ++i)
    {
        for (float t = 0.0f; t < 30.0f; t += 0.21f)
        {
            int f = FlipFrame(4, i, 3, t, 0.72f + 0.01f * i, 7.0f);
            EXPECT_GE(f, 0);
            EXPECT_LT(f, 4);
        }
    }
}

TEST(FlipFrameTest, IsDeterministic)
{
    EXPECT_EQ(FlipFrame(4, 9, 1, 5.5f, 1.1f, 6.0f), FlipFrame(4, 9, 1, 5.5f, 1.1f, 6.0f));
}

TEST(FlipFrameTest, AdvancesWithTime)
{
    // At 4 fps a frame must change within any half-second window.
    int changes = 0;
    int prev = FlipFrame(4, 7, 0, 0.0f, 1.0f, 4.0f);
    for (float t = 0.05f; t < 2.0f; t += 0.05f)
    {
        int f = FlipFrame(4, 7, 0, t, 1.0f, 4.0f);
        if (f != prev)
        {
            ++changes;
        }
        prev = f;
    }
    EXPECT_GE(changes, 4);
}

TEST(FlipFrameTest, PhaseDesyncsParticles)
{
    // At one instant a flock must not share a single frame (the Flip trait
    // spreads phases). With 32 particles over 4 frames, expect > 1 distinct.
    bool seen[4] = {};
    for (int i = 0; i < 32; ++i)
    {
        seen[FlipFrame(4, i, 2, 3.0f, 1.0f, 7.0f)] = true;
    }
    int distinct = seen[0] + seen[1] + seen[2] + seen[3];
    EXPECT_GT(distinct, 1);
}

// ============================================================================
// Tests: ZapEnvelope (strike attack/decay)
// ============================================================================

TEST(ZapEnvelopeTest, StartsDark)
{
    // The envelope must be zero at the cycle seam so the hash re-seat
    // (teleport) is never visible.
    EXPECT_NEAR(ZapEnvelope(0.0f), 0.0f, 1e-4f);
}

TEST(ZapEnvelopeTest, PeaksAtAttackEnd)
{
    EXPECT_NEAR(ZapEnvelope(0.0999f), 1.0f, 0.01f);
}

TEST(ZapEnvelopeTest, AttackIsMonotonic)
{
    float prev = ZapEnvelope(0.0f);
    for (float f = 0.005f; f < 0.10f; f += 0.005f)
    {
        float e = ZapEnvelope(f);
        EXPECT_GE(e, prev);
        prev = e;
    }
}

TEST(ZapEnvelopeTest, DecaysToDarkBeforeCycleEnd)
{
    // Well below the renderer's 0.05 alpha cull by the back half of the cycle.
    EXPECT_LT(ZapEnvelope(0.6f), 0.05f);
    EXPECT_LT(ZapEnvelope(0.99f), 0.01f);
}

// ============================================================================
// Tests: PopFrame (once-through burst playback)
// ============================================================================

// Mirror of RenderArchRiseParticle's pop-frame selection: the burst strip
// plays once across the pop window (popT 0..1), holding the last frame.
static int PopFrame(float popT, int frames)
{
    return (std::min)(static_cast<int>(popT * static_cast<float>(frames)), frames - 1);
}

TEST(PopFrameTest, StaysInRange)
{
    for (float t = 0.0f; t <= 1.0f; t += 0.01f)
    {
        int f = PopFrame(t, 4);
        EXPECT_GE(f, 0);
        EXPECT_LT(f, 4);
    }
}

TEST(PopFrameTest, PlaysFramesInOrderOnce)
{
    int prev = PopFrame(0.0f, 4);
    EXPECT_EQ(prev, 0);
    bool seen[4] = {true, false, false, false};
    for (float t = 0.0f; t <= 1.0f; t += 0.005f)
    {
        int f = PopFrame(t, 4);
        EXPECT_GE(f, prev);  // never rewinds
        seen[f] = true;
        prev = f;
    }
    EXPECT_TRUE(seen[0] && seen[1] && seen[2] && seen[3]);
}

TEST(PopFrameTest, HoldsLastFrameAtEnd)
{
    EXPECT_EQ(PopFrame(1.0f, 4), 3);
    EXPECT_EQ(PopFrame(0.999f, 4), 3);
}

TEST(PopFrameTest, StaticPopAlwaysFrameZero)
{
    for (float t = 0.0f; t <= 1.0f; t += 0.1f)
    {
        EXPECT_EQ(PopFrame(t, 1), 0);
    }
}

// ============================================================================
// Tests: WithAlphaFrom (highlight alpha adoption)
// ============================================================================

// Mirror of TextEffectsInternal WithAlphaFrom: RGB from the first color,
// alpha from the second. Bright sweep targets must adopt the fill's alpha or
// the effect makes text transparent exactly where it brightens.
static uint32_t WithAlphaFromT(uint32_t rgbSrc, uint32_t alphaSrc)
{
    constexpr uint32_t kAlphaMask = static_cast<uint32_t>(0xFF) << IM_COL32_A_SHIFT;
    return (rgbSrc & ~kAlphaMask) | (alphaSrc & kAlphaMask);
}

TEST(WithAlphaFromTest, TakesRGBFromFirstAlphaFromSecond)
{
    const uint32_t rgb = IM_COL32(255, 214, 140, 40);  // low effectAlpha
    const uint32_t alpha = IM_COL32(10, 20, 30, 230);  // fill alpha
    const uint32_t out = WithAlphaFromT(rgb, alpha);
    EXPECT_EQ((out >> IM_COL32_R_SHIFT) & 0xFF, 255u);
    EXPECT_EQ((out >> IM_COL32_G_SHIFT) & 0xFF, 214u);
    EXPECT_EQ((out >> IM_COL32_B_SHIFT) & 0xFF, 140u);
    EXPECT_EQ((out >> IM_COL32_A_SHIFT) & 0xFF, 230u);
}

TEST(WithAlphaFromTest, IdentityWhenAlphasMatch)
{
    const uint32_t c = IM_COL32(1, 2, 3, 99);
    EXPECT_EQ(WithAlphaFromT(c, c), c);
}

// ============================================================================
// Tests: flow wrap regression (counter-direction curtain visibility)
// ============================================================================

TEST(FlowWrapTest, CounterFlowStaysInUnitRange)
{
    // Regression for the aurora fmod bug: with dirSign = -1 the flow phase
    // goes negative and fmod kept it negative, zeroing the edge fade and
    // hiding the particle forever. Frac must keep it in [0, 1).
    for (float t = 0.0f; t < 200.0f; t += 1.7f)
    {
        float flow = Frac(t * 0.18f * -1.0f + 0.5f);
        EXPECT_GE(flow, 0.0f);
        EXPECT_LT(flow, 1.0f);
    }
}

// ============================================================================
// Re-implement status icon badge helpers (same logic as RendererLayout.cpp)
// ============================================================================

// Simplified mirrors of the renderer-side enums and snapshot icon config.
// Always-on slot model: every enabled slot renders, neutral/inactive states
// muted.  Keep in sync with RendererLayout.cpp::ComposeBadges.
enum class RelKind
{
    Hostile,
    Neutral,
    Ally,
    Follower
};
enum class LvlDelta
{
    Weak,
    Even,
    Strong,
    Deadly
};
enum class CritKind
{
    NPC,
    Beast,
    Undead,
    Daedra,
    Dragon
};
enum class RoleK
{
    Commoner,
    Merchant,
    Guard
};
enum class ProtK
{
    Mortal,
    Protected,
    Essential
};
enum class EngK
{
    Idle,
    Alert,
    Combat
};
enum class SneakK
{
    Off,
    Hidden,
    Detected
};

struct TestColor
{
    float r = 1.0f, g = 1.0f, b = 1.0f;
};

struct IconCfg
{
    bool enabled = true;
    bool deadlyPulse = true;
    // Full-color emblem tier badge (default off -> FA medal/gem/crown path).
    bool tierBadgeImages = false;
    int tierImageCount = 0;
    float tierBadgeGamma = 1.8f;
    // Original three slots.
    std::string icoFollower = "shield-halved", icoAlly = "handshake";
    std::string icoHostile = "skull-crossbones";
    std::string icoWeak = "caret-down", icoStrong = "caret-up", icoDeadly = "skull";
    std::string icoBeast = "paw", icoUndead = "ghost", icoDaedra = "fire", icoDragon = "dragon";
    // Expanded always-on slots.
    std::string icoNeutral = "circle", icoHumanoid = "user", icoEven = "equals";
    std::string icoGuard = "helmet-battle", icoMerchant = "coins", icoCommoner = "house";
    std::string icoEssential = "certificate", icoProtected = "shield-check", icoMortal = "heart";
    std::string icoCombat = "swords", icoAlert = "eye", icoIdle = "moon";
    std::string icoSneakHidden = "eye-slash", icoSneakDetected = "eye";
    std::string icoSneakOff = "person-walking";
    std::string icoEncumbered = "weight-hanging", icoNormalWeight = "feather";
    std::string icoWanted = "gavel", icoBountyClear = "scale-balanced";
    std::string icoTierLow = "medal", icoTierMid = "gem", icoTierHigh = "crown";
    TestColor colFollower{0.46f, 0.68f, 0.84f};
    TestColor colAlly{0.52f, 0.74f, 0.50f};
    TestColor colHostile{0.86f, 0.36f, 0.32f};
    TestColor colWeak{0.54f, 0.66f, 0.80f};
    TestColor colStrong{0.86f, 0.62f, 0.32f};
    TestColor colDeadly{0.90f, 0.28f, 0.24f};
    TestColor colCreature{0.80f, 0.74f, 0.62f};
    TestColor colGuard{0.60f, 0.68f, 0.84f};
    TestColor colMerchant{0.84f, 0.74f, 0.42f};
    TestColor colEssential{0.86f, 0.78f, 0.46f};
    TestColor colProtected{0.54f, 0.72f, 0.86f};
    TestColor colCombat{0.88f, 0.42f, 0.30f};
    TestColor colAlert{0.86f, 0.76f, 0.40f};
    TestColor colSneakHidden{0.50f, 0.64f, 0.84f};
    TestColor colSneakDetected{0.86f, 0.36f, 0.32f};
    TestColor colEncumbered{0.82f, 0.64f, 0.40f};
    TestColor colWanted{0.84f, 0.34f, 0.30f};
    TestColor colTierLow{0.70f, 0.62f, 0.52f};
    TestColor colTierMid{0.62f, 0.70f, 0.80f};
    TestColor colTierHigh{0.86f, 0.74f, 0.46f};
    TestColor colNeutral{0.56f, 0.62f, 0.70f};
    TestColor colHumanoid{0.74f, 0.68f, 0.58f};
    TestColor colCommoner{0.60f, 0.68f, 0.54f};
    TestColor colMortal{0.76f, 0.58f, 0.60f};
    TestColor colEven{0.60f, 0.70f, 0.72f};
    TestColor colIdle{0.56f, 0.60f, 0.76f};
    TestColor colSneakOff{0.64f, 0.68f, 0.60f};
    TestColor colNormalWeight{0.64f, 0.76f, 0.70f};
    TestColor colBountyClear{0.50f, 0.70f, 0.68f};
    TestColor colMuted{0.62f, 0.64f, 0.68f};
    bool relationshipEnabled = true, creatureEnabled = true, threatEnabled = true;
    bool roleEnabled = true, protectionEnabled = true, engagementEnabled = true;
    bool combatStateEnabled = true, alertStateEnabled = true;
    bool sneakEnabled = true, playerCombatEnabled = true;
    bool encumberedEnabled = true, bountyEnabled = true;
    bool tierEnabled = true;
};

// Mirror of RendererLayout.cpp::TierBandIndex -- keep in sync.
int TierBandIndex(int tierIdx, int tierCount)
{
    if (tierCount <= 1)
        return 0;
    return std::clamp(tierIdx * 3 / tierCount, 0, 2);
}

// Mirror of RendererLayout.cpp::TierImageBandIndex -- keep in sync.
int TierImageBandIndex(int tierIdx, int tierCount, int imageCount, float gamma)
{
    if (imageCount <= 1 || tierCount <= 1)
        return 0;
    const float t =
        std::clamp(static_cast<float>(tierIdx) / static_cast<float>(tierCount - 1), 0.0f, 1.0f);
    const float g = std::clamp(gamma, 0.1f, 8.0f);
    const int band = static_cast<int>(std::floor(std::pow(t, g) * static_cast<float>(imageCount)));
    return std::clamp(band, 0, imageCount - 1);
}

// Mirror of ActorDrawData's badge-relevant facts.
struct Facts
{
    bool isPlayer = false;
    RelKind relationship = RelKind::Neutral;
    LvlDelta levelDelta = LvlDelta::Even;
    CritKind creatureKind = CritKind::NPC;
    RoleK role = RoleK::Commoner;
    ProtK protection = ProtK::Mortal;
    EngK engagement = EngK::Idle;
    SneakK sneak = SneakK::Off;
    bool playerInCombat = false;
    bool encumbered = false;
    bool wanted = false;
};

struct BadgeSlot
{
    std::string icon;
    TestColor color;
    bool muted = false;
    bool pulse = false;
    int tierImage = -1;  // >=0 -> full-color emblem by index (icon unused)
};

struct BadgeComposition
{
    std::vector<BadgeSlot> slots;
    void push(const std::string& icon, TestColor color, bool muted, bool pulse = false)
    {
        if (!icon.empty())
            slots.push_back({icon, color, muted, pulse});
    }
    void pushTierImage(bool enabled, int imageIndex)
    {
        if (enabled && imageIndex >= 0)
            slots.push_back(BadgeSlot{"", {}, false, false, imageIndex});
    }
};

// Map actor facts to an ordered set of badge slots.  Mirrors
// RendererLayout.cpp::ComposeBadges -- keep the logic in sync.
BadgeComposition ComposeBadges(const Facts& d,
                               const IconCfg& cfg,
                               int tierIdx = 0,
                               int tierCount = 20,
                               bool includeRank = true)
{
    BadgeComposition out{};
    if (!cfg.enabled)
        return out;

    auto add =
        [&](bool enabled, const std::string& icon, TestColor color, bool muted, bool pulse = false)
    {
        if (enabled)
            out.push(icon, color, muted, pulse);
    };

    const auto addRank = [&]()
    {
        if (!includeRank)
            return;
        if (cfg.tierBadgeImages && cfg.tierImageCount > 0)
        {
            out.pushTierImage(
                cfg.tierEnabled,
                TierImageBandIndex(tierIdx, tierCount, cfg.tierImageCount, cfg.tierBadgeGamma));
            return;
        }

        const int band = TierBandIndex(tierIdx, tierCount);
        add(cfg.tierEnabled,
            band == 2 ? cfg.icoTierHigh : (band == 1 ? cfg.icoTierMid : cfg.icoTierLow),
            band == 2 ? cfg.colTierHigh : (band == 1 ? cfg.colTierMid : cfg.colTierLow),
            false);
    };

    if (!d.isPlayer)
    {
        addRank();

        switch (d.relationship)
        {
            case RelKind::Hostile:
                add(cfg.relationshipEnabled, cfg.icoHostile, cfg.colHostile, false);
                break;
            case RelKind::Ally:
                add(cfg.relationshipEnabled, cfg.icoAlly, cfg.colAlly, false);
                break;
            case RelKind::Follower:
                add(cfg.relationshipEnabled, cfg.icoFollower, cfg.colFollower, false);
                break;
            case RelKind::Neutral:
                add(cfg.relationshipEnabled, cfg.icoNeutral, cfg.colNeutral, true);
                break;
        }
        switch (d.creatureKind)
        {
            case CritKind::Dragon:
                add(cfg.creatureEnabled, cfg.icoDragon, cfg.colCreature, false);
                break;
            case CritKind::Daedra:
                add(cfg.creatureEnabled, cfg.icoDaedra, cfg.colCreature, false);
                break;
            case CritKind::Undead:
                add(cfg.creatureEnabled, cfg.icoUndead, cfg.colCreature, false);
                break;
            case CritKind::Beast:
                add(cfg.creatureEnabled, cfg.icoBeast, cfg.colCreature, false);
                break;
            case CritKind::NPC:
                add(cfg.creatureEnabled, cfg.icoHumanoid, cfg.colHumanoid, true);
                break;
        }
        switch (d.role)
        {
            case RoleK::Guard:
                add(cfg.roleEnabled, cfg.icoGuard, cfg.colGuard, false);
                break;
            case RoleK::Merchant:
                add(cfg.roleEnabled, cfg.icoMerchant, cfg.colMerchant, false);
                break;
            case RoleK::Commoner:
                add(cfg.roleEnabled, cfg.icoCommoner, cfg.colCommoner, true);
                break;
        }
        switch (d.protection)
        {
            case ProtK::Essential:
                add(cfg.protectionEnabled, cfg.icoEssential, cfg.colEssential, false);
                break;
            case ProtK::Protected:
                add(cfg.protectionEnabled, cfg.icoProtected, cfg.colProtected, false);
                break;
            case ProtK::Mortal:
                add(cfg.protectionEnabled, cfg.icoMortal, cfg.colMortal, true);
                break;
        }
        switch (d.levelDelta)
        {
            case LvlDelta::Deadly:
                add(cfg.threatEnabled, cfg.icoDeadly, cfg.colDeadly, false, cfg.deadlyPulse);
                break;
            case LvlDelta::Strong:
                add(cfg.threatEnabled, cfg.icoStrong, cfg.colStrong, false);
                break;
            case LvlDelta::Weak:
                add(cfg.threatEnabled, cfg.icoWeak, cfg.colWeak, false);
                break;
            case LvlDelta::Even:
                add(cfg.threatEnabled, cfg.icoEven, cfg.colEven, true);
                break;
        }
        switch (d.engagement)
        {
            case EngK::Combat:
                add(cfg.engagementEnabled && cfg.combatStateEnabled,
                    cfg.icoCombat,
                    cfg.colCombat,
                    false);
                break;
            case EngK::Alert:
                add(cfg.engagementEnabled && cfg.alertStateEnabled,
                    cfg.icoAlert,
                    cfg.colAlert,
                    false);
                break;
            case EngK::Idle:
                add(cfg.engagementEnabled, cfg.icoIdle, cfg.colIdle, true);
                break;
        }
        return out;
    }

    addRank();

    switch (d.sneak)
    {
        case SneakK::Detected:
            add(cfg.sneakEnabled, cfg.icoSneakDetected, cfg.colSneakDetected, false);
            break;
        case SneakK::Hidden:
            add(cfg.sneakEnabled, cfg.icoSneakHidden, cfg.colSneakHidden, false);
            break;
        case SneakK::Off:
            add(cfg.sneakEnabled, cfg.icoSneakOff, cfg.colSneakOff, true);
            break;
    }
    add(cfg.playerCombatEnabled,
        d.playerInCombat ? cfg.icoCombat : cfg.icoIdle,
        d.playerInCombat ? cfg.colCombat : cfg.colIdle,
        !d.playerInCombat);
    add(cfg.encumberedEnabled,
        d.encumbered ? cfg.icoEncumbered : cfg.icoNormalWeight,
        d.encumbered ? cfg.colEncumbered : cfg.colNormalWeight,
        !d.encumbered);
    add(cfg.bountyEnabled,
        d.wanted ? cfg.icoWanted : cfg.icoBountyClear,
        d.wanted ? cfg.colWanted : cfg.colBountyClear,
        !d.wanted);
    return out;
}

// Pure classifier mirrors (the game-thread RE:: reads can't be unit-tested,
// but the classification logic + priority can).  Keep in sync with
// RendererSnapshot.cpp.
ProtK ClassifyProtection(bool essential, bool prot)
{
    if (essential)
        return ProtK::Essential;
    if (prot)
        return ProtK::Protected;
    return ProtK::Mortal;
}
RoleK ClassifyRole(bool guard, bool vendor)
{
    if (guard)
        return RoleK::Guard;
    if (vendor)
        return RoleK::Merchant;
    return RoleK::Commoner;
}
EngK ClassifyEngagement(bool inCombat, bool weaponDrawn, int detection)
{
    if (inCombat)
        return EngK::Combat;
    if (weaponDrawn && detection > 0)
        return EngK::Alert;
    return EngK::Idle;
}

// Mirror of the DrawBadges muted desaturation (toward luma by `desat`).
TestColor ApplyMutedDesat(TestColor c, float desat)
{
    const float luma = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
    return {c.r + (luma - c.r) * desat, c.g + (luma - c.g) * desat, c.b + (luma - c.b) * desat};
}

// Mirror of the DrawBadges alpha treatment. IconOpacity applies to the status
// row before the optional resting-state multiplier.
float ResolveBadgeAlpha(
    float plateAlpha, float badgeAlphaMul, float iconOpacity, bool muted, float mutedAlpha)
{
    float alpha = std::min(1.0f, plateAlpha * badgeAlphaMul * iconOpacity);
    if (muted)
        alpha *= mutedAlpha;
    return alpha;
}

// Mirror of the DrawTierEmblem crisp-mark alpha treatment.
float ResolveTierEmblemAlpha(float plateAlpha, float badgeAlphaMul, float crispAlpha)
{
    return plateAlpha * badgeAlphaMul * crispAlpha;
}

// ============================================================================
// Tests: ComposeBadges (always-on slot model)
// ============================================================================

TEST(ComposeBadgesTest, NpcAlwaysHasSevenSlots)
{
    auto s = ComposeBadges(Facts{}, IconCfg{});  // all-neutral commoner
    ASSERT_EQ(s.slots.size(), 7u);
    EXPECT_EQ(s.slots[0].icon, "medal");  // rank is an always-lit identity fact
    EXPECT_FALSE(s.slots[0].muted);
    for (size_t i = 1; i < s.slots.size(); ++i)
    {
        EXPECT_TRUE(s.slots[i].muted);  // every neutral status slot renders muted
    }
}

TEST(ComposeBadgesTest, NpcRankPrecedesStatusRow)
{
    Facts f;
    f.relationship = RelKind::Follower;
    auto s = ComposeBadges(f, IconCfg{});
    ASSERT_EQ(s.slots.size(), 7u);
    EXPECT_EQ(s.slots[0].icon, "medal");          // rank is slot 0
    EXPECT_EQ(s.slots[1].icon, "shield-halved");  // relationship starts the status row
    EXPECT_FALSE(s.slots[1].muted);
    EXPECT_FLOAT_EQ(s.slots[1].color.b, 0.84f);  // colFollower
}

TEST(ComposeBadgesTest, NeutralRelationshipUsesRestingColor)
{
    auto s = ComposeBadges(Facts{}, IconCfg{});
    EXPECT_EQ(s.slots[1].icon, "circle");
    EXPECT_TRUE(s.slots[1].muted);               // still drawn dimmed, but in its own hue
    EXPECT_FLOAT_EQ(s.slots[1].color.r, 0.56f);  // colNeutral
    EXPECT_FLOAT_EQ(s.slots[1].color.g, 0.62f);
}

TEST(ComposeBadgesTest, NewNpcSlotsLitWhenActive)
{
    Facts f;
    f.role = RoleK::Merchant;
    f.protection = ProtK::Essential;
    f.engagement = EngK::Combat;
    auto s = ComposeBadges(f, IconCfg{});
    ASSERT_EQ(s.slots.size(), 7u);
    EXPECT_EQ(s.slots[3].icon, "coins");  // role
    EXPECT_FALSE(s.slots[3].muted);
    EXPECT_EQ(s.slots[4].icon, "certificate");  // protection
    EXPECT_FALSE(s.slots[4].muted);
    EXPECT_EQ(s.slots[6].icon, "swords");  // engagement
    EXPECT_FALSE(s.slots[6].muted);
}

TEST(ComposeBadgesTest, DeadlyThreatPulsesWeakDoesNot)
{
    Facts deadly;
    deadly.levelDelta = LvlDelta::Deadly;
    auto sd = ComposeBadges(deadly, IconCfg{});
    EXPECT_EQ(sd.slots[5].icon, "skull");  // threat is slot 5 after rank
    EXPECT_TRUE(sd.slots[5].pulse);
    Facts weak;
    weak.levelDelta = LvlDelta::Weak;
    auto sw = ComposeBadges(weak, IconCfg{});
    EXPECT_EQ(sw.slots[5].icon, "caret-down");
    EXPECT_FALSE(sw.slots[5].pulse);
}

TEST(ComposeBadgesTest, PlayerGetsFivePlayerSlots)
{
    Facts f;
    f.isPlayer = true;
    f.sneak = SneakK::Hidden;
    f.playerInCombat = true;
    f.encumbered = true;
    f.wanted = true;
    auto s = ComposeBadges(f, IconCfg{});
    ASSERT_EQ(s.slots.size(), 5u);
    EXPECT_EQ(s.slots[0].icon, "medal");           // tier band (default idx 0 = low)
    EXPECT_EQ(s.slots[1].icon, "eye-slash");       // sneak hidden
    EXPECT_EQ(s.slots[2].icon, "swords");          // combat
    EXPECT_EQ(s.slots[3].icon, "weight-hanging");  // encumbered
    EXPECT_EQ(s.slots[4].icon, "gavel");           // wanted
    for (const auto& slot : s.slots)
    {
        EXPECT_FALSE(slot.muted);
    }
}

TEST(ComposeBadgesTest, PlayerNeutralSlotsMuted)
{
    Facts f;
    f.isPlayer = true;  // not sneaking, not in combat, unencumbered, no bounty
    auto s = ComposeBadges(f, IconCfg{});
    ASSERT_EQ(s.slots.size(), 5u);
    EXPECT_EQ(s.slots[0].icon, "medal");  // tier badge is always lit
    EXPECT_FALSE(s.slots[0].muted);
    EXPECT_EQ(s.slots[1].icon, "person-walking");
    EXPECT_EQ(s.slots[4].icon, "scale-balanced");
    for (size_t i = 1; i < s.slots.size(); ++i)
    {
        EXPECT_TRUE(s.slots[i].muted);
    }
}

TEST(ComposeBadgesTest, PlayerDetectedUsesDetectedIcon)
{
    Facts f;
    f.isPlayer = true;
    f.sneak = SneakK::Detected;
    auto s = ComposeBadges(f, IconCfg{});
    EXPECT_EQ(s.slots[1].icon, "eye");
    EXPECT_FALSE(s.slots[1].muted);
}

TEST(ComposeBadgesTest, PlayerTierBadgeIsFirstSlot)
{
    Facts f;
    f.isPlayer = true;
    auto s = ComposeBadges(f, IconCfg{}, 19, 20);  // top tier -> high band
    ASSERT_GE(s.slots.size(), 1u);
    EXPECT_EQ(s.slots[0].icon, "crown");
    EXPECT_FALSE(s.slots[0].muted);
    EXPECT_FLOAT_EQ(s.slots[0].color.r, 0.86f);  // colTierHigh
}

TEST(ComposeBadgesTest, TierBandThirdsMapLowMidHigh)
{
    EXPECT_EQ(TierBandIndex(0, 20), 0);
    EXPECT_EQ(TierBandIndex(6, 20), 0);
    EXPECT_EQ(TierBandIndex(7, 20), 1);
    EXPECT_EQ(TierBandIndex(13, 20), 1);
    EXPECT_EQ(TierBandIndex(14, 20), 2);
    EXPECT_EQ(TierBandIndex(19, 20), 2);
    // Degenerate ladders collapse to the low band.
    EXPECT_EQ(TierBandIndex(0, 1), 0);
    EXPECT_EQ(TierBandIndex(0, 0), 0);
}

TEST(ComposeBadgesTest, TierImageBandIsTopWeighted)
{
    // 9 emblems over 20 tiers, gamma 1.8: monotonic, clamped, endpoints anchored,
    // and the lowest emblem spans strictly more tiers than the top one.
    EXPECT_EQ(TierImageBandIndex(0, 20, 9, 1.8f), 0);
    EXPECT_EQ(TierImageBandIndex(19, 20, 9, 1.8f), 8);
    int prev = -1;
    int firstSpan = 0;
    int lastSpan = 0;
    for (int t = 0; t < 20; ++t)
    {
        const int b = TierImageBandIndex(t, 20, 9, 1.8f);
        EXPECT_GE(b, 0);
        EXPECT_LE(b, 8);
        EXPECT_GE(b, prev);  // non-decreasing across the ladder
        prev = b;
        firstSpan += (b == 0) ? 1 : 0;
        lastSpan += (b == 8) ? 1 : 0;
    }
    EXPECT_GT(firstSpan, lastSpan);  // top-weighted: rare emblems near the top
    // Degenerate inputs collapse to band 0.
    EXPECT_EQ(TierImageBandIndex(5, 20, 1, 1.8f), 0);
    EXPECT_EQ(TierImageBandIndex(5, 1, 9, 1.8f), 0);
}

TEST(ComposeBadgesTest, TierBadgeImagesUsesEmblemSlot)
{
    Facts f;
    f.isPlayer = true;
    IconCfg cfg;
    cfg.tierBadgeImages = true;
    cfg.tierImageCount = 9;
    auto s = ComposeBadges(f, cfg, 19, 20);  // top tier
    ASSERT_FALSE(s.slots.empty());
    // First player slot is the emblem: resolved by index, no icon name.
    EXPECT_EQ(s.slots[0].tierImage, 8);  // top tier -> top emblem
    EXPECT_TRUE(s.slots[0].icon.empty());
}

TEST(ComposeBadgesTest, TierBadgeImagesFallsBackWhenNoneLoaded)
{
    Facts f;
    f.isPlayer = true;
    IconCfg cfg;
    cfg.tierBadgeImages = true;
    cfg.tierImageCount = 0;  // nothing loaded -> FA medal/gem/crown path
    auto s = ComposeBadges(f, cfg, 19, 20);
    ASSERT_FALSE(s.slots.empty());
    EXPECT_EQ(s.slots[0].tierImage, -1);
    EXPECT_EQ(s.slots[0].icon, "crown");  // high band
}

TEST(ComposeBadgesTest, TierDisabledDropsSlot)
{
    IconCfg cfg{};
    cfg.tierEnabled = false;
    Facts f;
    f.isPlayer = true;
    auto s = ComposeBadges(f, cfg);
    ASSERT_EQ(s.slots.size(), 4u);
    for (const auto& slot : s.slots)
    {
        EXPECT_NE(slot.icon, "medal");
    }
}

TEST(ComposeBadgesTest, NpcGetsRankBadge)
{
    Facts f;  // NPC facts
    auto s = ComposeBadges(f, IconCfg{}, 19, 20);
    ASSERT_EQ(s.slots.size(), 7u);
    EXPECT_EQ(s.slots[0].icon, "crown");
    EXPECT_FALSE(s.slots[0].muted);
    EXPECT_EQ(s.slots[1].icon, "circle");  // status ordering remains intact
}

TEST(ComposeBadgesTest, NpcTierImageUsesEmblemSlot)
{
    IconCfg cfg;
    cfg.tierBadgeImages = true;
    cfg.tierImageCount = 9;
    auto s = ComposeBadges(Facts{}, cfg, 19, 20);
    ASSERT_EQ(s.slots.size(), 7u);
    EXPECT_EQ(s.slots[0].tierImage, 8);
    EXPECT_TRUE(s.slots[0].icon.empty());
}

TEST(ComposeBadgesTest, NpcTierDisabledKeepsSixStatusSlots)
{
    IconCfg cfg;
    cfg.tierEnabled = false;
    auto s = ComposeBadges(Facts{}, cfg);
    ASSERT_EQ(s.slots.size(), 6u);
    EXPECT_EQ(s.slots[0].icon, "circle");
}

TEST(ComposeBadgesTest, NpcRankCanBeOmittedWhenSurfaceHasSeparateSeal)
{
    auto s = ComposeBadges(Facts{}, IconCfg{}, 19, 20, false);
    ASSERT_EQ(s.slots.size(), 6u);
    EXPECT_EQ(s.slots[0].icon, "circle");
}

TEST(ComposeBadgesTest, DisabledGetsNothing)
{
    IconCfg cfg{};
    cfg.enabled = false;
    Facts f;
    f.relationship = RelKind::Hostile;
    f.levelDelta = LvlDelta::Deadly;
    EXPECT_TRUE(ComposeBadges(f, cfg).slots.empty());
}

TEST(ComposeBadgesTest, PerSlotEnableDropsSlot)
{
    IconCfg cfg{};
    cfg.roleEnabled = false;
    auto s = ComposeBadges(Facts{}, cfg);
    EXPECT_EQ(s.slots.size(), 6u);  // role slot dropped; rank + five status slots remain
    for (const auto& slot : s.slots)
    {
        EXPECT_NE(slot.icon, "house");
        EXPECT_NE(slot.icon, "coins");
    }
}

TEST(ComposeBadgesTest, DisabledCombatStateDropsCombatBadge)
{
    IconCfg cfg{};
    cfg.combatStateEnabled = false;
    Facts f;
    f.engagement = EngK::Combat;
    auto s = ComposeBadges(f, cfg);
    EXPECT_EQ(s.slots.size(), 6u);  // engagement slot empty; rank remains
    for (const auto& slot : s.slots)
    {
        EXPECT_NE(slot.icon, "swords");
    }
}

TEST(ComposeBadgesTest, EmptyIconNameDropsSlot)
{
    IconCfg cfg{};
    cfg.icoFollower = "";
    Facts f;
    f.relationship = RelKind::Follower;
    auto s = ComposeBadges(f, cfg);
    EXPECT_EQ(s.slots.size(), 6u);
    for (const auto& slot : s.slots)
    {
        EXPECT_NE(slot.icon, "shield-halved");
    }
}

TEST(ComposeBadgesTest, DragonCreatureLitInCreatureColor)
{
    Facts f;
    f.creatureKind = CritKind::Dragon;
    auto s = ComposeBadges(f, IconCfg{});
    EXPECT_EQ(s.slots[2].icon, "dragon");  // creature is slot 2 after rank
    EXPECT_FALSE(s.slots[2].muted);
    EXPECT_FLOAT_EQ(s.slots[2].color.r, 0.80f);  // colCreature
}

// ============================================================================
// Tests: status classifiers + muted styling
// ============================================================================

TEST(ClassifyTest, ProtectionEssentialBeatsProtected)
{
    EXPECT_EQ(ClassifyProtection(true, true), ProtK::Essential);
    EXPECT_EQ(ClassifyProtection(false, true), ProtK::Protected);
    EXPECT_EQ(ClassifyProtection(false, false), ProtK::Mortal);
}

TEST(ClassifyTest, RoleGuardBeatsMerchant)
{
    EXPECT_EQ(ClassifyRole(true, true), RoleK::Guard);
    EXPECT_EQ(ClassifyRole(false, true), RoleK::Merchant);
    EXPECT_EQ(ClassifyRole(false, false), RoleK::Commoner);
}

TEST(ClassifyTest, EngagementCombatBeatsAlert)
{
    EXPECT_EQ(ClassifyEngagement(true, true, 100), EngK::Combat);
    EXPECT_EQ(ClassifyEngagement(false, true, 1), EngK::Alert);
    EXPECT_EQ(ClassifyEngagement(false, true, 0), EngK::Idle);     // no detection
    EXPECT_EQ(ClassifyEngagement(false, false, 100), EngK::Idle);  // weapon sheathed
}

TEST(MutedStyleTest, FullDesatGoesToLuma)
{
    auto c = ApplyMutedDesat(TestColor{1.0f, 0.0f, 0.0f}, 1.0f);
    EXPECT_NEAR(c.r, 0.299f, 1e-5f);
    EXPECT_NEAR(c.g, 0.299f, 1e-5f);
    EXPECT_NEAR(c.b, 0.299f, 1e-5f);
}

TEST(MutedStyleTest, ZeroDesatKeepsColor)
{
    auto c = ApplyMutedDesat(TestColor{0.2f, 0.5f, 0.9f}, 0.0f);
    EXPECT_FLOAT_EQ(c.r, 0.2f);
    EXPECT_FLOAT_EQ(c.g, 0.5f);
    EXPECT_FLOAT_EQ(c.b, 0.9f);
}

TEST(BadgeTreatmentTest, DefaultRestingAndActiveAlphaMatchDuringPlateFades)
{
    constexpr float kIconOpacity = 0.92f;
    constexpr float kDefaultMutedAlpha = 1.0f;

    const float activeFull = ResolveBadgeAlpha(1.0f, 1.0f, kIconOpacity, false, kDefaultMutedAlpha);
    const float restingFull = ResolveBadgeAlpha(1.0f, 1.0f, kIconOpacity, true, kDefaultMutedAlpha);
    EXPECT_FLOAT_EQ(restingFull, activeFull);

    const float activeFading =
        ResolveBadgeAlpha(0.42f, 0.65f, kIconOpacity, false, kDefaultMutedAlpha);
    const float restingFading =
        ResolveBadgeAlpha(0.42f, 0.65f, kIconOpacity, true, kDefaultMutedAlpha);
    EXPECT_FLOAT_EQ(restingFading, activeFading);
}

TEST(BadgeTreatmentTest, CustomMutedAlphaDimsOnlyRestingBadges)
{
    constexpr float kMutedAlpha = 0.35f;
    const float active = ResolveBadgeAlpha(0.42f, 0.65f, 0.92f, false, kMutedAlpha);
    const float resting = ResolveBadgeAlpha(0.42f, 0.65f, 0.92f, true, kMutedAlpha);

    EXPECT_NEAR(resting, active * kMutedAlpha, 1e-6f);
    EXPECT_LT(resting, active);
}

TEST(BadgeTreatmentTest, RankBadgeStaysSlightlyAboveStatusRowDuringFades)
{
    constexpr float kStatusOpacity = 0.92f;
    constexpr float kRankOpacity = 0.95f;

    const float statusFull = ResolveBadgeAlpha(1.0f, 1.0f, kStatusOpacity, false, 1.0f);
    const float rankFull = ResolveTierEmblemAlpha(1.0f, 1.0f, kRankOpacity);
    EXPECT_FLOAT_EQ(statusFull, 0.92f);
    EXPECT_FLOAT_EQ(rankFull, 0.95f);
    EXPECT_GT(rankFull, statusFull);

    const float statusFading = ResolveBadgeAlpha(0.42f, 0.65f, kStatusOpacity, false, 1.0f);
    const float rankFading = ResolveTierEmblemAlpha(0.42f, 0.65f, kRankOpacity);
    EXPECT_GT(rankFading, statusFading);
}

// ============================================================================
// The Quiet Frame -- camera-motion quiet target mapping
// (mirrors QuietTarget in Renderer.cpp; keep the logic in sync)
// ============================================================================

static float QuietTarget(float degPerSec, float lo, float hi)
{
    if (hi <= lo)
        return degPerSec >= hi ? 1.0f : 0.0f;
    return SmoothStep(Saturate((degPerSec - lo) / (hi - lo)));
}

TEST(QuietFrame, BelowLowThresholdIsUntouched)
{
    EXPECT_FLOAT_EQ(QuietTarget(0.0f, 40.0f, 160.0f), 0.0f);
    EXPECT_FLOAT_EQ(QuietTarget(39.9f, 40.0f, 160.0f), 0.0f);
}

TEST(QuietFrame, AboveHighThresholdIsFullyQuiet)
{
    EXPECT_FLOAT_EQ(QuietTarget(160.0f, 40.0f, 160.0f), 1.0f);
    EXPECT_FLOAT_EQ(QuietTarget(720.0f, 40.0f, 160.0f), 1.0f);
}

TEST(QuietFrame, MidpointIsSmoothstepHalf)
{
    // SmoothStep(0.5) = 0.5 for the quintic; the mapping must be monotonic.
    EXPECT_FLOAT_EQ(QuietTarget(100.0f, 40.0f, 160.0f), 0.5f);
    EXPECT_LT(QuietTarget(60.0f, 40.0f, 160.0f), QuietTarget(120.0f, 40.0f, 160.0f));
}

TEST(QuietFrame, DegenerateThresholdsActAsStep)
{
    EXPECT_FLOAT_EQ(QuietTarget(10.0f, 50.0f, 50.0f), 0.0f);
    EXPECT_FLOAT_EQ(QuietTarget(50.0f, 50.0f, 50.0f), 1.0f);
}

// ============================================================================
// Roll Call -- entrance stagger slot delays
// (mirrors the assignment in Renderer.cpp DrawLabel; keep in sync)
// ============================================================================

static float StaggerDelay(int slot, float step, float maxDelay)
{
    return std::min(static_cast<float>(slot) * step, maxDelay);
}

TEST(RollCall, FirstSlotStartsImmediately)
{
    EXPECT_FLOAT_EQ(StaggerDelay(0, 0.06f, 0.8f), 0.0f);
}

TEST(RollCall, SlotsAccumulateByStep)
{
    EXPECT_FLOAT_EQ(StaggerDelay(1, 0.06f, 0.8f), 0.06f);
    EXPECT_FLOAT_EQ(StaggerDelay(5, 0.06f, 0.8f), 0.30f);
}

TEST(RollCall, DelayIsCapped)
{
    EXPECT_FLOAT_EQ(StaggerDelay(100, 0.06f, 0.8f), 0.8f);
}

TEST(RollCall, ZeroStepDisablesStagger)
{
    EXPECT_FLOAT_EQ(StaggerDelay(7, 0.0f, 0.8f), 0.0f);
}

// ============================================================================
// Last Rites -- valediction phase math
// (mirrors ComputeDeathRitePhases in Renderer.cpp; keep in sync)
// ============================================================================

struct DeathRitePhases
{
    float drainT;
    float dissolveT;
};

static DeathRitePhases ComputeDeathRitePhases(float t)
{
    constexpr float kHoldEnd = 0.22f;
    constexpr float kDrainEnd = 0.55f;
    DeathRitePhases p{};
    p.drainT = Saturate((t - kHoldEnd) / (kDrainEnd - kHoldEnd));
    p.dissolveT = Saturate((t - kDrainEnd) / (1.0f - kDrainEnd));
    return p;
}

TEST(DeathRite, HoldPhaseLeavesInkUntouched)
{
    const auto p = ComputeDeathRitePhases(0.10f);
    EXPECT_FLOAT_EQ(p.drainT, 0.0f);
    EXPECT_FLOAT_EQ(p.dissolveT, 0.0f);
}

TEST(DeathRite, DrainCompletesBeforeDissolveBegins)
{
    const auto p = ComputeDeathRitePhases(0.55f);
    EXPECT_FLOAT_EQ(p.drainT, 1.0f);
    EXPECT_FLOAT_EQ(p.dissolveT, 0.0f);
}

TEST(DeathRite, EndOfRiteIsFullyDissolved)
{
    const auto p = ComputeDeathRitePhases(1.0f);
    EXPECT_FLOAT_EQ(p.drainT, 1.0f);
    EXPECT_FLOAT_EQ(p.dissolveT, 1.0f);
}

TEST(DeathRite, MidDrainIsProportional)
{
    const auto p = ComputeDeathRitePhases(0.385f);  // halfway through drain
    EXPECT_NEAR(p.drainT, 0.5f, 1e-4f);
    EXPECT_FLOAT_EQ(p.dissolveT, 0.0f);
}

// ============================================================================
// Candlelight Metering -- exposure gain mapping
// (mirrors the gain computation in ApplyCandlelight, RendererLayout.cpp)
// ============================================================================

static float CandleGain(float bgLum, float strength)
{
    return 1.0f + std::clamp((0.5f - bgLum) * 2.0f * strength, -strength, strength);
}

TEST(Candlelight, MidGreySceneLeavesInkUntouched)
{
    EXPECT_FLOAT_EQ(CandleGain(0.5f, 0.08f), 1.0f);
}

TEST(Candlelight, DarkSceneLiftsInkUpToCap)
{
    EXPECT_FLOAT_EQ(CandleGain(0.0f, 0.08f), 1.08f);
    EXPECT_FLOAT_EQ(CandleGain(0.1f, 0.08f), 1.064f);
}

TEST(Candlelight, BrightSceneDimsInkUpToCap)
{
    EXPECT_FLOAT_EQ(CandleGain(1.0f, 0.08f), 0.92f);
}

TEST(Candlelight, ZeroStrengthIsIdentity)
{
    EXPECT_FLOAT_EQ(CandleGain(0.0f, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(CandleGain(1.0f, 0.0f), 1.0f);
}

// ---- Orbit sampler (mirror of TextEffectsParticle.cpp SampleOrbit) ----
static constexpr float kOrbitTiltT = 0.45f;

struct OrbitSampleT
{
    float ox, oy, depthAng, sizeMul, alphaMul, tangent;
    bool behindText;
};

static OrbitSampleT SampleOrbitT(float orbitAngle, float radialAnchor, float heightBand, float tilt)
{
    OrbitSampleT s;
    const float c = std::cos(orbitAngle), sn = std::sin(orbitAngle);
    s.ox = c * radialAnchor;
    s.oy = sn * radialAnchor * tilt + heightBand;
    s.depthAng = 0.5f + 0.5f * sn;
    s.sizeMul = 0.78f + 0.40f * s.depthAng;
    s.alphaMul = 0.62f + 0.38f * s.depthAng;
    s.tangent = orbitAngle + 1.5707963f;
    s.behindText = s.depthAng < 0.5f;
    return s;
}

TEST(SampleOrbit, DepthAndScaleWithinBounds)
{
    for (int i = 0; i < 360; ++i)
    {
        const float ang = static_cast<float>(i) * 0.0174533f;
        const OrbitSampleT s = SampleOrbitT(ang, 0.8f, 0.1f, kOrbitTiltT);
        EXPECT_GE(s.depthAng, 0.0f);
        EXPECT_LE(s.depthAng, 1.0f);
        EXPECT_GE(s.sizeMul, 0.78f);
        EXPECT_LE(s.sizeMul, 1.18f + 1e-4f);
        EXPECT_GE(s.alphaMul, 0.62f);
        EXPECT_LE(s.alphaMul, 1.0f + 1e-4f);
    }
}

TEST(SampleOrbit, FrontIsBottomBrighterThanBack)
{
    const OrbitSampleT front = SampleOrbitT(1.5707963f, 0.8f, 0.0f, kOrbitTiltT);  // sin=+1
    const OrbitSampleT back = SampleOrbitT(-1.5707963f, 0.8f, 0.0f, kOrbitTiltT);  // sin=-1
    EXPECT_GT(front.oy, back.oy);  // front lower on screen (larger +y)
    EXPECT_GT(front.alphaMul, back.alphaMul);
    EXPECT_GT(front.sizeMul, back.sizeMul);
    EXPECT_FALSE(front.behindText);
    EXPECT_TRUE(back.behindText);
}

TEST(SampleOrbit, VerticalExtentSquashedByTilt)
{
    const OrbitSampleT top = SampleOrbitT(1.5707963f, 1.0f, 0.0f, kOrbitTiltT);
    EXPECT_NEAR(std::fabs(top.oy), kOrbitTiltT, 1e-4f);  // |oy| = radialAnchor*tilt
}

// ---- Duotone secondary opacity floor (mirror of BadgeTextures RasterizeIcon) ----
static float SecondaryOpacityFloor(float shapeOpacity)
{
    constexpr float kFloor = 0.80f;
    return shapeOpacity < kFloor ? kFloor : shapeOpacity;
}

TEST(IconOpacityFloor, LiftsSecondaryKeepsPrimary)
{
    EXPECT_NEAR(SecondaryOpacityFloor(0.40f), 0.80f, 1e-6f);  // duotone secondary lifted
    EXPECT_NEAR(SecondaryOpacityFloor(1.00f), 1.00f, 1e-6f);  // primary untouched
    EXPECT_NEAR(SecondaryOpacityFloor(0.80f), 0.80f, 1e-6f);  // at floor unchanged
    EXPECT_NEAR(SecondaryOpacityFloor(0.90f), 0.90f, 1e-6f);  // above floor unchanged
}

// ============================================================================
// Long-name fitting -- production width-driven fit from NameFit.hpp
// ============================================================================

TEST(NameFit, OrdinaryNamesKeepAuthoredTypography)
{
    const auto fit = Renderer::NameFit::Compute(Renderer::NameFit::MAX_WIDTH_EM * 100.0f, 100.0f);
    EXPECT_FLOAT_EQ(fit.fontScale, 1.0f);
    EXPECT_FLOAT_EQ(fit.horizontalScale, 1.0f);
}

TEST(NameFit, LongNamesUseBothGentleReductions)
{
    constexpr float fontSize = 100.0f;
    constexpr float measuredWidth = 7.5f * fontSize;
    const auto fit = Renderer::NameFit::Compute(measuredWidth, fontSize);

    EXPECT_LT(fit.fontScale, 1.0f);
    EXPECT_GT(fit.fontScale, Renderer::NameFit::MIN_FONT_SCALE);
    EXPECT_LT(fit.horizontalScale, 1.0f);
    EXPECT_GT(fit.horizontalScale, Renderer::NameFit::MIN_HORIZONTAL_SCALE);
    EXPECT_NEAR(measuredWidth * fit.fontScale * fit.horizontalScale,
                Renderer::NameFit::MAX_WIDTH_EM * fontSize,
                1e-3f);
}

TEST(NameFit, FitIsStableAcrossDistanceScaling)
{
    const auto nearFit = Renderer::NameFit::Compute(760.0f, 100.0f);
    const auto farFit = Renderer::NameFit::Compute(190.0f, 25.0f);

    EXPECT_NEAR(nearFit.fontScale, farFit.fontScale, 1e-6f);
    EXPECT_NEAR(nearFit.horizontalScale, farFit.horizontalScale, 1e-6f);
}

TEST(NameFit, ExtremeNamesRespectLegibilityFloors)
{
    const auto fit = Renderer::NameFit::Compute(2000.0f, 100.0f);
    EXPECT_FLOAT_EQ(fit.fontScale, Renderer::NameFit::MIN_FONT_SCALE);
    EXPECT_FLOAT_EQ(fit.horizontalScale, Renderer::NameFit::MIN_HORIZONTAL_SCALE);
}

TEST(NameFit, InvalidMeasurementsAreSafeNoOps)
{
    const auto zeroWidth = Renderer::NameFit::Compute(.0f, 100.0f);
    const auto zeroFont = Renderer::NameFit::Compute(100.0f, .0f);
    EXPECT_FLOAT_EQ(zeroWidth.fontScale, 1.0f);
    EXPECT_FLOAT_EQ(zeroWidth.horizontalScale, 1.0f);
    EXPECT_FLOAT_EQ(zeroFont.fontScale, 1.0f);
    EXPECT_FLOAT_EQ(zeroFont.horizontalScale, 1.0f);
}

// ============================================================================
// Configurable actor processing limits
// ============================================================================

TEST(ActorLimits, DefaultsRemainTheFormerCompileTimeLimits)
{
    const auto limits = RenderConstants::ClampActorLimits(RenderConstants::DEFAULT_MAX_PLATES,
                                                          RenderConstants::DEFAULT_MAX_SCAN_ACTORS);
    EXPECT_EQ(limits.maxPlates, 16);
    EXPECT_EQ(limits.maxScanActors, 128);
}

TEST(ActorLimits, ValuesAreClampedToSafetyBounds)
{
    const auto low = RenderConstants::ClampActorLimits(-10, -20);
    EXPECT_EQ(low.maxPlates, RenderConstants::MIN_PLATES);
    EXPECT_EQ(low.maxScanActors, RenderConstants::MIN_SCAN_ACTORS);

    const auto high = RenderConstants::ClampActorLimits(10000, 20000);
    EXPECT_EQ(high.maxPlates, RenderConstants::MAX_PLATES);
    EXPECT_EQ(high.maxScanActors, RenderConstants::MAX_SCAN_ACTORS);
}

TEST(ActorLimits, ScanLimitCanAlwaysFillPlateBudget)
{
    const auto limits = RenderConstants::ClampActorLimits(64, 8);
    EXPECT_EQ(limits.maxPlates, 64);
    EXPECT_EQ(limits.maxScanActors, 64);
}
