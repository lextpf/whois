#pragma once

#include <d3d11.h>
#include <imgui.h>
#include <string>

/**
 * @namespace ParticleTextures
 * @brief Particle texture management for sprite-based effects.
 * @author Alex (https://github.com/lextpf)
 * @ingroup ParticleTextures
 *
 * Loads PNG sprites per particle style and creates D3D11 shader resource views for ImGui
 * textured quads. Per-particle texture selection is deterministic, so a particle keeps the
 * same sprite across frames.
 *
 * All functions are render-thread only. Initialize and Shutdown serialize on an internal
 * mutex. The draw and query functions take no lock: they read the loaded texture tables and
 * the device, context, sampler, blend-state and callback-stack statics that only the render
 * thread may touch. A Shutdown from any other thread therefore races every query and draw.
 *
 * ## :material-folder-image: Sprite Files
 *
 * Sprite files are GUID-named and are resolved through the obfuscated asset manifest. There
 * is no on-disk scan and no filename contract. `ProjectManifest::ParticleVariants(token)`
 * returns the file list that `glyph.project.json` maps for a style token under `particles`,
 * and `ProjectManifest::BubblePop()` returns the file mapped under `bubblePop`. The token
 * from `Settings::kParticleStyleTokens` is only the lookup key. A style that ends the load
 * with no texture gets procedurally generated sprites instead (see Quality Pipeline). That
 * covers a style the manifest maps nothing for, a style whose every mapped file failed to
 * decode, and every style when COM or WIC is unavailable.
 *
 * The loader derives the flipbook frame count from the image dimensions:
 *
 * | Mapped image | Meaning                                              |
 * |--------------|------------------------------------------------------|
 * | `16x16`      | Single frame                                         |
 * | `Nx16`       | Horizontal flipbook; the shipped strips are 64x16    |
 *
 * $$\text{frames} = \frac{\text{width}}{\text{height}}$$
 *
 * That rule applies only when the width is an exact multiple of the height and the quotient
 * is more than 1. Any other size counts as one frame and is drawn as a single quad.
 *
 * Frames run left to right. Frame index f takes the U window below and the full V range, so
 * a vertical strip is not a supported layout:
 *
 * $$u_0 = \frac{f}{\text{frames}}, \qquad u_1 = \frac{f + 1}{\text{frames}}$$
 *
 * All mapped variants of a style enter the per-particle variant rotation, so a style with N
 * variants spawns each on ~1/N of its particles, stably across frames. The pop sprite stays
 * outside the rotation.
 *
 * ## :material-image-filter-hdr: Texture Pipeline
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef io fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef process fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef render fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     A[Manifest PNG paths]:::io --> B[WIC decode, alphaGamma bake]:::process
 *     A2[Style left with no texture]:::io --> P[Procedural generator set]:::process
 *     B --> C[ID3D11Texture2D]:::process
 *     P --> C
 *     C --> D[Shader Resource View]:::process
 *     D --> E[ImGui Textured Quad]:::render
 *     D -.->|zero textures in total| Z[Release all, stay uninitialized]:::process
 * ```
 *
 * ## :material-dice-multiple-outline: Texture Selection
 *
 * Each particle takes its texture from a stratified round-robin over the style's variants
 * with a per-style hash offset, so a style with N variants puts each variant on an equal
 * share of its particles (within one) and the choice never changes between frames:
 *
 * $$\text{texture} = (\text{particleIndex} + \text{hash}(\text{style})) \bmod \text{textureCount}$$
 *
 * ## :material-auto-fix: Quality Pipeline
 *
 * A style that ends the load with no texture falls back to procedurally generated 256x256
 * white-on-transparent sprites. Every generated sprite runs through 4x rotated-grid
 * supersampling (anti-aliased line work), interleaved-gradient-noise dithering at 8-bit
 * quantization (no banding in soft glows), and a full CPU-built mip chain (no shimmer under
 * minification). A mapped image whose larger side is above 64 px gets a GPU-generated mip
 * chain at load time for the same reason. Sampling uses a separate threshold: point sampling
 * up to 64 px on the larger side of one frame, linear above it. Every shipped sprite is 16x16
 * or a 64x16 strip, so neither threshold fires for shipped art.
 */
namespace ParticleTextures
{
/**
 * @enum BlendMode
 * @brief Blend state a particle sprite draw selects.
 *
 * These enumerator values are not the INI integer convention, which is 0=Additive, 1=Screen,
 * 2=Alpha. TextEffects::DrawParticleAura remaps the INI integer to this enum before it calls
 * DrawSpriteWithIndex.
 */
enum class BlendMode
{
    /// Bind no blend state: the sprite draws with whatever ImGui has bound, which is standard
    /// alpha blending. This mode emits no blend callback pair.
    Alpha = 0,
    Additive = 1,  ///< dst + src*src_alpha; for bright, glowing particles
    Screen = 2     ///< dst*(1-src_color) + src*src_alpha; softer luminous sprites
};

/**
 * @struct StyleVisibilityTuning
 * @brief Visibility compensation for the art of one particle style.
 *
 * The motion renderer owns the physical particle size. These values only compensate for how
 * much of the source frame the art actually paints; the shipped frames are 16x16. The table
 * is indexed by ParticleStyle ordinal and lives in ParticleTextures.cpp.
 */
struct StyleVisibilityTuning
{
    float coreSizeScale = 1.0f;  ///< Crisp sprite only; does not enlarge the halo

    /// Source-alpha curve; lower is more opaque. Unlike the other two fields, this value is
    /// not read per draw: Initialize bakes it into the texture on decode (pow(alpha, gamma),
    /// applied only when the value is < 1.0) and only for manifest-loaded sprites.
    /// Procedural fallback sprites and already-loaded textures keep their alpha, so a change
    /// takes effect only after Shutdown and a new Initialize.
    float alphaGamma = 1.0f;

    float haloAlphaScale = 1.0f;  ///< Per-style multiplier for the shared halo
};

/**
 * @brief Get visibility tuning for a particle-style ordinal.
 *
 * @param style  Particle-style ordinal.
 * @return       The style tuning, or neutral tuning when the ordinal is invalid.
 */
const StyleVisibilityTuning& GetStyleVisibilityTuning(int style);

/**
 * @brief Initialize particle textures using the D3D11 device.
 *
 * Loads the manifest-mapped sprite variants of every style, procedurally generates sprites
 * for any style left with no texture, and generates the shared soft-glow disc. Call after the
 * D3D11 device is created and ImGui is initialized.
 *
 * The call serializes on an internal mutex. After a successful call it returns true at once
 * and reloads nothing, so it does not notice a new device: call Shutdown first when the D3D11
 * device changes. After a failed call the next call retries the whole load. A null device
 * returns false and changes nothing.
 *
 * When the run loads no texture at all, it releases every resource again, including the
 * shared soft-glow disc, so IsInitialized() and HasSoftGlow() both stay false and every draw
 * function degrades to a no-op.
 *
 * @param device D3D11 device to create textures on
 * @return true if at least one texture loaded successfully
 */
bool Initialize(ID3D11Device* device);

/**
 * @brief Check if particle textures have been loaded.
 *
 * @return true if textures are available for use
 */
bool IsInitialized();

/**
 * @brief Release all particle texture resources.
 *
 * Releases the sprite, pop and soft-glow textures, the samplers and the blend states, clears
 * the pending callback stacks, and drops the cached device and context. Every ImTextureID
 * that GetRandomTexture returned before this call becomes invalid. Safe to call multiple
 * times, and serialized against Initialize on the same mutex.
 */
void Shutdown();

/**
 * @brief Get the number of loaded texture variants for a particle style.
 *
 * The count includes procedural fallback sprites, which enter the same variant rotation as
 * manifest-loaded ones. It excludes the pop sprite and the shared soft-glow disc.
 *
 * @param style ParticleStyle ordinal
 * @return Number of variants available; 0 when the ordinal is out of range or the style
 *         loaded nothing
 */
int GetTextureCount(int style);

/**
 * @brief Get the texture that a particle index selects.
 *
 * Despite the name, selection is deterministic (see Texture Selection above), so a particle
 * keeps the same variant across frames.
 *
 * The result wraps a shader resource view this namespace owns. Shutdown invalidates it, so do
 * not cache it across frames.
 *
 * @param style ParticleStyle ordinal
 * @param particleIndex Caller-assigned particle index. It must stay the same across frames
 *                      for the same particle, or the sprite flickers.
 * @return The selected texture, or an empty ImTextureID when the ordinal is out of range or
 *         the style loaded nothing
 */
ImTextureID GetRandomTexture(int style, int particleIndex);

/**
 * @brief Draw a textured particle sprite with specific particle index.
 *
 * No-op when the style ordinal is out of range or the style loaded no texture. The draw emits
 * a sampler callback pair, and for a non-alpha blend mode a blend callback pair as well. Each
 * pair saves and restores only the state it changes, so an enclosing Graffito vertex shader
 * or DepthClip pixel shader survives the sprite.
 *
 * @param list ImGui draw list
 * @param center Center position of the sprite
 * @param size Requested quad edge in pixels. A source frame larger than
 *             1200 px is scaled down by clamp(1200 / maxFrameDim, 0.45, 1.0)
 *             so high-resolution art does not draw larger than the pixel-art
 *             sprites. Every shipped sprite gives a factor of 1.0, so `size`
 *             is then the on-screen edge. Nothing is drawn when the scaled
 *             edge falls to 0.02 px or below.
 * @param style ParticleStyle ordinal; selects the style's loaded variant set
 * @param particleIndex Caller-assigned particle index; it picks the variant and must stay
 *                      the same across frames for the same particle
 * @param color Tint color (white = no tint)
 * @param blendMode Blend state to use while drawing this sprite
 * @param rotation Rotation angle in radians. Positive turns clockwise on screen, because
 *                 ImGui's y axis points down. Exactly 0 takes an axis-aligned fast path.
 * @param frame Flipbook frame for animated strips (wrapped into range;
 *              ignored for 1-frame statics)
 */
void DrawSpriteWithIndex(ImDrawList* list,
                         const ImVec2& center,
                         float size,
                         int style,
                         int particleIndex,
                         ImU32 color,
                         BlendMode blendMode = BlendMode::Alpha,
                         float rotation = .0f,
                         int frame = 0);

/**
 * @brief Flipbook frame count of the texture the given particle selects.
 *
 * The count comes from the image dimensions (see Sprite Files above).
 *
 * @param style ParticleStyle ordinal
 * @param particleIndex The same index the caller passes to DrawSpriteWithIndex; it picks the
 *                      variant whose frame count is returned
 * @return >= 1; 1 when the selected sprite is a single frame (or nothing
 *         loaded, or the image size does not match the flipbook rule)
 */
int GetFrameCountForIndex(int style, int particleIndex);

/**
 * @brief Whether the style has an end-of-life pop sprite loaded.
 *
 * The pop sprite is the manifest's `bubblePop` entry. Initialize assigns it to the Bubble slot
 * only, so every other style always returns false. Pop sprites stay outside the variant
 * rotation: a pop frame inside the rotation would render a fraction of the particles as
 * permanently mid-pop.
 *
 * @param style ParticleStyle ordinal
 * @return true when a pop sprite is loaded for the style
 */
bool HasPopSprite(int style);

/**
 * @brief Flipbook frame count of the style's pop sprite.
 *
 * The count comes from the image dimensions (see Sprite Files above).
 *
 * @param style ParticleStyle ordinal
 * @return >= 1; 1 when the pop sprite is a single frame, when the style has no pop sprite,
 *         or when the ordinal is out of range
 */
int GetPopFrameCount(int style);

/**
 * @brief Draw the style's pop sprite (see HasPopSprite). No-op when absent.
 *
 * `list`, `center`, `size`, `style`, `color`, `blendMode` and `rotation` have
 * the same meaning as in DrawSpriteWithIndex, which documents them.
 *
 * @param list       ImGui draw list.
 * @param center     Sprite center, in screen pixels.
 * @param size       Sprite edge length, in pixels.
 * @param style      Particle-style ordinal.
 * @param color      Packed sprite tint.
 * @param blendMode  Sprite blend mode.
 * @param rotation   Clockwise rotation, in radians.
 * @param frame      Flipbook frame for an animated pop strip. The value wraps into range.
 *                   Use zero for a static sprite.
 */
void DrawPopSprite(ImDrawList* list,
                   const ImVec2& center,
                   float size,
                   int style,
                   ImU32 color,
                   BlendMode blendMode = BlendMode::Alpha,
                   float rotation = .0f,
                   int frame = 0);

/**
 * @brief Draw the shared soft light disc, additively.
 *
 * The disc is a procedural Gaussian falloff and carries no art, so the glow-halo and glint
 * layer drawn behind and over a crisp sprite never reads as a scaled copy of that sprite.
 * No-op until Initialize has generated the disc.
 *
 * @param list ImGui draw list
 * @param center Center position
 * @param size Quad edge length in pixels. The disc is 256x256, below the 1200 px
 *             normalization threshold, so this is the on-screen edge. The visible glow radius
 *             is about a third of it.
 * @param color Tint color; alpha scales the glow strength
 */
void DrawSoftGlow(ImDrawList* list, const ImVec2& center, float size, ImU32 color);

/**
 * @brief Whether the shared soft light disc is currently drawable.
 *
 * False before Initialize, after a teardown that released the disc SRV (e.g. the
 * no-particle-textures fallback path), and when Initialize loaded sprites but the disc itself
 * failed to generate. Callers that draw a glow via DrawSoftGlow should gate on this and
 * provide their own fallback when false.
 *
 * @return True when the shared soft-light disc can be drawn.
 */
bool HasSoftGlow();

/**
 * @brief Push additive blend state onto the draw list via callback.
 *
 * Draws recorded after this call blend as dst + src*src_alpha, until the matching
 * PopBlendState. The state change happens when ImGui renders the draw list, not when this
 * function is called, so a push and its pop must sit in the same frame and in the same order.
 * When the additive blend state failed to create, the push changes no device state but still
 * pushes a stack entry that PopBlendState must remove.
 *
 * @param dl ImGui draw list
 */
void PushAdditiveBlend(ImDrawList* dl);

/**
 * @brief Push screen blend state onto the draw list via callback.
 *
 * Draws recorded after this call blend as src*src_alpha + dst*(1-src_color), until the
 * matching PopBlendState: colored pixels brighten the background and black pixels stay
 * invisible. The source-alpha factor keeps a transparent sprite border from lifting a
 * rectangle out of the background. Pairing and failure behavior match PushAdditiveBlend.
 *
 * @param dl ImGui draw list
 */
void PushScreenBlend(ImDrawList* dl);

/**
 * @brief Pop the blend state pushed by the matching PushAdditiveBlend or PushScreenBlend.
 *
 * This restores the blend state, blend factor and sample mask that were bound before the
 * matching push. It does not reset to an ImGui default, because a full reset would also
 * discard an enclosing Graffito vertex shader or DepthClip pixel shader. A pop with no
 * matching push is ignored. The restore happens when ImGui renders the draw list.
 *
 * @param dl ImGui draw list
 */
void PopBlendState(ImDrawList* dl);
}  // namespace ParticleTextures
