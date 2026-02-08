/**
 * Unit tests for whois settings parsing using Google Test.
 *
 * Tests INI parsing logic including tier definitions, color parsing,
 * effect type parsing, and format string handling.
 */

#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

// ============================================================================
// Re-implement parsing helpers (same logic as Settings.cpp)
// ============================================================================

static std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) return str;
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static float ParseFloat(const std::string& str, float defaultVal) {
    try {
        return std::stof(str);
    } catch (...) {
        return defaultVal;
    }
}

static int ParseInt(const std::string& str, int defaultVal) {
    try {
        return std::stoi(str);
    } catch (...) {
        return defaultVal;
    }
}

static void ParseColor3(const std::string& str, float out[3]) {
    std::istringstream ss(str);
    std::string token;
    int idx = 0;
    while (std::getline(ss, token, ',') && idx < 3) {
        out[idx++] = ParseFloat(Trim(token), 1.0f);
    }
}

enum class EffectType {
    None,
    Gradient,
    VerticalGradient,
    DiagonalGradient,
    RadialGradient,
    Shimmer,
    ChromaticShimmer,
    PulseGradient,
    RainbowWave,
    ConicRainbow,
    Fire,
    Sparkle,
    Plasma,
    Scanline
};

static EffectType ParseEffectType(const std::string& str) {
    std::string s = Trim(str);
    if (s == "None") return EffectType::None;
    if (s == "Gradient") return EffectType::Gradient;
    if (s == "VerticalGradient") return EffectType::VerticalGradient;
    if (s == "DiagonalGradient") return EffectType::DiagonalGradient;
    if (s == "RadialGradient") return EffectType::RadialGradient;
    if (s == "Shimmer") return EffectType::Shimmer;
    if (s == "ChromaticShimmer") return EffectType::ChromaticShimmer;
    if (s == "PulseGradient") return EffectType::PulseGradient;
    if (s == "RainbowWave") return EffectType::RainbowWave;
    if (s == "ConicRainbow") return EffectType::ConicRainbow;
    if (s == "Fire") return EffectType::Fire;
    if (s == "Sparkle") return EffectType::Sparkle;
    if (s == "Plasma") return EffectType::Plasma;
    if (s == "Scanline") return EffectType::Scanline;
    return EffectType::Gradient;  // Default
}

struct Segment {
    std::string format;
    bool useLevelFont;
};

static std::vector<Segment> ParseFormat(const std::string& val, std::string& titleFormat) {
    std::vector<Segment> segments;
    bool inQuote = false;
    std::string current;

    for (size_t i = 0; i < val.size(); ++i) {
        char c = val[i];
        if (c == '\\' && i + 1 < val.size()) {
            if (inQuote) {
                current += val[++i];
            }
            continue;
        }
        if (c == '"') {
            if (inQuote) {
                if (current.find("%t") != std::string::npos) {
                    titleFormat = current;
                } else {
                    bool isLevel = current.find("%l") != std::string::npos;
                    segments.push_back({current, isLevel});
                }
                current.clear();
                inQuote = false;
            } else {
                inQuote = true;
            }
        } else if (inQuote) {
            current += c;
        }
    }
    return segments;
}

// ============================================================================
// Tests: Trim
// ============================================================================

TEST(TrimTest, RemovesLeadingSpaces) {
    EXPECT_EQ(Trim("   hello"), "hello");
}

TEST(TrimTest, RemovesTrailingSpaces) {
    EXPECT_EQ(Trim("hello   "), "hello");
}

TEST(TrimTest, RemovesBoth) {
    EXPECT_EQ(Trim("   hello   "), "hello");
}

TEST(TrimTest, HandlesTabsAndNewlines) {
    EXPECT_EQ(Trim("\t\nhello\r\n"), "hello");
}

TEST(TrimTest, PreservesInternalSpaces) {
    EXPECT_EQ(Trim("  hello world  "), "hello world");
}

TEST(TrimTest, HandlesEmptyString) {
    std::string result = Trim("");
    EXPECT_TRUE(result.empty() || result == "");
}

// ============================================================================
// Tests: ParseFloat
// ============================================================================

TEST(ParseFloatTest, Valid) {
    EXPECT_NEAR(ParseFloat("3.14", 0.0f), 3.14f, 0.001f);
}

TEST(ParseFloatTest, WithSpaces) {
    EXPECT_NEAR(ParseFloat(" 2.5 ", 0.0f), 2.5f, 0.001f);
}

TEST(ParseFloatTest, Negative) {
    EXPECT_NEAR(ParseFloat("-1.5", 0.0f), -1.5f, 0.001f);
}

TEST(ParseFloatTest, InvalidReturnsDefault) {
    EXPECT_NEAR(ParseFloat("abc", 42.0f), 42.0f, 0.001f);
}

TEST(ParseFloatTest, EmptyReturnsDefault) {
    EXPECT_NEAR(ParseFloat("", 99.0f), 99.0f, 0.001f);
}

// ============================================================================
// Tests: ParseInt
// ============================================================================

TEST(ParseIntTest, Valid) {
    EXPECT_EQ(ParseInt("42", 0), 42);
}

TEST(ParseIntTest, Negative) {
    EXPECT_EQ(ParseInt("-10", 0), -10);
}

TEST(ParseIntTest, InvalidReturnsDefault) {
    EXPECT_EQ(ParseInt("xyz", 99), 99);
}

TEST(ParseIntTest, FloatTruncates) {
    // stoi stops at decimal point
    EXPECT_EQ(ParseInt("3.14", 0), 3);
}

// ============================================================================
// Tests: ParseColor3
// ============================================================================

TEST(ParseColor3Test, RGB) {
    float color[3] = {0, 0, 0};
    ParseColor3("0.5, 0.75, 1.0", color);
    EXPECT_NEAR(color[0], 0.5f, 0.01f);
    EXPECT_NEAR(color[1], 0.75f, 0.01f);
    EXPECT_NEAR(color[2], 1.0f, 0.01f);
}

TEST(ParseColor3Test, NoSpaces) {
    float color[3] = {0, 0, 0};
    ParseColor3("0.1,0.2,0.3", color);
    EXPECT_NEAR(color[0], 0.1f, 0.01f);
    EXPECT_NEAR(color[1], 0.2f, 0.01f);
    EXPECT_NEAR(color[2], 0.3f, 0.01f);
}

TEST(ParseColor3Test, PartialDefaults) {
    float color[3] = {0, 0, 0};
    ParseColor3("0.5", color);
    EXPECT_NEAR(color[0], 0.5f, 0.01f);
    // Only first value parsed, others remain 0
}

// ============================================================================
// Tests: ParseEffectType
// ============================================================================

TEST(ParseEffectTypeTest, None) {
    EXPECT_EQ(ParseEffectType("None"), EffectType::None);
}

TEST(ParseEffectTypeTest, Gradient) {
    EXPECT_EQ(ParseEffectType("Gradient"), EffectType::Gradient);
}

TEST(ParseEffectTypeTest, RainbowWave) {
    EXPECT_EQ(ParseEffectType("RainbowWave"), EffectType::RainbowWave);
}

TEST(ParseEffectTypeTest, Fire) {
    EXPECT_EQ(ParseEffectType("Fire"), EffectType::Fire);
}

TEST(ParseEffectTypeTest, WithWhitespace) {
    EXPECT_EQ(ParseEffectType("  Shimmer  "), EffectType::Shimmer);
}

TEST(ParseEffectTypeTest, UnknownDefaultsToGradient) {
    EXPECT_EQ(ParseEffectType("Unknown"), EffectType::Gradient);
    EXPECT_EQ(ParseEffectType(""), EffectType::Gradient);
}

// ============================================================================
// Tests: ParseFormat
// ============================================================================

TEST(ParseFormatTest, SimpleName) {
    std::string title;
    auto segs = ParseFormat("\"%n\"", title);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].format, "%n");
    EXPECT_FALSE(segs[0].useLevelFont);
}

TEST(ParseFormatTest, NameAndLevel) {
    std::string title;
    auto segs = ParseFormat("\"%n\" \"Lv.%l\"", title);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].format, "%n");
    EXPECT_FALSE(segs[0].useLevelFont);
    EXPECT_EQ(segs[1].format, "Lv.%l");
    EXPECT_TRUE(segs[1].useLevelFont);
}

TEST(ParseFormatTest, ExtractsTitle) {
    std::string title;
    auto segs = ParseFormat("\"%t\" \"%n\"", title);
    ASSERT_EQ(segs.size(), 1u);  // Only %n segment
    EXPECT_EQ(title, "%t");
}

TEST(ParseFormatTest, EscapedQuotes) {
    std::string title;
    auto segs = ParseFormat("\"\\\"hello\\\"\"", title);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].format, "\"hello\"");
}

TEST(ParseFormatTest, Empty) {
    std::string title;
    auto segs = ParseFormat("", title);
    EXPECT_EQ(segs.size(), 0u);
}
