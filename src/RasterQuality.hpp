#pragma once

#include <cstdint>

/**
 * @namespace RasterQuality
 * @brief Fixed source-raster quality values for the crisp render layer.
 *
 * These values change source textures only. They do not change nameplate geometry or the
 * backbuffer resolution. Font density is fixed because a density change requires a complete
 * font-atlas rebuild.
 */
namespace RasterQuality
{
inline constexpr float FONT_DENSITY = 1.5f;  ///< Preferred source density for all fonts.
inline constexpr float FONT_FALLBACK_DENSITY =
    1.0f;  ///< Source density used when the preferred atlas does not fit.
inline constexpr int FONT_GLYPH_PADDING = 8;          ///< Source pixels between packed glyphs.
inline constexpr int FONT_MIP_LIMIT = 3;              ///< Highest mip level used for font sampling.
inline constexpr int STATUS_ICON_TEXTURE_SIZE = 256;  ///< Square SVG raster size in pixels.
inline constexpr int RANK_EMBLEM_TEXTURE_SIZE = 512;  ///< Square emblem raster size in pixels.

/** @brief Return true when a positive integer is a power of two. */
constexpr bool IsPowerOfTwo(std::uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/**
 * @brief Return true when atlas padding remains at least one texel at the font mip limit.
 *
 * Each mip level halves the padding. A limit of three therefore requires eight source
 * pixels of padding.
 */
constexpr bool FontPaddingSupportsMipLimit()
{
    return FONT_MIP_LIMIT >= 0 && FONT_MIP_LIMIT < 31 &&
           FONT_GLYPH_PADDING >= (1 << FONT_MIP_LIMIT);
}

static_assert(IsPowerOfTwo(STATUS_ICON_TEXTURE_SIZE));
static_assert(IsPowerOfTwo(RANK_EMBLEM_TEXTURE_SIZE));
static_assert(FontPaddingSupportsMipLimit());
}  // namespace RasterQuality
