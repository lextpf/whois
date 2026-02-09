#include "AppearanceTemplate.h"
#include "Settings.h"

#include <SKSE/SKSE.h>

#include <memory>
#include <sstream>
#include <iomanip>
#include <vector>

namespace AppearanceTemplate
{
    namespace {
        struct TemplateState {
            bool applied = false;
            std::string plugin;
            RE::FormID formID = 0;
        };
    }

    static TemplateState& GetState() {
        static TemplateState state;
        return state;
    }

    void ResetAppliedFlag()
    {
        GetState().applied = false;
        SKSE::log::info("AppearanceTemplate: Applied flag reset");
    }

    // Stubs for overlay interface functions
    void QueryNiOverrideInterface() { /* NiOverride not used; kept for API compatibility */ }
    void RetryNiOverrideInterface() { /* NiOverride not used; kept for API compatibility */ }
    void TestOverlayOnPlayer()
    {
        SKSE::log::info("Overlay system not implemented");
    }

    // Forward declarations
    static bool CopyOutfitFromActor(RE::Actor* sourceActor, RE::Actor* player);

    /**
     * Find a loaded actor that uses the given NPC as its base.
     * Returns nullptr if no such actor is currently loaded.
     */
    static RE::Actor* FindActorByBase(RE::TESNPC* npc)
    {
        if (!npc) return nullptr;

        auto player = RE::PlayerCharacter::GetSingleton();

        // Search through all loaded actors
        const auto& [allForms, lock] = RE::TESForm::GetAllForms();
        for (auto& [formID, form] : *allForms) {
            if (!form) continue;

            auto actor = form->As<RE::Actor>();
            if (!actor) continue;

            // Skip the player
            if (actor == player) continue;

            // Check if this actor uses our template NPC as its base
            if (actor->GetActorBase() == npc) {
                // Verify actor is loaded
                if (actor->Is3DLoaded()) {
                    SKSE::log::debug("AppearanceTemplate: Found loaded actor for NPC {:08X}", npc->GetFormID());
                    return actor;
                }
            }
        }

        return nullptr;
    }

    std::string BuildFaceGenMeshPath(const std::string& pluginName, RE::FormID formID)
    {
        // FormID is stored as 8-digit lowercase hex
        std::ostringstream ss;
        ss << "meshes\\actors\\character\\facegendata\\facegeom\\"
           << pluginName << "\\"
           << std::setfill('0') << std::setw(8) << std::hex << std::uppercase << formID
           << ".nif";
        return ss.str();
    }

    std::string BuildFaceGenTintPath(const std::string& pluginName, RE::FormID formID)
    {
        std::ostringstream ss;
        ss << "textures\\actors\\character\\facegendata\\facetint\\"
           << pluginName << "\\"
           << std::setfill('0') << std::setw(8) << std::hex << std::uppercase << formID
           << ".dds";
        return ss.str();
    }

    // Compute the FaceGen file ID for a plugin
    // Light plugins (ESL/ESPFE): FaceGen uses lower 12 bits (e.g., FEDC6810 -> 00000810)
    // Regular plugins (ESP/ESM): FaceGen uses lower 24 bits (e.g., 05012345 -> 00012345)
    static RE::FormID FaceGenFileID(RE::FormID resolvedFormID, const RE::TESFile* plugin)
    {
        if (plugin && plugin->IsLight()) {
            // Light plugins: extract lower 12 bits
            return resolvedFormID & 0x00000FFF;
        }
        // Regular plugins: extract lower 24 bits
        return resolvedFormID & 0x00FFFFFF;
    }

    bool ApplyFaceGen(RE::TESNPC* templateNPC, const std::string& fallbackPluginName, RE::FormID fallbackFormID)
    {
        if (!templateNPC) {
            SKSE::log::error("AppearanceTemplate: ApplyFaceGen called with null template");
            return false;
        }

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            SKSE::log::error("AppearanceTemplate: Player not available for FaceGen");
            return false;
        }

        auto playerBase = player->GetActorBase();
        if (!playerBase) {
            SKSE::log::error("AppearanceTemplate: Player base not available for FaceGen");
            return false;
        }

        // Get BSFaceGenManager
        auto faceGenManager = RE::BSFaceGenManager::GetSingleton();
        if (!faceGenManager) {
            SKSE::log::warn("AppearanceTemplate: BSFaceGenManager not available");
            return false;
        }

        // Build candidate plugin list for FaceGen, in priority order:
        // 1) INI override (TemplateFaceGenPlugin)
        // 2) Winning overrides (sourceFiles)
        // 3) User-specified (TemplatePlugin)
        // 4) Origin/master (sourceFiles)
        std::vector<const RE::TESFile*> candidates;
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto lookupFile = [&](const std::string& name) -> const RE::TESFile* {
            if (!dh) return nullptr;
            for (auto* f : dh->files) {
                if (f && f->fileName && _stricmp(f->fileName, name.c_str()) == 0) return f;
            }
            return nullptr;
        };

        if (!Settings::TemplateFaceGenPlugin.empty()) {
            if (auto* f = lookupFile(Settings::TemplateFaceGenPlugin)) {
                candidates.push_back(f);
                SKSE::log::info("AppearanceTemplate: Using INI override for FaceGen plugin: {}", f->fileName);
            }
        }

        if (templateNPC->sourceFiles.array && !templateNPC->sourceFiles.array->empty()) {
            // Winning to origin, manual reverse because TESFileArray has no rbegin/rend
            for (int i = static_cast<int>(templateNPC->sourceFiles.array->size()) - 1; i >= 0; --i) {
                auto* f = (*templateNPC->sourceFiles.array)[i];
                if (f) candidates.push_back(f);
            }
        }

        if (!Settings::TemplatePlugin.empty()) {
            if (auto* f = lookupFile(Settings::TemplatePlugin)) {
                candidates.push_back(f);
            }
        }

        // Ensure origin/master is tried last if not already
        if (templateNPC->sourceFiles.array && !templateNPC->sourceFiles.array->empty()) {
            const RE::TESFile* origin = (*templateNPC->sourceFiles.array)[0];
            if (origin) candidates.push_back(origin);
        }

        // Deduplicate while preserving order
        std::vector<const RE::TESFile*> unique;
        for (auto* f : candidates) {
            bool exists = false;
            for (auto* u : unique) {
                if (u == f) { exists = true; break; }
            }
            if (!exists) unique.push_back(f);
        }

        RE::FormID resolvedFormID = templateNPC->GetFormID();
        std::string triedPrimary;
        std::string triedSecondary;
        std::string meshPath, tintPath;
        bool meshFound = false;

        for (size_t i = 0; i < unique.size(); ++i) {
            const RE::TESFile* plugin = unique[i];
            if (!plugin || !plugin->fileName) continue;
            RE::FormID faceID = FaceGenFileID(resolvedFormID, plugin);
            meshPath = BuildFaceGenMeshPath(plugin->fileName, faceID);
            tintPath = BuildFaceGenTintPath(plugin->fileName, faceID);

            RE::BSResourceNiBinaryStream meshStream(meshPath.c_str());
            if (i == 0) triedPrimary = meshPath; else triedSecondary = meshPath;

            if (meshStream.good()) {
                SKSE::log::info("AppearanceTemplate: Found FaceGen mesh: {}", meshPath);
                SKSE::log::info("AppearanceTemplate: FaceGen lookup - plugin: {}, FormID: {:08X}", plugin->fileName, faceID);
                meshFound = true;
                break;
            } else {
                SKSE::log::debug("AppearanceTemplate: FaceGen not found at: {}", meshPath);
            }
        }

        if (!meshFound) {
            SKSE::log::warn("AppearanceTemplate: FaceGen mesh not found!");
            if (!triedPrimary.empty()) SKSE::log::warn("AppearanceTemplate: Tried primary path: {}", triedPrimary);
            if (!triedSecondary.empty() && triedSecondary != triedPrimary) SKSE::log::warn("AppearanceTemplate: Tried fallback path: {}", triedSecondary);
            SKSE::log::warn("AppearanceTemplate: Falling back to record-only appearance copy");
            SKSE::log::warn("AppearanceTemplate: To fix, ensure FaceGen files exist or set TemplateFaceGenPlugin in INI");
            return false;
        }

        // Check if tint exists, optional but good to know
        bool tintExists = false;
        {
            RE::BSResourceNiBinaryStream tintStream(tintPath.c_str());
            tintExists = tintStream.good();
        }
        SKSE::log::info("AppearanceTemplate: FaceGen tint exists: {}", tintExists ? "yes" : "no");

        // Apply FaceGen by setting faceNPC to the template
        // This tells the game where to load FaceGen data from
        playerBase->faceNPC = templateNPC;
        SKSE::log::info("AppearanceTemplate: Set faceNPC to template ({:08X})", resolvedFormID);

        if (tintExists) {
            SKSE::log::info("AppearanceTemplate: FaceGen tint will be loaded from: {}", tintPath);
        }

        return true;
    }

    static RE::FormID ParseHexFormID(const std::string& str)
    {
        if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            return static_cast<RE::FormID>(std::stoul(str.substr(2), nullptr, 16));
        }
        return static_cast<RE::FormID>(std::stoul(str, nullptr, 16));
    }

    static RE::FormID BuildFormID(const RE::TESFile* file, RE::FormID baseFormID)
    {
        if (file->IsLight()) {
            uint32_t lightIndex = file->GetSmallFileCompileIndex();
            return 0xFE000000 | (lightIndex << 12) | (baseFormID & 0xFFF);
        }
        return (static_cast<RE::FormID>(file->GetCompileIndex()) << 24) | (baseFormID & 0x00FFFFFF);
    }

    static const RE::TESFile* FindPluginByName(RE::TESDataHandler* dh, const std::string& name)
    {
        for (auto* file : dh->files) {
            if (file && file->fileName && _stricmp(file->fileName, name.c_str()) == 0) {
                return file;
            }
        }
        return nullptr;
    }

    RE::FormID ResolveFormID(const std::string& formIdStr, const std::string& pluginName)
    {
        if (formIdStr.empty() || pluginName.empty()) {
            return 0;
        }

        RE::FormID baseFormID = 0;
        try {
            baseFormID = ParseHexFormID(formIdStr);
        } catch (...) {
            SKSE::log::error("AppearanceTemplate: Invalid FormID format: {}", formIdStr);
            return 0;
        }

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            SKSE::log::error("AppearanceTemplate: TESDataHandler not available");
            return 0;
        }

        const RE::TESFile* plugin = FindPluginByName(dataHandler, pluginName);
        if (!plugin) {
            SKSE::log::error("AppearanceTemplate: Plugin not found: {}", pluginName);
            return 0;
        }

        RE::FormID resolvedFormID = BuildFormID(plugin, baseFormID);
        if (RE::TESForm::LookupByID(resolvedFormID)) {
            SKSE::log::info("AppearanceTemplate: Resolved {}|{} to FormID {:08X}", formIdStr, pluginName, resolvedFormID);
            return resolvedFormID;
        }

        // Fallback: search all loaded plugins for the base FormID
        for (auto* file : dataHandler->files) {
            if (!file) continue;
            RE::FormID candidate = BuildFormID(file, baseFormID);
            if (RE::TESForm::LookupByID(candidate)) {
                SKSE::log::info("AppearanceTemplate: Resolved via fallback master {} to FormID {:08X}",
                    file->fileName ? file->fileName : "unknown", candidate);
                return candidate;
            }
        }

        SKSE::log::error("AppearanceTemplate: Failed to resolve FormID {} in any loaded plugin", formIdStr);
        return 0;
    }

    bool IsRaceCompatible(RE::TESNPC* templateNPC)
    {
        if (!templateNPC) return false;

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        auto playerBase = player->GetActorBase();
        if (!playerBase) return false;

        auto playerRace = playerBase->GetRace();
        auto templateRace = templateNPC->GetRace();

        if (!playerRace || !templateRace) return false;

        // Check if same race
        if (playerRace == templateRace) return true;

        // Check if races are in the same race group (e.g., human races)
        // For safety, we'll just check if they're the same
        SKSE::log::warn("AppearanceTemplate: Race mismatch - Player: {}, Template: {}",
            playerRace->GetFormEditorID() ? playerRace->GetFormEditorID() : "Unknown",
            templateRace->GetFormEditorID() ? templateRace->GetFormEditorID() : "Unknown");

        return false;
    }

    bool CopyAppearanceToPlayer(RE::TESNPC* templateNPC, bool includeRace, bool includeBody)
    {
        if (!templateNPC) {
            SKSE::log::error("AppearanceTemplate: Template NPC is null");
            return false;
        }

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            SKSE::log::error("AppearanceTemplate: Player not available");
            return false;
        }

        auto playerBase = player->GetActorBase();
        if (!playerBase) {
            SKSE::log::error("AppearanceTemplate: Player base actor not available");
            return false;
        }

        SKSE::log::info("AppearanceTemplate: Copying appearance from {} to player",
            templateNPC->GetName() ? templateNPC->GetName() : "Unknown NPC");

        // This MUST be done first, before head parts, as head parts are race-specific
        if (includeRace) {
            auto templateRace = templateNPC->GetRace();
            if (templateRace) {
                auto playerRace = playerBase->GetRace();
                if (playerRace != templateRace) {
                    SKSE::log::info("AppearanceTemplate: Changing race from {} to {}",
                        playerRace ? (playerRace->GetFormEditorID() ? playerRace->GetFormEditorID() : "Unknown") : "None",
                        templateRace->GetFormEditorID() ? templateRace->GetFormEditorID() : "Unknown");

                    // Set the race on the player's base actor
                    playerBase->race = templateRace;

                    // Mark race as changed
                    playerBase->AddChange(RE::TESNPC::ChangeFlags::ChangeFlag::kRace);

                    SKSE::log::info("AppearanceTemplate: Race changed successfully");
                } else {
                    SKSE::log::info("AppearanceTemplate: Race already matches, skipping");
                }
            }

            // Must match for head parts and animations to work correctly
            bool templateIsFemale = templateNPC->IsFemale();
            bool playerIsFemale = playerBase->IsFemale();
            if (templateIsFemale != playerIsFemale) {
                SKSE::log::info("AppearanceTemplate: Changing sex from {} to {}",
                    playerIsFemale ? "Female" : "Male",
                    templateIsFemale ? "Female" : "Male");

                if (templateIsFemale) {
                    playerBase->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
                } else {
                    playerBase->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kFemale);
                }

                // Mark gender as changed
                playerBase->AddChange(RE::TESNPC::ChangeFlags::ChangeFlag::kGender);

                SKSE::log::info("AppearanceTemplate: Sex changed successfully");
            }
        } else {
            // Check race compatibility if not copying race
            if (!IsRaceCompatible(templateNPC)) {
                SKSE::log::warn("AppearanceTemplate: Race mismatch - appearance may not work correctly!");
                SKSE::log::warn("AppearanceTemplate: Consider enabling TemplateIncludeRace = true");
            }
        }

        // Head parts include: Eyes, hair, facial hair, scars, brows, etc.
        if (templateNPC->headParts && templateNPC->numHeadParts > 0) {
            // Clear existing head parts
            if (playerBase->headParts) {
                RE::free(playerBase->headParts);
                playerBase->headParts = nullptr;
            }
            playerBase->numHeadParts = 0;

            // Allocate and copy head parts
            playerBase->numHeadParts = templateNPC->numHeadParts;
            playerBase->headParts = RE::calloc<RE::BGSHeadPart*>(templateNPC->numHeadParts);
            if (playerBase->headParts) {
                for (uint8_t i = 0; i < templateNPC->numHeadParts; ++i) {
                    playerBase->headParts[i] = templateNPC->headParts[i];
                    auto* part = templateNPC->headParts[i];
                    if (part) {
                        const char* typeName = "Unknown";
                        switch (part->type.get()) {
                            case RE::BGSHeadPart::HeadPartType::kMisc: typeName = "Misc"; break;
                            case RE::BGSHeadPart::HeadPartType::kFace: typeName = "Face"; break;
                            case RE::BGSHeadPart::HeadPartType::kEyes: typeName = "Eyes"; break;
                            case RE::BGSHeadPart::HeadPartType::kHair: typeName = "Hair"; break;
                            case RE::BGSHeadPart::HeadPartType::kFacialHair: typeName = "FacialHair"; break;
                            case RE::BGSHeadPart::HeadPartType::kScar: typeName = "Scar"; break;
                            case RE::BGSHeadPart::HeadPartType::kEyebrows: typeName = "Eyebrows"; break;
                            default: break;
                        }
                        SKSE::log::info("AppearanceTemplate:   [{}] {} - {} ({:08X})",
                            i, typeName,
                            part->GetFormEditorID() ? part->GetFormEditorID() : "(no editor ID)",
                            part->GetFormID());
                    }
                }
                SKSE::log::info("AppearanceTemplate: Copied {} head parts", templateNPC->numHeadParts);
            }
        }

        if (templateNPC->headRelatedData) {
            if (!playerBase->headRelatedData) {
                playerBase->headRelatedData = std::make_unique<RE::TESNPC::HeadRelatedData>().release();
            }

            // Hair color
            if (templateNPC->headRelatedData->hairColor) {
                playerBase->headRelatedData->hairColor = templateNPC->headRelatedData->hairColor;
                SKSE::log::info("AppearanceTemplate: Copied hair color");
            }

            // Face texture set, skin detail textures
            if (templateNPC->headRelatedData->faceDetails) {
                playerBase->headRelatedData->faceDetails = templateNPC->headRelatedData->faceDetails;
                SKSE::log::info("AppearanceTemplate: Copied face texture set");
            }
        }

        playerBase->bodyTintColor = templateNPC->bodyTintColor;
        SKSE::log::info("AppearanceTemplate: Copied body tint color");

        if (Settings::TemplateCopySkin) {
            if (templateNPC->farSkin) {
                playerBase->farSkin = templateNPC->farSkin;
                SKSE::log::info("AppearanceTemplate: Copied far skin");
            }

            // Copy skin form if available
            if (templateNPC->skin) {
                playerBase->skin = templateNPC->skin;
                SKSE::log::info("AppearanceTemplate: Copied skin form");
            }
        } else {
            SKSE::log::debug("AppearanceTemplate: Skin copy disabled (TemplateCopySkin = false)");
        }

        // Tint layers include: Skin tone, makeup, war paint, dirt, etc.
        if (auto templateTints = templateNPC->tintLayers) {
            // Get or create player tint layers
            if (!playerBase->tintLayers) {
                playerBase->tintLayers = std::make_unique<RE::BSTArray<RE::TESNPC::Layer*>>().release();
            } else {
                // Clear existing tints (unique_ptr handles deletion)
                for (auto* layer : *playerBase->tintLayers) {
                    auto cleanup = std::unique_ptr<RE::TESNPC::Layer>(layer);
                }
                playerBase->tintLayers->clear();
            }

            // Copy tint layers
            for (auto* srcLayer : *templateTints) {
                if (srcLayer) {
                    auto newLayer = std::make_unique<RE::TESNPC::Layer>();
                    newLayer->tintIndex = srcLayer->tintIndex;
                    newLayer->tintColor = srcLayer->tintColor;
                    newLayer->preset = srcLayer->preset;
                    newLayer->interpolationValue = srcLayer->interpolationValue;
                    playerBase->tintLayers->push_back(newLayer.release());
                }
            }
            SKSE::log::info("AppearanceTemplate: Copied {} tint layers", playerBase->tintLayers->size());
        }

        playerBase->weight = templateNPC->weight;
        SKSE::log::info("AppearanceTemplate: Copied weight: {}", templateNPC->weight);

        // Face morphs control the facial structure
        if (templateNPC->faceNPC) {
            playerBase->faceNPC = templateNPC->faceNPC;
            SKSE::log::info("AppearanceTemplate: Copied face NPC reference");
        }

        // Copy face data if present
        if (templateNPC->faceData && playerBase->faceData) {
            // Copy morph sliders
            for (int i = 0; i < RE::TESNPC::FaceData::Morphs::kTotal; ++i) {
                playerBase->faceData->morphs[i] = templateNPC->faceData->morphs[i];
            }

            // Copy face parts
            for (int i = 0; i < RE::TESNPC::FaceData::Parts::kTotal; ++i) {
                playerBase->faceData->parts[i] = templateNPC->faceData->parts[i];
            }
            SKSE::log::info("AppearanceTemplate: Copied face morphs and parts");
        } else if (templateNPC->faceData && !playerBase->faceData) {
            // Allocate face data for player if needed
            playerBase->faceData = std::make_unique<RE::TESNPC::FaceData>().release();
            for (int i = 0; i < RE::TESNPC::FaceData::Morphs::kTotal; ++i) {
                playerBase->faceData->morphs[i] = templateNPC->faceData->morphs[i];
            }
            for (int i = 0; i < RE::TESNPC::FaceData::Parts::kTotal; ++i) {
                playerBase->faceData->parts[i] = templateNPC->faceData->parts[i];
            }
            SKSE::log::info("AppearanceTemplate: Created and copied face morphs");
        }

        if (includeBody) {
            // Copy height
            playerBase->height = templateNPC->height;
            SKSE::log::info("AppearanceTemplate: Copied height: {}", templateNPC->height);

            // Note: Skin is race-dependent and copying it can cause crashes
            // Only height is safe to copy across different setups
        }

        // Mark appearance as changed
        playerBase->AddChange(RE::TESNPC::ChangeFlags::ChangeFlag::kFace);

        return true;
    }

    /**
     * Call BSFaceGenManager::RegenerateHead via REL::Relocation.
     * Fully reloading baked FaceGen meshes after faceNPC swap.
     * Not exposed in CommonLibSSE-NG headers, so we call it directly.
     */
    static void RegenerateHead(RE::Actor* a_actor)
    {
        if (!a_actor) return;

        auto faceGenManager = RE::BSFaceGenManager::GetSingleton();
        if (!faceGenManager) {
            SKSE::log::warn("AppearanceTemplate: BSFaceGenManager not available for RegenerateHead");
            return;
        }

        // BSFaceGenManager::RegenerateHead(Actor*)
        // SSE 1.5.97: ID 26257, AE: ID 26836
        using RegenerateHead_t = void(*)(RE::BSFaceGenManager*, RE::Actor*);
        REL::Relocation<RegenerateHead_t> RegenerateHeadFunc{ RELOCATION_ID(26257, 26836) };

        RegenerateHeadFunc(faceGenManager, a_actor);
        SKSE::log::info("AppearanceTemplate: Called RegenerateHead for full FaceGen reload");
    }

    void UpdatePlayerAppearance()
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        auto playerBase = player->GetActorBase();

        // Queue update for next frame
        SKSE::GetTaskInterface()->AddTask([player, playerBase]() {
            if (player) {
                // Update hair color first
                player->UpdateHairColor();

                // Update skin color
                player->UpdateSkinColor();

                // RegenerateHead fully reloads baked FaceGen mesh
                RegenerateHead(player);

                // Force 3D model reset to apply remaining appearance changes
                // This also handles NiNode updates internally
                player->DoReset3D(true);

                // Additional 3D model update for armor/equipment refresh
                player->Update3DModel();

                // Update neck seam after head changes
                if (playerBase) {
                    auto faceNode = player->GetFaceNodeSkinned();
                    if (faceNode) {
                        playerBase->UpdateNeck(faceNode);
                        SKSE::log::debug("AppearanceTemplate: Updated neck seam");
                    }
                }

                SKSE::log::info("AppearanceTemplate: Player appearance update completed");
            }
        });
    }

    static void ProcessSpawnedActor(RE::ObjectRefHandle handle, int framesRemaining)
    {
        if (framesRemaining > 0) {
            SKSE::GetTaskInterface()->AddTask([handle, framesRemaining]() {
                ProcessSpawnedActor(handle, framesRemaining - 1);
            });
            return;
        }

        auto spawnedRef = handle.get();
        if (!spawnedRef) {
            SKSE::log::warn("AppearanceTemplate: Spawned actor no longer valid");
            return;
        }

        auto* tempActor = spawnedRef->As<RE::Actor>();
        auto* player = RE::PlayerCharacter::GetSingleton();

        if (tempActor && player && Settings::TemplateCopyOutfit) {
            SKSE::log::info("AppearanceTemplate: Copying outfit from temporary actor...");
            CopyOutfitFromActor(tempActor, player);
        }

        spawnedRef->Disable();
        SKSE::log::info("AppearanceTemplate: Temporary actor disabled");
    }

    bool ApplyIfConfigured()
    {
        // Only apply once per session
        if (GetState().applied) {
            SKSE::log::debug("AppearanceTemplate: Already applied this session");
            return true;
        }

        // Check if feature is enabled
        if (!Settings::UseTemplateAppearance) {
            SKSE::log::debug("AppearanceTemplate: Feature disabled in settings");
            return false;
        }

        // Check if template is configured
        if (Settings::TemplateFormID.empty() || Settings::TemplatePlugin.empty()) {
            SKSE::log::warn("AppearanceTemplate: Enabled but no template configured");
            return false;
        }

        SKSE::log::info("AppearanceTemplate: Applying template {}|{}",
            Settings::TemplateFormID, Settings::TemplatePlugin);

        // Resolve the FormID
        RE::FormID resolvedID = ResolveFormID(Settings::TemplateFormID, Settings::TemplatePlugin);
        if (resolvedID == 0) {
            SKSE::log::error("AppearanceTemplate: Failed to resolve FormID");
            return false;
        }

        // Store template info for FaceGen paths
        GetState().plugin = Settings::TemplatePlugin;
        GetState().formID = resolvedID;

        // Look up the NPC
        auto form = RE::TESForm::LookupByID(resolvedID);
        if (!form) {
            SKSE::log::error("AppearanceTemplate: Form not found for ID {:08X}", resolvedID);
            return false;
        }

        auto templateNPC = form->As<RE::TESNPC>();
        if (!templateNPC) {
            SKSE::log::error("AppearanceTemplate: Form {:08X} is not an NPC (type: {})",
                resolvedID, static_cast<int>(form->GetFormType()));
            return false;
        }

        // FaceGen and head parts require compatible races
        bool racesCompatible = IsRaceCompatible(templateNPC);
        if (!racesCompatible && !Settings::TemplateIncludeRace) {
            SKSE::log::warn("AppearanceTemplate: Race mismatch detected!");
            SKSE::log::warn("AppearanceTemplate: FaceGen may not work correctly without TemplateIncludeRace = true");
        }

        // Apply the record-based appearance
        if (!CopyAppearanceToPlayer(templateNPC, Settings::TemplateIncludeRace, Settings::TemplateIncludeBody)) {
            SKSE::log::error("AppearanceTemplate: Failed to copy appearance");
            return false;
        }

        // Only apply FaceGen if races are compatible or we're copying race
        if (Settings::TemplateCopyFaceGen) {
            if (racesCompatible || Settings::TemplateIncludeRace) {
                // ApplyFaceGen will auto-detect the winning file for FaceGen paths
                bool faceGenApplied = ApplyFaceGen(templateNPC, Settings::TemplatePlugin, resolvedID);
                if (!faceGenApplied) {
                    SKSE::log::warn("AppearanceTemplate: FaceGen not applied - falling back to record data only");
                    // This is not a failure - we still have the record-based appearance
                }
            } else {
                SKSE::log::warn("AppearanceTemplate: Skipping FaceGen due to race mismatch");
                SKSE::log::warn("AppearanceTemplate: Enable TemplateIncludeRace to copy FaceGen across races");
            }
        } else {
            SKSE::log::info("AppearanceTemplate: FaceGen copy disabled in settings");
        }

        // Copy outfit if a loaded actor is available
        auto player = RE::PlayerCharacter::GetSingleton();
        RE::Actor* templateActor = FindActorByBase(templateNPC);

        if (templateActor && Settings::TemplateCopyOutfit && player) {
            SKSE::log::info("AppearanceTemplate: Found loaded actor for template NPC, copying outfit");
            CopyOutfitFromActor(templateActor, player);
        } else if (Settings::TemplateCopyOutfit && player) {
            // No actor loaded - spawn a temporary one to copy outfit from
            SKSE::log::info("AppearanceTemplate: No loaded actor found, spawning temporary actor for outfit...");

            auto spawned = player->PlaceObjectAtMe(templateNPC, false);
            if (spawned) {
                auto* spawnedActor = spawned->As<RE::Actor>();
                if (spawnedActor) {
                    SKSE::log::info("AppearanceTemplate: Spawned temporary actor {:08X}", spawnedActor->GetFormID());

                    RE::ObjectRefHandle spawnedHandle = spawnedActor->GetHandle();

                    // Wait 5 frames for actor to fully load, then copy outfit
                    ProcessSpawnedActor(spawnedHandle, 5);
                } else {
                    SKSE::log::warn("AppearanceTemplate: Spawned reference is not an actor");
                    spawned->Disable();
                    spawned->SetDelete(true);
                }
            } else {
                SKSE::log::warn("AppearanceTemplate: Failed to spawn temporary actor");
            }
        }

        // Update the player's 3D
        UpdatePlayerAppearance();

        GetState().applied = true;
        SKSE::log::info("AppearanceTemplate: Successfully applied template appearance");

        return true;
    }

    /**
     * Copy equipped outfit from source actor to player.
     * Copies all equipped armor items (not weapons).
     */
    static bool CopyOutfitFromActor(RE::Actor* sourceActor, RE::Actor* player)
    {
        if (!sourceActor || !player) {
            return false;
        }

        auto sourceInv = sourceActor->GetInventory();
        int copiedCount = 0;

        SKSE::log::info("whois: Copying outfit from source actor...");

        // Iterate through source's inventory to find equipped armor
        for (const auto& [form, data] : sourceInv) {
            if (!form || data.second->IsWorn() == false) continue;

            auto armor = form->As<RE::TESObjectARMO>();
            if (!armor) continue;

            // Check if player already has this item
            auto playerInv = player->GetInventory();
            bool hasItem = false;
            for (const auto& [pForm, pData] : playerInv) {
                if (pForm != form) continue;
                hasItem = true;
                if (!pData.second->IsWorn()) {
                    RE::ActorEquipManager::GetSingleton()->EquipObject(player, armor);
                }
                break;
            }

            if (!hasItem) {
                // Add the item to player's inventory and equip it
                player->AddObjectToContainer(armor, nullptr, 1, nullptr);
                RE::ActorEquipManager::GetSingleton()->EquipObject(player, armor);
                copiedCount++;
                SKSE::log::debug("whois: Added and equipped {}", armor->GetName());
            }
        }

        if (copiedCount > 0) {
            SKSE::log::info("whois: Copied {} armor items from source", copiedCount);
        } else {
            SKSE::log::info("whois: No new armor items to copy (player already has them or source has none)");
        }

        return copiedCount > 0;
    }
}
