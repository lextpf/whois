#pragma once

#include <algorithm>
#include <cmath>

/**
 * @namespace Renderer::NameFit
 * @brief Width-driven fit for long actor names, in font-relative units.
 * @author Alex (https://github.com/lextpf)
 *
 * The budget and the result are multiples of the font size, so the same name fits
 * identically at any distance scale, resolution, or font.
 *
 * This header has no ImGui, Direct3D, or Skyrim dependency. It is compiled into the
 * test harness and exercised directly by the NameFit cases in tests/test_utils.cpp.
 */
namespace Renderer::NameFit
{
/**
 * @brief Width budget for a name, in ems.
 *
 * A name that measures at or below `MAX_WIDTH_EM
 * * fontSize` pixels keeps its authored
 * typography.
 */
inline constexpr float MAX_WIDTH_EM = 5.75f;

// Legibility floors for very long or modded actor names. Compute() is best-effort: it
// returns an overflowing plate rather than an unreadable name.

/**
 * @brief Smallest permitted font-size multiplier.
 *
 * This floor applies to names wider than
 * approximately 8.58 em. While it is the only active
 * floor, the horizontal scale absorbs the
 * remaining reduction and keeps the fitted width on
 * the budget.
 */
inline constexpr float MIN_FONT_SCALE = .78f;

/**
 * @brief Smallest permitted horizontal extent multiplier.
 *
 * This floor applies to names
 * wider than approximately 8.78 em. Past that width, both floors
 * apply and cannot supply the
 * required reduction. The fitted width then exceeds
 * `MAX_WIDTH_EM * fontSize`. The caller must
 * accept or handle that overflow.
 */
inline constexpr float MIN_HORIZONTAL_SCALE = .84f;

/**
 * @brief Exponent share assigned to font-size reduction.
 *
 * The required scale $r$ is split
 * between font size and horizontal compression, with
 * $s$ equal to `FONT_REDUCTION_SHARE`:
 *
 *
 * $$f = r^{\,s} \qquad h = r^{\,1-s} \qquad f \cdot h = r$$
 *
 * A value above 0.5 gives font
 * reduction more of the work. The identity holds only while no
 * legibility floor applies.
 *
 *
 * With $f_{min}$ equal to `MIN_FONT_SCALE` and $h_{min}$ equal to
 * `MIN_HORIZONTAL_SCALE`, the
 * font floor applies when $r < f_{min}^{1/s}$. Both floors apply
 * when $r < f_{min} h_{min}$.
 * Here, $r$ is `MAX_WIDTH_EM` divided by the measured width in
 * ems. With the current constants,
 * the font floor applies at approximately 8.6 em and the
 * horizontal floor applies at
 * approximately 8.8 em.
 */
inline constexpr float FONT_REDUCTION_SHARE = .62f;

/**
 * @struct Result
 * @brief Multipliers that fit one name segment into the width budget.
 */
struct Result
{
    /// Font-size multiplier in [MIN_FONT_SCALE, 1]. The caller must re-measure the
    /// segment after it applies this factor.
    float fontScale = 1.0f;

    /// X-extent multiplier in [MIN_HORIZONTAL_SCALE, 1]. It compresses the x axis only
    /// and applies to the already-measured extents, so it needs no re-measurement.
    float horizontalScale = 1.0f;
};

/**
 * @brief Compute a width-driven fit in font-relative units.
 *
 * Both inputs are compared in ems, so the result is stable across distance scaling,
 * resolution changes, and localized glyphs.
 *
 * Measure the whole segment that carries the name, not the name substring alone: the
 * budget covers everything the segment draws. Apply the two multipliers in order.
 * Set the new font size from the font scale, re-measure the segment, then multiply the
 * new x extent by the horizontal scale.
 *
 * @param measuredWidth Measured text width in pixels, as returned by
 *                      `ImFont::CalcTextSizeA` at `fontSize`.
 * @param fontSize      Font size in pixels.
 *
 * @return Multipliers to apply to the name segment. `{1.0, 1.0}` means no
 *         adjustment, and is also the safe result for non-finite or
 *         non-positive input. The two cases are not distinguishable.
 */
inline Result Compute(float measuredWidth, float fontSize)
{
    if (!std::isfinite(measuredWidth) || !std::isfinite(fontSize) || measuredWidth <= .0f ||
        fontSize <= .0f)
    {
        return {};
    }

    const float maximumWidth = fontSize * MAX_WIDTH_EM;
    if (measuredWidth <= maximumWidth)
    {
        return {};
    }

    const float requestedScale = maximumWidth / measuredWidth;
    const float fontScale =
        std::clamp(std::pow(requestedScale, FONT_REDUCTION_SHARE), MIN_FONT_SCALE, 1.0f);
    const float horizontalScale =
        std::clamp(requestedScale / fontScale, MIN_HORIZONTAL_SCALE, 1.0f);
    return {fontScale, horizontalScale};
}
}  // namespace Renderer::NameFit
