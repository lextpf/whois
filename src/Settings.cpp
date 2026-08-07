// Settings - the glyph.ini parser, the kSettings scalar binding table, and the
// accessors that publish the parsed values.
//
// Load() is the only function that parses glyph.ini. It holds a unique lock on
// Mutex() for its whole body, and runs on three different threads over the plugin's
// life: the SKSE plugin-load thread (main.cpp), the game thread (Renderer queues a
// hot reload as an SKSE task), and the render thread (fallback used when the SKSE
// task interface is null). A caller must not already hold a lock on Mutex().
//
// Load() pipeline, in order:
//
//   ResetToDefaults()   Format/InfoFormat, the four indexed vectors, then
//        |              ResetTableDefaults() for every kSettings row.
//        v
//   line loop           Trim -> StripInlineComment -> section header or key=value.
//        |              A header selects one indexed family ([TierN], [SpecialTitleN],
//        |              [HonorificN], [RegisterN]) or the global context. A key is
//        |              offered to the active indexed parser first, then to
//        |              Format/InfoFormat, then to the kSettings map keyed on the raw
//        |              INI key. Anything left over is counted and warned about.
//        v
//   ClampAndValidate()  Per-row validation rules, then cross-field constraints, then
//        |              the string-to-Color3 derivations.
//        v
//   Generation()++      Release store. The render thread re-captures its
//                       RenderSettingsSnapshot when this counter changes.
//
// A missing glyph.ini short-circuits after ResetToDefaults() and ClampAndValidate(),
// and does NOT advance Generation().

#include "Settings.hpp"

#include "PCH.hpp"
#include "RenderConstants.hpp"
#include "SettingsBinding.hpp"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Settings
{
// Single source of truth for EffectType <-> lowercase string mapping.
static constexpr Stl::EnumStringMap<EffectType, 18> kEffectTypeMap{{
    {{"none", EffectType::None},
     {"gradient", EffectType::Gradient},
     {"verticalgradient", EffectType::VerticalGradient},
     {"diagonalgradient", EffectType::DiagonalGradient},
     {"radialgradient", EffectType::RadialGradient},
     {"shimmer", EffectType::Shimmer},
     {"ember", EffectType::Ember},
     {"aurora", EffectType::Aurora},
     {"sparkle", EffectType::Sparkle},
     {"enchant", EffectType::Enchant},
     {"frost", EffectType::Frost},
     {"breathe", EffectType::Breathe},
     {"drift", EffectType::Drift},
     {"mote", EffectType::Mote},
     {"wander", EffectType::Wander},
     {"eclipse", EffectType::Eclipse},
     {"pulse", EffectType::Pulse},
     {"electric", EffectType::Electric}},
}};

// Parser helper forward declarations (used before definitions).
static std::string Trim(const std::string& str);
static std::string ToLowerAscii(std::string_view input);
static float ParseFloat(const std::string& str, float defaultVal);
static int ParseInt(const std::string& str, int defaultVal);
static bool ParseBool(const std::string& str);
static void ParseColor3(const std::string& str, Color3& out);

std::shared_mutex& Mutex()
{
    static std::shared_mutex settingsMutex;
    return settingsMutex;
}

std::atomic<uint32_t>& Generation()
{
    static std::atomic<uint32_t> gen{0};
    return gen;
}

// Every accessor below returns a reference to a function-local static. First-call
// initialization is thread-safe, but the referenced object is not: a reader must hold
// a shared lock on Mutex() and a writer a unique lock. Load() rewrites the whole set.
// ConsoleCommands.cpp is the only other writer: it sets Display().EnableDebugOverlay
// under a unique lock and then bumps Generation() itself.

std::string& TitleFormat()
{
    static std::string s;
    return s;
}

std::vector<Segment>& DisplayFormat()
{
    static std::vector<Segment> v;
    return v;
}

std::vector<Segment>& InfoFormat()
{
    static std::vector<Segment> v;
    return v;
}

std::vector<TierDefinition>& Tiers()
{
    static std::vector<TierDefinition> v;
    return v;
}

std::vector<SpecialTitleDefinition>& SpecialTitles()
{
    static std::vector<SpecialTitleDefinition> v;
    return v;
}

std::vector<HonorificDefinition>& Honorifics()
{
    static std::vector<HonorificDefinition> v;
    return v;
}

std::vector<RegisterDefinition>& Registers()
{
    static std::vector<RegisterDefinition> v;
    return v;
}

RegisterSettings& RegisterConfig()
{
    static RegisterSettings s;
    return s;
}

// Category struct accessors (function-local statics)
DistanceSettings& Distance()
{
    static DistanceSettings s;
    return s;
}

OcclusionSettings& Occlusion()
{
    static OcclusionSettings s;
    return s;
}

ShadowOutlineSettings& ShadowOutline()
{
    static ShadowOutlineSettings s;
    return s;
}

GlowSettings& Glow()
{
    static GlowSettings s;
    return s;
}

ShineSettings& Shine()
{
    static ShineSettings s;
    return s;
}

TypewriterSettings& Typewriter()
{
    static TypewriterSettings s;
    return s;
}

OrnamentSettings& Ornament()
{
    static OrnamentSettings s;
    return s;
}

ParticleSettings& Particle()
{
    static ParticleSettings s;
    return s;
}

DisplaySettings& Display()
{
    static DisplaySettings s;
    return s;
}

DeckSettings& Deck()
{
    static DeckSettings s;
    return s;
}

AnimColorSettings& AnimColor()
{
    static AnimColorSettings s;
    return s;
}

FontSettings& Font()
{
    static FontSettings s;
    return s;
}

TransitionSettings& Transition()
{
    static TransitionSettings s;
    return s;
}

VisualSettings& Visual()
{
    static VisualSettings vs;
    return vs;
}

LabelSettings& Labels()
{
    static LabelSettings s;
    return s;
}

FocusSettings& Focus()
{
    static FocusSettings s;
    return s;
}

GraffitoSettings& Graffito()
{
    static GraffitoSettings s;
    return s;
}

IconSettings& Icons()
{
    static IconSettings s;
    return s;
}

NpcColorSettings& NpcColors()
{
    static NpcColorSettings s;
    return s;
}

QuietSettings& Quiet()
{
    static QuietSettings s;
    return s;
}

DeathRiteSettings& DeathRite()
{
    static DeathRiteSettings s;
    return s;
}

CompatSettings& Compat()
{
    static CompatSettings s;
    return s;
}

CandlelightSettings& Candlelight()
{
    static CandlelightSettings s;
    return s;
}

DepthClipSettings& DepthClipConfig()
{
    static DepthClipSettings s;
    return s;
}

// Default font paths. Referenced only by the kSettings rows below; ResetToDefaults()
// applies them through ResetTableDefaults(). The names are GUID-obfuscated assets. An
// entry in glyph.project.json overrides the path at font-load time (see Hooks.cpp), so
// these are the fallback used when the manifest has no entry for that role.
static constexpr auto kDefaultNameFontPath =
    "Data/SKSE/Plugins/glyph/fonts/bd1aab18-7649-4946-9f7b-6ddd6a81311d.ttf";
static constexpr auto kDefaultLevelFontPath =
    "Data/SKSE/Plugins/glyph/fonts/96120cca-4be2-4d10-b10a-b8183ac18467.ttf";
static constexpr auto kDefaultTitleFontPath =
    "Data/SKSE/Plugins/glyph/fonts/56cb786e-c94e-452c-ac54-360c46381de1.ttf";
static constexpr auto kDefaultOrnamentFontPath =
    "Data/SKSE/Plugins/glyph/fonts/050986eb-c23a-4891-a951-9fed313e44c2.otf";

// clang-format off

// Single source of truth for all scalar settings.
// Each row: key, alias, target ptr, default value, validation rule.
// Lookup does not depend on row order: GetKeyMap() indexes the table by lowercase key
// and lowercase alias, so a duplicated key or alias resolves to the last row that
// declares it.
// The default's variant alternative must match the target pointee type; see the
// SettingEntry warning in SettingsBinding.hpp.
static const auto kSettings = std::to_array<SettingEntry>({
    // Distance & Visibility
    {"FadeStartDistance",       "", &Distance().FadeStartDistance,     200.0f,   MinFloat{.0f}},
    {"FadeEndDistance",         "", &Distance().FadeEndDistance,       2500.0f,  MinFloat{.0f}},
    {"ScaleStartDistance",      "", &Distance().ScaleStartDistance,    200.0f,   MinFloat{.0f}},
    {"ScaleEndDistance",        "", &Distance().ScaleEndDistance,      2500.0f,  MinFloat{.0f}},
    {"MinimumScale",           "", &Distance().MinimumScale,          .1f,      ClampFloat{.01f, 5.0f}},
    {"MaxScanDistance",         "", &Distance().MaxScanDistance,       3000.0f,  MinFloat{.0f}},

    // Occlusion
    {"EnableOcclusionCulling",  "", &Occlusion().Enabled,              true,    NoClamping{}},
    {"OcclusionSettleTime",     "", &Occlusion().SettleTime,           .58f,    MinFloat{.01f}},
    {"OcclusionCheckInterval",  "", &Occlusion().CheckInterval,        3,       MinInt{1}},

    // Shadow & Outline
    {"TitleShadowOffsetX",     "", &ShadowOutline().TitleShadowOffsetX,    2.0f,     NoClamping{}},
    {"TitleShadowOffsetY",     "", &ShadowOutline().TitleShadowOffsetY,    2.0f,     NoClamping{}},
    {"MainShadowOffsetX",      "", &ShadowOutline().MainShadowOffsetX,     4.0f,     NoClamping{}},
    {"MainShadowOffsetY",      "", &ShadowOutline().MainShadowOffsetY,     4.0f,     NoClamping{}},
    {"OutlineWidthMin",        "", &ShadowOutline().OutlineWidthMin,       2.0f,     MinFloat{.0f}},
    {"OutlineWidthMax",        "", &ShadowOutline().OutlineWidthMax,       2.5f,     MinFloat{.0f}},
    {"FastOutlines",           "", &ShadowOutline().FastOutlines,          false,    NoClamping{}},

    // Outline Glow
    {"EnableOutlineGlow",      "", &ShadowOutline().OutlineGlowEnabled,    false,    NoClamping{}},
    {"OutlineGlowScale",       "", &ShadowOutline().OutlineGlowScale,      1.4f,     ClampFloat{1.0f, 4.0f}},
    {"OutlineGlowAlpha",       "", &ShadowOutline().OutlineGlowAlpha,      .1f,      ClampFloat{.0f, 1.0f}},
    {"OutlineGlowRings",       "", &ShadowOutline().OutlineGlowRings,      2,        ClampInt{1, 3}},
    {"OutlineGlowR",           "", &ShadowOutline().OutlineGlowR,          1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineGlowG",           "", &ShadowOutline().OutlineGlowG,          1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineGlowB",           "", &ShadowOutline().OutlineGlowB,          1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineGlowTierTint",    "", &ShadowOutline().OutlineGlowTierTint,   false,    NoClamping{}},
    // Dual-Tone Directional Outline
    {"EnableDualOutline",      "", &ShadowOutline().DualOutlineEnabled,    false,    NoClamping{}},
    {"InnerOutlineTint",       "", &ShadowOutline().InnerOutlineTint,      .3f,      ClampFloat{.0f, 1.0f}},
    {"InnerOutlineAlpha",      "", &ShadowOutline().InnerOutlineAlpha,     .5f,      ClampFloat{.0f, 1.0f}},
    {"InnerOutlineScale",      "", &ShadowOutline().InnerOutlineScale,     .5f,      ClampFloat{.1f, .9f}},
    {"DirectionalLightAngle",  "", &ShadowOutline().DirectionalLightAngle, 315.f,    ClampFloat{.0f, 360.f}},
    {"DirectionalLightBias",   "", &ShadowOutline().DirectionalLightBias,  .15f,     ClampFloat{.0f, .5f}},

    {"OutlineColorTint",       "", &ShadowOutline().OutlineColorTint,      .0f,      ClampFloat{.0f, .25f}},
    {"ShadowColorTint",        "", &ShadowOutline().ShadowColorTint,       .0f,      ClampFloat{.0f, .25f}},

    // Soft Directional Drop-Shadow
    {"EnableSoftShadow",       "", &ShadowOutline().SoftShadowEnabled,     false,    NoClamping{}},
    {"SoftShadowDistance",     "", &ShadowOutline().SoftShadowDistance,    4.0f,     ClampFloat{.0f, 16.0f}},
    {"SoftShadowSoftness",     "", &ShadowOutline().SoftShadowSoftness,    3.0f,     ClampFloat{.0f, 12.0f}},
    {"SoftShadowOpacity",      "", &ShadowOutline().SoftShadowOpacity,     .8f,      ClampFloat{.0f, 1.0f}},
    {"SoftShadowAngle",        "", &ShadowOutline().SoftShadowAngle,       45.0f,    ClampFloat{.0f, 360.0f}},
    {"SoftShadowSamples",      "", &ShadowOutline().SoftShadowSamples,     12,       ClampInt{4, 24}},

    // Glow
    {"EnableGlow",             "", &Glow().Enabled,                   false,    NoClamping{}},
    {"GlowRadius",             "", &Glow().Radius,                    4.0f,     MinFloat{.0f}},
    {"GlowIntensity",          "", &Glow().Intensity,                 .5f,      ClampFloat{.0f, 1.0f}},
    {"GlowSamples",            "", &Glow().Samples,                   8,        ClampInt{1, 64}},
    {"GlowDivideStrength",     "", &Glow().DivideStrength,            .0f,      ClampFloat{.0f, 1.0f}},

    // Shine Overlay
    {"EnableShine",            "", &Shine().Enabled,                  false,    NoClamping{}},
    {"ShineIntensity",         "", &Shine().Intensity,                .35f,     ClampFloat{.0f, 1.0f}},
    {"ShineFalloff",           "", &Shine().Falloff,                  2.0f,     ClampFloat{.5f, 8.0f}},
    {"TextGlowAlpha",          "", &Shine().TextGlowAlpha,            .0f,      ClampFloat{.0f, 1.0f}},

    // Typewriter
    {"EnableTypewriter",       "", &Typewriter().Enabled,             false,    NoClamping{}},
    {"TypewriterSpeed",        "", &Typewriter().Speed,               30.0f,    MinFloat{.0f}},
    {"TypewriterDelay",        "", &Typewriter().Delay,               .0f,      MinFloat{.0f}},

    // Entrance/Exit Transitions
    {"EnableEntranceAnimation","", &Transition().EnableEntrance,      false,    NoClamping{}},
    {"EntranceStyle",          "", &Transition().EntranceStyle,       0,        ClampInt{0, 2}},
    {"EntranceDuration",       "", &Transition().EntranceDuration,    .35f,     ClampFloat{.05f, 3.0f}},
    {"EnableExitAnimation",    "", &Transition().EnableExit,          false,    NoClamping{}},
    {"ExitDuration",           "", &Transition().ExitDuration,        .20f,     ClampFloat{.05f, 2.0f}},
    {"EntranceStaggerStep",    "", &Transition().EntranceStaggerStep, .06f,     ClampFloat{.0f, .5f}},
    {"EntranceStaggerMax",     "", &Transition().EntranceStaggerMax,  .8f,      ClampFloat{.0f, 3.0f}},

    // Debug
    {"EnableDebugOverlay",     "", &Display().EnableDebugOverlay,     false,    NoClamping{}},

    // Ornaments
    {"EnableOrnaments",        "EnableFlourishes",   &Ornament().Enabled,      true,     NoClamping{}},
    {"OrnamentScale",          "FlourishScale",      &Ornament().Scale,        1.0f,     NoClamping{}},
    {"OrnamentSpacing",        "FlourishSpacing",    &Ornament().Spacing,      3.0f,     NoClamping{}},
    {"OrnamentAnchorToMainLine", "",                  &Ornament().AnchorToMainLine, true, NoClamping{}},
    {"OrnamentOffsetY",        "FlourishOffsetY",    &Ornament().OffsetY,      .0f,      NoClamping{}},

    // Particle Aura
    {"EnableParticleAura",     "", &Particle().Enabled,               true,     NoClamping{}},
    {"UseParticleTextures",    "", &Particle().UseParticleTextures,   true,     NoClamping{}},
    {"ParticleCount",          "", &Particle().Count,                 8,        MinInt{0}},
    {"ParticleSize",           "", &Particle().Size,                  4.2f,     MinFloat{.0f}},
    {"ParticleSpeed",          "", &Particle().Speed,                 1.0f,     MinFloat{.0f}},
    {"ParticleSpread",         "", &Particle().Spread,                20.0f,    MinFloat{.0f}},
    {"ParticleAlpha",          "", &Particle().Alpha,                 .8f,      ClampFloat{.0f, 1.0f}},
    {"ParticleBlendMode",      "", &Particle().BlendMode,             1,        ClampInt{0, 2}},
    {"ParticleDepthStrength",  "", &Particle().DepthStrength,         .7f,      ClampFloat{.0f, 1.5f}},
    {"ParticleColorWarmth",    "", &Particle().ColorWarmth,           .5f,      ClampFloat{.0f, 1.0f}},
    {"ParticleGlowStrength",   "", &Particle().GlowStrength,          .28f,     ClampFloat{.0f, 1.0f}},
    {"ParticleGlowSize",       "", &Particle().GlowSize,              2.2f,     ClampFloat{1.0f, 4.0f}},
    {"ParticleShineThreshold", "", &Particle().ShineThreshold,        .84f,     ClampFloat{.0f, .99f}},

    // Display Options
    {"VerticalOffset",         "", &Display().VerticalOffset,         8.0f,     NoClamping{}},
    {"HorizontalOffset",       "", &Display().HorizontalOffset,      -10.0f,    ClampFloat{-200.0f, 200.0f}},
    {"HidePlayer",             "", &Display().HidePlayer,             false,    NoClamping{}},
    {"HideCreatures",          "", &Display().HideCreatures,          false,    NoClamping{}},
    {"MaxPlates",              "", &Display().MaxPlates,              RenderConstants::DEFAULT_MAX_PLATES,
                                                                              ClampInt{RenderConstants::MIN_PLATES,
                                                                                       RenderConstants::MAX_PLATES}},
    {"MaxScanActors",          "", &Display().MaxScanActors,          RenderConstants::DEFAULT_MAX_SCAN_ACTORS,
                                                                              ClampInt{RenderConstants::MIN_SCAN_ACTORS,
                                                                                       RenderConstants::MAX_SCAN_ACTORS}},
    {"ReloadKey",              "", &Display().ReloadKey,              0,        NoClamping{}},

    // Character card capture
    {"DeckEnabled",            "", &Deck().Enabled,                    true,     NoClamping{}},
    {"DeckKey",                "", &Deck().Key,                        119,      NoClamping{}},
    {"DeckOutputFolder",       "", &Deck().OutputFolder,               std::string("Data/SKSE/Plugins/glyph/cards"), NoClamping{}},
    {"DeckCardWidth",          "", &Deck().CardWidth,                  750,      ClampInt{384, 2048}},
    {"DeckCardHeight",         "", &Deck().CardHeight,                 1050,     ClampInt{538, 2867}},
    {"DeckTargetRadius",       "", &Deck().TargetRadius,               220,      ClampInt{32, 1000}},
    {"DeckPlayerFallback",     "", &Deck().PlayerFallback,             true,     NoClamping{}},
    {"DeckRarityRolls",        "", &Deck().RarityRolls,                true,     NoClamping{}},

    // Smoothing
    {"AlphaSettleTime",        "", &AnimColor().AlphaSettleTime,      .46f,     MinFloat{.01f}},
    {"ScaleSettleTime",        "", &AnimColor().ScaleSettleTime,      .46f,     MinFloat{.01f}},
    {"PositionSettleTime",     "", &AnimColor().PositionSettleTime,   .38f,     MinFloat{.01f}},
    {"InnerTextAlpha",         "", &AnimColor().InnerTextAlpha,        1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineAlpha",           "", &AnimColor().OutlineAlpha,          1.0f,     ClampFloat{.0f, 1.0f}},

    // Visual sub-settings (via Visual() singleton)
    {"EnableDistanceOutlineScale", "", &Visual().EnableDistanceOutlineScale, false, NoClamping{}},
    {"OutlineDistanceMin",     "", &Visual().OutlineDistanceMin,   .8f,     NoClamping{}},
    {"OutlineDistanceMax",     "", &Visual().OutlineDistanceMax,   1.5f,    NoClamping{}},
    {"MinimumPixelHeight",     "", &Visual().MinimumPixelHeight,   .0f,     NoClamping{}},
    {"EnableLOD",              "", &Visual().EnableLOD,            false,   NoClamping{}},
    {"LODFarDistance",         "", &Visual().LODFarDistance,       1800.0f, NoClamping{}},
    {"LODMidDistance",         "", &Visual().LODMidDistance,       800.0f,  NoClamping{}},
    {"LODTransitionRange",     "", &Visual().LODTransitionRange,  200.0f,  MinFloat{1.0f}},
    {"TitleAlphaMultiplier",   "", &Visual().TitleAlphaMultiplier, .80f,   NoClamping{}},
    {"LevelAlphaMultiplier",   "", &Visual().LevelAlphaMultiplier, .85f,   NoClamping{}},
    {"EnableOverlapPrevention","", &Visual().EnableOverlapPrevention, false, NoClamping{}},
    {"OverlapPaddingY",        "", &Visual().OverlapPaddingY,     4.0f,    NoClamping{}},
    {"OverlapIterations",      "", &Visual().OverlapIterations,   3,       ClampInt{1, 16}},
    {"PositionSmoothingBlend", "", &Visual().PositionSmoothingBlend, 1.0f, ClampFloat{.0f, 1.0f}},
    {"LargeMovementThreshold", "", &Visual().LargeMovementThreshold, 50.0f, MinFloat{.0f}},
    {"LargeMovementBlend",     "", &Visual().LargeMovementBlend,  .5f,     ClampFloat{.0f, 1.0f}},
    // Motion Trail
    {"EnableMotionTrail",      "", &Visual().EnableMotionTrail,      false,   NoClamping{}},
    {"TrailLength",            "", &Visual().TrailLength,             4,       ClampInt{1, 8}},
    {"TrailAlpha",             "", &Visual().TrailAlpha,              .3f,     ClampFloat{.0f, 1.0f}},
    {"TrailFalloff",           "", &Visual().TrailFalloff,            2.0f,    ClampFloat{.5f, 5.0f}},
    {"TrailMinDistance",        "", &Visual().TrailMinDistance,        2.0f,    MinFloat{.0f}},
    {"TrailMinTier",           "", &Visual().TrailMinTier,            0,       MinInt{0}},

    // Wave Displacement
    {"EnableWave",             "", &Visual().EnableWave,             false,   NoClamping{}},
    {"WaveAmplitude",          "", &Visual().WaveAmplitude,          1.5f,    ClampFloat{.0f, 10.0f}},
    {"WaveFrequency",          "", &Visual().WaveFrequency,          3.0f,    ClampFloat{.5f, 20.0f}},
    {"WaveSpeed",              "", &Visual().WaveSpeed,              1.0f,    ClampFloat{.0f, 10.0f}},
    {"WaveMinTier",            "", &Visual().WaveMinTier,            0,       MinInt{0}},

    {"EnableTierEffectGating", "", &Visual().EnableTierEffectGating, false, NoClamping{}},
    {"GlowMinTier",            "", &Visual().GlowMinTier,         5,       NoClamping{}},
    {"ParticleMinTier",        "", &Visual().ParticleMinTier,     10,      NoClamping{}},
    {"OrnamentMinTier",        "", &Visual().OrnamentMinTier,     10,      NoClamping{}},

    // Fonts
    {"NameFontPath",           "", &Font().NameFontPath,           std::string(kDefaultNameFontPath),    NoClamping{}},
    {"NameFontSize",           "", &Font().NameFontSize,           122.0f,   NoClamping{}},
    {"LevelFontPath",          "", &Font().LevelFontPath,          std::string(kDefaultLevelFontPath),   NoClamping{}},
    {"LevelFontSize",          "", &Font().LevelFontSize,          61.0f,    NoClamping{}},
    {"TitleFontPath",          "", &Font().TitleFontPath,          std::string(kDefaultTitleFontPath),   NoClamping{}},
    {"TitleFontSize",          "", &Font().TitleFontSize,          42.0f,    NoClamping{}},
    {"OrnamentFontPath",       "", &Ornament().FontPath,           std::string(kDefaultOrnamentFontPath), NoClamping{}},
    {"OrnamentFontSize",       "", &Ornament().FontSize,           64.0f,    NoClamping{}},

    // Contextual Label Tokens - %r relationship, %d level delta, %c creature kind.
    // Empty defaults render as nothing; pair with a trailing "?" in Format/InfoFormat
    // to drop the surrounding segment when the token expands to whitespace.
    {"RelationshipFollower",   "", &Labels().RelationshipFollower,  std::string("Follower"), NoClamping{}},
    {"RelationshipAlly",       "", &Labels().RelationshipAlly,      std::string("Ally"),     NoClamping{}},
    {"RelationshipNeutral",    "", &Labels().RelationshipNeutral,   std::string(),           NoClamping{}},
    {"RelationshipHostile",    "", &Labels().RelationshipHostile,   std::string("Hostile"),  NoClamping{}},
    {"LevelDeltaWeak",         "", &Labels().LevelDeltaWeak,        std::string("Weak"),     NoClamping{}},
    {"LevelDeltaEven",         "", &Labels().LevelDeltaEven,        std::string(),           NoClamping{}},
    {"LevelDeltaStrong",       "", &Labels().LevelDeltaStrong,      std::string("Strong"),   NoClamping{}},
    {"LevelDeltaDeadly",       "", &Labels().LevelDeltaDeadly,      std::string("Deadly"),   NoClamping{}},
    {"CreatureTypeNPC",        "", &Labels().CreatureTypeNPC,       std::string(),           NoClamping{}},
    {"CreatureTypeBeast",      "", &Labels().CreatureTypeBeast,     std::string("Beast"),    NoClamping{}},
    {"CreatureTypeUndead",     "", &Labels().CreatureTypeUndead,    std::string("Undead"),   NoClamping{}},
    {"CreatureTypeDaedra",     "", &Labels().CreatureTypeDaedra,    std::string("Daedra"),   NoClamping{}},
    {"CreatureTypeDragon",     "", &Labels().CreatureTypeDragon,    std::string("Dragon"),   NoClamping{}},

    // Level-delta classification thresholds (actor level minus player level).
    {"WeakAtOrBelow",          "", &Labels().WeakAtOrBelow,         -5,                      NoClamping{}},
    {"StrongAtOrAbove",        "", &Labels().StrongAtOrAbove,        5,                      NoClamping{}},
    {"DeadlyAtOrAbove",        "", &Labels().DeadlyAtOrAbove,       10,                      NoClamping{}},

    // Focus-target expanded nameplate
    {"FocusEnabled",           "", &Focus().Enabled,                 false,                  NoClamping{}},
    {"FocusConeAngleDegrees",  "", &Focus().ConeAngleDegrees,        8.0f,                   ClampFloat{.5f, 45.0f}},
    {"FocusMaxDistance",       "", &Focus().MaxDistance,             .0f,                    MinFloat{.0f}},
    {"FocusAmbientDimFactor",  "", &Focus().AmbientDimFactor,        .55f,                   ClampFloat{.05f, 1.0f}},
    {"FocusSettleTime",        "", &Focus().SettleTime,              .25f,                   ClampFloat{.0f, 2.0f}},
    {"FocusIgnoreOccluded",    "", &Focus().IgnoreOccluded,          true,                   NoClamping{}},

    // Graffito - actor-bound, perspective-correct world-plane text.
    {"GraffitoEnabled",               "", &Graffito().Enabled,               false,   NoClamping{}},
    {"GraffitoScale",                 "", &Graffito().Scale,                 1.0f,    ClampFloat{.25f, 4.0f}},
    {"GraffitoPlayerScale",           "", &Graffito().PlayerScale,           .72f,    ClampFloat{.25f, 2.0f}},
    {"GraffitoForwardOffset",         "", &Graffito().ForwardOffset,         4.0f,    ClampFloat{.0f, 32.0f}},
    {"GraffitoFacingFadeDegrees",     "", &Graffito().FacingFadeDegrees,     15.0f,   ClampFloat{.0f, 45.0f}},
    {"GraffitoBacksideBleedAlpha",    "", &Graffito().BacksideBleedAlpha,    .12f,    ClampFloat{.0f, .25f}},
    {"GraffitoEdgeSeamAlpha",         "", &Graffito().EdgeSeamAlpha,         .22f,    ClampFloat{.0f, .4f}},
    {"GraffitoFolioEnabled",           "", &Graffito().FolioEnabled,           true,    NoClamping{}},
    {"GraffitoFolioReverseAlpha",      "", &Graffito().FolioReverseAlpha,      .72f,    ClampFloat{.0f, 1.0f}},
    {"GraffitoFolioSpineAlpha",        "", &Graffito().FolioSpineAlpha,        .88f,    ClampFloat{.0f, 1.0f}},
    {"GraffitoFolioDepth",             "", &Graffito().FolioDepth,             2.0f,    ClampFloat{.0f, 28.0f}},
    {"GraffitoWrapDegrees",            "", &Graffito().WrapDegrees,            55.0f,   ClampFloat{.0f, 140.0f}},
    {"GraffitoFisheyeStrength",        "", &Graffito().FisheyeStrength,        .62f,    ClampFloat{.0f, 1.0f}},
    {"GraffitoLayerDepth",             "", &Graffito().LayerDepth,             .0f,     ClampFloat{.0f, .06f}},
    {"GraffitoEdgeSheen",              "", &Graffito().EdgeSheen,              .14f,    ClampFloat{.0f, .4f}},
    {"GraffitoOrientationSettleTime", "", &Graffito().OrientationSettleTime, .18f,    ClampFloat{.0f, 2.0f}},
    {"GraffitoMaxDistance",           "", &Graffito().MaxDistance,           1200.0f, MinFloat{.0f}},
    {"GraffitoFallenEpitaphEnabled",  "", &Graffito().FallenEpitaphEnabled,  true,    NoClamping{}},
    {"GraffitoEpitaphGroundLift",     "", &Graffito().EpitaphGroundLift,     2.0f,    ClampFloat{.0f, 16.0f}},

    // Status icon badges - duotone SVG folder, behavior, icon names, colors.
    {"IconFolder",             "", &Icons().Folder,           std::string("Data/SKSE/Plugins/glyph/duotone"), NoClamping{}},
    {"IconsEnabled",           "", &Icons().Enabled,          true,                             NoClamping{}},
    {"IconScale",              "", &Icons().Scale,            1.0f,                             ClampFloat{.5f, 2.0f}},
    {"IconDeadlyPulse",        "", &Icons().DeadlyPulse,      true,                             NoClamping{}},
    {"IconFollower",           "", &Icons().FollowerIcon,     std::string("shield-halved"),     NoClamping{}},
    {"IconAlly",               "", &Icons().AllyIcon,         std::string("handshake"),         NoClamping{}},
    {"IconHostile",            "", &Icons().HostileIcon,      std::string("skull-crossbones"),  NoClamping{}},
    {"IconWeak",               "", &Icons().WeakIcon,         std::string("caret-down"),        NoClamping{}},
    {"IconStrong",             "", &Icons().StrongIcon,       std::string("caret-up"),          NoClamping{}},
    {"IconDeadly",             "", &Icons().DeadlyIcon,       std::string("skull"),             NoClamping{}},
    {"IconBeast",              "", &Icons().BeastIcon,        std::string("paw"),               NoClamping{}},
    {"IconUndead",             "", &Icons().UndeadIcon,       std::string("ghost"),             NoClamping{}},
    {"IconDaedra",             "", &Icons().DaedraIcon,       std::string("fire"),              NoClamping{}},
    {"IconDragon",             "", &Icons().DragonIcon,       std::string("dragon"),            NoClamping{}},
    {"IconFollowerColor",      "", &Icons().FollowerColorStr, std::string("0.46, 0.68, 0.84"),  NoClamping{}},
    {"IconAllyColor",          "", &Icons().AllyColorStr,     std::string("0.52, 0.74, 0.50"),  NoClamping{}},
    {"IconHostileColor",       "", &Icons().HostileColorStr,  std::string("0.86, 0.36, 0.32"),  NoClamping{}},
    {"IconWeakColor",          "", &Icons().WeakColorStr,     std::string("0.54, 0.66, 0.80"),  NoClamping{}},
    {"IconStrongColor",        "", &Icons().StrongColorStr,   std::string("0.86, 0.62, 0.32"),  NoClamping{}},
    {"IconDeadlyColor",        "", &Icons().DeadlyColorStr,   std::string("0.90, 0.28, 0.24"),  NoClamping{}},
    {"IconCreatureColor",      "", &Icons().CreatureColorStr, std::string("0.80, 0.74, 0.62"),  NoClamping{}},

    // Always-on badge slots - icon names, each needing a matching SVG.
    {"IconNeutral",            "", &Icons().NeutralIcon,       std::string("circle"),            NoClamping{}},
    {"IconHumanoid",           "", &Icons().HumanoidIcon,      std::string("user"),              NoClamping{}},
    {"IconEven",               "", &Icons().EvenIcon,          std::string("equals"),            NoClamping{}},
    {"IconGuard",              "", &Icons().GuardIcon,         std::string("helmet-battle"),     NoClamping{}},
    {"IconMerchant",           "", &Icons().MerchantIcon,      std::string("coins"),             NoClamping{}},
    {"IconCommoner",           "", &Icons().CommonerIcon,      std::string("house"),             NoClamping{}},
    {"IconEssential",          "", &Icons().EssentialIcon,     std::string("certificate"),       NoClamping{}},
    {"IconProtected",          "", &Icons().ProtectedIcon,     std::string("shield-check"),      NoClamping{}},
    {"IconMortal",             "", &Icons().MortalIcon,        std::string("heart"),             NoClamping{}},
    {"IconCombat",             "", &Icons().CombatIcon,        std::string("swords"),            NoClamping{}},
    {"IconAlert",              "", &Icons().AlertIcon,         std::string("eye"),               NoClamping{}},
    {"IconIdle",               "", &Icons().IdleIcon,          std::string("moon"),              NoClamping{}},
    {"IconSneakHidden",        "", &Icons().SneakHiddenIcon,   std::string("eye-slash"),         NoClamping{}},
    {"IconSneakDetected",      "", &Icons().SneakDetectedIcon, std::string("eye"),               NoClamping{}},
    {"IconSneakOff",           "", &Icons().SneakOffIcon,      std::string("person-walking"),    NoClamping{}},
    {"IconEncumbered",         "", &Icons().EncumberedIcon,    std::string("weight-hanging"),    NoClamping{}},
    {"IconNormalWeight",       "", &Icons().NormalWeightIcon,  std::string("feather"),           NoClamping{}},
    {"IconWanted",             "", &Icons().WantedIcon,        std::string("gavel"),             NoClamping{}},
    {"IconBountyClear",        "", &Icons().BountyClearIcon,   std::string("scale-balanced"),    NoClamping{}},
    {"IconTierLow",            "", &Icons().TierLowIcon,       std::string("medal"),             NoClamping{}},
    {"IconTierMid",            "", &Icons().TierMidIcon,       std::string("gem"),               NoClamping{}},
    {"IconTierHigh",           "", &Icons().TierHighIcon,      std::string("crown"),             NoClamping{}},
    {"TierBadgeImages",        "", &Icons().TierBadgeImages,   true,                             NoClamping{}},
    {"TierBadgeFolder",        "", &Icons().TierBadgeFolder,   std::string("Data/SKSE/Plugins/glyph/badges"), NoClamping{}},
    {"TierBadgeGamma",         "", &Icons().TierBadgeGamma,    1.8f,                             ClampFloat{.5f, 4.0f}},
    {"TierBadgeScale",         "", &Icons().TierBadgeScale,    1.7f,                             ClampFloat{1.0f, 4.0f}},

    {"PlayerStripBedEnabled",   "", &Icons().PlayerStripBedEnabled,   true,             NoClamping{}},
    {"PlayerStripBedAlpha",     "", &Icons().PlayerStripBedAlpha,     0.10f,            ClampFloat{0.0f, 0.2f}},
    {"PlayerStripBedSize",      "", &Icons().PlayerStripBedSize,      2.6f,             ClampFloat{1.8f, 4.0f}},
    {"PlayerStripBedBreatheHz", "", &Icons().PlayerStripBedBreatheHz, 0.14f,            ClampFloat{0.0f, 2.0f}},
    {"PlayerStripBedColor",     "", &Icons().PlayerStripBedColorStr,  std::string(""),  NoClamping{}},
    {"EmblemBacklightEnabled",  "", &Icons().EmblemBacklightEnabled,  true,             NoClamping{}},
    {"EmblemBacklightSize",     "", &Icons().EmblemBacklightSize,     2.6f,             ClampFloat{1.8f, 4.0f}},
    {"EmblemBacklightAlpha",    "", &Icons().EmblemBacklightAlpha,    0.55f,            ClampFloat{0.0f, 1.0f}},
    {"EmblemBacklightBreatheHz","", &Icons().EmblemBacklightBreatheHz,0.167f,           ClampFloat{0.0f, 2.0f}},
    {"EmblemCrispAlpha",        "", &Icons().EmblemCrispAlpha,        0.95f,            ClampFloat{0.5f, 1.0f}},
    {"EmblemBacklightColor",    "", &Icons().EmblemBacklightColorStr, std::string(""),  NoClamping{}},

    {"PlayerRimLightEnabled",   "", &Icons().PlayerRimLightEnabled,  true,             NoClamping{}},
    {"PlayerRimAlpha",          "", &Icons().PlayerRimAlpha,         0.22f,            ClampFloat{0.0f, 0.6f}},
    {"PlayerCarveAlpha",        "", &Icons().PlayerCarveAlpha,       0.26f,            ClampFloat{0.0f, 0.6f}},
    {"PlayerRimOffset",         "", &Icons().PlayerRimOffset,        1.0f,             ClampFloat{0.0f, 3.0f}},
    {"PlayerRimColor",          "", &Icons().PlayerRimColorStr,      std::string(""),  NoClamping{}},
    {"EmblemKeyFillEnabled",    "", &Icons().EmblemKeyFillEnabled,   true,             NoClamping{}},
    {"EmblemKeyAlpha",          "", &Icons().EmblemKeyAlpha,         0.35f,            ClampFloat{0.0f, 1.0f}},
    {"EmblemFillAlpha",         "", &Icons().EmblemFillAlpha,        0.15f,            ClampFloat{0.0f, 1.0f}},
    {"EmblemKeyRise",           "", &Icons().EmblemKeyRise,          0.18f,            ClampFloat{0.0f, 0.6f}},
    {"EmblemFillDrop",          "", &Icons().EmblemFillDrop,         0.15f,            ClampFloat{0.0f, 0.6f}},
    {"EmblemKeyColor",          "", &Icons().EmblemKeyColorStr,      std::string(""),  NoClamping{}},
    {"EmblemFillColor",         "", &Icons().EmblemFillColorStr,     std::string(""),  NoClamping{}},

    // Always-on slots - lit (active) colors.
    {"IconGuardColor",         "", &Icons().GuardColorStr,         std::string("0.60, 0.68, 0.84"),  NoClamping{}},
    {"IconMerchantColor",      "", &Icons().MerchantColorStr,      std::string("0.84, 0.74, 0.42"),  NoClamping{}},
    {"IconEssentialColor",     "", &Icons().EssentialColorStr,     std::string("0.86, 0.78, 0.46"),  NoClamping{}},
    {"IconProtectedColor",     "", &Icons().ProtectedColorStr,     std::string("0.54, 0.72, 0.86"),  NoClamping{}},
    {"IconCombatColor",        "", &Icons().CombatColorStr,        std::string("0.88, 0.42, 0.30"),  NoClamping{}},
    {"IconAlertColor",         "", &Icons().AlertColorStr,         std::string("0.86, 0.76, 0.40"),  NoClamping{}},
    {"IconSneakHiddenColor",   "", &Icons().SneakHiddenColorStr,   std::string("0.50, 0.64, 0.84"),  NoClamping{}},
    {"IconSneakDetectedColor", "", &Icons().SneakDetectedColorStr, std::string("0.86, 0.36, 0.32"),  NoClamping{}},
    {"IconEncumberedColor",    "", &Icons().EncumberedColorStr,    std::string("0.82, 0.64, 0.40"),  NoClamping{}},
    {"IconWantedColor",        "", &Icons().WantedColorStr,        std::string("0.84, 0.34, 0.30"),  NoClamping{}},
    {"IconTierLowColor",       "", &Icons().TierLowColorStr,       std::string("0.70, 0.62, 0.52"),  NoClamping{}},
    {"IconTierMidColor",       "", &Icons().TierMidColorStr,       std::string("0.62, 0.70, 0.80"),  NoClamping{}},
    {"IconTierHighColor",      "", &Icons().TierHighColorStr,      std::string("0.86, 0.74, 0.46"),  NoClamping{}},
    // Always-on slots - per-slot resting colors (each muted slot's own hue).
    {"IconNeutralColor",       "", &Icons().NeutralColorStr,      std::string("0.56, 0.62, 0.70"),  NoClamping{}},
    {"IconHumanoidColor",      "", &Icons().HumanoidColorStr,     std::string("0.74, 0.68, 0.58"),  NoClamping{}},
    {"IconCommonerColor",      "", &Icons().CommonerColorStr,     std::string("0.60, 0.68, 0.54"),  NoClamping{}},
    {"IconMortalColor",        "", &Icons().MortalColorStr,       std::string("0.76, 0.58, 0.60"),  NoClamping{}},
    {"IconEvenColor",          "", &Icons().EvenColorStr,         std::string("0.60, 0.70, 0.72"),  NoClamping{}},
    {"IconIdleColor",          "", &Icons().IdleColorStr,         std::string("0.56, 0.60, 0.76"),  NoClamping{}},
    {"IconSneakOffColor",      "", &Icons().SneakOffColorStr,     std::string("0.64, 0.68, 0.60"),  NoClamping{}},
    {"IconNormalWeightColor",  "", &Icons().NormalWeightColorStr, std::string("0.64, 0.76, 0.70"),  NoClamping{}},
    {"IconBountyClearColor",   "", &Icons().BountyClearColorStr,  std::string("0.50, 0.70, 0.68"),  NoClamping{}},
    {"IconMutedColor",         "", &Icons().MutedColorStr,         std::string("0.62, 0.64, 0.68"),  NoClamping{}},

    // Always-on slots - per-slot enables.
    {"IconRelationshipEnabled","", &Icons().RelationshipEnabled, true,                            NoClamping{}},
    {"IconCreatureEnabled",    "", &Icons().CreatureEnabled,     true,                            NoClamping{}},
    {"IconThreatEnabled",      "", &Icons().ThreatEnabled,       true,                            NoClamping{}},
    {"IconRoleEnabled",        "", &Icons().RoleEnabled,         true,                            NoClamping{}},
    {"IconProtectionEnabled",  "", &Icons().ProtectionEnabled,   true,                            NoClamping{}},
    {"IconEngagementEnabled",  "", &Icons().EngagementEnabled,   true,                            NoClamping{}},
    {"IconCombatStateEnabled", "", &Icons().CombatStateEnabled,  true,                            NoClamping{}},
    {"IconAlertStateEnabled",  "", &Icons().AlertStateEnabled,   true,                            NoClamping{}},
    {"IconSneakEnabled",       "", &Icons().SneakEnabled,        true,                            NoClamping{}},
    {"IconPlayerCombatEnabled","", &Icons().PlayerCombatEnabled, true,                            NoClamping{}},
    {"IconEncumberedEnabled",  "", &Icons().EncumberedEnabled,   true,                            NoClamping{}},
    {"IconBountyEnabled",      "", &Icons().BountyEnabled,       true,                            NoClamping{}},
    {"IconTierEnabled",        "", &Icons().TierEnabled,         true,                            NoClamping{}},

    // Always-on slots - muted styling.
    {"IconMutedAlpha",         "", &Icons().MutedAlpha,          1.0f,             ClampFloat{.0f, 1.0f}},
    {"IconMutedDesat",         "", &Icons().MutedDesat,          0.18f,            ClampFloat{.0f, 1.0f}},
    {"IconOpacity",            "", &Icons().Opacity,             0.92f,            ClampFloat{.5f, 2.0f}},

    // One-shot death animation.
    {"DeathRiteEnabled",       "", &DeathRite().Enabled,          true,     NoClamping{}},
    {"DeathRiteDuration",      "", &DeathRite().Duration,         1.6f,     ClampFloat{.4f, 4.0f}},

    // TrueHUD / moreHUD deconfliction.
    {"CompatYieldToTrueHUD",     "", &Compat().YieldToTrueHUD,     true,   NoClamping{}},
    {"CompatTrueHUDYieldAlpha",  "", &Compat().TrueHUDYieldAlpha,  .0f,    ClampFloat{.0f, 1.0f}},
    {"CompatYieldLevelToMoreHUD","", &Compat().YieldLevelToMoreHUD,true,   NoClamping{}},
    {"CompatYieldSettleTime",    "", &Compat().YieldSettleTime,    .3f,    ClampFloat{.01f, 2.0f}},

    // Register system globals. The profiles themselves live in [RegisterN]
    // sections.
    {"RegistersEnabled",         "", &RegisterConfig().Enabled,          true,   NoClamping{}},
    {"RegisterTransitionTime",   "", &RegisterConfig().TransitionTime,   1.2f,   ClampFloat{.05f, 5.0f}},
    {"RegisterCrowdedThreshold", "", &RegisterConfig().CrowdedThreshold, 12,     MinInt{2}},

    // Per-pixel depth occlusion.
    {"DepthClipEnabled",       "", &DepthClipConfig().Enabled,    true,     NoClamping{}},
    {"DepthClipFeather",       "", &DepthClipConfig().Feather,    2.5f,     ClampFloat{.0f, 8.0f}},

    // Exposure-adaptive text brightness.
    {"CandlelightEnabled",     "", &Candlelight().Enabled,        true,     NoClamping{}},
    {"CandlelightStrength",    "", &Candlelight().Strength,       .08f,     ClampFloat{.0f, .15f}},
    {"CandlelightWarmth",      "", &Candlelight().Warmth,         .5f,      ClampFloat{.0f, 1.0f}},
    {"CandlelightSettleTime",  "", &Candlelight().SettleTime,     .6f,      ClampFloat{.05f, 3.0f}},

    // Camera-motion quieting (asymmetric envelope).
    {"QuietFrameEnabled",      "", &Quiet().Enabled,              true,     NoClamping{}},
    {"QuietPanThresholdLo",    "", &Quiet().PanThresholdLo,       40.0f,    ClampFloat{1.0f, 720.0f}},
    {"QuietPanThresholdHi",    "", &Quiet().PanThresholdHi,       160.0f,   ClampFloat{2.0f, 1440.0f}},
    {"QuietAttackTime",        "", &Quiet().AttackTime,           .10f,     ClampFloat{.01f, 1.0f}},
    {"QuietNameReleaseTime",   "", &Quiet().NameReleaseTime,      .28f,     ClampFloat{.01f, 2.0f}},
    {"QuietSubReleaseTime",    "", &Quiet().SubReleaseTime,       .50f,     ClampFloat{.01f, 3.0f}},
    {"QuietNameFloor",         "", &Quiet().NameFloor,            .35f,     ClampFloat{.0f, 1.0f}},

    // NPC support-layer tints. The name fill stays white and the title and level
    // fills take the matched tier's level-role gradient, so these colors only tint
    // the support layer (see ResolveNpcStyleColors in RendererLayout.cpp).
    {"NpcNeutralColor",        "", &NpcColors().NeutralColorStr,  std::string("1.0, 1.0, 1.0"),    NoClamping{}},
    {"NpcHostileColor",        "", &NpcColors().HostileColorStr,  std::string("1.0, 0.86, 0.84"),  NoClamping{}},
    {"NpcFollowerColor",       "", &NpcColors().FollowerColorStr, std::string("0.86, 0.91, 1.0"),  NoClamping{}},
    {"NpcLevelColor",          "", &NpcColors().LevelColorStr,    std::string("0.80, 0.82, 0.86"), NoClamping{}},
    {"NpcTitleColor",          "", &NpcColors().TitleColorStr,    std::string("0.92, 0.93, 0.95"), NoClamping{}},
});

// clang-format on

// Lazily-built lookup map: lowercase key -> SettingEntry pointer. Built on the first
// call and never rebuilt, so it stays valid across hot reloads. An alias maps to the
// same entry as its key. Load() looks the map up with the lowercased raw INI key, so
// scalar matching is case-insensitive and CanonicalizeStructKey() does not affect it.
static const std::unordered_map<std::string, const SettingEntry*>& GetKeyMap()
{
    static const auto map = []
    {
        std::unordered_map<std::string, const SettingEntry*> m;
        m.reserve(kSettings.size() * 2);
        for (const auto& s : kSettings)
        {
            m[ToLowerAscii(s.key)] = &s;
            if (!s.alias.empty())
            {
                m[ToLowerAscii(s.alias)] = &s;
            }
        }
        return m;
    }();
    return map;
}

// Apply a parsed string value to the correct typed target.
// Text that does not parse gives 0.0f / 0 / false, not the row's default value, so a
// typo in a numeric key reads as zero rather than as the shipped default. A string
// target takes the text verbatim: it is already trimmed and comment-stripped, but any
// surrounding quotes are kept.
static void ApplySettingValue(const SettingEntry& entry, const std::string& val)
{
    std::visit(
        overloaded{
            [&](float* p) { *p = ParseFloat(val, .0f); },
            [&](bool* p) { *p = ParseBool(val); },
            [&](int* p) { *p = ParseInt(val, 0); },
            [&](std::string* p) { *p = val; },
        },
        entry.target);
}

// Reset all table-driven settings to their defaults.
static void ResetTableDefaults()
{
    for (const auto& s : kSettings)
    {
        std::visit(
            [&](auto* ptr)
            {
                using T = std::remove_pointer_t<decltype(ptr)>;
                *ptr = std::get<T>(s.defaultValue);
            },
            s.target);
    }
}

// Apply validation rules from the table.
// Only float and int targets are validated. A bool or string target, and a rule whose
// type does not match the target (ClampInt on a float, for example), is skipped with
// no diagnostic.
static void ValidateTableSettings()
{
    for (const auto& s : kSettings)
    {
        std::visit(
            overloaded{
                [&](float* p)
                {
                    std::visit(
                        overloaded{
                            [p](ClampFloat c) { *p = std::clamp(*p, c.lo, c.hi); },
                            [p](MinFloat c) { *p = std::max(c.lo, *p); },
                            [](auto) {},
                        },
                        s.validation);
                },
                [&](int* p)
                {
                    std::visit(
                        overloaded{
                            [p](ClampInt c) { *p = std::clamp(*p, c.lo, c.hi); },
                            [p](MinInt c) { *p = std::max(c.lo, *p); },
                            [](auto) {},
                        },
                        s.validation);
                },
                [](auto*) {},
            },
            s.target);
    }
}

// Baseline tier used when the INI defines none, and the shape a back-filled tier
// takes. The values match the TierDefinition member initializers (levels 1-250, title
// "Unknown", white colors, Gradient effects), so a tier created by the
// Tiers().emplace_back() growth path in Load() is equivalent to this one.
static TierDefinition MakeDefaultTier()
{
    TierDefinition tier{};
    tier.minLevel = 1;
    tier.maxLevel = 250;
    tier.title = "Unknown";
    tier.leftColor = Color3::White();
    tier.rightColor = Color3::White();
    tier.highlightColor = Color3::White();
    tier.titleEffect.type = EffectType::Gradient;
    tier.nameEffect.type = EffectType::Gradient;
    tier.levelEffect.type = EffectType::Gradient;
    tier.leftOrnaments.clear();
    tier.rightOrnaments.clear();
    tier.particleTypes.clear();
    tier.particleCount = 0;
    return tier;
}

static std::string ToLowerAscii(std::string_view input)
{
    std::string out(input);
    for (auto& c : out)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Canonicalize a key that the kSettings table does not cover: the fields of the four
// indexed sections ([TierN], [SpecialTitleN], [HonorificN], [RegisterN]) plus the
// global Format key. The map below is what makes those names case-insensitive, and it
// folds two spellings onto one name (the INI key "Title" becomes "Name").
// A key that is not in the map is returned trimmed but otherwise unchanged, so the
// tier fields that ParseTierField() handles but the map omits (TitleLeftColor,
// TitleRightColor, LevelLeftColor, LevelRightColor, ParticleColor) must be spelled
// with exactly that casing in the INI.
static std::string CanonicalizeStructKey(const std::string& rawKey)
{
    static const std::unordered_map<std::string, std::string> kStructKeys = {
        {"name", "Name"},
        {"title", "Name"},
        {"minlevel", "MinLevel"},
        {"maxlevel", "MaxLevel"},
        {"leftcolor", "LeftColor"},
        {"rightcolor", "RightColor"},
        {"highlightcolor", "HighlightColor"},
        {"titleeffect", "TitleEffect"},
        {"nameeffect", "NameEffect"},
        {"leveleffect", "LevelEffect"},
        {"ornaments", "Ornaments"},
        {"particletypes", "ParticleTypes"},
        {"particlecount", "ParticleCount"},
        {"ornamentleftcolor", "OrnamentLeftColor"},
        {"ornamentrightcolor", "OrnamentRightColor"},
        {"keyword", "Keyword"},
        {"displaytitle", "DisplayTitle"},
        {"color", "Color"},
        {"glowcolor", "GlowColor"},
        {"forceornaments", "ForceOrnaments"},
        {"forceflourishes", "ForceFlourishes"},
        {"forceparticles", "ForceParticles"},
        {"priority", "Priority"},
        {"format", "Format"},
        {"faction", "Faction"},
        {"minrank", "MinRank"},
        {"playeronly", "PlayerOnly"},
        {"npconly", "NpcOnly"},
        {"when", "When"},
        {"alphamultiplier", "AlphaMultiplier"},
        {"fadedistancemultiplier", "FadeDistanceMultiplier"},
        {"sublinealphamultiplier", "SubLineAlphaMultiplier"},
        {"hideneutral", "HideNeutral"},
    };

    const std::string lowered = ToLowerAscii(Trim(rawKey));
    if (const auto it = kStructKeys.find(lowered); it != kStructKeys.end())
    {
        return it->second;
    }
    return Trim(rawKey);
}

static void ResetToDefaults()
{
    TitleFormat() = "%t";
    DisplayFormat() = {{"%n", false, false}, {" Lv.%l", true, false}};
    // Status icon badges take the place of the text info row by default. An
    // explicit InfoFormat in the INI still wins and restores the text row.
    InfoFormat().clear();

    Tiers().clear();
    Tiers().push_back(MakeDefaultTier());
    SpecialTitles().clear();
    Honorifics().clear();
    Registers().clear();

    // All scalar settings are reset from the descriptor table.
    ResetTableDefaults();
}

// Bring the whole settings set back into a usable state. Called at the end of Load()
// and also on the missing-file path, so it must tolerate pure defaults. Three phases
// run in order: the per-row table rules, then the fixes the table cannot express
// (cross-field constraints plus the per-tier, per-special-title, per-honorific and
// per-register clamps), then the string-to-Color3 derivations. The first two phases
// are order-dependent, because the cross-field fixes read values that the per-row
// rules already clamped. The derivations read only the INI color strings, which no
// earlier phase changes.
static void ClampAndValidate()
{
    // Apply per-setting validation from the descriptor table.
    ValidateTableSettings();

    // Cross-field constraints that cannot be expressed per-setting.
    auto& dist = Distance();
    dist.FadeEndDistance = std::max(dist.FadeStartDistance + 1.0f, dist.FadeEndDistance);
    dist.ScaleEndDistance = std::max(dist.ScaleStartDistance + 1.0f, dist.ScaleEndDistance);
    ShadowOutline().OutlineWidthMax =
        std::max(ShadowOutline().OutlineWidthMin, ShadowOutline().OutlineWidthMax);

    auto& display = Display();
    const auto actorLimits =
        RenderConstants::ClampActorLimits(display.MaxPlates, display.MaxScanActors);
    display.MaxPlates = actorLimits.maxPlates;
    display.MaxScanActors = actorLimits.maxScanActors;

    // Level-delta thresholds must be strictly ordered: Weak < Strong < Deadly.
    // Out-of-order values fall back to the defaults instead of producing
    // unreachable buckets.
    auto& lb = Labels();
    if (lb.WeakAtOrBelow >= lb.StrongAtOrAbove || lb.StrongAtOrAbove >= lb.DeadlyAtOrAbove)
    {
        SKSE::log::warn(
            "Settings: LevelDelta thresholds out of order (Weak={}, Strong={}, Deadly={}); "
            "resetting to defaults",
            lb.WeakAtOrBelow,
            lb.StrongAtOrAbove,
            lb.DeadlyAtOrAbove);
        lb.WeakAtOrBelow = -5;
        lb.StrongAtOrAbove = 5;
        lb.DeadlyAtOrAbove = 10;
    }

    if (Tiers().empty())
    {
        Tiers().push_back(MakeDefaultTier());
    }

    for (auto& tier : Tiers())
    {
        if (tier.maxLevel < tier.minLevel)
        {
            std::swap(tier.maxLevel, tier.minLevel);
        }
        tier.particleCount = std::max(0, tier.particleCount);
        // Only the three required colors are clamped. The optional per-element
        // overrides (title/level/ornament pairs and particleColor) keep whatever the
        // INI supplied.
        tier.leftColor.clamp01();
        tier.rightColor.clamp01();
        tier.highlightColor.clamp01();
    }

    for (auto& special : SpecialTitles())
    {
        special.keyword = Trim(special.keyword);
        special.keywordLower = ToLowerAscii(special.keyword);
        special.color.clamp01();
        special.glowColor.clamp01();
    }

    for (auto& honorific : Honorifics())
    {
        honorific.factionSpec = Trim(honorific.factionSpec);
        honorific.title = Trim(honorific.title);
        honorific.minRank = std::max(0, honorific.minRank);
    }

    for (auto& reg : Registers())
    {
        reg.alphaMul = std::clamp(reg.alphaMul, .0f, 1.0f);
        reg.fadeMul = std::clamp(reg.fadeMul, .2f, 2.0f);
        reg.subLineMul = std::clamp(reg.subLineMul, .0f, 1.0f);
    }

    // Derive icon colors from their INI string forms. An empty or unparsable string
    // resolves to white: deriveColor seeds the output with white, ParseColor3 leaves an
    // absent component untouched, and a present component that does not parse becomes
    // 1.0.
    auto& ic = Icons();
    const auto deriveColor = [](const std::string& str, Color3& out)
    {
        out = Color3::White();
        ParseColor3(str, out);
        out.clamp01();
    };
    deriveColor(ic.FollowerColorStr, ic.FollowerColor);
    deriveColor(ic.AllyColorStr, ic.AllyColor);
    deriveColor(ic.HostileColorStr, ic.HostileColor);
    deriveColor(ic.WeakColorStr, ic.WeakColor);
    deriveColor(ic.StrongColorStr, ic.StrongColor);
    deriveColor(ic.DeadlyColorStr, ic.DeadlyColor);
    deriveColor(ic.CreatureColorStr, ic.CreatureColor);
    deriveColor(ic.GuardColorStr, ic.GuardColor);
    deriveColor(ic.MerchantColorStr, ic.MerchantColor);
    deriveColor(ic.EssentialColorStr, ic.EssentialColor);
    deriveColor(ic.ProtectedColorStr, ic.ProtectedColor);
    deriveColor(ic.CombatColorStr, ic.CombatColor);
    deriveColor(ic.AlertColorStr, ic.AlertColor);
    deriveColor(ic.SneakHiddenColorStr, ic.SneakHiddenColor);
    deriveColor(ic.SneakDetectedColorStr, ic.SneakDetectedColor);
    deriveColor(ic.EncumberedColorStr, ic.EncumberedColor);
    deriveColor(ic.WantedColorStr, ic.WantedColor);
    deriveColor(ic.TierLowColorStr, ic.TierLowColor);
    deriveColor(ic.TierMidColorStr, ic.TierMidColor);
    deriveColor(ic.TierHighColorStr, ic.TierHighColor);
    deriveColor(ic.NeutralColorStr, ic.NeutralColor);
    deriveColor(ic.HumanoidColorStr, ic.HumanoidColor);
    deriveColor(ic.CommonerColorStr, ic.CommonerColor);
    deriveColor(ic.MortalColorStr, ic.MortalColor);
    deriveColor(ic.EvenColorStr, ic.EvenColor);
    deriveColor(ic.IdleColorStr, ic.IdleColor);
    deriveColor(ic.SneakOffColorStr, ic.SneakOffColor);
    deriveColor(ic.NormalWeightColorStr, ic.NormalWeightColor);
    deriveColor(ic.BountyClearColorStr, ic.BountyClearColor);
    deriveColor(ic.MutedColorStr, ic.MutedColor);

    // Player-only accent colors: an empty INI string leaves the optional empty,
    // so the render thread derives the color from the tier Name color at draw
    // time; a non-empty string is parsed, clamped and honored. Do not route
    // these through deriveColor above, which resolves an empty string to white.
    const auto deriveOptionalColor = [](const std::string& str, std::optional<Color3>& out)
    {
        if (Trim(str).empty())
        {
            out.reset();
            return;
        }
        Color3 c = Color3::White();
        ParseColor3(str, c);
        c.clamp01();
        out = c;
    };
    deriveOptionalColor(ic.PlayerStripBedColorStr, ic.PlayerStripBedColor);
    deriveOptionalColor(ic.EmblemBacklightColorStr, ic.EmblemBacklightColor);
    deriveOptionalColor(ic.PlayerRimColorStr, ic.PlayerRimColor);
    deriveOptionalColor(ic.EmblemKeyColorStr, ic.EmblemKeyColor);
    deriveOptionalColor(ic.EmblemFillColorStr, ic.EmblemFillColor);

    // Derive NPC text colors from their INI string forms.
    auto& nc = NpcColors();
    deriveColor(nc.NeutralColorStr, nc.NeutralColor);
    deriveColor(nc.HostileColorStr, nc.HostileColor);
    deriveColor(nc.FollowerColorStr, nc.FollowerColor);
    deriveColor(nc.LevelColorStr, nc.LevelColor);
    deriveColor(nc.TitleColorStr, nc.TitleColor);
}

// Remove leading and trailing whitespace.
static std::string Trim(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first)
    {
        return {};
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// Strip an inline ; or # comment. A ; or # inside a double-quoted run is literal, so
// a Format string may contain either. Quotes and backslash escapes are only read to
// steer the scan: the returned text still holds them, which is what lets
// ParseQuotedSegments see the segment quotes. An unterminated quote suppresses comment
// stripping for the rest of the line.
static std::string StripInlineComment(const std::string& str)
{
    bool inQuote = false;
    bool escaped = false;
    for (size_t i = 0; i < str.size(); ++i)
    {
        const char c = str[i];
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (c == '\\' && inQuote)
        {
            escaped = true;
            continue;
        }
        if (c == '"')
        {
            inQuote = !inQuote;
            continue;
        }
        if (!inQuote && (c == ';' || c == '#'))
        {
            return Trim(str.substr(0, i));
        }
    }
    return Trim(str);
}

// Remove a leading UTF-8 byte order mark. Applied to the first line only, so the
// first section header or key parses when an editor saved glyph.ini with a BOM.
static std::string StripUtf8Bom(const std::string& str)
{
    if (str.size() >= 3 && static_cast<unsigned char>(str[0]) == 0xEF &&
        static_cast<unsigned char>(str[1]) == 0xBB && static_cast<unsigned char>(str[2]) == 0xBF)
    {
        return str.substr(3);
    }
    return str;
}

// Parse a float; return defaultVal when the text does not parse.
// A numeric prefix is accepted, so "2.5px" gives 2.5. Only text with no leading
// number at all, or a value outside the float range, falls back to defaultVal.
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

// Parse an int; return defaultVal when the text does not parse.
// A numeric prefix is accepted, so "10 plates" gives 10, and "1.9" gives 1.
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

// Parse a bool: true/1/yes/on/enabled, case-insensitive; anything else false.
static bool ParseBool(const std::string& str)
{
    std::string lower = str;
    for (auto& c : lower)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return (lower == "true" || lower == "1" || lower == "yes" || lower == "on" ||
            lower == "enabled");
}

// Parse a comma-separated RGB color in 0.0-1.0. Components absent from the string
// keep their existing value in out, so "0.5" changes only the red channel. A
// component that is present but does not parse, including an empty field in
// "0.5,,0.2", becomes 1.0. Values are not clamped here; each caller clamps.
static void ParseColor3(const std::string& str, Color3& out)
{
    std::istringstream ss(str);
    std::string token;
    int idx = 0;
    float rgb[3] = {out.r, out.g, out.b};
    while (std::getline(ss, token, ',') && idx < 3)
    {
        rgb[idx++] = ParseFloat(Trim(token), 1.0f);
    }
    out = Color3(rgb[0], rgb[1], rgb[2]);
}

// Map an effect type name to the enum; unknown names fall back to Gradient.
static EffectType ParseEffectType(const std::string& str)
{
    return kEffectTypeMap.fromString(ToLowerAscii(Trim(str)), EffectType::Gradient);
}

// Parse an effect string "EffectType param1,param2,... whiteBase" into an EffectParams.
// Whitespace separates the effect name from the parameter list, so a comma directly
// after the name becomes part of the name token and the lookup falls back to Gradient.
// The optional whiteBase marker is matched case-insensitively anywhere in the
// parameter text; everything from that position on is discarded. At most 5 parameters
// are read and the rest are ignored.
static void ParseEffectString(const std::string& val, EffectParams& effect)
{
    std::istringstream ss(val);
    std::string effectTypeName;
    ss >> effectTypeName;

    effect.type = ParseEffectType(effectTypeName);

    std::string paramsStr;
    std::getline(ss, paramsStr);
    paramsStr = Trim(paramsStr);

    std::string paramsLower = ToLowerAscii(paramsStr);
    size_t wbPos = paramsLower.find("whitebase");
    if (wbPos != std::string::npos)
    {
        effect.useWhiteBase = true;
        paramsStr = paramsStr.substr(0, wbPos);
    }

    std::istringstream paramStream(paramsStr);
    std::string token;
    int paramIdx = 0;
    while (std::getline(paramStream, token, ',') && paramIdx < 5)
    {
        token = Trim(token);
        if (!token.empty())
        {
            const float v = ParseFloat(token, .0f);
            switch (paramIdx)
            {
                case 0:
                    effect.param1 = v;
                    break;
                case 1:
                    effect.param2 = v;
                    break;
                case 2:
                    effect.param3 = v;
                    break;
                case 3:
                    effect.param4 = v;
                    break;
                case 4:
                    effect.param5 = v;
                    break;
            }
        }
        // Advance even on empty fields so positional params keep their slot:
        // "Aurora 0.5,,0.85" assigns 0.85 to param3 (not param2); an empty field
        // leaves that param at its existing/default value.
        paramIdx++;
    }
}

// Parse an ornaments string: "LEFT, RIGHT", or a bare two-character "AB".
// The comma form takes each side whole, so a side may hold several characters. The
// bare form splits by byte and keeps only the first two, so it is correct for
// single-byte ornament codes only. A bare value shorter than two bytes clears both
// sides.
static void ParseOrnaments(const std::string& val,
                           std::string& leftOrnaments,
                           std::string& rightOrnaments)
{
    size_t commaPos = val.find(',');
    if (commaPos != std::string::npos)
    {
        leftOrnaments = Trim(val.substr(0, commaPos));
        rightOrnaments = Trim(val.substr(commaPos + 1));
    }
    else if (val.length() >= 2)
    {
        leftOrnaments = val.substr(0, 1);
        rightOrnaments = val.substr(1, 1);
    }
    else
    {
        leftOrnaments.clear();
        rightOrnaments.clear();
    }
}

// Parse a single key-value pair for a [TierN] section. Returns false when the key is
// not a tier field, which lets Load() offer the same line to the scalar table.
// The keys compared here are the canonical spellings from CanonicalizeStructKey(); the
// five colors it does not canonicalize (TitleLeftColor, TitleRightColor,
// LevelLeftColor, LevelRightColor, ParticleColor) are therefore case-sensitive.
static bool ParseTierField(TierDefinition& tier, const std::string& key, const std::string& val)
{
    if (key == "Name")
    {
        tier.title = val;
    }
    else if (key == "MinLevel")
    {
        // Unparsable text gives 1 here and 25 for MaxLevel; both are then clamped into
        // the uint16_t range. ClampAndValidate() swaps the pair if it ends up inverted.
        const int parsed = ParseInt(val, 1);
        const int clamped =
            std::clamp(parsed, 0, static_cast<int>((std::numeric_limits<uint16_t>::max)()));
        tier.minLevel = static_cast<uint16_t>(clamped);
    }
    else if (key == "MaxLevel")
    {
        const int parsed = ParseInt(val, 25);
        const int clamped =
            std::clamp(parsed, 0, static_cast<int>((std::numeric_limits<uint16_t>::max)()));
        tier.maxLevel = static_cast<uint16_t>(clamped);
    }
    else if (key == "LeftColor")
    {
        ParseColor3(val, tier.leftColor);
    }
    else if (key == "RightColor")
    {
        ParseColor3(val, tier.rightColor);
    }
    else if (key == "HighlightColor")
    {
        ParseColor3(val, tier.highlightColor);
    }
    else if (key == "TitleLeftColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.titleLeftColor = c;
    }
    else if (key == "TitleRightColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.titleRightColor = c;
    }
    else if (key == "LevelLeftColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.levelLeftColor = c;
    }
    else if (key == "LevelRightColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.levelRightColor = c;
    }
    else if (key == "ParticleColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.particleColor = c;
    }
    else if (key == "OrnamentLeftColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.ornamentLeftColor = c;
    }
    else if (key == "OrnamentRightColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.ornamentRightColor = c;
    }
    else if (key == "TitleEffect" || key == "NameEffect" || key == "LevelEffect")
    {
        EffectParams& effect = (key == "TitleEffect")  ? tier.titleEffect
                               : (key == "NameEffect") ? tier.nameEffect
                                                       : tier.levelEffect;
        ParseEffectString(val, effect);
    }
    else if (key == "Ornaments")
    {
        ParseOrnaments(val, tier.leftOrnaments, tier.rightOrnaments);
    }
    else if (key == "ParticleTypes")
    {
        tier.particleTypes = val;
    }
    else if (key == "ParticleCount")
    {
        tier.particleCount = ParseInt(val, 0);
    }
    else
    {
        return false;
    }
    return true;
}

// Parse a single key-value pair for a [SpecialTitleN] section. ForceFlourishes is the
// legacy spelling of ForceOrnaments and sets the same field.
static bool ParseSpecialTitleField(SpecialTitleDefinition& st,
                                   const std::string& key,
                                   const std::string& val)
{
    if (key == "Keyword")
    {
        st.keyword = val;
    }
    else if (key == "DisplayTitle")
    {
        st.displayTitle = val;
    }
    else if (key == "Color")
    {
        ParseColor3(val, st.color);
    }
    else if (key == "GlowColor")
    {
        ParseColor3(val, st.glowColor);
    }
    else if (key == "ForceOrnaments" || key == "ForceFlourishes")
    {
        st.forceOrnaments = ParseBool(val);
    }
    else if (key == "ForceParticles")
    {
        st.forceParticles = ParseBool(val);
    }
    else if (key == "Priority")
    {
        st.priority = ParseInt(val, 0);
    }
    else if (key == "Ornaments")
    {
        ParseOrnaments(val, st.leftOrnaments, st.rightOrnaments);
    }
    else
    {
        return false;
    }
    return true;
}

// Parse a comma-separated `When` predicate list into required and forbidden
// context masks. Tokens: interior, exterior, night, day, city, sneaking,
// dialogue, crowded; a leading '!' negates. `exterior` and `day` are short
// forms of !interior and !night. Unknown tokens are ignored.
// Mirrored in tests/test_settings.cpp - keep the logic in sync.
static void ParseWhenTokens(const std::string& val, uint32_t& whenMask, uint32_t& whenNotMask)
{
    whenMask = 0;
    whenNotMask = 0;
    std::istringstream ss(val);
    std::string token;
    while (std::getline(ss, token, ','))
    {
        token = ToLowerAscii(Trim(token));
        bool negate = false;
        if (!token.empty() && token[0] == '!')
        {
            negate = true;
            token = Trim(token.substr(1));
        }
        uint32_t bit = 0;
        if (token == "interior")
        {
            bit = Context::Interior;
        }
        else if (token == "exterior")
        {
            bit = Context::Interior;
            negate = !negate;
        }
        else if (token == "night")
        {
            bit = Context::Night;
        }
        else if (token == "day")
        {
            bit = Context::Night;
            negate = !negate;
        }
        else if (token == "city")
        {
            bit = Context::City;
        }
        else if (token == "sneaking")
        {
            bit = Context::Sneaking;
        }
        else if (token == "dialogue")
        {
            bit = Context::Dialogue;
        }
        else if (token == "crowded")
        {
            bit = Context::Crowded;
        }
        if (bit == 0)
        {
            continue;
        }
        (negate ? whenNotMask : whenMask) |= bit;
    }
}

// Parse a single key-value pair for a [RegisterN] section. Any recognized key marks
// the register configured, which is what promotes it from the inert placeholder that
// the section growth path creates.
static bool ParseRegisterField(RegisterDefinition& r,
                               const std::string& key,
                               const std::string& val)
{
    if (key == "Name")
    {
        r.name = val;
    }
    else if (key == "When")
    {
        ParseWhenTokens(val, r.whenMask, r.whenNotMask);
    }
    else if (key == "AlphaMultiplier")
    {
        r.alphaMul = ParseFloat(val, 1.0f);
    }
    else if (key == "FadeDistanceMultiplier")
    {
        r.fadeMul = ParseFloat(val, 1.0f);
    }
    else if (key == "SubLineAlphaMultiplier")
    {
        r.subLineMul = ParseFloat(val, 1.0f);
    }
    else if (key == "HideNeutral")
    {
        r.hideNeutral = ParseBool(val);
    }
    else if (key == "Priority")
    {
        r.priority = ParseInt(val, 0);
    }
    else
    {
        return false;
    }
    r.configured = true;
    return true;
}

// Parse a single key-value pair for an [HonorificN] section. The honorific
// text uses the `Title` INI key, which CanonicalizeStructKey folds to "Name".
static bool ParseHonorificField(HonorificDefinition& h,
                                const std::string& key,
                                const std::string& val)
{
    if (key == "Faction")
    {
        h.factionSpec = val;
    }
    else if (key == "Name")
    {
        h.title = val;
    }
    else if (key == "MinRank")
    {
        h.minRank = ParseInt(val, 0);
    }
    else if (key == "Priority")
    {
        h.priority = ParseInt(val, 0);
    }
    else if (key == "PlayerOnly")
    {
        h.playerOnly = ParseBool(val);
    }
    else if (key == "NpcOnly")
    {
        h.npcOnly = ParseBool(val);
    }
    else
    {
        return false;
    }
    return true;
}

// Parse quoted segments, each with an optional trailing `?` droppable marker.
// outTitle, when non-null, absorbs segments containing `%t`; the rest go to
// out. forceLevelFont overrides the per-segment auto-detection (presence of
// `%l`) and is used by the InfoFormat row, which renders in the level font.
//
// Text outside the quotes is discarded, except a `?` in the position directly after a
// closing quote. A title segment cannot be marked droppable, because absorbing it
// clears the pointer the `?` would apply to. Several `%t` segments are allowed but the
// last one wins. Both outputs are cleared first, so a value with no quoted run leaves
// them empty.
static void ParseQuotedSegments(const std::string& val,
                                std::vector<Segment>& out,
                                std::string* outTitle,
                                bool forceLevelFont)
{
    out.clear();
    if (outTitle != nullptr)
    {
        outTitle->clear();
    }

    bool inQuote = false;
    bool justClosed = false;  // True only for the character immediately after a closing `"`.
    std::string current;
    Segment* lastPushed = nullptr;

    for (size_t i = 0; i < val.size(); ++i)
    {
        const char c = val[i];

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
                if (outTitle != nullptr && current.find("%t") != std::string::npos)
                {
                    *outTitle = current;
                    lastPushed = nullptr;
                }
                else
                {
                    const bool isLevel = forceLevelFont || current.find("%l") != std::string::npos;
                    out.push_back({current, isLevel, false});
                    lastPushed = &out.back();
                }
                current.clear();
                inQuote = false;
                justClosed = true;  // Allow trailing `?` on the very next character.
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

        // Outside quotes: `?` immediately after a closing `"` marks the previous segment optional.
        if (justClosed && c == '?')
        {
            if (lastPushed != nullptr)
            {
                lastPushed->dropIfBlank = true;
            }
        }
        justClosed = false;
    }
}

// Parse the `Format` INI key: quoted segments forming the title line and main row.
// Each half is assigned only when it parsed to something, so `Format = ` and a value
// with no quoted run both keep the ResetToDefaults() rows. ParseInfoFormat() below
// deliberately does the opposite.
static void ParseDisplayFormat(const std::string& val)
{
    std::vector<Segment> newDisplayFormat;
    std::string newTitleFormat;
    ParseQuotedSegments(val, newDisplayFormat, &newTitleFormat, /*forceLevelFont*/ false);

    if (!newTitleFormat.empty())
    {
        TitleFormat() = newTitleFormat;
    }
    if (!newDisplayFormat.empty())
    {
        DisplayFormat() = newDisplayFormat;
    }
}

// Parse the `InfoFormat` INI key: quoted segments for the third row.
// Always assigns, so an empty `InfoFormat = ` disables the info row.
static void ParseInfoFormat(const std::string& val)
{
    std::vector<Segment> newInfoFormat;
    ParseQuotedSegments(val, newInfoFormat, /*outTitle*/ nullptr, /*forceLevelFont*/ true);
    InfoFormat() = newInfoFormat;
}

void Load()
{
    std::unique_lock<std::shared_mutex> settingsWriteLock(Mutex());

    ResetToDefaults();

    std::ifstream file("Data/SKSE/Plugins/glyph.ini");
    if (!file.is_open())
    {
        ClampAndValidate();
        SKSE::log::warn("Settings: glyph.ini not found, using defaults");
        // Early return: the defaults are already in place, but Generation() is left
        // alone, so a generation-gated consumer keeps the copy it already holds.
        return;
    }

    std::string line;
    std::string currentSection;
    std::string currentSectionLower;
    int currentTier = -1;          // Tracks which tier we're parsing (-1 = global settings)
    int currentSpecialTitle = -1;  // Tracks which special title we're parsing (-1 = none)
    int currentHonorific = -1;     // Tracks which honorific we're parsing (-1 = none)
    int currentRegister = -1;      // Tracks which register we're parsing (-1 = none)
    size_t lineNumber = 0;
    size_t malformedLineCount = 0;
    size_t unknownKeyCount = 0;
    size_t unknownSectionCount = 0;
    std::vector<std::string> parseWarnings;
    std::unordered_set<std::string> warnedUnknownSections;

    // Store at most MAX_WARNINGS messages. The three counters above keep counting past
    // the cap, so the summary line stays truthful when the detail list is truncated.
    auto addWarning = [&](size_t lineNo, const std::string& message)
    {
        constexpr size_t MAX_WARNINGS = 48;
        if (parseWarnings.size() < MAX_WARNINGS)
        {
            std::ostringstream ss;
            ss << "L" << lineNo << ": " << message;
            parseWarnings.push_back(ss.str());
        }
    };

    while (std::getline(file, line))
    {
        const size_t currentLineNumber = lineNumber + 1;
        if (lineNumber++ == 0)
        {
            line = StripUtf8Bom(line);
        }
        line = Trim(line);
        line = StripInlineComment(line);

        // Skip empty lines and ; / # comment lines.
        if (line.empty() || line[0] == ';' || line[0] == '#')
        {
            continue;
        }

        // A section header changes the parsing context for the key-value
        // pairs that follow it.
        if (line.size() >= 2 && line[0] == '[' && line.back() == ']')
        {
            currentSection = line.substr(1, line.size() - 2);
            currentSection = Trim(currentSection);
            currentSectionLower = ToLowerAscii(currentSection);

            // Tier numbers are 0-indexed. An index above MAX_TIER_INDEX is rejected
            // to keep a typo from allocating an unbounded number of tiers.
            if (currentSectionLower.size() >= 4 && currentSectionLower.rfind("tier", 0) == 0)
            {
                std::string numStr = currentSection.substr(4);
                currentTier = ParseInt(numStr, -1);
                currentSpecialTitle = -1;  // Not in a special title section
                currentHonorific = -1;
                currentRegister = -1;

                if (currentTier < 0 || currentTier > RenderConstants::MAX_TIER_INDEX)
                {
                    currentTier = -1;  // Invalid tier number, treat as non-tier section
                    addWarning(currentLineNumber,
                               "Invalid or out-of-range tier section '" + currentSection + "'");
                }
                else
                {
                    // Grow the Tiers vector to reach this index. Growing past the
                    // current size back-fills intermediate indices with default
                    // 'Unknown' tiers (level range 1-250). MatchTier() scans from
                    // index 0 and stops at the first level-range match, so those
                    // back-filled tiers shadow this and every higher tier; warn
                    // when a gap is created.
                    const int oldTierCount = static_cast<int>(Tiers().size());
                    if (oldTierCount < currentTier)
                    {
                        addWarning(currentLineNumber,
                                   "Tier section '" + currentSection + "' leaves tiers " +
                                       std::to_string(oldTierCount) + "-" +
                                       std::to_string(currentTier - 1) +
                                       " undefined; they default to 'Unknown' (level range "
                                       "1-250) and will shadow this and higher tiers");
                    }
                    while (static_cast<int>(Tiers().size()) <= currentTier)
                    {
                        Tiers().emplace_back();
                    }
                }
            }
            // Special title sections like [SpecialTitle0], [SpecialTitle1], etc.
            else if (currentSectionLower.size() >= 12 &&
                     currentSectionLower.rfind("specialtitle", 0) == 0)
            {
                std::string numStr = currentSection.substr(12);
                currentSpecialTitle = ParseInt(numStr, -1);
                currentTier = -1;  // Not in a tier section
                currentHonorific = -1;
                currentRegister = -1;

                if (currentSpecialTitle >= 0 &&
                    currentSpecialTitle <= RenderConstants::MAX_SPECIAL_TITLE_INDEX)
                {
                    // Dynamically grow the SpecialTitles vector
                    while (static_cast<int>(SpecialTitles().size()) <= currentSpecialTitle)
                    {
                        SpecialTitleDefinition newSpecial;
                        newSpecial.keyword = "";
                        newSpecial.displayTitle = "";
                        newSpecial.color = Color3::White();
                        newSpecial.glowColor = Color3::White();
                        newSpecial.forceOrnaments = true;
                        newSpecial.forceParticles = true;
                        newSpecial.priority = 0;
                        SpecialTitles().push_back(newSpecial);
                    }
                }
                else
                {
                    addWarning(
                        currentLineNumber,
                        "Invalid or out-of-range special title section '" + currentSection + "'");
                    currentSpecialTitle = -1;
                }
            }
            // Honorific sections like [Honorific0], [Honorific1], etc.
            else if (currentSectionLower.size() >= 9 &&
                     currentSectionLower.rfind("honorific", 0) == 0)
            {
                std::string numStr = currentSection.substr(9);
                currentHonorific = ParseInt(numStr, -1);
                currentTier = -1;
                currentSpecialTitle = -1;
                currentRegister = -1;

                if (currentHonorific >= 0 &&
                    currentHonorific <= RenderConstants::MAX_HONORIFIC_INDEX)
                {
                    while (static_cast<int>(Honorifics().size()) <= currentHonorific)
                    {
                        Honorifics().emplace_back();
                    }
                }
                else
                {
                    addWarning(
                        currentLineNumber,
                        "Invalid or out-of-range honorific section '" + currentSection + "'");
                    currentHonorific = -1;
                }
            }
            // Register sections like [Register0], [Register1], etc.
            else if (currentSectionLower.size() >= 8 &&
                     currentSectionLower.rfind("register", 0) == 0)
            {
                std::string numStr = currentSection.substr(8);
                currentRegister = ParseInt(numStr, -1);
                currentTier = -1;
                currentSpecialTitle = -1;
                currentHonorific = -1;

                if (currentRegister >= 0 && currentRegister <= RenderConstants::MAX_REGISTER_INDEX)
                {
                    const int oldCount = static_cast<int>(Registers().size());
                    if (oldCount < currentRegister)
                    {
                        addWarning(currentLineNumber,
                                   "Register section '" + currentSection + "' leaves registers " +
                                       std::to_string(oldCount) + "-" +
                                       std::to_string(currentRegister - 1) +
                                       " undefined; they stay inert until given keys");
                    }
                    while (static_cast<int>(Registers().size()) <= currentRegister)
                    {
                        Registers().emplace_back();
                    }
                }
                else
                {
                    addWarning(currentLineNumber,
                               "Invalid or out-of-range register section '" + currentSection + "'");
                    currentRegister = -1;
                }
            }
            else
            {
                currentTier = -1;  // Non-tier section, switch to global context
                currentSpecialTitle = -1;
                currentHonorific = -1;
                currentRegister = -1;

                // Non-indexed section names that parse without a warning. The empty
                // string covers keys written before the first header. An unknown name
                // only warns: its keys are still matched against the scalar table,
                // because a scalar is looked up by key name and not by section.
                static const std::unordered_set<std::string> kKnownSections = {"",
                                                                               "general",
                                                                               "display",
                                                                               "deck",
                                                                               "debug",
                                                                               "visual",
                                                                               "fonts",
                                                                               "particles",
                                                                               "occlusion",
                                                                               "labels",
                                                                               "leveldelta",
                                                                               "icons",
                                                                               "focus",
                                                                               "graffito",
                                                                               "quiet",
                                                                               "deathrite",
                                                                               "compat",
                                                                               "candlelight",
                                                                               "depthclip"};
                if (kKnownSections.find(currentSectionLower) == kKnownSections.end())
                {
                    ++unknownSectionCount;
                    if (warnedUnknownSections.insert(currentSectionLower).second)
                    {
                        addWarning(currentLineNumber, "Unknown section [" + currentSection + "]");
                    }
                }
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            ++malformedLineCount;
            addWarning(currentLineNumber, "Ignoring malformed setting line (missing '=')");
            continue;
        }

        std::string keyRaw = Trim(line.substr(0, eq));
        std::string key = CanonicalizeStructKey(keyRaw);
        std::string val = Trim(line.substr(eq + 1));

        // Key dispatch, first match wins: the parser for the active indexed section,
        // then Format / InfoFormat, then the scalar table. At most one indexed parser
        // can be active, because a section header clears the other three indices.
        // The indexed parsers see the canonicalized key; the scalar lookup uses keyRaw,
        // so canonicalization cannot rename a scalar out of the table.
        bool handled = false;

        if (currentTier >= 0 && currentTier < static_cast<int>(Tiers().size()))
        {
            handled = ParseTierField(Tiers()[currentTier], key, val);
        }

        if (!handled && currentSpecialTitle >= 0 &&
            currentSpecialTitle < static_cast<int>(SpecialTitles().size()))
        {
            handled = ParseSpecialTitleField(SpecialTitles()[currentSpecialTitle], key, val);
        }

        if (!handled && currentHonorific >= 0 &&
            currentHonorific < static_cast<int>(Honorifics().size()))
        {
            handled = ParseHonorificField(Honorifics()[currentHonorific], key, val);
        }

        if (!handled && currentRegister >= 0 &&
            currentRegister < static_cast<int>(Registers().size()))
        {
            handled = ParseRegisterField(Registers()[currentRegister], key, val);
        }

        if (!handled)
        {
            if (key == "Format")
            {
                ParseDisplayFormat(val);
            }
            else if (keyRaw == "InfoFormat" || ToLowerAscii(keyRaw) == "infoformat")
            {
                ParseInfoFormat(val);
            }
            // Table-driven lookup for all scalar settings.
            else if (auto it = GetKeyMap().find(ToLowerAscii(keyRaw)); it != GetKeyMap().end())
            {
                ApplySettingValue(*it->second, val);
            }
            else
            {
                ++unknownKeyCount;
                std::string sectionName = currentSection.empty() ? "<global>" : currentSection;
                addWarning(currentLineNumber,
                           "Unknown key '" + keyRaw + "' in section " + sectionName);
            }
        }
    }

    ClampAndValidate();

    if (malformedLineCount > 0 || unknownKeyCount > 0 || unknownSectionCount > 0)
    {
        SKSE::log::warn(
            "Settings: parsed glyph.ini with {} malformed lines, {} unknown keys, {} unknown "
            "sections",
            malformedLineCount,
            unknownKeyCount,
            unknownSectionCount);
        for (const auto& warning : parseWarnings)
        {
            SKSE::log::warn("Settings: {}", warning);
        }
        // 48 is the MAX_WARNINGS cap inside addWarning; the list cannot exceed it.
        if (parseWarnings.size() == 48)
        {
            SKSE::log::warn("Settings: warning output truncated");
        }
    }

    // Publish last, while the write lock is still held. The release order pairs with
    // the acquire load in RefreshCachedSettingsSnapshot() on the render thread, so a
    // reader that observes the new generation also observes every value written above.
    Generation().fetch_add(1, std::memory_order_release);
}
}  // namespace Settings
