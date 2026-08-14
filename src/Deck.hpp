#pragma once

#include "DeckUtils.hpp"
#include "Settings.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <string>
#include <vector>

/**
 * @namespace Deck
 * @brief One-key character-card capture and PNG export.
 * @author Fable 5 (https://github.com/claude)
 * @ingroup Renderer
 *
 * The render thread queues a plain-data CardRequest, copies the scene, and composes the card
 * into a dedicated D3D11 render target. GPU readback is deferred and WIC encoding runs on a
 * worker, so card generation does not block the frame that accepted the key press.
 *
 * The frame hooks call these functions on the render thread, and the shared state has no
 * internal lock. Shutdown() is the exception: the device-creation thunk calls it on the thread
 * that creates the renderer device. Nothing here dereferences a game object: the caller
 * resolves each actor fact into CardRequest first. Only the PNG encode runs elsewhere, on one
 * private worker thread.
 * At most one card composes at a time: a key edge taken before the previous card finished its
 * GPU readback is rejected. A card that only waits for its PNG encode does not block the next
 * one, so the encoder can hold more than one job.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef rt fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef gpu fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef worker fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *
 *     A[PollInput - key edge]:::rt --> B[ConsumeCaptureRequest]:::rt
 *     B --> C[Queue - CardRequest]:::rt
 *     C --> D[CaptureScene - scene copy]:::gpu
 *     D --> E[Process - draw card, copy to staging]:::gpu
 *     E --> F[PollReadback - later frame, event query, RGBA to BGRA]:::gpu
 *     F --> G[Encoder worker - writes the PNG]:::worker
 *     G --> H[DrawNotification - result toast]:::rt
 * ```
 *
 * When the scene copy is taken depends on which hook drives the frame:
 *
 * - The HUDMenu::PostDisplay path calls CaptureScene() before it calls the original
 *   PostDisplay, so the copy holds no crosshair and no meters.
 * - The IDXGISwapChain::Present fallback, used by upscaler stacks that skip PostDisplay,
 *   calls CaptureScene() at Present time. The HUD movie has already drawn by then.
 * - Process() captures late when no earlier copy succeeded, which is also after the HUD.
 *
 * On the last two paths the HUD can appear inside the portrait. That is the accepted
 * fallback, not a defect.
 */
namespace Deck
{
/**
 * @struct PortraitUV
 * @brief Normalized source rectangle for an actor portrait.
 *
 * The values are texture coordinates in the captured scene texture, so they do not depend on
 * resolution. The origin is the top-left corner. Positive V points down, which is the ImGui
 * image convention.
 */
struct PortraitUV
{
    float left = .0f;     ///< Left edge, in [0, 1] of scene width.
    float top = .0f;      ///< Top edge, in [0, 1] of scene height.
    float right = 1.0f;   ///< Right edge, in [0, 1] of scene width.
    float bottom = 1.0f;  ///< Bottom edge, in [0, 1] of scene height.
};

/**
 * @struct CardBadge
 * @brief One status or statistic icon in the card badge strip.
 *
 * The renderer drops a badge when its texture is not loaded. The remaining badges stay
 * centered below the card divider.
 */
struct CardBadge
{
    std::string icon;          ///< Duotone icon name; ignored when tierImage >= 0
    Settings::Color3 color{};  ///< Tint for duotone icons; ignored for a tier emblem
    bool muted = false;        ///< Resting state: desaturated tint and lower opacity
    int tierImage = -1;        ///< Tier emblem index, or -1 to use icon; emblems draw untinted
};

/**
 * @struct CardRequest
 * @brief Complete render-thread-safe description of one card.
 *
 * The renderer resolves each game fact before `Queue` receives the request. The card can then
 * be composed in a later frame without access to game objects.
 */
struct CardRequest
{
    /// Actor FormID. Seeds the foil pattern and its glints, the text-effect phase, the
    /// particle phase, the printed card number, and the output filename. Two captures of one
    /// actor therefore repeat the same foil and glint placement.
    std::uint32_t formID = 0;
    std::string name;      ///< Actor display name, UTF-8.
    std::string title;     ///< Special title, honorific, or tier title, in that order.
    std::string tierName;  ///< Treatment tier title. Printed in the card footer.
    /// Card output folder. A relative path resolves against the Skyrim directory, and an
    /// absolute path is used unchanged. An empty string is not an error: it selects
    /// `Data/SKSE/Plugins/glyph/cards`. The encoder worker creates the folder on demand.
    std::string outputFolder;
    /// Comma-separated `[TierN] ParticleTypes` tokens. All whitespace in a token is removed,
    /// the token is lowercased, and an optional `:weight` suffix is stripped and ignored.
    /// Only the first two recognized tokens are drawn. The whole particle block is skipped
    /// unless rarity is Rarity::Legendary.
    std::string particleTypes;

    /// Actor level. Printed in the level medallion.
    std::uint16_t level = 0;
    int tierIndex = 0;  ///< Treatment tier from TreatmentTierForRarity, not the actor's own tier.
    int tierCount = 1;  ///< Tier-ladder length; with tierIndex it selects the tier emblem.
    int width = 750;    ///< Card width, in pixels. From DeckCardWidth.
    int height = 1050;  ///< Card height, in pixels. From DeckCardHeight.
    /// Resolved card rarity. A higher rarity adds frame weight and effect strength, and
    /// Legendary adds the spectral border, the holographic foil, and the particle block.
    Rarity rarity = Rarity::Common;
    PortraitUV portrait{};  ///< Portrait crop, normalized against the captured scene.

    // Resolved styling. The treatment tier supplies these, and a matching special title
    // overrides the name and title colors. Common cards drop to plain ink with no effect.
    Settings::Color3 nameLeft{};
    Settings::Color3 nameRight{};
    Settings::Color3 titleLeft{};
    Settings::Color3 titleRight{};
    Settings::Color3 frameLeft{};
    Settings::Color3 frameRight{};
    Settings::Color3 highlight{};
    Settings::Color3 particleColor{};
    Settings::Color3 ornamentLeft{};
    Settings::Color3 ornamentRight{};
    Settings::EffectParams nameEffect{};
    Settings::EffectParams titleEffect{};
    std::string leftOrnaments;
    std::string rightOrnaments;
    int particleCount = 0;  ///< Nameplate-scale hint; halved, then clamped to [4, 10] per style.

    /// Badge strip contents, in draw order. An empty vector leaves the strip out.
    std::vector<CardBadge> badges;
};

/**
 * @brief Edge-poll the configured Win32 virtual key on the render thread.
 *
 * A rejected key edge is not silent: it shows an error toast. An edge is rejected when
 * @p worldReady is false, or when a capture request or a card is already in flight.
 * An accepted edge arms ConsumeCaptureRequest() and shows a status toast.
 *
 * @param enabled     False clears the key-edge state and a capture request that was not
 *                    consumed yet. A card that already reached Queue keeps developing.
 * @param virtualKey  Win32 virtual-key code. A value <= 0 disables polling and clears the
 *                    same state as a false `enabled`.
 * @param worldReady  False makes a key edge raise the "world is loading or a menu is open"
 *                    toast instead of starting a capture.
 *
 * @note The poll uses GetAsyncKeyState, which is process-global, so the key edge is
 *       detected even when the game window has no focus.
 */
void PollInput(bool enabled, int virtualKey, bool worldReady);

/**
 * @brief Consume one capture request from an accepted key edge.
 *
 * A true return clears the request. The next call returns false until another key edge is
 * accepted.
 *
 * @return True when a capture request was pending.
 */
bool ConsumeCaptureRequest();

/**
 * @brief Publish the fully resolved card metadata for the current capture.
 *
 * On acceptance the scene-copy flag is cleared, so the next hook that runs takes a fresh
 * copy, and a status toast names the rarity and the actor.
 *
 * @param request  Plain-data card description. Moved into the pending slot.
 * @return         True when the request was accepted. False when a card is already queued or
 *                 waiting for readback; Queue has then already shown the "still developing
 *                 the previous card" toast, so the caller must not show a second one. A card
 *                 that only waits for its PNG encode does not block a new request.
 */
bool Queue(CardRequest request);

/**
 * @brief Show an in-game error toast and log its reason.
 *
 * The toast uses the error color and remains visible for five seconds. The function logs the
 * reason at warning level.
 *
 * @param message  Error reason to display and log.
 */
void NotifyError(std::string message);

/**
 * @brief Report whether the queued card needs a scene copy.
 *
 * @return True before the scene copy is taken. False when no card is queued or the queued
 *         card already has a scene copy.
 */
bool NeedsSceneCapture();

/**
 * @brief Copy the backbuffer or the bound render target into a readable texture.
 *
 * The copy is the portrait source: it is created with a shader-resource binding and sampled
 * by the card's portrait pixel shader. A multisampled source is resolved, not copied. An
 * sRGB source is copied through the matching typeless format and viewed as UNORM, so the
 * presented bytes reach the PNG without a second gamma conversion. A successful call
 * replaces any earlier copy and satisfies NeedsSceneCapture().
 *
 * @param device     D3D11 device. A null device makes the call return false.
 * @param context    Immediate context. A null context makes the call return false.
 * @param swapChain  Swap chain to read the backbuffer from. When null, or when the
 *                   backbuffer cannot be acquired, the currently bound render target is used
 *                   instead.
 * @return           True only when this call produced a scene copy. False on a D3D failure,
 *                   which includes no bound render target on the fallback path, and a render
 *                   target view that selects several array slices or an unsupported
 *                   dimension. False also when there is nothing to capture: no queued card, a
 *                   copy already taken for it, or a null device or context. The caller must
 *                   not treat false as an error on its own. Call NeedsSceneCapture() first to
 *                   separate the two cases.
 */
bool CaptureScene(ID3D11Device* device,
                  ID3D11DeviceContext* context,
                  IDXGISwapChain* swapChain = nullptr);

/**
 * @brief Report whether the render hook must service the Deck subsystem.
 *
 * Service remains necessary while a key edge waits, a card is queued, a readback or PNG
 * encode is active, or a status toast is visible. This condition is independent of the
 * nameplate enable state.
 *
 * @return True when the Deck subsystem needs a render frame.
 */
bool NeedsFrame();

/**
 * @brief Render a queued card, poll readback, and send finished pixels to the encoder.
 *
 * Readback is deferred, and completed pixels go to the WIC encoder worker. Call after
 * ImGui::Render() and before rendering the normal ImGui draw data to the game backbuffer.
 * When no earlier hook captured the scene, Process() captures it here, which is after the
 * HUD has drawn.
 *
 * One call drains finished encoder results, polls the outstanding readback, then composes
 * the queued card. Encoder results are drained even when the device or the context is null.
 * Any failure drops the queued card and raises an error toast; there is no retry.
 *
 * The pass binds its own render target and viewport and restores the previous ones. The ImGui
 * DX11 backend saves and restores every pipeline state it changes, so the pass leaves no
 * shader, blend or input-assembler state behind either.
 *
 * @param device     D3D11 device used to create the card target and the staging texture.
 * @param context    Immediate context used for the draw and the readback.
 * @param swapChain  Swap chain used by the late capture. When null, the bound render target
 *                   is used instead.
 */
void Process(ID3D11Device* device,
             ID3D11DeviceContext* context,
             IDXGISwapChain* swapChain = nullptr);

/**
 * @brief Draw the transient Deck status toast.
 *
 * The toast is anchored to the top-right corner of the display. The function draws nothing
 * after the toast deadline.
 *
 * @pre Call between `ImGui::NewFrame` and `ImGui::Render`.
 */
void DrawNotification();

/**
 * @brief Release device-owned textures and abandon incomplete GPU work.
 *
 * The D3D11 device-creation hook calls this when it detects a device or swap-chain change.
 * Nothing calls it at process exit. An abandoned request raises the "capture was interrupted"
 * toast. The key-edge state survives, so an edge that was accepted but not consumed still
 * produces a card once the device is back.
 *
 * @warning Shutdown does not touch the WIC encoder worker. PNG encodes that are already
 *          queued keep running: they still create the output folder and write their files
 *          after this call returns. The worker thread ends only when the process destroys
 *          its statics, and a job still waiting in the queue at that point is dropped.
 */
void Shutdown();
}  // namespace Deck
