#pragma once

#include "PCH.hpp"

/**
 * @namespace Renderer
 * @brief Main rendering system for the nameplate overlay.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Renderer
 *
 * Multi-threaded floating nameplates for Skyrim SE: world-to-screen projection, actor
 * tracking, smoothing, and visual effects (particles, ornaments, tier text effects).
 *
 * ## :material-sitemap-outline: Architecture
 *
 * Producer/consumer split by thread. The game thread collects actor data through
 * `SKSE::GetTaskInterface()->AddTask()`; the render thread draws from the published copy.
 * The render thread never dereferences an actor: every actor fact reaches it as plain data
 * in the snapshot. The only `RE::*` state it reads is the camera and renderer singletons
 * (`RE::Main::WorldRootCamera`, `RE::PlayerCamera`, `RE::BSGraphics::Renderer`): the camera
 * pose for projection, near-camera scaling, pan-speed quieting and the aim ray, and the
 * renderer for the screen size.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef thread fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef data fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *     classDef render fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *
 *     RT[Render Thread]:::thread -->|Schedule update| GT[Game Thread]:::thread
 *     GT -->|Publish ActorDrawData| Snap[Snapshot - guarded by snapshotLock]:::data
 *     Snap -->|Copy under snapshotLock| RT
 *     RT -->|Owns smoothing state| Cache[ActorCache - render thread only]:::data
 *     RT -->|Draw nameplates| ImGui[ImGui]:::render
 * ```
 *
 * ## :material-pipe: Render Pipeline
 *
 * Order inside one `Draw()` call. The first three nodes are gates: each one returns without
 * drawing, so a suppressed frame never reaches the snapshot copy.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef check fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef process fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef render fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     A[Hot reload in flight?]:::check --> B[Overlay allowed?]:::check
 *     B --> C[Post-load cooldown?]:::check
 *     C --> D[Queue actor update]:::process
 *     D --> E[Copy snapshot]:::process
 *     E --> F[Project / smooth]:::process
 *     F --> G[Draw plates, rites, exits]:::render
 *     G --> H[Composite and prune cache]:::render
 * ```
 *
 * ## :material-chart-bell-curve-cumulative: Exponential Smoothing
 *
 * Alpha, scale, occlusion, focus, yield, the camera-quiet factors, the scene-profile knobs
 * and the per-actor candlelight sample all approach their targets framerate-independently:
 *
 * $$v_{new} = v_{old} + (v_{target} - v_{old}) \cdot \alpha$$
 *
 * where $\alpha = 1 - \epsilon^{\,\Delta t \,/\, T_{settle}}$, $\epsilon = 0.01$ (a 1%
 * residual threshold, not machine epsilon), and $T_{settle}$ is the time for the value to
 * settle within $\epsilon$ of the target.
 *
 * Screen position is the exception. It blends the exponential result with an 8-frame
 * moving average (`Visual().PositionSmoothingBlend`), and above
 * `Visual().LargeMovementThreshold` it applies the raw per-frame
 * `Visual().LargeMovementBlend`. Both of those terms count frames, not time, so they are
 * framerate-dependent.
 *
 * ## :material-cube-scan: World-to-Screen Projection
 *
 * `RE::NiCamera::WorldPtToScreenPt3()` from CommonLibSSE projects a 3D world point into
 * normalized viewport coordinates:
 *
 * | Component | Meaning                                                     |
 * |:---------:|-------------------------------------------------------------|
 * | $x$       | Horizontal screen position $[0, 1]$                         |
 * | $y$       | Vertical screen position $[0, 1]$, measured upward           |
 * | $z$       | Viewport depth $[0, 1]$; outside that range the point is cut |
 *
 * The internal wrapper `Renderer::WorldToScreen` (declared in RendererInternal.hpp, defined
 * in Renderer.cpp) converts them to ImGui pixels ($x_{px} = x \cdot w$,
 * $y_{px} = (1 - y) \cdot h$), so $y$ increases downward in every consumer. The draw path
 * rejects a point when $z < 0$ or $z > 1$. The depth-buffer polarity is not known at compile
 * time; `ComputeDepthPolarity()` resolves it at runtime from two probe points.
 *
 * ## :material-connection: SKSE Integration
 *
 * | System                 | API                                     | Purpose                  |
 * |------------------------|-----------------------------------------|--------------------------|
 * | D3D11 init             | `CreateD3DAndSwapChain` (thunk call)    | Device and swapchain     |
 * | Render hook            | `HUDMenu::PostDisplay` (vtable[6])      | Per-frame draw timing    |
 * | Render hook (fallback) | `IDXGISwapChain::Present` (vtable[8])   | Upscaler fallback path   |
 * | Actor iteration        | `RE::ProcessLists`                      | Find nearby actors       |
 * | Occlusion              | `RE::Actor::HasLineOfSight()`           | Line-of-sight checks     |
 * | Task scheduling        | `SKSE::GetTaskInterface()->AddTask()`   | Game-thread data collect |
 *
 * The Present hook drives the same overlay path as `HUDMenu::PostDisplay`, and renders
 * only when PostDisplay did not render this frame, which happens when an upscaler stack
 * (DLSS, FSR) skips or defers PostDisplay.
 *
 * ## :material-speedometer: Performance
 *
 * | Strategy           | Detail                                                              |
 * |--------------------|---------------------------------------------------------------------|
 * | Occlusion throttle | `Occlusion().CheckInterval` snapshot updates between casts, default 3 |
 * | Cache pruning      | 60 idle render frames, 3x that while a plate is still exiting       |
 * | Actor cap          | `MaxPlates`: default 16, clamped to 1-128                           |
 * | Scan cap           | `MaxScanActors`: default 128, 1-4096, never below `MaxPlates`       |
 *
 * @see Hooks::Install, TextEffects
 */
namespace Renderer
{
/**
 * @brief Draw all floating nameplates for this frame.
 *
 * Primary entry point from the render hook. Render-thread only; actor data is read from a
 * mutex-protected snapshot.
 *
 * The hook calls `TickRT()` before the original function, then renders through a helper
 * that opens and closes the ImGui frame around this call. The `IDXGISwapChain::Present`
 * fallback hook drives the same helper when an upscaler skips `HUDMenu::PostDisplay`.
 *
 * The call also carries the frame's shared work: it runs the hot-reload key check, advances
 * every animation timer, and prunes the actor cache. A skipped call freezes all of that, not
 * only the pixels. The snapshot request is the exception: the hook calls `TickRT()` on every
 * frame, so the actor data stays current while `Draw()` is skipped.
 *
 * Three gates make the call return early before it draws anything: a hot reload is in flight,
 * the game-thread `allowOverlay` gate is false, or the post-load cooldown is still counting
 * down. The cooldown suppresses 300 rendered frames (about 5 s at 60 fps) after the overlay
 * leaves a suppressed state, and the first frame drawn after it replays every entrance
 * animation as a staggered cascade. The frame is also dropped when the BSGraphics renderer
 * singleton is unavailable or the copied snapshot is empty.
 *
 * ```cpp
 * // Hooks.cpp: HUDMenu::PostDisplay thunk (simplified)
 * static void thunk(RE::IMenu* a_menu) {
 *     Renderer::TickRT();
 *     Renderer::PrepareDeckCaptureRT();
 *     // Deck can request the frame on its own; Draw() itself stays gated below.
 *     const bool shouldRender = Renderer::IsOverlayAllowedRT() || Deck::NeedsFrame();
 *     func(a_menu);  // Call original
 *     if (shouldRender) {
 *         RenderOverlayNow();
 *     }
 * }
 *
 * // Hooks.cpp: RenderOverlayNow (simplified) - Draw() runs inside the frame
 * ImGui::NewFrame();
 * if (Renderer::IsOverlayAllowedRT()) {
 *     Renderer::Draw();
 * }
 * ImGui::EndFrame();
 * ImGui::Render();
 * ```
 *
 * @pre ImGui context must be initialized.
 * @pre Must be called within an ImGui frame (after NewFrame, before EndFrame).
 *
 * @see TickRT, IsOverlayAllowedRT
 */
void Draw();

/**
 * @brief Schedule the game-thread actor update for this frame.
 *
 * Call once per frame, before `Draw()`, and unconditionally: also on frames where the
 * overlay is not drawn. The game-thread update publishes the `allowOverlay` gate that
 * `IsOverlayAllowedRT()` reports, so the overlay never bootstraps without this call. The
 * data collection itself runs on the game thread through SKSE's task interface. Requests
 * coalesce, so at most one game-thread task is ever in flight.
 *
 * It also polls the Deck capture key, which reads the settings under the shared settings
 * lock. A press is only recorded here; PrepareDeckCaptureRT resolves it into a request.
 *
 * @pre Must be called from the render thread.
 *
 * @see Draw, IsOverlayAllowedRT, PrepareDeckCaptureRT
 */
void TickRT();

/**
 * @brief Check if overlay rendering is allowed.
 *
 * The result is the manual-enable flag AND the published game-state flag. It is false when
 * the user disabled rendering with `ToggleEnabled()` or `SetEnabled()`, and false when
 * `GameState::CanDrawOverlay()` returned false (loading, menus, combat, unattached cell,
 * and similar states).
 *
 * `TickRT()` only queues a game-thread snapshot update. That update calls
 * `GameState::CanDrawOverlay()` on the game thread and publishes the result into an atomic
 * flag, so the value reflects the last completed snapshot, not the current frame.
 *
 * A false result stops nameplate drawing only. Deck capture has its own game-thread gate,
 * so the hook can still open an ImGui frame for the Deck status toast while this is false.
 *
 * @return `true` if overlay can be drawn, `false` if it should be hidden.
 *
 * @post Return value is valid for current frame only.
 *
 * @see Draw, TickRT, GameState::CanDrawOverlay
 */
bool IsOverlayAllowedRT();

/**
 * @brief Toggle the rendering on/off.
 *
 * Atomic read-modify-write on the manual-enable flag. Callable from any thread; the console
 * command handler calls it from the game thread. The change takes effect on the next frame.
 *
 * @return The new enabled state (true = enabled, false = disabled).
 */
bool ToggleEnabled();

/**
 * @brief Set the nameplate rendering state to a specific value.
 *
 * Absolute-target form of `ToggleEnabled()`, with the same atomic store and the same
 * next-frame effect.
 *
 * @param enabled  New enabled state.
 */
void SetEnabled(bool enabled);

/**
 * @brief Return the current manual-enabled state.
 *
 * True if the user has not turned rendering off with the console command
 * (`ToggleEnabled()` or `SetEnabled()`). Independent of game-state gating that may still
 * suppress drawing for other reasons, so a true result does not mean plates are visible.
 * Atomic load, callable from any thread.
 *
 * @return true while manual rendering is enabled.
 */
bool IsEnabled();

/**
 * @brief Force a one-shot refresh of the player's cached identity.
 *
 * Drops the player's render-thread cache entry, so the name and the typewriter reveal
 * rebuild on the next frame. Called from the RaceMenu-close event sink so a rename is
 * picked up promptly. Thread-safe: sets an atomic consumed on the render thread.
 *
 * The next `Draw()` consumes the flag. It erases the player cache entry and clears the
 * snapshot pause, so the following game-thread update is guaranteed to run and to publish
 * the new name.
 */
void RequestIdentityRefresh();

/**
 * @brief Publish a pending Deck card render request.
 *
 * Resolves a pending Deck key press against the game-thread actor snapshot and publishes a
 * plain-data card render request. Render-thread only; safe to call before the HUD is drawn
 * so Deck can immediately copy a clean scene.
 *
 * Returns without work when no key press is pending or when Deck is disabled. The target is
 * the live crosshair actor, then the live and unoccluded non-player actor whose projected
 * head-to-feet segment is closest to the screen center, within `DeckTargetRadius` pixels of
 * it, then the player when `DeckPlayerFallback` allows it. Every failure path reports
 * through Deck's own status toast instead of raising an error.
 */
void PrepareDeckCaptureRT();
}  // namespace Renderer
