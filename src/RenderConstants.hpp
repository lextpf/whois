#pragma once

#include <algorithm>
#include <cstdint>

// clang-format off
/**
 * @namespace RenderConstants
 * @brief Compile-time constants for the rendering pipeline.
 * @author Alex (https://github.com/lextpf)
 * @ingroup RenderConstants
 *
 * Actor processing limits, cache management, and the smoothing parameters used by
 * the render thread.
 *
 * ## :material-ruler: Distance Units
 *
 * Skyrim uses game units where $\approx 70$ units $= 1$ meter:
 *
 * $$d_{meters} = \frac{d_{units}}{70}$$
 *
 * ## :material-account-group-outline: Actor Processing
 *
 * - `MaxPlates`: default 16, range 1-128; maximum visible nameplates.
 * - `MaxScanActors`: default 128, range 1-4096; cheap actor-scan runaway guard.
 *   `ClampActorLimits` then raises it to at least the clamped `MaxPlates`, so the
 *   effective lower bound is `MaxPlates`, not `MIN_SCAN_ACTORS`.
 *
 * ## :material-chart-bell-curve-cumulative: Exponential Settling
 *
 * Alpha, text scale and the occlusion factor settle exponentially. For a settle time
 * $T$ and a frame delta $\Delta t$:
 *
 * $$\alpha = 1 - \epsilon^{\,\Delta t \,/\, T}$$
 *
 * where $\epsilon = 0.01$ (1% residual). Each quantity updates as:
 *
 * $$v_{smooth} = v_{old} + \alpha \cdot (v_{new} - v_{old})$$
 *
 * $T$ comes from `Settings::AnimColor().AlphaSettleTime` and `ScaleSettleTime`,
 * and from `Settings::Occlusion().SettleTime` for the occlusion factor.
 *
 * ## :material-chart-bell-curve-cumulative: Position Smoothing
 *
 * Position is **not** plain exponential decay. It runs in three stages per frame
 * (Renderer.cpp, `UpdateCacheSmoothing`):
 *
 * 1. Exponential term, using the $\alpha$ formula above with
 *    $T$ = `Settings::AnimColor().PositionSettleTime` (0.38 s when the key is
 *    absent; the shipped INI sets 0.28 s). The player's own plate overrides $T$
 *    with a fixed 0.015 s, about one frame at 60 fps:
 *
 * $$p_{exp} = p_{old} + \alpha \cdot (p_{new} - p_{old})$$
 *
 * 2. Moving-average term: the plain mean of the last `POSITION_HISTORY_SIZE` (8)
 *    raw screen positions.
 *
 * 3. Cross-fade of the two terms by `Settings::Visual().PositionSmoothingBlend`:
 *
 * $$p_{blend} = p_{exp} + b \cdot (p_{ma} - p_{exp})$$
 *
 * The shipped default of $b = 1.0$ discards the exponential term entirely, so out
 * of the box a plate follows the 8-frame boxcar average, and the 0.015 s player
 * override has no effect. Lower `PositionSmoothingBlend` in the INI to bring the
 * exponential term back.
 *
 * For large movements ($\|p_{new} - p_{old}\| > threshold$), the head takes only a
 * fraction $\beta$ of the step toward the blended result, which damps hard screen
 * jumps such as a camera cut or a teleport:
 *
 * $$p_{smooth} = p_{old} + \beta \cdot (p_{blend} - p_{old})$$
 *
 * Otherwise $p_{smooth} = p_{blend}$, a full step. The threshold and blend factor
 * are runtime-configurable via `Settings::Visual().LargeMovementThreshold`
 * (default 50px) and `Settings::Visual().LargeMovementBlend` (default 0.5).
 *
 * The whole chain, once per frame and per plate:
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef data fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *     classDef process fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef check fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *
 *     RAW[Raw screen position]:::data --> EXP[Exponential term]:::process
 *     RAW --> MA[Mean of the last 8 raw positions]:::process
 *     RAW --> J{Raw step past LargeMovementThreshold?}:::check
 *     EXP --> MIX[Cross-fade by PositionSmoothingBlend]:::process
 *     MA --> MIX
 *     MIX --> J
 *     J -->|yes| DAMP[Advance by LargeMovementBlend]:::process
 *     J -->|no| FULL[Take the full step]:::process
 *     DAMP --> OUT[Smoothed head position]:::data
 *     FULL --> OUT
 * ```
 */
// clang-format on
namespace RenderConstants
{
// Actor processing defaults and safety bounds. The values themselves are set by the
// MaxPlates and MaxScanActors INI keys; only the defaults and the guard rails are
// compile-time constants.
inline constexpr int DEFAULT_MAX_PLATES = 16;
inline constexpr int DEFAULT_MAX_SCAN_ACTORS = 128;
inline constexpr int MIN_PLATES = 1;
inline constexpr int MAX_PLATES = 128;
inline constexpr int MIN_SCAN_ACTORS = 1;
inline constexpr int MAX_SCAN_ACTORS = 4096;

/**
 * @struct ActorLimits
 * @brief Validated pair of actor-processing limits.
 * @ingroup RenderConstants
 *
 * Only ClampActorLimits produces this type, so both fields are already inside their
 * compile-time bounds and satisfy maxScanActors >= maxPlates.  The default member values
 * are the values used when the INI omits both keys.
 */
struct ActorLimits
{
    int maxPlates = DEFAULT_MAX_PLATES;           ///< Visible nameplate cap, clamped to 1-128.
    int maxScanActors = DEFAULT_MAX_SCAN_ACTORS;  ///< Actor-scan cap, never below maxPlates.
};

/**
 * @brief Clamp user-provided actor limits and ensure the scan can fill every plate slot.
 *
 * @param maxPlates      Requested nameplate cap, clamped to MIN_PLATES..MAX_PLATES.
 * @param maxScanActors  Requested scan cap, clamped to MIN_SCAN_ACTORS..MAX_SCAN_ACTORS, then
 *                       raised to the clamped maxPlates when it is still lower.
 * @return The validated pair, with maxScanActors at least maxPlates.
 */
[[nodiscard]] constexpr ActorLimits ClampActorLimits(int maxPlates, int maxScanActors) noexcept
{
    maxPlates = std::clamp(maxPlates, MIN_PLATES, MAX_PLATES);
    maxScanActors = std::clamp(maxScanActors, MIN_SCAN_ACTORS, MAX_SCAN_ACTORS);
    if (maxScanActors < maxPlates)
    {
        maxScanActors = maxPlates;
    }
    return {maxPlates, maxScanActors};
}

// Cache Management
/**
 * @brief Base retention period for actor cache entries.
 *
 * An entry that is playing its exit
 * animation remains for at most three times this value.
 * This hard limit prevents a leak when
 * exit animation is disabled during the transition.
 */
inline constexpr uint32_t CACHE_GRACE_FRAMES =
    60;  ///< Frames to keep cache entries after actor leaves view (~1s at 60fps)
inline constexpr int POSITION_HISTORY_SIZE =
    8;  ///< Position history buffer size for moving average smoothing

// Debug Overlay
inline constexpr float RELOAD_NOTIFICATION_DURATION =
    2.f;  ///< Duration to show "Reloaded!" notification (seconds)
inline constexpr int FRAME_TIME_SAMPLES = 60;  ///< Number of frame time samples for averaging

// Font Indices
inline constexpr int FONT_INDEX_NAME = 0;      ///< Name font (loaded first)
inline constexpr int FONT_INDEX_LEVEL = 1;     ///< Level font
inline constexpr int FONT_INDEX_TITLE = 2;     ///< Title font
inline constexpr int FONT_INDEX_ORNAMENT = 3;  ///< Ornament/flourish font

// INI Parsing Limits
inline constexpr int MAX_TIER_INDEX =
    100;  ///< Maximum tier index in INI (prevents unbounded allocation)
inline constexpr int MAX_SPECIAL_TITLE_INDEX = 50;  ///< Maximum special title index in INI
inline constexpr int MAX_HONORIFIC_INDEX = 63;      ///< Maximum honorific index in INI
inline constexpr int MAX_REGISTER_INDEX = 31;       ///< Maximum register index in INI

// ----------------------------------------------------------------------------
// Layout and animation tuning. These are compile-time constants, not INI keys,
// because tuning them is rare. Add an INI key for a value that has to change
// without a rebuild.
//
// The gaps and paddings below are pixels at the reference font size.
// RendererLayout multiplies each one by the plate's text-size scale, so a plate
// keeps its proportions as it shrinks with distance.
// ----------------------------------------------------------------------------

inline constexpr float TITLE_MAIN_GAP =
    8.0f;  ///< Vertical gap between title and main line (pixels)
inline constexpr float INFO_LINE_GAP =
    5.0f;  ///< Vertical gap between main line and info row (pixels)
inline constexpr float SEGMENT_PADDING =
    6.0f;  ///< Horizontal padding between main-line segments (pixels)
inline constexpr float BADGE_ICON_FACTOR =
    .45f;  ///< Badge icon edge as a fraction of the level font size; IconScale multiplies it
inline constexpr float BADGE_SPACING =
    10.0f;  ///< Horizontal gap between badge icons inside the strip (pixels)
inline constexpr float BADGE_ROW_GAP =
    6.0f;  ///< Gap above the plate top for the badge strip, and strip to emblem row (px)
inline constexpr float OUTLINE_MIN_SCALE =
    .75f;  ///< Floor for the per-row outline ratio, fontSize / NameFontSize
inline constexpr bool PROPORTIONAL_SPACING =
    true;  ///< Marker only, read nowhere: spacing always scales with text size

// Effect intensity range. Both bands interpolate on the actor's level position
// inside its own tier band (minLevel to maxLevel), not on the tier index: an
// actor at its tier minimum gets MIN, one at the tier maximum gets MAX, and a
// level below 100 scales both bands by a further 0.85 (RendererLayout.cpp). A
// tier whose maxLevel is not above its minLevel pins that position at 0, so every
// actor in it gets MIN.
// EFFECT_ALPHA_* sets the alpha of the packed highlight color the effect layers
// draw with; EFFECT_STRENGTH_* is the amplitude multiplier handed to the animated
// effects that accept one - the gradient effects, Aurora and Enchant ignore it
// (RendererEffects.cpp). The strength band multiplies into the effect's amplitude and
// stacks with per-effect caps and INI intensities, so a low band collapses to a
// net modulation of a few percent and reads as no effect at all. The band must
// stay visible at its minimum: an actor at the bottom of its tier should still
// read at a glance, with level progression adding weight rather than gating
// visibility.
inline constexpr float EFFECT_ALPHA_MIN = .32f;
inline constexpr float EFFECT_ALPHA_MAX = .78f;
inline constexpr float EFFECT_STRENGTH_MIN = .78f;
inline constexpr float EFFECT_STRENGTH_MAX = 1.0f;

// Animation speed bands by tier, in phase cycles per second: the effect phase
// advances as frac(time * speed + per-actor seed), so smaller = slower. High
// tiers are the slowest, and no band drops below the point where the motion stops
// registering at a glance. The band is selected by the normalized tier ratio
// tierIdx / (tierCount - 1), not by absolute tier index, so the bands move with
// the number of [TierN] sections the INI defines. The shipped 20-tier INI maps
// HIGH to tiers 18-19, MID to 16-17 and LOW to 0-15. A sub-level-100 actor
// multiplies the selected band by a further 0.85 (RendererLayout.cpp).
inline constexpr float ANIM_SPEED_LOW_TIER = .24f;   ///< Ratio below .8, and single-tier setups
inline constexpr float ANIM_SPEED_MID_TIER = .17f;   ///< Ratio in [.8, .9)
inline constexpr float ANIM_SPEED_HIGH_TIER = .12f;  ///< Ratio >= .9 (top 10% of tiers)

// Entrance and exit motion offsets, in raw screen pixels: unlike the layout gaps
// above, they are not multiplied by the plate's text-size scale. Renderer.cpp
// translates the whole laid-out box by the current offset, so the label rises into
// place on entry and sinks as it leaves.
inline constexpr float ENTRANCE_RISE_PX = 10.0f;  ///< Upward settle distance on entrance
inline constexpr float EXIT_SINK_PX = 8.0f;       ///< Downward recede distance on exit

}  // namespace RenderConstants
