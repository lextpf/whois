#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <type_traits>

/**
 * @namespace Graffito::ShaderContract
 * @brief Shared CPU/HLSL ABI and reference transform for Graffito vertices.
 * @author Alex (https://github.com/lextpf)
 *
 * The field order and arithmetic mirror `kPlaneVS` in Graffito.cpp. The
 * static_asserts below and the unit tests pin the CPU-side packing and struct
 * layout only. `kPlaneVS` is a hand-maintained mirror compiled at runtime: edit
 * both sides together, because nothing in the repo cross-checks them. HLSL drift
 * in row order, the Y flip, the depth dot product, the cbuffer field order, the
 * `float4 segments[48]` size, or the shift by two that hard-codes the
 * SEGMENT_FLOAT4S stride reaches the game unreported.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef input fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef step fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef output fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     K[PackSegments]:::step --> C[Constants]:::input
 *     P[Absolute ImDraw position]:::input --> F[EvaluateFisheyePosition]:::step
 *     C --> F
 *     F --> S[EvaluateSegmentSlot<br/>from warped X]:::step
 *     C --> S
 *     F --> A[Subtract sourceAnchor]:::step
 *     C --> A
 *     S --> H[Selected homography<br/>and depth rows]:::step
 *     C --> H
 *     A --> H
 *     H --> V[ClipPosition<br/>before the divide]:::output
 *     P --> W[EvaluateWingWeight<br/>from unwarped X]:::step
 *     C --> W
 *     I[ImDraw vertex color]:::input --> E[EvaluateColor]:::step
 *     C --> E
 *     W --> E
 *     E --> O[Treated vertex color]:::output
 * ```
 */
namespace Graffito::ShaderContract
{
/**
 * @brief Maximum number of chord strips in one plate.
 *
 * This value must equal `Graffito::MAX_SEGMENTS`. A static assertion checks that pair. No
 * assertion checks the HLSL array size.
 */
inline constexpr std::size_t MAX_SEGMENTS = 12;
/**
 * @brief Number of float4 rows stored for each chord.
 *
 * Each chord contains three homography rows and one depth-plane row. `kPlaneVS` hard-codes
 * the same stride as a shift by two. No check validates that shader expression.
 */
inline constexpr std::size_t SEGMENT_FLOAT4S = 4;

/**
 * @struct SegmentData
 * @brief Shader payload for one chord strip before float4 packing.
 */
struct SegmentData
{
    std::array<float, 9> homography{};  ///< Row-major local-pixel -> NDC homogeneous map.
    std::array<float, 3> depthPlane{};  ///< xSlope, ySlope, constant over normalized screen.
};

/**
 * @struct Constants
 * @brief CPU image of the GraffitoCB buffer bound at vertex-shader slot b0.
 *
 * @verbatim
 * byte  0  sourceAnchor    = anchorX, anchorY, wingCenterX, wingInvHalfWidth
 * byte 16  segmentParams   = firstBoundaryX, invStride, maxSlot, unused
 * byte 32  materialParams  = desaturation, brightness, opacity, edgeSheen
 * byte 48  fisheyeParams   = centerX, centerY, invHalfWidth, strength
 * byte 64  segments[4i+0]  = homography row 0 in xyz, w unused
 *          segments[4i+1]  = homography row 1 in xyz, w unused
 *          segments[4i+2]  = homography row 2 in xyz, w unused
 *          segments[4i+3]  = xSlope, ySlope, constant, w unused
 * @endverbatim
 *
 * Chord i starts at byte 64 + 64 * i, and rows past the packed chord count stay
 * zero. Every field is a float4 lane group, so the total size stays a multiple
 * of 16, which D3D11 requires of a constant buffer.
 */
struct alignas(16) Constants
{
    /// Lanes: anchorX, anchorY, wingCenterX, wingInvHalfWidth. Lanes 2 and 3 are
    /// the wing band, not part of the anchor.
    std::array<float, 4> sourceAnchor{};
    /// Lanes: firstBoundaryX, invStride, maxSlot, unused. maxSlot is
    /// segmentCount - 1, not the count.
    std::array<float, 4> segmentParams{};
    /// Lanes: desaturation, brightness, opacity, edgeSheen.
    std::array<float, 4> materialParams{};
    std::array<float, 4> fisheyeParams{};  ///< Lanes: centerX, centerY, invHalfWidth, strength.
    /// SEGMENT_FLOAT4S rows per segment: three homography rows in xyz, then the
    /// depth plane as xSlope, ySlope, constant. Lane 3 of each row is unused.
    std::array<std::array<float, 4>, SEGMENT_FLOAT4S * MAX_SEGMENTS> segments{};
};

static_assert(std::is_standard_layout_v<Constants>);
static_assert(std::is_trivially_copyable_v<Constants>);
static_assert(alignof(Constants) == 16);
static_assert(sizeof(Constants) == 832);
static_assert(sizeof(Constants) == 64 + 64 * MAX_SEGMENTS);
static_assert(offsetof(Constants, sourceAnchor) == 0);
static_assert(offsetof(Constants, segmentParams) == 16);
static_assert(offsetof(Constants, materialParams) == 32);
static_assert(offsetof(Constants, fisheyeParams) == 48);
static_assert(offsetof(Constants, segments) == 64);

/**
 * @struct ClipPosition
 * @brief Homogeneous clip position before the perspective divide.
 *
 * The vertex shader writes this value to `SV_POSITION`.
 */
struct ClipPosition
{
    float x = .0f;
    float y = .0f;
    float z = .0f;
    float w = 1.0f;
};

/**
 * @struct VertexColor
 * @brief Vertex color channels from zero through one.
 *
 * The layout matches the normalized `COLOR0` shader input.
 */
struct VertexColor
{
    float r = .0f;
    float g = .0f;
    float b = .0f;
    float a = .0f;
};

/**
 * @brief Pack a segmented projection into the constant-buffer layout `kPlaneVS` reads.
 *
 * This function establishes the lane assignment named on each Constants field.
 * Two lanes are easy to misread: @p wingBand occupies `sourceAnchor` lanes 2 and
 * 3, and `segmentParams` lane 2 holds @p segmentCount minus 1, because the
 * shader clamps a slot index against it. Rows past @p segmentCount stay zero.
 *
 * @param segments       Per-segment homography and depth plane, in slot order.
 * @param segmentCount   Number of valid segments. It is clamped to
 *                       [1, MAX_SEGMENTS] before use.
 * @param firstBoundaryX Absolute source X at the first chord boundary.
 * @param invStride      Reciprocal source width of one chord.
 * @param sourceAnchor   Absolute source position treated as plane-local (0, 0).
 * @param wingBand       Wing center X and reciprocal half-width.
 * @param material       Desaturation, brightness, opacity, and edge sheen.
 * @param fisheye        Center X, center Y, reciprocal half-width, and strength.
 *                       The default all-zero value disables the warp.
 * @return The packed constants.
 */
inline Constants PackSegments(const std::array<SegmentData, MAX_SEGMENTS>& segments,
                              std::size_t segmentCount,
                              float firstBoundaryX,
                              float invStride,
                              const std::array<float, 2>& sourceAnchor,
                              const std::array<float, 2>& wingBand,
                              const std::array<float, 4>& material,
                              const std::array<float, 4>& fisheye = {}) noexcept
{
    Constants out{};
    segmentCount = std::clamp<std::size_t>(segmentCount, 1, MAX_SEGMENTS);
    out.sourceAnchor = {sourceAnchor[0], sourceAnchor[1], wingBand[0], wingBand[1]};
    out.segmentParams = {firstBoundaryX, invStride, static_cast<float>(segmentCount - 1), .0f};
    out.materialParams = {material[0], material[1], material[2], material[3]};
    out.fisheyeParams = fisheye;
    for (std::size_t i = 0; i < segmentCount; ++i)
    {
        const std::size_t base = i * SEGMENT_FLOAT4S;
        const auto& homography = segments[i].homography;
        const auto& depth = segments[i].depthPlane;
        out.segments[base] = {homography[0], homography[1], homography[2], .0f};
        out.segments[base + 1] = {homography[3], homography[4], homography[5], .0f};
        out.segments[base + 2] = {homography[6], homography[7], homography[8], .0f};
        out.segments[base + 3] = {depth[0], depth[1], depth[2], .0f};
    }
    return out;
}

/**
 * @brief Mirror of the row-local barrel magnification in `kPlaneVS`.
 *
 * The horizontal polynomial fixes both row endpoints, expands the midpoint, and
 * compresses the wing intervals. Vertical scale independently falls from 1.18 at
 * the midpoint to .74 at either endpoint at full strength.
 *
 * The zero-strength early-out below is a deliberate CPU-only divergence: it
 * returns a bit-exact identity, while `kPlaneVS` always evaluates the vertical
 * recentring `c + (y - c) * 1.0`, which is not bit-exact for a large center. Do
 * not make either side match the other.
 *
 * @param constants         Packed constants. Only fisheyeParams is read.
 * @param absolutePosition  Unwarped vertex position, in absolute ImDraw pixels.
 * @return The warped position. It feeds EvaluateSegmentSlot and the homography;
 *         the wing weight keeps using the unwarped X, as the shader does.
 */
inline std::array<float, 2> EvaluateFisheyePosition(
    const Constants& constants, const std::array<float, 2>& absolutePosition) noexcept
{
    constexpr float HORIZONTAL_BARREL = .24f;
    constexpr float CENTER_VERTICAL_SCALE = 1.18f;
    constexpr float EDGE_VERTICAL_SCALE = .74f;

    const float invHalfWidth = std::max(constants.fisheyeParams[2], .0f);
    const float strength =
        invHalfWidth > .0f ? std::clamp(constants.fisheyeParams[3], .0f, 1.0f) : .0f;
    if (strength <= .0f)
    {
        return absolutePosition;
    }

    const float halfWidth = 1.0f / invHalfWidth;
    const float normalized = (absolutePosition[0] - constants.fisheyeParams[0]) * invHalfWidth;
    const float t = std::clamp(normalized, -1.0f, 1.0f);
    const float edge = std::abs(t);
    const float edgeWeight = edge * edge * (3.0f - 2.0f * edge);
    const float verticalScale =
        1.0f + strength * ((CENTER_VERTICAL_SCALE - 1.0f) * (1.0f - edgeWeight) +
                           (EDGE_VERTICAL_SCALE - 1.0f) * edgeWeight);

    return {absolutePosition[0] + halfWidth * HORIZONTAL_BARREL * strength * t * (1.0f - t * t),
            constants.fisheyeParams[1] +
                (absolutePosition[1] - constants.fisheyeParams[1]) * verticalScale};
}

/**
 * @brief Pack a planar projection while retaining the segmented shader ABI.
 *
 * It writes one chord, no chord boundary, no wing band and no fisheye, so the
 * shader follows the same path as a flat solve. A zero stride lane pins the slot
 * to chord 0 for every vertex X, which is why the boundary lane may be 0 here
 * while BuildProjection's own flat path writes the bounds left edge into it.
 * The material array holds desaturation, brightness and opacity only; edge sheen
 * is forced to 0, which a flat plate cannot show anyway because its wing band
 * width is zero.
 *
 * @param homography    Row-major local-pixel to NDC homogeneous map.
 * @param depthPlane    xSlope, ySlope and constant over normalized screen.
 * @param sourceAnchor  Absolute source position treated as plane-local (0, 0).
 * @param material      Desaturation, brightness and opacity.
 * @return The packed constants.
 */
inline Constants Pack(const std::array<float, 9>& homography,
                      const std::array<float, 3>& depthPlane,
                      const std::array<float, 2>& sourceAnchor,
                      const std::array<float, 3>& material) noexcept
{
    std::array<SegmentData, MAX_SEGMENTS> segments{};
    segments[0] = {homography, depthPlane};
    return PackSegments(segments,
                        1,
                        .0f,
                        .0f,
                        sourceAnchor,
                        {.0f, .0f},
                        {material[0], material[1], material[2], .0f});
}

/**
 * @brief Smoothstep weight of the wing sheen at a source position.
 *
 * `kPlaneVS` evaluates this on the unwarped vertex X, not on the fisheye output,
 * so pass the unwarped position here as well. A zero reciprocal half-width in
 * `sourceAnchor` lane 3 returns 0 and disables the sheen, which is what the flat
 * solve writes.
 *
 * @param constants         Packed constants. Only sourceAnchor lanes 2 and 3 are read.
 * @param absolutePosition  Unwarped vertex position, in absolute ImDraw pixels.
 * @return Band weight from 0 at the wing center to 1 at or past the half-width.
 */
inline float EvaluateWingWeight(const Constants& constants,
                                const std::array<float, 2>& absolutePosition) noexcept
{
    const float normalizedDistance =
        std::clamp(std::abs(absolutePosition[0] - constants.sourceAnchor[2]) *
                       std::max(constants.sourceAnchor[3], .0f),
                   .0f,
                   1.0f);
    return normalizedDistance * normalizedDistance * (3.0f - 2.0f * normalizedDistance);
}

/**
 * @brief Select the chord slot for a source position.
 *
 * `kPlaneVS` selects the slot from the fisheye-warped X, so pass the output of
 * EvaluateFisheyePosition, not the raw vertex position. The non-finite guard and
 * the extra clamp to MAX_SEGMENTS - 1 are deliberate CPU-only divergences;
 * `kPlaneVS` clamps only against `segmentParams` lane 2.
 *
 * @param constants         Packed constants. Only segmentParams is read.
 * @param absolutePosition  Fisheye-warped position, in absolute ImDraw pixels.
 * @return Slot index. Multiply it by SEGMENT_FLOAT4S to reach the chord's rows.
 */
inline std::size_t EvaluateSegmentSlot(const Constants& constants,
                                       const std::array<float, 2>& absolutePosition) noexcept
{
    const float raw =
        std::floor((absolutePosition[0] - constants.segmentParams[0]) * constants.segmentParams[1]);
    if (!std::isfinite(raw))
    {
        return 0;
    }
    const float maxSlot =
        std::clamp(constants.segmentParams[2], .0f, static_cast<float>(MAX_SEGMENTS - 1));
    return static_cast<std::size_t>(std::clamp(raw, .0f, maxSlot));
}

/**
 * @brief Mirror of the position arithmetic in `kPlaneVS`.
 *
 * It is not bit-identical: EvaluateFisheyePosition and EvaluateSegmentSlot each
 * carry one deliberate CPU-only divergence, described on those functions.
 *
 * With warped source offset $(x, y)$, the selected chord's homography rows
 * $h_0, h_1, h_2$ and its depth plane $(d_x, d_y, d_c)$:
 *
 * $$
 * \begin{aligned}
 * (X, Y, W) &= (h_0 \cdot s,\; h_1 \cdot s,\; h_2 \cdot s), \qquad s = (x, y, 1) \\
 * \tilde{u} &= \tfrac{1}{2}(X + W), \qquad \tilde{v} = \tfrac{1}{2}(W - Y) \\
 * Z &= d_x \tilde{u} + d_y \tilde{v} + d_c W
 * \end{aligned}
 * $$
 *
 * The rasterizer divides by $W$. So $X/W$ and $Y/W$ are NDC, $\tilde{u}/W$ and
 * $\tilde{v}/W$ are the normalized top-left screen coordinates the depth plane
 * is fitted over, and $Z/W$ is the viewport depth DepthClip compares against.
 * Scaling a whole homography leaves all three quotients unchanged, which is why
 * BuildProjection may rescale one chord to match its neighbour.
 *
 * @param constants         Packed constants for this plate.
 * @param absolutePosition  Unwarped vertex position, in absolute ImDraw pixels.
 * @return The homogeneous clip position, before the perspective divide.
 */
inline ClipPosition EvaluateVertex(const Constants& constants,
                                   const std::array<float, 2>& absolutePosition) noexcept
{
    const auto warpedPosition = EvaluateFisheyePosition(constants, absolutePosition);
    const float sourceX = warpedPosition[0] - constants.sourceAnchor[0];
    const float sourceY = warpedPosition[1] - constants.sourceAnchor[1];
    const std::size_t base = EvaluateSegmentSlot(constants, warpedPosition) * SEGMENT_FLOAT4S;
    const auto& row0 = constants.segments[base];
    const auto& row1 = constants.segments[base + 1];
    const auto& row2 = constants.segments[base + 2];
    const auto& depth = constants.segments[base + 3];
    const std::array<float, 3> projected{row0[0] * sourceX + row0[1] * sourceY + row0[2],
                                         row1[0] * sourceX + row1[1] * sourceY + row1[2],
                                         row2[0] * sourceX + row2[1] * sourceY + row2[2]};
    const float x01Numerator = .5f * (projected[0] + projected[2]);
    const float y01Numerator = .5f * (projected[2] - projected[1]);
    const float depthNumerator =
        depth[0] * x01Numerator + depth[1] * y01Numerator + depth[2] * projected[2];

    ClipPosition out{};
    out.x = projected[0];
    out.y = projected[1];
    out.z = depthNumerator;
    out.w = projected[2];
    return out;
}

/**
 * @brief Reference implementation of the color arithmetic in `kPlaneVS`.
 *
 * Desaturation, opacity and the sheen lane are clamped to [0, 1], but brightness
 * is only lower-clamped at 0. A brightness above 1 therefore returns RGB above
 * 1. `kPlaneVS` clamps the same way, so the shader keeps the overbright value.
 *
 * @param constants   Packed constants. Only materialParams is read.
 * @param input       Vertex color taken from the ImGui draw vertex.
 * @param wingWeight  Band weight from EvaluateWingWeight. The default 0 removes
 *                    the sheen term, which matches every flat plate.
 * @return The treated color. Alpha is scaled by the opacity lane only.
 */
inline VertexColor EvaluateColor(const Constants& constants,
                                 const VertexColor& input,
                                 float wingWeight = .0f) noexcept
{
    const float desaturation = std::clamp(constants.materialParams[0], .0f, 1.0f);
    const float brightness = std::max(constants.materialParams[1], .0f);
    const float luminance = input.r * .2126f + input.g * .7152f + input.b * .0722f;
    const float sheen =
        std::clamp(constants.materialParams[3], .0f, 1.0f) * std::clamp(wingWeight, .0f, 1.0f);
    const float r = (input.r + (luminance - input.r) * desaturation) * brightness;
    const float g = (input.g + (luminance - input.g) * desaturation) * brightness;
    const float b = (input.b + (luminance - input.b) * desaturation) * brightness;
    return {r + (1.0f - r) * sheen,
            g + (1.0f - g) * sheen,
            b + (1.0f - b) * sheen,
            input.a * std::clamp(constants.materialParams[2], .0f, 1.0f)};
}
}  // namespace Graffito::ShaderContract
