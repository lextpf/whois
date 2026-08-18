#pragma once

#include "RenderConstants.hpp"

#include <cstddef>
#include <cstdint>

/**
 * @namespace DebugOverlay
 * @brief Debug overlay for performance monitoring and diagnostics.
 * @author Alex (https://github.com/lextpf)
 * @ingroup DebugOverlay
 *
 * Displays frame timing, actor counts, cache statistics and settings state as an ImGui window
 * in the top-left corner.
 *
 * ## :material-cogs: Settings
 *
 * Configure in glyph.ini in the section-less block near the `;; Debug Overlay`
 * comment. The parser matches scalar keys by name, so a hand-added `[Debug]`
 * section is accepted as well.
 *
 * |            Setting | Type | Default | Description                          |
 * |--------------------|------|---------|--------------------------------------|
 * | EnableDebugOverlay | bool | false   | Show the debug overlay window        |
 *
 * ## :material-view-dashboard: Displayed Metrics
 *
 * `Render()` emits these six section headers, in this order:
 *
 * | Section       | Metrics                                              |
 * |---------------|------------------------------------------------------|
 * | Performance   | FPS, frame time, rolling average, FPS bar            |
 * | Actors        | Plate-drawing count, visible, occluded, player       |
 * | Cache         | Entry count, current frame number                    |
 * | Updates       | Updates per second, post-load cooldown               |
 * | Settings      | Occlusion, glow, typewriter, offsets, caps, key      |
 * | Memory (Est.) | Cache and snapshot byte estimates                    |
 *
 * `lastReloadTime` is not printed as a value. It only drives a fading
 * `[Reloaded!]` flash next to the title.
 *
 * ## :material-view-dashboard: Usage
 *
 * Call `UpdateFrameStats()` then `Render()` on each frame the overlay is enabled. With the
 * overlay off, skip both calls; Stats keeps the values it held when the overlay was last on.
 *
 * ```cpp
 * // In the render loop, gate both calls on the frame's RenderSettingsSnapshot
 * // instead of reading the mutex-guarded settings directly:
 * if (snap.enableDebugOverlay) {
 *     DebugOverlay::UpdateFrameStats(stats, deltaTime, currentTime, ...);
 *     DebugOverlay::Render(context);
 * }
 * ```
 *
 * ## :material-view-dashboard: Performance
 *
 * Frame time is smoothed by a rolling average over `RenderConstants::FRAME_TIME_SAMPLES`
 * samples. Overhead is about 0.1 ms per frame when visible.
 */
namespace DebugOverlay
{
/**
 * @struct Stats
 * @brief Frame timing and actor counters shown by the overlay.
 *
 * Plain data, owned by the caller. UpdateFrameStats refreshes it on each frame the overlay
 * is enabled; the values are held unchanged while it is disabled, so a re-enabled overlay
 * shows the last frame it saw until the next update.
 */
struct Stats
{
    // Frame Timing
    float fps = .0f;             ///< Current frames per second
    float frameTimeMs = .0f;     ///< Current frame time in milliseconds
    float avgFrameTimeMs = .0f;  ///< Rolling average frame time

    // Actor Stats
    /// Plate-drawing actors in this frame's snapshot. Deck-only entries and
    /// the player, when the player is hidden, are not counted. The number of
    /// tracked actors is cacheSize.
    int actorCount = 0;

    int visibleActors = 0;   ///< Actors currently visible
    int occludedActors = 0;  ///< Actors hidden by occlusion

    /// 1 when the player is present in this frame's snapshot. Set before the
    /// occlusion test, so an occluded player also reports 1.
    int playerVisible = 0;

    // Cache Stats
    size_t cacheSize = 0;  ///< Number of entries in actor cache

    // Update Stats
    int updatesPerSecond = 0;  ///< Actor data updates per second

    // Rolling Average Data
    float frameTimeHistory[RenderConstants::FRAME_TIME_SAMPLES] = {
        0};                  ///< Frame time history buffer
    int frameTimeIndex = 0;  ///< Current index in history buffer
};

/**
 * @struct Context
 * @brief Everything Render needs, so the overlay does not couple to Renderer internals.
 *
 * Built fresh by the caller each frame and passed by const reference. Only `stats` is a
 * pointer; every other field is a copy, so the struct holds no lock and owns nothing.
 */
struct Context
{
    Stats* stats = nullptr;    ///< Non-owning; must outlive Render, which only reads it
    uint32_t frameNumber = 0;  ///< Current frame counter
    int postLoadCooldown = 0;  ///< Frames remaining in post-load cooldown
    /// Time of the last settings reload, on the same clock as ImGui::GetTime(). It drives
    /// a fading [Reloaded!] flash next to the title and is never printed as a value. The
    /// default sits far enough in the past that no flash shows at startup.
    float lastReloadTime = -10.0f;
    size_t actorCacheEntrySize = 0;  ///< sizeof(ActorCache) for memory estimate
    size_t actorDrawDataSize = 0;    ///< sizeof(ActorDrawData) for memory estimate

    // Settings copied from the frame's RenderSettingsSnapshot, which is itself
    // captured under Settings::Mutex(). No lock is held while filling these.
    bool occlusionEnabled = false;   ///< Occlusion culling is on
    bool glowEnabled = false;        ///< Text glow is on
    bool typewriterEnabled = false;  ///< Typewriter reveal is on
    bool hidePlayer = false;         ///< Player plate is suppressed
    float verticalOffset = .0f;      ///< Plate height above the actor's head, in game units
    int maxPlates = 0;               ///< Cap on plates drawn per frame
    int maxScanActors = 0;           ///< Cap on actors scanned per snapshot
    size_t tierCount = 0;            ///< Number of configured color tiers
    int reloadKey = 0;               ///< Windows virtual-key code for hot reload; <= 0 disables it
};

/**
 * @brief Update frame timing statistics.
 *
 * Call every frame before Render(). It writes one sample into the circular history buffer,
 * recomputes the rolling average over the whole buffer, and refreshes the per-second update
 * rate. The rate is differenced at most once per second, so it holds its previous value in
 * between; the caller must keep the two in/out parameters alive across frames for that.
 *
 * @param stats            Stats to update, in place.
 * @param deltaTime        Frame delta, in seconds. A value of 0, which the game reports while
 *                         paused, yields an FPS of 0 rather than a division by zero.
 * @param currentTime      Current time in seconds, from `ImGui::GetTime`.
 * @param lastUpdateTime   In/out: the `currentTime` of the last per-second sample. It is
 *                         rewritten only when at least one second has passed.
 * @param updateCounter    Running total of snapshot updates since start.
 * @param lastUpdateCount  In/out: the counter value at the last per-second sample. The
 *                         difference against `updateCounter` becomes updatesPerSecond.
 */
void UpdateFrameStats(Stats& stats,
                      float deltaTime,
                      float currentTime,
                      float& lastUpdateTime,
                      int updateCounter,
                      int& lastUpdateCount);

/**
 * @brief Draw the ImGui window with performance metrics and state information.
 *
 * @pre An ImGui frame is active. The function issues ImGui calls directly and does not
 *      start or end a frame of its own.
 * @pre The caller has already checked the overlay enable flag. Render() does not read
 *      Settings and does not re-check it.
 *
 * @note Render-thread only.
 * @note A null `ctx.stats` makes the call a no-op. That is the only input it validates.
 *
 * @param ctx Context containing all data needed for display. Read only, not retained.
 */
void Render(const Context& ctx);

}  // namespace DebugOverlay
