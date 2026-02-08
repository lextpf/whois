#pragma once

/**
 * Minimal NiOverride interface definitions for SSE 1.5.97
 *
 * These are stripped-down versions of the NiOverride interfaces,
 * containing only what's needed for overlay copying functionality.
 * Avoids pulling in the full SKSE type system.
 */

#include <cstdint>

// Forward declaration - we use this as an opaque pointer
class TESObjectREFR;

/**
 * Base interface that all NiOverride interfaces inherit from.
 */
class IPluginInterface
{
public:
    IPluginInterface() {}
    virtual ~IPluginInterface() {}

    virtual uint32_t GetVersion() = 0;
    virtual void Revert() = 0;
};

/**
 * Interface map for querying specific interfaces by name.
 */
class IInterfaceMap
{
public:
    virtual IPluginInterface* QueryInterface(const char* name) = 0;
    virtual bool AddInterface(const char* name, IPluginInterface* pluginInterface) = 0;
    virtual IPluginInterface* RemoveInterface(const char* name) = 0;
};

/**
 * Message sent by NiOverride containing the interface map.
 * Received via SKSE messaging when registered as a listener for "NiOverride".
 */
struct InterfaceExchangeMessage
{
    enum : uint32_t
    {
        kMessage_ExchangeInterface = 0x9E3779B9
    };

    IInterfaceMap* interfaceMap = nullptr;
};

/**
 * Overlay interface - manages overlay nodes on actors.
 *
 * This is a minimal declaration matching the NiOverride OverlayInterface vtable.
 * The actual implementation lives in NiOverride.dll.
 *
 * Note: OverlayInterface also inherits from IAddonAttachmentInterface, but we
 * don't need those methods. The vtable layout puts IPluginInterface virtuals first.
 */
class IOverlayInterface : public IPluginInterface
{
public:
    // IPluginInterface overrides are inherited

    // OverlayInterface-specific methods (in vtable order)
    virtual void Save(void* intfc, uint32_t kVersion) = 0;
    virtual bool Load(void* intfc, uint32_t kVersion) = 0;
    // Revert is inherited from IPluginInterface

    virtual bool HasOverlays(TESObjectREFR* reference) = 0;
    virtual void AddOverlays(TESObjectREFR* reference) = 0;
    virtual void RemoveOverlays(TESObjectREFR* reference) = 0;
    virtual void RevertOverlays(TESObjectREFR* reference, bool resetDiffuse) = 0;
    virtual void RevertOverlay(TESObjectREFR* reference, void* nodeName, uint32_t armorMask, uint32_t addonMask, bool resetDiffuse) = 0;
    virtual void EraseOverlays(TESObjectREFR* reference) = 0;
    virtual void RevertHeadOverlays(TESObjectREFR* reference, bool resetDiffuse) = 0;
    virtual void RevertHeadOverlay(TESObjectREFR* reference, void* nodeName, uint32_t partType, uint32_t shaderType, bool resetDiffuse) = 0;
};

/**
 * Override interface - manages property overrides on actors.
 *
 * This is a minimal declaration. The actual interface has many more methods,
 * but we only need GetVersion() for logging.
 */
class IOverrideInterface : public IPluginInterface
{
public:
    // We inherit GetVersion() and Revert() from IPluginInterface
    // The actual interface has 50+ methods but we don't call them directly
};
