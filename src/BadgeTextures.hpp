#pragma once

#include <d3d11.h>
#include <imgui.h>
#include <string>
#include <vector>

/**
 * @namespace BadgeTextures
 * @brief Status badge icon textures rasterized from duotone SVGs.
 * @author Alex (https://github.com/lextpf)
 * @ingroup BadgeTextures
 *
 * Loads the status badge icons referenced by the `[Icons]` settings from a folder of Font
 * Awesome duotone SVGs, rasterizes them via nanosvg, and creates mipmapped D3D11 shader
 * resource views for ImGui textured quads.
 *
 * ## :material-layers: Duotone Pipeline
 *
 * Each Font Awesome duotone SVG carries two `currentColor` paths: a secondary layer at
 * `opacity=".4"` and a fully opaque primary layer. The source `.4` is not used as-is: before
 * rasterization every shape below an opacity floor of 0.80 is lifted to 0.80, because at `.4`
 * half of each icon reads faint. The primary layer at 1.0 is untouched, so the on-screen
 * two-tone ratio is 0.80:1.0. Both layers are rasterized together, so the per-pixel alpha
 * already encodes the two-tone look; the badge's semantic color is applied as a vertex tint at
 * draw time. Icons whose strongest layer is translucent (some glyphs keep all content in the
 * secondary layer) are normalized so their peak alpha is fully opaque.
 *
 * Texels are white with computed alpha; fully transparent texels carry black RGB so
 * screen-style blending cannot lift hidden white into the framebuffer. A full CPU-built mip
 * chain keeps minified badges stable. Status icons use a 256-pixel square source canvas.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart TB
 *     classDef input fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef step fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef output fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     subgraph D[Duotone status icon]
 *         direction LR
 *         S[SVG paths]:::input --> A[Raise low opacity to 0.80]:::step
 *         A --> R[nanosvg raster]:::step
 *         R --> N[Normalize peak alpha when needed]:::step
 *         N --> W[White RGB and computed alpha]:::step
 *         W --> M[CPU mip chain and SRV]:::step
 *         M --> T[ImGui quad with vertex tint]:::output
 *     end
 *
 *     subgraph P[Full-color tier emblem]
 *         direction LR
 *         G[Rank-ordered PNG paths]:::input --> I[WIC decode each file]:::step
 *         I --> C[Trim opaque bounds]:::step
 *         C --> Q[Center and resample to a square]:::step
 *         Q --> U[Mip chain and SRV]:::step
 *         U --> O[ImGui quad with white tint]:::output
 *         I -. one file fails .-> B[Keep its rank slot blank]:::step
 *         I -. no file loads .-> F[Use duotone rank icons]:::output
 *     end
 * ```
 */
namespace BadgeTextures
{
/**
 * @brief Rasterize the given icon names and create their textures.
 *
 * Names resolve to `<folder>/<name>.svg`; empty names are skipped, missing or unparsable
 * files are logged once and skipped.
 *
 * Call on the render thread once the D3D11 device exists. The call replaces the icon set: it
 * drops the previously loaded icons first, also when it then fails, so a reload with a bad
 * folder leaves no icon loaded. No Shutdown() is needed between reloads (settings hot reload
 * calls Initialize directly).
 *
 * @param device D3D11 device to create textures on
 * @param folder SVG folder path (relative to the game directory or absolute)
 * @param names  Icon names to load (duplicates are loaded once)
 * @return true if at least one icon loaded successfully
 */
bool Initialize(ID3D11Device* device,
                const std::string& folder,
                const std::vector<std::string>& names);

/// @brief True after Initialize has run, even when no icon loaded.
bool IsInitialized();

/// @brief Release all badge texture resources.
void Shutdown();

/**
 * @brief Look up a loaded badge texture by icon name.
 *
 * @param name Icon name as configured (e.g. "shield-halved")
 * @return ImTextureID of the icon, or 0 when the icon is not loaded
 */
ImTextureID Get(const std::string& name);

/**
 * @brief Load the full-color prestige emblem PNGs used by actor rank badges.
 *
 * Unlike the duotone SVG icons above (alpha masks tinted at draw time), these are true-color
 * images rendered untinted. `paths` lists the emblem files in rank order (index 0 = lowest
 * rank), resolved from the obfuscated asset manifest. A file that fails to load keeps its rank
 * slot (rendered blank) so the emblem-to-rank alignment is preserved; if *none* load, the set
 * is cleared so the tier badge falls back to the Font Awesome medal/gem/crown icons.
 *
 * Each image is trimmed to its opaque content and resampled into a centered, 512-pixel,
 * mipmapped square so all emblems read at a uniform on-screen size.
 *
 * Call on the render thread once the D3D11 device exists. An empty `paths` clears any loaded
 * emblems. Safe to call again (settings hot reload).
 *
 * @pre COM is initialized on the calling thread; WIC decodes the PNGs. The
 *      call warns and returns 0 otherwise, and the emblems fall back to the
 *      Font Awesome icons.
 * @param device D3D11 device to create textures on
 * @param paths  Emblem file paths in rank order (empty clears the set)
 * @return number of emblem images that loaded successfully
 */
int InitializeTierImages(ID3D11Device* device, const std::vector<std::string>& paths);

/**
 * @brief Get a tier-emblem texture by zero-based index.
 *
 * @param index  Tier-emblem index.
 * @return       The texture, or zero when it is not loaded.
 */
ImTextureID GetTierImage(int index);

/**
 * @brief Get the number of tier-emblem rank slots.
 *
 * A slot whose file failed still counts, and `GetTierImage` returns zero for it. The result
 * can exceed the number that `InitializeTierImages` loaded. This query is lock-free.
 *
 * @return The number of rank slots, or zero when tier emblems are disabled.
 */
int TierImageCount();
}  // namespace BadgeTextures
