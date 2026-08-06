#pragma once

#include <array>
#include <string>
#include <string_view>
#include <variant>

/**
 * @namespace Settings
 * @brief Declarative descriptor table for Settings scalars.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Core
 *
 * Each `SettingEntry` describes a single INI key: its canonical name, optional
 * alias, pointer to the backing variable, default value, and validation rule.
 * The table is `kSettings`, defined in Settings.cpp, and it is the only place a
 * scalar is declared.  Three consumers read it, in this order inside `Load()`:
 *
 * 1. `ResetToDefaults()` writes every `defaultValue` to its `target`.
 * 2. The parser resolves each INI key through a lazily built lookup map that
 *    holds the lowercased `key` and, when present, the lowercased `alias`, then
 *    overwrites the target.  Matching is case-insensitive and ignores which
 *    section the key appears in.
 * 3. `ClampAndValidate()` applies the `validation` rule of every row once,
 *    after the whole file is read.  It is not applied per key at parse time.
 *
 * Step 2 is the last dispatch step, not the first: a key is offered to the
 * active indexed-section parser (TierN, SpecialTitleN, HonorificN, RegisterN)
 * and then to `Format` / `InfoFormat` before the table is consulted, so a name
 * an indexed section owns never reaches a scalar row.  Row order carries no
 * meaning, but a key or alias declared twice keeps the last row that declares
 * it, because the map is built by overwriting.
 *
 * ```mermaid
 * flowchart TD
 *     K[one INI key = value line] --> A{indexed section active<br/>and field name known?}
 *     A -- yes --> S[section parser writes the indexed entry]
 *     A -- no --> F{key is Format or InfoFormat?}
 *     F -- yes --> P[quoted-segment parser]
 *     F -- no --> M{lowercased key in the kSettings map?}
 *     M -- yes --> T[ApplySettingValue writes the row target]
 *     M -- no --> W[counted as an unknown key and warned]
 * ```
 *
 * A numeric value that does not parse becomes 0, not the row's `defaultValue`,
 * and step 3 then clamps that zero into range.  A bool key is true only for
 * true, 1, yes, on or enabled, case-insensitive; every other text is false.  A
 * string key stores the trimmed, comment-stripped text with no validation;
 * surrounding quotes are kept.
 *
 * Every `target` points into a function-local static owned by an accessor that
 * Settings.hpp declares.  Writing through one is safe only under a unique lock on
 * `Settings::Mutex()`, which `Load()` holds for its whole body.
 *
 * ## :material-cog-outline: Validation Rules
 *
 * |       Rule | Effect                                          |
 * |------------|-------------------------------------------------|
 * | ClampFloat | Clamp to `[lo, hi]` after parse                 |
 * |   MinFloat | Clamp to `>= lo`, no upper bound                |
 * |   ClampInt | Clamp to `[lo, hi]` after parse                 |
 * |     MinInt | Clamp to `>= lo`, no upper bound                |
 * | NoClamping | Accept the raw parsed value as-is               |
 *
 * Validation runs on `float*` and `int*` targets only, and only when the rule
 * family matches the target type: ClampFloat / MinFloat for `float*`,
 * ClampInt / MinInt for `int*`.  A mismatched rule, and any rule on a `bool*`
 * or `std::string*` target, is ignored without a diagnostic.  Use NoClamping
 * on those rows so the table states the real behavior.
 *
 * ## :material-code-tags: Example Entry
 *
 * ```cpp
 * SettingEntry{
 *     "FadeStartDistance",            // canonical key
 *     "",                             // no alias
 *     &Distance().FadeStartDistance,  // backing variable
 *     200.0f,                         // default
 *     MinFloat{.0f},                  // validation
 * }
 * ```
 *
 * @see Settings::Load, Settings::ResetToDefaults
 */

namespace Settings
{

/**
 * @struct overloaded
 * @brief Lambda overload set for variant dispatch.
 *
 * The settings
 * loader uses this type to dispatch over the `target` and `validation` variants.
 *
 * @tparam Ts
 * Callable base types in the overload set.
 */
template <class... Ts>
struct overloaded : Ts...
{
    using Ts::operator()...;
};

// --- Validation rules -------------------------------------------------------

/**
 * @struct ClampFloat
 * @brief Clamp a parsed float to the closed interval from `lo` to `hi`.

 */
struct ClampFloat
{
    float lo;  ///< Inclusive lower bound.
    float hi;  ///< Inclusive upper bound.
};

/**
 * @struct MinFloat
 * @brief Raise a parsed float to `lo` when it is lower.
 *
 * The rule does
 * not apply an upper bound.
 */
struct MinFloat
{
    float lo;  ///< Inclusive lower bound.
};

/**
 * @struct ClampInt
 * @brief Clamp a parsed integer to the closed interval from `lo` to `hi`.

 */
struct ClampInt
{
    int lo;  ///< Inclusive lower bound.
    int hi;  ///< Inclusive upper bound.
};

/**
 * @struct MinInt
 * @brief Raise a parsed integer to `lo` when it is lower.
 *
 * The rule does
 * not apply an upper bound.
 */
struct MinInt
{
    int lo;  ///< Inclusive lower bound.
};

/**
 * @struct NoClamping
 * @brief Accept a parsed value without numeric clamping.
 */
struct NoClamping
{
};

/// @brief Variant of all supported validation rules.
using Validation = std::variant<ClampFloat, MinFloat, ClampInt, MinInt, NoClamping>;

// --- Setting entry ----------------------------------------------------------

/**
 * @struct SettingEntry
 * @brief One row of the `kSettings` descriptor table: a single INI scalar.
 * @ingroup Core
 *
 * `key` and `alias` are non-owning views, so store only string literals or
 * objects that outlive `kSettings`.
 *
 * @warning `defaultValue`'s active alternative must match `target`'s pointee
 *          type. Write `0.0f` for a `float*` target, not `0` (which selects
 *          the `int` alternative), and `std::string("x")` for a
 *          `std::string*` target, not a bare `"x"`. A mismatch makes the
 *          first `ResetToDefaults()` throw `std::bad_variant_access`, which
 *          nothing catches, so the plugin fails at load.
 */
struct SettingEntry
{
    std::string_view key;  ///< Canonical key name; INI lookup is case-insensitive
    /// Fully equivalent second key, not a deprecation marker (e.g.
    /// "EnableFlourishes" for "EnableOrnaments"). Empty = none.
    std::string_view alias;
    std::variant<float*, bool*, int*, std::string*> target;    ///< Backing variable
    std::variant<float, bool, int, std::string> defaultValue;  ///< Applied by ResetToDefaults()
    /// Applied once by ClampAndValidate() after the whole INI file is read, not
    /// per key at parse time.  Ignored on a bool or std::string target, and on
    /// a target whose type does not match the rule family.
    Validation validation;
};

}  // namespace Settings
