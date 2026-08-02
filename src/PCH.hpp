#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#define IMGUI_DEFINE_MATH_OPERATORS

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <RE/F/FightReactions.h>

#include <dxgi.h>
#include <shlobj.h>
#include <ranges>
#include <shared_mutex>

#include <boost/functional/hash.hpp>
#include <unordered_map>
#include <unordered_set>

#include <freetype/freetype.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <xbyak/xbyak.h>
#include <srell.hpp>

#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

/// @brief Export marker for DLL entry points.
#define DLLEXPORT __declspec(dllexport)

/// @brief Short name for the SKSE logging interface.
namespace logger = SKSE::log;

/**
 * @namespace RE
 * @brief Extensions to CommonLibSSE's reverse-engineered types.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Utilities
 *
 * Adds comparison and hash support to `BSPointerHandle`, so a handle can be a key in ordered
 * and unordered containers.
 */
namespace RE
{
/**
 * @brief Order two handles by native handle value.
 *
 * Makes `BSPointerHandle` usable in `std::map`, `std::set`, and sorted algorithms.
 *
 * @tparam T Handle target type, for example Actor or TESObjectREFR.
 * @param[in] a_lhs Left-hand operand.
 * @param[in] a_rhs Right-hand operand.
 * @return `true` when the left native handle value is smaller than the right one.
 *
 * @see hash_value
 */
template <class T>
bool operator<(const RE::BSPointerHandle<T>& a_lhs, const RE::BSPointerHandle<T>& a_rhs)
{
    return a_lhs.native_handle() < a_rhs.native_handle();
}

/**
 * @brief Boost-compatible hash for a handle.
 *
 * Makes `BSPointerHandle` usable with `boost::hash` and the Boost unordered containers.
 *
 * @tparam T Handle target type.
 * @param[in] a_handle Handle to hash.
 * @return Hash of the native handle value.
 */
template <class T>
std::size_t hash_value(const BSPointerHandle<T>& a_handle)
{
    boost::hash<uint32_t> hasher;
    return hasher(a_handle.native_handle());
}
}  // namespace RE

/**
 * @namespace Stl
 * @brief Hook utilities and STL extensions for SKSE plugins.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Utilities
 *
 * `WriteThunkCall` and `HookFunctionPrologue` redirect code through SKSE's trampoline.
 * `WriteVfunc` patches a vtable slot in place through `REL::Relocation::write_vfunc` and uses
 * no trampoline memory.
 *
 * @see SKSE::GetTrampoline()
 */
namespace Stl
{
/**
 * @brief Redirect a five-byte call through an SKSE trampoline.
 *
 * The function redirects the
 * call to `T::thunk` and stores the original callee address in
 * `T::func`.
 *
 * @tparam T Hook
 * descriptor that provides `static decltype(func) func` and
 *              `static thunk(...)`.
 *
 * @param a_src Address of the call instruction to patch.
 * @pre `a_src` points to a five-byte
 * relative call instruction.
 * @warning A CommonLibSSE failure, such as exhausted trampoline space
 * or a missing Address
 *          Library ID, calls `stl::report_and_fail`. That function shows a
 * message box and
 *          terminates the process. A try/catch block around this helper does not
 * intercept
 *          the failure.
 */
template <class T>
void WriteThunkCall(std::uintptr_t a_src)
{
    auto& trampoline = SKSE::GetTrampoline();
    T::func = trampoline.write_call<5>(a_src, T::thunk);
}

/**
 * @brief Replace one vtable entry with a hook thunk.
 *
 * The function stores the original
 * function pointer in `T::func` and writes `T::thunk` into
 * the selected slot.
 *
 * @tparam F
 * Class whose vtable is patched. The class must provide `VTABLE`.
 * @tparam T Hook descriptor that
 * provides `idx`, `func`, and `thunk`.
 * @pre `T::idx` is a valid slot in `F::VTABLE[0]`. An
 * invalid index writes past the vtable and
 *      corrupts memory.
 * @warning A missing Address
 * Library ID for `F::VTABLE[0]` calls
 *          `stl::report_and_fail`. That function shows a
 * message box and terminates the
 *          process. A try/catch block does not intercept the
 * failure. In Release, a patch
 *          site that cannot be made writable fails silently because
 * `REL::safe_write` only
 *          asserts.
 */
template <class F, class T>
void WriteVfunc()
{
    REL::Relocation<std::uintptr_t> vtbl{F::VTABLE[0]};
    T::func = vtbl.write_vfunc(T::idx, T::thunk);
}

/**
 * @brief Install a hook at a function prologue.
 *
 * Xbyak generates a trampoline that copies
 * the first `BYTES` bytes of the original function
 * and then jumps to `a_src + BYTES`. A
 * five-byte branch at `a_src` redirects to `T::thunk`.
 * The function stores the trampoline
 * address in `T::func`, so the thunk can run the original
 * prologue.
 *
 * @verbatim
 * Before
 * patching:
 *
 * a_src                                                a_src + BYTES
 *   |
 * [complete original instructions, BYTES bytes]          | [function body ...]
 *
 * After
 * patching:
 *
 * a_src -> [five-byte branch to T::thunk]
 *                              |
 * +--
 * optional original call --> T::func
 * |
 * +--> [copy of BYTES]
 * |
 * +--> a_src + BYTES
 *
 * @endverbatim
 *
 * Only five bytes are overwritten at `a_src`. The relocated block contains all
 * `BYTES`
 * original bytes, including any bytes beyond the branch.
 *
 * @tparam T     Hook
 * descriptor that provides `func` and `thunk`.
 * @tparam BYTES Number of prologue bytes to
 * relocate. The value must be at least five.
 * @param a_src  Address of the function prologue to
 * patch.
 * @pre `BYTES` covers complete instructions at `a_src`. It does not split an
 * instruction.
 */
template <class T, std::size_t BYTES>
void HookFunctionPrologue(std::uintptr_t a_src)
{
    struct Patch : Xbyak::CodeGenerator
    {
        Patch(std::uintptr_t a_originalFuncAddr, std::size_t a_originalByteLength)
        {
            for (size_t i = 0; i < a_originalByteLength; ++i)
            {
                db(*reinterpret_cast<std::uint8_t*>(a_originalFuncAddr + i));
            }

            jmp(ptr[rip]);
            dq(a_originalFuncAddr + a_originalByteLength);
        }
    };

    Patch p(a_src, BYTES);
    p.ready();

    auto& trampoline = SKSE::GetTrampoline();
    trampoline.write_branch<5>(a_src, T::thunk);

    auto alloc = trampoline.allocate(p.getSize());
    std::memcpy(alloc, p.getCode(), p.getSize());

    T::func = reinterpret_cast<std::uintptr_t>(alloc);
}

/**
 * @brief Create a view over a half-open range of enum values.
 *
 * The range includes `first`
 * and excludes `last`. The function maps each underlying integer
 * back to the enum type with
 * `static_cast`. The parameters are deduced independently, but the
 * element type comes from
 * `decltype(first)`. Both parameters must have the same enum type. A
 * mismatched `last` can
 * compile and reinterpret the values.
 *
 * @param first  First enumerator in the range.
 * @param
 * last   Past-the-end enumerator.
 * @return       A `views::iota | views::transform` range of enum
 * values.
 */
constexpr inline auto EnumRange(auto first, auto last)
{
    auto result =
        std::views::iota(std::to_underlying(first), std::to_underlying(last)) |
        std::views::transform([](auto enum_val) { return static_cast<decltype(first)>(enum_val); });

    return result;
}

/**
 * @struct EnumStringMap
 * @brief Compile-time bidirectional enum-string mapping.
 *
 * @tparam E Enum type.
 * @tparam N Number of entries.
 */
template <typename E, std::size_t N>
struct EnumStringMap
{
    /**
     * @struct Entry
     * @brief One enum-value and string-name pair.
     */
    struct Entry
    {
        std::string_view name;  ///< Display name for the enumerator.
        E value;                ///< Corresponding enum value.
    };

    std::array<Entry, N> entries;  ///< Name/value pairs, scanned linearly; first match wins.

    /**
     * @brief Find an enum value by its string name.
     *
     * @param s         String
     * to match exactly against an entry name.
     * @param fallback  Value to return when no entry
     * matches.
     * @return          The matching enum value, or `fallback` when no entry
     * matches.
     */
    constexpr E fromString(std::string_view s, E fallback) const
    {
        for (const auto& e : entries)
        {
            if (e.name == s)
            {
                return e.value;
            }
        }
        return fallback;
    }

    /**
     * @brief Convert an enum value to its string name.
     *
     * @param v  Enum value
     * to find.
     * @return   The matching name, or `"unknown"` when no entry matches.
     */
    constexpr std::string_view toString(E v) const
    {
        for (const auto& e : entries)
        {
            if (e.value == v)
            {
                return e.name;
            }
        }
        return "unknown";
    }
};
}  // namespace Stl

/**
 * @brief Select an address offset for the detected Skyrim edition.
 *
 * Used with `REL::ID` or a direct address so one DLL serves Skyrim SE and AE/GOG.
 * CommonLibSSE-NG picks the value that matches the detected runtime.
 *
 * @param se Skyrim SE (1.5.97) offset.
 * @param ae Skyrim AE/GOG (1.6.x) offset.
 * @return The appropriate offset for the current runtime.
 */
#define GLYPH_OFFSET(se, ae) REL::Relocate((se), (ae))

#include "Version.hpp"
