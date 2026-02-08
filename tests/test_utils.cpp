/**
 * Unit tests for whois utility functions using Google Test.
 *
 * Tests core math and color manipulation functions that are independent
 * of the game runtime.
 */

#include <gtest/gtest.h>
#include <cmath>
#include <algorithm>

// ============================================================================
// Re-implement functions under test (same logic as TextEffects.cpp)
// ============================================================================

float Saturate(float x) {
    return std::clamp(x, 0.0f, 1.0f);
}

float SmoothStep(float t) {
    t = Saturate(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// ImGui color format macros
#define IM_COL32_R_SHIFT 0
#define IM_COL32_G_SHIFT 8
#define IM_COL32_B_SHIFT 16
#define IM_COL32_A_SHIFT 24
#define IM_COL32(R,G,B,A) (((unsigned int)(A)<<IM_COL32_A_SHIFT) | ((unsigned int)(B)<<IM_COL32_B_SHIFT) | ((unsigned int)(G)<<IM_COL32_G_SHIFT) | ((unsigned int)(R)<<IM_COL32_R_SHIFT))

using ImU32 = unsigned int;

ImU32 LerpColorU32(ImU32 a, ImU32 b, float t) {
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

static inline float Frac(float x) { return x - std::floor(x); }

struct ImVec4 { float x, y, z, w; };

ImVec4 HSVtoRGB(float h, float s, float v, float a) {
    h = Frac(h);
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(Frac(h * 6.0f) * 2.0f - 1.0f));
    const float m = v - c;
    float r=0, g=0, b=0;
    const int i = (int)std::floor(h * 6.0f);
    switch (i % 6) {
    case 0: r=c; g=x; b=0; break;
    case 1: r=x; g=c; b=0; break;
    case 2: r=0; g=c; b=x; break;
    case 3: r=0; g=x; b=c; break;
    case 4: r=x; g=0; b=c; break;
    case 5: r=c; g=0; b=x; break;
    }
    return {r + m, g + m, b + m, a};
}

// ============================================================================
// Tests: Saturate
// ============================================================================

TEST(SaturateTest, ClampsAboveOne) {
    EXPECT_FLOAT_EQ(Saturate(1.5f), 1.0f);
    EXPECT_FLOAT_EQ(Saturate(100.0f), 1.0f);
}

TEST(SaturateTest, ClampsBelowZero) {
    EXPECT_FLOAT_EQ(Saturate(-0.5f), 0.0f);
    EXPECT_FLOAT_EQ(Saturate(-100.0f), 0.0f);
}

TEST(SaturateTest, PreservesValidRange) {
    EXPECT_FLOAT_EQ(Saturate(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(Saturate(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(Saturate(1.0f), 1.0f);
}

// ============================================================================
// Tests: SmoothStep (quintic)
// ============================================================================

TEST(SmoothStepTest, ReturnsZeroAtZero) {
    EXPECT_FLOAT_EQ(SmoothStep(0.0f), 0.0f);
}

TEST(SmoothStepTest, ReturnsOneAtOne) {
    EXPECT_FLOAT_EQ(SmoothStep(1.0f), 1.0f);
}

TEST(SmoothStepTest, ReturnsHalfAtMidpoint) {
    EXPECT_FLOAT_EQ(SmoothStep(0.5f), 0.5f);
}

TEST(SmoothStepTest, ClampsBelowZero) {
    EXPECT_FLOAT_EQ(SmoothStep(-1.0f), 0.0f);
}

TEST(SmoothStepTest, ClampsAboveOne) {
    EXPECT_FLOAT_EQ(SmoothStep(2.0f), 1.0f);
}

TEST(SmoothStepTest, HasZeroDerivativeAtEdges) {
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

TEST(LerpColorTest, ReturnsFirstAtZero) {
    ImU32 a = IM_COL32(100, 150, 200, 255);
    ImU32 b = IM_COL32(200, 100, 50, 128);
    ImU32 result = LerpColorU32(a, b, 0.0f);
    EXPECT_EQ(result, a);
}

TEST(LerpColorTest, ReturnsSecondAtOne) {
    ImU32 a = IM_COL32(100, 150, 200, 255);
    ImU32 b = IM_COL32(200, 100, 50, 128);
    ImU32 result = LerpColorU32(a, b, 1.0f);
    EXPECT_EQ(result, b);
}

TEST(LerpColorTest, InterpolatesAtHalf) {
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

TEST(LerpColorTest, ClampsT) {
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

TEST(HSVtoRGBTest, RedAtHueZero) {
    ImVec4 rgb = HSVtoRGB(0.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 1.0f, 0.01f);  // R = 1
    EXPECT_NEAR(rgb.y, 0.0f, 0.01f);  // G = 0
    EXPECT_NEAR(rgb.z, 0.0f, 0.01f);  // B = 0
}

TEST(HSVtoRGBTest, GreenAtHueThird) {
    ImVec4 rgb = HSVtoRGB(1.0f/3.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 0.0f, 0.01f);  // R = 0
    EXPECT_NEAR(rgb.y, 1.0f, 0.01f);  // G = 1
    EXPECT_NEAR(rgb.z, 0.0f, 0.01f);  // B = 0
}

TEST(HSVtoRGBTest, BlueAtHueTwoThirds) {
    ImVec4 rgb = HSVtoRGB(2.0f/3.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 0.0f, 0.01f);  // R = 0
    EXPECT_NEAR(rgb.y, 0.0f, 0.01f);  // G = 0
    EXPECT_NEAR(rgb.z, 1.0f, 0.01f);  // B = 1
}

TEST(HSVtoRGBTest, WhiteAtZeroSaturation) {
    ImVec4 rgb = HSVtoRGB(0.5f, 0.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 1.0f, 0.01f);
    EXPECT_NEAR(rgb.y, 1.0f, 0.01f);
    EXPECT_NEAR(rgb.z, 1.0f, 0.01f);
}

TEST(HSVtoRGBTest, BlackAtZeroValue) {
    ImVec4 rgb = HSVtoRGB(0.5f, 1.0f, 0.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 0.0f, 0.01f);
    EXPECT_NEAR(rgb.y, 0.0f, 0.01f);
    EXPECT_NEAR(rgb.z, 0.0f, 0.01f);
}

TEST(HSVtoRGBTest, PreservesAlpha) {
    ImVec4 rgb = HSVtoRGB(0.0f, 1.0f, 1.0f, 0.5f);
    EXPECT_NEAR(rgb.w, 0.5f, 0.01f);
}

TEST(HSVtoRGBTest, WrapsHue) {
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

TEST(FracTest, ReturnsDecimalPart) {
    EXPECT_NEAR(Frac(1.25f), 0.25f, 0.0001f);
    EXPECT_NEAR(Frac(3.75f), 0.75f, 0.0001f);
}

TEST(FracTest, HandlesNegative) {
    EXPECT_NEAR(Frac(-0.25f), 0.75f, 0.0001f);  // -0.25 - floor(-0.25) = -0.25 - (-1) = 0.75
}

TEST(FracTest, HandlesWholeNumbers) {
    EXPECT_NEAR(Frac(5.0f), 0.0f, 0.0001f);
    EXPECT_NEAR(Frac(0.0f), 0.0f, 0.0001f);
}
