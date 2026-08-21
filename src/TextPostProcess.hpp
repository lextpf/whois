#pragma once

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace TextPostProcess
 * @brief GPU glow and color divide passes for nameplate text.
 * @author Alex (https://github.com/lextpf)
 * @ingroup TextPostProcess
 *
 * Render-to-texture passes that run text glow and color divide as GPU shader passes
 * instead of repeated CPU draws.  ImGui draw callbacks (the same pattern as the
 * ParticleTextures blend states) bracket the affected draws with render target switches
 * and shader dispatch.  This module implements those two passes only; outline and shadow
 * stay on the CPU in TextEffects.
 *
 * Every function is render-thread only, and none of them read game state.  Initialize and
 * Shutdown hold an internal mutex; every other function assumes a single caller and uses
 * unsynchronized file-scope state.  The draw callbacks run later on the same thread,
 * inside the ImGui D3D11 backend, and consume the values that SetGlowParams and
 * SetDivideParams stored earlier in the same frame.
 *
 * ## Glow pipeline
 *
 * ```
 * AddText (single call) -> half-res offscreen RT -> Gaussian blur (H then V)
 *                       -> screen-style composite onto the saved render target
 * ```
 *
 * The capture render target and its viewport are half the backbuffer size (integer
 * division, at least 1 pixel each way), so every draw placed between the glow brackets
 * rasterizes at half resolution, unless the color divide pass binds its own full-res
 * target first (see the warning at the end of this block).
 *
 * The composite shader is not a plain multiply of the blurred capture.  For a blurred
 * texel with color $c_{rgb}$ and alpha $c_a$, glow intensity $I$, and $\mathrm{sat}$ as a
 * clamp to [0, 1]:
 *
 * $$src_{rgb} = \mathrm{sat}\bigl(c_{rgb}\,(0.46 + 0.34\,I)\bigr), \qquad
 * a_{veil} = \mathrm{sat}\bigl(c_a^{\,0.72}\,(0.14 + 0.16\,I)\bigr)$$
 *
 * $$a_{core} = \mathrm{sat}\bigl(c_a^{\,1.18 + 0.60\,I}\,(0.34 + 0.18\,I)\bigr), \qquad
 * src_a = \mathrm{sat}\bigl(a_{veil} + 0.55\,a_{core}\bigr)$$
 *
 * The blend state then writes $src_{rgb} \cdot src_a + dst_{rgb} \cdot (1 - src_{rgb})$.
 * That is a screen-style blend, not additive: a bright background cannot clip to white.
 *
 * The CPU path `TextEffects::AddTextGlow` (up to 24 AddText calls per string) stays live:
 * each draw site selects it whenever this module is not initialized.
 *
 * ## Color divide pipeline
 *
 * ```
 * Snapshot the bound render target -> nametag draws to offscreen RT
 *                                  -> Photoshop-style color divide composite
 * ```
 *
 * The composite reads the snapshot as the background, not the current content of the
 * bound target, and writes opaque pixels with blending disabled.  The shader discards
 * every pixel whose captured text alpha is below 0.001, so pixels with no text keep
 * whatever the bound target already held.
 *
 * ## Settings
 *
 * Configure in glyph.ini.  These keys sit in the section-less area at the top of the
 * file.  The parser matches scalar keys by name in any section, and there is no
 * [Effects] section: writing one only produces an unknown-section warning.
 *
 * |            Setting | Type  | Default | Description                                    |
 * |--------------------|-------|---------|------------------------------------------------|
 * |         EnableGlow | bool  | false   | Master toggle for glow (GPU pass and CPU path) |
 * |         GlowRadius | float | 4.0     | Blur radius in pixels (Gaussian sigma proxy)   |
 * |      GlowIntensity | float | 0.5     | Composite intensity, clamped to [0, 1]         |
 * |        GlowSamples | int   | 8       | CPU fallback only; unused by the GPU blur      |
 * | GlowDivideStrength | float | 0.0     | Divide blend strength in [0, 1]; 0 disables    |
 *
 * EnableGlow gates the glow bracket and every glow draw.  Per-tier gating removes the
 * glow again for low tiers when EnableTierEffectGating is on and the tier index is below
 * GlowMinTier, but the bracket is still added and the blur still runs.
 *
 * @c GlowRadius becomes a sigma of `max(1.0, radius * 0.48)` half-res texels, and the
 * blur shader clamps that sigma to 16/3.  Radius values below about 2.1 and above about
 * 11.1 therefore have no further effect.
 *
 * @c GlowSamples reaches the CPU fallback only.  The GPU blur uses a fixed 33-tap kernel
 * (one centre tap plus a 16-tap half-kernel each side).
 *
 * @c GlowDivideStrength is independent of `EnableGlow`: any value above 0 enables the
 * divide bracket on its own.  The shader multiplies the strength by
 * smoothstep(0.10, 0.25, luma) of each text pixel, so dark outlines and shadows keep the
 * normal alpha composite at every strength.
 *
 * ## Callback bracket pattern
 *
 * Each pass is a pair of ImDrawCallbacks placed around the affected draws.  The render
 * code adds them through `ImDrawList::AddCallback`, and appends
 * `ImDrawCallback_ResetRenderState` after every End callback so ImGui gets its own
 * shader, blend state, viewport and sampler back:
 *
 * ```cpp
 * TextPostProcess::SetGlowParams(radius, intensity);
 * drawList->AddCallback(TextPostProcess::BeginGlowCapture, nullptr);
 *     // ... AddText / AddTextHorizontalGradient / etc. for glow targets ...
 * drawList->AddCallback(TextPostProcess::EndGlowAndComposite, nullptr);
 * drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
 * ```
 *
 * The color divide pass uses the same shape with `BeginDivideCapture` /
 * `EndDivideAndComposite`.  Only its End side is outermost: the renderer adds
 * `EndDivideAndComposite` after the splitter merge, but appends `BeginDivideCapture` to
 * splitter channel 0 directly after `BeginGlowCapture`.  The execution order of one frame
 * with both passes active is:
 *
 * @verbatim
 * channel 0 : BeginGlowCapture      clear + bind half-res glow RT, half-res
 *                                   viewport
 *             BeginDivideCapture    snapshot the bound RT (the glow RT here),
 *                                   then clear + bind the full-res divide RT
 *             ... glow-target draws ...
 *             EndGlowAndComposite   blur the glow RT, rebind the RT saved by
 *                                   BeginGlowCapture, then composite
 *             ImDrawCallback_ResetRenderState
 * channel 1 : particle draws
 * channel 2 : text, shadow and outline draws
 * (merge)
 *             EndDivideAndComposite rebind the RT saved by BeginDivideCapture,
 *                                   then composite the divide RT
 *             ImDrawCallback_ResetRenderState
 * @endverbatim
 *
 * @warning With both passes active the divide capture nests inside the glow capture, and
 *          the two passes do not compose in the current renderer ordering.  The divide
 *          capture saves the half-res glow RT as its main target, and its copy into the
 *          snapshot texture is invalid, because that texture is full-res and carries the
 *          backbuffer format, so the snapshot keeps stale content.  The divide RT then
 *          takes the glow capture's place for the draws that follow, which leaves the
 *          glow capture empty.
 */
namespace TextPostProcess
{
/**
 * @brief Compile the shaders and create the constant buffers, sampler and pipeline states.
 *
 * Call after the D3D11 device exists.  Render targets are not created here; OnResize
 * creates them.  The module keeps a reference to both interfaces until Shutdown.  It also
 * selects the offscreen target format here: R16G16B16A16_FLOAT when the device supports
 * it as a render target and a sampled texture, otherwise R8G8B8A8_UNORM.
 *
 * On any failure this function releases every partial resource, including the device and
 * context references, and leaves the module uninitialized.  A later call can retry.
 *
 * @param device   D3D11 device for resource creation.
 * @param context  D3D11 immediate context for shader dispatch.
 * @return true when initialization succeeded, or when the module is already initialized.
 *         false when either argument is null, or when a resource creation step failed.
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/**
 * @brief Report whether the shader-side resources exist.
 *
 * @return true when the shaders, constant buffers and pipeline states exist.  Render
 *         targets come from OnResize; without them both bracket pairs are silent no-ops,
 *         so a true result alone does not guarantee a working pass.
 */
bool IsInitialized();

/**
 * @brief Release all GPU resources.
 *
 * Releases the render targets, the shaders, the pipeline states and the device and
 * context references.  Safe to call multiple times or when not initialized.  Draw
 * callbacks that are already queued in a draw list test for the released state and return
 * early, so a mid-frame shutdown never dispatches with released resources.  A shutdown
 * between a Begin and its End callback still leaves the capture target bound for the rest
 * of the frame, because the End callback that would rebind the saved target returns early.
 */
void Shutdown();

/**
 * @brief Create the render targets, or recreate them after a size change.
 *
 * Creates the half-res glow pair, the full-res divide target and the backbuffer
 * snapshot.  No-op when the module is not initialized, when either argument is 0, or
 * when the size is unchanged.
 *
 * Call while the game's main render target is bound.  The snapshot texture must match the
 * backbuffer for CopyResource, so this function reads the format from the bound render
 * target and falls back to R8G8B8A8_UNORM when nothing is bound.
 *
 * The requested size is recorded before creation, so a creation failure is not retried
 * until the size changes again.  A failure only logs: the glow pair and the divide
 * resources fail independently, and each bracket whose targets are missing degrades to a
 * no-op instead of disturbing the frame.
 *
 * @param width   Backbuffer width in pixels.
 * @param height  Backbuffer height in pixels.
 */
void OnResize(uint32_t width, uint32_t height);

/**
 * @brief Set glow parameters for the current frame.
 *
 * Call once per frame before the glow bracket is added.  The values are stored in
 * unsynchronized file-scope state and read later by EndGlowAndComposite.  Neither value
 * is clamped here; Settings clamps GlowIntensity to [0, 1] and GlowRadius to 0 or above.
 *
 * @param radius     Blur radius in pixels (maps to Gaussian sigma).  Values outside
 *                   roughly [2.1, 11.1] have no further effect, because sigma is
 *                   max(1.0, radius * 0.48) half-res texels and the shader clamps it to
 *                   16/3.  One half-res texel spans two backbuffer pixels.
 * @param intensity  Composite intensity.  The composite constants assume [0, 1]; see the
 *                   glow pipeline section for the mapping.  Higher values extrapolate.
 */
void SetGlowParams(float radius, float intensity);

/**
 * @brief ImDrawCallback: bind the half-res glow capture RT and clear it.
 *
 * Add to the draw list before the glow text draws.  The callback saves the bound render
 * target, depth-stencil view and viewport, then binds the glow target with no
 * depth-stencil view and installs a half-res viewport, so every draw inside this bracket
 * rasterizes at half resolution until another callback binds a different target
 * (BeginDivideCapture does).  EndGlowAndComposite restores the saved state.
 *
 * No-op when the module has no context or no glow target.  Nothing is saved in that case,
 * and EndGlowAndComposite is a no-op as well, so the frame renders unchanged.
 *
 * @param dl   Draw list that owns the command (unused).
 * @param cmd  Draw command that carries the callback (unused).
 */
void BeginGlowCapture(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * @brief ImDrawCallback: blur the glow capture and composite it onto the saved target.
 *
 * Add to the draw list after all glow text draws.  The pass blurs the capture with a
 * separable Gaussian (horizontal into the second target, vertical back into the first),
 * then restores the render target and viewport saved by BeginGlowCapture and composites
 * onto them with the screen-style blend state described in the glow pipeline section, not
 * with additive blending.  The saved state is consumed, so a second call in the same frame
 * is a no-op.
 *
 * The blur and the composite also run when nothing drew into the capture.  The result is
 * transparent and the frame is unchanged, but the three fullscreen passes still cost GPU
 * time every frame the bracket is present.
 *
 * @param dl   Draw list that owns the command (unused).
 * @param cmd  Draw command that carries the callback (unused).
 *
 * @post The caller must append ImDrawCallback_ResetRenderState immediately after this
 *       callback; it restores ImGui's own shader, blend state, viewport and sampler.
 */
void EndGlowAndComposite(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * @brief Set color divide parameters for the current frame.
 *
 * Call once per frame before the divide bracket is added.  The value is stored in
 * unsynchronized file-scope state and read later by EndDivideAndComposite.
 *
 * @param strength  Blend strength [0, 1].  The shader multiplies it by
 *                  smoothstep(0.10, 0.25, luma) per text pixel, so dark pixels keep the
 *                  normal alpha composite.
 */
void SetDivideParams(float strength);

/**
 * @brief ImDrawCallback: snapshot the bound RT, then bind and clear the divide RT.
 *
 * Add to the draw list before the nametag draws.  The callback saves the bound render
 * target, depth-stencil view and viewport, copies the bound target into the snapshot
 * texture, then binds the full-res divide target with a full-res viewport.  The copy
 * needs the same size and format on both sides, which holds only while the backbuffer is
 * the bound target; see the warning in the namespace block for the nested case.
 *
 * No-op when the module has no context, no divide target or no snapshot texture.  Nothing
 * is saved in that case, and EndDivideAndComposite is a no-op as well.
 *
 * @param dl   Draw list that owns the command (unused).
 * @param cmd  Draw command that carries the callback (unused).
 */
void BeginDivideCapture(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * @brief ImDrawCallback: composite the captured nametag RT onto the saved render target.
 *
 * Uses a Photoshop-style color divide blend: the snapshot color divided by the captured
 * text color.  Add after all nametag draws, after the splitter merge.  The callback first
 * restores the render target and viewport saved by BeginDivideCapture, then draws with
 * blending disabled: the shader discards captured pixels with alpha below 0.001 and
 * writes every other pixel opaque, so it replaces the destination wherever text exists.
 * The saved state is consumed, so a second call in the same frame is a no-op.
 *
 * @param dl   Draw list that owns the command (unused).
 * @param cmd  Draw command that carries the callback (unused).
 *
 * @post The caller must append ImDrawCallback_ResetRenderState immediately after this
 *       callback.
 */
void EndDivideAndComposite(const ImDrawList* dl, const ImDrawCmd* cmd);
}  // namespace TextPostProcess
