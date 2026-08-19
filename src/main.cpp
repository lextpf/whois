//  ============================================================================================
//                                                             ⠀    ⠀⠀⡄⠀⠀⠀⠀⠀⠀⣠⠀⠀⢀⠀⠀⢠⠀⠀⠀
//                                                             ⠀     ⢸⣧⠀⠀⠀⠀⢠⣾⣇⣀⣴⣿⠀⠀⣼⡇⠀⠀
//                                                                ⠀⠀⣾⣿⣧⠀⠀⢀⣼⣿⣿⣿⣿⣿⠀⣼⣿⣷⠀⠀
//                                                                ⠀⢸⣿⣿⣿⡀⠀⠸⠿⠿⣿⣿⣿⡟⢀⣿⣿⣿⡇⠀
//        ::::::::  :::     :::   ::: :::::::::  :::    :::       ⠀⣾⣿⣿⣿⣿⡀⠀⢀⣼⣿⣿⡿⠁⣿⣿⣿⣿⣷⠀
//       :+:    :+: :+:     :+:   :+: :+:    :+: :+:    :+:       ⢸⣿⣿⣿⣿⠁⣠⣤⣾⣿⣿⣯⣤⣄⠙⣿⣿⣿⣿⡇
//       +:+        +:+      +:+ +:+  +:+    +:+ +:+    +:+       ⣿⣿⣿⣿⣿⣶⣿⣿⣿⣿⣿⣿⣿⣿⣶⣿⣿⣿⣿⣿
//       :#:        +#+       +#++:   +#++:++#+  +#++:++#++       ⠘⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡏
//       +#+   +#+# +#+        +#+    +#+        +#+    +#+       ⠀⠘⢿⣿⣿⣿⠛⠻⢿⣿⣿⣿⠹⠟⣿⣿⣿⣿⣿⠀
//       #+#    #+# #+#        #+#    #+#        #+#    #+#       ⠀⠀⠘⢿⣿⣿⣦⡄⢸⣿⣿⣿⡇⠠⣿⣿⣿⣿⡇⠀
//        ########  ########## ###    ###        ###    ###       ⠀⠀⠀⠘⢿⣿⣿⠀⣸⣿⣿⣿⠇⠀⠙⣿⣿⣿⠁⠀
//                                                                ⠀⠀⠀⠀⠘⣿⠃⢰⣿⣿⣿⡇⠀⠀⠀⠈⢻⡇⠀⠀
//                                                                ⠀⠀⠀⠀⠀⠈⠀⠈⢿⣿⣿⣿⣶⡶⠂⠀⠀⠁⠀⠀
//                                << S K Y R I M   P L U G I N >>         ⠀⠀⠈⠻⣿⡿⠋⠀⠀⠀⠀⠀⠀⠀
//
//  ============================================================================================
//
//      An SKSE plugin for Skyrim SE/AE that renders an ImGui overlay displaying
//      actor nameplates via the game's D3D11 pipeline.
//
//    ----------------------------------------------------------------------
//
//      Repository:   https://github.com/lextpf/glyph
//      License:      MIT

#include "PCH.hpp"

#include "ConsoleCommands.hpp"
#include "Hooks.hpp"
#include "HudCompat.hpp"
#include "ProjectManifest.hpp"
#include "Renderer.hpp"
#include "Settings.hpp"

#include <string>

namespace
{
// On "RaceMenu" close, request a player identity refresh so a rename reaches
// the plate. The live name is logged to confirm the engine already applied it.
// RE::UI dispatches menu events on the game thread, so the RE::* dereference in
// ProcessEvent is legal. The refresh only sets a flag: Renderer::Draw() consumes
// it on the render thread, drops the player cache entry, and clears the snapshot
// pause so the next game-thread update republishes the name.
class RaceMenuCloseSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    static RaceMenuCloseSink* GetSingleton()
    {
        static RaceMenuCloseSink s;
        return &s;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        // String literal (not RE::RaceSexMenu::MENU_NAME) to avoid a
        // BSFixedString-vs-string_view comparison ambiguity; the value is stable.
        if (e && !e->opening && e->menuName == "RaceSex Menu")
        {
            if (auto* pc = RE::PlayerCharacter::GetSingleton())
            {
                const char* nm = pc->GetDisplayFullName();
                logger::info("RaceSex closed; player GetDisplayFullName() = '{}'",
                             nm ? nm : "(null)");
            }
            Renderer::RequestIdentityRefresh();
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
}  // namespace

// One CommonLibSSE-NG build supports SE 1.5.x and AE 1.6.x (Steam and GOG).
// Address Library resolves the runtime-specific IDs; CommonLib handles the
// pre/post-1.6.629 structure layouts.
SKSEPluginInfo(.Version = REL::Version(0, 1, 0, 0),
               .Name = "glyph",
               .Author = "lextpf | powerof3 | expired6978",
               .StructCompatibility = SKSE::StructCompatibility::Independent,
               .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

    // SKSE message handler for plugin lifecycle events.
    void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type)
    {
        case SKSE::MessagingInterface::kPostLoad:
            logger::debug("Post load event received");
            break;

        case SKSE::MessagingInterface::kPostPostLoad:
            // Every PostLoad handler has run; SKEE may send its interface here.
            logger::debug("PostPostLoad event received");
            // Every SKSE plugin is loaded, so request the TrueHUD API and detect
            // the moreHUD SKSE DLL. That detection keeps the two overlays from
            // both labelling the same actor. The .esp/.esl fallback inside
            // HudCompat::Initialize needs the TESDataHandler mod list, which is
            // empty until kDataLoaded, so that branch does nothing here. This is
            // the only call site, so the fallback never contributes: in practice
            // the AHZmoreHUDPlugin.dll module probe is what detects moreHUD.
            HudCompat::Initialize();
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            logger::debug("Data loaded event received");
            ConsoleCommands::Register();
            // RaceMenu rename -> prompt player-name refresh (see RaceMenuCloseSink).
            if (auto* ui = RE::UI::GetSingleton())
            {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(RaceMenuCloseSink::GetSingleton());
            }
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
            logger::debug("Post load game event received");
            break;

        case SKSE::MessagingInterface::kNewGame:
            logger::debug("New game event received");
            break;
    }
}

// SKSE plugin entry point. Rejects any runtime that is not SE or AE, allocates
// the trampoline, starts logging, loads settings and the asset manifest,
// registers the message listener, then installs the hooks.
//
// The order is a constraint, not a preference. Hooks::Install() must run last:
// the CreateD3DAndSwapChain hook can fire as soon as it is installed, and the
// ImGui init it drives reads font sizes from Settings and font paths from
// ProjectManifest. Both must already be loaded. SKSE::AllocTrampoline must run
// before Install() because the call hook draws from that trampoline.
//
// Returns false to abort loading: unsupported runtime, or no SKSE log directory.
extern "C" __declspec(dllexport) bool __cdecl SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    using namespace std::literals;

    const auto runtime = REL::Module::GetRuntime();
    if (runtime != REL::Module::Runtime::SE && runtime != REL::Module::Runtime::AE)
    {
        return false;
    }

    SKSE::Init(a_skse);

    static constexpr std::size_t TRAMPOLINE_SIZE = 256;
    SKSE::AllocTrampoline(TRAMPOLINE_SIZE);

    // glyph.log in the SKSE log directory, truncated on each launch.
    auto path = logger::log_directory();
    if (!path)
    {
        return false;
    }

    *path /= "glyph.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

    log->set_level(spdlog::level::debug);  // Debug level carries the draw diagnostics
    log->flush_on(spdlog::level::debug);   // Flush at debug too, so a crash keeps the tail

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v"s);

    logger::info("glyph loaded on Skyrim {} ({})",
                 REL::Module::get().version().string("."),
                 runtime == REL::Module::Runtime::AE ? "AE/GOG" : "SE");
    Settings::Load();

    // Resolve the obfuscated asset manifest: fonts, tier emblems, particles. A
    // missing or invalid manifest is not fatal; each loader falls back to its
    // built-in default.
    ProjectManifest::Load();

    // Lifecycle messages drive the rest of the setup (see MessageHandler).
    auto messaging = SKSE::GetMessagingInterface();
    if (messaging)
    {
        messaging->RegisterListener(MessageHandler);
        logger::debug("Registered SKSE message listener");
    }

    Hooks::Install();

    return true;
}
