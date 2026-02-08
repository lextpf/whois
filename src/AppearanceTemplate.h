#pragma once

#include <RE/Skyrim.h>
#include <cstdint>
#include <string>

/**
 * @namespace AppearanceTemplate
 * @brief NPC appearance template system for copying appearance to player.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Core
 *
 * Provides functionality to copy an NPC's appearance (head parts, tint layers,
 * hair color, weight, face morphs, FaceGen data) to the player character based
 * on INI settings. Useful for using a pre-made character as a template.
 *
 * ## :material-cogs: Settings
 *
 * Configure in whois.ini under `[AppearanceTemplate]`:
 *
 * ### :material-cogs: Core Settings
 * |               Setting | Type   | Default | Description                                      |
 * |-----------------------|--------|---------|--------------------------------------------------|
 * |        TemplateFormID | hex    | -       | FormID of the NPC (e.g., 0xD62)                  |
 * |        TemplatePlugin | string | -       | Plugin file containing the NPC (e.g., Inigo.esp) |
 * | UseTemplateAppearance | bool   | false   | Enable/disable the feature                       |
 * |   TemplateIncludeRace | bool   | false   | Copy the NPC's race (REQUIRED)                   |
 * |   TemplateIncludeBody | bool   | false   | Also copy height/body morphs                     |
 *
 * ### :material-cogs: FaceGen Options
 * |                 Setting | Type   | Default | Description                                     |
 * |-------------------------|--------|---------|-------------------------------------------------|
 * |     TemplateCopyFaceGen | bool   | true    | Copy FaceGen NIF/tint files (recommended)       |
 * |        TemplateCopySkin | bool   | false   | Copy skin textures                              |
 * |    TemplateCopyOverlays | bool   | false   | Copy RaceMenu overlays (requires NiOverride)    |
 * |      TemplateCopyOutfit | bool   | false   | Copy equipped armor from template actor         |
 * | TemplateReapplyOnReload | bool   | false   | Re-apply appearance on hot reload               |
 * |   TemplateFaceGenPlugin | string | ""      | Override plugin for FaceGen paths (auto-detect) |
 *
 * ## :material-help: What Gets Copied
 * - Race (TemplateIncludeRace)
 * - Head parts (eyes, hair, brows, scars, facial hair)
 * - Hair color
 * - Tint layers (skin tone, makeup, war paint, dirt)
 * - Face morphs and sculpt data
 * - Weight/body morph
 * - FaceGen mesh and tint texture (TemplateCopyFaceGen)
 * - Skin textures (TemplateCopySkin)
 * - Equipped outfit (TemplateCopyOutfit)
 *
 * ## :material-help: Example Configuration
 *
 * ```ini
 * [AppearanceTemplate]
 * ;; Inigo example
 * TemplateFormID = 0xD62
 * TemplatePlugin = Inigo.esp
 * UseTemplateAppearance = true
 * TemplateIncludeRace = true    ; REQUIRED for Inigo's custom race
 * TemplateIncludeBody = true
 * TemplateCopyFaceGen = true
 * TemplateCopySkin = true
 * TemplateCopyOverlays = false  ; Requires NiOverride, not implemented
 * TemplateCopyOutfit = true
 * ```
 *
 * @note For followers with custom races (Inigo, etc.), you MUST enable
 *       TemplateIncludeRace, otherwise the head parts won't work!
 *
 * Applied on game load when the player is initialized.
 */
namespace AppearanceTemplate
{
    /**
     * @brief Build the FaceGen mesh path for an NPC.
     *
     * FaceGen meshes are stored at:
     * meshes/actors/character/facegendata/facegeom/<PluginName>/<FormID>.nif
     *
     * @param pluginName Plugin filename (e.g., "MyFollower.esp")
     * @param formID The NPC's FormID
     * @return Full path to FaceGen mesh
     */
    std::string BuildFaceGenMeshPath(const std::string& pluginName, RE::FormID formID);

    /**
     * @brief Build the FaceGen tint texture path for an NPC.
     *
     * FaceGen tints are stored at:
     * textures/actors/character/facegendata/facetint/<PluginName>/<FormID>.dds
     *
     * @param pluginName Plugin filename (e.g., "MyFollower.esp")
     * @param formID The NPC's FormID
     * @return Full path to FaceGen tint texture
     */
    std::string BuildFaceGenTintPath(const std::string& pluginName, RE::FormID formID);

    /**
     * @brief Apply FaceGen data from template NPC to player.
     *
     * Loads the template's FaceGen NIF and tint texture and applies them
     * to the player character.
     *
     * @param templateNPC The template NPC
     * @param pluginName Plugin containing the template
     * @param formID Original FormID used for FaceGen paths
     * @return true if FaceGen was applied, false if files not found or error
     */
    bool ApplyFaceGen(RE::TESNPC* templateNPC, const std::string& pluginName, RE::FormID formID);
    
    /**
     * @brief Apply template appearance to player if configured in settings.
     *
     * Checks if UseTemplateAppearance is enabled and a valid template is specified.
     * If so, resolves the FormID and applies the appearance.
     *
     * Safe to call multiple times; will only apply once per game session unless
     * ResetAppliedFlag() is called or settings are reloaded.
     *
     * @return true if appearance was applied successfully, false otherwise
     */
    bool ApplyIfConfigured();

    /**
     * @brief Reset the applied flag to allow re-applying appearance.
     *
     * Call this before ApplyIfConfigured() to force re-application,
     * for example during hot reload.
     */
    void ResetAppliedFlag();

    /**
     * @brief Resolve a FormID from a plugin file.
     *
     * Handles load order by looking up the plugin's actual index via TESDataHandler.
     *
     * @param formIdStr Hex FormID string (e.g., "0x12345" or "12345")
     * @param pluginName Plugin filename (e.g., "MyFollower.esp")
     * @return Resolved FormID, or 0 if plugin not found
     */
    RE::FormID ResolveFormID(const std::string& formIdStr, const std::string& pluginName);

    /**
     * @brief Copy appearance from template NPC to player.
     *
     * Copies the following attributes:
     * - Race (required for cross-race templates)
     * - Head parts (eyes, hair, brows, scars, etc.)
     * - Tint layers (skin tone, war paint, dirt, etc.)
     * - Hair color
     * - Weight/body morph
     * - Face morph presets and sculpt data
     *
     * @param templateNPC The NPC to copy appearance from
     * @param includeRace Whether to copy the template's race
     * @param includeBody Whether to also copy height/body morphs
     * @return true if copy succeeded, false otherwise
     */
    bool CopyAppearanceToPlayer(RE::TESNPC* templateNPC, bool includeRace, bool includeBody);

    /**
     * @brief Force player appearance update after changes.
     *
     * Marks the player's face as dirty and queues NiNode update.
     * Call after modifying appearance data.
     */
    void UpdatePlayerAppearance();

    /**
     * @brief Check if a template NPC is compatible with the player's race.
     *
     * @param templateNPC The template NPC to check
     * @return true if races are compatible, false otherwise
     */
    bool IsRaceCompatible(RE::TESNPC* templateNPC);

    /**
     * @brief Initialize overlay system (no-op, kept for compatibility).
     */
    void QueryNiOverrideInterface();

    /**
     * @brief Retry overlay initialization (no-op, kept for compatibility).
     */
    void RetryNiOverrideInterface();

    /**
     * @brief Check if manual overlay system is available.
     *
     * @return false (not implemented)
     */
    bool HasOverlayInterface();

    /**
     * @brief Test manual overlay extraction on player.
     *
     * Scans player's 3D nodes for overlay geometry and logs results.
     * Used for debugging overlay integration. (not implemented)
     */
    void TestOverlayOnPlayer();

}
