#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/**
 * @namespace Deck
 * @brief Runtime-independent helpers for character-card capture.
 * @author Fable 5 (https://github.com/claude)
 * @ingroup Renderer
 *
 * Every entry point here is a stateless free function over plain values. The header pulls in
 * no CommonLibSSE, D3D11 or ImGui dependency, so DeckUtils.cpp compiles straight into the
 * glyph_test_deck target and each function is tested directly.
 *
 * ## :material-cards-playing-outline: Rarity Pipeline
 *
 * The render thread chains four of these helpers when it prepares a capture. The roll step
 * runs only when the Deck RarityRolls setting is on and the actor is unique; without it the
 * chain is deterministic for a given actor and tier ladder.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef input fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef step fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef out fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *     A[ActorRarityProfile]:::input --> B[RarityFromActor]:::step
 *     C[actor tier index]:::input --> D[RarityFromTier]:::step
 *     B --> E[higher rank wins]:::step
 *     D --> E
 *     F[NextDeckRarityRoll - DeterministicPhase sample]:::input --> G[ApplyRarityRoll]:::step
 *     E --> G
 *     G --> H[final Rarity]:::out
 *     H --> I[TreatmentTierForRarity]:::step
 *     I --> J[tier that paints the card]:::out
 * ```
 */
namespace Deck
{
/**
 * @enum Rarity
 * @brief Rarity bands for collectible cards.
 *
 * The underlying values are
 * ranks 0 through 4. The helpers compare, clamp, and cast these
 * values as plain integers. The
 * order and the number of bands are part of the contract.
 */
enum class Rarity : std::uint8_t
{
    Common,
    Uncommon,
    Rare,
    Epic,
    Legendary
};

/**
 * @enum RarityArchetype
 * @brief Actor family used by the rarity score.
 *
 * The enum mirrors
 * the game creature kinds as plain data, so the helpers do not need game
 * types. Humanoid adds no
 * bonus. Dragon overrides the level score. `RarityFromActor` defines
 * the level thresholds for
 * the other families.
 */
enum class RarityArchetype : std::uint8_t
{
    Humanoid,
    Beast,
    Undead,
    Daedra,
    Dragon
};

/**
 * @struct ActorRarityProfile
 * @brief Plain actor facts that determine the intrinsic card
 * rarity.
 */
struct ActorRarityProfile
{
    /// Actor level. A value below 1 is treated as level 1.
    int level = 1;
    RarityArchetype archetype = RarityArchetype::Humanoid;  ///< Selects the archetype bonus.
    bool essential = false;                                 ///< Adds one rank when true.
};

/**
 * @struct PortraitCrop
 * @brief Pixel-space source rectangle for an aspect-preserving portrait
 * crop.
 *
 * The origin is the top-left corner of the source image. Positive Y points down, which
 * is the
 * screen-space convention that the projection uses.
 */
struct PortraitCrop
{
    float x = .0f;       ///< Left edge, in source pixels.
    float y = .0f;       ///< Top edge, in source pixels.
    float width = .0f;   ///< Crop width, in source pixels.
    float height = .0f;  ///< Crop height, in source pixels.
};

/**
 * @brief Uniform layout scale that fits the 750x1050 card design into the target.
 *
 * The card layout is authored against a 750x1050 pixel reference and multiplies its
 * design-space lengths by this scale before drawing. The value is not clamped, so a target
 * larger than the reference scales the design up.
 *
 * @param width   Target card width, in pixels.
 * @param height  Target card height, in pixels.
 * @return        min(width / 750, height / 1050). Returns 1.0f when either dimension is
 *                non-finite or not positive. That value is the unscaled reference scale, not
 *                a fitted one, so the design is not kept inside the target in that case.
 */
[[nodiscard]] float CardLayoutScale(float width, float height) noexcept;

/**
 * @brief Copy a mapped RGBA8 texture into tightly packed BGRA8 pixels for WIC.
 *
 * Source rows may contain driver padding, so the copy walks rows by @p sourceRowPitch and
 * writes them back at a width * 4 stride. Each pixel swaps its red and blue bytes. Alpha is
 * copied unchanged and stays straight (non-premultiplied), which is what EncodeBgraPng
 * expects.
 *
 * The copy writes exactly width * height * 4 bytes and leaves any extra destination capacity
 * untouched. The two buffers must not overlap.
 *
 * @param source           First byte of the mapped RGBA8 image.
 * @param sourceRowPitch   Mapped row pitch, in bytes. Must be >= width * 4.
 * @param width            Image width, in pixels. Must be > 0.
 * @param height           Image height, in pixels. Must be > 0.
 * @param destination      Target buffer for tightly packed BGRA8 pixels.
 * @param destinationSize  Capacity of `destination`, in bytes. Must be at least
 *                         width * height * 4; a larger buffer is allowed.
 * @return                 True when the copy was done. False on a null pointer, a width or
 *                         height that is not positive, a too-small row pitch, a too-small
 *                         destination, or a byte count that overflows std::size_t. Nothing is
 *                         written on a false return.
 */
[[nodiscard]] bool CopyRgbaToBgra(const std::uint8_t* source,
                                  std::size_t sourceRowPitch,
                                  int width,
                                  int height,
                                  std::uint8_t* destination,
                                  std::size_t destinationSize) noexcept;

/**
 * @brief Map a zero-based tier to one of five evenly spaced rarity bands.
 *
 * With the clamped index $i$ and the ladder length $n$, the band rank is
 *
 * $$b = \min\left(\left\lfloor \frac{5\,i}{n - 1} \right\rfloor,\; 4\right)$$
 *
 * The first configured tier is always Common and, when more than one tier is configured, the
 * last is always Legendary: the floor term reaches 5 only at the last index, where the
 * minimum pulls it back to Legendary. Out-of-range indices clamp to those endpoints. A
 * missing or single-tier ladder resolves to Common.
 *
 * @param tierIndex  Zero-based tier index. Clamped to [0, tierCount - 1].
 * @param tierCount  Configured tier-ladder length.
 * @return           The rarity band for the tier.
 */
[[nodiscard]] Rarity RarityFromTier(int tierIndex, int tierCount) noexcept;

/**
 * @brief Score intrinsic rarity from stable actor facts.
 *
 * The rank starts at Common. Each level milestone reached (20, 35, 50) adds one rank. The
 * archetype adds at most one further rank, and only at or above its own level: Daedra 25,
 * Undead 30, Beast 40. Essential status adds one more, after the archetype step. A level-50
 * Undead actor is therefore Legendary.
 *
 * RarityArchetype::Dragon is an override, not a bonus: it replaces the level score, so any
 * dragon is Legendary at any level. The final rank is clamped to Legendary.
 *
 * @param profile  Stable actor facts. A level below 1 is treated as level 1.
 * @return         The intrinsic rarity, before the rarity roll.
 */
[[nodiscard]] Rarity RarityFromActor(const ActorRarityProfile& profile) noexcept;

/**
 * @brief Raise a card's rarity by a random roll, never lowering the intrinsic rarity.
 *
 * The roll cuts at 0.45, 0.70, 0.85 and 0.95, which gives 45/25/15/10/5 percent
 * Common-through-Legendary odds. Those odds hold only for a uniformly distributed `roll`,
 * and they describe the rolled rank, not the returned one: the return is the higher of the
 * rolled and the intrinsic rank.
 *
 * @param intrinsicRarity  Rarity from RarityFromActor or RarityFromTier. A value outside the
 *                         enum is clamped into the range Common through Legendary.
 * @param roll             Uniform sample in [0, 1). A value outside that range is clamped. A
 *                         non-finite value returns the clamped intrinsic rarity.
 * @return                 The higher of the intrinsic and the rolled rarity.
 */
[[nodiscard]] Rarity ApplyRarityRoll(Rarity intrinsicRarity, float roll) noexcept;

/**
 * @brief Select the authored tier that supplies a rarity's palette and effects.
 *
 * The function scans the ladder for the tiers whose RarityFromTier band equals `rarity`,
 * then clamps the actor's own tier into that contiguous run. A short ladder leaves bands
 * empty; when no tier maps to the band, the result is the proportionally nearest tier
 * instead,
 *
 * $$t = \text{round}\!\left(\frac{r}{4}\,(n - 1)\right)$$
 *
 * with the rarity rank $r$ from 0 for Common to 4 for Legendary, and the ladder length $n$.
 *
 * @param rarity          Resolved card rarity.
 * @param actorTierIndex  The actor's own zero-based tier index. Clamped into the band.
 * @param tierCount       Configured tier-ladder length. A count <= 1 returns 0.
 * @return                Zero-based index of the tier to draw the card with, always in
 *                        [0, tierCount - 1].
 */
[[nodiscard]] int TreatmentTierForRarity(Rarity rarity, int actorTierIndex, int tierCount) noexcept;

/**
 * @brief Get the capitalized English display name for a rarity.
 *
 * The returned view points
 * to a static string literal and remains valid after the call.
 *
 * @param rarity  Rarity value to
 * name.
 * @return        The display name, or "Common" when `rarity` is invalid.
 */
[[nodiscard]] std::string_view RarityName(Rarity rarity) noexcept;

/**
 * @brief Build a portrait crop around projected head and feet positions.
 *
 * The crop starts as the axis-aligned box around the projected head-to-feet segment, padded
 * on all four sides by 12.5 percent of the longer of the two projected spans. With the head
 * at $(x_h, y_h)$ and the feet at $(x_f, y_f)$:
 *
 * $$s = \max\bigl(|x_f - x_h|,\; |y_f - y_h|\bigr), \qquad p = 0.125\,s$$
 *
 * $$w_0 = |x_f - x_h|
 * + 2p, \qquad h_0 = |y_f - y_h| + 2p$$
 *
 * @verbatim
 * Source image: origin at the top-left; +x
 * points right; +y points down.
 *
 * +--------------------------------------------------+
 * |
 * +----------------------------+            |  final aspect crop
 * |        | +--------------+ |
 * |
 * |        |      |      H       |      |            |  padded head-feet box
 * |        | |
 * |       |      |            |
 * |        |      |      F       |      |            |
 * | |
 * +--------------+      |            |
 * |        +----------------------------+            |
 *
 * +--------------------------------------------------+
 *
 * H is the head point. F is the feet
 * point. Near an image edge, move the final
 * crop inside the source. Keep its width-to-height
 * ratio unchanged.
 * @endverbatim
 *
 * That box then grows on one axis until it matches @p
 * desiredAspect (width / height), scales
 * down uniformly until it fits inside the source, and is
 * finally centered on the head-feet midpoint and clamped into the source rectangle. The aspect
 * survives every step, and the returned crop is always fully inside the source, so a subject near
 * an edge ends up off-center rather than cropped to a different shape.
 *
 * A non-finite projection, or a span $s$ of 0.001 pixels or less, uses the largest centered
 * crop with the requested aspect instead.
 *
 * @param sourceWidth   Source image width, in pixels. Must be finite and > 0.
 * @param sourceHeight  Source image height, in pixels. Must be finite and > 0.
 * @param headX         Projected head x, in source pixels.
 * @param headY         Projected head y, in source pixels; +y is downward.
 * @param feetX         Projected feet x, in source pixels.
 * @param feetY         Projected feet y, in source pixels; +y is downward.
 * @param desiredAspect Target width / height. A non-finite or non-positive value falls back
 *                      to the source aspect.
 * @return              The crop rectangle, in source pixels. An invalid source dimension
 *                      returns an empty rectangle.
 */
[[nodiscard]] PortraitCrop ComputePortraitCrop(float sourceWidth,
                                               float sourceHeight,
                                               float headX,
                                               float headY,
                                               float feetX,
                                               float feetY,
                                               float desiredAspect) noexcept;

/**
 * @brief Deterministic, well-distributed phase that is at least 0 and less than 1.
 *
 * The result serves two roles: animation phase, and the uniform sample that reaches
 * ApplyRarityRoll through Renderer's NextDeckRarityRoll. Uniformity is therefore a
 * correctness requirement, not only a visual one: changing the hash changes the rarity odds.
 *
 * The value is the top 24 bits of an integer finalizer divided by 2^24, so it is always a
 * multiple of 2^-24. NextDeckRarityRoll mixes a clock reading and a call serial into the
 * FormID before it calls this function, so the rarity roll is not stable per actor even
 * though this function is.
 *
 * @param formID  Seed value. An input of 0 returns exactly 0.
 * @return        A value in [0, 1).
 */
[[nodiscard]] float DeterministicPhase(std::uint32_t formID) noexcept;

/**
 * @brief Convert an actor name to a readable, Windows-safe ASCII filename stem.
 *
 * A run of unsafe or non-ASCII bytes becomes one underscore. Unsafe means a control byte, a
 * byte of 127 or more, or one of the characters Windows forbids in a name (angle brackets,
 * colon, quote, forward slash, backslash, pipe, question mark, asterisk), so a non-ASCII name
 * collapses to underscores instead of transliterating. Trailing spaces and dots are then
 * removed.
 *
 * A reserved Windows device name is prefixed with an underscore. That check is
 * case-insensitive and ignores everything from the first dot on, so `con.txt` is protected
 * too. It covers CON, PRN, AUX, NUL, CLOCK$, CONIN$, CONOUT$, COM1 to COM9, and LPT1 to
 * LPT9; it does not cover COM0 or COM10.
 *
 * The stem is capped at 220 output bytes. That leaves room inside the 255-character path
 * component limit for the FormID, timestamp, collision suffix, and `.png` extension the
 * caller appends.
 *
 * @param value  Actor name, in UTF-8.
 * @return       The filename stem. Empty input, and input that reduces to an empty stem,
 *               both resolve to "Actor".
 */
[[nodiscard]] std::string SanitizeFilenameStem(std::string_view value);
}  // namespace Deck
