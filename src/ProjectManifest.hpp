#pragma once

#include <string>
#include <vector>

/**
 * @namespace ProjectManifest
 * @brief Asset manifest (`glyph.project.json`) resolving obfuscated GUID assets.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Utilities
 *
 * The plugin's custom assets (fonts, tier emblem badges, particle sprites) ship under
 * GUID-obfuscated file names. `glyph.project.json` sits next to the DLL in
 * `Data/SKSE/Plugins/` and maps logical roles and tokens to those files, so each loader can
 * find its asset. A relative path in the manifest is resolved against the manifest's own
 * folder. An entry that is already absolute (a drive letter, or a leading slash or
 * backslash) is used verbatim. The shipped manifest uses a `glyph/...` prefix, which is what
 * yields the `Data/SKSE/Plugins/glyph/...` paths.
 *
 * When the manifest is missing or malformed, every accessor returns empty and each loader
 * falls back to its built-in default (Font Awesome tier icons, procedural particle sprites,
 * INI font paths). A bad manifest never hard-fails the plugin.
 *
 * A manifest that parses but carries a section of the wrong JSON type is not treated as
 * malformed. That section is skipped without a warning, only its own accessors stay empty,
 * and Load() and IsLoaded() still report success.
 */
namespace ProjectManifest
{
/**
 * @brief Load and parse the manifest.
 *
 * Call once at startup, before any asset load. The call resets all resolved state before it
 * opens the file, so a second call that fails leaves the manifest empty and discards an
 * earlier successful load.
 *
 * @param path  Manifest path. Defaults to the shipped location next to the DLL.
 * @return      True if the manifest parsed; false, with every accessor empty, otherwise.
 *
 * @warning The resolved state is a plain function-local static with no mutex. Load() runs on
 *          the plugin-load thread while the accessors run on the render thread. The "once at
 *          startup, before any asset load" rule is what keeps the two apart.
 */
bool Load(const std::string& path = "Data/SKSE/Plugins/glyph.project.json");

/// @brief True after a successful manifest load.
bool IsLoaded();

// Font paths by role, from the manifest's "fonts" object. Each returns a resolved full path,
// or a stable empty string when that role is not mapped, in which case the font loader falls
// back to the matching *FontPath key in glyph.ini.

/// @brief Return the actor-name font path for the manifest role "name".
const std::string& FontName();

/// @brief Return the level-number font path for the manifest role "level".
const std::string& FontLevel();

/// @brief Return the title and honorific font path for the manifest role "title".
const std::string& FontTitle();

/// @brief Return the ornament glyph font path for the manifest role "ornament".
const std::string& FontOrnament();

/**
 * @brief Tier emblem badge paths in rank order, index 0 being the lowest rank.
 *
 * @return The mapped emblem paths. Empty when no emblems are mapped.
 */
const std::vector<std::string>& TierBadges();

/**
 * @brief Particle sprite variant paths for a style token.
 *
 * @param token  Style token, for example "smoke".
 * @return       The mapped sprite paths. A stable empty vector when the token has no mapped
 *               sprites.
 */
const std::vector<std::string>& ParticleVariants(const std::string& token);

/// @brief Return the bubble-pop sprite path, or an empty string when it is not mapped.
const std::string& BubblePop();
}  // namespace ProjectManifest
