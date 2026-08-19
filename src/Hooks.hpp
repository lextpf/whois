#pragma once

#include "PCH.hpp"

/**
 * @namespace Hooks
 * @brief Game engine hooks for D3D11 and HUD rendering integration.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Hooks
 *
 * Intercepts D3D11 initialization and HUD rendering so the ImGui overlay can be drawn inside
 * the game's own pipeline, with one SKSE trampoline call hook and two vtable patches.
 *
 * ## :material-hook: Hook Architecture
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef core fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef hook fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *     classDef render fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef note fill:transparent,stroke:#94a3b8,color:#e2e8f0,stroke-dasharray:6 4
 *     subgraph GE[Game Engine]
 *         direction TB
 *         Game[Skyrim Engine]:::core
 *     end
 *     subgraph HS[Hook System]
 *         direction TB
 *         Hook[Hooks::Install]:::hook
 *         D3D[CreateD3DAndSwapChain]:::hook
 *         HUD[HUDMenu::PostDisplay]:::hook
 *         PRE[IDXGISwapChain::Present]:::hook
 *     end
 *     subgraph R[Rendering]
 *         direction TB
 *         Boot[ImGui initialization]:::render
 *         Renderer[Renderer::Draw]:::render
 *     end
 *     Game -->|SKSEPlugin_Load| Hook
 *     Hook -->|Install thunk call hook| D3D
 *     Hook -->|Install vtable hook| HUD
 *     Game -->|Create swap chain| D3D
 *     D3D -->|Call original function| D3D
 *     D3D -->|Start init| Boot
 *     HUD -->|Retry init on 5 s backoff| Boot
 *     Boot -->|Install COM vtable hook| PRE
 *     N[Runtime Execution]:::note
 *     Boot --- N --- Renderer
 *     subgraph LOOP[Every Frame]
 *         direction TB
 *         Game2[Game]:::core -->|PostDisplay| HUD2[HUDMenu::PostDisplay]:::hook
 *         HUD2 -->|Call original function| HUD2
 *         HUD2 -->|Draw overlays| Renderer2[Renderer::Draw]:::render
 *         Game3[Game]:::core -->|Present, PostDisplay skipped| PRE2[PresentHook]:::hook
 *         PRE2 -->|Draw overlays| Renderer2
 *     end
 * ```
 *
 * Only `CreateD3DAndSwapChain` and `HUDMenu::PostDisplay` start ImGui initialization. The
 * Present hook draws, but never initializes, so it cannot bootstrap the overlay on its own.
 *
 * ## :material-hook: Hooked Functions
 *
 * | Function                  | Method     | Address / Index               | Purpose           |
 * |---------------------------|------------|-------------------------------|-------------------|
 * | `CreateD3DAndSwapChain`   | Thunk call | `RELOCATION_ID(75595, 77226)` | D3D11 device init |
 * | `HUDMenu::PostDisplay`    | VTable     | `vtable[6]`                   | Per-frame overlay |
 * | `IDXGISwapChain::Present` | COM VTable | `vtable[8]`                   | Upscaler fallback |
 *
 * The `CreateD3DAndSwapChain` hook does not patch the function start. It patches a `CALL rel32`
 * inside the function, at `GLYPH_OFFSET(0x9, 0x275)` (SE offset 0x9, AE offset 0x275).
 * `Install()` reads the byte at that address and installs the hook only when the byte is 0xE8.
 *
 * The Present hook is a safety net: when upscalers (DLSS, FSR) restructure the pipeline and skip
 * `PostDisplay`, the overlay is drawn in `PresentHook` just before the backbuffer flip.
 *
 * `Install()` does not install the Present hook. ImGui initialization installs it on the first
 * frame that has a swapchain, and the device-change handler installs it again when the renderer
 * device, context, or swapchain pointer changes. That same path shuts down and re-creates every
 * GPU-owning subsystem (ParticleTextures, BadgeTextures, Deck, TextPostProcess, SceneMeter,
 * DepthClip, Graffito). Do not cache a D3D pointer obtained through these hooks across frames.
 *
 * The `CreateD3DAndSwapChain` thunk is the only caller of the device-change handler. A device
 * or swapchain swap that does not re-enter that call is therefore not detected.
 *
 * ## :material-hook: Hook Flow
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef core fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef render fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef entity fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *
 *     A[Game calls hooked function]:::core --> B{Hook installed?}
 *     B -->|No| F[Normal execution]:::core
 *     B -->|Yes| C[Enter thunk]:::entity
 *     C --> P["Pre-pass: PostDisplay and Present only"]:::render
 *     P --> D["Call original: T::func, or the saved Present"]:::core
 *     D --> E["Post-pass: PostDisplay and CreateD3DAndSwapChain only"]:::render
 * ```
 *
 * The three hooks use different halves of that shape. `CreateD3DAndSwapChain` runs the original
 * first and then initializes. `HUDMenu::PostDisplay` runs a pre-pass, then the original, then
 * the overlay draw. `PresentHook` runs the fallback frame first, then the original Present.
 *
 * ## :material-transit-detour: SKSE Trampoline System
 *
 * `SKSE::GetTrampoline()` allocates the executable memory for a detour and keeps the original
 * function bytes, so the hook can call the original through a stored function pointer.
 *
 * |  Hook Type | Patch site    | Function                 |
 * |------------|---------------|--------------------------|
 * | Thunk call | 5 bytes       | `Stl::WriteThunkCall<T>` |
 * |     VTable | 8 bytes (ptr) | `Stl::WriteVfunc<F, T>`  |
 *
 * The "Patch site" column is the number of bytes overwritten in place, not a trampoline budget.
 * A VTable hook writes one 8-byte function pointer and uses no trampoline memory. Only the
 * thunk-call hook draws from `SKSE::GetTrampoline()`, and its trampoline allocation is larger
 * than the 5-byte patch site.
 *
 * ## :material-bookmark: Address Library Compatibility
 *
 * `RELOCATION_ID` resolves version-independent addresses through Address Library:
 *
 * | Edition         | Version | Support |
 * |-----------------|---------|---------|
 * | Skyrim SE       | 1.5.97  | Full    |
 * | Skyrim AE Steam | 1.6.x   | Full    |
 * | Skyrim AE GOG   | 1.6.x   | Full    |
 * | Skyrim VR       | 1.4.15  | None    |
 *
 * ## :material-lock-outline: Thread Safety
 *
 * The two per-frame hooks, `HUDMenu::PostDisplay` and `IDXGISwapChain::Present`, execute on the
 * render thread. The `CreateD3DAndSwapChain` thunk executes on the thread that creates the
 * renderer device: at startup, and again on each later device or swapchain re-creation that
 * re-enters that call. The HUDMenu hook wraps the game's HUD rendering in three steps: the pre-pass
 * (frame flags, lazy ImGui init, `Renderer::TickRT`, the pre-HUD Deck scene capture, and the
 * render gate), then the original `PostDisplay`, then the overlay, which is drawn last so it
 * lands on top of the HUD and in screenshots.
 *
 * The hooks run off the game thread, so they must never read live game state. Gate rendering on
 * `Renderer::IsOverlayAllowedRT()`, the atomic that the game-thread snapshot publishes and that
 * `Renderer::TickRT()` queues. A direct read (for example `player->GetParentCell()`) can race
 * with cell teardown during exterior streaming.
 *
 * @note The trampoline must be allocated before `Install()` is called.
 * @note Hook failures are logged and do not prevent plugin loading.
 *
 * @see Stl::WriteThunkCall, Stl::WriteVfunc, Renderer::Draw
 */
namespace Hooks
{
/**
 * @brief Install the D3D11 and HUD hooks.
 *
 * Call once during plugin initialization, from `SKSEPlugin_Load`. The caller allocates the
 * trampoline; `Install()` does not.
 *
 * **Installation Sequence:**
 * 1. Reject any runtime that is not SE or AE. This is defense in depth;
 *    `SKSEPlugin_Load` already rejects those runtimes.
 * 2. Verify the `CreateD3DAndSwapChain` patch site reads 0xE8 (`CALL rel32`),
 *    then hook it with `Stl::WriteThunkCall`.
 * 3. Hook `HUDMenu::PostDisplay` (vtable index 6) with `Stl::WriteVfunc`.
 *
 * On a runtime other than SE or AE, `Install()` logs an error and returns without installing
 * anything. Hook failures are logged and reported as a partial install. If only the
 * `CreateD3DAndSwapChain` hook fails (patch-site opcode mismatch), the overlay still initializes
 * lazily from the `HUDMenu::PostDisplay` path. If only the `PostDisplay` hook fails,
 * initialization depends on the `CreateD3DAndSwapChain` thunk: when that thunk still runs it
 * starts ImGui, and the Present fallback then draws every frame. If both hooks fail, nothing
 * starts ImGui and no plate is drawn.
 *
 * ```cpp
 * // In SKSEPlugin_Load:
 * SKSE::AllocTrampoline(256);
 * Hooks::Install();
 * ```
 *
 * @pre SKSE trampoline must be initialized with sufficient space.
 * @pre Address Library must be loaded and available.
 *
 * @post D3D11 initialization attempts ImGui setup. `HUDMenu::PostDisplay` retries the same
 *       attempt on a 5 s backoff until it succeeds. The Present hook does not retry it.
 * @post HUD rendering includes the overlay draw.
 *
 * @see Renderer::Draw, Renderer::TickRT
 */
void Install();
}  // namespace Hooks
