#pragma once

#include "PCH.h"

/**
 * @namespace Hooks
 * @brief Game engine hooks for D3D11 and HUD rendering integration.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Hooks
 *
 * Provides low-level integration with Skyrim's rendering pipeline using
 * SKSE's trampoline system for safe function hooking. Intercepts D3D11
 * initialization and HUD rendering to inject ImGui overlay drawing.
 *
 * ## :material-hook: Hook Architecture
 *
 * ```mermaid
 * %%{init: {'theme':'dark', 'look':'handDrawn'}}%%
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
 *     end
 *     subgraph R[Rendering]
 *         direction TB
 *         Renderer[Renderer::Draw]:::render
 *     end
 *     Game -->|SKSEPlugin_Load| Hook
 *     Hook -->|Install thunk call hook| D3D
 *     Hook -->|Install vtable hook| HUD
 *     N[Runtime Execution]:::note
 *     Hook --- N --- Renderer
 *     Game -->|Create swap chain| D3D
 *     D3D -->|Call original function| D3D
 *     D3D -->|Initialize ImGui| Renderer
 *     subgraph LOOP[Every Frame]
 *         direction TB
 *         Game2[Game]:::core -->|PostDisplay| HUD2[HUDMenu::PostDisplay]:::hook
 *         HUD2 -->|Call original function| HUD2
 *         HUD2 -->|Draw overlays| Renderer2[Renderer::Draw]:::render
 *     end
 * ```
 *
 * ## :material-hook: Hooked Functions
 *
 * |                                      Function |   Method   |    Address/Index     | Purpose                     |
 * |-----------------------------------------------|------------|----------------------|-----------------------------|
 * | `BSGraphics::Renderer::CreateD3DAndSwapChain` | Thunk call | REL_ID(75595, 77226) | D3D11 device/swapchain init |
 * |                        `HUDMenu::PostDisplay` |   VTable   |      vtable[6]       | Per-frame overlay rendering |
 *
 * ## :material-hook: Hook Flow
 *
 * ```mermaid
 * %%{init: {'theme':'dark', 'look':'handDrawn'}}%%
 * flowchart LR
 *     classDef core fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef render fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef entity fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *
 *     A[Game calls function]:::core --> B{Hook installed?}
 *     B -->|Yes| C[Execute thunk]:::entity
 *     C --> D[Call original via T::func]:::core
 *     D --> E[Execute custom code]:::render
 *     B -->|No| F[Normal execution]:::core
 * ```
 *
 * ## :material-transit-detour: SKSE Trampoline System
 *
 * Hooks use `SKSE::GetTrampoline()` to allocate executable memory for
 * detours. The trampoline preserves original function bytes, allowing
 * safe calling of the original implementation via stored function pointer.
 *
 * |  Hook Type | Bytes Required | Function                   |
 * |------------|----------------|----------------------------|
 * | Thunk call |    5 bytes     | `stl::write_thunk_call<T>` |
 * |     VTable | 8 bytes (ptr)  | `stl::write_vfunc<F, T>`   |
 *
 * ## :material-bookmark: Address Library Compatibility
 *
 * Uses Address Library (`RELOCATION_ID`) for version-independent addresses:
 *
 * |   Edition | Version | Support |
 * |-----------|---------|---------|
 * | Skyrim SE | 1.5.97  | Full    |
 * | Skyrim AE | 1.6.x   | None    |
 * | Skyrim VR | 1.4.15  | None    |
 *
 * ## :material-lock-outline: Thread Safety
 *
 * All hooks execute on the **render thread**. The HUDMenu hook triggers
 * after the game's HUD rendering, ensuring proper draw order and avoiding
 * race conditions with game state.
 *
 * @note Trampoline must be allocated before calling `Install()`.
 * @note Hook failures are logged but don't prevent plugin loading.
 *
 * @see stl::write_thunk_call, stl::write_vfunc, Renderer::Draw
 */
namespace Hooks
{
    /**
     * Install all required game hooks.
     *
     * Installs hooks for D3D11 initialization and HUD rendering. Must be called
     * once during plugin initialization in `SKSEPlugin_Load`.
     *
     * **Installation Sequence:**
     * 1. Allocate trampoline memory (14 bytes minimum)
     * 2. Hook `BSGraphics::Renderer::CreateD3DAndSwapChain`
     * 3. Hook `HUDMenu::PostDisplay` virtual function
     *
     * If hooks fail to install, errors are logged via SKSE's logger.
     * The plugin will continue loading but overlay will not function.
     *
     * ```cpp
     * // In SKSEPlugin_Load:
     * SKSE::AllocTrampoline(14);
     * Hooks::Install();
     * ```
     *
     * @pre SKSE trampoline must be initialized with sufficient space.
     * @pre Address Library must be loaded and available.
     *
     * @post D3D11 initialization will trigger ImGui setup.
     * @post HUD rendering will include overlay drawing.
     *
     * @see Renderer::Draw, Renderer::TickRT
     */
    void Install();
}
