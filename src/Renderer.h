#pragma once

#include "PCH.h"

/**
 * @namespace Renderer
 * @brief Main rendering system for the overlay.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Rendering
 *
 * Implements a multi-threaded floating nameplate system for Skyrim SE.
 * Handles world-to-screen projection, actor tracking, smooth animations,
 * and visual effects (particles, ornaments, tier text effects).
 *
 * ## :material-sitemap-outline: Architecture
 *
 * Uses a producer-consumer pattern for thread safety:
 *
 * - **Game Thread**: Collects actor data via `SKSE::GetTaskInterface()->AddTask()`
 * - **Render Thread**: Draws nameplates using cached data
 *
 * ```mermaid
 * %%{init: {'theme':'dark', 'look':'handDrawn'}}%%
 * flowchart LR
 *     classDef thread fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef data fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *     classDef render fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *
 *     RT[Render Thread]:::thread -->|Schedule update| GT[Game Thread]:::thread
 *     GT -->|Collect actors| Cache[Actor Cache]:::data
 *     RT -->|Copy snapshot| Cache
 *     RT -->|Draw nameplates| ImGui[ImGui]:::render
 * ```
 *
 * ## :material-pipe: Render Pipeline
 *
 * ```mermaid
 * %%{init: {'theme':'dark', 'look':'handDrawn'}}%%
 * flowchart LR
 *     classDef check fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef process fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef render fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     A[Overlay Allowed?]:::check --> B[Queue Actor Update]:::process
 *     B --> C[Copy Snapshot]:::process
 *     C --> D[Project / Smooth]:::process
 *     D --> E[Apply Effects]:::render
 *     E --> F[Render]:::render
 * ```
 *
 * ## :material-chart-bell-curve-cumulative: Exponential Smoothing
 *
 * All animated values use framerate-independent exponential approach:
 *
 * $$v_{new} = v_{old} + (v_{target} - v_{old}) \cdot \alpha$$
 *
 * where $\alpha = 1 - \epsilon^{\,\Delta t \,/\, T_{settle}}$ and $T_{settle}$
 * is the time for the value to reach within $\epsilon$ (default 1%) of the
 * target. This ensures identical visual behavior regardless of framerate.
 *
 * ## :material-cube-scan: World-to-Screen Projection
 *
 * Uses `RE::NiCamera::WorldPtToScreenPt3()` from CommonLibSSE to project
 * 3D world coordinates to 2D screen space:
 *
 * | Component | Meaning                                     |
 * |:---------:|---------------------------------------------|
 * | $x$       | Horizontal screen position $[0, 1]$         |
 * | $y$       | Vertical screen position $[0, 1]$           |
 * | $z$       | $> 0$ in front of camera, $< 0$ behind      |
 *
 * ## :material-connection: SKSE Integration
 *
 * | System               | API                                     | Purpose                  |
 * |----------------------|-----------------------------------------|--------------------------|
 * | Render hook          | `HUDMenu::PostDisplay` (vtable[6])      | Per-frame draw timing    |
 * | Actor iteration      | `RE::ProcessLists`                      | Find nearby actors       |
 * | Occlusion            | `RE::TESObjectREFR::HasLineOfSight()`   | Line-of-sight checks     |
 * | Task scheduling      | `SKSE::GetTaskInterface()->AddTask()`   | Game-thread data collect |
 *
 * ## :material-speedometer: Performance
 *
 * | Strategy                | Detail                                    |
 * |-------------------------|-------------------------------------------|
 * | Occlusion throttle      | Configurable interval                     |
 * | Cache pruning           | Stale entries removed after grace period  |
 * | Actor cap               | Max 16 rendered simultaneously            |
 *
 * @see Hooks::PostDisplay, TextEffects
 */
namespace Renderer
{
    /**
     * Main draw function called each frame.
     *
     * Renders all floating nameplates using ImGui. This is the primary
     * entry point called from the render hook.
     *
     * Must be called from the render thread. Actor data is accessed
     * through a mutex-protected snapshot.
     *
     * ```cpp
     * // Called from HUDMenu::PostDisplay hook
     * void HUDHook::thunk(RE::HUDMenu* a_this) {
     *     func(a_this);  // Call original
     *
     *     Renderer::TickRT();
     *     if (Renderer::IsOverlayAllowedRT()) {
     *         Renderer::Draw();
     *     }
     * }
     * ```
     *
     * @pre ImGui context must be initialized.
     * @pre Must be called within an ImGui frame (after NewFrame, before EndFrame).
     *
     * @see TickRT, IsOverlayAllowedRT
     */
    void Draw();

    /**
     * Render thread tick function.
     *
     * Called every frame to schedule actor data updates. The actual
     * data collection runs on the game thread via SKSE's task interface.
     *
     * @pre Must be called from the render thread.
     *
     * @see Draw, IsOverlayAllowedRT
     */
    void TickRT();

    /**
     * Check if overlay rendering is allowed.
     *
     * Determines whether the floating names overlay should be visible
     * based on game state.
     *
     * @return `true` if overlay can be drawn, `false` if it should be hidden.
     *
     * @post Return value is valid for current frame only.
     *
     * Overlay is hidden when:
     * - Game is loading or resetting
     * - Player is in menus (inventory, map, journal, etc.)
     * - Player cell is not attached
     * - Player is in combat (if configured)
     * - Manually disabled via ToggleEnabled()
     *
     * @see Draw, TickRT
     */
    bool IsOverlayAllowedRT();

    /**
     * Toggle the rendering on/off.
     *
     * @return The new enabled state (true = enabled, false = disabled).
     */
    bool ToggleEnabled();
}
