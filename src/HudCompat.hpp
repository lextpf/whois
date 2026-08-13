#pragma once

#include <cstdint>

/**
 * @namespace HudCompat
 * @brief TrueHUD and moreHUD deconfliction, one label per actor.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Utilities
 *
 * Several HUD mods can label the same actor at once. HudCompat detects the two glyph most
 * often overlaps with and suppresses part of glyph's own output per actor:
 *
 * - **TrueHUD**: when `CompatYieldToTrueHUD` is on (default true) and TrueHUD floats an info
 *   bar over an actor - a bar anchored over the actor's head, not the docked boss bar -
 *   glyph fades that actor's plate to `CompatTrueHUDYieldAlpha` (default: fully out),
 *   crossfading over `CompatYieldSettleTime` seconds (default 0.3, clamped to [0.01, 2.0]),
 *   and returns it when the bar goes away.
 * - **moreHUD**: when `CompatYieldLevelToMoreHUD` is on and its crosshair readout already
 *   shows the target's level, glyph drops the level segment from that actor's main line. The
 *   code default is true, but the shipped glyph.ini sets it to false, so a default install
 *   does not drop the level.
 *
 * Detection needs no user patching: the TrueHUD API is requested at kPostPostLoad; moreHUD
 * presence is probed by DLL / plugin name. All queries run on the game thread and flow into
 * the actor snapshot.
 */
namespace HudCompat
{
/**
 * @brief Request the TrueHUD API and probe for moreHUD.
 *
 * Call once at SKSE kPostPostLoad, when all plugins are loaded. Not thread-safe and not
 * re-entrant: it writes two plain globals that the predicates below read without
 * synchronization, so HasTrueHUD() and HasMoreHUD() return false until it has run.
 *
 * moreHUD detection prefers the loaded module (AHZmoreHUDPlugin.dll) and falls back to an
 * AHZmoreHUD.esp / .esl lookup only when the data handler exists. Both outcomes are logged.
 *
 * The function never fails: an absent plugin is a normal result, not an error.
 */
void Initialize();

/// @brief True when the TrueHUD API interface is available.
bool HasTrueHUD();

/// @brief True when moreHUD is installed.
bool HasMoreHUD();

/**
 * @brief Check whether TrueHUD floats an info bar over this actor.
 *
 * Counts only floating bars, which are anchored over the actor's head where a nameplate
 * would stack against them. The docked boss bar does not count.
 *
 * @note Game-thread only. It dereferences the actor to take its handle.
 *
 * @param actor Actor to query.
 * @return `true` only when TrueHUD is present and floats a bar over `actor`. A null actor
 *         or an absent TrueHUD both return `false`.
 */
bool TrueHUDShowsBarFor(RE::Actor* actor);

/**
 * @brief Get the formID of the reference under the crosshair.
 *
 * Prefers the engine's actor-specific pick over the generic one, because the generic
 * target can be a weapon, an activator or a collision proxy standing in front of the
 * actor. Callers that compare this against an actor formID need that preference.
 *
 * @note Game-thread only. It reads `RE::CrosshairPickData`.
 *
 * @return The actor formID when the engine resolved one, otherwise the generic target's
 *         formID, otherwise 0. A return of 0 means nothing usable is under the crosshair.
 */
std::uint32_t CrosshairTargetFormID();
}  // namespace HudCompat
