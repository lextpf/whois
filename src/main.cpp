/*  ============================================================================================  *
 *                                                             ⠀    ⠀⠀⡄⠀⠀⠀⠀⠀⠀⣠⠀⠀⢀⠀⠀⢠⠀⠀⠀
 *                                                             ⠀     ⢸⣧⠀⠀⠀⠀⢠⣾⣇⣀⣴⣿⠀⠀⣼⡇⠀⠀
 *                                                                ⠀⠀⣾⣿⣧⠀⠀⢀⣼⣿⣿⣿⣿⣿⠀⣼⣿⣷⠀⠀
 *                                                                ⠀⢸⣿⣿⣿⡀⠀⠸⠿⠿⣿⣿⣿⡟⢀⣿⣿⣿⡇⠀
 *    :::       ::: :::    :::  :::::::: ::::::::::: ::::::::     ⠀⣾⣿⣿⣿⣿⡀⠀⢀⣼⣿⣿⡿⠁⣿⣿⣿⣿⣷⠀
 *    :+:       :+: :+:    :+: :+:    :+:    :+:    :+:    :+:    ⢸⣿⣿⣿⣿⠁⣠⣤⣾⣿⣿⣯⣤⣄⠙⣿⣿⣿⣿⡇
 *    +:+       +:+ +:+    +:+ +:+    +:+    +:+    +:+           ⣿⣿⣿⣿⣿⣶⣿⣿⣿⣿⣿⣿⣿⣿⣶⣿⣿⣿⣿⣿
 *    +#+  +:+  +#+ +#++:++#++ +#+    +:+    +#+    +#++:++#++    ⠘⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡏
 *    +#+ +#+#+ +#+ +#+    +#+ +#+    +#+    +#+           +#+    ⠀⠘⢿⣿⣿⣿⠛⠻⢿⣿⣿⣿⠹⠟⣿⣿⣿⣿⣿⠀
 *     #+#+# #+#+#  #+#    #+# #+#    #+#    #+#    #+#    #+#    ⠀⠀⠘⢿⣿⣿⣦⡄⢸⣿⣿⣿⡇⠠⣿⣿⣿⣿⡇⠀
 *      ###   ###   ###    ###  ######## ########### ########     ⠀⠀⠀⠘⢿⣿⣿⠀⣸⣿⣿⣿⠇⠀⠙⣿⣿⣿⠁⠀
 *                                                                ⠀⠀⠀⠀⠘⣿⠃⢰⣿⣿⣿⡇⠀⠀⠀⠈⢻⡇⠀⠀
 *                                                                ⠀⠀⠀⠀⠀⠈⠀⠈⢿⣿⣿⣿⣶⡶⠂⠀⠀⠁⠀⠀
 *                                << S K Y R I M   P L U G I N >>         ⠀⠀⠈⠻⣿⡿⠋⠀⠀⠀⠀⠀⠀⠀
 *                                                                                                  
 *  ============================================================================================  *
 * 
 *      An SKSE plugin for Skyrim SE/AE that renders an ImGui overlay
 *      displaying actor information and allows copying NPC appearance
 *      templates onto the player character via the game's D3D11 pipeline.
 *
 *    ----------------------------------------------------------------------
 *
 *      Repository:   https://github.com/lextpf/whois
 *      License:      MIT
 */
#include "PCH.h"

#include <atomic>
#include <string>

#include "AppearanceTemplate.h"
#include "Hooks.h"
#include "Renderer.h"
#include "Settings.h"

namespace ConsoleCommands
{
    /**
     * whois console command.
     * Usage: Type 'whois' in console to toggle nameplate rendering on/off.
     */
    bool WhoisExecute(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*,
                      RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*,
                      double&, std::uint32_t&)
    {
        auto console = RE::ConsoleLog::GetSingleton();
        bool newState = Renderer::ToggleEnabled();

        if (console) {
            if (newState) {
                console->Print("whois: Nameplate rendering ENABLED");
            } else {
                console->Print("whois: Nameplate rendering DISABLED");
            }
        }

        logger::info("whois: Rendering toggled to {}", newState ? "ON" : "OFF");

        return true;
    }

    void Register()
    {
        logger::info("Registering whois console command...");

        auto* commands = RE::SCRIPT_FUNCTION::GetFirstConsoleCommand();
        if (!commands) {
            logger::error("Failed to get console command table");
            return;
        }

        std::uint32_t commandCount = RE::SCRIPT_FUNCTION::Commands::kConsoleCommandsEnd -
                                     RE::SCRIPT_FUNCTION::Commands::kConsoleOpBase;

        // Find an unused command slot to replace
        bool found = false;
        for (std::uint32_t i = 0; i < commandCount && !found; ++i) {
            auto* cmd = &commands[i];
            if (!cmd || !cmd->functionName) continue;

            // Replace TestSeenData
            if (_stricmp(cmd->functionName, "TestSeenData") == 0) {
                cmd->functionName = "whois";
                cmd->shortName = "";
                cmd->helpString = "Toggle nameplate rendering on/off";
                cmd->referenceFunction = false;
                cmd->executeFunction = WhoisExecute;
                cmd->numParams = 0;
                cmd->params = nullptr;
                found = true;
                logger::info("Registered 'whois' console command");
            }
        }

        if (!found) {
            logger::warn("Could not find slot for whois command");
        } else {
            logger::info("Usage: Type 'whois' to toggle nameplate rendering");
        }
    }
}

// Flag to indicate we're waiting to apply appearance template
static std::atomic<bool> s_pendingAppearanceApply{false};
static std::atomic<int> s_checkCount{0};

void CheckPendingAppearanceTemplate()
{
    // Log once to confirm function is being called
    static bool loggedOnce = false;
    if (!loggedOnce) {
        logger::info("CheckPendingAppearanceTemplate called for first time");
        loggedOnce = true;
    }

    if (!s_pendingAppearanceApply.load()) {
        return;
    }

    // Log every 60 frames to avoid spam
    int count = s_checkCount.fetch_add(1);
    bool shouldLog = (count % 60 == 0);

    auto player = RE::PlayerCharacter::GetSingleton();
    auto playerBase = player ? player->GetActorBase() : nullptr;
    bool is3DLoaded = player ? player->Is3DLoaded() : false;

    if (shouldLog) {
        logger::debug("Appearance check #{}: player={}, base={}, 3D={}",
            count, player != nullptr, playerBase != nullptr, is3DLoaded);
    }

    // Check if player is fully initialized and in game world
    if (player && playerBase && is3DLoaded) {
        logger::info("Player ready after {} checks, applying appearance template", count);
        s_pendingAppearanceApply.store(false);
        s_checkCount.store(0);
        AppearanceTemplate::ApplyIfConfigured();
    }
}

void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            logger::debug("Post load event received");
            break;

        case SKSE::MessagingInterface::kPostPostLoad:
            // All PostLoad handlers have run, SKEE might send interface here
            logger::debug("PostPostLoad event received");
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            logger::debug("Data loaded event received");
            ConsoleCommands::Register();
            // Retry getting NiOverride interface, SKEE should be fully loaded by now
            AppearanceTemplate::RetryNiOverrideInterface();
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
            // Loading a save, player should be available soon
            logger::debug("Post load game event received");
            if (Settings::UseTemplateAppearance) {
                s_pendingAppearanceApply.store(true);
            }
            // Test overlay interface after game load
            AppearanceTemplate::TestOverlayOnPlayer();
            break;

        case SKSE::MessagingInterface::kNewGame:
            // New game, player won't exist until after character creation
            logger::debug("New game event received - will apply after character creation");
            logger::info("UseTemplateAppearance={}, FormID={}, Plugin={}",
                Settings::UseTemplateAppearance, Settings::TemplateFormID, Settings::TemplatePlugin);
            if (Settings::UseTemplateAppearance) {
                s_pendingAppearanceApply.store(true);
                logger::info("Pending appearance flag set to TRUE");
            } else {
                logger::warn("UseTemplateAppearance is FALSE, not setting pending flag");
            }
            break;
    }
}

/**
 * SKSE plugin load entry point.
 *
 * Called by SKSE after the plugin DLL is loaded. Initializes logging,
 * loads settings, and installs hooks.
 *
 * @param a_skse SKSE load interface.
 *
 * @return `true` if initialization succeeded.
 */
extern "C" __declspec(dllexport) bool __cdecl SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SKSE::AllocTrampoline(1 << 8);

    Settings::Load();

    // Setup logging
    auto path = logger::log_directory();
    if (!path) {
        return false;
    }

    *path /= "whois.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

    log->set_level(spdlog::level::debug);  // Enable debug to see draw diagnostics
    log->flush_on(spdlog::level::debug);   // Flush debug too so heartbeat shows in file

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v"s);

    logger::debug("whois loaded");

    // Register for SKSE messages
    auto messaging = SKSE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(MessageHandler);
        logger::debug("Registered SKSE message listener");

        // Register for NiOverride/SKEE interface exchange
        // This must happen before PostLoad so we receive the interface broadcast
        // TODO: This does not work at all on 1.5.97, newer Racemenu (4.19 on 1.6.xx)
        AppearanceTemplate::QueryNiOverrideInterface();
    }

    Hooks::Install();

    return true;
}

/**
 * SKSE plugin version information.
 *
 * Provides version, name, and compatibility info to SKSE.
 */
extern "C" __declspec(dllexport) constinit const auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData version;
    version.PluginVersion(REL::Version(0, 1, 0, 0));
    version.PluginName("whois");
    version.AuthorName("lextpf | powerof3 | expired6978");
    version.UsesAddressLibrary(true);
    return version;
}();

/**
 * SKSE plugin query entry point.
 *
 * Called by SKSE during plugin enumeration. Provides basic plugin info.
 *
 * @param a_info Output plugin information structure.
 *
 * @return `true` always.
 */
extern "C" __declspec(dllexport) bool __cdecl SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = "whois";
    a_info->version = 1;
    return true;
}
