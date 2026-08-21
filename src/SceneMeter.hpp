#pragma once

#include <cstdint>

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace SceneMeter
 * @brief Image-space scene sampling that adapts nameplate ink to the scene's light.
 * @author Alex (https://github.com/lextpf)
 * @ingroup TextPostProcess
 *
 * Meters the backbuffer so nameplate ink tracks the light around it instead of sitting
 * at fixed sRGB white: over a bright scene the ink dims slightly, in a dark warm-lit
 * one it lifts a few percent and takes on some of the light's color.
 *
 * ## Pipeline (two frames of latency, never stalls)
 *
 * ```
 * frame N   : capture callback (before any glyph draws)
 *             backbuffer -> mip chain -> tiny mip -> staging ring slot
 * frame N+2 : CollectResults() maps the oldest pending slot (DO_NOT_WAIT)
 *             into a CPU grid; plates bilinear-sample it during layout
 * ```
 *
 * The ring holds 3 slots.  A capture writes slot `counter % 3`, and CollectResults maps
 * slot `(counter + 1) % 3` - the slot written two captures earlier - so the CPU never
 * waits on the GPU.  CollectResults also runs while the draw list is built, whereas the
 * capture
 * callback executes later at ImGui render time, which adds the second frame of
 * latency.
 *
 *
 * @verbatim
 * For capture counter c:
 *
 * draw-list build now    read slot (c + 1) % 3 capture c
 * - 2
 * GPU may still own      pending slot (c + 2) % 3  capture c - 1
 * ImGui execution later
 * write slot c % 3          capture c
 *
 * The three roles rotate by one slot after each capture.

 * * @endverbatim
 *
 * The capture runs before glyph's own draws, so the meter never reads the
 * overlay's own
 * text back.  Any failure - exotic backbuffer format, MSAA, no mip autogen -
 * disables the feature with one log line and no visual change.  The failure latches:
 * `IsInitialized` returns false from then on, even when the backbuffer format later changes back to
 * a supported one.  Only `Initialize` or `Shutdown` clears the latch, so a resize alone does not
 * re-enable the feature.
 */
namespace SceneMeter
{
/**
 * @brief Store the device references and clear the latched failure flag.
 *
 * Resources themselves are (re)created lazily on the first capture, so a backbuffer
 * format change (ENB, upscalers) is handled there.
 *
 * @param device   D3D11 device for resource creation.
 * @param context  D3D11 immediate context for the copies and the readback.
 * @return true when both pointers are valid.
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/// @brief True when the scene meter is initialized and usable.
bool IsInitialized();

/// @brief Release all GPU resources.
void Shutdown();

/**
 * @brief Invalidate the cached resources after a backbuffer size change.
 *
 * Releases nothing here: the next capture rebuilds the resources and releases the old
 * ones.  A format-only change is ignored here and detected in CaptureCallback instead.
 *
 * @param width   New backbuffer width in pixels.
 * @param height  New backbuffer height in pixels.
 */
void OnResize(uint32_t width, uint32_t height);

/**
 * @brief ImDrawCallback: downsample the current backbuffer into the staging ring.
 *
 * Add as the FIRST callback of the overlay draw list, before any glyph content, so the
 * sample excludes the overlay itself.  Alters no pipeline state (copies and
 * GenerateMips only), so no ResetRenderState is needed.
 *
 * @param dl   Draw list that owns the command (unused).
 * @param cmd  Draw command that carries the callback (unused).
 */
void CaptureCallback(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * @brief Map the oldest pending staging texture into the CPU grid, without blocking.
 *
 * Call once per frame from the render thread before plates sample.  Keeps the previous
 * grid when the slot is still in use by the GPU.
 */
void CollectResults();

/**
 * @brief Bilinear-sample the metered scene at normalized screen coordinates.
 *
 * @param x01          Normalized viewport x in [0, 1]; outside values clamp.
 * @param y01          Normalized viewport y in [0, 1]; outside values clamp.
 * @param[out] outLum  Rec.709 luminance of the sampled region [0,1].
 * @param[out] outRGB  Average color of the sampled region (clamped [0,1]).
 * @return false while no results are available yet (feature warms up).  Both outputs
 *         stay untouched in that case.
 */
bool Sample(float x01, float y01, float& outLum, float outRGB[3]);
}  // namespace SceneMeter
