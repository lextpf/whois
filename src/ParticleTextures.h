#pragma once

#include <d3d11.h>
#include <imgui.h>
#include <string>

/**
 * @namespace ParticleTextures
 * @brief Particle texture management for sprite-based effects.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Rendering
 *
 * Loads PNG textures from subfolders and creates D3D11 shader resource views
 * for use with ImGui textured quads. Each particle type can have multiple
 * textures loaded from its folder, with consistent per-particle selection
 * to avoid flickering.
 *
 * ## :material-folder-image: Folder Structure
 *
 * Textures are loaded from `Data/SKSE/Plugins/whois/particles/<type>/`:
 *
 * | Subfolder | Style Index | Description          |
 * |-----------|:-----------:|----------------------|
 * | `stars/`  | 0           | Star sparkle sprites |
 * | `sparks/` | 1           | Spark effect sprites |
 * | `wisps/`  | 2           | Wisp glow sprites    |
 *
 * ## :material-image-filter-hdr: Texture Pipeline
 *
 * ```mermaid
 * %%{init: {'theme':'dark', 'look':'handDrawn'}}%%
 * flowchart LR
 *     classDef io fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef process fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef render fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     A[PNG Files]:::io --> B[stb_image Load]:::process
 *     B --> C[ID3D11Texture2D]:::process
 *     C --> D[Shader Resource View]:::process
 *     D --> E[ImGui Textured Quad]:::render
 * ```
 *
 * ## :material-dice-multiple-outline: Texture Selection
 *
 * Each particle is assigned a texture deterministically using its index
 * to avoid random flickering across frames:
 *
 * $$\text{texture} = \text{particleIndex} \bmod \text{textureCount}$$
 */
namespace ParticleTextures
{
    /**
     * Initialize particle textures using the D3D11 device.
     * Scans subfolders and loads all PNG textures.
     * Call after D3D11 device is created and ImGui is initialized.
     *
     * @param device D3D11 device to create textures on
     * @return true if at least one texture loaded successfully
     */
    bool Initialize(ID3D11Device* device);

    /**
     * Release all loaded particle textures.
     * Call before D3D11 device is released.
     */
    void Shutdown();

    /**
     * Check if particle textures have been loaded.
     * @return true if textures are available for use
     */
    bool IsInitialized();

    /**
     * Get the number of loaded textures for a particle type.
     * @param style Particle style index
     * @return Number of textures available (0 if none)
     */
    int GetTextureCount(int style);

    /**
     * Get the first texture ID for a particle type.
     * @param style Particle style to get texture for
     * @return ImTextureID (D3D11 SRV) or empty if not loaded
     */
    ImTextureID GetTexture(int style);

    /**
     * Get a texture based on particle index.
     * Uses particle index to select consistently (no flickering).
     * @param style Particle style
     * @param particleIndex Index of the particle
     * @return ImTextureID or empty if not loaded
     */
    ImTextureID GetRandomTexture(int style, int particleIndex);

    /**
     * Get the size of a loaded texture.
     * @param style Particle style
     * @param outWidth Output width in pixels
     * @param outHeight Output height in pixels
     * @return true if texture exists and size was retrieved
     */
    bool GetTextureSize(int style, int& outWidth, int& outHeight);

    /**
     * Draw a textured particle sprite.
     * Uses the first texture for the style.
     *
     * @param list ImGui draw list
     * @param center Center position of the sprite
     * @param size Size of the sprite (width and height)
     * @param style Particle style (determines which texture folder)
     * @param color Tint color (white = no tint)
     * @param rotation Rotation angle in radians
     */
    void DrawSprite(ImDrawList* list, const ImVec2& center, float size,
                    int style, ImU32 color, float rotation = 0.0f);

    /**
     * Draw a textured particle sprite with specific particle index.
     * Selects texture based on particle index for variety.
     *
     * @param list ImGui draw list
     * @param center Center position of the sprite
     * @param size Size of the sprite (width and height)
     * @param style Particle style (determines which texture folder)
     * @param particleIndex Index of the particle
     * @param color Tint color (white = no tint)
     * @param rotation Rotation angle in radians
     */
    void DrawSpriteWithIndex(ImDrawList* list, const ImVec2& center, float size,
                             int style, int particleIndex, ImU32 color, float rotation = 0.0f);
}
