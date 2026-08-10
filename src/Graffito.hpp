#pragma once

#include <RE/N/NiPoint3.h>

#include <array>

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace Graffito
 * @brief Perspective-correct ImGui geometry attached to a world-space plane.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Rendering
 *
 * ImGui transforms every `ImDrawVert::pos` through one orthographic matrix,
 * which makes nameplates camera-facing. Graffito keeps the ImDrawVert layout and
 * temporarily replaces only the vertex shader: the source position is read as a
 * coordinate on an arbitrary world plane and transformed by a projective
 * homography with a real homogeneous `w`, so font UVs receive hardware
 * perspective-correct interpolation. The same vertex shader also applies the
 * InkMaterial treatment to the vertex color, so no extra pass is needed for it.
 *
 * ApplyCallback and RestoreCallback save and restore only the vertex shader and
 * VS constant-buffer slot 0. They never touch the active viewport, render
 * target, pixel shader, blend state, or rasterizer state, so they compose with
 * the glow capture and DepthClip callbacks.
 *
 * Every function here runs on the render thread and on one D3D11 immediate
 * context. Initialize, Shutdown, BeginFrame, BuildProjection and
 * MakeCallbackParams are called while the frame is being built; ApplyCallback
 * and RestoreCallback run later on the same thread, when the ImGui backend
 * renders the draw data. BuildProjection reads `RE::Main::WorldRootCamera()`,
 * which makes it one of the few render-thread functions that touch game state.
 * That read is null-guarded, so it fails closed.
 *
 * All plates share one vertex-shader constant buffer, and RestoreCallback
 * rebinds only the saved buffer pointer, not its contents. Apply/Restore
 * brackets must therefore not nest: close each bracket, or confine it to its
 * own ImDrawListSplitter channel, before the next ApplyCallback runs.
 */
namespace Graffito
{
/**
 * @brief Maximum number of chord strips in one plate.
 *
 * Three locations contain this value
 * and must change together: this constant,
 * `Graffito::ShaderContract::MAX_SEGMENTS`, and the
 * `float4 segments[N]` array in the
 * `kPlaneVS` HLSL source. A static assertion checks the two
 * C++ constants. Nothing checks the
 * HLSL array size, where N is `SEGMENT_FLOAT4S *
 * MAX_SEGMENTS`.
 */
inline constexpr std::size_t MAX_SEGMENTS = 12;

/**
 * @struct CylinderWrap
 * @brief Cylindrical source-space wrap applied to an inscription.
 *
 * The readable midpoint stays at the arc apex while the wings turn away on
 * concentric chord planes. A segment count of one, or zero curvature, preserves
 * the flat-plane projection exactly.
 */
struct CylinderWrap
{
    float sourceCenterX = .0f;  ///< Absolute ImDraw X of the arc apex.
    /// Source-space curvature. A value at or below 1e-9 keeps the flat plane; a
    /// negative value makes BuildProjection return false.
    float radiansPerPixel = .0f;
    /// Radius of this concentric surface, in world units. Horizontal extent is
    /// radius * sin(theta), so this value also sets the physical width: the front
    /// surface must use worldUnitsPerPixel / radiansPerPixel, and each concentric
    /// layer adds its own normal offset to that front radius. Zero keeps the flat
    /// plane; a negative value makes BuildProjection return false.
    float surfaceRadius = .0f;
    /// Chord count. It must be 1 or more, because zero or less makes
    /// BuildProjection return false. A value above MAX_SEGMENTS is clamped down.
    int segmentCount = 1;
};

/**
 * @struct FisheyeWarp
 * @brief Row-local barrel magnification layered over the cylindrical surface.
 *
 * The row endpoints keep their horizontal position while glyphs expand near the
 * midpoint and compress toward the ends. Vertical scale is a separate term about
 * center.y: at full strength the row is 1.18 times its height at the midpoint
 * and .74 times at either endpoint.
 *
 * A zero strength, or a zero reciprocal half-width, disables the warp. Only the
 * CPU mirror is then bit-exact: the shader still evaluates the vertical
 * recentring as `c + (y - c) * 1.0`, which is not bit-exact for a large center.
 */
struct FisheyeWarp
{
    ImVec2 center{};           ///< Absolute source-space center of this typography row.
    float invHalfWidth = .0f;  ///< Reciprocal row half-width; zero disables the warp.
    /// Normalized magnification strength. The value is validated, not clamped: a
    /// value below 0 or above 1 makes MakeCallbackParams return nullptr, so the
    /// plate is not drawn at all.
    float strength = .0f;
};

/**
 * @struct WorldPlane
 * @brief Plane placement expressed in game-world units.
 *
 * The two axes need not be unit length or exactly orthogonal. BuildProjection
 * normalizes right first, then re-orthogonalizes up against it.
 */
struct WorldPlane
{
    RE::NiPoint3 origin{};           ///< World point corresponding to sourceAnchor.
    RE::NiPoint3 right{};            ///< Page-right axis when viewed from the readable side.
    RE::NiPoint3 up{};               ///< Page-up axis.
    float worldUnitsPerPixel = .0f;  ///< World units per source pixel; must be above 0.
    CylinderWrap wrap{};             ///< Optional cylindrical wrap; zero curvature stays flat.
};

/**
 * @struct SourceBounds
 * @brief Absolute ImDraw coordinate bounds that the plane must contain.
 *
 * The bounds must have a positive extent on both axes. A zero-width or
 * zero-height bound makes BuildProjection return false instead of projecting a
 * degenerate quad.
 */
struct SourceBounds
{
    ImVec2 min{};  ///< Top-left corner, in absolute ImDraw coordinates.
    ImVec2 max{};  ///< Bottom-right corner; must be strictly greater than min on both axes.
};

/**
 * @struct InkMaterial
 * @brief View-dependent color treatment applied uniformly to all projected ink.
 *
 * The four values are validated, not clamped: desaturation, opacity and
 * edgeSheen must be in [0, 1] and brightness must be 0 or more. Any other value
 * makes MakeCallbackParams return nullptr, so the plate is not drawn. The shader
 * clamps the same values a second time, with saturate on desaturation, opacity
 * and edgeSheen and a lower bound of 0 on brightness, but that path is never
 * reached with out-of-range input. Brightness has no upper bound on either side,
 * so a value above 1 overbrightens on purpose.
 */
struct InkMaterial
{
    float desaturation = .0f;  ///< Blend RGB toward luminance [0, 1].
    float brightness = 1.0f;   ///< RGB multiplier after desaturation.
    float opacity = 1.0f;      ///< Uniform alpha multiplier [0, 1].
    /// Ribbon-wing highlight mixed toward white [0, 1]. It is inert on a flat
    /// plate, because the flat solve sets the wing band width to zero and the
    /// shader multiplies the sheen by that band weight.
    float edgeSheen = .0f;
};

/**
 * @struct DepthPlane
 * @brief Viewport-depth equation over normalized top-left screen coordinates.
 *
 * `depth = xSlope*x01 + ySlope*y01 + constant`. A planar surface can pass this
 * directly to DepthClip, so angled ink is occluded per pixel instead of at one
 * anchor depth.
 */
struct DepthPlane
{
    float xSlope = .0f;     ///< Depth change per unit of normalized screen x.
    float ySlope = .0f;     ///< Depth change per unit of normalized screen y, y downward.
    float constant = 1.0f;  ///< Depth at the top-left screen corner.

    /**
     * @brief Evaluate the plane at normalized screen coordinates.
     *
     * @param x01
     * Normalized screen X coordinate.
     * @param y01  Normalized screen Y coordinate.
     *
     * @return     The plane value at the specified point.
     */
    float Sample(float x01, float y01) const { return xSlope * x01 + ySlope * y01 + constant; }
};

/**
 * @struct Projection
 * @brief Fully resolved per-plate projection.
 *
 * MakeCallbackParams copies it, and ApplyCallback packs it into the shader
 * constant buffer. Every source coordinate stored here is an absolute ImDraw
 * coordinate; the shader subtracts sourceAnchor first, so each homography maps
 * plane-local pixels.
 */
struct Projection
{
    /**
     * @struct Segment
     * @brief Exact homography and depth plane for one chord strip.

     */
    struct Segment
    {
        /// Row-major local-pixel -> NDC homogeneous homography.
        std::array<float, 9> homography{1.0f, .0f, .0f, .0f, 1.0f, .0f, .0f, .0f, 1.0f};
        DepthPlane depth{};  ///< Exact viewport depth over this chord.
    };

    /// Chord projections in ascending source X. Entries at or past segmentCount
    /// are unused and stay default-constructed.
    std::array<Segment, MAX_SEGMENTS> segments{};
    ImVec2 sourceAnchor{};       ///< Absolute ImDrawVert position treated as plane-local (0,0).
    int segmentCount = 1;        ///< Number of valid chord projections.
    float firstBoundaryX = .0f;  ///< Absolute source X at the first chord boundary.
    float invStride = .0f;       ///< Reciprocal source width of one chord; 0 on a flat plate.
    float wingCenterX = .0f;     ///< Absolute source X of the sheen/wrap midpoint.
    /// Reciprocal half-width; zero disables wing sheen. The flat solve always
    /// writes zero here, so only a wrapped plate shows the sheen.
    float wingInvHalfWidth = .0f;
    /// Row-local midpoint magnification. BuildProjection always resets this field
    /// to {}; the caller assigns it after a successful solve, so the warped
    /// positions are outside the projection accuracy preflight.
    FisheyeWarp fisheye{};
    /// Whole-surface depth fit passed to DepthClip. It is exact only on the flat
    /// path. On a wrapped plate it is a least-squares fit whose worst residual is
    /// computed and then discarded, so it carries no accuracy guarantee.
    DepthPlane plateDepth{};
    bool valid = false;  ///< True only after BuildProjection succeeded.
};

/**
 * @brief Compile the plane vertex shader and allocate its constant buffer.
 *
 * The shader source is compiled at run time with D3DCompile, target vs_5_0. On
 * success Graffito keeps a reference to the device and to the context until
 * Shutdown. A call made while Graffito is already initialized returns true and
 * ignores both arguments, so call Shutdown first after a device change. A failed
 * call retains nothing and leaves the whole module inert, which lets the caller
 * retry on a later frame. A failed shader or buffer creation logs a warning; a
 * null argument is silent, and a failed compile logs only the message that the
 * compiler returned.
 *
 * @param device   Device that creates the shader and the constant buffer.
 * @param context  Immediate context used by ApplyCallback and RestoreCallback.
 * @return true when the shader and the constant buffer exist.
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/**
 * @brief Release all GPU resources and pending callback state.
 *
 * Safe to call repeatedly and at any point in a frame. Pointers handed out by
 * MakeCallbackParams dangle afterwards, but a callback still queued in a draw
 * list is inert: both callbacks return at once while the immediate context is
 * absent, so neither dereferences its user data.
 */
void Shutdown();

/// @brief True after successful Initialize and before Shutdown.
bool IsInitialized();

/**
 * @brief Clear the callback-parameter arena and the saved-state stack.
 *
 * Call once before constructing any Graffito draw commands for a frame. It is
 * safe before Initialize. Clearing the stack also drops a bracket left open by
 * the previous frame: such a bracket keeps the Graffito shader bound for the
 * rest of its own frame, but it cannot make the next frame's RestoreCallback
 * pop a foreign state.
 */
void BeginFrame();

/**
 * @brief Resolve a world plane against the current world camera.
 *
 * Flat planes project the four corners of `sourceBounds`. Wrapped planes split
 * the bounds into vertical chord strips, project their shared rulings through
 * `NiCamera::WorldPtToScreenPt3`, and solve one exact homography and depth plane
 * per strip. A missing camera, a degenerate or edge-on plane, a behind-camera
 * bound, or a non-finite solve returns false and leaves @p out invalid.
 *
 * A wrap is attempted only when the curvature is above 1e-9, the surface radius
 * is above 0, and
 * the clamped strip count is 2 or more. The fallback ladder is
 * fixed: the requested strip count,
 * then one retry at half that count when the
 * half is still 2 or more, then the exact flat path.

 * *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart TD
 *
 * A[Clear output and validate inputs] --> V{Inputs, basis, and camera valid?}
 *     V -- No -->
 * X[Return false; output stays invalid]
 *     V -- Yes --> W{Valid wrapped request with at least
 * two strips?}
 *     W -- No --> F[Try the exact flat solve]
 *     W -- Yes --> R[Try the
 * requested strip count]
 *     R --> P{Projection and preflight pass?}
 *     P -- Yes -->
 * S[Return the valid projection]
 *     P -- No --> H{Half the count is at least two?}
 *     H --
 * Yes --> T[Retry once at half the count]
 *     H -- No --> F
 *     T --> Q{Projection and
 * preflight pass?}
 *     Q -- Yes --> S
 *     Q -- No --> F
 *     F --> G{Flat projection and
 * preflight pass?}
 *     G -- Yes --> S
 *     G -- No --> X
 * ```
 *
 * A candidate is accepted
 * only after an accuracy preflight, not only after a
 * finiteness check. The CPU mirror in
 * GraffitoShaderContract.hpp is evaluated at the plate anchor on the flat path, and at the
 * horizontal midpoint of the first, anchor-owning and last strip on the wrapped path. The candidate
 * is rejected when the mirrored position differs from the camera projection by more than 2e-4 NDC
 * units, or the mirrored depth by more than 5e-4 viewport-depth units. A wrapped candidate is also
 * rejected when one strip projects with the opposite winding to the first strip, which indicates a
 * fold.
 *
 * ImDraw source +Y points downward, so it maps toward `-plane.up`.
 *
 * Render-thread only. Like Renderer's WorldToScreen, it reads the world root
 * camera from the render thread. The read is null-guarded, so a missing camera
 * fails the call instead of faulting.
 *
 * @pre `plane`.right and `plane`.up need not be normalized or exactly
 *      orthogonal. `right` is normalized first, then `up` is re-orthogonalized
 *      against it, so `right` wins any disagreement. The readable normal is
 *      `right` cross `up`.
 * @pre `plane`.worldUnitsPerPixel must be greater than 0, so a
 *      default-constructed WorldPlane always fails.
 *
 * @param plane         World placement, physical scale, and optional cylindrical
 *                      wrap for this surface.
 * @param sourceAnchor  Absolute ImDraw position treated as plane-local (0, 0).
 * @param sourceBounds  Absolute ImDraw bounds the projection must cover.
 * @param out           Receives the resolved projection. It is cleared first, and
 *                      the fisheye field is left at {} for the caller to assign.
 * @return true when a flat or wrapped solve succeeded; false leaves @p out
 *         invalid.
 *
 * @post On success `out.segmentCount` can be lower than the requested strip
 *       count, because of the retry or the flat fallback. Compare the two when
 *       several surfaces must share one strip layout.
 */
bool BuildProjection(const WorldPlane& plane,
                     const ImVec2& sourceAnchor,
                     const SourceBounds& sourceBounds,
                     Projection& out);

/**
 * @brief Copy projection/material constants into stable callback storage.
 *
 * The returned pointer stays valid until the next BeginFrame or Shutdown, both
 * of which clear the arena. Pass it as the user data of ImDrawList::AddCallback
 * with ApplyCallback. Each call appends one entry and nothing reclaims entries
 * inside a frame, so the arena grows with the number of bracketed surfaces.
 *
 * @param projection  Resolved plate projection to copy.
 * @param material    Ink treatment to copy.
 * @return Pointer suitable for ImDrawList::AddCallback, or nullptr when Graffito
 *         is not initialized or the projection or material fails validation.
 */
void* MakeCallbackParams(const Projection& projection, const InkMaterial& material);

/**
 * @brief Bind the Graffito VS for subsequent commands, preserving the previous VS state.
 *
 * On success it writes this plate's constants into the shared dynamic buffer
 * with a discard map, saves the bound vertex shader and VS constant-buffer slot
 * 0, then binds the Graffito shader and that buffer. All plates share the one
 * buffer, so the written constants stay in effect only until the next
 * ApplyCallback overwrites them.
 *
 * On any recoverable failure - Graffito not initialized, missing
 * UserCallbackData, invalid params, or a failed constant-buffer Map - the
 * previous vertex shader stays bound. That shader is normally ImGui's
 * orthographic one, so the bracketed geometry still draws flat and
 * camera-facing instead of being skipped. A balanced inactive marker is still
 * pushed, so RestoreCallback stays paired.
 *
 * The one case that pushes no marker is a missing immediate context: before
 * Initialize, or after Shutdown. RestoreCallback returns on the same condition,
 * so the pair stays balanced there as well.
 *
 * @param drawList  Draw list issuing the callback. It is not used.
 * @param command   Draw command whose UserCallbackData holds the params from
 *                  MakeCallbackParams.
 */
void ApplyCallback(const ImDrawList* drawList, const ImDrawCmd* command);

/**
 * @brief Restore the vertex shader and VS constant-buffer slot 0 of one bracket.
 *
 * It pops one marker. A marker pushed by a failed ApplyCallback restores
 * nothing, and a call with an empty stack does nothing. The marker keeps the
 * pairing, so a failed ApplyCallback cannot make this call restore the state of
 * an earlier bracket. Only the buffer binding is restored, never the buffer
 * contents.
 *
 * @param drawList  Draw list issuing the callback. It is not used.
 * @param command   Draw command. It is not used.
 */
void RestoreCallback(const ImDrawList* drawList, const ImDrawCmd* command);
}  // namespace Graffito
