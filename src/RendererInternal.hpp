#pragma once

#include "PCH.hpp"

#include "DebugOverlay.hpp"
#include "RenderConstants.hpp"
#include "Settings.hpp"
#include "TextEffects.hpp"
#include "Utf8Utils.hpp"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @namespace Renderer
 * @brief Shared internal types and state for the Renderer translation units.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Renderer
 *
 * The public entry points live in Renderer.hpp.  This file declares what the five
 * translation units that include it share: Renderer.cpp (frame orchestration),
 * RendererSnapshot.cpp (game thread), RendererLayout.cpp (measurement),
 * RendererEffects.cpp (draw passes) and Deck.cpp (card capture).
 *
 * Thread affinity is part of every declaration here.  A name suffixed `_GameThread` runs on
 * the game thread only; `_RenderThread` and `...RT` run on the render thread only.
 * `ActorDrawData` is the plain-data handover between the two threads and must never hold an
 * `RE::Actor*`.
 *
 * ## :material-swap-horizontal: Snapshot handshake
 *
 * The atomics in RendererState coalesce update requests and let a hot reload drain the
 * pipeline before Settings::Load() runs.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * sequenceDiagram
 *     participant RT as Render thread
 *     participant Q as SKSE task queue
 *     participant GT as Game thread
 *     RT->>RT: QueueSnapshotUpdate_RenderThread
 *     Note over RT: no-op while pauseSnapshotUpdates is set,<br/>or if updateQueued was set
 *     RT->>Q: AddTask
 *     Q->>GT: UpdateSnapshot_GameThread
 *     GT->>GT: snapshotUpdateRunning = true
 *     GT->>GT: publish allowOverlay and allowDeck
 *     GT->>GT: scan actors, derive plate facts, publish activeRegister
 *     GT->>RT: snapshot and crosshairTarget, under snapshotLock
 *     GT->>GT: snapshotUpdateRunning = false, updateQueued = false
 *     RT->>RT: Draw copies the snapshot under snapshotLock
 * ```
 *
 * ## :material-vector-link: Per-label Data Derivation
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef input fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef state fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *     classDef resolved fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef draw fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     A[ActorDrawData<br/>game-thread facts]:::input --> R[Frame resolution]:::resolved
 *     S[RenderSettingsSnapshot<br/>immutable settings]:::input --> R
 *     C[ActorCache<br/>persistent animation state]:::state --> R
 *     R -.->|advance for the next frame| C
 *     R --> D[DistanceFactors]:::resolved
 *     R --> X[ActorLabelContext<br/>and RenderSeg text]:::resolved
 *     R --> B[BadgeComposition]:::resolved
 *     R --> Y[LabelStyle]:::resolved
 *     D --> L[ComputeLabelLayout]:::resolved
 *     X --> L
 *     B --> L
 *     Y --> L
 *     C --> L
 *     L --> O[LabelLayout<br/>and BadgeDrawItem]:::resolved
 *     O --> P[Draw passes]:::draw
 *     Y --> P
 *     P --> I[ImDrawList]:::draw
 * ```
 */
namespace Renderer
{
using Utf8Utils::Utf8CharCount;
using Utf8Utils::Utf8Next;
using Utf8Utils::Utf8Truncate;

// ============================================================================
// Render-thread Settings snapshot
// ============================================================================

/**
 * @struct RenderSettingsSnapshot
 * @brief Render-thread copy of the Settings values the draw path consumes.
 * @ingroup Renderer
 *
 * Holds every mutable Settings value the per-frame draw path uses, so the render thread never
 * reads non-POD Settings globals without a lock.  `RefreshCachedSettingsSnapshot`
 * (Renderer.cpp) recaptures it under a `Settings::Mutex()` shared lock only when
 * `Settings::Generation()` advances, from `Draw()` and `PrepareDeckCaptureRT()`; in steady
 * state nothing is captured.  The copy lives at `RendererState::cachedSnap` and is passed by
 * const reference to every render-thread function.
 *
 * Two values are read from Settings directly under a shared lock instead: the Deck block
 * (`TickRT`, `PrepareDeckCaptureRT`) and the reload key (`HandleHotReload` reads
 * `Settings::Display().ReloadKey`, not the `reloadKey` member below).
 */
struct RenderSettingsSnapshot
{
    // Distance and fade
    float fadeStartDistance = .0f;
    float fadeEndDistance = .0f;
    float scaleStartDistance = .0f;
    float scaleEndDistance = .0f;
    float minimumScale = .0f;

    // Outline and shadow
    float outlineWidthMin = .0f;
    float outlineWidthMax = .0f;
    float titleShadowOffsetX = .0f;
    float titleShadowOffsetY = .0f;
    float mainShadowOffsetX = .0f;
    float mainShadowOffsetY = .0f;
    bool fastOutlines = false;

    // Glow
    bool enableGlow = false;
    float glowRadius = .0f;
    float glowIntensity = .0f;
    int glowSamples = 0;
    float glowDivideStrength = .0f;

    // Outline glow
    bool enableOutlineGlow = false;
    float outlineGlowScale = 1.6f;
    float outlineGlowAlpha = .1f;
    int outlineGlowRings = 2;
    float outlineGlowR = 1.0f;
    float outlineGlowG = 1.0f;
    float outlineGlowB = 1.0f;
    bool outlineGlowTierTint = false;
    // Dual-tone directional outline
    bool dualOutlineEnabled = false;
    float innerOutlineTint = .3f;
    float innerOutlineAlpha = .5f;
    float innerOutlineScale = .5f;
    float directionalLightAngle = 315.f;
    float directionalLightBias = .15f;

    float outlineColorTint = .0f;
    float shadowColorTint = .0f;

    // Soft directional drop-shadow
    bool softShadowEnabled = false;
    float softShadowDistance = 4.0f;
    float softShadowSoftness = 3.0f;
    float softShadowOpacity = .8f;
    float softShadowAngle = 45.0f;
    int softShadowSamples = 12;

    // Shine overlay
    bool enableShine = false;
    float shineIntensity = .35f;
    float shineFalloff = 2.0f;
    float textGlowAlpha = .0f;

    // Typewriter
    bool enableTypewriter = false;
    float typewriterSpeed = .0f;
    float typewriterDelay = .0f;

    // Ornaments
    bool enableOrnaments = false;
    float ornamentScale = .0f;
    float ornamentSpacing = .0f;
    std::string ornamentFontPath = {};
    float ornamentFontSize = .0f;
    bool ornamentAnchorToMainLine = true;
    float ornamentOffsetY = .0f;

    // Particles
    bool useParticleTextures = false;
    bool enableParticleAura = false;
    int particleCount = 0;
    float particleSize = .0f;
    float particleSpeed = .0f;
    float particleSpread = .0f;
    float particleAlpha = .0f;
    int particleBlendMode = 0;
    float particleDepthStrength = .7f;    ///< Depth drive on size, alpha, parallax [0, 1.5]
    float particleColorWarmth = .5f;      ///< Warm/cool depth temperature mix [0, 1]
    float particleGlowStrength = .28f;    ///< Additive backlight halo alpha [0, 1]
    float particleGlowSize = 2.2f;        ///< Halo radius, multiple of the sprite [1, 4]
    float particleShineThreshold = .84f;  ///< Glint sine threshold [0, 0.99]; higher = rarer

    // Animation and color
    float innerTextAlpha = 1.0f;
    float outlineAlpha = 1.0f;

    /**
     * @struct NpcColors
     * @brief Flat white-leaning colors for NPC nameplate text.
     *
     * @see Settings::NpcColorSettings
     */
    struct NpcColors
    {
        Settings::Color3 neutral{};   ///< Neutral and talkable civilians
        Settings::Color3 hostile{};   ///< Hostiles; slight warm lean
        Settings::Color3 follower{};  ///< Teammates; slight cool lean
        Settings::Color3 level{};     ///< Level readout; dimmed silver
        Settings::Color3 title{};     ///< Title text; soft white
    };
    NpcColors npcColors = {};

    // Smoothing
    float alphaSettleTime = .0f;
    float scaleSettleTime = .0f;
    float positionSettleTime = .0f;
    float occlusionSettleTime = .0f;

    // Display
    bool enableDebugOverlay = false;
    bool enableOcclusionCulling = false;
    float verticalOffset = .0f;
    float horizontalOffset = .0f;
    bool hidePlayer = false;
    int maxPlates = RenderConstants::DEFAULT_MAX_PLATES;
    int maxScanActors = RenderConstants::DEFAULT_MAX_SCAN_ACTORS;
    int reloadKey = 0;

    // Font size (paths are only used during ImGui init, not per-frame)
    float nameFontSize = .0f;

    // Entrance and exit transitions
    bool enableEntrance = false;
    int entranceStyle = 0;
    float entranceDuration = .35f;
    bool enableExit = false;
    float exitDuration = .20f;
    float entranceStaggerStep = .06f;
    float entranceStaggerMax = .8f;

    // Visual polish settings (value copy)
    Settings::VisualSettings visual = {};

    // Non-POD collections (value copies - small, recaptured on generation change)
    std::vector<Settings::TierDefinition> tiers = {};
    std::string titleFormat = {};
    std::vector<Settings::Segment> displayFormat = {};
    std::vector<Settings::Segment> infoFormat = {};
    std::vector<Settings::SpecialTitleDefinition> specialTitles = {};

    // Pre-sorted special titles (pointers into specialTitles above).
    // Populated once per snapshot via PopulateSortedSpecialTitles().
    std::vector<const Settings::SpecialTitleDefinition*> sortedSpecialTitles = {};

    /**
     * @struct LabelTokens
     * @brief Resolved text and thresholds for contextual nameplate tokens.
     *
     * The values support `%r`, `%d`, and `%c`. They remain plain values so render helpers can
     * use a `string_view` for each actor without locking the settings again.
     */
    struct LabelTokens
    {
        std::string relFollower;
        std::string relAlly;
        std::string relNeutral;
        std::string relHostile;
        std::string ldWeak;
        std::string ldEven;
        std::string ldStrong;
        std::string ldDeadly;
        std::string ctNPC;
        std::string ctBeast;
        std::string ctUndead;
        std::string ctDaedra;
        std::string ctDragon;
        int deltaWeakBelow = -5;
        int deltaStrongAbove = 5;
        int deltaDeadlyAbove = 10;
    };
    LabelTokens labels = {};

    /**
     * @struct IconTokens
     * @brief Resolved configuration for status-icon badges.
     *
     * `Settings::ClampAndValidate` derives the colors. Icon names map to duotone SVG textures
     * that `BadgeTextures` loads. Layout drops a badge when its icon is not loaded. `enabled`
     * is false when badges are disabled or no icon folder is configured.
     */
    struct IconTokens
    {
        bool enabled = false;
        float scale = 1.0f;
        bool deadlyPulse = true;
        std::string icoFollower;
        std::string icoAlly;
        std::string icoHostile;
        std::string icoWeak;
        std::string icoStrong;
        std::string icoDeadly;
        std::string icoBeast;
        std::string icoUndead;
        std::string icoDaedra;
        std::string icoDragon;
        Settings::Color3 colFollower{};
        Settings::Color3 colAlly{};
        Settings::Color3 colHostile{};
        Settings::Color3 colWeak{};
        Settings::Color3 colStrong{};
        Settings::Color3 colDeadly{};
        Settings::Color3 colCreature{};

        // Always-on neutral resting states, plus the NPC category slots.
        std::string icoNeutral;   ///< Resting relationship slot (muted)
        std::string icoHumanoid;  ///< Resting creature slot (muted)
        std::string icoEven;      ///< Resting threat slot (muted)
        std::string icoGuard;
        std::string icoMerchant;
        std::string icoCommoner;  ///< Resting role slot (muted)
        std::string icoEssential;
        std::string icoProtected;
        std::string icoMortal;  ///< Resting protection slot (muted)
        std::string icoCombat;
        std::string icoAlert;
        std::string icoIdle;  ///< Resting engagement slot (muted)
        // Player slot icons.
        std::string icoSneakHidden;
        std::string icoSneakDetected;
        std::string icoSneakOff;  ///< Resting sneak slot (muted)
        std::string icoEncumbered;
        std::string icoNormalWeight;  ///< Resting encumbrance slot (muted)
        std::string icoWanted;
        std::string icoBountyClear;  ///< Resting bounty slot (muted)
        // Actor rank badge (low / mid / high ladder thirds).
        std::string icoTierLow;
        std::string icoTierMid;
        std::string icoTierHigh;
        // Lit colors for the active states.
        Settings::Color3 colGuard{};
        Settings::Color3 colMerchant{};
        Settings::Color3 colEssential{};
        Settings::Color3 colProtected{};
        Settings::Color3 colCombat{};
        Settings::Color3 colAlert{};
        Settings::Color3 colSneakHidden{};
        Settings::Color3 colSneakDetected{};
        Settings::Color3 colEncumbered{};
        Settings::Color3 colWanted{};
        Settings::Color3 colTierLow{};
        Settings::Color3 colTierMid{};
        Settings::Color3 colTierHigh{};
        // Full-color emblem images for the tier badge.  Used instead of the medal/gem/crown
        // icons above when enabled and the images loaded.
        bool tierBadgeImages = false;  ///< Use emblem PNGs for the tier badge
        int tierImageCount = 0;        ///< Emblem images loaded (from BadgeTextures)
        float tierBadgeGamma = 1.8f;   ///< Top-weighting exponent (>1 = high emblems rarer)
        float tierBadgeScale = 1.7f;   ///< Emblem size as a multiple of the status-icon size
        // Per-slot resting colors; each muted slot carries its own hue.
        Settings::Color3 colNeutral{};
        Settings::Color3 colHumanoid{};
        Settings::Color3 colCommoner{};
        Settings::Color3 colMortal{};
        Settings::Color3 colEven{};
        Settings::Color3 colIdle{};
        Settings::Color3 colSneakOff{};
        Settings::Color3 colNormalWeight{};
        Settings::Color3 colBountyClear{};
        Settings::Color3 colMuted{};  ///< Unused shared tint; no draw path reads it
        // Per-slot enables (a disabled active state drops that actor's badge).
        bool relationshipEnabled = true;
        bool creatureEnabled = true;
        bool threatEnabled = true;
        bool roleEnabled = true;
        bool protectionEnabled = true;
        bool engagementEnabled = true;  ///< Master gate for the NPC engagement slot
        bool combatStateEnabled = true;
        bool alertStateEnabled = true;
        bool sneakEnabled = true;
        bool playerCombatEnabled = true;
        bool encumberedEnabled = true;
        bool bountyEnabled = true;
        bool tierEnabled = true;  ///< Gate for the actor rank badge
        float opacity = 0.92f;
        // Muted styling: alpha multiplier and desaturation strength [0,1].
        float mutedAlpha = 1.0f;
        float mutedDesat = 0.18f;
        // Player-only lighting block behind the badge strip and the emblem (see DrawBadges
        // and DrawTierEmblem).  Colors are optional: empty => derive from the Name color.
        bool playerStripBedEnabled = true;
        float playerStripBedAlpha = 0.10f;
        float playerStripBedSize = 2.6f;
        float playerStripBedBreatheHz = 0.14f;
        std::optional<Settings::Color3> playerStripBedColor;
        bool emblemBacklightEnabled = true;
        float emblemBacklightSize = 2.6f;
        float emblemBacklightAlpha = 0.55f;
        float emblemBacklightBreatheHz = 0.167f;
        float emblemCrispAlpha = 0.95f;
        std::optional<Settings::Color3> emblemBacklightColor;

        bool playerRimLightEnabled = true;
        float playerRimAlpha = 0.22f;
        float playerCarveAlpha = 0.26f;
        float playerRimOffset = 1.0f;
        std::optional<Settings::Color3> playerRimColor;
        bool emblemKeyFillEnabled = true;
        float emblemKeyAlpha = 0.35f;
        float emblemFillAlpha = 0.15f;
        float emblemKeyRise = 0.18f;
        float emblemFillDrop = 0.15f;
        std::optional<Settings::Color3> emblemKeyColor;
        std::optional<Settings::Color3> emblemFillColor;
    };
    IconTokens icons = {};

    /// Focus-target expanded-nameplate settings (value copy).
    Settings::FocusSettings focus = {};

    /// Graffito perspective-correct actor-plane typography settings.
    Settings::GraffitoSettings graffito = {};
    float graffitoWrapRadians = .0f;  ///< Cached WrapDegrees conversion for render-time geometry.

    /// Camera-motion quieting settings (value copy).
    Settings::QuietSettings quiet = {};

    /// Death-fade settings, from the DeathRite INI section.
    bool deathRiteEnabled = true;
    float deathRiteDuration = 1.6f;

    /// Context-conditional setting profiles, from the RegisterN INI sections (value copies).
    bool registersEnabled = true;
    float registerTransitionTime = 1.2f;
    std::vector<Settings::RegisterDefinition> registers = {};

    /// HUD-compat yield settings: fade our plate when another HUD mod owns the actor.
    float compatTrueHUDYieldAlpha = .0f;
    float compatYieldSettleTime = .3f;

    /// Scene-adaptive text color settings.
    bool candleEnabled = true;
    float candleStrength = .08f;
    float candleWarmth = .5f;
    float candleSettleTime = .6f;

    /// Per-pixel depth occlusion settings.
    bool depthClipEnabled = true;
    float depthClipFeather = 2.5f;

    /// @brief Capture all render settings while a shared settings lock is held.
    static RenderSettingsSnapshot CaptureFromSettings();

    /**
     * @brief Build the priority-ordered special-title pointer list.
     *
     * The function removes entries with an empty `keywordLower` and sorts the other entries
     * by descending priority. Call it only after the snapshot reaches its final storage
     * location. The pointers refer to elements in `specialTitles`. Copying the snapshot after
     * this call leaves the copy with pointers into the original snapshot.
     */
    void PopulateSortedSpecialTitles();
};

// ============================================================================
// Structs
// ============================================================================

/**
 * @struct ActorCache
 * @brief Per-actor render-thread cache for smooth nameplate animation.
 * @ingroup Renderer
 *
 * Holds the smoothed values and the animation state for one actor's plate: position, alpha,
 * scale, occlusion, entrance and exit, and the effect timers.  Keyed by formID in
 * `RendererState::cache`.  Render-thread only.
 */
struct ActorCache
{
    /// Smoothed screen position: an exponential approach (positionSettleTime, or a fixed
    /// 0.015 s for the player) blended with the posHistory moving average by
    /// Visual().PositionSmoothingBlend.  A jump larger than Visual().LargeMovementThreshold
    /// advances by Visual().LargeMovementBlend instead, so the head never lags a hard screen
    /// jump.
    ImVec2 smooth{};
    float alphaSmooth = 1.0f;      ///< Smoothed alpha for fade transitions
    float textSizeScale = 1.0f;    ///< Smoothed font scale for distance-based sizing
    float occlusionSmooth = 1.0f;  ///< Smoothed occlusion (1.0=visible, 0.0=hidden)

    bool initialized = false;    ///< True after first frame of data
    uint32_t lastSeenFrame = 0;  ///< Frame counter when actor was last in snapshot

    // The next two fields are never read or written.  The live line-of-sight throttle state
    // is OcclusionCacheEntry, on the game thread.
    uint32_t lastOcclusionCheckFrame = 0;  ///< Unused; see OcclusionCacheEntry::lastCheckFrame
    bool cachedOccluded = false;           ///< Unused; see OcclusionCacheEntry::cachedOccluded
    bool wasOccluded = false;              ///< Previous frame's occlusion state

    static constexpr int HISTORY_SIZE = RenderConstants::POSITION_HISTORY_SIZE;
    ImVec2 posHistory[HISTORY_SIZE]{};  ///< Circular buffer of raw screen positions
    int historyIndex = 0;               ///< Current write index in posHistory
    bool historyFilled = false;         ///< True once posHistory has wrapped at least once

    float typewriterTime = .0f;       ///< Seconds since actor first appeared
    bool typewriterComplete = false;  ///< True when reveal animation finished

    float entrancePhase = .0f;  ///< Entrance animation progress (0=start, 1=complete)
    float exitPhase = .0f;      ///< Exit animation progress (0=visible, 1=fully exited)
    bool entranceDone = false;  ///< True when entrance animation finished

    /// Entrance stagger: seconds this plate still waits before its entrance begins.  A
    /// negative value means not yet assigned; on the first frame the entrance block runs
    /// for this plate it claims the next stagger slot in snapshot order, which is the
    /// player first, then the camera-ray target, then the Deck target, then nearest first.
    float entranceDelay = -1.0f;

    // ---- Death fade - one-shot, replays the last live facts ----
    // It plays only for actors this cache entry saw alive; corpses first seen dead never
    // render it.  deathDone latches, so the fade never repeats for the same corpse while
    // this entry lives.
    bool sawAlive = false;   ///< Entry rendered this actor alive at least once
    float deathPhase = .0f;  ///< Fade progress (0=just died, 1=finished)
    bool deathDone = false;  ///< Fade finished, or cancelled on overlay wake

    /// Smoothed focus state for the focus-target expanded nameplate.  Targets 1.0 while this
    /// actor is the cone winner, 0.0 otherwise.  Drives both the ambient dim multiplier and
    /// the title/info row fade.
    float focusSmooth = .0f;

    /// Smoothed actor yaw used by Graffito.  Keeping the angle on the render thread avoids
    /// visible stepping at the game-thread snapshot cadence.
    float graffitoYaw = .0f;
    bool graffitoYawInitialized = false;
    bool graffitoForcedFront = false;  ///< Previous live frame was held readable by the aim ray

    // ---- Low-latency player pose tracking ----
    // The game thread publishes rendered-head samples.  On render frames that reuse a
    // sample, a bounded velocity estimate bridges the gap.  NPCs do not use these fields.
    RE::NiPoint3 playerPoseSample{};    ///< Last published rendered-head position (world units)
    RE::NiPoint3 playerPoseVelocity{};  ///< Estimated head velocity (world units per second)
    /// Time of playerPoseSample, in seconds on the monotonic pose clock
    /// (`PoseClockSeconds()`, a steady_clock epoch - not `ImGui::GetTime()`).
    double playerPoseSampleTime = .0;
    /// Measured seconds between game-thread samples.  The measurement is clamped to the
    /// range 0.001 s to 0.050 s.  Seeded to 1/60 s and held there until a second sample
    /// arrives.
    double playerPoseInterval = 1.0 / 60.0;
    bool playerPoseInitialized = false;  ///< True once a first sample seeded the tracker

    /// Smoothed HUD-compat yield state.  Targets 1.0 while another HUD mod (TrueHUD) floats
    /// a widget over this actor, fading the plate to the configured yield alpha and back on
    /// the same curve.  It settles on `compatYieldSettleTime`, not on the focus weight.
    float yieldSmooth = .0f;

    // ---- Smoothed scene sample behind this plate ----
    float bgLum = -1.0f;  ///< Smoothed background luminance (-1 = no sample yet)
    float bgR = .5f;      ///< Smoothed background red
    float bgG = .5f;      ///< Smoothed background green
    float bgB = .5f;      ///< Smoothed background blue

    /// Motion trail history, separate from the posHistory used for smoothing.  Stored in
    /// world space and reprojected through the current camera each frame, so a pure camera
    /// pan maps every ghost onto the head and only real actor movement leaves a trail.
    static constexpr int TRAIL_HISTORY_SIZE = 8;
    RE::NiPoint3 trailHistory[TRAIL_HISTORY_SIZE]{};  ///< Trail ghost positions (world space)
    int trailIndex = 0;                               ///< Current write index in trailHistory
    bool trailFilled = false;                         ///< True once trailHistory has wrapped

    std::string cachedName;       ///< Last known name (to detect changes)
    std::string cachedNameLower;  ///< Pre-lowered name for special title matching

    /**
     * @brief Add a screen-position sample and compute the ring-buffer mean.
     *
     * The first call returns the new sample without modification.
     *
     * @param pos  Screen position to add.
     * @return     The unweighted mean of the filled part of the ring buffer.
     */
    ImVec2 AddAndGetSmoothed(const ImVec2& pos)
    {
        posHistory[historyIndex] = pos;
        historyIndex = (historyIndex + 1) % HISTORY_SIZE;
        if (historyIndex == 0)
        {
            historyFilled = true;
        }

        int count = historyFilled ? HISTORY_SIZE : historyIndex;
        if (count == 0)
        {
            return pos;
        }

        ImVec2 sum{0, 0};
        for (int i = 0; i < count; i++)
        {
            sum.x += posHistory[i].x;
            sum.y += posHistory[i].y;
        }
        return ImVec2(sum.x / count, sum.y / count);
    }
};

/**
 * @enum RelationshipKind
 * @brief Player-relationship channel for the `%r` token and the badge strip.
 * @ingroup Renderer
 *
 * Also selects the folio glass tint and the NPC text support tint
 * (`RenderSettingsSnapshot::npcColors`).
 */
enum class RelationshipKind : std::uint8_t
{
    Hostile,  ///< IsHostileToActor(player)
    Neutral,  ///< Default - neither hostile, nor teammate, nor talkable
    Ally,     ///< CanTalkToPlayer (friendly NPC) but not a teammate
    Follower  ///< IsPlayerTeammate
};

/**
 * @enum LevelDelta
 * @brief Actor level relative to player level, for the `%d` token.
 * @ingroup Renderer
 *
 * The three thresholds live in `Settings::Labels()` and default to <=-5, >=+5 and >=+10.
 * `Settings::ClampAndValidate()` requires WeakAtOrBelow < StrongAtOrAbove < DeadlyAtOrAbove
 * and resets all three to those defaults when the order is broken.  The bounds are otherwise
 * free, so the Even band is not symmetric unless the user configures it that way.
 */
enum class LevelDelta : std::uint8_t
{
    Weak,    ///< delta <= WeakAtOrBelow
    Even,    ///< WeakAtOrBelow < delta < StrongAtOrAbove (the band width is user-defined)
    Strong,  ///< StrongAtOrAbove <= delta < DeadlyAtOrAbove
    Deadly   ///< delta >= DeadlyAtOrAbove (tested first, so it wins over Strong)
};

/**
 * @enum CreatureKind
 * @brief Coarse actor classification for the `%c` token.
 * @ingroup Renderer
 *
 * Derived from the `ActorTypeX` keyword set.  NPC is the fallback when no creature keyword
 * matches.
 */
enum class CreatureKind : std::uint8_t
{
    NPC,     ///< Humanoid (ActorTypeNPC) - default
    Beast,   ///< ActorTypeCreature / ActorTypeAnimal
    Undead,  ///< ActorTypeUndead
    Daedra,  ///< ActorTypeDaedra
    Dragon   ///< ActorTypeDragon
};

/**
 * @enum RoleKind
 * @brief Social role for the NPC role badge slot.
 * @ingroup Renderer
 *
 * Commoner is the muted default; Guard and Merchant render lit.  Derived from `IsGuard()`
 * and vendor-faction membership on the game thread.
 */
enum class RoleKind : std::uint8_t
{
    Commoner,  ///< No notable role - muted
    Merchant,  ///< Member of a vendor faction (TESFaction::IsVendor)
    Guard      ///< Actor::IsGuard()
};

/**
 * @enum ProtectionKind
 * @brief Invulnerability status for the protection badge slot.
 * @ingroup Renderer
 *
 * Mortal is the muted default.  Essential overrides Protected when both flags are set.
 */
enum class ProtectionKind : std::uint8_t
{
    Mortal,     ///< Killable - muted
    Protected,  ///< Only the player can kill (IsProtected)
    Essential   ///< Cannot be killed (IsEssential)
};

/**
 * @enum EngagementKind
 * @brief Awareness state for the engagement badge slot.
 * @ingroup Renderer
 *
 * One mutually exclusive axis: combat supersedes alert, alert supersedes idle.  Idle is
 * muted.
 */
enum class EngagementKind : std::uint8_t
{
    Idle,   ///< Not in combat, not alerted - muted
    Alert,  ///< Aware/searching proxy (weapon drawn + detects player)
    Combat  ///< IsInCombat()
};

/**
 * @enum SneakKind
 * @brief Player stealth state for the sneak badge slot.
 * @ingroup Renderer
 *
 * Off is the muted default (not sneaking); Hidden and Detected render lit while sneaking.
 */
enum class SneakKind : std::uint8_t
{
    Off,      ///< Not sneaking - muted
    Hidden,   ///< Sneaking, undetected
    Detected  ///< Sneaking, detected by a nearby hostile
};

/**
 * @struct ActorDrawData
 * @brief Plain-data description of one actor, handed from game to render thread.
 * @ingroup Renderer
 *
 * Every fact the render thread needs about an actor is resolved on the game thread and stored
 * here.  The struct holds no engine pointer, so the render thread can read it after the game
 * thread has moved on.
 *
 * A dead actor is published as a bare marker: identity, pose, targeting bounds, heading,
 * distance, level, uniqueness and `isDead`.  The contextual facts below (relationship, level
 * delta, creature kind, badge slots, honorific, occlusion, yield) keep their default values
 * for a corpse, because the death fade replays that actor's last live
 * `RendererState::lastDrawData` entry instead.
 */
struct ActorDrawData
{
    uint32_t formID{0};       ///< Actor's form ID (unique identifier)
    RE::NiPoint3 worldPos{};  ///< Nameplate anchor above the head, in world units
    RE::NiPoint3 feetPos{};   ///< Actor root/feet position for portrait framing
    /// Center of the actor's rendered world bound, or a height-derived sphere center when the
    /// 3D root is absent or its bound radius is outside [4, 4096] world units.
    RE::NiPoint3 raycastCenter{};
    float raycastRadius{.0f};   ///< Radius of that sphere, in world units
    float headingRadians{.0f};  ///< Actor world yaw, captured on the game thread
    /// Time of the worldPos and feetPos sample, in seconds on the monotonic pose clock
    /// (a steady_clock epoch).  The render thread extrapolates the player pose from it.
    double poseSampleTime{.0};
    std::string name;         ///< Display name (capitalized); may be empty for an NPC
    uint16_t level{0};        ///< Actor's level
    float distToPlayer{.0f};  ///< Distance to the player in world units; 0 for the player
    bool isPlayer{false};     ///< Whether this is the player character
    bool deckOnly{false};     ///< Extra crosshair target retained only for card capture
    bool isUnique{false};     ///< Unique actor base; eligible for collectible rarity rolls
    bool isOccluded{false};   ///< Line-of-sight result; stays false when occlusion is off
    /// Actor is dead - triggers the death fade,
    /// which replays the last live facts
    bool isDead{false};

    /// Faction-earned honorific, resolved on the game thread; empty means none.  It replaces
    /// the tier title in the title slot, but a special title still wins.
    std::string honorific;

    // ---- HUD-compat facts (resolved on the game thread) ----
    bool yieldPlate{false};  ///< TrueHUD floats a bar here - fade the plate out
    bool yieldLevel{false};  ///< moreHUD shows this target's level - drop ours

    // ---- Contextual nameplate facts (resolved on game thread) ----
    RelationshipKind relationship{RelationshipKind::Neutral};  ///< %r token source
    LevelDelta levelDelta{LevelDelta::Even};                   ///< %d token source
    CreatureKind creatureKind{CreatureKind::NPC};              ///< %c token source

    // ---- Status badge facts (resolved on the game thread) ----
    // NPC slots: role, protection, engagement.  Player slots: sneak, playerInCombat,
    // encumbered, wanted.  Each drives one always-on badge.
    RoleKind role{RoleKind::Commoner};                  ///< NPC role badge
    ProtectionKind protection{ProtectionKind::Mortal};  ///< NPC protection badge
    EngagementKind engagement{EngagementKind::Idle};    ///< NPC engagement badge
    SneakKind sneak{SneakKind::Off};                    ///< Player sneak badge
    bool playerInCombat{false};                         ///< Player engagement badge
    bool encumbered{false};                             ///< Player encumbered badge
    bool wanted{false};                                 ///< Player bounty badge
};

/**
 * @struct ComposedBadgeSlot
 * @brief One actor-fact badge before texture lookup and screen-space layout.
 * @ingroup Renderer
 *
 * `icon` views into the active `RenderSettingsSnapshot`, so a slot is valid only while that
 * snapshot lives.
 */
struct ComposedBadgeSlot
{
    std::string_view icon;     ///< Duotone icon name, resolved later by BadgeTextures
    Settings::Color3 color{};  ///< Semantic tint for this slot
    bool muted = false;        ///< Resting state: dimmed and desaturated at draw time
    bool pulse = false;        ///< Breathing alpha (the Deadly threat slot)
    int tierImage = -1;        ///< >=0 -> full-color emblem image in place of an icon
    bool isRank = false;       ///< Rank identity mark, distinct from actor-status slots
};

/**
 * @brief Maximum number of badges in one composition.
 *
 * Seven is the exact NPC slot count that `ComposeBadges` produces: rank, relationship,
 * creature, role, protection, threat, and engagement. Raise this value before adding an
 * eighth NPC slot. The mutators silently drop a slot that exceeds the capacity.
 */
inline constexpr int MAX_BADGE_SLOTS = 7;

/**
 * @struct BadgeComposition
 * @brief Fixed-capacity ordered badge set shared by nameplates and Deck cards.
 * @ingroup Renderer
 *
 * The mutators fail silently: a slot that does not fit, or that carries no icon, is discarded
 * without a diagnostic.
 */
struct BadgeComposition
{
    ComposedBadgeSlot slots[MAX_BADGE_SLOTS];  ///< Filled slots, in draw order
    int count = 0;                             ///< Filled slot count, at most MAX_BADGE_SLOTS

    /**
     * @brief Append one status badge slot.
     *
     * Ignored when the set is full or @p icon is empty.
     *
     * @param icon   Duotone icon name resolved later by BadgeTextures.
     * @param color  Semantic tint for the slot.
     * @param muted  True to draw the resting (dimmed, desaturated) treatment.
     * @param pulse  True to breathe the alpha (used by the Deadly threat slot).
     */
    void push(std::string_view icon, Settings::Color3 color, bool muted, bool pulse = false)
    {
        if (count < MAX_BADGE_SLOTS && !icon.empty())
        {
            slots[count++] = {icon, color, muted, pulse};
        }
    }

    /**
     * @brief Append the rank identity slot as an icon.
     *
     * Ignored when the set is full or @p icon is empty.
     *
     * @param icon   Duotone icon name for the rank band (low / mid / high).
     * @param color  Tint for the rank band.
     * @param muted  True to draw the resting treatment.
     * @param pulse  True to breathe the alpha.
     */
    void pushRank(std::string_view icon, Settings::Color3 color, bool muted, bool pulse = false)
    {
        if (count < MAX_BADGE_SLOTS && !icon.empty())
        {
            slots[count++] = {icon, color, muted, pulse, -1, true};
        }
    }

    /**
     * @brief Append the rank identity slot as a full-color emblem image.
     *
     * Ignored when @p enabled is false, the set is full, or @p imageIndex is
     * negative.
     *
     * @param enabled     Rank badge gate (`IconTokens::tierEnabled`).
     * @param imageIndex  Emblem image index; negative means no emblem resolved.
     */
    void pushTierImage(bool enabled, int imageIndex)
    {
        if (enabled && count < MAX_BADGE_SLOTS && imageIndex >= 0)
        {
            slots[count++] = ComposedBadgeSlot{{}, {}, false, false, imageIndex, true};
        }
    }
};

/**
 * @struct OcclusionCacheEntry
 * @brief Cached line-of-sight result for one actor, keyed by formID.
 * @ingroup Renderer
 *
 * A fresh check runs only after `Settings::Occlusion().CheckInterval` snapshot updates (at
 * least 1) have passed; every snapshot in between reuses the cached result.  Entries are
 * created only while occlusion culling is enabled, and every snapshot prunes the map down to
 * the actors it just saw.  Game-thread state, held in `GetOcclusionCache()`.
 */
struct OcclusionCacheEntry
{
    /// SnapshotState::frame (game thread - not RendererState::frame) at the last
    /// line-of-sight check.  0 means never checked, and forces a fresh check.
    uint32_t lastCheckFrame{0};
    bool cachedOccluded{false};  ///< Result of that check; reused until the interval expires
};

/**
 * @struct RendererState
 * @brief Aggregates the renderer's mutable state behind GetState().
 * @ingroup Renderer
 *
 * Members are render-thread-only unless noted; game-thread writes go through `snapshotLock`
 * or through atomics.  Three further mutable stores are declared in this header, each behind
 * its own accessor: `GetOcclusionCache()` (game thread), `OverlapOffsets()` (render thread)
 * and `GetSnapshotState()` (game thread).  RendererSnapshot.cpp adds one more, a file-local
 * honorific cache behind `GetHonorificRuntime()`, also game thread.
 */
struct RendererState
{
    // ---- Cache ----
    std::unordered_map<uint32_t, ActorCache> cache;  ///< Per-actor animation cache, keyed by formID
    std::unordered_map<uint32_t, ActorDrawData>
        lastDrawData;    ///< Last live draw data; the death fade and exit animation replay it
    uint32_t frame = 0;  ///< Render-thread frame counter

    // ---- Snapshot and thread safety ----
    std::vector<ActorDrawData> snapshot;  ///< Current actor draw data; guarded by snapshotLock
    /// FormID of the aimed reference, guarded by snapshotLock.  The game thread resolves it
    /// only when moreHUD level-yield or Deck is enabled, so it is 0 otherwise.
    uint32_t crosshairTarget = 0;
    std::mutex snapshotLock;                        ///< Publishes snapshot and target together
    std::atomic<bool> updateQueued{false};          ///< True while a game-thread update is pending
    std::atomic<bool> pauseSnapshotUpdates{false};  ///< Suppress updates during reload
    /// RaceMenu rename: the next drawn frame drops the player cache entry so the new name
    /// re-reveals through the typewriter.  The player plate skips the entrance, so only the
    /// reveal replays.
    std::atomic<bool> pendingIdentityRefresh{false};
    std::atomic<bool> snapshotUpdateRunning{false};         ///< Game-thread update is active
    std::atomic<bool> clearOcclusionCacheRequested{false};  ///< Request to clear occlusion cache

    // ---- Frame state ----
    bool wasInInvalidState = true;  ///< True if previous frame was in an invalid game state
    int postLoadCooldown = 0;       ///< Frames remaining in post-load cooldown

    // ---- Overlay wake - staggered re-entry ----
    /// Set when the overlay resumes after suppression (combat, menus, loads).  The next
    /// drawn frame re-arms every visible plate's entrance and typewriter reveal.  The
    /// entrance block then hands out stagger slots in snapshot order, not by distance.
    bool wakeReplayPending = false;
    /// Entrances that began this frame; each new start claims the next stagger slot.  Reset
    /// at the top of every drawn frame.
    int entrancesStartedThisFrame = 0;

    // ---- Camera-motion quieting ----
    // Smoothed quiet factors in [0,1] driven by camera angular speed: 0 = settled, 1 = fully
    // quiet.  The envelope is asymmetric (Quiet().AttackTime rising, the per-row release time
    // falling), so a pan quiets fast and resolves back slowly.
    RE::NiPoint3 prevCamForward{};  ///< Camera forward on the previous frame
    bool prevCamValid = false;      ///< prevCamForward holds a real sample
    /// Name-row quiet factor.  Advanced every frame, but no draw path reads it yet; it exists
    /// so the name row can opt into thinning (Quiet().NameFloor is likewise unreferenced).
    float quietName = .0f;
    /// Sub-line quiet factor, and the only one consumed.  A live or exiting plate scales the
    /// title row and the badge strip by 1 - quietSub; the dying plate scales the info row and
    /// the badge strip.  Name, level and particles stay at full alpha on every path.
    float quietSub = .0f;

    // ---- Context-conditional setting profiles ----
    // The game thread publishes the active register index each snapshot; the render thread
    // eases its effective values toward that register's values (or the 1/1/1/0 base state)
    // so transitions ramp instead of snapping.
    std::atomic<int> activeRegister{-1};  ///< Index into snapshot registers, -1 = none
    float regAlphaMul = 1.0f;             ///< Smoothed overlay-wide alpha multiplier
    /// Smoothed alpha-fade distance multiplier.  Scales `fadeStartDistance` and
    /// `fadeEndDistance` only; the font-scale distances are not affected.
    float regFadeMul = 1.0f;
    float regSubLineMul = 1.0f;  ///< Smoothed sub-line alpha multiplier
    float regHideNeutral = .0f;  ///< Smoothed neutral/ally plate hide factor

    /// True while this frame's draws are depth-clipped (depth SRV located, polarity
    /// resolved).  Render-thread only.
    bool depthClipFrame = false;

    // ---- Debug stats ----
    DebugOverlay::Stats debugStats;   ///< Debug overlay performance metrics
    float lastDebugUpdateTime = .0f;  ///< Time of last debug stats refresh
    int updateCounter = 0;            ///< Total snapshot updates since startup
    int lastUpdateCount = 0;          ///< Update counter at last debug refresh

    // ---- Hot reload ----
    bool reloadKeyWasDown = false;             ///< Previous frame's reload key state
    float lastReloadTime = -10.0f;             ///< Time of last settings reload
    std::atomic<bool> reloadRequested{false};  ///< Render thread requests reload
    std::atomic<bool> reloadCompleted{false};  ///< Set by game thread when async Load() finishes

    // ---- Overlay gates ----
    // allowOverlay and allowDeck are published by UpdateSnapshot_GameThread.
    std::atomic<bool> allowOverlay{false};  ///< True when game state allows overlay
    std::atomic<bool> allowDeck{false};     ///< World is safe for an explicit card capture
    /// User master switch.  Written only by ToggleEnabled() and SetEnabled(), which are
    /// reachable from the console command handler only.
    std::atomic<bool> manualEnabled{true};

    /// Cached settings snapshot (re-captured only when Settings::Generation() changes).
    RenderSettingsSnapshot cachedSnap;
    uint32_t lastSnapGeneration = 0;  ///< Generation counter at last snapshot capture
};

/**
 * @struct RenderSeg
 * @brief One formatted text segment on the main or info nameplate line.
 * @ingroup Renderer
 */
struct RenderSeg
{
    std::string text;              ///< Formatted text to display
    std::string displayText;       ///< Text after typewriter truncation
    bool isLevel = false;          ///< Whether to use level font
    bool containsName = false;     ///< Whether the source format contains %n
    ImFont* font = nullptr;        ///< Font to use for rendering
    float fontSize = .0f;          ///< Scaled (and potentially fitted) font size
    float horizontalScale = 1.0f;  ///< Name-only width condensation
    ImVec2 size{};                 ///< Measured size after horizontal fitting
    ImVec2 displaySize{};          ///< Fitted size of displayText
};

/**
 * @brief Scale newly emitted vertices horizontally around a text anchor.
 *
 * ImGui has no horizontal scale for one draw call. This operation keeps a fitted name fill,
 * outline, glow, shadow, and animated effect coincident. A null draw list or a scale of
 * 0.9999 or more causes no change. A negative first vertex is clamped to zero.
 *
 * @param drawList         Draw list that owns the vertices.
 * @param firstVertex      First vertex to scale.
 * @param anchorX          Horizontal scaling origin, in screen pixels.
 * @param horizontalScale  Horizontal scale factor.
 */
inline void ScaleNewVerticesX(ImDrawList* drawList,
                              int firstVertex,
                              float anchorX,
                              float horizontalScale)
{
    if (!drawList || horizontalScale >= .9999f)
    {
        return;
    }

    const int vertexEnd = drawList->VtxBuffer.Size;
    firstVertex = std::max(firstVertex, 0);
    for (int i = firstVertex; i < vertexEnd; ++i)
    {
        float& x = drawList->VtxBuffer[i].pos.x;
        x = anchorX + (x - anchorX) * horizontalScale;
    }
}

/**
 * @struct BadgeDrawItem
 * @brief One resolved status badge icon, fully positioned during layout.
 * @ingroup Renderer
 *
 * Color carries no alpha: DrawBadges packs it with the per-frame style alpha (plus the
 * Deadly pulse) at draw time.
 */
struct BadgeDrawItem
{
    ImTextureID tex = 0;     ///< Rasterized duotone icon SRV (from BadgeTextures)
    Settings::Color3 color;  ///< Semantic badge tint (ignored when fullColor)
    ImVec2 pos;              ///< Top-left draw position (screen pixels)
    ImVec2 size;             ///< Draw size (square)
    bool pulse;              ///< Deadly skull breathing alpha
    bool muted = false;      ///< Neutral/inactive slot - dimmed + desaturated at draw time
    bool fullColor = false;  ///< True for the emblem tier badge: draw untinted (white multiply)
    bool isRank = false;     ///< Rank identity mark rather than an actor-status indicator
};

/**
 * @struct ActorLabelContext
 * @brief Resolved actor-fact values passed to FormatString for one expansion.
 * @ingroup Renderer
 *
 * The views have two different owners.  `relationship`, `levelDelta` and `creatureKind`
 * point into `RenderSettingsSnapshot::labels`, which is stable until `Settings::Generation()`
 * advances.  `name` points into the caller's `ActorDrawData`, which lives in the render
 * thread's frame-local copy of the snapshot, or at a static " " literal when the actor name
 * is empty.  The whole context is therefore valid only inside the frame that built it.
 */
struct ActorLabelContext
{
    std::string_view name;          ///< Actor display name (%n)
    int level = 0;                  ///< Actor level (%l)
    const char* title = nullptr;    ///< Tier or special title (%t); nullable
    std::string_view relationship;  ///< Resolved %r label
    std::string_view levelDelta;    ///< Resolved %d label
    std::string_view creatureKind;  ///< Resolved %c label
    std::uint32_t formID = 0;       ///< For deterministic per-actor effects in future tokens
};

/**
 * @struct LabelStyle
 * @brief All color, tier, and effect data computed once per label.
 * @ingroup Renderer
 *
 * Colors resolve once per label into per-role values (name, level, title) plus per-role text
 * effects.  The player uses the tier palette exactly as authored in the INI.  NPC names stay
 * white while their title and level reuse the matched tier's level colors, and the NPC
 * colors supply the relationship and support tints.  Draw code applies the resolved style
 * uniformly and has no per-actor color branches.
 */
struct LabelStyle
{
    int tierIdx;                           ///< Index into snap.tiers; 0 when no tiers configured
    const Settings::TierDefinition* tier;  ///< Matched tier, or GetFallbackTier(); never null
    const Settings::SpecialTitleDefinition* specialTitle;  ///< Matched special title, or null

    // ---- Resolved per-role text effects (tier effects, or None for NPCs) ----
    const Settings::EffectParams* nameEffect;
    const Settings::EffectParams* levelEffect;
    const Settings::EffectParams* titleEffect;
    bool usesTierVisuals;  ///< Player / special title: tier effects, wave, shine overlay

    ImU32 colL, colR;            ///< Name gradient (packed)
    ImU32 colLTitle, colRTitle;  ///< Title gradient
    ImU32 colLLevel, colRLevel;  ///< Level gradient
    ImU32 highlight;             ///< Shimmer / sparkle highlight

    ImVec4 LcName, RcName;    ///< Resolved name color (float, for glow).
    ImVec4 LcTitle, RcTitle;  ///< Resolved title color (float, for glow).
    ImVec4 LcLevel, RcLevel;  ///< Resolved level color (float, for glow).
    ImVec4 supportName;       ///< Representative tint for name support layers.
    ImVec4 supportTitle;      ///< Representative tint for title support layers.
    ImVec4 supportLevel;      ///< Representative tint for level support layers.
    ImVec4 specialGlowColor;  ///< Special title glow color.

    float alpha;        ///< Plate alpha for the main line, after distance fade and dimming
    float titleAlpha;   ///< alpha * Visual().TitleAlphaMultiplier
    float levelAlpha;   ///< alpha * Visual().LevelAlphaMultiplier
    float effectAlpha;  ///< Alpha for the effect layers: alpha scaled by the tier level band
    float strength;     ///< Effect strength multiplier from tier intensity and level position
    float phase01;      ///< Animation phase in [0,1]; seeded per formID so plates desynchronize

    // Tier effect gates.  Each is true when tier gating is off, or when tierIdx reaches the
    // matching Visual() minimum tier.
    bool tierAllowsGlow;
    bool tierAllowsParticles;
    bool tierAllowsOrnaments;
    bool outlineGlowAllowed = true;  ///< False for restrained Graffito world typography

    float nameOutlineWidth;   ///< Outline width for the name row, in pixels
    float levelOutlineWidth;  ///< Outline width for the level row, in pixels
    float titleOutlineWidth;  ///< Outline width for the title row, in pixels
    float outlineWidth;       ///< Primary outline width (a copy of nameOutlineWidth)

    // Stored for deferred outline width computation (see CalcOutlineWidth).
    float baseOutlineWidth;  ///< OutlineWidthMin + OutlineWidthMax, before per-row scaling
    float distToPlayer;      ///< Distance to the player in game units (~70 per metre)

    /// Extra alpha multiplier for the info row and only the info row, so it can be hidden on
    /// ambient actors and faded in on focus changes.  1.0 = no effect, 0.0 = fully hidden.
    float infoAlphaMul = 1.0f;

    /// Extra alpha multiplier for the status badge strip.  Camera-motion quieting folds the
    /// strip away with the title during fast pans; the name and level stay at full alpha.
    float badgeAlphaMul = 1.0f;

    /**
     * @brief Scale the base outline width for one row's font size.
     *
     * The font-size ratio is floored at `RenderConstants::OUTLINE_MIN_SCALE`, so a very
     * small row keeps a visible outline.  Applies the optional distance ramp between
     * `fadeStartDistance` and `fadeEndDistance` when `Visual().EnableDistanceOutlineScale`
     * is set.  The ramp uses the unscaled snapshot distances, so the active register's fade
     * multiplier does not move it.
     *
     * @param fontSize  Scaled font size of the row, in pixels.
     * @param snap      Active render settings snapshot.
     * @return Outline width in pixels.
     */
    float CalcOutlineWidth(float fontSize, const RenderSettingsSnapshot& snap) const
    {
        float ratio = std::max(fontSize / snap.nameFontSize, RenderConstants::OUTLINE_MIN_SCALE);
        float w = baseOutlineWidth * ratio;
        if (snap.visual.EnableDistanceOutlineScale)
        {
            float distT = TextEffects::Saturate((distToPlayer - snap.fadeStartDistance) /
                                                (snap.fadeEndDistance - snap.fadeStartDistance));
            float distMul =
                snap.visual.OutlineDistanceMin +
                (snap.visual.OutlineDistanceMax - snap.visual.OutlineDistanceMin) * distT;
            w *= distMul;
        }
        return w;
    }
};

/**
 * @struct LabelLayout
 * @brief Text measurement, font selection, and position data for one label.
 * @ingroup Renderer
 *
 * Coordinates are in screen pixels (Y increases downward).  `startPos` is the plate anchor
 * and every other position derives from it.  It is seeded from `ActorCache::smooth` (the
 * smoothed projection, not a raw `WorldToScreen` result), then shifted in X by the configured
 * horizontal optical-centering correction scaled by the plate's text scale, then shifted in Y
 * by this actor's `OverlapOffsets()` entry when `Visual().EnableOverlapPrevention` is set.
 *
 * Two conventions coexist: the row offsets (titleY, mainLineY, infoLineY) are relative to
 * `startPos`, while the nameplate bounds (nameplateTop, nameplateLeft, nameplateCenter, ...)
 * are absolute screen pixels.
 */
struct LabelLayout
{
    ImFont* fontName;       ///< Name font pointer
    ImFont* fontLevel;      ///< Level font pointer
    ImFont* fontTitle;      ///< Title font pointer
    float nameFontSize;     ///< Scaled name font size in pixels
    float levelFontSize;    ///< Scaled level font size in pixels
    float titleFontSize;    ///< Scaled title font size in pixels
    bool isPlayer = false;  ///< Copied from ActorDrawData; gates player-only embellishments

    std::vector<RenderSeg> segments;  ///< Main line segments (built from DisplayFormat)
    float mainLineWidth;              ///< Total width of all segments plus padding
    float mainLineHeight;             ///< Height of the tallest segment
    float segmentPadding;             ///< Horizontal padding between segments

    std::vector<RenderSeg> infoSegments;  ///< Info row segments (built from InfoFormat)
    float infoLineWidth = .0f;            ///< Total width of all info segments plus padding
    float infoLineHeight = .0f;           ///< Height of the tallest info segment

    std::vector<BadgeDrawItem> badges;  ///< Status icon badges (built from snap.icons)

    // Full-color rank emblem, on its own row above the icon strip and larger than the icons.
    // Screen-space plates add bloom; Graffito keeps it crisp.
    bool tierEmblemShown = false;
    ImTextureID tierEmblemTex = 0;
    ImVec2 tierEmblemPos{};   ///< Top-left draw position (screen pixels)
    ImVec2 tierEmblemSize{};  ///< Draw size (square)

    std::string titleStr;         ///< Full title text
    std::string titleDisplayStr;  ///< Title text after typewriter truncation
    ImVec2 titleSize;             ///< Measured size of titleDisplayStr

    ImVec2 startPos;  ///< Plate anchor in screen pixels (see the struct doc)
    // The three row offsets below are relative to startPos.y, not absolute screen Y: a draw
    // site adds startPos.y.  mainLineY and titleY are negative (rows sit above the anchor)
    // and infoLineY is positive.
    float titleY;           ///< Y offset of the title line top
    float mainLineY;        ///< Y offset of the main line top
    float infoLineY = .0f;  ///< Y offset of the info row top; 0 when infoSegments is empty
    float totalWidth;       ///< Width of the widest of title/main/info line

    ImVec2 nameplateCenter;  ///< Center of the nameplate bounding box
    float nameplateTop;      ///< Top edge of nameplate (screen Y)
    float nameplateBottom;   ///< Bottom edge of nameplate (screen Y)
    float nameplateLeft;     ///< Left edge of nameplate (screen X)
    float nameplateRight;    ///< Right edge of nameplate (screen X)
    float nameplateWidth;    ///< Total nameplate width in pixels
    float nameplateHeight;   ///< Total nameplate height in pixels
    float mainLineCenterY;   ///< Vertical center of main line (for ornament anchoring)

    /// Text-only bounds, captured before badges and emblems expand the nameplate* fields.
    /// Stored relative to startPos, so later world anchoring and animation move them with no
    /// extra bookkeeping.
    ImVec2 textBoundsMin{};
    ImVec2 textBoundsMax{};
};

/**
 * @struct DistanceFactors
 * @brief Distance-based fade, LOD, and scale factors for one label.
 * @ingroup Renderer
 */
struct DistanceFactors
{
    float alphaTarget;       ///< Target alpha from distance fade
    float textScaleTarget;   ///< Target font scale from distance
    float lodTitleFactor;    ///< LOD multiplier for title visibility [0,1]
    float lodEffectsFactor;  ///< LOD multiplier for particles/ornaments [0,1]
};

// ============================================================================
// Snapshot state (game-thread only)
// ============================================================================

/**
 * @struct SnapshotState
 * @brief Mutable state used only while collecting a snapshot on the game thread.
 * @ingroup Renderer
 *
 * Held behind `GetSnapshotState()` rather than at namespace scope, per the CONTRIBUTING rule
 * "avoid non-trivial global state".
 */
struct SnapshotState
{
    /// Game-thread snapshot counter, incremented once per published snapshot.  This is the
    /// counter OcclusionCacheEntry::lastCheckFrame stores, not the render-thread
    /// RendererState::frame.  Reset to 0 when the occlusion cache is cleared.
    uint32_t frame = 0;
    RE::BGSKeyword* npcKeyword = nullptr;    ///< Cached ActorTypeNPC keyword for creature filtering
    bool npcKeywordLookupAttempted = false;  ///< True after first keyword lookup attempt
    bool npcKeywordMissingLogged = false;    ///< True after logging keyword-not-found warning
};
/// @brief Return the game-thread snapshot-state singleton.
SnapshotState& GetSnapshotState();

/**
 * @brief Alias of the reload-notification duration in the Renderer namespace.
 *
 * No renderer translation unit reads this value. `DebugOverlay` uses the value from
 * `RenderConstants` directly.
 */
inline constexpr float RELOAD_NOTIFICATION_DURATION = RenderConstants::RELOAD_NOTIFICATION_DURATION;

// ============================================================================
// Accessors
// ============================================================================

/// @brief Return the renderer-state singleton.
RendererState& GetState();

/// @brief Return the game-thread per-actor occlusion cache.
std::unordered_map<uint32_t, OcclusionCacheEntry>& GetOcclusionCache();

/// @brief Return render-thread overlap offsets keyed by FormID.
std::unordered_map<uint32_t, float>& OverlapOffsets();

// ============================================================================
// Shared helpers
// ============================================================================

/**
 * @brief Project a world position to screen coordinates.
 *
 * Render-thread helper.  It reads `RE::Main::WorldRootCamera()` and
 * `RE::BSGraphics::Renderer::GetSingleton()` directly, which makes it one of the few
 * render-thread functions that touches `RE::*` state.  Both reads are null-guarded, so the
 * function fails closed.
 *
 * @param worldPos  World-space position.
 * @param[out] screenPos  Projection result: x and y in screen pixels with y increasing
 *        downward, z as viewport depth in [0,1].  Left untouched when the function returns
 *        false.
 * @param[out] cameraPosOut  Optional camera world position.  It is written as soon as the
 *        world camera resolves, so it can be filled even on a false return.
 * @return true when the point could be projected.  False when the world camera is missing,
 *         when `RE::NiCamera::WorldPtToScreenPt3` rejects the point (behind or on the camera
 *         plane), or when the BSGraphics renderer is missing.  A true result is not
 *         bounds-checked: the caller must reject z outside [0,1] and x/y outside the viewport.
 */
bool WorldToScreen(const RE::NiPoint3& worldPos,
                   RE::NiPoint3& screenPos,
                   RE::NiPoint3* cameraPosOut = nullptr);

// ============================================================================
// Snapshot functions (RendererSnapshot.cpp)
// ============================================================================

/**
 * @brief Convert ASCII words to title case and preserve UTF-8 byte sequences.
 *
 * Multi-byte codepoints remain unchanged. A character after a multi-byte codepoint does not
 * start a new word. The function trims leading and trailing spaces, tabs, carriage returns,
 * and line feeds.
 *
 * @param text  Null-terminated UTF-8 text. The pointer can be null.
 * @return      The converted text, or an empty string for null, empty, or whitespace-only
 *              input.
 */
std::string Capitalize(const char* text);

/**
 * @brief Collect and publish visible actor data on the game thread.
 *
 * Two passes: a cheap distance filter over every high-process actor, then full derivation
 * (name, relationship, badges, honorific, occlusion) for the kept plates only.  The kept set
 * is ordered by distance, except that the Graffito camera-ray target sorts first and the
 * Deck crosshair target sorts second when it needs the extra private slot.  The player
 * entry, when one is published, precedes that whole set.
 *
 * Also publishes the `allowOverlay` and `allowDeck` gate atomics - the overlay cannot
 * bootstrap until this function runs - plus `crosshairTarget` under the same lock and
 * `activeRegister`.  It services `clearOcclusionCacheRequested` and prunes the occlusion and
 * honorific per-actor caches down to the actors it just saw. When `pauseSnapshotUpdates` is
 * set, or when the game state allows neither the overlay nor a Deck capture, it clears the
 * gates and the snapshot, then returns without scanning.
 */
void UpdateSnapshot_GameThread();

/**
 * @brief Schedule a game-thread snapshot update.
 *
 * The render thread calls this function through the SKSE task interface. Requests coalesce
 * through `updateQueued`, so at most one game-thread task is active. The function clears the
 * flag when `UpdateSnapshot_GameThread` finishes. It does nothing while
 * `pauseSnapshotUpdates` is set. If the task interface is unavailable, it clears the queued
 * flag so a later frame can retry.
 */
void QueueSnapshotUpdate_RenderThread();

// ============================================================================
// Focus selection (Renderer.cpp)
// ============================================================================

/**
 * @brief Select the actor nearest the center of the configured focus cone.
 *
 * This render-thread function reads the camera pose through `Occlusion::GetCameraInfo`. It
 * returns zero when focus is disabled, the snapshot is empty, the camera pose is unavailable,
 * or no actor qualifies.
 *
 * The function skips the player, dead actors, Deck-only entries, and disallowed occluded
 * actors. It also skips actors closer than one world unit to the camera or farther than the
 * configured maximum. A maximum of zero or less applies no additional distance limit. The
 * cone and distance tests use the camera position. The tie-break uses player distance.
 *
 * Ties are resolved by the higher dot product, then the smaller player distance, then the
 * lower FormID.
 *
 * @param snap          Actor snapshot to search.
 * @param snapSettings  Render settings that contain the focus configuration.
 * @return              The selected actor FormID, or zero when no actor qualifies.
 */
uint32_t SelectFocusedActor(const std::vector<ActorDrawData>& snap,
                            const RenderSettingsSnapshot& snapSettings);

// ============================================================================
// Layout functions (RendererLayout.cpp)
// ============================================================================

/**
 * @brief Measure the tightest vertical glyph bounds for text.
 *
 * @param font       Font used for measurement.
 * @param fontSize   Font size, in pixels.
 * @param text       Null-terminated UTF-8 text.
 * @param outTop     Receives the topmost glyph offset, in pixels.
 * @param outBottom  Receives the bottommost glyph offset, in pixels.
 */
void CalcTightYBoundsFromTop(
    ImFont* font, float fontSize, const char* text, float& outTop, float& outBottom);

/**
 * @brief Expand nameplate placeholders in one format string.
 *
 * The function replaces `%n`, `%l`, `%t`, `%r`, `%d`, and `%c` with values from the supplied
 * `ActorLabelContext`. Replacement is single-pass, so placeholders
 * embedded in substitution values are not expanded.
 *
 * `%t` expands only when `ctx.title` is non-null; otherwise the literal text `%t` stays in
 * the output.  Every other token always substitutes.  An unrecognised `%` sequence and a
 * trailing `%` are copied through unchanged.
 *
 * @param fmt  Format string to expand.
 * @param ctx  Actor values used for substitution.
 * @return     The expanded string.
 */
std::string FormatString(const std::string& fmt, const ActorLabelContext& ctx);

/**
 * @brief Return the fallback tier definition.
 *
 * The function initializes the value on the first call. Callers use it when no tiers are
 * configured.
 *
 * @return The shared fallback tier definition.
 */
const Settings::TierDefinition& GetFallbackTier();

/**
 * @brief Compose the ordered status-badge set for one actor.
 *
 * Slot order is rank, relationship, creature, role, protection, threat, engagement for an
 * NPC (7 slots, the MAX_BADGE_SLOTS budget) and rank, sneak, engagement, encumbered, bounty
 * for the player (5).  Every enabled slot is emitted: a resting fact is marked muted rather
 * than dropped. Returns an empty set when `IconTokens::enabled` is false.
 *
 * @param d            Actor facts to classify.
 * @param cfg          Resolved icon configuration.
 * @param tierIdx      Matched tier index used for the rank band.
 * @param tierCount    Configured tier count. One or less selects the lowest rank band.
 * @param includeRank  Whether to include the rank slot.
 * @return             The ordered fixed-capacity badge set.
 */
BadgeComposition ComposeBadges(const ActorDrawData& d,
                               const RenderSettingsSnapshot::IconTokens& cfg,
                               int tierIdx,
                               int tierCount,
                               bool includeRank = true);

/**
 * @brief Compute all resolved style data for one actor label.
 *
 * The result includes tier matching, color adjustment, effect gating, and animation phase.
 *
 * @param d          Actor facts.
 * @param nameLower  Lowercase actor name for special-title matching.
 * @param alpha      Current plate alpha.
 * @param time       Animation clock, in seconds.
 * @param snap       Immutable render settings.
 * @return           The resolved label style.
 */
LabelStyle ComputeLabelStyle(const ActorDrawData& d,
                             const std::string& nameLower,
                             float alpha,
                             float time,
                             const RenderSettingsSnapshot& snap);

/**
 * @brief Measure text and compute all positions for one actor label.
 *
 * The function builds segments, applies typewriter truncation, measures the title and main
 * line, and computes the nameplate bounding box.
 * Returns a default-constructed layout when any of the three plate fonts is missing, which
 * happens before the font atlas is built.  A null `fontName` in the result therefore means
 * there is nothing to draw, and the rest of the layout carries no meaning.
 *
 * @param d                  Actor facts.
 * @param entry              Mutable per-actor animation cache.
 * @param style              Resolved label style.
 * @param textSizeScale      Distance-based text scale.
 * @param snap               Immutable render settings.
 * @param forcedCharsToShow  Character budget override. A negative value uses the cache-driven
 *                           typewriter state.
 * @return                   The measured label layout.
 */
LabelLayout ComputeLabelLayout(const ActorDrawData& d,
                               ActorCache& entry,
                               const LabelStyle& style,
                               float textSizeScale,
                               const RenderSettingsSnapshot& snap,
                               int forcedCharsToShow = -1);

/**
 * @brief Apply one stage of the death-rite color transition.
 *
 * The function moves each resolved style color toward `target` by `mixT`, reduces effect
 * strength, and repacks draw-ready colors. Repeated calls can create a multi-stop ramp.
 *
 * @param style   Label style to modify.
 * @param target  Target transition color.
 * @param mixT    Blend factor.
 * @param snap    Immutable render settings.
 */
void ApplyDeathRiteTint(LabelStyle& style,
                        const ImVec4& target,
                        float mixT,
                        const RenderSettingsSnapshot& snap);

/**
 * @brief Adapt resolved text colors to the sampled scene.
 *
 * The function dims text over bright backgrounds and lifts and warms it over dark backgrounds.
 * It then repacks the draw-ready colors.
 * `bgLum` and `bgRGB` are the smoothed per-actor scene sample; adjustments are capped by
 * `snap.candleStrength`.  Pure color math, mirrored in tests/test_utils.cpp (CandleAdjust);
 * keep the mapping in sync.
 *
 * @param style  Label style to modify.
 * @param bgLum  Smoothed background luminance.
 * @param bgRGB  Smoothed background RGB channels.
 * @param snap   Immutable render settings.
 */
void ApplyCandlelight(LabelStyle& style,
                      float bgLum,
                      const float bgRGB[3],
                      const RenderSettingsSnapshot& snap);

// ============================================================================
// Effects functions (RendererEffects.cpp)
// ============================================================================

/**
 * @brief Resolve an optional positive effect parameter.
 *
 * An unconfigured effect parameter is zero and selects the fallback.
 *
 * @param p         Configured effect parameter.
 * @param fallback  Value to use when `p` is not positive.
 * @return          `p` when positive, or `fallback` otherwise.
 */
constexpr float ParamOr(float p, float fallback)
{
    return p > .0f ? p : fallback;
}

/**
 * @brief Select and apply one configured text effect.
 *
 * @param drawList      Target draw list.
 * @param font          Font for the text.
 * @param fontSize      Scaled font size in pixels.
 * @param pos           Screen-space draw position.
 * @param text          Null-terminated UTF-8 string.
 * @param effect        Effect parameters (type, speed, etc.).
 * @param colL          Left gradient color (packed).
 * @param colR          Right gradient color (packed).
 * @param highlight     Shimmer/sparkle highlight color (packed).
 * @param outlineColor  Outline color (packed, alpha-scaled).
 * @param outlineWidth  Outline thickness in pixels.
 * @param phase01       Animation phase in [0,1].
 * @param strength      Effect strength multiplier.
 * @param textSizeScale Distance-based text scale factor.
 * @param alpha         Overall alpha multiplier.
 * @param fastOutlines  Use 4-dir outlines instead of 8-dir.
 * @param outlineGlow   Optional outline-glow configuration.
 * @param dualOutline   Optional dual-outline configuration.
 * @param wave          Optional wave-displacement configuration.
 * @param shine         Optional top-edge shine configuration.
 */
void ApplyTextEffect(ImDrawList* drawList,
                     ImFont* font,
                     float fontSize,
                     ImVec2 pos,
                     const char* text,
                     const Settings::EffectParams& effect,
                     ImU32 colL,
                     ImU32 colR,
                     ImU32 highlight,
                     ImU32 outlineColor,
                     float outlineWidth,
                     float phase01,
                     float strength,
                     float textSizeScale,
                     float alpha,
                     bool fastOutlines,
                     const TextEffects::OutlineGlowParams* outlineGlow = nullptr,
                     const TextEffects::DualOutlineParams* dualOutline = nullptr,
                     const TextEffects::WaveParams* wave = nullptr,
                     const TextEffects::ShineParams* shine = nullptr);

/**
 * @brief Draw the soft halo behind a complete nameplate.
 *
 * The function uses the glow layer, so the halo remains behind particles, outlines, and text.
 *
 * @param dl              ImGui draw list.
 * @param style           Resolved label style.
 * @param layout          Resolved label layout.
 * @param lodTitleFactor  Distance-based title visibility factor.
 * @param splitter        Draw-list channel splitter.
 * @param snap            Immutable render settings.
 */
void DrawBackgroundGlow(ImDrawList* dl,
                        const LabelStyle& style,
                        const LabelLayout& layout,
                        float lodTitleFactor,
                        ImDrawListSplitter* splitter,
                        const RenderSettingsSnapshot& snap);

/**
 * @brief Draw the particle aura and ornament glyphs for one nameplate.
 *
 * The function computes the shared ornament geometry once, so the aura sizing and ornament
 * draw pass use the same result. The effect applies only to the player unless a special title
 * forces it.
 *
 * @param dl                ImGui draw list.
 * @param d                 Actor facts.
 * @param style             Resolved label style.
 * @param layout            Resolved label layout.
 * @param lodEffectsFactor  Distance-based effect visibility factor.
 * @param time              Animation clock, in seconds.
 * @param splitter          Draw-list channel splitter.
 * @param fastOutlines      Whether to use four-direction outlines.
 * @param snap              Immutable render settings.
 */
void DrawParticlesAndOrnaments(ImDrawList* dl,
                               const ActorDrawData& d,
                               const LabelStyle& style,
                               const LabelLayout& layout,
                               float lodEffectsFactor,
                               float time,
                               ImDrawListSplitter* splitter,
                               bool fastOutlines,
                               const RenderSettingsSnapshot& snap);

/**
 * @brief Draw the title line above the main nameplate line.
 *
 * @param dl              ImGui draw list.
 * @param style           Resolved label style.
 * @param layout          Resolved label layout.
 * @param lodTitleFactor  Distance-based title visibility factor.
 * @param splitter        Draw-list channel splitter.
 * @param fastOutlines    Whether to use four-direction outlines.
 * @param snap            Immutable render settings.
 */
void DrawTitleText(ImDrawList* dl,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodTitleFactor,
                   ImDrawListSplitter* splitter,
                   bool fastOutlines,
                   const RenderSettingsSnapshot& snap);

/**
 * @brief Draw each segment of the main nameplate line.
 *
 * @param dl            ImGui draw list.
 * @param style         Resolved label style.
 * @param layout        Resolved label layout.
 * @param splitter      Draw-list channel splitter.
 * @param fastOutlines  Whether to use four-direction outlines.
 * @param snap          Immutable render settings.
 */
void DrawMainLineSegments(ImDrawList* dl,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          ImDrawListSplitter* splitter,
                          bool fastOutlines,
                          const RenderSettingsSnapshot& snap);

/**
 * @brief Draw the contextual information row below the main line.
 *
 * The function does nothing when no segment remains after drop-if-blank trimming.
 *
 * @param dl            ImGui draw list.
 * @param style         Resolved label style.
 * @param layout        Resolved label layout.
 * @param splitter      Draw-list channel splitter.
 * @param fastOutlines  Whether to use four-direction outlines.
 * @param snap          Immutable render settings.
 */
void DrawInfoLineSegments(ImDrawList* dl,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          ImDrawListSplitter* splitter,
                          bool fastOutlines,
                          const RenderSettingsSnapshot& snap);

/**
 * @brief Draw resolved status-badge textures around the main line.
 *
 * The function does nothing when layout
 * produced no badge items; icons whose `BadgeTextures` lookup failed are already dropped
 * during `ComputeLabelLayout`, so no font or texture check happens here.
 *
 * `restrainedWorld` is set for Graffito world planes: it suppresses the player lighting
 * block, the player rim light, and the badge glow geometry, and leaves only the crisp icon
 * quads.
 *
 * @param dl                ImGui draw list.
 * @param style             Resolved label style.
 * @param layout            Resolved label layout.
 * @param splitter          Draw-list channel splitter.
 * @param fastOutlines      Whether to use four-direction outlines.
 * @param snap              Immutable render settings.
 * @param restrainedWorld  Whether to suppress screen-space badge lighting.
 */
void DrawBadges(ImDrawList* dl,
                const LabelStyle& style,
                const LabelLayout& layout,
                ImDrawListSplitter* splitter,
                bool fastOutlines,
                const RenderSettingsSnapshot& snap,
                bool restrainedWorld = false);

/**
 * @brief Draw the rank emblem above the status-icon strip.
 *
 * Screen-space plates receive a soft bloom. Restrained world planes draw only the crisp
 * emblem.
 *
 * @param dl                ImGui draw list.
 * @param style             Resolved label style.
 * @param layout            Resolved label layout.
 * @param time              Animation clock, in seconds.
 * @param splitter          Draw-list channel splitter.
 * @param snap              Immutable render settings.
 * @param restrainedWorld  Whether to suppress screen-space emblem lighting.
 */
void DrawTierEmblem(ImDrawList* dl,
                    const LabelStyle& style,
                    const LabelLayout& layout,
                    float time,
                    ImDrawListSplitter* splitter,
                    const RenderSettingsSnapshot& snap,
                    bool restrainedWorld = false);

// ============================================================================
// Coordinator functions (Renderer.cpp)
// ============================================================================

/**
 * @brief Get a loaded ImGui font by index.
 *
 * @param index  Font index.
 * @return       The indexed font, or the default font when the index is invalid.
 */
ImFont* GetFontAt(int index);

/**
 * @brief Remove stale actor entries from the render cache.
 *
 * The grace period is `RenderConstants::CACHE_GRACE_FRAMES` (60 frames, about 1 s at 60
 * fps), extended to three times that while an entry is mid-exit (0 < exitPhase < 1) so a
 * dying plate finishes.  The extended ceiling stops a mid-exit entry from leaking if exit
 * animations are switched off.  Removing an entry also erases the actor's `lastDrawData`
 * replay entry.
 *
 * @param snap  Current actor snapshot.
 */
void PruneCacheToSnapshot(const std::vector<ActorDrawData>& snap);

/**
 * @brief Compute a frame-rate-independent exponential smoothing factor.
 *
 * Returns alpha in [0,1] for use as `current = lerp(current, target, alpha)`:
 *
 * $$\alpha = 1 - \epsilon^{\,dt / settleTime}$$
 *
 * @param dt          Frame delta in seconds; a negative value is clamped to 0.
 * @param settleTime  Seconds for the residual to fall to @p epsilon of the initial delta;
 *                    floored at 1e-5, so 0 snaps in one frame.  Passing milliseconds
 *                    saturates alpha at 1.0 and silently disables the smoothing.
 * @param epsilon     Convergence threshold (default 0.01 = settle to 1% of the initial
 *                    delta).
 * @return Smoothing factor in [0,1].
 */
float ExpApproachAlpha(float dt, float settleTime, float epsilon = .01f);

}  // namespace Renderer
