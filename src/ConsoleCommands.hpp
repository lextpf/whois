#pragma once

/**
 * @namespace ConsoleCommands
 * @brief Skyrim console command handlers for the glyph plugin.
 * @author Fable 5 (https://github.com/claude)
 * @ingroup Core
 *
 * Takes over the engine's unused `TestSeenData` console-command slot with a `glyph`
 * dispatcher that exposes several sub-commands. The raw command text is read from the
 * `Script*` parameter through `Script::GetCommand()` and tokenized in-process.
 *
 * Any non-empty leading prefix of a sub-command is accepted (`h`, `s`, `n`, `p`, `d`). The
 * sub-command initials are all distinct, so a single letter is never ambiguous.
 *
 * | Sub-command                              | Effect                                      |
 * |------------------------------------------|---------------------------------------------|
 * | `glyph`                                  | Toggle nameplate rendering (default action) |
 * | `glyph help`                             | Print available sub-commands                |
 * | `glyph ?`                                | Alias for `help`                            |
 * | `glyph status`                           | Print current state of every toggle         |
 * | `glyph nameplates [on\|off]`             | Set / toggle nameplate rendering            |
 * | `glyph plates [on\|off]`                 | Alias for `nameplates`                      |
 * | `glyph debug [on\|off]`                  | Set / toggle debug overlay                  |
 *
 * @note The sub-commands change live state only. They are never written to `glyph.ini` and
 *       they reset on restart. An argument of `on`, `1`, `true`, or `yes` enables the toggle;
 *       `off`, `0`, `false`, or `no` disables it; any other token, or no token, toggles it.
 *
 * ## :material-console: Example Session
 *
 * ```text
 * > glyph help
 * glyph - sub-commands (any unambiguous prefix works, e.g. n / d / s):
 *   glyph                              toggle nameplate rendering
 *   glyph help | ?                     show this help
 *   glyph status | s                   print current state
 *   glyph nameplates | n [on|off]      enable / disable / toggle nameplates
 *   glyph plates | p [on|off]          alias for 'nameplates'
 *   glyph debug | d [on|off]           enable / disable / toggle debug overlay
 *
 * > glyph debug on
 * glyph: debug overlay ENABLED
 * ```
 */
namespace ConsoleCommands
{
/**
 * @brief Register the glyph console command dispatcher.
 *
 * Overwrites the unused `TestSeenData` slot in the SCRIPT_FUNCTION table with the `glyph`
 * command and an empty parameter list.
 *
 * @pre Call once from the SKSE message handler on `kDataLoaded` (see `MessageHandler` in
 *      `main.cpp`), when the engine's console command table is populated.
 * @post `glyph` and its sub-commands are available at the in-game console.
 * @note The dispatcher runs on the game thread. The two toggles reach the render thread by
 *       different routes, so do not look for one mechanism:
 *       - `nameplates` / `plates` / bare `glyph` call Renderer::SetEnabled or
 *         Renderer::ToggleEnabled, which write the `manualEnabled` atomic in the renderer's
 *         own state. They never touch `Settings` and never bump `Settings::Generation()`.
 *       - `debug` writes `Settings::Display().EnableDebugOverlay` under `Settings::Mutex()`
 *         and then bumps `Settings::Generation()` with release order, so the render thread
 *         re-captures its `RenderSettingsSnapshot` on the next frame.
 * @note Registration is a logged no-op in three cases, and never throws: the console command
 *       table is unavailable (logged as an error), a command named `glyph` is already
 *       registered (logged as info, which makes a second call harmless), or the
 *       `TestSeenData` slot is absent (logged as a warning).
 */
void Register();
}  // namespace ConsoleCommands
