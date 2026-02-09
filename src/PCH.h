#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#define IMGUI_DEFINE_MATH_OPERATORS

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <RE/F/FightReactions.h>

#include <ranges>
#include <dxgi.h>
#include <shared_mutex>
#include <shlobj.h>

#include <boost/functional/hash.hpp>
#include <unordered_map>
#include <unordered_set>

#include <freetype/freetype.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <srell.hpp>
#include <xbyak/xbyak.h>

#include "imgui_impl_win32.h"
#include "imgui_internal.h"

#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_dx11.h>
#include <imgui_stdlib.h>

using namespace std::literals;

/// Export macro for DLL entry points
#define DLLEXPORT __declspec(dllexport)

/// Alias for SKSE's logging interface
namespace logger = SKSE::log;

/**
 * Hash map with Boost hash function support.
 *
 * Uses boost::hash by default which provides better hash distribution than
 * std::hash for many types, particularly for RE::FormID and handle types.
 *
 * @tparam K Key type.
 * @tparam D Data/value type.
 * @tparam H Hash function.
 * @tparam KEqual Key equality comparator.
 *
 * @ingroup Utilities
 *
 * @see FlatSet
 */
template <class K, class D, class H = boost::hash<K>, class KEqual = std::equal_to<K>>
using FlatMap = std::unordered_map<K, D, H, KEqual>;

/**
 * Hash set with Boost hash function support.
 *
 * @tparam K Key type.
 * @tparam H Hash function.
 * @tparam KEqual Key equality comparator.
 *
 * @ingroup Utilities
 *
 * @see FlatMap
 */
template <class K, class H = boost::hash<K>, class KEqual = std::equal_to<K>>
using FlatSet = std::unordered_set<K, H, KEqual>;

/**
 * Extensions to CommonLibSSE's reverse-engineered types.
 *
 * Adds comparison and hash support for `BSPointerHandle` types, enabling
 * their use as keys in ordered and unordered containers.
 *
 * @ingroup Utilities
 */
namespace RE
{
	/**
	 * Less-than comparison for BSPointerHandle.
	 *
	 * Enables use in `std::map`, `std::set`, and sorted algorithms.
	 * Compares the underlying native handle values.
	 *
	 * @tparam T Handle target type (e.g., Actor, TESObjectREFR).
	 * @param[in] a_lhs Left-hand operand.
	 * @param[in] a_rhs Right-hand operand.
	 * @return `true` if lhs handle value is less than rhs.
	 *
	 * @see hash_value
	 */
	template <class T>
	bool operator<(const RE::BSPointerHandle<T>& a_lhs, const RE::BSPointerHandle<T>& a_rhs)
	{
		return a_lhs.native_handle() < a_rhs.native_handle();
	}

	/**
	 * Boost-compatible hash function for BSPointerHandle.
	 *
	 * Enables use with `boost::hash` and `boost::unordered_map/set`.
	 * Used by FlatMap/FlatSet when keyed by actor handles.
	 *
	 * @tparam T Handle target type.
	 * @param[in] a_handle Handle to hash.
	 * @return Hash value derived from native handle.
	 *
	 * @see FlatMap, FlatSet
	 */
	template <class T>
	std::size_t hash_value(const BSPointerHandle<T>& a_handle)
	{
		boost::hash<uint32_t> hasher;
		return hasher(a_handle.native_handle());
	};
}

/**
 * Hook utilities and STL extensions for SKSE plugins.
 *
 * Provides template functions for common hooking patterns used in SKSE plugins.
 * All hook functions use SKSE's trampoline for safe code redirection.
 *
 * @see SKSE::GetTrampoline()
 */
namespace stl
{
	template <class T>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		T::func = trampoline.write_call<5>(a_src, T::thunk);
	}

	template <class F, class T>
	void write_vfunc()
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[0] };
		T::func = vtbl.write_vfunc(T::idx, T::thunk);
	}

	template <class T, std::size_t BYTES>
	void hook_function_prologue(std::uintptr_t a_src)
	{
		struct Patch : Xbyak::CodeGenerator
		{
			Patch(std::uintptr_t a_originalFuncAddr, std::size_t a_originalByteLength)
			{
				// Hook returns here. Execute the restored bytes and jump back to the original function.
				for (size_t i = 0; i < a_originalByteLength; ++i) {
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

	constexpr inline auto enum_range(auto first, auto last)
	{
		auto enum_range =
			std::views::iota(
				std::to_underlying(first),
				std::to_underlying(last)) |
			std::views::transform([](auto enum_val) {
				return (decltype(first))enum_val;
			});

		return enum_range;
	};
}

/**
 * Extended utilities for CommonLibSSE-based plugins.
 *
 * @ingroup Utilities
 */
namespace REX
{
	template <class T>
	class Singleton
	{
	public:
		/**
		 * Get the singleton instance.
		 *
		 * @return Pointer to the singleton instance (never null after first call).
		 */
		static T* GetSingleton()
		{
			static T singleton;
			return std::addressof(singleton);
		}

	protected:
		Singleton() = default;
		~Singleton() = default;

		Singleton(const Singleton&) = delete;
		Singleton(Singleton&&) = delete;

		Singleton& operator=(const Singleton&) = delete;
		Singleton& operator=(Singleton&&) = delete;
	};
}

/**
 * Check if the floating names overlay should be rendered.
 *
 * Determines whether game state allows overlay rendering. The overlay is hidden
 * during loading, menus, combat, and other states where it would be intrusive
 * or cause visual issues.
 *
 * @return `true` if overlay can be drawn, `false` if it should be hidden.
 *
 * @ingroup Utilities
 *
 * @see Renderer::IsOverlayAllowedRT
 */
static bool CanDrawOverlay()
{
    auto* main = RE::Main::GetSingleton();
    if (!main) {
        return false;
    }

	// "Loading is basically still happening" window
	if (!main->gameActive || main->freezeTime || main->freezeNextFrame || main->fullReset ||
		main->resetGame || main->reloadContent) {
		return false;
	}

	if (auto ui = RE::UI::GetSingleton()) {
		if (ui->IsMenuOpen("Loading Menu") ||
			ui->IsMenuOpen("Main Menu") ||
			ui->IsMenuOpen("MapMenu") ||
			ui->IsMenuOpen("Fader Menu") ||
			ui->IsMenuOpen("Menu") ||
			ui->IsMenuOpen("Console") ||
			ui->IsMenuOpen("TweenMenu") ||
            ui->IsMenuOpen("Journal Menu") ||
            ui->IsMenuOpen("InventoryMenu") ||
            ui->IsMenuOpen("MagicMenu") ||
            ui->IsMenuOpen("ContainerMenu") ||
            ui->IsMenuOpen("BarterMenu") ||
            ui->IsMenuOpen("GiftMenu") ||
            ui->IsMenuOpen("Crafting Menu") ||
            ui->IsMenuOpen("FavoritesMenu") ||
            ui->IsMenuOpen("Lockpicking Menu") ||
            ui->IsMenuOpen("Sleep/Wait Menu") ||
            ui->IsMenuOpen("StatsMenu")) {
			return false;
		}
    }

	auto player = RE::PlayerCharacter::GetSingleton();
	if (!player || !player->GetParentCell() || !player->GetParentCell()->IsAttached()) {
		return false;
	}

    if (player->IsInCombat()) {
        return false;
    }

    return true;
}

/**
 * Select address offset based on Skyrim edition.
 *
 * Used with REL::ID or direct addresses to support both Skyrim SE and AE builds.
 * The correct value is selected at compile time based on the SKYRIM_AE define.
 *
 * @param se Skyrim SE (1.5.97) offset.
 * @param ae Skyrim AE (1.6.x) offset.
 * @return The appropriate offset for the current build target.
 */
#ifdef SKYRIM_AE
#	define OFFSET(se, ae) ae
#else
#	define OFFSET(se, ae) se
#endif

#include "Version.h"