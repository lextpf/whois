#pragma once

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace RenderSampling
 * @brief Scoped D3D11 samplers for crisp font and badge textures.
 *
 * The helper adds balanced callbacks to ImGui draw lists. A push callback saves pixel-shader
 * sampler slot 0 and binds a quality sampler. A pop callback restores the saved sampler. If
 * sampler creation fails, the add functions do nothing and ImGui keeps its current sampler.
 */
namespace RenderSampling
{
/**
 * @brief Create the font and badge samplers for one D3D11 device.
 *
 * Repeated calls for the same device do not create more resources. Call Shutdown after a
 * device change before this function receives the new device.
 *
 * @param device D3D11 device that creates the sampler states.
 * @param context Immediate context used by draw callbacks.
 * @return True when both quality samplers are ready.
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/// @brief Release sampler resources and discard saved callback state.
void Shutdown();

/// @brief Add a callback that pushes the trilinear font sampler with a limited mip range.
void PushFontSampler(ImDrawList* drawList);

/// @brief Add a callback that pushes the trilinear badge sampler with the full mip range.
void PushBadgeSampler(ImDrawList* drawList);

/// @brief Add a callback that restores the sampler saved by the matching push callback.
void PopSampler(ImDrawList* drawList);
}  // namespace RenderSampling
