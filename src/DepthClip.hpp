#pragma once

#include <cstdint>

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace DepthClip
 * @brief Per-pixel depth occlusion for nameplates.
 * @author Alex (https://github.com/lextpf)
 * @ingroup TextPostProcess
 *
 * Line-of-sight culling can only show or hide a whole plate, so a plate pops in and
 * out at the moment sight breaks.  This clips each plate against the game's depth
 * buffer instead, so geometry cuts it at the real intersection, with a soft feather
 * at the edge.
 *
 * ## Mechanism
 *
 * A custom pixel shader (ImGui's shader plus a scene-depth compare) is bound around
 * each plate's draws through ImGui draw callbacks.  The plate depth is the viewport
 * depth that `WorldToScreen` already computes with the game's own projection, so it
 * matches what the rasterizer wrote into the depth buffer by construction.  The
 * shader compares the two values directly, with no remap in either direction.
 * Depth-convention
 * polarity is derived each frame from two projected probe points,
 * with no readback.
 *
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * sequenceDiagram
 *
 * participant R as Renderer - frame build
 *     participant L as ImDrawList
 *     participant B
 * as ImGui D3D11 backend
 *     participant D as DepthClip
 *     participant G as D3D11 state
 *
 * R->>D: BeginFrame - clear parameter arena and state stack
 *     R->>D: MakePlateParams,
 * MakePlaneParams, or MakeNeutralParams
 *     D-->>R: frame-owned callback parameters
 * R->>L:
 * queue ApplyCallback, plate draws, RestoreCallback
 *     L->>B: execute draw commands later
 *
 * B->>D: ApplyCallback with parameters
 *     D->>G: save pixel shader, CB0, and SRV1
 *     D->>D:
 * push active or inactive marker
 *     opt resources are usable
 *         D->>G: bind depth
 * shader, constants, and scene depth
 *     end
 *     B->>G: draw the plate
 *     B->>D:
 * RestoreCallback
 *     D->>D: pop the matching LIFO marker
 *     opt marker is active
 * D->>G:
 * restore pixel shader, CB0, and SRV1
 *     end
 *     Note over D: No context makes both
 * callbacks return without a marker
 * ```
 *
 * The scene depth SRV comes from
 * `kPOST_ZPREPASS_COPY` only.  `kMAIN` is not a
 * fallback: the live `kMAIN` depth may still be
 * bound as a DSV while the UI renders, and D3D silently nulls a conflicting SRV binding, which
 * would read as depth 0 and make every plate invisible.  When the copy is absent, the frame goes
 * unclipped and line-of-sight culling stays the only occlusion.
 *
 * ## Fallback contract
 *
 * Any unexpected condition - no depth SRV (some ENB or upscaler stacks), shader
 * compile failure, indeterminate polarity - leaves the frame unclipped, with
 * line-of-sight culling as the only occlusion.  The coarse line-of-sight gate stays
 * on regardless; depth clipping is the finer, sub-plate layer on top of it.
 *
 * ## Threading
 *
 * All module state is plain globals with no synchronization, so every entry point is
 * render-thread only.  Initialize runs from the overlay's first-frame setup, and
 * Shutdown from the device-change and teardown paths in Hooks.cpp.
 */
namespace DepthClip
{
/**
 * @brief Compile the depth-clip shader and create its constant buffer.
 *
 * @param device
 * D3D11 device that creates the resources.
 * @param context  D3D11 immediate context used by draw
 * callbacks.
 * @return         True when initialization succeeds.
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/// @brief True when the shader and buffers exist.
bool IsInitialized();

/// @brief Release all GPU resources.
void Shutdown();

/**
 * @brief Per-frame setup: find the scene depth SRV and latch feather and polarity.
 *
 * Clears the per-frame param arena and the Apply/Restore state stack before any other
 * check, so params made in an earlier frame become invalid even when this call fails.
 *
 * @param featherPx  Feather radius at the occlusion edge, in pixels.  Clamped to [0, 8].
 * @param polarity   +1 standard z (larger = farther), -1 reversed.
 * @return false when depth clipping cannot run this frame: not initialized, polarity 0
 *         (indeterminate), no renderer singleton, or no depth SRV.
 */
bool BeginFrame(float featherPx, float polarity);

/**
 * @brief Allocate per-plate callback params (valid until the next BeginFrame).
 *
 * @param plateDepthNDC  The plate's viewport depth from WorldToScreen, used unchanged
 *                       as a constant depth over the whole plate.
 * @return Opaque params pointer for ApplyCallback's user data.
 */
void* MakePlateParams(float plateDepthNDC);

/**
 * @brief Allocate params for a projected world plane.
 *
 * The coefficients describe viewport depth over normalized screen coordinates:
 * `z = depthX * u + depthY * v + depthConstant`.  This is exact for a
 * perspective-projected plane, so world geometry cuts an angled Graffito at the depth
 * of each individual pixel.
 *
 * @param depthX         Depth gradient along normalized screen x.
 * @param depthY         Depth gradient along normalized screen y.
 * @param depthConstant  Depth at the screen origin.
 * @return Opaque params pointer for ApplyCallback's user data.
 */
void* MakePlaneParams(float depthX, float depthY, float depthConstant);

/**
 * @brief Allocate params that disable clipping for the following draws.
 *
 * Used by exit and death ghosts whose reprojection failed, so the bracket structure
 * stays intact while the depth test is off.
 *
 * @return Opaque params pointer for ApplyCallback's user data.
 */
void* MakeNeutralParams();

/**
 * @brief ImDrawCallback: bind the depth-clip shader and this plate's constants.
 *
 * Applies to the following draws in the current splitter channel.  Saves the previous
 * pixel shader, pixel-shader constant-buffer slot 0 and pixel-shader resource slot 1.
 *
 * @param dl   Draw list that owns the command (unused).
 * @param cmd  Draw command whose user data is a Make*Params pointer.
 *
 * @pre The params pointer must come from a Make*Params call of the current frame.
 * @post Pushes one entry on an internal LIFO stack, including on the recoverable
 *       early-out paths (missing shader, buffer, SRV or params; empty viewport; failed
 *       Map).  Exactly one RestoreCallback must match each ApplyCallback, in LIFO
 *       order, on the same splitter channel.
 */
void ApplyCallback(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * @brief Restore the pixel-shader state captured by the matching ApplyCallback.
 *
 * Pops one stack entry.  Does nothing when the stack is empty, and touches no pipeline
 * state when the matching ApplyCallback took a recoverable early-out.
 *
 * @param dl   Draw list that owns the command (unused).
 * @param cmd  Draw command that carries the callback (unused).
 */
void RestoreCallback(const ImDrawList* dl, const ImDrawCmd* cmd);
}  // namespace DepthClip
