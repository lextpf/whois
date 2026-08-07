#pragma once

#include "RenderConstants.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

// clang-format off
/**
 * @namespace Settings
 * @brief Configuration management and INI parsing.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Settings
 *
 * Holds every user-configurable value: tier definitions, visual effects, fonts
 * and behavior parameters.
 *
 * ## :material-file-cog-outline: Configuration File
 *
 * `Data/SKSE/Plugins/glyph.ini`, a key-value file with section headers.
 *
 * | Section           | Purpose                                            |
 * |-------------------|----------------------------------------------------|
 * | `[General]`       | Core scalars: fonts, distances, effects, behavior   |
 * | `[TierN]`         | Per-tier colors, effects, ornaments, particles      |
 * | `[SpecialTitleN]` | Keyword-based title and style overrides             |
 * | `[HonorificN]`    | Faction-driven honorific titles                     |
 * | `[RegisterN]`     | Context-conditional overlay profiles                |
 * | `[Labels]`        | Label strings for the `%r`, `%d`, `%c` tokens       |
 * | `[LevelDelta]`    | Level-delta classification thresholds               |
 * | `[Icons]`         | Status icon badges and tier emblems                 |
 * | `[Focus]`         | Focus-target expanded nameplate                     |
 * | `[Deck]`          | Collectible character card capture                  |
 * | `[Graffito]`      | Perspective-correct actor-plane text                |
 * | `[Quiet]`         | Camera-motion quieting                              |
 * | `[DeathRite]`     | One-shot death animation                            |
 * | `[Compat]`        | TrueHUD / moreHUD deconfliction                     |
 * | `[Candlelight]`   | Exposure-adaptive brightness                        |
 * | `[DepthClip]`     | Per-pixel depth occlusion                           |
 *
 * The parser also accepts `[Display]`, `[Visual]`, `[Fonts]`, `[Particles]`,
 * `[Occlusion]` and `[Debug]` without a warning, but the shipped `glyph.ini`
 * uses none of them.  Keys written before the first section header are accepted
 * as global scope; the shipped `Format` key sits there.  Any other section name
 * produces one parse warning per distinct name, and its keys are still matched
 * against the scalar table.
 *
 * @note Scalar settings are matched by key name regardless of which section
 * they appear in.  The four indexed families are the exception: inside
 * `[TierN]`, `[SpecialTitleN]`, `[HonorificN]` and `[RegisterN]` the section's
 * own field names (MinLevel, MaxLevel, Name/Title, Color, GlowColor,
 * TitleEffect, ParticleTypes, ParticleCount, Priority, Faction, MinRank,
 * When, ...) are consumed by the section parser first and never reach the
 * scalar table.  `ParticleCount` inside `[Tier0]` therefore sets that tier's
 * count, not the global `ParticleCount`.
 *
 * @note Runtime defaults for scalars live in the `kSettings` binding table in
 * `Settings.cpp`.  `Load()` calls `ResetToDefaults()` first, which applies
 * those defaults to every field, then parses `glyph.ini` over them.  The
 * struct member initializers below are compile-time placeholders and may
 * differ from the operative `kSettings` values.  The indexed families are not
 * in `kSettings`: `TierDefinition` defaults come from `MakeDefaultTier()` and
 * `SpecialTitleDefinition` defaults are assigned inline in `Load()` when a
 * section grows the vector.
 *
 * ## :material-refresh: Hot Reload
 *
 * `ReloadKey` is a Win32 virtual key code.  Zero or a negative value disables
 * the key.  The `kSettings` default is 0, which applies when the key is absent
 * or `glyph.ini` is missing; the shipped `glyph.ini` sets 118, the code for F7.
 *
 * The render thread only requests the reload.  `Load()` itself is queued to the
 * game thread through the SKSE task interface, so the file read never stalls a
 * frame, and snapshot updates stay paused for the whole operation.  The render
 * thread clears the actor cache and requests an occlusion-cache clear when the
 * reload reports back.  When the task interface is unavailable, the render
 * thread runs `Load()` itself; this is safe because nothing in the parser
 * dereferences a game object.
 *
 * ```mermaid
 * sequenceDiagram
 *     participant R as Render thread
 *     participant G as Game thread
 *     R->>R: ReloadKey edge sets reloadRequested
 *     R->>R: in-flight snapshot task drains, then pauseSnapshotUpdates
 *     R->>G: AddTask(Settings::Load)
 *     G->>G: Load() under a unique Mutex() lock, then Generation() + 1
 *     G-->>R: reloadCompleted
 *     R->>R: clear actor cache and occlusion cache, resume snapshots
 * ```
 *
 * ## :material-blur-linear: Distance Fading
 *
 * Nameplate alpha fades with the distance $d$ from the player to the actor
 * (not from the camera), through a squared quintic smoothstep:
 *
 * $$d'_{start} = d_{start} \cdot m_{fade}, \quad d'_{end} = d_{end} \cdot m_{fade}$$
 *
 * $$t = \text{clamp}\!\left(\frac{d - d'_{start}}{d'_{end} - d'_{start}},\; 0,\; 1\right)$$
 *
 * $$\text{smoothstep}(t) = 6t^5 - 15t^4 + 10t^3$$
 *
 * $$\alpha = \left(1 - \text{smoothstep}(t)\right)^2$$
 *
 * The denominator is floored at 1 game unit, so a register multiplier that
 * collapses the envelope cannot divide by zero.
 *
 * $m_{fade}$ is the active register's smoothed `FadeDistanceMultiplier`.  It
 * is 1.0 when no register matches or when `RegistersEnabled = false`.  The
 * shipped INI defines `[Register0]` with `FadeDistanceMultiplier = 0.85`, so
 * a stock install pulls the envelope in at night in exteriors.
 *
 * The result is near-full opacity at close range, a gradual falloff through
 * mid-range, and rapid fadeout approaching `FadeEndDistance`.
 *
 * ## :material-format-font-size-decrease: Font Scaling
 *
 * Font size falls off with the square root of distance, which keeps text
 * readable longer before it shrinks:
 *
 * $$t = \text{clamp}\!\left(\frac{d - d_{start}}{d_{end} - d_{start}},\; 0,\; 1\right)$$
 *
 * $$scale = 1 + (scale_{min} - 1) \cdot \sqrt{t}$$
 *
 * Absent the two stages below, the scale is $1.0$ (full size) at
 * $d = d_{start}$ and reaches $scale_{min}$ (default $0.1$, or 10% of
 * original) at $d = d_{end}$.
 *
 * The denominator is floored at 1 game unit here as well.
 *
 * The curve is evaluated twice, on the player-to-actor distance and on the
 * camera-to-actor distance.  The camera term is skipped when the player camera
 * is unavailable.  Otherwise the smaller result wins, so a camera pulled back
 * behind the player shrinks the text further:
 *
 * $$scale = \min\left(scale_{player},\; scale_{camera}\right)$$
 *
 * When `MinimumPixelHeight` is above zero, a pixel floor is applied last:
 *
 * $$scale = \max\left(scale,\; \text{MinimumPixelHeight} / \text{NameFontSize}\right)$$
 *
 * The register `FadeDistanceMultiplier` does not scale $d_{start}$ or
 * $d_{end}$ here; it applies to the alpha fade only.
 *
 * ## :material-connection: SKSE Integration
 *
 * | Resource       | Path / API                                                |
 * |----------------|-----------------------------------------------------------|
 * | Settings file  | `Data/SKSE/Plugins/glyph.ini`                             |
 * | Log file       | `Documents/My Games/Skyrim Special Edition/SKSE/glyph.log`|
 * | Actor data     | `RE::TESDataHandler`                                      |
 */
// clang-format on
namespace Settings
{
/**
 * @struct Color3
 * @brief RGB color triplet.
 * @ingroup Settings
 *
 * Backs every color field.  Channel values are in [0.0, 1.0].  `clamp01()`
 * constrains all three channels in place and returns the object; `White()`
 * builds the all-ones default.
 */
struct Color3
{
    float r = 1.f, g = 1.f, b = 1.f;

    constexpr Color3() = default;
    constexpr Color3(float r_, float g_, float b_)
        : r(r_),
          g(g_),
          b(b_)
    {
    }

    constexpr Color3& clamp01()
    {
        r = std::clamp(r, 0.f, 1.f);
        g = std::clamp(g, 0.f, 1.f);
        b = std::clamp(b, 0.f, 1.f);
        return *this;
    }

    static constexpr Color3 White() { return {1.f, 1.f, 1.f}; }
    constexpr bool operator==(const Color3&) const = default;
};

/**
 * @struct Segment
 * @brief Display format segment for nameplate composition.
 * @ingroup Settings
 *
 * Segments are concatenated horizontally to form one nameplate row.
 *
 * **Format Placeholders:**
 * - `%n` - Actor's display name
 * - `%l` - Actor's level
 * - `%t` - Title, expanded on the title line only.  Every other row copies the
 *   two characters through literally.
 * - `%r` - Relationship label, from LabelSettings
 * - `%d` - Level-delta label, from LabelSettings
 * - `%c` - Creature-kind label, from LabelSettings
 *
 * Substitution is single pass, so a placeholder inside a substituted value is
 * not expanded again.
 *
 * Segments are not separate INI keys.  One `Format` key holds the whole row
 * as a run of double-quoted segments, and one `InfoFormat` key holds the
 * info row the same way.  Both keys are matched in any section; the shipped
 * INI puts `Format` at global scope.
 *
 * ```ini
 * ; Three segments. The first holds %t and becomes the title line, with a
 * ; literal quote on each side. The other two form the main row: Lydia|lv.42
 * Format = "\"%t\"" "%n" "|lv.%l"
 * ```
 *
 * Parsing rules:
 * - A segment that contains `%t` is siphoned into the title line instead of
 *   the main row.  Only `Format` siphons; a `%t` segment inside `InfoFormat`
 *   stays in the info row.  When several `Format` segments contain `%t`, the
 *   last one wins.
 * - A `Format` value that yields no segments leaves the previous main row in
 *   place, so an empty `Format` cannot blank the nameplate.  An empty
 *   `InfoFormat` always clears the info row.
 * - A segment that contains `%l` selects the level font automatically; there
 *   is no per-segment font selector.  Every `InfoFormat` segment uses the
 *   level font.
 * - A `?` placed immediately after a segment's closing quote sets
 *   `dropIfBlank` for that segment.
 *
 * @see DisplayFormat, InfoFormat, TierDefinition
 */
struct Segment
{
    std::string format;         ///< Format string with placeholders (%n, %l, %t, %r, %d, %c)
    bool useLevelFont = false;  ///< If true, uses level font; otherwise uses name font
    /// Set by trailing `?` in Format / InfoFormat - segment is
    /// omitted when its expansion trims to empty.
    bool dropIfBlank = false;
};

/**
 * @enum EffectType
 * @brief Visual effect types for text rendering.
 * @ingroup Settings
 *
 * Applies to names, levels and titles.  Effects are drawn through ImGui's
 * draw list with per-vertex coloring.
 *
 * **Effect Categories:** the animated and complex groups match one
 * implementation translation unit each.
 * - Static: None, Gradient, VerticalGradient, DiagonalGradient, RadialGradient.
 *   The four gradients are in TextEffectsGradient.cpp; None is the plain fill,
 *   drawn by the shared outline path in TextEffectsCore.cpp.
 * - Animated: Shimmer, Ember, Breathe, Mote, Wander, Eclipse, Pulse, Electric
 *   (TextEffectsAnimated.cpp)
 * - Complex: Aurora, Sparkle, Enchant, Frost, Drift (TextEffectsComplex.cpp)
 *
 * @see EffectParams, TextEffects::ApplyVertexEffect
 */
enum class EffectType
{
    None,              ///< No effect, solid color
    Gradient,          ///< Horizontal gradient (left to right)
    VerticalGradient,  ///< Vertical gradient (top to bottom)
    DiagonalGradient,  ///< Diagonal gradient (requires direction in param1, param2)
    RadialGradient,    ///< Radial gradient from center (param1 = gamma)
    Shimmer,           ///< Moving highlight band (param1 = width, param2 = strength)
    Ember,             ///< Warm flickering glow (param1 = speed, param2 = intensity)
    /// Northern lights effect (param1 = speed, param2 = waves, param3 = intensity,
    /// param4 = sway)
    Aurora,
    Sparkle,  ///< Glittering stars (param1 = density, param2 = speed, param3 = intensity)
    Enchant,  ///< Flowing magical energy (param1 = speed, param2 = scale, param3 = intensity)
    Frost,    ///< Crystalline ice sparkle (param1 = density, param2 = speed, param3 = intensity)
    Breathe,  ///< Slow uniform brightness pulse (param1 = speed Hz, param2 = amplitude)
    Drift,    ///< Slow uniform hue wander (param1 = speed Hz, param2 = hue range degrees)
    Mote,     ///< Rare single twinkle (param1 = period s, param2 = peak alpha)
    /// Per-character asynchronous breathing (param1 = speed Hz, param2 = amplitude,
    /// param3 = phase spread)
    Wander,
    /// Shadow band sweeping with a hot leading rim (param1 = width,
    /// param2 = strength)
    Eclipse,
    Pulse,    ///< Weighty two-beat heartbeat glow (param1 = rate Hz, param2 = amplitude)
    Electric  ///< Rare crackling arc sweeping the text (param1 = rate Hz, param2 = intensity)
};

/**
 * @struct EffectParams
 * @brief Parameters for visual effects.
 * @ingroup Settings
 *
 * Each effect interprets the parameters differently; see EffectType for the
 * per-effect meanings.
 *
 * ```cpp
 * // Aurora effect: speed=0.5, waves=3.0, intensity=1.0, sway=0.3
 * EffectParams aurora;
 * aurora.type = EffectType::Aurora;
 * aurora.param1 = .5f;   // speed
 * aurora.param2 = 3.0f;   // wave count
 * aurora.param3 = 1.0f;   // intensity
 * aurora.param4 = .3f;   // sway amount
 * ```
 *
 * @see EffectType, TierDefinition, TextEffects::ApplyVertexEffect
 */
struct EffectParams
{
    EffectType type = EffectType::Gradient;  ///< Effect type to apply

    float param1 = .0f;  ///< Effect parameter 1 (meaning varies by effect)
    float param2 = .0f;  ///< Effect parameter 2
    float param3 = .0f;  ///< Effect parameter 3
    float param4 = .0f;  ///< Effect parameter 4
    float param5 = .0f;  ///< Effect parameter 5; parsed, but no effect reads it

    /// Set by the trailing `whiteBase` token in an effect string.  Parsed and
    /// stored, but no draw path reads it, so it currently changes nothing.
    bool useWhiteBase = false;
};

/**
 * @struct TierDefinition
 * @brief Tier definition for level-based visual styling.
 * @ingroup Settings
 *
 * One tier covers a range of character levels and supplies the visual
 * properties for actors in it.  Tiers come from `[TierN]` INI sections.
 *
 * **Color Format:**
 * Comma-separated RGB floats in range [0.0, 1.0].
 *
 * ```ini
 * [Tier5]
 * MinLevel = 30
 * MaxLevel = 39
 * Title = Veteran
 * LeftColor = 0.2, 0.6, 1.0
 * RightColor = 0.8, 0.2, 1.0
 * HighlightColor = 1.0, 1.0, 1.0
 * TitleEffect = Shimmer 0.3,0.8
 * NameEffect = Gradient
 * LevelEffect = Gradient
 * ```
 *
 * **Effect Syntax:**
 * Whitespace separates the effect name from its parameter list; the
 * parameters themselves are comma-separated.  A comma directly after the
 * effect name becomes part of the name token, so the lookup misses and the
 * effect falls back to `EffectType::Gradient` without a warning.  At most five
 * parameters are read; the rest are dropped.  The optional token `whiteBase`
 * sets `EffectParams::useWhiteBase`.  It is matched case-insensitively anywhere
 * in the parameter text, and everything from that position on is dropped.
 *
 * @see Tiers, EffectParams, Renderer::Draw
 */
struct TierDefinition
{
    uint16_t minLevel = 1;          ///< Minimum level for this tier (inclusive)
    uint16_t maxLevel = 250;        ///< Maximum level for this tier (inclusive)
    std::string title = "Unknown";  ///< Title text (e.g., "Novice", "Legend of Tamriel")
    Color3 leftColor;               ///< RGB color for name gradient left/top
    Color3 rightColor;              ///< RGB color for name gradient right/bottom
    Color3 highlightColor;          ///< RGB color for shimmer/sparkle highlights

    // Per-element color overrides.  Each empty optional falls back to a value
    // derived from the three colors above; the derivations are named below.
    std::optional<Color3>
        titleLeftColor;  ///< Title gradient left (default: leftColor blended 25% toward white)
    std::optional<Color3>
        titleRightColor;  ///< Title gradient right (default: rightColor blended 25% toward white)
    std::optional<Color3>
        levelLeftColor;  ///< Level gradient left (default: leftColor blended 40% toward white)
    std::optional<Color3> levelRightColor;  ///< Level gradient right (rightColor, 40% toward white)
    std::optional<Color3> particleColor;    ///< Particle tint (default: highlightColor)
    /// Ornament gradient left.  When empty, the nameplate derives it from the
    /// resolved title-left color: mixed 32% toward highlightColor, then 15% back
    /// toward the name color, then saturation-boosted 1.25x.  A Deck card uses a
    /// simpler fallback, the resolved title-left color itself.
    std::optional<Color3> ornamentLeftColor;
    /// Ornament gradient right.  Same recipe on the right-side colors, except
    /// that the nameplate mixes 42% toward highlightColor instead of 32%.
    std::optional<Color3> ornamentRightColor;

    EffectParams titleEffect;  ///< Visual effect for title text (player / special titles)
    EffectParams nameEffect;   ///< Visual effect for name text (player / special titles)
    EffectParams levelEffect;  ///< Visual effect for level text (player / special titles)

    std::string
        leftOrnaments;  ///< Left side ornament characters (e.g., "AC"), empty = no ornaments
    std::string
        rightOrnaments;  ///< Right side ornament characters (e.g., "BD"), empty = no ornaments

    // Per-tier particle settings. Only particleCount falls back to a global.
    /// Comma-separated particle style tokens, e.g. "Snow,Firefly,Wisp,Runes".
    /// Tokens are the case-insensitive names in `kParticleStyleTokens`; an
    /// unknown token matches nothing and renders nothing.  An entry may carry
    /// a weight as `token:weight`, e.g. "snow:2,wisp:0.5".  A weight is clamped
    /// to 0.1-10 and sets the ratio between the listed styles only: the weights
    /// are mean-normalized, so the tier's total particle budget holds and equal
    /// weights behave exactly like no weights.  Empty or "None" disables
    /// particles for this tier.
    std::string particleTypes;
    int particleCount = 0;  ///< Number of particles (0 = use the global ParticleCount)
};

/**
 * @struct SpecialTitleDefinition
 * @brief Name-keyword override of the normal tier styling.
 *
 * When an actor's name contains `keyword`, this definition replaces the tier
 * styling, giving staff- or VIP-style nameplates (Admin, Moderator, VIP).
 * Defined in `[SpecialTitleN]` INI sections.
 */
struct SpecialTitleDefinition
{
    std::string keyword;       ///< Keyword to match in name (case-insensitive)
    std::string keywordLower;  ///< Cached lowercase keyword for fast runtime matching
    std::string displayTitle;  ///< Title to display
    Color3 color;              ///< RGB color for name/title
    Color3 glowColor;          ///< RGB glow color (more saturated)
    /// Always show ornaments.  Omitting `ForceOrnaments` gives true, not false:
    /// `Load()` assigns true when a `SpecialTitleN` section grows the vector.
    /// The legacy key `ForceFlourishes` sets the same field.
    bool forceOrnaments;
    /// Always show particle aura.  Omitting `ForceParticles` gives true, as
    /// above.  This is the only route to particles on an NPC plate.
    bool forceParticles;
    int priority;                ///< Higher = checked first (0 when the section omits Priority)
    std::string leftOrnaments;   ///< Left side ornament characters
    std::string rightOrnaments;  ///< Right side ornament characters
};

/**
 * @struct HonorificDefinition
 * @brief Faction-driven honorific title.
 *
 * An actor (the player included) that holds at least `minRank` in the resolved
 * faction shows `title` in the title slot instead of the static tier title.
 * Defined in `[HonorificN]` INI sections; the highest-priority match wins, and
 * name-keyword special titles still take precedence.
 *
 * `factionSpec` is `0xFORMID` or `0xFORMID@Plugin.esp` (plugin defaults to
 * Skyrim.esm), resolved against the load order on the game thread.  A spec that
 * does not resolve logs one warning per settings generation and never matches.
 * On a priority tie the first match found wins.  Inside one faction that is the
 * lower index in Honorifics(), but between two factions it is whichever faction
 * the actor's faction walk reaches first, which is not index order.
 */
struct HonorificDefinition
{
    std::string factionSpec;  ///< "0xFORMID[@Plugin.esp]" faction reference
    std::string title;        ///< Honorific text shown in the title slot
    int minRank = 0;          ///< Minimum faction rank required
    int priority = 0;         ///< Higher wins when several factions match
    bool playerOnly = false;  ///< Only the player's plate may earn this
    bool npcOnly = false;     ///< Only NPC plates may earn this
};

/**
 * @namespace Settings::Context
 * @brief Scene-context predicate bits for register selection.

 * *
 * The game thread computes the current mask once per snapshot. A register matches when all
 *
 * bits in `whenMask` are set and no bit in `whenNotMask` is set.
 */
namespace Context
{
inline constexpr uint32_t Interior = 1u << 0;  ///< Player's cell is interior
inline constexpr uint32_t Night = 1u << 1;     ///< Game hour outside [6, 20)
inline constexpr uint32_t City = 1u << 2;      ///< Location keyword LocTypeCity/Town
inline constexpr uint32_t Sneaking = 1u << 3;  ///< Player is sneaking
inline constexpr uint32_t Dialogue = 1u << 4;  ///< Dialogue menu speaker active
inline constexpr uint32_t Crowded = 1u << 5;   ///< Visible plates >= threshold
}  // namespace Context

/**
 * @struct RegisterDefinition
 * @brief One context-conditional overlay profile.
 *
 * Binds a small knob set to scene predicates so the overlay adjusts itself
 * instead of needing per-situation retuning: dimmer at night, fewer plates in
 * a crowded city,
 * quieter sub-lines in dialogue.  The highest-priority
 * matching register is active; transitions
 * ease over
 * `RegisterSettings::TransitionTime` on the render thread.  An empty `When`
 * matches
 * every scene.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 *
 * flowchart TD
 *     C[Game thread computes the context mask] --> G{Registers enabled?}
 *     G
 * -- No --> N[Publish activeRegister = -1]
 *     G -- Yes --> R[Scan configured entries in index
 * order]
 *     R --> M{All required bits set<br/>and all forbidden bits clear?}
 *     M -- No -->
 * Q{More entries?}
 *     M -- Yes --> P{Priority greater than the current best?}
 *     P -- Yes
 * --> B[Replace the best match]
 *     P -- No --> K[Keep the earlier match]
 *     B --> Q
 * K
 * --> Q
 *     Q -- Yes --> R
 *     Q -- No --> U[Publish the best index, or -1]
 *     U -->
 * T[Render thread reads activeRegister]
 *     N --> T
 *     T --> E[Ease alpha, fade, sub-line,
 * and hide-neutral values]
 * ```
 */
struct RegisterDefinition
{
    std::string name;          ///< Optional label (logs/debugging)
    uint32_t whenMask = 0;     ///< Context bits that must all be set
    uint32_t whenNotMask = 0;  ///< Context bits that must all be clear
    float alphaMul = 1.0f;     ///< Overlay-wide alpha multiplier [0,1]
    /// Alpha-fade distance multiplier, INI key `FadeDistanceMultiplier`, clamped
    /// to 0.2 - 2.  It scales `FadeStartDistance` and `FadeEndDistance` only;
    /// the font-scale distances are not affected.
    float fadeMul = 1.0f;
    float subLineMul = 1.0f;   ///< Title/info/level/badge alpha multiplier [0,1]
    bool hideNeutral = false;  ///< Hide neutral + ally NPC plates entirely
    int priority = 0;          ///< Highest-priority match wins

    /// True once any key parsed inside this section.  A gap in the RegisterN
    /// numbering back-fills with default entries whose empty `When` would
    /// otherwise match every scene and shadow a real register on a priority
    /// tie, so an unconfigured entry never matches.
    bool configured = false;
};

/**
 * @struct RegisterSettings
 * @brief Global controls for context-conditional setting profiles.

 * *
 * `Registers` stores the profiles.
 */
struct RegisterSettings
{
    bool Enabled = true;          ///< Master toggle for the register system
    float TransitionTime = 1.2f;  ///< Seconds for knob transitions to settle
    int CrowdedThreshold = 12;    ///< Visible plates that count as "crowded"
};
RegisterSettings& RegisterConfig();

// Collection accessors (function-local statics for safe destruction order)
std::string& TitleFormat();             ///< Format string for title line (e.g., "%t")
std::vector<Segment>& DisplayFormat();  ///< Segments for main nameplate line (row 2)
std::vector<Segment>& InfoFormat();     ///< Segments for contextual info row (row 3, below main)
std::vector<TierDefinition>& Tiers();   ///< All tier definitions (indexed by tier number)
std::vector<SpecialTitleDefinition>& SpecialTitles();  ///< Special title overrides
std::vector<HonorificDefinition>& Honorifics();        ///< Faction-driven honorific titles
std::vector<RegisterDefinition>& Registers();          ///< Context-conditional profiles

/**
 * @struct DistanceSettings
 * @brief Distance limits for visibility fading and text scaling.

 * *
 * Each distance is in Skyrim game units. The fade envelope and scan cutoff use the distance
 *
 * from the player to the actor. The scale envelope is evaluated with the player distance and
 * the
 * camera distance. The smaller result applies. `ClampAndValidate` raises each end
 * distance to at
 * least its start distance plus one, so an envelope cannot collapse.
 */
struct DistanceSettings
{
    float FadeStartDistance = 200.0f;   ///< Distance where fade begins
    float FadeEndDistance = 2500.0f;    ///< Distance where fully transparent
    float ScaleStartDistance = 200.0f;  ///< Distance where font size scaling begins
    float ScaleEndDistance = 2500.0f;   ///< Distance where minimum font size is reached
    float MinimumScale = .1f;           ///< Smallest font size multiplier, clamped to 0.01 - 5
    /// Actor scan cutoff on the game thread, measured from the player.  An actor
    /// beyond it gets no plate and no later setting restores one.  The Deck
    /// crosshair target is the exception: it is injected past the cutoff as a
    /// capture-only entry that draws nothing.
    float MaxScanDistance = 3000.0f;
};
DistanceSettings& Distance();

/**
 * @struct OcclusionSettings
 * @brief Settings for line-of-sight occlusion culling.
 */
struct OcclusionSettings
{
    bool Enabled = true;      ///< Enable LOS-based occlusion
    float SettleTime = .58f;  ///< Fade settle time in seconds
    int CheckInterval = 3;    ///< Frames between LOS checks
};
OcclusionSettings& Occlusion();

/**
 * @struct ShadowOutlineSettings
 * @brief Settings for text shadows and outlines.
 *
 *
 * Offsets, widths, and radii are in pixels at full text scale. The renderer scales them with
 * the
 * plate.
 */
struct ShadowOutlineSettings
{
    float TitleShadowOffsetX = 2.0f;  ///< Title shadow X offset in pixels
    float TitleShadowOffsetY = 2.0f;  ///< Title shadow Y offset in pixels
    float MainShadowOffsetX = 4.0f;   ///< Main text shadow X offset
    float MainShadowOffsetY = 4.0f;   ///< Main text shadow Y offset
    /// First summand of the base outline width, in pixels.
    float OutlineWidthMin = 2.0f;
    /// Second summand of the base outline width, in pixels.  There is no tier
    /// dependence: every plate uses `OutlineWidthMin + OutlineWidthMax`.
    /// `ClampAndValidate()` raises this to `OutlineWidthMin` when it is lower,
    /// which keeps the pair ordered even though both terms are summed.
    float OutlineWidthMax = 2.5f;
    bool FastOutlines = false;  ///< Use 4-dir outlines instead of 8-dir

    // Outline Glow (white halo behind outline)
    bool OutlineGlowEnabled = false;   ///< Enable white glow behind text outline
    float OutlineGlowScale = 1.4f;     ///< Glow radius as multiplier of outline width
    float OutlineGlowAlpha = .1f;      ///< Peak glow ring opacity 0-1
    int OutlineGlowRings = 2;          ///< Concentric glow rings (1-3)
    float OutlineGlowR = 1.0f;         ///< Glow color red
    float OutlineGlowG = 1.0f;         ///< Glow color green
    float OutlineGlowB = 1.0f;         ///< Glow color blue
    bool OutlineGlowTierTint = false;  ///< Blend glow color with tier color

    // Dual-tone directional outline
    bool DualOutlineEnabled = false;  ///< Enable inner outline tinted with tier color
    float InnerOutlineTint = .3f;     ///< How much to blend toward tier color (0=outline, 1=tier)
    float InnerOutlineAlpha = .5f;    ///< Inner outline opacity multiplier
    float InnerOutlineScale = .5f;    ///< Inner outline width as fraction of outer
    float DirectionalLightAngle = 315.f;  ///< Light direction in degrees (0=right, 90=down)
    float DirectionalLightBias = .15f;    ///< Directional width variation (0=uniform)

    // Color tinting
    float OutlineColorTint = .0f;  ///< Tier-color tint for outlines 0-0.25
    float ShadowColorTint = .0f;   ///< Tier-color tint for shadows 0-0.25

    // Soft directional drop-shadow (feathered; used instead of the hard offset shadow)
    bool SoftShadowEnabled = false;   ///< Use a soft directional shadow instead of a hard offset
    float SoftShadowDistance = 4.0f;  ///< Offset distance along the cast angle (pixels)
    float SoftShadowSoftness = 3.0f;  ///< Feather/blur radius of the shadow disc (pixels)
    float SoftShadowOpacity = .8f;    ///< Peak shadow opacity multiplier 0-1
    float SoftShadowAngle = 45.0f;    ///< Shadow cast direction in degrees (0=right, 90=down)
    int SoftShadowSamples = 12;       ///< Feather sample count 4-24 (higher = smoother)
};
ShadowOutlineSettings& ShadowOutline();

/**
 * @struct GlowSettings
 * @brief Settings for text glow and color-divide effects.
 */
struct GlowSettings
{
    bool Enabled = false;   ///< Enable glow effect
    float Radius = 4.0f;    ///< Glow spread in pixels
    float Intensity = .5f;  ///< Glow brightness 0-1
    int Samples = 8;        ///< Quality samples 1-64 (recommended: 8-16)
    /// Color-divide blend, INI key `GlowDivideStrength`, clamped to 0 - 1.
    /// 0 disables the divide pass; any value above 0 enables it, weighted per
    /// pixel by `smoothstep(0.10, 0.25, luma)`, so dark outlines and shadows
    /// still composite normally.  The pass is independent of `EnableGlow`, and
    /// it degrades to a no-op when TextPostProcess is not initialized.
    float DivideStrength = 0;
};
GlowSettings& Glow();

/**
 * @struct ShineSettings
 * @brief Settings for the static top-edge shine overlay.
 */
struct ShineSettings
{
    bool Enabled = false;    ///< Enable shine overlay on text
    float Intensity = .35f;  ///< Peak brightness at top edge 0-1
    float Falloff = 2.0f;    ///< Vertical falloff exponent (higher = sharper)
    float TextGlowAlpha =
        .0f;  ///< Translucent text body alpha reduction 0-1 (0=opaque, 1=fully translucent)
};
ShineSettings& Shine();

/**
 * @struct TypewriterSettings
 * @brief Settings for the typewriter reveal effect.
 */
struct TypewriterSettings
{
    bool Enabled = false;  ///< Enable typewriter reveal
    float Speed = 30.0f;   ///< Characters per second
    float Delay = .0f;     ///< Delay before reveal starts
};
TypewriterSettings& Typewriter();

/**
 * @struct OrnamentSettings
 * @brief Settings for side ornaments.
 */
struct OrnamentSettings
{
    bool Enabled = true;     ///< Enable side ornaments
    float Scale = 1.0f;      ///< Size multiplier
    float Spacing = 3.0f;    ///< Distance from text edges
    std::string FontPath;    ///< Path to ornament font (TTF/OTF)
    float FontSize = 64.0f;  ///< Ornament font size in pixels (ImGui size_pixels)
    bool AnchorToMainLine =
        true;  ///< Anchor ornaments to main text line instead of nameplate center
    /// Manual vertical nudge in pixels (negative = up),
    /// scaled with text size, applied after anchoring
    float OffsetY = .0f;
};
OrnamentSettings& Ornament();

/**
 * @enum ParticleStyle
 * @brief Visual styles for particle aura effects.
 * @ingroup Settings
 *
 * @note The ordinal is significant: it indexes the per-type sprite set in
 * `ParticleTextures`. The single source of truth linking each style to its
 * INI token is `kParticleStyleTokens` below - every dependent table
 * static_asserts against `kParticleStyleCount`.
 *
 * @see ParticleSettings, TextEffects::DrawParticleAura
 */
enum class ParticleStyle
{
    Firefly,        ///< Slow wandering glow that flickers in place
    Snow,           ///< Snowflakes drifting around a broad slow orbit
    Smoke,          ///< Rising, expanding, fading puffs
    Spark,          ///< Orbiting embers that periodically flare outward hot
    Wisp,           ///< Serpentine trails
    Leaf,           ///< Orbiting leaves with in/out drift and fluttering tumble
    Aurora,         ///< Wavy horizontal shimmer band
    CherryBlossom,  ///< Orbiting petals with a gentle bob and slow spin
    Dust,           ///< Very slow floating motes
    Mote,           ///< Soft glowing motes that gently drift and pulse
    // --- Extra sprite styles. Append new styles here so existing ordinals stay stable. --
    Arcane,         ///< Orbiting rune, slow spin, glow pulse
    Ash,            ///< Slow-falling embers with a fading glow pulse
    Bat,            ///< Bats circling the plate with swooping flight (flap strip)
    Bubble,         ///< Rising wobbling bubbles that pop at the top
    Butterfly,      ///< Slow fluttering orbit with figure-eight bob (flap strip)
    Coin,           ///< Slowly orbiting coins spinning on their axis (spin strip)
    Confetti,       ///< Tumbling festive scraps drifting down
    Constellation,  ///< Near-static twinkling star clusters, high band
    Curse,          ///< Skittering dark sigils on a slow low orbit
    Enchant,        ///< Orbiting arcane sparkles with a shimmer weave
    Fairy,          ///< Bright darting wanderer, firefly-class but livelier
    Fog,            ///< Slow horizontal haze bank along the lower edge
    Gem,            ///< Upright orbiting jewels with a facet glint
    Glitter,        ///< Anchored sparkle field, twinkling in place
    Heart,          ///< Rising hearts with a heartbeat pulse
    Hex,            ///< Slow orbiting hex marks with green flame flicker
    Ink,            ///< Matte ink marks creeping around a low slow orbit
    Moon,           ///< A single slow crescent arcing across the top band
    Planet,         ///< A single ringed planet on a slow deep orbit
    Pollen,         ///< Air-borne golden specks on a very slow sway-fall
    Soul,           ///< Ghostly wisps rising and fading out
    Steam,          ///< Brisk narrow rising vapor that expands
    Void,           ///< Dark swirls orbiting with an inward breathing pull
    Vortex,         ///< Coherent spinning spirals on a brisk shared orbit
    Wind,           ///< Fast horizontal gust streaks across the plate
    Zap,            ///< Electric arcs jumping between hashed positions
    Zzz,            ///< Sleepy Z glyphs drifting up and to the side
    Ember,          ///< Rising fire embers that flicker as they cool
    Pixiedust,      ///< Pastel sparkle motes sprinkling down, twinkling
    Runes,          ///< Floating glyphs that morph between rune shapes
    Sand            ///< Wind-blown grains gusting across the lower band
};

/**
 * @struct ParticleStyleToken
 * @brief Canonical mapping from a particle style to its
 * configuration token.
 *
 * The token is the case-insensitive `ParticleTypes` INI name and the
 * lookup key in the
 * `particles` map in `glyph.project.json`. The map resolves the token to one
 * or more
 * obfuscated PNG paths under `Data/SKSE/Plugins/glyph/particles/`. Sprite files use
 * GUID
 * names, so a file named after the token does not load. Image dimensions determine
 * flipbook
 * frame counts. There is no `_strip` suffix convention.
 *
 * The order must match
 * `ParticleStyle`. Dependent tables in `ParticleTextures`,
 * `RendererEffects`, and the motion
 * specification check their size against
 * `kParticleStyleCount`.
 */
struct ParticleStyleToken
{
    ParticleStyle style;
    const char* token;
};
inline constexpr ParticleStyleToken kParticleStyleTokens[] = {
    {ParticleStyle::Firefly, "firefly"},
    {ParticleStyle::Snow, "snow"},
    {ParticleStyle::Smoke, "smoke"},
    {ParticleStyle::Spark, "spark"},
    {ParticleStyle::Wisp, "wisp"},
    {ParticleStyle::Leaf, "leaf"},
    {ParticleStyle::Aurora, "aurora"},
    {ParticleStyle::CherryBlossom, "cherryblossom"},
    {ParticleStyle::Dust, "dust"},
    {ParticleStyle::Mote, "mote"},
    {ParticleStyle::Arcane, "arcane"},
    {ParticleStyle::Ash, "ash"},
    {ParticleStyle::Bat, "bat"},
    {ParticleStyle::Bubble, "bubble"},
    {ParticleStyle::Butterfly, "butterfly"},
    {ParticleStyle::Coin, "coin"},
    {ParticleStyle::Confetti, "confetti"},
    {ParticleStyle::Constellation, "constellation"},
    {ParticleStyle::Curse, "curse"},
    {ParticleStyle::Enchant, "enchant"},
    {ParticleStyle::Fairy, "fairy"},
    {ParticleStyle::Fog, "fog"},
    {ParticleStyle::Gem, "gem"},
    {ParticleStyle::Glitter, "glitter"},
    {ParticleStyle::Heart, "heart"},
    {ParticleStyle::Hex, "hex"},
    {ParticleStyle::Ink, "ink"},
    {ParticleStyle::Moon, "moon"},
    {ParticleStyle::Planet, "planet"},
    {ParticleStyle::Pollen, "pollen"},
    {ParticleStyle::Soul, "soul"},
    {ParticleStyle::Steam, "steam"},
    {ParticleStyle::Void, "void"},
    {ParticleStyle::Vortex, "vortex"},
    {ParticleStyle::Wind, "wind"},
    {ParticleStyle::Zap, "zap"},
    {ParticleStyle::Zzz, "zzz"},
    {ParticleStyle::Ember, "ember"},
    {ParticleStyle::Pixiedust, "pixiedust"},
    {ParticleStyle::Runes, "runes"},
    {ParticleStyle::Sand, "sand"},
};
inline constexpr int kParticleStyleCount =
    static_cast<int>(sizeof(kParticleStyleTokens) / sizeof(kParticleStyleTokens[0]));
static_assert(
    []() consteval
    {
        for (int i = 0; i < kParticleStyleCount; ++i)
        {
            if (static_cast<int>(kParticleStyleTokens[i].style) != i)
            {
                return false;
            }
        }
        return true;
    }(),
    "kParticleStyleTokens rows must be listed in ParticleStyle enum order");

/**
 * @struct ParticleSettings
 * @brief Settings for the particle aura.
 *
 *
 * `TierDefinition::particleTypes` selects the styles for each tier. `Enabled` is the master
 *
 * switch. A tier with an empty value or `ParticleTypes = None` renders no particles.
 *
 * Tier
 * particle auras apply only to the player, as the ornament gate does. An NPC plate shows
 *
 * particles only through a matched `SpecialTitleDefinition` with `ForceParticles = 1`.
 * Other
 * combinations of `ParticleTypes`, `ParticleCount`, and `EnableParticleAura` do not
 * enable a
 * tier aura for an NPC.
 *
 * When `EnableLOD` is true, particles stop after
 * `Visual().LODMidDistance` plus
 * `LODTransitionRange`.
 */
struct ParticleSettings
{
    bool Enabled = true;              ///< Master enable for particle aura
    bool UseParticleTextures = true;  ///< Use texture sprites instead of shapes
    int Count = 8;                    ///< Particles per type
    float Size = 3.5f;                ///< Particle size in pixels
    float Speed = 1.0f;               ///< Animation speed multiplier
    float Spread = 20.0f;             ///< How far particles spread from text
    float Alpha = .8f;                ///< Maximum particle opacity
    int BlendMode = 0;                ///< 0=Additive, 1=Screen, 2=Alpha
    float DepthStrength = .7f;        ///< Scales the 3D depth read (size/alpha/parallax)
    float ColorWarmth = .5f;          ///< Warm/cool depth temperature mix [0,1]
    float GlowStrength = .28f;        ///< Additive backlight halo alpha (0 disables glow pass)
    float GlowSize = 2.2f;            ///< Halo radius as a multiple of the crisp sprite size
    float ShineThreshold = .84f;      ///< Sine threshold for the rare specular glint
};
ParticleSettings& Particle();

/**
 * @struct DisplaySettings
 * @brief Settings for display placement and actor limits.
 */
struct DisplaySettings
{
    float VerticalOffset = 8.0f;      ///< Height above actor's head in game units
    float HorizontalOffset = -10.0f;  ///< Screen-space X correction at full scale
    bool HidePlayer = false;          ///< Hide player's own nameplate
    bool HideCreatures = false;       ///< Hide nameplates for non-NPC actors
    int MaxPlates = RenderConstants::DEFAULT_MAX_PLATES;  ///< Visible nameplate limit
    /// Upper bound on the actor handles the cheap distance pass inspects per
    /// snapshot.  `ClampAndValidate()` raises it to at least MaxPlates, so the
    /// scan can always fill every plate slot.
    int MaxScanActors = RenderConstants::DEFAULT_MAX_SCAN_ACTORS;
    int ReloadKey = 0;                ///< Hot-reload virtual key (0 or negative = disabled)
    bool EnableDebugOverlay = false;  ///< Show performance/cache overlay
};
DisplaySettings& Display();

/**
 * @struct DeckSettings
 * @brief Settings for collectible character-card capture.
 */
struct DeckSettings
{
    bool Enabled = true;  ///< Enable one-key character card capture
    /// Win32 virtual key code that captures a card; 119 is F8.  0 or a negative
    /// value disables capture, as `ReloadKey` does.
    int Key = 119;
    std::string OutputFolder =
        "Data/SKSE/Plugins/glyph/cards";  ///< Folder where generated PNG cards are written
    int CardWidth = 750;                  ///< Output card width in pixels
    int CardHeight = 1050;                ///< Output card height in pixels
    /// Fallback pick radius in pixels around screen center.  It is used only
    /// when the game's own crosshair pick returns no actor, which happens for
    /// distant targets and some third-person camera angles.
    int TargetRadius = 220;
    bool PlayerFallback = true;  ///< Capture the player when no actor is targeted
    bool RarityRolls = true;     ///< Let unique actors roll collectible rarity upgrades
};
DeckSettings& Deck();

/**
 * @struct AnimColorSettings
 * @brief Settings for animation timing and color intensity.
 */
struct AnimColorSettings
{
    float AlphaSettleTime = .46f;     ///< Alpha settle time in seconds
    float ScaleSettleTime = .46f;     ///< Font scale settle time in seconds
    float PositionSettleTime = .38f;  ///< Position settle time for NPCs in seconds
    float InnerTextAlpha = 1.0f;      ///< Text body alpha multiplier 0-1 (outlines unaffected)
    float OutlineAlpha = 1.0f;        ///< Outline and shadow alpha multiplier 0-1
};
AnimColorSettings& AnimColor();

/**
 * @struct FontSettings
 * @brief Settings for font paths and sizes.
 */
struct FontSettings
{
    std::string NameFontPath;     ///< Path to name font TTF file
    float NameFontSize = 122.0f;  ///< Name font size in pixels (ImGui size_pixels)
    std::string LevelFontPath;    ///< Path to level font TTF file
    float LevelFontSize = 61.0f;  ///< Level font size in pixels (ImGui size_pixels)
    std::string TitleFontPath;    ///< Path to title font TTF file
    float TitleFontSize = 42.0f;  ///< Title font size in pixels (ImGui size_pixels)
};
FontSettings& Font();

/**
 * @struct TransitionSettings
 * @brief Settings for entrance and exit transitions.
 */
struct TransitionSettings
{
    bool EnableEntrance = false;    ///< Enable pop-in/slide entrance animation
    int EntranceStyle = 0;          ///< 0=PopIn, 1=SlideDown, 2=Expand
    float EntranceDuration = .35f;  ///< Entrance animation duration in seconds
    bool EnableExit = false;        ///< Enable exit animation
    float ExitDuration = .20f;      ///< Exit animation duration in seconds

    // Staggered entrance cascade.  When several plates begin their entrance on
    // the same frame (overlay wake after combat, menus or loads, hot reload, a
    // group entering range), each successive plate waits one step longer,
    // nearest first, so they arrive as a near-to-far wave instead of together.
    // Step 0 disables staggering.
    float EntranceStaggerStep = .06f;  ///< Delay between successive entrances (s)
    float EntranceStaggerMax = .8f;    ///< Ceiling for any single plate's delay (s)
};
TransitionSettings& Transition();

/**
 * @struct VisualSettings
 * @brief Settings for optional visual adjustments.
 *
 * The struct
 * keeps these values out of mutable global variables.
 */
struct VisualSettings
{
    // Distance-Based Outline
    bool EnableDistanceOutlineScale = false;  ///< Scale outline width by distance
    float OutlineDistanceMin = .8f;           ///< Outline multiplier at close range
    float OutlineDistanceMax = 1.5f;          ///< Outline multiplier at far range
    // Minimum Readable Size
    float MinimumPixelHeight = .0f;  ///< Min pixel height for name text, 0=disabled
    // LOD by Distance
    bool EnableLOD = false;             ///< Enable distance-based content LOD
    float LODFarDistance = 1800.0f;     ///< Beyond this: name+level only
    float LODMidDistance = 800.0f;      ///< Beyond this: no particles/ornaments
    float LODTransitionRange = 200.0f;  ///< Smooth transition width in game units
    // Visual Hierarchy
    float TitleAlphaMultiplier = .80f;  ///< Alpha multiplier for title text
    float LevelAlphaMultiplier = .85f;  ///< Alpha multiplier for level text
    // Overlap Prevention
    bool EnableOverlapPrevention = false;  ///< Push overlapping labels apart
    float OverlapPaddingY = 4.0f;          ///< Vertical padding between labels
    int OverlapIterations = 3;             ///< Relaxation passes for overlap resolution
    // Position Smoothing Tuning
    float PositionSmoothingBlend = 1.0f;   ///< 1.0=moving-avg, 0.0=exponential
    float LargeMovementThreshold = 50.0f;  ///< Pixel threshold for large movement handling
    float LargeMovementBlend = .5f;        ///< Blend factor for large movements
    // Motion Trail
    bool EnableMotionTrail = false;  ///< Enable afterimage trail on moving nameplates
    int TrailLength = 4;             ///< Number of ghost copies (1-8)
    float TrailAlpha = .3f;          ///< Peak ghost opacity
    float TrailFalloff = 2.0f;       ///< Alpha falloff exponent (higher = faster fade)
    float TrailMinDistance = 2.0f;   ///< Min pixels moved before trail renders
    int TrailMinTier = 0;            ///< Minimum tier index for trail

    // Wave Displacement
    bool EnableWave = false;     ///< Enable per-glyph sine wave displacement
    float WaveAmplitude = 1.5f;  ///< Wave height in pixels
    float WaveFrequency = 3.0f;  ///< Cycles across text width
    float WaveSpeed = 1.0f;      ///< Animation speed multiplier
    int WaveMinTier = 0;         ///< Minimum tier index for wave effect

    // Tier Effect Gating
    bool EnableTierEffectGating = false;  ///< Gate effects by tier index
    int GlowMinTier = 5;                  ///< Minimum tier for glow effects
    int ParticleMinTier = 10;             ///< Minimum tier for particle effects
    int OrnamentMinTier = 10;             ///< Minimum tier for ornament display
};
VisualSettings& Visual();

/**
 * @struct LabelSettings
 * @brief Label strings and thresholds for the contextual nameplate tokens.
 * @ingroup Settings
 *
 * Covers `%r` (relationship), `%d` (level delta) and `%c` (creature type).
 *
 * Empty strings render as nothing - pair with a trailing `?` segment marker in
 * `Format` / `InfoFormat` to drop the segment entirely when its tokens expand
 * to whitespace.
 *
 * @see Segment::dropIfBlank, RelationshipKind, LevelDelta, CreatureKind
 */
struct LabelSettings
{
    // Relationship labels (`%r`)
    std::string RelationshipFollower = "Follower";  ///< Player teammate
    std::string RelationshipAlly = "Ally";          ///< Friendly NPC that can talk
    std::string RelationshipNeutral;                ///< Default: empty
    std::string RelationshipHostile = "Hostile";    ///< Actively hostile to player

    // Level delta labels (`%d`, actor level vs. player level)
    std::string LevelDeltaWeak = "Weak";      ///< Far below player
    std::string LevelDeltaEven;               ///< Default: empty (similar level)
    std::string LevelDeltaStrong = "Strong";  ///< Notably above player
    std::string LevelDeltaDeadly = "Deadly";  ///< Far above player

    // Creature type labels (`%c`)
    std::string CreatureTypeNPC;                ///< Default: empty
    std::string CreatureTypeBeast = "Beast";    ///< ActorTypeCreature / ActorTypeAnimal
    std::string CreatureTypeUndead = "Undead";  ///< ActorTypeUndead
    std::string CreatureTypeDaedra = "Daedra";  ///< ActorTypeDaedra
    std::string CreatureTypeDragon = "Dragon";  ///< ActorTypeDragon

    // Level delta classification thresholds (actor level minus player level)
    int WeakAtOrBelow = -5;    ///< delta <= this -> Weak
    int StrongAtOrAbove = 5;   ///< delta >= this -> Strong
    int DeadlyAtOrAbove = 10;  ///< delta >= this -> Deadly (overrides Strong)
};
LabelSettings& Labels();

/**
 * @struct IconSettings
 * @brief Status icon badge settings.
 * @ingroup Settings
 *
 * Draws the contextual facts (relationship, level delta, creature kind) as a
 * strip of icons below the name/level row, in place of the text info row.
 * Icons are duotone SVGs (Font Awesome naming) rasterized at load time from
 * `IconFolder`; each `Icon*` value is a file name without the `.svg`
 * extension.  An empty name hides that badge.
 *
 * Colors are comma-separated RGB floats in [0,1]; the `*Color` members are
 * derived from the string forms in `ClampAndValidate()`.
 *
 * A member's INI key is not its member name.  Most badge members take an `Icon`
 * prefix and drop the `Icon` / `ColorStr` suffix: `Folder` is `IconFolder`,
 * `FollowerIcon` is `IconFollower`, `FollowerColorStr` is `IconFollowerColor`,
 * `MutedAlpha` is `IconMutedAlpha`, `Opacity` is `IconOpacity`.  The tier-emblem
 * and player-lighting members break that pattern and take no prefix at all:
 * `TierBadgeImages`, `TierBadgeScale` and `PlayerStripBedAlpha` are spelled
 * exactly as named here, and a `*ColorStr` member among them only drops the
 * `Str`, so `EmblemBacklightColorStr` is `EmblemBacklightColor`.  `kSettings` in
 * `Settings.cpp` is the authority for every mapping.
 *
 * @note The bundled duotone set is Font Awesome Pro (licensed per-seat) and
 *       is excluded from the public repository.  An empty `IconFolder`
 *       disables badges entirely.
 */
struct IconSettings
{
    std::string Folder = "Data/SKSE/Plugins/glyph/duotone";  ///< SVG folder; empty disables
    bool Enabled = true;      ///< Master toggle (effective only with a folder)
    float Scale = 1.0f;       ///< Badge size relative to the level font size
    bool DeadlyPulse = true;  ///< Subtle alpha pulse on the Deadly skull

    // Icon names (SVG file names without extension - the INI surface)
    std::string FollowerIcon = "shield-halved";
    std::string AllyIcon = "handshake";
    std::string HostileIcon = "skull-crossbones";
    std::string WeakIcon = "caret-down";
    std::string StrongIcon = "caret-up";
    std::string DeadlyIcon = "skull";
    std::string BeastIcon = "paw";
    std::string UndeadIcon = "ghost";
    std::string DaedraIcon = "fire";
    std::string DragonIcon = "dragon";

    // Badge colors (comma-separated RGB strings - the INI surface)
    std::string FollowerColorStr = "0.46, 0.68, 0.84";
    std::string AllyColorStr = "0.52, 0.74, 0.50";
    std::string HostileColorStr = "0.86, 0.36, 0.32";
    std::string WeakColorStr = "0.54, 0.66, 0.80";
    std::string StrongColorStr = "0.86, 0.62, 0.32";
    std::string DeadlyColorStr = "0.90, 0.28, 0.24";
    std::string CreatureColorStr = "0.80, 0.74, 0.62";

    // Derived values (computed in ClampAndValidate, not INI keys)
    Color3 FollowerColor;
    Color3 AllyColor;
    Color3 HostileColor;
    Color3 WeakColor;
    Color3 StrongColor;
    Color3 DeadlyColor;
    Color3 CreatureColor;

    // Always-on slots: further NPC and player indicators drawn alongside the
    // relationship, creature and threat badges.  A neutral or inactive state
    // draws muted (MutedColor, dimmed by MutedAlpha and desaturated by
    // MutedDesat at draw time); an active state draws lit in its own color.
    // Set any `*Icon` empty to hide that state; toggle a whole slot via its
    // `*Enabled`.  Icon names require matching SVGs in the IconFolder.
    std::string NeutralIcon = "circle";  ///< muted relationship
    std::string HumanoidIcon = "user";   ///< muted creature
    std::string EvenIcon = "equals";     ///< muted threat
    std::string GuardIcon = "helmet-battle";
    std::string MerchantIcon = "coins";
    std::string CommonerIcon = "house";  ///< muted role
    std::string EssentialIcon = "certificate";
    std::string ProtectedIcon = "shield-check";
    std::string MortalIcon = "heart";  ///< muted protection
    std::string CombatIcon = "swords";
    std::string AlertIcon = "eye";
    std::string IdleIcon = "moon";  ///< muted engagement
    // Player slot icons.
    std::string SneakHiddenIcon = "eye-slash";
    std::string SneakDetectedIcon = "eye";
    std::string SneakOffIcon = "person-walking";  ///< muted
    std::string EncumberedIcon = "weight-hanging";
    std::string NormalWeightIcon = "feather";  ///< muted
    std::string WantedIcon = "gavel";
    std::string BountyClearIcon = "scale-balanced";  ///< muted
    // Actor rank badge (low / mid / high thirds of the ladder).
    std::string TierLowIcon = "medal";
    std::string TierMidIcon = "gem";
    std::string TierHighIcon = "crown";
    // Full-color emblem images for the tier badge.  When enabled and at least
    // one emblem loads, they take the place of the medal/gem/crown icons above
    // with a top-weighted rank climb.  Emblems come from glyph.project.json's
    // tierBadges list, in rank order; the files are GUID-named.  See
    // BadgeTextures::InitializeTierImages.
    bool TierBadgeImages = true;
    /// Unused emblem folder.  Nothing reads this value; the emblem paths come
    /// from the project manifest.  Changing it has no effect.
    std::string TierBadgeFolder = "Data/SKSE/Plugins/glyph/badges";
    float TierBadgeGamma = 1.8f;  ///< Emblem->tier top-weighting (>1 = high emblems rarer)
    float TierBadgeScale = 1.7f;  ///< Emblem size as a multiple of the status-icon size

    // --- Player-only lighting layers ---
    // Strip light bed: additive discs behind the player's icon strip, with a
    // slow alpha breathe.
    bool PlayerStripBedEnabled = true;
    float PlayerStripBedAlpha = 0.10f;          ///< Per-disc additive alpha (overlap sums)
    float PlayerStripBedSize = 2.6f;            ///< Disc EDGE as a multiple of row height
    float PlayerStripBedBreatheHz = 0.14f;      ///< Bed breathe frequency (alpha only)
    std::string PlayerStripBedColorStr;         ///< Empty => derive near-neutral from Name
    std::optional<Color3> PlayerStripBedColor;  ///< Empty => derive at draw time
    // Emblem backlight: a single radial disc behind the tier emblem.
    bool EmblemBacklightEnabled = true;
    float EmblemBacklightSize = 2.6f;            ///< Disc EDGE as a multiple of emblem edge
    float EmblemBacklightAlpha = 0.55f;          ///< Peak backlight alpha (x breathe)
    float EmblemBacklightBreatheHz = 0.167f;     ///< Backlight breathe freq (=> sin(t*1.05))
    float EmblemCrispAlpha = 0.95f;              ///< Crisp emblem alpha on all draw paths
    std::string EmblemBacklightColorStr;         ///< Empty => derive near-neutral tier accent
    std::optional<Color3> EmblemBacklightColor;  ///< Empty => derive at draw time

    // --- Optional emboss layers (enabled by default, low values) ---
    // Player resting-icon emboss: warm top rim plus carved bottom shadow.
    bool PlayerRimLightEnabled = true;
    float PlayerRimAlpha = 0.22f;    ///< Top-rim highlight alpha (x muted alpha)
    float PlayerCarveAlpha = 0.26f;  ///< Bottom carve-shadow alpha (x muted alpha)
    float PlayerRimOffset = 1.0f;    ///< Emboss offset in px (scaled by text size)
    std::string PlayerRimColorStr;   ///< Empty => derive warm-white from Name
    std::optional<Color3> PlayerRimColor;
    // Emblem directional lights: warm key above and cool fill below the backlight.
    bool EmblemKeyFillEnabled = true;
    float EmblemKeyAlpha = 0.35f;   ///< Key (top) light alpha (x breathe)
    float EmblemFillAlpha = 0.15f;  ///< Fill (bottom) bounce alpha (x breathe)
    float EmblemKeyRise = 0.18f;    ///< Key offset above center (fraction of emblem edge)
    float EmblemFillDrop = 0.15f;   ///< Fill offset below center (fraction of emblem edge)
    std::string EmblemKeyColorStr;  ///< Empty => derive warm from Name
    std::optional<Color3> EmblemKeyColor;
    std::string EmblemFillColorStr;  ///< Empty => derive cool from Name
    std::optional<Color3> EmblemFillColor;

    // Lit colors for the active states.
    std::string GuardColorStr = "0.60, 0.68, 0.84";
    std::string MerchantColorStr = "0.84, 0.74, 0.42";
    std::string EssentialColorStr = "0.86, 0.78, 0.46";
    std::string ProtectedColorStr = "0.54, 0.72, 0.86";
    std::string CombatColorStr = "0.88, 0.42, 0.30";
    std::string AlertColorStr = "0.86, 0.76, 0.40";
    std::string SneakHiddenColorStr = "0.50, 0.64, 0.84";
    std::string SneakDetectedColorStr = "0.86, 0.36, 0.32";
    std::string EncumberedColorStr = "0.82, 0.64, 0.40";
    std::string WantedColorStr = "0.84, 0.34, 0.30";
    // Tier-band colors: bronze -> silver-blue -> gold, at the same low
    // saturation as the other lit colors.
    std::string TierLowColorStr = "0.70, 0.62, 0.52";
    std::string TierMidColorStr = "0.62, 0.70, 0.80";
    std::string TierHighColorStr = "0.86, 0.74, 0.46";
    // Resting-state colors: each muted slot carries its own hue, so the
    // always-on strip is a colored spectrum rather than uniform grey.
    std::string NeutralColorStr = "0.56, 0.62, 0.70";       ///< neutral relationship
    std::string HumanoidColorStr = "0.74, 0.68, 0.58";      ///< humanoid creature
    std::string CommonerColorStr = "0.60, 0.68, 0.54";      ///< commoner role
    std::string MortalColorStr = "0.76, 0.58, 0.60";        ///< mortal protection
    std::string EvenColorStr = "0.60, 0.70, 0.72";          ///< even threat
    std::string IdleColorStr = "0.56, 0.60, 0.76";          ///< idle engagement
    std::string SneakOffColorStr = "0.64, 0.68, 0.60";      ///< sneak off (player)
    std::string NormalWeightColorStr = "0.64, 0.76, 0.70";  ///< normal weight (player)
    std::string BountyClearColorStr = "0.50, 0.70, 0.68";   ///< bounty clear (player)
    std::string MutedColorStr = "0.62, 0.64, 0.68";         ///< shared tint, unused

    Color3 GuardColor;
    Color3 MerchantColor;
    Color3 EssentialColor;
    Color3 ProtectedColor;
    Color3 CombatColor;
    Color3 AlertColor;
    Color3 SneakHiddenColor;
    Color3 SneakDetectedColor;
    Color3 EncumberedColor;
    Color3 WantedColor;
    Color3 TierLowColor;
    Color3 TierMidColor;
    Color3 TierHighColor;
    Color3 NeutralColor;
    Color3 HumanoidColor;
    Color3 CommonerColor;
    Color3 MortalColor;
    Color3 EvenColor;
    Color3 IdleColor;
    Color3 SneakOffColor;
    Color3 NormalWeightColor;
    Color3 BountyClearColor;
    Color3 MutedColor;

    // Per-slot enables (a disabled active state drops that actor's badge).
    bool RelationshipEnabled = true;
    bool CreatureEnabled = true;
    bool ThreatEnabled = true;
    bool RoleEnabled = true;
    bool ProtectionEnabled = true;
    bool EngagementEnabled = true;  ///< master for the NPC engagement slot
    bool CombatStateEnabled = true;
    bool AlertStateEnabled = true;
    bool SneakEnabled = true;
    bool PlayerCombatEnabled = true;
    bool EncumberedEnabled = true;
    bool BountyEnabled = true;
    bool TierEnabled = true;  ///< rank badge for player and NPCs

    // Status-row opacity multiplier. The draw path clamps the result to 1.0.
    // Resting slots then use MutedAlpha as an optional multiplier.
    float Opacity = 0.92f;

    // Resting styling: alpha multiplier and desaturation strength [0,1].
    // A lower desat keeps more of each resting slot's own hue.
    float MutedAlpha = 1.0f;
    float MutedDesat = 0.18f;
};
IconSettings& Icons();

/**
 * @struct NpcColorSettings
 * @brief NPC nameplate support-layer colors.
 * @ingroup Settings
 *
 * NPC name fill stays white; title and level fill reuse the matched tier's
 * player-level gradient.  These colors tint only the outline, shadow and glow
 * support layers: hostiles warm, teammates cool, everyone else - including
 * civilians that can be talked to - neutral.
 *
 * Colors are comma-separated RGB floats in [0,1]; the `*Color` members are
 * derived from the string forms in `ClampAndValidate()`.
 */
struct NpcColorSettings
{
    // Colors (comma-separated RGB strings - the INI surface)
    std::string NeutralColorStr = "1.0, 1.0, 1.0";     ///< Neutral name support
    std::string HostileColorStr = "1.0, 0.72, 0.68";   ///< Hostile name support
    std::string FollowerColorStr = "0.72, 0.84, 1.0";  ///< Follower name support
    std::string LevelColorStr = "0.82, 0.84, 0.88";    ///< Level support tint
    std::string TitleColorStr = "0.92, 0.93, 0.95";    ///< Title support tint

    // Derived values (computed in ClampAndValidate, not INI keys)
    Color3 NeutralColor;
    Color3 HostileColor;
    Color3 FollowerColor;
    Color3 LevelColor;
    Color3 TitleColor;
};
NpcColorSettings& NpcColors();

/**
 * @struct FocusSettings
 * @brief Focus-target expanded-nameplate settings.
 * @ingroup Settings
 *
 * When `Enabled`, the render thread picks at most one actor per frame: the one
 * whose direction from the camera lies inside a cone around camera-forward
 * with the smallest angular offset.  That actor draws the full title + main +
 * info row stack; every other actor draws only the main line, at a reduced
 * alpha (`AmbientDimFactor`).
 *
 * The player is never picked as the focus target and is never dimmed.  Dead
 * actors and card-capture-only entries are also skipped when picking.
 *
 * An actor the camera ray already resolves to - the Graffito raycast target,
 * or the game's crosshair target when Graffito is off - counts as focused for
 * that frame and jumps straight to full content, without the crossfade.
 *
 * Transitions between ambient and focused are smoothed by a per-actor value
 * driven by `Renderer::ExpApproachAlpha` with `SettleTime`.
 *
 * @see Renderer::SelectFocusedActor, Renderer::ActorCache::focusSmooth
 */
struct FocusSettings
{
    bool Enabled = false;           ///< Master toggle (off = no focus pick, no dimming)
    float ConeAngleDegrees = 8.0f;  ///< Cone half-angle for selection [0.5, 45]
    /// Max world distance for focus selection; 0 = no additional limit.
    /// Candidates are already bounded by `Distance().MaxScanDistance`, so a
    /// non-zero value only shrinks the cone further.
    float MaxDistance = .0f;
    float AmbientDimFactor = .55f;  ///< Alpha multiplier for non-focused actors [0.05, 1]
    float SettleTime = .25f;        ///< Seconds for focusSmooth crossfade [0, 2]
    bool IgnoreOccluded = true;     ///< Skip occluded actors when picking focus
};
FocusSettings& Focus();

// clang-format off
/**
 * @struct GraffitoSettings
 * @brief Perspective-correct text on an actor-bound world plane.
 * @ingroup Settings
 *
 * NPC plates use actor-aligned geometry instead of rotating toward the camera.
 * With `FolioEnabled`, the full inscription stays on the front, its flat
 * glyphs are built into shallow relief, and side and rear views resolve into
 * compact triangular facets.  With `FolioEnabled` off, the plate is a single
 * sheet with front, seam and mirrored-bleed states.
 *
 * `Scale` multiplies the renderer's calibrated physical text size.
 * `PlayerScale` scales the whole player plate uniformly after that shared
 * calibration, so every authored layout ratio is preserved.
 * `ForwardOffset` moves the plane ahead of the actor in world units.
 * When `FolioEnabled` is off, and while the folio collapses to a single sheet,
 * `FacingFadeDegrees` is the angular width of the material transition around
 * edge-on; zero selects hard front/seam/back states.  `MaxDistance` limits the
 * effect, with zero reusing the normal nameplate distance limit.
 */
struct GraffitoSettings
{
    bool Enabled = false;                 ///< Master toggle (off = billboard nameplates)
    float Scale = 1.0f;                   ///< Physical text-size multiplier [0.25, 4]
    float PlayerScale = .72f;             ///< Uniform player-only multiplier [0.25, 2]
    float ForwardOffset = 4.0f;           ///< Plane offset along actor-forward [0, 32]
    float FacingFadeDegrees = 15.0f;      ///< Single-sheet edge fade in degrees [0, 45]
    float BacksideBleedAlpha = .12f;      ///< Single-sheet mirrored ink opacity [0, .25]
    float EdgeSeamAlpha = .22f;           ///< Single-sheet edge-seam opacity [0, .4]
    bool FolioEnabled = true;             ///< Relief front plus one side/rear head marker
    float FolioReverseAlpha = .72f;       ///< Head-marker opacity from the rear [0, 1]
    float FolioSpineAlpha = .88f;         ///< Head-marker opacity edge-on [0, 1]
    float FolioDepth = 2.0f;              ///< Relief plane spacing in source pixels [0, 28]
    float WrapDegrees = 55.0f;            ///< Total cylindrical arc across content [0, 140]
    float FisheyeStrength = .62f;         ///< Text-row midpoint magnification [0, 1]
    float LayerDepth = .0f;               ///< Role separation, fraction of view distance [0, .06]
    float EdgeSheen = .14f;               ///< Grazing highlight strength on crowned wings [0, .4]
    float OrientationSettleTime = .18f;   ///< Actor-plane orientation smoothing [0, 2] seconds
    float MaxDistance = 1200.0f;          ///< Effect range; 0 = normal nameplate distance
    bool FallenEpitaphEnabled = true;     ///< Hinge death-rite text onto the ground plane
    float EpitaphGroundLift = 2.0f;       ///< Ground-plane lift against z-fighting [0, 16]
};
GraffitoSettings& Graffito();

/**
 * @struct CompatSettings
 * @brief TrueHUD and moreHUD deconfliction.
 *
 * When TrueHUD floats an info or boss bar over an actor, that actor's plate
 * fades to `TrueHUDYieldAlpha`; when moreHUD's crosshair readout already shows
 * the target's level, the level segment is dropped for that actor.  Both
 * checks are automatic, and stay inactive when the mods are absent.
 *
 * @see HudCompat
 */
// clang-format on
struct CompatSettings
{
    bool YieldToTrueHUD = true;       ///< Yield plates to TrueHUD floating bars
    float TrueHUDYieldAlpha = .0f;    ///< Plate alpha while yielded [0,1]
    bool YieldLevelToMoreHUD = true;  ///< Drop level for moreHUD's crosshair target
    float YieldSettleTime = .3f;      ///< Seconds for the yield crossfade
};
CompatSettings& Compat();

/**
 * @struct DeathRiteSettings
 * @brief One-shot death animation for a plate whose actor dies in view.
 * @ingroup Settings
 *
 * The plate holds position instead of popping out, its color drains toward
 * ash, and the name plays a creature-keyed exit: a mortal's fades and sinks, a
 * draugr's crumbles letter by letter, a dragon's flares bright and then goes
 * dark.  It plays once per corpse, never for an actor first seen already dead,
 * and an animation still pending while the overlay was suppressed (player in
 * combat) is cancelled on wake rather than replayed.
 */
struct DeathRiteSettings
{
    bool Enabled = true;    ///< Master toggle
    float Duration = 1.6f;  ///< Full animation length in seconds
};
DeathRiteSettings& DeathRite();

/**
 * @struct QuietSettings
 * @brief Camera-motion quieting.
 * @ingroup Settings
 *
 * Text is unreadable during a fast camera pan, so the overlay reduces itself:
 * sub-lines (title, info row, level, badges) are hidden entirely and names
 * drop to a low alpha.  When the camera settles the content returns names
 * first, sub-lines after, through an asymmetric envelope - fast attack into
 * quiet, slow release back.
 *
 * Angular speed below `PanThresholdLo` leaves the overlay untouched; above
 * `PanThresholdHi` it is fully quiet; smoothstep in between.
 *
 * @see RendererState::quietName
 */
struct QuietSettings
{
    bool Enabled = true;            ///< Master toggle
    float PanThresholdLo = 40.0f;   ///< deg/s where quieting begins
    float PanThresholdHi = 160.0f;  ///< deg/s where fully quiet
    float AttackTime = .10f;        ///< Settle time receding into quiet (s)
    float NameReleaseTime = .28f;   ///< Name resolve-back settle time (s)
    float SubReleaseTime = .50f;    ///< Sub-line resolve-back settle time (s)
    float NameFloor = .35f;         ///< Name alpha multiplier at full quiet
};
QuietSettings& Quiet();

/**
 * @struct CandlelightSettings
 * @brief Exposure-adaptive text brightness.
 * @ingroup Settings
 *
 * The renderer meters the scene behind each plate: over a bright background
 * the text dims slightly, over a dark one it brightens slightly and picks up
 * some of the scene's warmth.  Every adjustment is capped by `Strength` and
 * smoothed per actor.  Render setups that cannot supply the measurement
 * disable the feature silently: one log line, no visual change.
 */
struct CandlelightSettings
{
    bool Enabled = true;     ///< Master toggle
    float Strength = .08f;   ///< Max brightness adjustment (fraction, [0, 0.15])
    float Warmth = .5f;      ///< Chroma pull toward the scene in dark shots [0,1]
    float SettleTime = .6f;  ///< Per-actor smoothing settle time (s)
};
CandlelightSettings& Candlelight();

/**
 * @struct DepthClipSettings
 * @brief Per-pixel depth occlusion for nameplates.
 * @ingroup Settings
 *
 * Line-of-sight culling can only show or hide a whole plate, so a plate pops
 * at the moment sight breaks.  Plates are additionally depth-tested per pixel
 * against the game's depth buffer, so geometry cuts them at the real
 * intersection, with a soft feather at the edge.  The coarse line-of-sight
 * culling stays on regardless.  Setups where the depth buffer is unavailable
 * (some ENB or upscaler stacks) fall back to line-of-sight-only occlusion
 * automatically, with one log line.
 */
struct DepthClipSettings
{
    bool Enabled = true;   ///< Master toggle
    float Feather = 2.5f;  ///< Feather radius at the occlusion edge (px, [0,8])
};
DepthClipSettings& DepthClipConfig();

/**
 * @brief Shared settings mutex.
 *
 * Every accessor in this namespace returns a reference to a function-local
 * static.  First-call construction is thread-safe, but the referenced object is
 * not: a reader must hold a shared lock on this mutex and a writer a unique
 * lock.  `Load()` rewrites the whole set.  ConsoleCommands.cpp is the only other
 * writer; it sets `DisplaySettings::EnableDebugOverlay` alone.  The render
 * thread does not lock per draw; it copies a `RenderSettingsSnapshot` once per
 * frame when
 * Generation() changes.
 *
 * @return The shared settings mutex.
 */
[[nodiscard]] std::shared_mutex& Mutex();

/**
 * @brief Settings generation counter.
 *
 * Incremented each time `Load()` parses the INI file to the end.  Consumers
 * compare it against a cached value to avoid re-copying settings that have not
 * changed.
 *
 *
 * @return The shared generation counter.
 *
 * @warning The counter is NOT incremented when
 * `glyph.ini` is missing: Load()
 *          applies the defaults, validates them and returns
 * early. The counter starts at 0 and so do the consumers' cached values, so on a missing-INI start
 * every generation-gated consumer keeps its default-constructed copy.
 */
[[nodiscard]] std::atomic<uint32_t>& Generation();

/**
 * @brief Load all settings from `Data/SKSE/Plugins/glyph.ini`.
 *
 * Called once during plugin initialization and again on every hot reload.  No
 * INI content can make it throw: a missing file leaves the defaults in place,
 * and a malformed line or unknown key is counted, logged and skipped.  The one
 * exception path is a malformed `kSettings` row, which is a programming error;
 * see the SettingEntry warning in SettingsBinding.hpp.
 *
 * An unparsable numeric value on a `kSettings` scalar key does NOT fall back to
 * its default.  It becomes 0 and `ClampAndValidate()` then pulls that 0 into the
 * row's valid range, so a typo in such a key reads as the range floor, not as
 * the shipped default.  Only a key that is absent keeps its default.  The four
 * indexed families parse with their own per-field fallbacks and are not covered
 * by this rule.
 *
 * Runs on whichever thread calls it: the SKSE plugin-load thread at startup,
 * the game thread for a queued hot reload, or the render thread when the SKSE
 * task interface is null.  Nothing in the parser dereferences a game object, so
 * all three are safe.
 *
 * @pre The caller does not already hold a lock on Mutex(); Load() takes it
 *      uniquely for its whole body.
 * @post All settings objects reachable through this namespace's accessors are
 *       populated with values from the INI file or their defaults.
 * @post Tiers is indexed by tier number: index N holds `TierN`. A gap in the
 *       numbering is back-filled with a default 'Unknown' tier covering levels
 *       1-250, which shadows every higher tier, so a gap is warned about. The
 *       vector is never empty.
 * @post DisplayFormat and TitleFormat hold the main row and the title line
 *       parsed from the `Format` key, which is matched in any section; the
 *       shipped INI puts it at global scope. InfoFormat holds the segments
 *       parsed from the separate `InfoFormat` key, which defaults to empty
 *       because the status icon badges supersede the text info row.
 * @post Generation() has advanced, unless the file was missing, in which case
 *       it is deliberately left alone.
 *
 * @see Renderer::Draw, TierDefinition, Segment
 */
void Load();
}  // namespace Settings
