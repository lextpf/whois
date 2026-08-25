#pragma once

/**
 * @brief Compile-time version macros for the glyph plugin.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Utilities
 *
 * `PCH.hpp` includes this header on its last line, so every translation unit that uses the
 * precompiled header sees these macros. Do not include this file directly.
 *
 * No C++ code reads these macros. The two consumers are outside the compiler:
 *
 * - `scripts/_clean_docs.py` greps GLYPH_VERSION_MAJOR, GLYPH_VERSION_MINOR and
 *   GLYPH_VERSION_PATCH out of this file to build the documentation home-page subtitle.
 *   GLYPH_VERSION_RELEASE is not read.
 * - `SKSEPluginInfo` in main.cpp repeats the same four numbers as a literal
 *   `REL::Version(0, 1, 0, 0)`. It does not reference these macros, so a version bump must
 *   be applied in both places or the plugin reports a version the documentation does not.
 */

/// @brief Major version number.
#define GLYPH_VERSION_MAJOR 0
/// @brief Minor version number.
#define GLYPH_VERSION_MINOR 1
/// @brief Patch version number.
#define GLYPH_VERSION_PATCH 0
/// @brief Release version number.
#define GLYPH_VERSION_RELEASE 0

/**
 * @brief Stringify the four version components.
 *
 * Applies `#` directly to each parameter. An operand of `#` is not macro-expanded, so an
 * object-like macro passed in yields the macro name, not its value.
 *
 * @param major Major version.
 * @param minor Minor version.
 * @param patch Patch version.
 * @param release Release version.
 *
 * @see GLYPH_VERSION
 */
#define GLYPH_VERSION_STRINGIFY(major, minor, patch, release) \
    #major "." #minor "." #patch "." #release

/**
 * @brief Build the version string from the four component macros.
 *
 *
 * `GLYPH_VERSION_STRINGIFY` expands to the macro names, not their values. The result is
 *
 * "GLYPH_VERSION_MAJOR.GLYPH_VERSION_MINOR." plus the remaining two names, not "0.1.0.0".
 * The
 * macro has no call site.
 */
#define GLYPH_VERSION        \
    GLYPH_VERSION_STRINGIFY( \
        GLYPH_VERSION_MAJOR, GLYPH_VERSION_MINOR, GLYPH_VERSION_PATCH, GLYPH_VERSION_RELEASE)
