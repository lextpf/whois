#pragma once

#include "RenderConstants.h"

#include <cstddef>
#include <cstdint>

/**
 * @namespace DebugOverlay
 * @brief Debug overlay for performance monitoring and diagnostics.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Rendering
 *
 * Provides real-time display of frame timing, actor counts, cache statistics,
 * and settings state. Rendered as an ImGui window in the top-left corner.
 *
 * ## :material-cogs: Settings
 *
 * Configure in whois.ini under `[Debug]`:
 *
 * |            Setting | Type | Default | Description                          |
 * |--------------------|------|---------|--------------------------------------|
 * | EnableDebugOverlay | bool | false   | Show the debug overlay window        |
 *
 * ## :material-view-dashboard: Displayed Metrics
 *
 * | Category     | Metrics                                              |
 * |--------------|------------------------------------------------------|
 * | Frame Timing | FPS, frame time, rolling average                     |
 * | Actors       | Total tracked, visible, occluded, player visible     |
 * | Cache        | Entry count, memory estimate                         |
 * | Updates      | Actor data updates per second                        |
 * | State        | Post-load cooldown, last reload time, frame number   |
 *
 * ## :material-view-dashboard: Usage
 *
 * The overlay is automatically rendered when enabled. Statistics are updated
 * every frame via `UpdateFrameStats()` and displayed via `Render()`.
 *
 * ```cpp
 * // In render loop:
 * DebugOverlay::UpdateFrameStats(stats, deltaTime, currentTime, ...);
 * if (Settings::EnableDebugOverlay) {
 *     DebugOverlay::Render(context);
 * }
 * ```
 *
 * ## :material-view-dashboard: Performance
 *
 * The overlay uses a rolling average for frame time smoothing with a
 * configurable sample count (`RenderConstants::kFrameTimeSamples`).
 * Overhead is minimal (~0.1ms per frame when visible).
 */
namespace DebugOverlay
{
    /**
     * Statistics tracked for the debug overlay display.
     * Updated every frame with current performance metrics and actor state.
     */
    struct Stats
    {
        // Frame Timing
        float fps = 0.0f;             ///< Current frames per second
        float frameTimeMs = 0.0f;     ///< Current frame time in milliseconds
        float avgFrameTimeMs = 0.0f;  ///< Rolling average frame time

        // Actor Stats
        int actorCount = 0;           ///< Total actors being tracked
        int visibleActors = 0;        ///< Actors currently visible
        int occludedActors = 0;       ///< Actors hidden by occlusion
        int playerVisible = 0;        ///< Whether player nameplate is visible

        // Cache Stats
        size_t cacheSize = 0;         ///< Number of entries in actor cache

        // Update Stats
        int updatesPerSecond = 0;     ///< Actor data updates per second

        // Rolling Average Data
        float frameTimeHistory[RenderConstants::kFrameTimeSamples] = {0};  ///< Frame time history buffer
        int frameTimeIndex = 0;                                            ///< Current index in history buffer
    };

    /**
     * Contains all state needed to display the overlay without coupling
     * to Renderer internals.
     */
    struct Context
    {
        Stats* stats = nullptr;           ///< Pointer to stats (for updating frame history)
        uint32_t frameNumber = 0;         ///< Current frame counter
        int postLoadCooldown = 0;         ///< Frames remaining in post-load cooldown
        float lastReloadTime = -10.0f;    ///< Time of last settings reload
        size_t actorCacheEntrySize = 0;   ///< sizeof(ActorCache) for memory estimate
        size_t actorDrawDataSize = 0;     ///< sizeof(ActorDrawData) for memory estimate
    };

    /**
     * Update frame timing statistics.
     *
     * Call this every frame before rendering. Updates the rolling average
     * and per-second metrics.
     *
     * @param stats The stats structure to update.
     * @param deltaTime Frame delta time in seconds.
     * @param currentTime Current time in seconds (from `ImGui::GetTime`).
     * @param lastUpdateTime Reference to last debug update time.
     * @param updateCounter Current update counter.
     * @param lastUpdateCount Reference to last update count.
     */
    void UpdateFrameStats(
        Stats& stats,
        float deltaTime,
        float currentTime,
        float& lastUpdateTime,
        int updateCounter,
        int& lastUpdateCount
    );

    /**
     * Render the debug overlay window.
     *
     * Draws an ImGui window with performance metrics and state information.
     *
     * @param ctx Context containing all data needed for display.
     */
    void Render(const Context& ctx);

}
