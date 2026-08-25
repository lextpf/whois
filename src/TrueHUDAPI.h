#pragma once

/**
 * @namespace TRUEHUD_API
 * @brief Vendored TrueHUD API interface used for HUD compatibility.
 * @author Ershin (https://github.com/ersh1)
 * @ingroup Utilities
 *
 * This interface is a condensed copy of `src/TrueHUDAPI.h` from TrueHUD. TrueHUD uses the MIT
 * license. glyph calls only `RequestPluginAPI` and `IVTrueHUD3::HasInfoBar`. The other virtual
 * functions preserve the original vtable layout. Do not reorder or remove them.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * classDiagram
 *     direction LR
 *     class InterfaceVersion {
 *         <<enumeration>>
 *         V1
 *         V2
 *         V3
 *         V4
 *     }
 *     class IVTrueHUD1 {
 *         <<interface>>
 *         +RequestTargetControl()
 *         +AddActorInfoBar()
 *     }
 *     class IVTrueHUD2 {
 *         <<interface>>
 *         +OverrideBarColor()
 *     }
 *     class IVTrueHUD3 {
 *         <<interface>>
 *         +DrawLine()
 *         +HasInfoBar()
 *     }
 *     IVTrueHUD1 <|-- IVTrueHUD2
 *     IVTrueHUD2 <|-- IVTrueHUD3
 *     InterfaceVersion ..> IVTrueHUD3 : RequestPluginAPI with V3
 *     note for IVTrueHUD3 "glyph calls only HasInfoBar"
 *     note for InterfaceVersion "V4 has no interface class in this vendored subset"
 * ```
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include <Windows.h>

namespace TRUEHUD_API
{
constexpr const auto TrueHUDPluginName = "TrueHUD";

/**
 * @enum InterfaceVersion
 * @brief Available TrueHUD interface versions.
 */
enum class InterfaceVersion : uint8_t
{
    V1,
    V2,
    V3,
    V4
};

/**
 * @enum APIResult
 * @brief Result codes returned by the TrueHUD API.
 */
enum class APIResult : uint8_t
{
    OK,
    NotOwner,
    MustKeep,
    AlreadyGiven,
    AlreadyTaken,
    WidgetFailedToLoad,
    BadThread,
};

/**
 * @enum WidgetRemovalMode
 * @brief Available widget removal behaviors.
 */
enum class WidgetRemovalMode : uint8_t
{
    Immediate,
    Normal,
    Delayed,
};

/**
 * @enum BarColorType
 * @brief Bar color channels supported by TrueHUD interface version 2.
 *
 * glyph does not use these color overrides.
 */
enum class BarColorType : uint8_t
{
    FlashColor,
    BarColor,
    PhantomColor,
    BackgroundColor,
    PenaltyColor,
};

class WidgetBase;

using SpecialResourceCallback = std::function<float(RE::Actor* a_actor)>;
using APIResultCallback = std::function<void(APIResult)>;

/**
 * @class IVTrueHUD1
 * @brief TrueHUD mod interface version 1.
 */
class IVTrueHUD1
{
public:
    [[nodiscard]] virtual unsigned long GetTrueHUDThreadId() const noexcept = 0;
    [[nodiscard]] virtual APIResult RequestTargetControl(
        SKSE::PluginHandle a_myPluginHandle) noexcept = 0;
    [[nodiscard]] virtual APIResult RequestSpecialResourceBarsControl(
        SKSE::PluginHandle a_myPluginHandle) noexcept = 0;
    virtual APIResult SetTarget(SKSE::PluginHandle a_myPluginHandle,
                                RE::ActorHandle a_actorHandle) noexcept = 0;
    virtual APIResult SetSoftTarget(SKSE::PluginHandle a_myPluginHandle,
                                    RE::ActorHandle a_actorHandle) noexcept = 0;
    virtual void AddActorInfoBar(RE::ActorHandle a_actorHandle) noexcept = 0;
    virtual void RemoveActorInfoBar(RE::ActorHandle a_actorHandle,
                                    WidgetRemovalMode a_removalMode) noexcept = 0;
    virtual void AddBoss(RE::ActorHandle a_actorHandle) noexcept = 0;
    virtual void RemoveBoss(RE::ActorHandle a_actorHandle,
                            WidgetRemovalMode a_removalMode) noexcept = 0;
    virtual void FlashActorValue(RE::ActorHandle a_actorHandle,
                                 RE::ActorValue a_actorValue,
                                 bool a_bLong) noexcept = 0;
    virtual APIResult FlashActorSpecialBar(SKSE::PluginHandle a_myPluginHandle,
                                           RE::ActorHandle a_actorHandle,
                                           bool a_bLong) noexcept = 0;
    virtual APIResult RegisterSpecialResourceFunctions(
        SKSE::PluginHandle a_myPluginHandle,
        SpecialResourceCallback&& a_getCurrentSpecialResource,
        SpecialResourceCallback&& a_getMaxSpecialResource,
        bool a_bSpecialMode,
        bool a_bDisplaySpecialForPlayer = true) noexcept = 0;
    virtual void LoadCustomWidgets(SKSE::PluginHandle a_myPluginHandle,
                                   std::string_view a_filePath,
                                   APIResultCallback&& a_successCallback) noexcept = 0;
    virtual void RegisterNewWidgetType(SKSE::PluginHandle a_myPluginHandle,
                                       uint32_t a_widgetType) noexcept = 0;
    virtual void AddWidget(SKSE::PluginHandle a_myPluginHandle,
                           uint32_t a_widgetType,
                           uint32_t a_widgetID,
                           std::string_view a_symbolIdentifier,
                           std::shared_ptr<WidgetBase> a_widget) noexcept = 0;
    virtual void RemoveWidget(SKSE::PluginHandle a_myPluginHandle,
                              uint32_t a_widgetType,
                              uint32_t a_widgetID,
                              WidgetRemovalMode a_removalMode) noexcept = 0;
    virtual SKSE::PluginHandle GetTargetControlOwner() const noexcept = 0;
    virtual SKSE::PluginHandle GetPlayerWidgetBarColorsControlOwner() const noexcept = 0;
    virtual SKSE::PluginHandle GetSpecialResourceBarControlOwner() const noexcept = 0;
    virtual APIResult ReleaseTargetControl(SKSE::PluginHandle a_myPluginHandle) noexcept = 0;
    virtual APIResult ReleaseSpecialResourceBarControl(
        SKSE::PluginHandle a_myPluginHandle) noexcept = 0;
};

/**
 * @class IVTrueHUD2
 * @brief TrueHUD mod interface version 2.
 *
 * This version adds per-bar color overrides. glyph keeps the functions only to preserve the
 * vtable layout.
 */
class IVTrueHUD2 : public IVTrueHUD1
{
public:
    virtual APIResult OverrideBarColor(RE::ActorHandle a_actorHandle,
                                       RE::ActorValue a_actorValue,
                                       BarColorType a_colorType,
                                       uint32_t a_color) noexcept = 0;
    virtual APIResult OverrideSpecialBarColor(RE::ActorHandle a_actorHandle,
                                              BarColorType a_colorType,
                                              uint32_t a_color) noexcept = 0;
    virtual APIResult RevertBarColor(RE::ActorHandle a_actorHandle,
                                     RE::ActorValue a_actorValue,
                                     BarColorType a_colorType) noexcept = 0;
    virtual APIResult RevertSpecialBarColor(RE::ActorHandle a_actorHandle,
                                            BarColorType a_colorType) noexcept = 0;
};

/**
 * @class IVTrueHUD3
 * @brief TrueHUD mod interface version 3.
 *
 * This version adds debug drawing and `HasInfoBar`. glyph uses only `HasInfoBar`.
 */
class IVTrueHUD3 : public IVTrueHUD2
{
public:
    virtual void DrawLine(const RE::NiPoint3& a_start,
                          const RE::NiPoint3& a_end,
                          float a_duration = 0.f,
                          uint32_t a_color = 0xFF0000FF,
                          float a_thickness = 1.f) noexcept = 0;
    virtual void DrawPoint(const RE::NiPoint3& a_position,
                           float a_size,
                           float a_duration = 0.f,
                           uint32_t a_color = 0xFF0000FF) noexcept = 0;
    virtual void DrawArrow(const RE::NiPoint3& a_start,
                           const RE::NiPoint3& a_end,
                           float a_size = 10.f,
                           float a_duration = 0.f,
                           uint32_t a_color = 0xFF0000FF,
                           float a_thickness = 1.f) noexcept = 0;
    virtual void DrawBox(const RE::NiPoint3& a_center,
                         const RE::NiPoint3& a_extent,
                         const RE::NiQuaternion& a_rotation,
                         float a_duration = 0.f,
                         uint32_t a_color = 0xFF0000FF,
                         float a_thickness = 1.f) noexcept = 0;
    virtual void DrawCircle(const RE::NiPoint3& a_center,
                            const RE::NiPoint3& a_x,
                            const RE::NiPoint3& a_y,
                            float a_radius,
                            uint32_t a_segments,
                            float a_duration = 0.f,
                            uint32_t a_color = 0xFF0000FF,
                            float a_thickness = 1.f) noexcept = 0;
    virtual void DrawHalfCircle(const RE::NiPoint3& a_center,
                                const RE::NiPoint3& a_x,
                                const RE::NiPoint3& a_y,
                                float a_radius,
                                uint32_t a_segments,
                                float a_duration = 0.f,
                                uint32_t a_color = 0xFF0000FF,
                                float a_thickness = 1.f) noexcept = 0;
    virtual void DrawSphere(const RE::NiPoint3& a_origin,
                            float a_radius,
                            uint32_t a_segments = 16,
                            float a_duration = 0.f,
                            uint32_t a_color = 0xFF0000FF,
                            float a_thickness = 1.f) noexcept = 0;
    virtual void DrawCylinder(const RE::NiPoint3& a_start,
                              const RE::NiPoint3& a_end,
                              float a_radius,
                              uint32_t a_segments,
                              float a_duration = 0.f,
                              uint32_t a_color = 0xFF0000FF,
                              float a_thickness = 1.f) noexcept = 0;
    virtual void DrawCone(const RE::NiPoint3& a_origin,
                          const RE::NiPoint3& a_direction,
                          float a_length,
                          float a_angleWidth,
                          float a_angleHeight,
                          uint32_t a_segments,
                          float a_duration = 0.f,
                          uint32_t a_color = 0xFF0000FF,
                          float a_thickness = 1.f) noexcept = 0;
    virtual void DrawCapsule(const RE::NiPoint3& a_origin,
                             float a_halfHeight,
                             float a_radius,
                             const RE::NiQuaternion& a_rotation,
                             float a_duration = 0.f,
                             uint32_t a_color = 0xFF0000FF,
                             float a_thickness = 1.f) noexcept = 0;

    /**
     * @brief Report whether TrueHUD displays an info bar for an actor.
     *
     * When `a_bFloatingOnly` is true, the query includes only bars that float above the
     * actor's head and can overlap a nameplate.
     *
     * @param a_actorHandle    Actor to query.
     * @param a_bFloatingOnly  Whether to include only floating bars.
     * @return                 True when a matching info bar is visible.
     */
    [[nodiscard]] virtual bool HasInfoBar(RE::ActorHandle a_actorHandle,
                                          bool a_bFloatingOnly = false) const noexcept = 0;
};

using _RequestPluginAPI = void* (*)(const InterfaceVersion interfaceVersion);

/**
 * @brief Request a TrueHUD API interface.
 *
 * @param a_interfaceVersion  Required interface version.
 * @return                    The interface pointer, or nullptr when TrueHUD is absent or does
 *                            not support the requested version.
 */
[[nodiscard]] inline void* RequestPluginAPI(
    const InterfaceVersion a_interfaceVersion = InterfaceVersion::V3)
{
    const auto pluginHandle = GetModuleHandleA("TrueHUD.dll");
    if (!pluginHandle)
    {
        return nullptr;
    }
    const auto requestAPIFunction =
        reinterpret_cast<_RequestPluginAPI>(GetProcAddress(pluginHandle, "RequestPluginAPI"));
    if (requestAPIFunction)
    {
        return requestAPIFunction(a_interfaceVersion);
    }
    return nullptr;
}
}  // namespace TRUEHUD_API
