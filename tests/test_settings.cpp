// Unit tests for glyph settings parsing using Google Test.
//
// Tests INI parsing logic: tier definitions, color parsing, effect type
// parsing, and format string handling.
//
// The harness links no game code, so the parsing helpers below MIRROR
// src/Settings.cpp instead of calling it. A mirror goes stale silently, so
// change the copy here in the same edit that changes the production parser.

#include "../src/Settings.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// Re-implement parsing helpers (same logic as Settings.cpp)
// ============================================================================

static std::string Trim(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first)
        return str;
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static float ParseFloat(const std::string& str, float defaultVal)
{
    try
    {
        return std::stof(str);
    }
    catch (...)
    {
        return defaultVal;
    }
}

static int ParseInt(const std::string& str, int defaultVal)
{
    try
    {
        return std::stoi(str);
    }
    catch (...)
    {
        return defaultVal;
    }
}

static void ParseColor3(const std::string& str, float out[3])
{
    std::istringstream ss(str);
    std::string token;
    int idx = 0;
    while (std::getline(ss, token, ',') && idx < 3)
    {
        out[idx++] = ParseFloat(Trim(token), 1.0f);
    }
}

enum class EffectType
{
    None,
    Gradient,
    VerticalGradient,
    DiagonalGradient,
    RadialGradient,
    Shimmer,
    Ember,
    Aurora,
    Sparkle,
    Enchant,
    Frost,
    Breathe,
    Drift,
    Mote,
    Wander,
    Eclipse,
    Pulse,
    Electric
};

static EffectType ParseEffectType(const std::string& str)
{
    std::string s = Trim(str);
    if (s == "None")
        return EffectType::None;
    if (s == "Gradient")
        return EffectType::Gradient;
    if (s == "VerticalGradient")
        return EffectType::VerticalGradient;
    if (s == "DiagonalGradient")
        return EffectType::DiagonalGradient;
    if (s == "RadialGradient")
        return EffectType::RadialGradient;
    if (s == "Shimmer")
        return EffectType::Shimmer;
    if (s == "Ember")
        return EffectType::Ember;
    if (s == "Aurora")
        return EffectType::Aurora;
    if (s == "Sparkle")
        return EffectType::Sparkle;
    if (s == "Enchant")
        return EffectType::Enchant;
    if (s == "Frost")
        return EffectType::Frost;
    if (s == "Breathe")
        return EffectType::Breathe;
    if (s == "Drift")
        return EffectType::Drift;
    if (s == "Mote")
        return EffectType::Mote;
    if (s == "Wander")
        return EffectType::Wander;
    if (s == "Eclipse")
        return EffectType::Eclipse;
    if (s == "Pulse")
        return EffectType::Pulse;
    if (s == "Electric")
        return EffectType::Electric;
    return EffectType::Gradient;  // Default
}

struct Segment
{
    std::string format;
    bool useLevelFont = false;
    bool dropIfBlank = false;  // Set by trailing "?" after closing quote.
};

// Mirrors ParseQuotedSegments in Settings.cpp. `forceLevelFont` is the
// InfoFormat-row mode; when false (Format-row), title segments (`%t`) are
// extracted into `titleFormat` and other segments use level font iff they
// contain `%l`.
static std::vector<Segment> ParseFormat(const std::string& val,
                                        std::string& titleFormat,
                                        bool forceLevelFont = false)
{
    std::vector<Segment> segments;
    bool inQuote = false;
    bool justClosed = false;
    std::string current;
    Segment* lastPushed = nullptr;

    for (size_t i = 0; i < val.size(); ++i)
    {
        char c = val[i];
        if (c == '\\' && i + 1 < val.size())
        {
            if (inQuote)
            {
                current += val[++i];
            }
            justClosed = false;
            continue;
        }
        if (c == '"')
        {
            if (inQuote)
            {
                if (!forceLevelFont && current.find("%t") != std::string::npos)
                {
                    titleFormat = current;
                    lastPushed = nullptr;
                }
                else
                {
                    bool isLevel = forceLevelFont || current.find("%l") != std::string::npos;
                    segments.push_back({current, isLevel, false});
                    lastPushed = &segments.back();
                }
                current.clear();
                inQuote = false;
                justClosed = true;
            }
            else
            {
                inQuote = true;
                justClosed = false;
            }
            continue;
        }
        if (inQuote)
        {
            current += c;
            justClosed = false;
            continue;
        }
        if (justClosed && c == '?')
        {
            if (lastPushed != nullptr)
            {
                lastPushed->dropIfBlank = true;
            }
        }
        justClosed = false;
    }
    return segments;
}

// ============================================================================
// Tests: Trim
// ============================================================================

TEST(TrimTest, RemovesLeadingSpaces)
{
    EXPECT_EQ(Trim("   hello"), "hello");
}

TEST(TrimTest, RemovesTrailingSpaces)
{
    EXPECT_EQ(Trim("hello   "), "hello");
}

TEST(TrimTest, RemovesBoth)
{
    EXPECT_EQ(Trim("   hello   "), "hello");
}

TEST(TrimTest, HandlesTabsAndNewlines)
{
    EXPECT_EQ(Trim("\t\nhello\r\n"), "hello");
}

TEST(TrimTest, PreservesInternalSpaces)
{
    EXPECT_EQ(Trim("  hello world  "), "hello world");
}

TEST(TrimTest, HandlesEmptyString)
{
    std::string result = Trim("");
    EXPECT_TRUE(result.empty() || result == "");
}

// ============================================================================
// Tests: ParseFloat
// ============================================================================

TEST(ParseFloatTest, Valid)
{
    EXPECT_NEAR(ParseFloat("3.14", 0.0f), 3.14f, 0.001f);
}

TEST(ParseFloatTest, WithSpaces)
{
    EXPECT_NEAR(ParseFloat(" 2.5 ", 0.0f), 2.5f, 0.001f);
}

TEST(ParseFloatTest, Negative)
{
    EXPECT_NEAR(ParseFloat("-1.5", 0.0f), -1.5f, 0.001f);
}

TEST(ParseFloatTest, InvalidReturnsDefault)
{
    EXPECT_NEAR(ParseFloat("abc", 42.0f), 42.0f, 0.001f);
}

TEST(ParseFloatTest, EmptyReturnsDefault)
{
    EXPECT_NEAR(ParseFloat("", 99.0f), 99.0f, 0.001f);
}

// ============================================================================
// Tests: ParseInt
// ============================================================================

TEST(ParseIntTest, Valid)
{
    EXPECT_EQ(ParseInt("42", 0), 42);
}

TEST(ParseIntTest, Negative)
{
    EXPECT_EQ(ParseInt("-10", 0), -10);
}

TEST(ParseIntTest, InvalidReturnsDefault)
{
    EXPECT_EQ(ParseInt("xyz", 99), 99);
}

TEST(ParseIntTest, FloatTruncates)
{
    // stoi stops at decimal point
    EXPECT_EQ(ParseInt("3.14", 0), 3);
}

// ============================================================================
// Tests: ParseColor3
// ============================================================================

TEST(ParseColor3Test, RGB)
{
    float color[3] = {0, 0, 0};
    ParseColor3("0.5, 0.75, 1.0", color);
    EXPECT_NEAR(color[0], 0.5f, 0.01f);
    EXPECT_NEAR(color[1], 0.75f, 0.01f);
    EXPECT_NEAR(color[2], 1.0f, 0.01f);
}

TEST(ParseColor3Test, NoSpaces)
{
    float color[3] = {0, 0, 0};
    ParseColor3("0.1,0.2,0.3", color);
    EXPECT_NEAR(color[0], 0.1f, 0.01f);
    EXPECT_NEAR(color[1], 0.2f, 0.01f);
    EXPECT_NEAR(color[2], 0.3f, 0.01f);
}

TEST(ParseColor3Test, PartialDefaults)
{
    float color[3] = {0, 0, 0};
    ParseColor3("0.5", color);
    EXPECT_NEAR(color[0], 0.5f, 0.01f);
    // Only first value parsed, others remain 0
}

// Regression guard on the new always-on badge slot color defaults (these
// strings live in Settings.cpp's binding table; a typo would ship a bad tint).
TEST(ParseColor3Test, NewBadgeSlotDefaults)
{
    float muted[3] = {0, 0, 0};
    ParseColor3("0.62, 0.64, 0.68", muted);  // IconMutedColor
    EXPECT_NEAR(muted[0], 0.62f, 0.01f);
    EXPECT_NEAR(muted[1], 0.64f, 0.01f);
    EXPECT_NEAR(muted[2], 0.68f, 0.01f);

    float combat[3] = {0, 0, 0};
    ParseColor3("0.88, 0.42, 0.30", combat);  // IconCombatColor
    EXPECT_NEAR(combat[0], 0.88f, 0.01f);
    EXPECT_NEAR(combat[1], 0.42f, 0.01f);
    EXPECT_NEAR(combat[2], 0.30f, 0.01f);
}

// Mirror of Settings.cpp ParseEffectString's positional-parameter splitting
// (post-fix). The bug was that an empty comma-separated field did `continue`
// without advancing the index, collapsing later values into earlier slots. The
// fix advances the index on every field so an empty field keeps its slot (the
// param stays at its default).
static void ParseEffectParams(const std::string& params, float out[5])
{
    std::istringstream paramStream(params);
    std::string token;
    int paramIdx = 0;
    while (std::getline(paramStream, token, ',') && paramIdx < 5)
    {
        token = Trim(token);
        if (!token.empty())
        {
            out[paramIdx] = ParseFloat(token, 0.0f);
        }
        paramIdx++;
    }
}

// ============================================================================
// Tests: ParseEffectParams (positional slots survive empty fields)
// ============================================================================

TEST(ParseEffectParamsTest, EmptyFieldPreservesPositionalSlot)
{
    float out[5] = {0, 0, 0, 0, 0};
    ParseEffectParams("0.5,,0.85", out);  // e.g. "Aurora 0.5,,0.85"
    EXPECT_NEAR(out[0], 0.5f, 0.0001f);
    EXPECT_NEAR(out[1], 0.0f, 0.0001f);  // empty field keeps its default
    EXPECT_NEAR(out[2], 0.85f, 0.0001f);
}

TEST(ParseEffectParamsTest, LeadingEmptyField)
{
    float out[5] = {0, 0, 0, 0, 0};
    ParseEffectParams(",0.5", out);
    EXPECT_NEAR(out[0], 0.0f, 0.0001f);
    EXPECT_NEAR(out[1], 0.5f, 0.0001f);
}

TEST(ParseEffectParamsTest, TrailingEmptyField)
{
    float out[5] = {0, 0, 0, 0, 0};
    ParseEffectParams("0.5,", out);
    EXPECT_NEAR(out[0], 0.5f, 0.0001f);
    EXPECT_NEAR(out[1], 0.0f, 0.0001f);
}

TEST(ParseEffectParamsTest, AllFieldsPopulated)
{
    float out[5] = {0, 0, 0, 0, 0};
    ParseEffectParams("1,2,3,4,5", out);
    EXPECT_NEAR(out[0], 1.0f, 0.0001f);
    EXPECT_NEAR(out[1], 2.0f, 0.0001f);
    EXPECT_NEAR(out[2], 3.0f, 0.0001f);
    EXPECT_NEAR(out[3], 4.0f, 0.0001f);
    EXPECT_NEAR(out[4], 5.0f, 0.0001f);
}

// ============================================================================
// Tests: ParseEffectType
// ============================================================================

TEST(ParseEffectTypeTest, None)
{
    EXPECT_EQ(ParseEffectType("None"), EffectType::None);
}

TEST(ParseEffectTypeTest, Gradient)
{
    EXPECT_EQ(ParseEffectType("Gradient"), EffectType::Gradient);
}

TEST(ParseEffectTypeTest, Aurora)
{
    EXPECT_EQ(ParseEffectType("Aurora"), EffectType::Aurora);
}

TEST(ParseEffectTypeTest, Ember)
{
    EXPECT_EQ(ParseEffectType("Ember"), EffectType::Ember);
}

TEST(ParseEffectTypeTest, Enchant)
{
    EXPECT_EQ(ParseEffectType("Enchant"), EffectType::Enchant);
}

TEST(ParseEffectTypeTest, Frost)
{
    EXPECT_EQ(ParseEffectType("Frost"), EffectType::Frost);
}

TEST(ParseEffectTypeTest, Breathe)
{
    EXPECT_EQ(ParseEffectType("Breathe"), EffectType::Breathe);
}

TEST(ParseEffectTypeTest, Drift)
{
    EXPECT_EQ(ParseEffectType("Drift"), EffectType::Drift);
}

TEST(ParseEffectTypeTest, Mote)
{
    EXPECT_EQ(ParseEffectType("Mote"), EffectType::Mote);
}

TEST(ParseEffectTypeTest, Wander)
{
    EXPECT_EQ(ParseEffectType("Wander"), EffectType::Wander);
}

TEST(ParseEffectTypeTest, Eclipse)
{
    EXPECT_EQ(ParseEffectType("Eclipse"), EffectType::Eclipse);
}

TEST(ParseEffectTypeTest, Pulse)
{
    EXPECT_EQ(ParseEffectType("Pulse"), EffectType::Pulse);
}

TEST(ParseEffectTypeTest, Electric)
{
    EXPECT_EQ(ParseEffectType("Electric"), EffectType::Electric);
}

TEST(ParseEffectTypeTest, WithWhitespace)
{
    EXPECT_EQ(ParseEffectType("  Shimmer  "), EffectType::Shimmer);
}

TEST(ParseEffectTypeTest, UnknownDefaultsToGradient)
{
    EXPECT_EQ(ParseEffectType("Unknown"), EffectType::Gradient);
    EXPECT_EQ(ParseEffectType(""), EffectType::Gradient);
}

// ============================================================================
// Tests: ParseFormat
// ============================================================================

TEST(ParseFormatTest, SimpleName)
{
    std::string title;
    auto segs = ParseFormat("\"%n\"", title);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].format, "%n");
    EXPECT_FALSE(segs[0].useLevelFont);
}

TEST(ParseFormatTest, NameAndLevel)
{
    std::string title;
    auto segs = ParseFormat("\"%n\" \"Lv.%l\"", title);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].format, "%n");
    EXPECT_FALSE(segs[0].useLevelFont);
    EXPECT_EQ(segs[1].format, "Lv.%l");
    EXPECT_TRUE(segs[1].useLevelFont);
}

TEST(ParseFormatTest, ExtractsTitle)
{
    std::string title;
    auto segs = ParseFormat("\"%t\" \"%n\"", title);
    ASSERT_EQ(segs.size(), 1u);  // Only %n segment
    EXPECT_EQ(title, "%t");
}

TEST(ParseFormatTest, EscapedQuotes)
{
    std::string title;
    auto segs = ParseFormat("\"\\\"hello\\\"\"", title);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].format, "\"hello\"");
}

TEST(ParseFormatTest, Empty)
{
    std::string title;
    auto segs = ParseFormat("", title);
    EXPECT_EQ(segs.size(), 0u);
}

TEST(ParseFormatTest, DropIfBlankDefaultsToFalse)
{
    std::string title;
    auto segs = ParseFormat("\"%n\"", title);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_FALSE(segs[0].dropIfBlank);
}

TEST(ParseFormatTest, TrailingQuestionMarkSetsDropIfBlank)
{
    std::string title;
    auto segs = ParseFormat("\"%n\" \"%r\"?", title);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].format, "%n");
    EXPECT_FALSE(segs[0].dropIfBlank);
    EXPECT_EQ(segs[1].format, "%r");
    EXPECT_TRUE(segs[1].dropIfBlank);
}

TEST(ParseFormatTest, MultipleDropIfBlankSegments)
{
    std::string title;
    auto segs = ParseFormat("\"%n\" \"  %r\"? \"  %d\"? \"  %c\"?", title);
    ASSERT_EQ(segs.size(), 4u);
    EXPECT_FALSE(segs[0].dropIfBlank);
    EXPECT_TRUE(segs[1].dropIfBlank);
    EXPECT_TRUE(segs[2].dropIfBlank);
    EXPECT_TRUE(segs[3].dropIfBlank);
}

TEST(ParseFormatTest, QuestionMarkAfterWhitespaceIsLiteral)
{
    // The "?" must be immediately adjacent to the closing quote.
    // "?" separated from the close by whitespace is ignored (parser eats it
    // along with other non-quote, non-special chars outside quotes).
    std::string title;
    auto segs = ParseFormat("\"%n\" ? \"%r\"", title);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_FALSE(segs[0].dropIfBlank);
    EXPECT_FALSE(segs[1].dropIfBlank);
}

TEST(ParseFormatTest, BackToBackDropSegments)
{
    // "%n"?"%r"? -- no whitespace between segments must still work.
    std::string title;
    auto segs = ParseFormat("\"%n\"?\"%r\"?", title);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_TRUE(segs[0].dropIfBlank);
    EXPECT_TRUE(segs[1].dropIfBlank);
}

TEST(ParseFormatTest, TitleSegmentNotMarkedDroppable)
{
    // "?" after a title-bearing segment must not affect the display vector
    // (title is hoisted, no segment is pushed).
    std::string title;
    auto segs = ParseFormat("\"%t\"? \"%n\"", title);
    EXPECT_EQ(title, "%t");
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_FALSE(segs[0].dropIfBlank);
}

TEST(ParseFormatTest, EscapedQuotesDoNotTriggerJustClosed)
{
    // The closing quote of a segment containing an escaped \" must still
    // accept a trailing "?" -- escapes inside quotes don't break tracking.
    std::string title;
    auto segs = ParseFormat("\"\\\"hi\\\"\"?", title);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].format, "\"hi\"");
    EXPECT_TRUE(segs[0].dropIfBlank);
}

// ============================================================================
// Tests: ParseFormat -- InfoFormat mode (forceLevelFont = true)
// ============================================================================

TEST(InfoFormatTest, ForceLevelFontOverridesAutoDetect)
{
    std::string title;
    auto segs = ParseFormat("\"%r\" \"%d\" \"Lv.%l\"", title, /*forceLevelFont*/ true);
    ASSERT_EQ(segs.size(), 3u);
    EXPECT_TRUE(segs[0].useLevelFont);
    EXPECT_TRUE(segs[1].useLevelFont);
    EXPECT_TRUE(segs[2].useLevelFont);
}

TEST(InfoFormatTest, NoTitleExtractionInInfoMode)
{
    // In InfoFormat mode, segments with %t are NOT hoisted to title;
    // they render inline like any other segment.
    std::string title;
    auto segs = ParseFormat("\"%t\" \"%r\"", title, /*forceLevelFont*/ true);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].format, "%t");
    EXPECT_TRUE(title.empty());
}

TEST(InfoFormatTest, DropIfBlankStillWorks)
{
    std::string title;
    auto segs = ParseFormat("\"%r\"? \"  %d\"? \"  %c\"?", title, /*forceLevelFont*/ true);
    ASSERT_EQ(segs.size(), 3u);
    for (const auto& s : segs)
    {
        EXPECT_TRUE(s.useLevelFont);
        EXPECT_TRUE(s.dropIfBlank);
    }
}

// ============================================================================
// Focus Target Expanded Nameplate settings
// ============================================================================
//
// Mirrors Settings::FocusSettings + the clamping rules registered for it in
// Settings.cpp's kSettings binding table.  Production code uses a descriptor
// table for clamping; here we re-implement the same rules so we can validate
// them without linking the SKSE plugin.

namespace focus_test
{

struct FocusSettings
{
    bool Enabled = false;
    float ConeAngleDegrees = 8.0f;
    float MaxDistance = 0.0f;
    float AmbientDimFactor = 0.55f;
    float SettleTime = 0.25f;
    bool IgnoreOccluded = true;
};

// Apply the same per-field validation rules as kSettings in Settings.cpp.
static void ClampFocus(FocusSettings& f)
{
    f.ConeAngleDegrees = std::clamp(f.ConeAngleDegrees, 0.5f, 45.0f);
    f.AmbientDimFactor = std::clamp(f.AmbientDimFactor, 0.05f, 1.0f);
    f.SettleTime = std::clamp(f.SettleTime, 0.0f, 2.0f);
    if (f.MaxDistance < 0.0f)
    {
        f.MaxDistance = 0.0f;
    }
}

// Mirror of Settings.cpp's ParseBool -- accept true/false/1/0/yes/no.
static bool ParseBool(const std::string& s)
{
    std::string lower;
    lower.reserve(s.size());
    for (char c : s)
    {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

}  // namespace focus_test

TEST(FocusSettings, DefaultsMatchProduction)
{
    focus_test::FocusSettings f;
    EXPECT_FALSE(f.Enabled);
    EXPECT_FLOAT_EQ(f.ConeAngleDegrees, 8.0f);
    EXPECT_FLOAT_EQ(f.MaxDistance, 0.0f);
    EXPECT_FLOAT_EQ(f.AmbientDimFactor, 0.55f);
    EXPECT_FLOAT_EQ(f.SettleTime, 0.25f);
    EXPECT_TRUE(f.IgnoreOccluded);
}

TEST(FocusSettings, ClampConeAngleBelowMin)
{
    focus_test::FocusSettings f;
    f.ConeAngleDegrees = 0.1f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.ConeAngleDegrees, 0.5f);
}

TEST(FocusSettings, ClampConeAngleAboveMax)
{
    focus_test::FocusSettings f;
    f.ConeAngleDegrees = 90.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.ConeAngleDegrees, 45.0f);
}

TEST(FocusSettings, ConeAngleInRangePreserved)
{
    focus_test::FocusSettings f;
    f.ConeAngleDegrees = 12.5f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.ConeAngleDegrees, 12.5f);
}

TEST(FocusSettings, ClampAmbientDimFactorBelowMin)
{
    focus_test::FocusSettings f;
    f.AmbientDimFactor = 0.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.AmbientDimFactor, 0.05f);
}

TEST(FocusSettings, ClampAmbientDimFactorAboveMax)
{
    focus_test::FocusSettings f;
    f.AmbientDimFactor = 2.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.AmbientDimFactor, 1.0f);
}

TEST(FocusSettings, ClampSettleTimeNegative)
{
    focus_test::FocusSettings f;
    f.SettleTime = -0.5f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.SettleTime, 0.0f);
}

TEST(FocusSettings, ClampSettleTimeAboveMax)
{
    focus_test::FocusSettings f;
    f.SettleTime = 5.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.SettleTime, 2.0f);
}

TEST(FocusSettings, SettleTimeZeroAllowed)
{
    // SettleTime = 0 is a legitimate "instant focus" mode -- must not be clamped up.
    focus_test::FocusSettings f;
    f.SettleTime = 0.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.SettleTime, 0.0f);
}

TEST(FocusSettings, MaxDistanceZeroIsSentinel)
{
    // 0.0 means "reuse MaxScanDistance"; it must survive validation unchanged.
    focus_test::FocusSettings f;
    f.MaxDistance = 0.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.MaxDistance, 0.0f);
}

TEST(FocusSettings, MaxDistanceNegativeClampedToZero)
{
    focus_test::FocusSettings f;
    f.MaxDistance = -100.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.MaxDistance, 0.0f);
}

TEST(FocusSettings, MaxDistancePositivePreserved)
{
    focus_test::FocusSettings f;
    f.MaxDistance = 1500.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.MaxDistance, 1500.0f);
}

TEST(FocusSettings, BoolParseTrueVariants)
{
    EXPECT_TRUE(focus_test::ParseBool("true"));
    EXPECT_TRUE(focus_test::ParseBool("True"));
    EXPECT_TRUE(focus_test::ParseBool("TRUE"));
    EXPECT_TRUE(focus_test::ParseBool("1"));
    EXPECT_TRUE(focus_test::ParseBool("yes"));
    EXPECT_TRUE(focus_test::ParseBool("on"));
}

TEST(FocusSettings, BoolParseFalseVariants)
{
    EXPECT_FALSE(focus_test::ParseBool("false"));
    EXPECT_FALSE(focus_test::ParseBool("False"));
    EXPECT_FALSE(focus_test::ParseBool("0"));
    EXPECT_FALSE(focus_test::ParseBool("no"));
    EXPECT_FALSE(focus_test::ParseBool(""));
    EXPECT_FALSE(focus_test::ParseBool("anything-else"));
}

TEST(DisplaySettings, ActorLimitDefaultsMatchRenderDefaults)
{
    const Settings::DisplaySettings display;
    EXPECT_EQ(display.MaxPlates, RenderConstants::DEFAULT_MAX_PLATES);
    EXPECT_EQ(display.MaxScanActors, RenderConstants::DEFAULT_MAX_SCAN_ACTORS);
}

// ============================================================================
// Graffito world-plane text settings
// ============================================================================
//
// The settings value type is production code. The clamp helper mirrors the
// validation rules registered in Settings.cpp's kSettings binding table; the
// production plugin itself is not linked into this lightweight test executable.

namespace graffito_test
{

static void ClampGraffito(Settings::GraffitoSettings& g)
{
    g.Scale = std::clamp(g.Scale, 0.25f, 4.0f);
    g.PlayerScale = std::clamp(g.PlayerScale, 0.25f, 2.0f);
    g.ForwardOffset = std::clamp(g.ForwardOffset, 0.0f, 32.0f);
    g.FacingFadeDegrees = std::clamp(g.FacingFadeDegrees, 0.0f, 45.0f);
    g.BacksideBleedAlpha = std::clamp(g.BacksideBleedAlpha, 0.0f, 0.25f);
    g.EdgeSeamAlpha = std::clamp(g.EdgeSeamAlpha, 0.0f, 0.4f);
    g.FolioReverseAlpha = std::clamp(g.FolioReverseAlpha, 0.0f, 1.0f);
    g.FolioSpineAlpha = std::clamp(g.FolioSpineAlpha, 0.0f, 1.0f);
    g.FolioDepth = std::clamp(g.FolioDepth, 0.0f, 28.0f);
    g.WrapDegrees = std::clamp(g.WrapDegrees, 0.0f, 140.0f);
    g.FisheyeStrength = std::clamp(g.FisheyeStrength, 0.0f, 1.0f);
    g.LayerDepth = std::clamp(g.LayerDepth, 0.0f, 0.06f);
    g.EdgeSheen = std::clamp(g.EdgeSheen, 0.0f, 0.4f);
    g.OrientationSettleTime = std::clamp(g.OrientationSettleTime, 0.0f, 2.0f);
    g.MaxDistance = std::max(g.MaxDistance, 0.0f);
    g.EpitaphGroundLift = std::clamp(g.EpitaphGroundLift, 0.0f, 16.0f);
}

}  // namespace graffito_test

TEST(GraffitoSettings, DefaultsMatchProduction)
{
    Settings::GraffitoSettings g;
    EXPECT_FALSE(g.Enabled);
    EXPECT_FLOAT_EQ(g.Scale, 1.0f);
    EXPECT_FLOAT_EQ(g.PlayerScale, 0.72f);
    EXPECT_FLOAT_EQ(g.ForwardOffset, 4.0f);
    EXPECT_FLOAT_EQ(g.FacingFadeDegrees, 15.0f);
    EXPECT_FLOAT_EQ(g.BacksideBleedAlpha, 0.12f);
    EXPECT_FLOAT_EQ(g.EdgeSeamAlpha, 0.22f);
    EXPECT_TRUE(g.FolioEnabled);
    EXPECT_FLOAT_EQ(g.FolioReverseAlpha, 0.72f);
    EXPECT_FLOAT_EQ(g.FolioSpineAlpha, 0.88f);
    EXPECT_FLOAT_EQ(g.FolioDepth, 2.0f);
    EXPECT_FLOAT_EQ(g.WrapDegrees, 55.0f);
    EXPECT_FLOAT_EQ(g.FisheyeStrength, 0.62f);
    EXPECT_FLOAT_EQ(g.LayerDepth, 0.0f);
    EXPECT_FLOAT_EQ(g.EdgeSheen, 0.14f);
    EXPECT_FLOAT_EQ(g.OrientationSettleTime, 0.18f);
    EXPECT_FLOAT_EQ(g.MaxDistance, 1200.0f);
    EXPECT_TRUE(g.FallenEpitaphEnabled);
    EXPECT_FLOAT_EQ(g.EpitaphGroundLift, 2.0f);
}

TEST(GraffitoSettings, ClampScaleRange)
{
    Settings::GraffitoSettings g;
    g.Scale = 0.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.Scale, 0.25f);
    g.Scale = 12.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.Scale, 4.0f);
}

TEST(GraffitoSettings, ClampPlayerScaleRange)
{
    Settings::GraffitoSettings g;
    g.PlayerScale = 0.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.PlayerScale, 0.25f);
    g.PlayerScale = 4.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.PlayerScale, 2.0f);
}

TEST(GraffitoSettings, ClampForwardOffsetRange)
{
    Settings::GraffitoSettings g;
    g.ForwardOffset = -1.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.ForwardOffset, 0.0f);
    g.ForwardOffset = 64.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.ForwardOffset, 32.0f);
}

TEST(GraffitoSettings, ClampFacingFadeDegreesRange)
{
    Settings::GraffitoSettings g;
    g.FacingFadeDegrees = -5.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.FacingFadeDegrees, 0.0f);
    g.FacingFadeDegrees = 90.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.FacingFadeDegrees, 45.0f);
}

TEST(GraffitoSettings, FacingFadeZeroAllowsHardTransition)
{
    Settings::GraffitoSettings g;
    g.FacingFadeDegrees = 0.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.FacingFadeDegrees, 0.0f);
}

TEST(GraffitoSettings, ClampReverseInkAlphaRanges)
{
    Settings::GraffitoSettings g;
    g.BacksideBleedAlpha = -1.0f;
    g.EdgeSeamAlpha = 2.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.BacksideBleedAlpha, 0.0f);
    EXPECT_FLOAT_EQ(g.EdgeSeamAlpha, 0.4f);

    g.BacksideBleedAlpha = 1.0f;
    g.EdgeSeamAlpha = -1.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.BacksideBleedAlpha, 0.25f);
    EXPECT_FLOAT_EQ(g.EdgeSeamAlpha, 0.0f);
}

TEST(GraffitoSettings, ClampFolioAlphaRanges)
{
    Settings::GraffitoSettings g;
    g.FolioReverseAlpha = -1.0f;
    g.FolioSpineAlpha = 2.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.FolioReverseAlpha, 0.0f);
    EXPECT_FLOAT_EQ(g.FolioSpineAlpha, 1.0f);

    g.FolioReverseAlpha = 2.0f;
    g.FolioSpineAlpha = -1.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.FolioReverseAlpha, 1.0f);
    EXPECT_FLOAT_EQ(g.FolioSpineAlpha, 0.0f);
}

TEST(GraffitoSettings, FolioDepthAllowsZeroAndClampsRange)
{
    Settings::GraffitoSettings g;
    g.FolioDepth = -1.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.FolioDepth, 0.0f);

    g.FolioDepth = 0.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.FolioDepth, 0.0f);

    g.FolioDepth = 2.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.FolioDepth, 2.0f);

    g.FolioDepth = 100.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.FolioDepth, 28.0f);
}

TEST(GraffitoSettings, ClampDimensionalFrontRanges)
{
    Settings::GraffitoSettings g;
    g.WrapDegrees = -1.0f;
    g.FisheyeStrength = -1.0f;
    g.LayerDepth = 1.0f;
    g.EdgeSheen = -1.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.WrapDegrees, 0.0f);
    EXPECT_FLOAT_EQ(g.FisheyeStrength, 0.0f);
    EXPECT_FLOAT_EQ(g.LayerDepth, 0.06f);
    EXPECT_FLOAT_EQ(g.EdgeSheen, 0.0f);

    g.WrapDegrees = 180.0f;
    g.FisheyeStrength = 2.0f;
    g.LayerDepth = -1.0f;
    g.EdgeSheen = 1.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.WrapDegrees, 140.0f);
    EXPECT_FLOAT_EQ(g.FisheyeStrength, 1.0f);
    EXPECT_FLOAT_EQ(g.LayerDepth, 0.0f);
    EXPECT_FLOAT_EQ(g.EdgeSheen, 0.4f);
}

TEST(GraffitoSettings, FolioCanBeDisabledWithoutChangingSingleSheetControls)
{
    Settings::GraffitoSettings g;
    g.FolioEnabled = false;
    g.BacksideBleedAlpha = 0.08f;
    g.EdgeSeamAlpha = 0.30f;
    graffito_test::ClampGraffito(g);

    EXPECT_FALSE(g.FolioEnabled);
    EXPECT_FLOAT_EQ(g.BacksideBleedAlpha, 0.08f);
    EXPECT_FLOAT_EQ(g.EdgeSeamAlpha, 0.30f);
}

TEST(GraffitoSettings, ClampOrientationSettleTimeRange)
{
    Settings::GraffitoSettings g;
    g.OrientationSettleTime = -1.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.OrientationSettleTime, 0.0f);
    g.OrientationSettleTime = 5.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.OrientationSettleTime, 2.0f);
}

TEST(GraffitoSettings, OrientationSettleTimeZeroIsInstant)
{
    Settings::GraffitoSettings g;
    g.OrientationSettleTime = 0.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.OrientationSettleTime, 0.0f);
}

TEST(GraffitoSettings, MaxDistanceZeroIsSentinel)
{
    Settings::GraffitoSettings g;
    g.MaxDistance = 0.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.MaxDistance, 0.0f);
}

TEST(GraffitoSettings, MaxDistanceNegativeClampedToZero)
{
    Settings::GraffitoSettings g;
    g.MaxDistance = -100.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.MaxDistance, 0.0f);
}

TEST(GraffitoSettings, ClampEpitaphGroundLiftRange)
{
    Settings::GraffitoSettings g;
    g.EpitaphGroundLift = -1.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.EpitaphGroundLift, 0.0f);
    g.EpitaphGroundLift = 50.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.EpitaphGroundLift, 16.0f);
}

TEST(GraffitoSettings, InRangeValuesPreserved)
{
    Settings::GraffitoSettings g;
    g.Scale = 1.5f;
    g.PlayerScale = 0.8f;
    g.ForwardOffset = 8.0f;
    g.FacingFadeDegrees = 20.0f;
    g.BacksideBleedAlpha = 0.08f;
    g.EdgeSeamAlpha = 0.3f;
    g.FolioEnabled = false;
    g.FolioReverseAlpha = 0.42f;
    g.FolioSpineAlpha = 0.65f;
    g.FolioDepth = 0.32f;
    g.WrapDegrees = 112.0f;
    g.FisheyeStrength = 0.65f;
    g.LayerDepth = 0.05f;
    g.EdgeSheen = 0.2f;
    g.OrientationSettleTime = 0.3f;
    g.MaxDistance = 900.0f;
    g.FallenEpitaphEnabled = false;
    g.EpitaphGroundLift = 4.0f;
    graffito_test::ClampGraffito(g);
    EXPECT_FLOAT_EQ(g.Scale, 1.5f);
    EXPECT_FLOAT_EQ(g.PlayerScale, 0.8f);
    EXPECT_FLOAT_EQ(g.ForwardOffset, 8.0f);
    EXPECT_FLOAT_EQ(g.FacingFadeDegrees, 20.0f);
    EXPECT_FLOAT_EQ(g.BacksideBleedAlpha, 0.08f);
    EXPECT_FLOAT_EQ(g.EdgeSeamAlpha, 0.3f);
    EXPECT_FALSE(g.FolioEnabled);
    EXPECT_FLOAT_EQ(g.FolioReverseAlpha, 0.42f);
    EXPECT_FLOAT_EQ(g.FolioSpineAlpha, 0.65f);
    EXPECT_FLOAT_EQ(g.FolioDepth, 0.32f);
    EXPECT_FLOAT_EQ(g.WrapDegrees, 112.0f);
    EXPECT_FLOAT_EQ(g.FisheyeStrength, 0.65f);
    EXPECT_FLOAT_EQ(g.LayerDepth, 0.05f);
    EXPECT_FLOAT_EQ(g.EdgeSheen, 0.2f);
    EXPECT_FLOAT_EQ(g.OrientationSettleTime, 0.3f);
    EXPECT_FLOAT_EQ(g.MaxDistance, 900.0f);
    EXPECT_FALSE(g.FallenEpitaphEnabled);
    EXPECT_FLOAT_EQ(g.EpitaphGroundLift, 4.0f);
}

// ============================================================================
// Soft directional drop-shadow settings
// ============================================================================
//
// Mirrors the soft-shadow fields of Settings::ShadowOutlineSettings + the
// clamping rules registered for them in Settings.cpp's kSettings binding table.

namespace soft_shadow_test
{

struct SoftShadowSettings
{
    bool Enabled = false;
    float Distance = 4.0f;
    float Softness = 3.0f;
    float Opacity = 0.8f;
    float Angle = 45.0f;
    int Samples = 12;
};

// Apply the same per-field validation rules as kSettings in Settings.cpp.
static void ClampSoftShadow(SoftShadowSettings& s)
{
    s.Distance = std::clamp(s.Distance, 0.0f, 16.0f);
    s.Softness = std::clamp(s.Softness, 0.0f, 12.0f);
    s.Opacity = std::clamp(s.Opacity, 0.0f, 1.0f);
    s.Angle = std::clamp(s.Angle, 0.0f, 360.0f);
    s.Samples = std::clamp(s.Samples, 4, 24);
}

}  // namespace soft_shadow_test

TEST(SoftShadow, DefaultsMatchProduction)
{
    soft_shadow_test::SoftShadowSettings s;
    EXPECT_FALSE(s.Enabled);
    EXPECT_FLOAT_EQ(s.Distance, 4.0f);
    EXPECT_FLOAT_EQ(s.Softness, 3.0f);
    EXPECT_FLOAT_EQ(s.Opacity, 0.8f);
    EXPECT_FLOAT_EQ(s.Angle, 45.0f);
    EXPECT_EQ(s.Samples, 12);
}

TEST(SoftShadow, ClampDistanceRange)
{
    soft_shadow_test::SoftShadowSettings s;
    s.Distance = -1.0f;
    soft_shadow_test::ClampSoftShadow(s);
    EXPECT_FLOAT_EQ(s.Distance, 0.0f);
    s.Distance = 99.0f;
    soft_shadow_test::ClampSoftShadow(s);
    EXPECT_FLOAT_EQ(s.Distance, 16.0f);
}

TEST(SoftShadow, ClampOpacityRange)
{
    soft_shadow_test::SoftShadowSettings s;
    s.Opacity = 2.0f;
    soft_shadow_test::ClampSoftShadow(s);
    EXPECT_FLOAT_EQ(s.Opacity, 1.0f);
    s.Opacity = -0.5f;
    soft_shadow_test::ClampSoftShadow(s);
    EXPECT_FLOAT_EQ(s.Opacity, 0.0f);
}

TEST(SoftShadow, ClampSamplesRange)
{
    soft_shadow_test::SoftShadowSettings s;
    s.Samples = 1;
    soft_shadow_test::ClampSoftShadow(s);
    EXPECT_EQ(s.Samples, 4);
    s.Samples = 99;
    soft_shadow_test::ClampSoftShadow(s);
    EXPECT_EQ(s.Samples, 24);
}

TEST(SoftShadow, ClampSoftnessAndAngle)
{
    soft_shadow_test::SoftShadowSettings s;
    s.Softness = 50.0f;
    s.Angle = 400.0f;
    soft_shadow_test::ClampSoftShadow(s);
    EXPECT_FLOAT_EQ(s.Softness, 12.0f);
    EXPECT_FLOAT_EQ(s.Angle, 360.0f);
}

TEST(SoftShadow, InRangeValuesPreserved)
{
    soft_shadow_test::SoftShadowSettings s;
    s.Distance = 6.0f;
    s.Softness = 4.0f;
    s.Opacity = 0.55f;
    s.Angle = 315.0f;
    s.Samples = 16;
    soft_shadow_test::ClampSoftShadow(s);
    EXPECT_FLOAT_EQ(s.Distance, 6.0f);
    EXPECT_FLOAT_EQ(s.Softness, 4.0f);
    EXPECT_FLOAT_EQ(s.Opacity, 0.55f);
    EXPECT_FLOAT_EQ(s.Angle, 315.0f);
    EXPECT_EQ(s.Samples, 16);
}

// ============================================================================
// Particle aura settings
// ============================================================================
//
// Mirrors Settings::ParticleSettings defaults + the clamping rules registered
// in Settings.cpp's kSettings binding table (the production source of truth).
// Re-implemented here per the CLAUDE.md rule that tests mirror, not link, the
// plugin code. Covers the premium-pass keys (depth/warmth/glow/shine) plus the
// reconciled ParticleSize default.

namespace particle_test
{

struct ParticleSettings
{
    bool Enabled = true;
    bool UseParticleTextures = true;
    int Count = 8;
    float Size = 4.2f;  // visibility pass: sprites render at/above native 16px
    float Speed = 1.0f;
    float Spread = 20.0f;
    float Alpha = 0.8f;
    int BlendMode = 1;  // Screen: readable on bright scenes (visibility pass)
    float DepthStrength = 0.7f;
    float ColorWarmth = 0.5f;
    float GlowStrength = 0.28f;  // subdued backlight; crisp sprite owns the read
    float GlowSize = 2.2f;
    float ShineThreshold = 0.84f;
};

// Apply the same per-field validation rules as kSettings in Settings.cpp.
static void ClampParticle(ParticleSettings& p)
{
    if (p.Count < 0)
    {
        p.Count = 0;
    }
    if (p.Size < 0.0f)
    {
        p.Size = 0.0f;
    }
    if (p.Speed < 0.0f)
    {
        p.Speed = 0.0f;
    }
    if (p.Spread < 0.0f)
    {
        p.Spread = 0.0f;
    }
    p.Alpha = std::clamp(p.Alpha, 0.0f, 1.0f);
    p.BlendMode = std::clamp(p.BlendMode, 0, 2);
    p.DepthStrength = std::clamp(p.DepthStrength, 0.0f, 1.5f);
    p.ColorWarmth = std::clamp(p.ColorWarmth, 0.0f, 1.0f);
    p.GlowStrength = std::clamp(p.GlowStrength, 0.0f, 1.0f);
    p.GlowSize = std::clamp(p.GlowSize, 1.0f, 4.0f);
    p.ShineThreshold = std::clamp(p.ShineThreshold, 0.0f, 0.99f);
}

}  // namespace particle_test

TEST(ParticleSettings, DefaultsMatchProduction)
{
    particle_test::ParticleSettings p;
    EXPECT_TRUE(p.Enabled);
    EXPECT_TRUE(p.UseParticleTextures);
    EXPECT_EQ(p.Count, 8);
    EXPECT_FLOAT_EQ(p.Size, 4.2f);
    EXPECT_FLOAT_EQ(p.Speed, 1.0f);
    EXPECT_FLOAT_EQ(p.Spread, 20.0f);
    EXPECT_FLOAT_EQ(p.Alpha, 0.8f);
    EXPECT_EQ(p.BlendMode, 1);
    EXPECT_FLOAT_EQ(p.DepthStrength, 0.7f);
    EXPECT_FLOAT_EQ(p.ColorWarmth, 0.5f);
    EXPECT_FLOAT_EQ(p.GlowStrength, 0.28f);
    EXPECT_FLOAT_EQ(p.GlowSize, 2.2f);
    EXPECT_FLOAT_EQ(p.ShineThreshold, 0.84f);
}

TEST(ParticleSettings, ClampDepthStrengthRange)
{
    particle_test::ParticleSettings p;
    p.DepthStrength = -0.5f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.DepthStrength, 0.0f);
    p.DepthStrength = 9.0f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.DepthStrength, 1.5f);
}

TEST(ParticleSettings, ClampColorWarmthRange)
{
    particle_test::ParticleSettings p;
    p.ColorWarmth = 2.0f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.ColorWarmth, 1.0f);
    p.ColorWarmth = -1.0f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.ColorWarmth, 0.0f);
}

TEST(ParticleSettings, ClampGlowStrengthRange)
{
    particle_test::ParticleSettings p;
    p.GlowStrength = 5.0f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.GlowStrength, 1.0f);
    p.GlowStrength = -0.1f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.GlowStrength, 0.0f);
}

TEST(ParticleSettings, ClampGlowSizeRange)
{
    particle_test::ParticleSettings p;
    p.GlowSize = 0.5f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.GlowSize, 1.0f);
    p.GlowSize = 9.0f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.GlowSize, 4.0f);
}

TEST(ParticleSettings, ClampShineThresholdRange)
{
    particle_test::ParticleSettings p;
    p.ShineThreshold = 1.5f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.ShineThreshold, 0.99f);
    p.ShineThreshold = -0.2f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.ShineThreshold, 0.0f);
}

TEST(ParticleSettings, InRangeValuesPreserved)
{
    particle_test::ParticleSettings p;
    p.DepthStrength = 1.0f;
    p.ColorWarmth = 0.6f;
    p.GlowStrength = 0.5f;
    p.GlowSize = 2.6f;
    p.ShineThreshold = 0.9f;
    particle_test::ClampParticle(p);
    EXPECT_FLOAT_EQ(p.DepthStrength, 1.0f);
    EXPECT_FLOAT_EQ(p.ColorWarmth, 0.6f);
    EXPECT_FLOAT_EQ(p.GlowStrength, 0.5f);
    EXPECT_FLOAT_EQ(p.GlowSize, 2.6f);
    EXPECT_FLOAT_EQ(p.ShineThreshold, 0.9f);
}

TEST(ParticleSettings, SizeDefaultReconciled)
{
    // S2 reconciled INI/default/comment to 3.5; the visibility pass then
    // raised the shared default to 4.2 so 16px sprites render at native size.
    particle_test::ParticleSettings p;
    EXPECT_FLOAT_EQ(p.Size, 4.2f);
}

// ============================================================================
// NPC nameplate text colors
// ============================================================================
//
// Mirrors Settings::NpcColorSettings defaults (the kSettings binding table
// rows are the source of truth in production) and ClampAndValidate()'s
// deriveColor step: start from white, ParseColor3, clamp to [0, 1].

namespace npc_colors_test
{

struct NpcColorSettings
{
    std::string NeutralColorStr = "1.0, 1.0, 1.0";
    std::string HostileColorStr = "1.0, 0.86, 0.84";
    std::string FollowerColorStr = "0.86, 0.91, 1.0";
    std::string LevelColorStr = "0.80, 0.82, 0.86";
    std::string TitleColorStr = "0.92, 0.93, 0.95";
};

// Mirror of ClampAndValidate's deriveColor lambda.
static void DeriveColor(const std::string& str, float out[3])
{
    out[0] = out[1] = out[2] = 1.0f;
    ParseColor3(str, out);
    for (int i = 0; i < 3; ++i)
    {
        out[i] = std::clamp(out[i], 0.0f, 1.0f);
    }
}

}  // namespace npc_colors_test

TEST(NpcColors, DefaultsMatchProduction)
{
    npc_colors_test::NpcColorSettings n;
    EXPECT_EQ(n.NeutralColorStr, "1.0, 1.0, 1.0");
    EXPECT_EQ(n.HostileColorStr, "1.0, 0.86, 0.84");
    EXPECT_EQ(n.FollowerColorStr, "0.86, 0.91, 1.0");
    EXPECT_EQ(n.LevelColorStr, "0.80, 0.82, 0.86");
    EXPECT_EQ(n.TitleColorStr, "0.92, 0.93, 0.95");
}

TEST(NpcColors, DefaultStringsParse)
{
    npc_colors_test::NpcColorSettings n;
    float c[3];

    npc_colors_test::DeriveColor(n.NeutralColorStr, c);
    EXPECT_FLOAT_EQ(c[0], 1.0f);
    EXPECT_FLOAT_EQ(c[1], 1.0f);
    EXPECT_FLOAT_EQ(c[2], 1.0f);

    npc_colors_test::DeriveColor(n.HostileColorStr, c);
    EXPECT_FLOAT_EQ(c[0], 1.0f);
    EXPECT_FLOAT_EQ(c[1], 0.86f);
    EXPECT_FLOAT_EQ(c[2], 0.84f);

    npc_colors_test::DeriveColor(n.FollowerColorStr, c);
    EXPECT_FLOAT_EQ(c[0], 0.86f);
    EXPECT_FLOAT_EQ(c[1], 0.91f);
    EXPECT_FLOAT_EQ(c[2], 1.0f);

    npc_colors_test::DeriveColor(n.LevelColorStr, c);
    EXPECT_FLOAT_EQ(c[0], 0.80f);
    EXPECT_FLOAT_EQ(c[1], 0.82f);
    EXPECT_FLOAT_EQ(c[2], 0.86f);

    npc_colors_test::DeriveColor(n.TitleColorStr, c);
    EXPECT_FLOAT_EQ(c[0], 0.92f);
    EXPECT_FLOAT_EQ(c[1], 0.93f);
    EXPECT_FLOAT_EQ(c[2], 0.95f);
}

TEST(NpcColors, OutOfRangeValuesClampTo01)
{
    float c[3];
    npc_colors_test::DeriveColor("1.5, -0.25, 0.5", c);
    EXPECT_FLOAT_EQ(c[0], 1.0f);
    EXPECT_FLOAT_EQ(c[1], 0.0f);
    EXPECT_FLOAT_EQ(c[2], 0.5f);
}

TEST(NpcColors, MalformedStringFallsBackToWhite)
{
    float c[3];
    npc_colors_test::DeriveColor("garbage", c);
    EXPECT_FLOAT_EQ(c[0], 1.0f);
    EXPECT_FLOAT_EQ(c[1], 1.0f);
    EXPECT_FLOAT_EQ(c[2], 1.0f);
}

// ============================================================================
// Registers -- When-token predicate parsing
// (mirrors ParseWhenTokens in Settings.cpp; keep the logic in sync)
// ============================================================================

namespace Context
{
inline constexpr uint32_t Interior = 1u << 0;
inline constexpr uint32_t Night = 1u << 1;
inline constexpr uint32_t City = 1u << 2;
inline constexpr uint32_t Sneaking = 1u << 3;
inline constexpr uint32_t Dialogue = 1u << 4;
inline constexpr uint32_t Crowded = 1u << 5;
}  // namespace Context

static std::string ToLowerAsciiCopy(const std::string& input)
{
    std::string out = input;
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static void ParseWhenTokens(const std::string& val, uint32_t& whenMask, uint32_t& whenNotMask)
{
    whenMask = 0;
    whenNotMask = 0;
    std::istringstream ss(val);
    std::string token;
    while (std::getline(ss, token, ','))
    {
        token = ToLowerAsciiCopy(Trim(token));
        bool negate = false;
        if (!token.empty() && token[0] == '!')
        {
            negate = true;
            token = Trim(token.substr(1));
        }
        uint32_t bit = 0;
        if (token == "interior")
            bit = Context::Interior;
        else if (token == "exterior")
        {
            bit = Context::Interior;
            negate = !negate;
        }
        else if (token == "night")
            bit = Context::Night;
        else if (token == "day")
        {
            bit = Context::Night;
            negate = !negate;
        }
        else if (token == "city")
            bit = Context::City;
        else if (token == "sneaking")
            bit = Context::Sneaking;
        else if (token == "dialogue")
            bit = Context::Dialogue;
        else if (token == "crowded")
            bit = Context::Crowded;
        if (bit == 0)
            continue;
        (negate ? whenNotMask : whenMask) |= bit;
    }
}

TEST(RegisterWhen, SimpleTokensSetRequiredBits)
{
    uint32_t when = 0, whenNot = 0;
    ParseWhenTokens("city, crowded", when, whenNot);
    EXPECT_EQ(when, Context::City | Context::Crowded);
    EXPECT_EQ(whenNot, 0u);
}

TEST(RegisterWhen, ExteriorAndDayAreNegatedSugar)
{
    uint32_t when = 0, whenNot = 0;
    ParseWhenTokens("exterior, night", when, whenNot);
    EXPECT_EQ(when, Context::Night);
    EXPECT_EQ(whenNot, Context::Interior);

    ParseWhenTokens("day", when, whenNot);
    EXPECT_EQ(when, 0u);
    EXPECT_EQ(whenNot, Context::Night);
}

TEST(RegisterWhen, BangNegates)
{
    uint32_t when = 0, whenNot = 0;
    ParseWhenTokens("!dialogue, sneaking", when, whenNot);
    EXPECT_EQ(when, Context::Sneaking);
    EXPECT_EQ(whenNot, Context::Dialogue);
}

TEST(RegisterWhen, DoubleNegationCancels)
{
    uint32_t when = 0, whenNot = 0;
    ParseWhenTokens("!exterior", when, whenNot);  // !(!interior) == interior
    EXPECT_EQ(when, Context::Interior);
    EXPECT_EQ(whenNot, 0u);
}

TEST(RegisterWhen, UnknownTokensAndEmptyAreIgnored)
{
    uint32_t when = 0, whenNot = 0;
    ParseWhenTokens("bogus,, city ,", when, whenNot);
    EXPECT_EQ(when, Context::City);
    EXPECT_EQ(whenNot, 0u);
}

// ============================================================================
// Registers -- profile matching / priority pick
// (mirrors PickRegister in RendererSnapshot.cpp; keep the logic in sync)
// ============================================================================

struct TestRegister
{
    uint32_t whenMask = 0;
    uint32_t whenNotMask = 0;
    int priority = 0;
    bool configured = true;  // index-gap backfill entries stay false -> inert
};

static int PickRegister(uint32_t ctxMask, const std::vector<TestRegister>& regs)
{
    int best = -1;
    int bestPriority = INT_MIN;
    for (size_t i = 0; i < regs.size(); ++i)
    {
        const auto& r = regs[i];
        if (!r.configured || (ctxMask & r.whenMask) != r.whenMask || (ctxMask & r.whenNotMask) != 0)
            continue;
        if (best < 0 || r.priority > bestPriority)
        {
            best = static_cast<int>(i);
            bestPriority = r.priority;
        }
    }
    return best;
}

TEST(RegisterPick, HighestPriorityMatchWins)
{
    std::vector<TestRegister> regs = {
        {Context::Night, 0, 1},
        {Context::Night | Context::City, 0, 5},
    };
    EXPECT_EQ(PickRegister(Context::Night | Context::City, regs), 1);
    EXPECT_EQ(PickRegister(Context::Night, regs), 0);
}

TEST(RegisterPick, ForbiddenBitsExclude)
{
    std::vector<TestRegister> regs = {
        {Context::Night, Context::Interior, 1},
    };
    EXPECT_EQ(PickRegister(Context::Night | Context::Interior, regs), -1);
    EXPECT_EQ(PickRegister(Context::Night, regs), 0);
}

TEST(RegisterPick, EmptyWhenIsBaseRegister)
{
    std::vector<TestRegister> regs = {{0, 0, 0}};
    EXPECT_EQ(PickRegister(0, regs), 0);
    EXPECT_EQ(PickRegister(Context::City | Context::Crowded, regs), 0);
}

TEST(RegisterPick, NoMatchReturnsNone)
{
    std::vector<TestRegister> regs = {{Context::Sneaking, 0, 0}};
    EXPECT_EQ(PickRegister(Context::Night, regs), -1);
}

// ============================================================================
// Deeds, Not Words -- faction spec parsing + priority pick
// (mirrors ResolveFactionSpec splitting / ResolveHonorific priority logic
//  in RendererSnapshot.cpp; keep in sync)
// ============================================================================

struct FactionSpecParts
{
    uint32_t formID = 0;
    std::string plugin = "Skyrim.esm";
};

static FactionSpecParts SplitFactionSpec(const std::string& spec)
{
    FactionSpecParts parts;
    std::string idPart = spec;
    if (const size_t at = spec.find('@'); at != std::string::npos)
    {
        idPart = spec.substr(0, at);
        parts.plugin = spec.substr(at + 1);
    }
    parts.formID = static_cast<uint32_t>(std::strtoul(idPart.c_str(), nullptr, 16));
    return parts;
}

TEST(HonorificSpec, BareHexDefaultsToSkyrimEsm)
{
    const auto parts = SplitFactionSpec("0x0001BDB3");
    EXPECT_EQ(parts.formID, 0x1BDB3u);
    EXPECT_EQ(parts.plugin, "Skyrim.esm");
}

TEST(HonorificSpec, PluginSuffixIsRespected)
{
    const auto parts = SplitFactionSpec("0x000800@MyMod.esp");
    EXPECT_EQ(parts.formID, 0x800u);
    EXPECT_EQ(parts.plugin, "MyMod.esp");
}

TEST(HonorificSpec, HexWithoutPrefixParses)
{
    const auto parts = SplitFactionSpec("48181");
    EXPECT_EQ(parts.formID, 0x48181u);
}

TEST(HonorificSpec, GarbageResolvesToZero)
{
    EXPECT_EQ(SplitFactionSpec("not-a-number").formID, 0u);
    EXPECT_EQ(SplitFactionSpec("").formID, 0u);
}

struct TestHonorific
{
    int minRank = 0;
    int priority = 0;
    bool playerOnly = false;
    bool npcOnly = false;
    bool inFaction = false;  // stand-in for the faction membership test
    int rank = 0;
};

static int PickHonorific(const std::vector<TestHonorific>& defs, bool isPlayer)
{
    int best = -1;
    int bestPriority = INT_MIN;
    for (size_t i = 0; i < defs.size(); ++i)
    {
        const auto& d = defs[i];
        if (!d.inFaction || d.rank < d.minRank || (d.playerOnly && !isPlayer) ||
            (d.npcOnly && isPlayer))
            continue;
        if (best < 0 || d.priority > bestPriority)
        {
            best = static_cast<int>(i);
            bestPriority = d.priority;
        }
    }
    return best;
}

TEST(HonorificPick, HighestPriorityMembershipWins)
{
    std::vector<TestHonorific> defs = {
        {0, 10, false, false, true, 0},
        {0, 20, false, false, true, 0},
        {0, 30, false, false, false, 0},  // not a member
    };
    EXPECT_EQ(PickHonorific(defs, false), 1);
}

TEST(HonorificPick, MinRankGates)
{
    std::vector<TestHonorific> defs = {{4, 10, false, false, true, 3}};
    EXPECT_EQ(PickHonorific(defs, false), -1);
    defs[0].rank = 4;
    EXPECT_EQ(PickHonorific(defs, false), 0);
}

TEST(HonorificPick, PlayerAndNpcOnlyFlags)
{
    std::vector<TestHonorific> defs = {
        {0, 10, true, false, true, 0},  // player only
        {0, 5, false, true, true, 0},   // npc only
    };
    EXPECT_EQ(PickHonorific(defs, true), 0);
    EXPECT_EQ(PickHonorific(defs, false), 1);
}

TEST(RegisterPick, UnconfiguredBackfillNeverShadows)
{
    // [Register1] written without [Register0]: index 0 is a gap-filled
    // default (empty When = matches everything) that must stay inert, or it
    // would win priority ties against the user's real register.
    std::vector<TestRegister> regs = {
        {0, 0, 0, false},              // phantom backfill
        {Context::Night, 0, 0, true},  // the user's register, priority 0
    };
    EXPECT_EQ(PickRegister(Context::Night, regs), 1);
    EXPECT_EQ(PickRegister(0, regs), -1);  // phantom must not act as a base
}

static float ClampRangeIcon(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

TEST(IconOpacity, ParsesAndClampsToRange)
{
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("0.92", 0.92f), 0.5f, 2.0f), 0.92f, 1e-6f);
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("5.0", 0.92f), 0.5f, 2.0f), 2.0f, 1e-6f);   // over max
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("0.1", 0.92f), 0.5f, 2.0f), 0.5f, 1e-6f);   // under min
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("bad", 0.92f), 0.5f, 2.0f), 0.92f, 1e-6f);  // default
}

TEST(EmblemCrispAlpha, DefaultsToSlightlyHigherOpacityAndClampsToRange)
{
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("bad", 0.95f), 0.5f, 1.0f), 0.95f, 1e-6f);
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("0.4", 0.95f), 0.5f, 1.0f), 0.5f, 1e-6f);
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("1.2", 0.95f), 0.5f, 1.0f), 1.0f, 1e-6f);
}

TEST(IconMutedAlpha, DefaultsToFullOpacityAndClampsToRange)
{
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("bad", 1.0f), 0.0f, 1.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("0.35", 1.0f), 0.0f, 1.0f), 0.35f, 1e-6f);
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("2.0", 1.0f), 0.0f, 1.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(ClampRangeIcon(ParseFloat("-0.2", 1.0f), 0.0f, 1.0f), 0.0f, 1e-6f);
}

// Mirror of RendererEffects.cpp ParticleTypeWeight (per repo test convention).
static float ParticleTypeWeightT(const std::string& particleTypes, const char* token)
{
    constexpr float kMinW = 0.1f, kMaxW = 10.0f;
    const size_t tokenLen = std::strlen(token);
    size_t start = 0;
    while (start <= particleTypes.size())
    {
        size_t comma = particleTypes.find(',', start);
        size_t end = (comma == std::string::npos) ? particleTypes.size() : comma;
        size_t colon = particleTypes.find(':', start);
        size_t typeEnd = (colon != std::string::npos && colon < end) ? colon : end;
        size_t a = start, b = typeEnd;
        while (a < b && std::isspace((unsigned char)particleTypes[a]))
            ++a;
        while (b > a && std::isspace((unsigned char)particleTypes[b - 1]))
            --b;
        const size_t len = b - a;
        bool match = (tokenLen == len);
        for (size_t i = 0; match && i < len; ++i)
            match = std::tolower((unsigned char)particleTypes[a + i]) == (unsigned char)token[i];
        if (match)
        {
            float w = 1.0f;
            if (colon != std::string::npos && colon < end)
            {
                try
                {
                    w = std::stof(particleTypes.substr(colon + 1, end - (colon + 1)));
                }
                catch (...)
                {
                    w = 1.0f;
                }
            }
            return std::clamp(w, kMinW, kMaxW);
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return 0.0f;
}

TEST(ParticleTypeWeight, BareTypeIsWeightOne)
{
    EXPECT_FLOAT_EQ(ParticleTypeWeightT("Firefly", "firefly"), 1.0f);
    EXPECT_FLOAT_EQ(ParticleTypeWeightT("Firefly, Mote", "mote"), 1.0f);
}
TEST(ParticleTypeWeight, ParsesExplicitWeight)
{
    EXPECT_FLOAT_EQ(ParticleTypeWeightT("Firefly:3, Mote:1", "firefly"), 3.0f);
    EXPECT_FLOAT_EQ(ParticleTypeWeightT("Firefly:3, Mote:1", "mote"), 1.0f);
    EXPECT_FLOAT_EQ(ParticleTypeWeightT("Mote:3, Spark:2, Cherryblossom:1", "spark"), 2.0f);
}
TEST(ParticleTypeWeight, CaseInsensitiveAndWhitespace)
{
    EXPECT_FLOAT_EQ(ParticleTypeWeightT("FIREFLY:2", "firefly"), 2.0f);
    EXPECT_FLOAT_EQ(ParticleTypeWeightT(" Firefly : 2 ", "firefly"), 2.0f);
}
TEST(ParticleTypeWeight, AbsentTypeIsZero)
{
    EXPECT_FLOAT_EQ(ParticleTypeWeightT("Wisp, Mote", "firefly"), 0.0f);
}
TEST(ParticleTypeWeight, MalformedWeightDefaultsToOne)
{
    EXPECT_FLOAT_EQ(ParticleTypeWeightT("Firefly:abc", "firefly"), 1.0f);
}
TEST(ParticleTypeWeight, WeightClamped)
{
    EXPECT_FLOAT_EQ(ParticleTypeWeightT("Firefly:99", "firefly"), 10.0f);
    EXPECT_FLOAT_EQ(ParticleTypeWeightT("Firefly:0", "firefly"), 0.1f);
}
TEST(ParticleTypeWeight, MeanNormalizationIsIdentityForEqualWeights)
{
    // norm = enabledStyles / sumWeights; equal weights -> norm == 1 (legacy).
    const float w[2] = {1.0f, 1.0f};
    const float norm = 2.0f / (w[0] + w[1]);
    EXPECT_FLOAT_EQ(w[0] * norm, 1.0f);
    EXPECT_FLOAT_EQ(w[1] * norm, 1.0f);
    // 3:2:1 over 3 types -> normalized 1.5 : 1.0 : 0.5 (total budget preserved).
    const float v[3] = {3.0f, 2.0f, 1.0f};
    const float n3 = 3.0f / (v[0] + v[1] + v[2]);
    EXPECT_FLOAT_EQ(v[0] * n3, 1.5f);
    EXPECT_FLOAT_EQ(v[1] * n3, 1.0f);
    EXPECT_FLOAT_EQ(v[2] * n3, 0.5f);
}

// Mirror of the Seat-of-Light optional-color rule added to Settings.cpp
// ClampAndValidate: empty/whitespace string -> no value (derive at draw time);
// non-empty -> parsed + clamped. Keep in sync with deriveOptionalColor there.
static std::string SoL_Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}
static std::optional<std::array<float, 3>> SoL_DeriveOptionalColor(const std::string& str)
{
    if (SoL_Trim(str).empty())
        return std::nullopt;
    std::array<float, 3> rgb{1.0f, 1.0f, 1.0f};
    std::istringstream ss(str);
    std::string tok;
    int i = 0;
    while (std::getline(ss, tok, ',') && i < 3)
    {
        try
        {
            rgb[i] = std::stof(SoL_Trim(tok));
        }
        catch (...)
        {
            rgb[i] = 1.0f;
        }
        ++i;
    }
    for (auto& v : rgb)
        v = std::clamp(v, 0.0f, 1.0f);
    return rgb;
}

TEST(SeatOfLightColor, EmptyStringLeavesOptionalEmpty)
{
    EXPECT_FALSE(SoL_DeriveOptionalColor("").has_value());
    EXPECT_FALSE(SoL_DeriveOptionalColor("   ").has_value());
}
TEST(SeatOfLightColor, FilledStringPopulatesAndClamps)
{
    auto c = SoL_DeriveOptionalColor("0.9, 0.8, 0.5");
    ASSERT_TRUE(c.has_value());
    EXPECT_FLOAT_EQ((*c)[0], 0.9f);
    EXPECT_FLOAT_EQ((*c)[1], 0.8f);
    EXPECT_FLOAT_EQ((*c)[2], 0.5f);
    auto over = SoL_DeriveOptionalColor("2.0, -1.0, 0.5");
    ASSERT_TRUE(over.has_value());
    EXPECT_FLOAT_EQ((*over)[0], 1.0f);
    EXPECT_FLOAT_EQ((*over)[1], 0.0f);
}

// Mirrors the HorizontalOffset binding and scale-aware application in
// RendererLayout.cpp. The neutral value must remain an exact opt-out for fonts
// whose visible ink is already centered within its advance width.
static float ResolveHorizontalOffset(float configuredPixels, float textSizeScale)
{
    configuredPixels = std::clamp(configuredPixels, -200.0f, 200.0f);
    return configuredPixels * textSizeScale;
}

TEST(HorizontalOffset, ZeroDisablesCorrection)
{
    EXPECT_FLOAT_EQ(ResolveHorizontalOffset(0.0f, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(ResolveHorizontalOffset(0.0f, 0.25f), 0.0f);
}

TEST(HorizontalOffset, PreservesDirectionAndFollowsTextScale)
{
    EXPECT_FLOAT_EQ(ResolveHorizontalOffset(-10.0f, 1.0f), -10.0f);
    EXPECT_FLOAT_EQ(ResolveHorizontalOffset(-10.0f, 0.5f), -5.0f);
    EXPECT_FLOAT_EQ(ResolveHorizontalOffset(12.0f, 0.5f), 6.0f);
}

TEST(HorizontalOffset, ClampsExtremeConfiguration)
{
    EXPECT_FLOAT_EQ(ResolveHorizontalOffset(-500.0f, 1.0f), -200.0f);
    EXPECT_FLOAT_EQ(ResolveHorizontalOffset(500.0f, 1.0f), 200.0f);
}

// Mirrors the Deck bindings in Settings.cpp. These bounds protect the
// offscreen allocation while still allowing portrait and social-media sizes.
static void ClampDeck(Settings::DeckSettings& deck)
{
    deck.CardWidth = std::clamp(deck.CardWidth, 384, 2048);
    deck.CardHeight = std::clamp(deck.CardHeight, 538, 2867);
    deck.TargetRadius = std::clamp(deck.TargetRadius, 32, 1000);
}

TEST(DeckSettings, DefaultsMatchProduction)
{
    const Settings::DeckSettings deck;
    EXPECT_TRUE(deck.Enabled);
    EXPECT_EQ(deck.Key, 119);
    EXPECT_EQ(deck.OutputFolder, "Data/SKSE/Plugins/glyph/cards");
    EXPECT_EQ(deck.CardWidth, 750);
    EXPECT_EQ(deck.CardHeight, 1050);
    EXPECT_EQ(deck.TargetRadius, 220);
    EXPECT_TRUE(deck.PlayerFallback);
    EXPECT_TRUE(deck.RarityRolls);
}

TEST(DeckSettings, ClampsOutputDimensions)
{
    Settings::DeckSettings deck;
    deck.CardWidth = 1;
    deck.CardHeight = 1;
    ClampDeck(deck);
    EXPECT_EQ(deck.CardWidth, 384);
    EXPECT_EQ(deck.CardHeight, 538);

    deck.CardWidth = 9999;
    deck.CardHeight = 9999;
    ClampDeck(deck);
    EXPECT_EQ(deck.CardWidth, 2048);
    EXPECT_EQ(deck.CardHeight, 2867);
}

TEST(DeckSettings, ClampsTargetRadius)
{
    Settings::DeckSettings deck;
    deck.TargetRadius = -10;
    ClampDeck(deck);
    EXPECT_EQ(deck.TargetRadius, 32);

    deck.TargetRadius = 5000;
    ClampDeck(deck);
    EXPECT_EQ(deck.TargetRadius, 1000);
}

TEST(DeckSettings, PreservesValidValues)
{
    Settings::DeckSettings deck;
    deck.CardWidth = 1200;
    deck.CardHeight = 1600;
    deck.TargetRadius = 360;
    ClampDeck(deck);
    EXPECT_EQ(deck.CardWidth, 1200);
    EXPECT_EQ(deck.CardHeight, 1600);
    EXPECT_EQ(deck.TargetRadius, 360);
}
