// Unit tests for the contextual nameplate label/token system.
//
// Per CLAUDE.md, tests deliberately re-implement the logic under test
// (RE::Actor / CommonLibSSE / ImGui cannot link in the test harness), so the
// fixtures below mirror the production code in:
//   - src/RendererSnapshot.cpp   -- ClassifyDelta
//   - src/RendererLayout.cpp     -- FormatString, LabelFor
//   - src/RendererInternal.hpp   -- RelationshipKind / LevelDelta / CreatureKind,
//                                   ActorLabelContext
//
// This mirror goes stale silently. When you change either side, update the
// other in the same commit.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

// ============================================================================
// Mirrored enums and label table
// ============================================================================

enum class RelationshipKind : std::uint8_t
{
    Hostile,
    Neutral,
    Ally,
    Follower
};
enum class LevelDelta : std::uint8_t
{
    Weak,
    Even,
    Strong,
    Deadly
};
enum class CreatureKind : std::uint8_t
{
    NPC,
    Beast,
    Undead,
    Daedra,
    Dragon
};

struct LabelTokens
{
    std::string relFollower = "Follower";
    std::string relAlly = "Ally";
    std::string relNeutral;
    std::string relHostile = "Hostile";
    std::string ldWeak = "Weak";
    std::string ldEven;
    std::string ldStrong = "Strong";
    std::string ldDeadly = "Deadly";
    std::string ctNPC;
    std::string ctBeast = "Beast";
    std::string ctUndead = "Undead";
    std::string ctDaedra = "Daedra";
    std::string ctDragon = "Dragon";
    int deltaWeakBelow = -5;
    int deltaStrongAbove = 5;
    int deltaDeadlyAbove = 10;
};

struct ActorLabelContext
{
    std::string_view name;
    int level = 0;
    const char* title = nullptr;
    std::string_view relationship;
    std::string_view levelDelta;
    std::string_view creatureKind;
    std::uint32_t formID = 0;
};

// ============================================================================
// ClassifyDelta -- mirrors RendererSnapshot.cpp
// ============================================================================

static LevelDelta ClassifyDelta(
    int actorLv, int playerLv, int weakAtOrBelow, int strongAtOrAbove, int deadlyAtOrAbove)
{
    const int delta = actorLv - playerLv;
    if (delta >= deadlyAtOrAbove)
        return LevelDelta::Deadly;
    if (delta >= strongAtOrAbove)
        return LevelDelta::Strong;
    if (delta <= weakAtOrBelow)
        return LevelDelta::Weak;
    return LevelDelta::Even;
}

// ============================================================================
// LabelFor -- mirrors RendererLayout.cpp
// ============================================================================

static std::string_view LabelFor(RelationshipKind r, const LabelTokens& lbl)
{
    switch (r)
    {
        case RelationshipKind::Hostile:
            return lbl.relHostile;
        case RelationshipKind::Neutral:
            return lbl.relNeutral;
        case RelationshipKind::Ally:
            return lbl.relAlly;
        case RelationshipKind::Follower:
            return lbl.relFollower;
    }
    return {};
}

static std::string_view LabelFor(LevelDelta d, const LabelTokens& lbl)
{
    switch (d)
    {
        case LevelDelta::Weak:
            return lbl.ldWeak;
        case LevelDelta::Even:
            return lbl.ldEven;
        case LevelDelta::Strong:
            return lbl.ldStrong;
        case LevelDelta::Deadly:
            return lbl.ldDeadly;
    }
    return {};
}

static std::string_view LabelFor(CreatureKind k, const LabelTokens& lbl)
{
    switch (k)
    {
        case CreatureKind::NPC:
            return lbl.ctNPC;
        case CreatureKind::Beast:
            return lbl.ctBeast;
        case CreatureKind::Undead:
            return lbl.ctUndead;
        case CreatureKind::Daedra:
            return lbl.ctDaedra;
        case CreatureKind::Dragon:
            return lbl.ctDragon;
    }
    return {};
}

// ============================================================================
// FormatString -- mirrors RendererLayout.cpp
// ============================================================================

static std::string FormatString(const std::string& fmt, const ActorLabelContext& ctx)
{
    std::string result;
    result.reserve(fmt.size() + ctx.name.size() + 64);

    for (size_t i = 0; i < fmt.size(); ++i)
    {
        if (fmt[i] == '%' && i + 1 < fmt.size())
        {
            switch (fmt[i + 1])
            {
                case 'n':
                    result.append(ctx.name.data(), ctx.name.size());
                    ++i;
                    continue;
                case 'l':
                    result.append(std::to_string(ctx.level));
                    ++i;
                    continue;
                case 't':
                    if (ctx.title != nullptr)
                    {
                        result.append(ctx.title);
                        ++i;
                        continue;
                    }
                    break;
                case 'r':
                    result.append(ctx.relationship.data(), ctx.relationship.size());
                    ++i;
                    continue;
                case 'd':
                    result.append(ctx.levelDelta.data(), ctx.levelDelta.size());
                    ++i;
                    continue;
                case 'c':
                    result.append(ctx.creatureKind.data(), ctx.creatureKind.size());
                    ++i;
                    continue;
            }
        }
        result += fmt[i];
    }
    return result;
}

// ============================================================================
// Tests: ClassifyDelta
// ============================================================================

TEST(ClassifyDeltaTest, DefaultsEvenAtPlayerLevel)
{
    EXPECT_EQ(ClassifyDelta(10, 10, -5, 5, 10), LevelDelta::Even);
}

TEST(ClassifyDeltaTest, JustBelowStrongIsEven)
{
    EXPECT_EQ(ClassifyDelta(14, 10, -5, 5, 10), LevelDelta::Even);  // delta = +4
}

TEST(ClassifyDeltaTest, ExactlyStrongThresholdIsStrong)
{
    EXPECT_EQ(ClassifyDelta(15, 10, -5, 5, 10), LevelDelta::Strong);  // delta = +5
}

TEST(ClassifyDeltaTest, JustBelowDeadlyIsStrong)
{
    EXPECT_EQ(ClassifyDelta(19, 10, -5, 5, 10), LevelDelta::Strong);  // delta = +9
}

TEST(ClassifyDeltaTest, ExactlyDeadlyThresholdIsDeadly)
{
    EXPECT_EQ(ClassifyDelta(20, 10, -5, 5, 10), LevelDelta::Deadly);  // delta = +10
}

TEST(ClassifyDeltaTest, FarAboveIsDeadly)
{
    EXPECT_EQ(ClassifyDelta(100, 10, -5, 5, 10), LevelDelta::Deadly);
}

TEST(ClassifyDeltaTest, JustAboveWeakIsEven)
{
    EXPECT_EQ(ClassifyDelta(6, 10, -5, 5, 10), LevelDelta::Even);  // delta = -4
}

TEST(ClassifyDeltaTest, ExactlyWeakThresholdIsWeak)
{
    EXPECT_EQ(ClassifyDelta(5, 10, -5, 5, 10), LevelDelta::Weak);  // delta = -5
}

TEST(ClassifyDeltaTest, FarBelowIsWeak)
{
    EXPECT_EQ(ClassifyDelta(1, 100, -5, 5, 10), LevelDelta::Weak);
}

TEST(ClassifyDeltaTest, CustomThresholds)
{
    // User changes Strong to +3 and Deadly to +6.
    EXPECT_EQ(ClassifyDelta(12, 10, -5, 3, 6), LevelDelta::Even);    // delta = +2 -> Even
    EXPECT_EQ(ClassifyDelta(13, 10, -5, 3, 6), LevelDelta::Strong);  // delta = +3 -> Strong
    EXPECT_EQ(ClassifyDelta(16, 10, -5, 3, 6), LevelDelta::Deadly);  // delta = +6 -> Deadly
}

TEST(ClassifyDeltaTest, PlayerLevelOneEdge)
{
    // Brand-new player level 1.  Most enemies are "Strong" or worse.
    EXPECT_EQ(ClassifyDelta(1, 1, -5, 5, 10), LevelDelta::Even);
    EXPECT_EQ(ClassifyDelta(6, 1, -5, 5, 10), LevelDelta::Strong);
    EXPECT_EQ(ClassifyDelta(11, 1, -5, 5, 10), LevelDelta::Deadly);
}

// ============================================================================
// Tests: LabelFor (resolution)
// ============================================================================

TEST(LabelForTest, RelationshipDefaults)
{
    LabelTokens lbl;
    EXPECT_EQ(LabelFor(RelationshipKind::Hostile, lbl), "Hostile");
    EXPECT_EQ(LabelFor(RelationshipKind::Neutral, lbl), "");  // empty by default
    EXPECT_EQ(LabelFor(RelationshipKind::Ally, lbl), "Ally");
    EXPECT_EQ(LabelFor(RelationshipKind::Follower, lbl), "Follower");
}

TEST(LabelForTest, LevelDeltaDefaults)
{
    LabelTokens lbl;
    EXPECT_EQ(LabelFor(LevelDelta::Weak, lbl), "Weak");
    EXPECT_EQ(LabelFor(LevelDelta::Even, lbl), "");  // empty by default
    EXPECT_EQ(LabelFor(LevelDelta::Strong, lbl), "Strong");
    EXPECT_EQ(LabelFor(LevelDelta::Deadly, lbl), "Deadly");
}

TEST(LabelForTest, CreatureKindDefaults)
{
    LabelTokens lbl;
    EXPECT_EQ(LabelFor(CreatureKind::NPC, lbl), "");  // empty by default
    EXPECT_EQ(LabelFor(CreatureKind::Beast, lbl), "Beast");
    EXPECT_EQ(LabelFor(CreatureKind::Undead, lbl), "Undead");
    EXPECT_EQ(LabelFor(CreatureKind::Daedra, lbl), "Daedra");
    EXPECT_EQ(LabelFor(CreatureKind::Dragon, lbl), "Dragon");
}

TEST(LabelForTest, OverrideLabels)
{
    LabelTokens lbl;
    lbl.relHostile = "Enemy";
    lbl.ldDeadly = "Lethal";
    lbl.ctDragon = "Wyrm";
    EXPECT_EQ(LabelFor(RelationshipKind::Hostile, lbl), "Enemy");
    EXPECT_EQ(LabelFor(LevelDelta::Deadly, lbl), "Lethal");
    EXPECT_EQ(LabelFor(CreatureKind::Dragon, lbl), "Wyrm");
}

// ============================================================================
// Tests: FormatString -- token substitution
// ============================================================================

static ActorLabelContext MakeCtx(std::string_view name, int level)
{
    ActorLabelContext ctx;
    ctx.name = name;
    ctx.level = level;
    ctx.title = nullptr;
    ctx.relationship = {};
    ctx.levelDelta = {};
    ctx.creatureKind = {};
    ctx.formID = 0;
    return ctx;
}

TEST(FormatStringTest, NameOnly)
{
    auto ctx = MakeCtx("Lydia", 12);
    EXPECT_EQ(FormatString("%n", ctx), "Lydia");
}

TEST(FormatStringTest, LevelOnly)
{
    auto ctx = MakeCtx("Lydia", 12);
    EXPECT_EQ(FormatString("Lv.%l", ctx), "Lv.12");
}

TEST(FormatStringTest, TitleNullExpandsLiterally)
{
    auto ctx = MakeCtx("Lydia", 12);  // title = nullptr
    // %t falls through to literal output when title is null.
    EXPECT_EQ(FormatString("%t", ctx), "%t");
}

TEST(FormatStringTest, TitleNonNull)
{
    auto ctx = MakeCtx("Lydia", 12);
    ctx.title = "Squire";
    EXPECT_EQ(FormatString("\"%t\"", ctx), "\"Squire\"");
}

TEST(FormatStringTest, RelationshipToken)
{
    auto ctx = MakeCtx("Lydia", 12);
    ctx.relationship = "Follower";
    EXPECT_EQ(FormatString("%r", ctx), "Follower");
}

TEST(FormatStringTest, LevelDeltaToken)
{
    auto ctx = MakeCtx("Bandit", 15);
    ctx.levelDelta = "Strong";
    EXPECT_EQ(FormatString("%d", ctx), "Strong");
}

TEST(FormatStringTest, CreatureKindToken)
{
    auto ctx = MakeCtx("Frost Troll", 14);
    ctx.creatureKind = "Beast";
    EXPECT_EQ(FormatString("%c", ctx), "Beast");
}

TEST(FormatStringTest, EmptyTokenExpansion)
{
    auto ctx = MakeCtx("Guard", 10);
    ctx.relationship = "";  // explicitly empty
    EXPECT_EQ(FormatString("  *  %r", ctx), "  *  ");
}

TEST(FormatStringTest, AllSixTokensInOneString)
{
    auto ctx = MakeCtx("Test", 20);
    ctx.title = "Tester";
    ctx.relationship = "Ally";
    ctx.levelDelta = "Even";
    ctx.creatureKind = "NPC";
    EXPECT_EQ(FormatString("[%t] %n L%l (%r/%d/%c)", ctx), "[Tester] Test L20 (Ally/Even/NPC)");
}

TEST(FormatStringTest, LiteralPercentPreserved)
{
    // A `%` followed by an unrecognized char passes through unchanged.
    auto ctx = MakeCtx("X", 1);
    EXPECT_EQ(FormatString("%n is at 100%z", ctx), "X is at 100%z");
}

TEST(FormatStringTest, NoSubstitutionInResult)
{
    // If the name itself contains "%l", the literal substring must NOT be
    // re-expanded -- the FormatString loop is single-pass.
    auto ctx = MakeCtx("%l", 7);
    EXPECT_EQ(FormatString("%n", ctx), "%l");
}

TEST(FormatStringTest, RepeatedTokens)
{
    auto ctx = MakeCtx("Name", 5);
    ctx.relationship = "Foo";
    EXPECT_EQ(FormatString("%n %n %r %r", ctx), "Name Name Foo Foo");
}

TEST(FormatStringTest, EmptyFormat)
{
    auto ctx = MakeCtx("Name", 5);
    EXPECT_EQ(FormatString("", ctx), "");
}

TEST(FormatStringTest, OnlyLiterals)
{
    auto ctx = MakeCtx("Name", 5);
    EXPECT_EQ(FormatString("Hello, world!", ctx), "Hello, world!");
}

TEST(FormatStringTest, TrailingPercent)
{
    // A trailing `%` with no following character is preserved literally.
    auto ctx = MakeCtx("Name", 5);
    EXPECT_EQ(FormatString("100%", ctx), "100%");
}
