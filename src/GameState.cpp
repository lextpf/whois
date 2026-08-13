#include "GameState.hpp"

#include "PCH.hpp"

namespace GameState
{
namespace
{
bool IsWorldReady()
{
    auto* main = RE::Main::GetSingleton();
    if (!main)
    {
        return false;
    }

    if (!main->gameActive)
    {
        return false;
    }

    if (auto ui = RE::UI::GetSingleton())
    {
        static constexpr const char* SUPPRESSED_MENUS[] = {
            "Loading Menu",
            "Main Menu",
            "MapMenu",
            "Fader Menu",
            "RaceSex Menu",
            "MessageBoxMenu",
            "Menu",
            "Console",
            "TweenMenu",
            "Journal Menu",
            "InventoryMenu",
            "MagicMenu",
            "ContainerMenu",
            "BarterMenu",
            "GiftMenu",
            "Crafting Menu",
            "FavoritesMenu",
            "Lockpicking Menu",
            "Sleep/Wait Menu",
            "StatsMenu",
        };
        for (const auto* menu : SUPPRESSED_MENUS)
        {
            if (ui->IsMenuOpen(menu))
            {
                return false;
            }
        }
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->GetParentCell() || !player->GetParentCell()->IsAttached())
    {
        return false;
    }

    return true;
}
}  // namespace

bool CanCaptureDeck()
{
    return IsWorldReady();
}

bool CanDrawOverlay()
{
    if (!IsWorldReady())
    {
        return false;
    }

    // Labels hide during combat so they cannot reveal enemy positions. Deck is a
    // key-triggered capture, so it stops at the lighter IsWorldReady gate.
    const auto* player = RE::PlayerCharacter::GetSingleton();
    return player && !player->IsInCombat();
}
}  // namespace GameState
