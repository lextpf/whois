#pragma once

/**
 * @namespace GameState
 * @brief Game state queries for overlay visibility.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Utilities
 *
 * Both queries share one gate, the file-local `IsWorldReady` in GameState.cpp: the game
 * must be active, no menu in the fixed suppressed-menu list may be open, and the player
 * must have an attached parent cell. CanDrawOverlay() adds a combat gate on top of it.
 *
 * Neither query is called from the render thread. UpdateSnapshot_GameThread calls
 * CanDrawOverlay() once per snapshot, and CanCaptureDeck() only when the Deck feature is
 * enabled. It publishes both results into the `allowOverlay` and `allowDeck` atomics in
 * RendererInternal.hpp, which the render thread reads instead of querying game state.
 */
namespace GameState
{
/**
 * @brief Check if the floating names overlay should be rendered.
 *
 * The overlay is hidden while the player is in combat, while the game is not
 * active (`RE::Main::gameActive`), while the player has no attached parent cell,
 * and while any menu in the fixed `SUPPRESSED_MENUS` list in GameState.cpp is
 * open. A menu outside that list, for example Dialogue Menu or BookMenu, does not
 * hide the overlay.
 *
 * @note Game-thread only. It dereferences RE::* game state such as the player's
 *       parent cell, so the render thread must not call it. The game thread
 *       publishes the result into the `allowOverlay` atomic, which render-thread
 *       code reads via Renderer::IsOverlayAllowedRT().
 *
 * @return `true` if overlay can be drawn, `false` if it should be hidden.
 *
 * @see Renderer::IsOverlayAllowedRT
 */
bool CanDrawOverlay();

/**
 * @brief Check whether the world is stable enough to capture a Deck portrait.
 *
 * Applies the same loading, menu and cell gates as CanDrawOverlay(), but permits
 * combat, because an actor in combat is a valid capture subject.
 *
 * @note Game-thread only. UpdateSnapshot_GameThread combines the result with the Deck
 *       enable flag and publishes it into the `allowDeck` atomic, which the render
 *       thread reads before it captures. A disabled Deck skips this call entirely.
 *
 * @return `true` when a Deck capture may run this frame.
 */
bool CanCaptureDeck();
}  // namespace GameState
