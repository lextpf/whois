#pragma once

#include <string>
#include <vector>

/**
 * @namespace Settings
 * @brief Configuration management and INI parsing.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Configuration
 *
 * Contains all user-configurable settings loaded from `whois.ini`,
 * including tier definitions, visual effects, fonts, and behavior parameters.
 *
 * ## :material-file-cog-outline: Configuration File
 *
 * Settings are loaded from `Data/SKSE/Plugins/whois.ini` using a simple
 * key-value format with section headers for tier definitions.
 *
 * | Section            | Purpose                                  |
 * |--------------------|------------------------------------------|
 * | `[General]`        | Core settings, fonts, distances          |
 * | `[TierN]`          | Per-tier colors, effects, ornaments      |
 * | `[SpecialTitleN]`  | Keyword-based title overrides            |
 * | `[Appearance]`     | NPC appearance template settings         |
 *
 * ## :material-refresh: Hot Reload
 *
 * Settings can be reloaded at runtime by pressing the configured `ReloadKey`
 * (default: F7). This calls `Load()` and clears the actor cache.
 *
 * ## :material-blur-linear: Distance Fading
 *
 * Nameplate alpha fades based on distance $d$ from camera to actor using
 * a squared smoothstep for a gentle falloff:
 *
 * $$t = \text{clamp}\!\left(\frac{d - d_{start}}{d_{end} - d_{start}},\; 0,\; 1\right)$$
 *
 * $$\text{smoothstep}(t) = 3t^2 - 2t^3$$
 *
 * $$\alpha = 1 - \text{smoothstep}(t)^2$$
 *
 * This produces near-full opacity at close range, a gradual falloff through
 * mid-range, and rapid fadeout approaching `FadeEndDistance`.
 *
 * ## :material-format-font-size-decrease: Font Scaling
 *
 * Font size scales down with distance using square root falloff, which
 * keeps text readable longer before shrinking:
 *
 * $$t = \text{clamp}\!\left(\frac{d - d_{start}}{d_{end} - d_{start}},\; 0,\; 1\right)$$
 *
 * $$scale = 1 + (scale_{min} - 1) \cdot \sqrt{t}$$
 *
 * At $d = d_{start}$ the scale is $1.0$ (full size). At $d = d_{end}$
 * it reaches $scale_{min}$ (default $0.1$, or 10% of original).
 *
 * ## :material-connection: SKSE Integration
 *
 * | Resource       | Path / API                                                |
 * |----------------|-----------------------------------------------------------|
 * | Settings file  | `Data/SKSE/Plugins/whois.ini`                             |
 * | Log file       | `Documents/My Games/Skyrim Special Edition/SKSE/whois.log`|
 * | Actor data     | `RE::TESDataHandler`                                      |
 */
namespace Settings
{
    /**
     * Display format segment for nameplate composition.
     *
     * Defines a single segment of the nameplate display. Multiple segments
     * are concatenated horizontally to form the complete nameplate.
     *
     * **Format Placeholders:**
     * - `%n` - Actor's display name
     * - `%l` - Actor's level
     * - `%t` - Tier title (from TierDefinition)
     *
     * ```ini
     * ; Example: "Lydia [42]" with name in name font, level in level font
     * [Display]
     * Segment1 = %n,0
     * Segment2 = " [",0
     * Segment3 = %l,1
     * Segment4 = "]",0
     * ```
     *
     * @see DisplayFormat, TierDefinition
     */
    struct Segment {
        std::string format;     ///< Format string with placeholders (%n, %l, %t)
        bool useLevelFont;      ///< If true, uses level font; otherwise uses name font
    };

    /**
     * Visual effect types for text rendering.
     *
     * Defines the available visual effects that can be applied to
     * names, levels, and titles. Effects are rendered using ImGui's
     * draw list with per-vertex coloring.
     *
     * **Effect Categories:**
     * - Static: None, Gradient, VerticalGradient, DiagonalGradient, RadialGradient
     * - Animated: Shimmer, ChromaticShimmer, PulseGradient, RainbowWave, ConicRainbow, Aurora
     * - Complex: Sparkle, Plasma, Scanline
     *
     * @see EffectParams, TextEffects::ApplyVertexEffect
     */
    enum class EffectType {
        None,                    ///< No effect, solid color
        Gradient,                ///< Horizontal gradient (left to right)
        VerticalGradient,        ///< Vertical gradient (top to bottom)
        DiagonalGradient,        ///< Diagonal gradient (requires direction in param1, param2)
        RadialGradient,          ///< Radial gradient from center (param1 = gamma)
        Shimmer,                 ///< Moving highlight band (param1 = width, param2 = strength)
        ChromaticShimmer,        ///< Chromatic aberration shimmer (param1-4 for tuning)
        PulseGradient,           ///< Pulsing brightness modulation (param1 = freq, param2 = amp)
        RainbowWave,             ///< Animated rainbow (param1-5 for hue/speed/saturation)
        ConicRainbow,            ///< Circular rainbow rotation (param1-4 for tuning)
        Aurora,                  ///< Northern lights effect (param1 = speed, param2 = waves, param3 = intensity, param4 = sway)
        Sparkle,                 ///< Glittering stars (param1 = density, param2 = speed, param3 = intensity)
        Plasma,                  ///< Demoscene plasma pattern (param1 = freq1, param2 = freq2, param3 = speed)
        Scanline                 ///< Horizontal scanning bar (param1 = speed, param2 = width, param3 = intensity)
    };

    /**
     * Parameters for visual effects.
     *
     * Generic parameter structure for effect configuration. Different effects
     * interpret the parameters differently. See EffectType documentation for
     * parameter meanings per effect.
     *
     * ```cpp
     * // Aurora effect: speed=0.5, waves=3.0, intensity=1.0, sway=0.3
     * EffectParams aurora;
     * aurora.type = EffectType::Aurora;
     * aurora.param1 = 0.5f;   // speed
     * aurora.param2 = 3.0f;   // wave count
     * aurora.param3 = 1.0f;   // intensity
     * aurora.param4 = 0.3f;   // sway amount
     * ```
     *
     * @see EffectType, TierDefinition, TextEffects::ApplyVertexEffect
     */
    struct EffectParams {
        EffectType type = EffectType::Gradient;  ///< Effect type to apply

        float param1 = 0.0f;  ///< Effect parameter 1 (meaning varies by effect)
        float param2 = 0.0f;  ///< Effect parameter 2
        float param3 = 0.0f;  ///< Effect parameter 3
        float param4 = 0.0f;  ///< Effect parameter 4
        float param5 = 0.0f;  ///< Effect parameter 5

        bool useWhiteBase = false;  ///< Draw white base layer under rainbow effects for brightness
    };

    /**
     * Tier definition for level-based visual styling.
     *
     * Each tier defines visual properties for a range of character levels.
     * Higher tiers typically have more elaborate visual effects. Tiers are
     * defined in `[TierN]` sections of the INI file.
     *
     * **Color Format:**
     * Colors are specified as comma-separated RGB floats in range [0.0, 1.0].
     *
     * ```ini
     * [Tier5]
     * MinLevel = 30
     * MaxLevel = 39
     * Title = Veteran
     * LeftColor = 0.2, 0.6, 1.0
     * RightColor = 0.8, 0.2, 1.0
     * HighlightColor = 1.0, 1.0, 1.0
     * TitleEffect = Shimmer, 0.3, 0.8
     * NameEffect = Gradient
     * LevelEffect = Gradient
     * ```
     *
     * @see Tiers, EffectParams, Renderer::Draw
     */
    struct TierDefinition {
        uint16_t minLevel;           ///< Minimum level for this tier (inclusive)
        uint16_t maxLevel;           ///< Maximum level for this tier (inclusive)
        std::string title;           ///< Title text (e.g., "Novice", "Legend of Tamriel")
        float leftColor[3];          ///< RGB color for left/top of gradients
        float rightColor[3];         ///< RGB color for right/bottom of gradients
        float highlightColor[3];     ///< RGB color for shimmer/sparkle highlights

        EffectParams titleEffect;    ///< Visual effect for title text (player only)
        EffectParams nameEffect;     ///< Visual effect for name text (player only)
        EffectParams levelEffect;    ///< Visual effect for level text (all actors)

        std::string leftOrnaments;   ///< Left side ornament characters (e.g., "AC"), empty = no ornaments
        std::string rightOrnaments;  ///< Right side ornament characters (e.g., "BD"), empty = no ornaments

        // Per-tier particle settings (empty = use global, "None" = disabled)
        std::string particleTypes;   ///< Particle types: "Stars,Wisps,Orbs,Sparks,Runes" (comma-separated)
        int particleCount;           ///< Number of particles (0 = use global setting)
    };

    /**
     * @brief Special title definition for MMORPG-style staff/VIP nameplates.
     *
     * Special titles override normal tier styling when an actor's name contains
     * the specified keyword. Used for Admin, Moderator, VIP, etc. nameplates.
     *
     * | Field | Description |
     * |-------|-------------|
     * | keyword | Text to match in actor name (case-insensitive) |
     * | displayTitle | Title shown above name (e.g., "[ADMIN]") |
     * | color | RGB color for the nameplate |
     * | glowColor | RGB color for enhanced glow effect |
     * | forceOrnaments | Always show ornaments regardless of tier |
     * | forceParticles | Always show particle effects |
     * | priority | Higher priority special titles take precedence |
     */
    struct SpecialTitleDefinition {
        std::string keyword;         ///< Keyword to match in name (case-insensitive)
        std::string displayTitle;    ///< Title to display (e.g., "[ADMIN]", "★ Moderator ★")
        float color[3];              ///< RGB color for name/title
        float glowColor[3];          ///< RGB glow color (more saturated)
        bool forceOrnaments;         ///< Always show ornaments
        bool forceParticles;         ///< Always show particle aura
        int priority;                ///< Higher = checked first
        std::string leftOrnaments;   ///< Left side ornament characters
        std::string rightOrnaments;  ///< Right side ornament characters
    };

    // Display Format
    extern std::string TitleFormat;             ///< Format string for title line (e.g., "%t")
    extern std::vector<Segment> DisplayFormat;  ///< Segments for main nameplate line

    // Tier Definitions
    extern std::vector<TierDefinition> Tiers;  ///< All tier definitions (indexed by tier number)

    // Special Titles (Admin, Moderator, VIP, etc.)
    extern std::vector<SpecialTitleDefinition> SpecialTitles;  ///< Special title overrides

    // Distance & Visibility
    extern float FadeStartDistance;      ///< Distance where fade begins (default: 200.0)
    extern float FadeEndDistance;        ///< Distance where fully transparent (default: 2500.0)
    extern float ScaleStartDistance;     ///< Distance where font size scaling begins (default: 200.0)
    extern float ScaleEndDistance;       ///< Distance where minimum font size is reached (default: 2500.0)
    extern float MinimumScale;           ///< Smallest font size multiplier (default: 0.1)
    extern float MaxScanDistance;        ///< Maximum actor scan distance (default: 3000.0)

    // Occlusion Culling
    extern bool  EnableOcclusionCulling;   ///< Enable LOS-based occlusion (default: true)
    extern float OcclusionSettleTime;      ///< Fade settle time in seconds (default: 0.58)
    extern int   OcclusionCheckInterval;   ///< Frames between LOS checks (default: 3)

    // Shadow & Visual Effects
    extern float TitleShadowOffsetX;     ///< Title shadow X offset in pixels (default: 2.0)
    extern float TitleShadowOffsetY;     ///< Title shadow Y offset in pixels (default: 2.0)
    extern float MainShadowOffsetX;      ///< Main text shadow X offset (default: 4.0)
    extern float MainShadowOffsetY;      ///< Main text shadow Y offset (default: 4.0)
    extern float SegmentPadding;         ///< Horizontal padding between segments (default: 4.0)

    // Outline Settings
    extern float OutlineWidthMin;        ///< Base outline width (default: 2.0)
    extern float OutlineWidthMax;        ///< Additional width for high tiers (default: 2.5)
    extern bool  FastOutlines;           ///< Use 4-dir outlines instead of 8-dir (default: false)

    // Glow Effect
    extern bool  EnableGlow;             ///< Enable glow effect (default: false)
    extern float GlowRadius;             ///< Glow spread in pixels (default: 6.0)
    extern float GlowIntensity;          ///< Glow brightness 0-1 (default: 0.6)
    extern int   GlowSamples;            ///< Quality samples 8-16 (default: 12)

    // Typewriter Effect
    extern bool  EnableTypewriter;       ///< Enable typewriter reveal (default: false)
    extern float TypewriterSpeed;        ///< Characters per second (default: 30.0)
    extern float TypewriterDelay;        ///< Delay before reveal starts (default: 0.0)

    // Debug Settings
    extern bool  EnableDebugOverlay;     ///< Show performance/cache overlay (default: false)

    // Side Ornaments
    extern bool  EnableOrnaments;        ///< Enable side ornaments (default: true)
    extern float OrnamentScale;          ///< Size multiplier (default: 1.0)
    extern float OrnamentSpacing;        ///< Distance from text edges (default: 3.0)

    /**
     * Visual styles for particle aura effects.
     *
     * @see EnableParticleAura, TextEffects::DrawParticleAura
     */
    enum class ParticleStyle {
        Stars,      ///< Twinkling blue star points
        Sparks,     ///< Fast, yellowish fire-like sparks
        Wisps,      ///< Slow, ethereal wisps with pale/blue tint
        Runes,      ///< Small magical rune symbols
        Orbs        ///< Soft glowing orbs
    };

    // Particle Aura
    extern bool  EnableParticleAura;     ///< Master enable for particle aura (default: true)
    extern bool  UseParticleTextures;    ///< Use texture sprites instead of shapes (default: true)
    // Textures are now loaded from subfolders: Data/SKSE/Plugins/whois/particles/<type>/

    extern bool  EnableStars;            ///< Enable twinkling stars (default: true)
    extern bool  EnableSparks;           ///< Enable fire-like sparks (default: false)
    extern bool  EnableWisps;            ///< Enable ethereal wisps (default: false)
    extern bool  EnableRunes;            ///< Enable magical runes (default: false)
    extern bool  EnableOrbs;             ///< Enable glowing orbs (default: false)
    extern int   ParticleCount;          ///< Particles per type (default: 8)
    extern float ParticleSize;           ///< Particle size in pixels (default: 3.0)
    extern float ParticleSpeed;          ///< Animation speed multiplier (default: 1.0)
    extern float ParticleSpread;         ///< How far particles spread from text (default: 20.0)
    extern float ParticleAlpha;          ///< Maximum particle opacity (default: 0.8)

    // Display Options
    extern float VerticalOffset;         ///< Height above actor's head in units (default: 8.0)
    extern bool  HidePlayer;             ///< Hide player's own nameplate (default: false)
    extern int   ReloadKey;              ///< Virtual key code for hot reload (default: 0 = disabled)

    // Animation Speed
    extern float AnimSpeedLowTier;       ///< Speed for tiers 0-7 (default: 0.35)
    extern float AnimSpeedMidTier;       ///< Speed for tier 8 (default: 0.20)
    extern float AnimSpeedHighTier;      ///< Speed for tier 9+ (default: 0.1)

    // Color & Effect Intensity
    extern float ColorWashAmount;        ///< Desaturation toward white 0-1 (default: 0.5)
    extern float NameColorMix;           ///< Base color strength 0-1 (default: 0.35)
    extern float EffectAlphaMin;         ///< Minimum effect alpha (default: 0.20)
    extern float EffectAlphaMax;         ///< Maximum effect alpha (default: 0.60)
    extern float StrengthMin;            ///< Minimum effect strength (default: 0.15)
    extern float StrengthMax;            ///< Maximum effect strength (default: 0.60)

    // Smoothing
    extern float AlphaSettleTime;        ///< Alpha settle time in seconds (default: 0.46)
    extern float ScaleSettleTime;        ///< Font scale settle time in seconds (default: 0.46)
    extern float PositionSettleTime;     ///< Position settle time for NPCs in seconds (default: 0.38)

    /// Visual polish settings, encapsulated to avoid non-const globals.
    struct VisualSettings {
        // Distance-Based Outline
        bool  EnableDistanceOutlineScale = false; ///< Scale outline width by distance
        float OutlineDistanceMin = 0.8f;          ///< Outline multiplier at close range
        float OutlineDistanceMax = 1.5f;          ///< Outline multiplier at far range
        // Minimum Readable Size
        float MinimumPixelHeight = 0.0f;          ///< Min pixel height for name text, 0=disabled
        // LOD by Distance
        bool  EnableLOD = false;                  ///< Enable distance-based content LOD
        float LODFarDistance = 1800.0f;           ///< Beyond this: name+level only
        float LODMidDistance = 800.0f;            ///< Beyond this: no particles/ornaments
        float LODTransitionRange = 200.0f;        ///< Smooth transition width in game units
        // Visual Hierarchy
        float TitleAlphaMultiplier = 0.80f;       ///< Alpha multiplier for title text
        float LevelAlphaMultiplier = 0.85f;       ///< Alpha multiplier for level text
        // Overlap Prevention
        bool  EnableOverlapPrevention = false;     ///< Push overlapping labels apart
        float OverlapPaddingY = 4.0f;              ///< Vertical padding between labels
        int   OverlapIterations = 3;               ///< Relaxation passes for overlap resolution
        // Position Smoothing Tuning
        float PositionSmoothingBlend = 1.0f;       ///< 1.0=moving-avg, 0.0=exponential
        float LargeMovementThreshold = 50.0f;      ///< Pixel threshold for large movement handling
        float LargeMovementBlend = 0.5f;           ///< Blend factor for large movements
        // Tier Effect Gating
        bool  EnableTierEffectGating = false;       ///< Gate effects by tier index
        int   GlowMinTier = 5;                     ///< Minimum tier for glow effects
        int   ParticleMinTier = 10;                ///< Minimum tier for particle effects
        int   OrnamentMinTier = 10;                ///< Minimum tier for ornament display
    };
    VisualSettings& Visual();

    // Font Settings
    extern std::string NameFontPath;     ///< Path to name font TTF file
    extern float NameFontSize;           ///< Name font size in points (default: 122.0)
    extern std::string LevelFontPath;    ///< Path to level font TTF file
    extern float LevelFontSize;          ///< Level font size in points (default: 61.0)
    extern std::string TitleFontPath;    ///< Path to title font TTF file
    extern float TitleFontSize;          ///< Title font size in points (default: 42.0)

    // Ornament Font Settings
    extern std::string OrnamentFontPath;   ///< Path to ornament font (TTF/OTF)
    extern float OrnamentFontSize;         ///< Ornament font size in points (default: 64.0)

    // Appearance Template Settings
    extern std::string TemplateFormID;     ///< FormID of template NPC (hex, e.g., "0x12345")
    extern std::string TemplatePlugin;     ///< Plugin file containing template (e.g., "MyFollower.esp")
    extern bool UseTemplateAppearance;     ///< Whether to apply template appearance to player
    extern bool TemplateIncludeRace;       ///< Whether to copy race (required for cross-race templates)
    extern bool TemplateIncludeBody;       ///< Whether to copy height/body morphs
    extern bool TemplateCopyFaceGen;       ///< Whether to load and apply FaceGen NIF/tint (default: true)
    extern bool TemplateCopySkin;          ///< Whether to copy skin textures (default: false, risky for custom bodies)
    extern bool TemplateCopyOverlays;      ///< Whether to copy RaceMenu overlays if available (default: false)
    extern bool TemplateCopyOutfit;        ///< Whether to copy equipped armor from template actor (default: false)
    extern bool TemplateReapplyOnReload;   ///< Whether to re-apply appearance on hot reload (default: false)
    extern std::string TemplateFaceGenPlugin;  ///< Optional override for FaceGen plugin path (empty = auto-detect)

    /**
     * Load all settings from whois.ini.
     *
     * Parses the configuration file and populates all settings variables.
     * Called once during plugin initialization and on hot reload.
     *
     * File Location: Data/SKSE/Plugins/whois.ini
     *
     * Missing file or invalid values use defaults. No errors are thrown.
     *
     * @post All extern variables in this namespace are populated with values
     *       from the INI file or their defaults.
     * @post Tiers vector contains all `[TierN]` sections sorted by tier number.
     * @post DisplayFormat contains parsed format segments from the `[Display]` section.
     *
     * @see Renderer::Draw, TierDefinition, Segment
     */
    void Load();
}
