#include "Renderer.h"
#include "TextEffects.h"
#include "Settings.h"
#include "Occlusion.h"
#include "RenderConstants.h"
#include "DebugOverlay.h"
#include "AppearanceTemplate.h"

#include <SKSE/SKSE.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Renderer
{
    // Count UTF-8 characters in string
    static size_t Utf8CharCount(const char *s)
    {
        size_t count = 0;
        if (!s)
            return 0;

        while (*s)
        {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80)
            {
                s++;
            }  // 1-byte (ASCII)
            else if (c < 0xE0)
            {
                s += 2;
            }  // 2-byte
            else if (c < 0xF0)
            {
                s += 3;
            }  // 3-byte
            else
            {
                s += 4;
            }  // 4-byte
            count++;
        }
        return count;
    }

    // Truncate UTF-8 string to maxChars codepoints
    static std::string Utf8Truncate(const char *s, size_t maxChars)
    {
        if (!s || maxChars == 0)
            return "";

        const char *start = s;
        size_t count = 0;

        while (*s && count < maxChars)
        {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80)
            {
                s++;
            }  // 1-byte (ASCII)
            else if (c < 0xE0)
            {
                s += 2;
            }  // 2-byte
            else if (c < 0xF0)
            {
                s += 3;
            }  // 3-byte
            else
            {
                s += 4;
            }  // 4-byte
            count++;
        }

        return std::string(start, s - start);
    }

    // Parse next UTF-8 codepoint, returns pointer to next char
    static const char *Utf8Next(const char *s, unsigned int &out)
    {
        out = 0;
        if (!s || !*s)
            return s;

        const unsigned char c = (unsigned char)s[0];

        // Single-byte ASCII character (0x00-0x7F)
        if (c < 0x80)
        {
            out = c;
            return s + 1;
        }

        // Reject continuation bytes (0x80-0xBF) appearing as start bytes
        // These are invalid in UTF-8 when they start a sequence
        if (c < 0xC0)
        {
            out = 0xFFFD;
            return s + 1;
        }  // 0xFFFD = replacement character (U+FFFD)

        // 2-byte sequence (0xC0-0xDF): 110xxxxx 10xxxxxx
        if (c < 0xE0)
        {
            if (!s[1])
            {
                out = 0xFFFD;
                return s + 1;
            }  // Truncated sequence
            const unsigned char c1 = (unsigned char)s[1];
            if ((c1 & 0xC0) != 0x80)
            {
                out = 0xFFFD;
                return s + 1;
            }  // Invalid continuation byte
            // Decode: take 5 bits from first byte, 6 bits from second
            out = ((c & 0x1F) << 6) | (c1 & 0x3F);
            return s + 2;
        }

        // 3-byte sequence (0xE0-0xEF): 1110xxxx 10xxxxxx 10xxxxxx
        if (c < 0xF0)
        {
            if (!s[1] || !s[2])
            {
                out = 0xFFFD;
                return s + 1;
            }
            const unsigned char c1 = (unsigned char)s[1];
            const unsigned char c2 = (unsigned char)s[2];
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80)
            {
                out = 0xFFFD;
                return s + 1;
            }
            // Decode: 4 bits from first, 6 bits each from second and third
            out = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            return s + 3;
        }

        // 4-byte sequence (0xF0-0xF7): 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        if (c < 0xF8)
        {
            if (!s[1] || !s[2] || !s[3])
            {
                out = 0xFFFD;
                return s + 1;
            }
            const unsigned char c1 = (unsigned char)s[1];
            const unsigned char c2 = (unsigned char)s[2];
            const unsigned char c3 = (unsigned char)s[3];
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
            {
                out = 0xFFFD;
                return s + 1;
            }
            // Decode: 3 bits from first, 6 bits each from second, third, and fourth
            out = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            return s + 4;
        }

        // Invalid UTF-8 sequence
        out = 0xFFFD;  // Replacement character
        return s + 1;
    }

    // Get byte length of UTF-8 character at position
    static size_t Utf8CharLen(const char* s)
    {
        if (!s || !*s) return 0;
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) return 1;
        if (c < 0xE0) return 2;
        if (c < 0xF0) return 3;
        if (c < 0xF8) return 4;
        return 1;  // Invalid, treat as single byte
    }

    // Extract UTF-8 characters from string into a vector of strings
    static std::vector<std::string> Utf8ToChars(const std::string& str)
    {
        std::vector<std::string> chars;
        const char* s = str.c_str();
        while (*s) {
            size_t len = Utf8CharLen(s);
            chars.emplace_back(s, len);
            s += len;
        }
        return chars;
    }

    // Calculate tight vertical bounds of text glyphs
    static void CalcTightYBoundsFromTop(ImFont *font, float fontSize, const char *text, float &outTop, float &outBottom)
    {
        // Initialize to extremes so first glyph will replace them
        outTop = +FLT_MAX;
        outBottom = -FLT_MAX;

        if (!font || !text || !*text)
        {
            outTop = 0.0f;
            outBottom = 0.0f;
            return;
        }

        // Calculate scale factor from font's native size to desired size
        const float scale = fontSize / font->FontSize;

        // Iterate through each character in the UTF-8 string
        for (const char *p = text; *p;)
        {
            unsigned int cp;
            p = Utf8Next(p, cp);

            // Skip newlines (shouldn't be in our text, but be safe)
            if (cp == '\n' || cp == '\r')
                continue;

            // Get glyph data for this character
            const ImFontGlyph *g = font->FindGlyph((ImWchar)cp);
            if (!g)
                continue; // Character not in font

            // Y0 and Y1 are relative to the baseline
            // Y0 is negative (above baseline), Y1 is positive (below baseline)
            // We want the tightest bounds, so min of Y0 (most negative), max of Y1 (most positive)
            outTop = std::min(outTop, g->Y0 * scale);
            outBottom = std::max(outBottom, g->Y1 * scale);
        }

        // If no valid glyphs were found, return zeros
        if (outTop == +FLT_MAX)
        {
            outTop = 0.0f;
            outBottom = 0.0f;
        }
    }

    /// Cache entry for smooth actor nameplate animations.
    /// Stores smoothed values and state for position, alpha, and effects.
    struct ActorCache
    {
        ImVec2 smooth{};                       ///< Smoothed screen position (result of moving average)
        float alphaSmooth = 1.0f;              ///< Smoothed alpha for fade transitions
        float textSizeScale = 1.0f;            ///< Smoothed font scale for distance-based sizing
        float occlusionSmooth = 1.0f;          ///< Smoothed occlusion (1.0=visible, 0.0=hidden)

        bool initialized = false;              ///< True after first frame of data
        uint32_t lastSeenFrame = 0;            ///< Frame counter when actor was last in snapshot

        uint32_t lastOcclusionCheckFrame = 0;  ///< Frame when LOS was last checked
        bool cachedOccluded = false;           ///< Cached LOS result
        bool wasOccluded = false;              ///< Previous frame's occlusion state

        static constexpr int kHistorySize = RenderConstants::kPositionHistorySize;
        ImVec2 posHistory[kHistorySize]{};
        int historyIndex = 0;
        bool historyFilled = false;

        float typewriterTime = 0.0f;      ///< Seconds since actor first appeared
        bool typewriterComplete = false;  ///< True when reveal animation finished

        std::string cachedName;           ///< Last known name (to detect changes)

        /// Add position sample to history and return smoothed average.
        ImVec2 AddAndGetSmoothed(const ImVec2& pos)
        {
            posHistory[historyIndex] = pos;
            historyIndex = (historyIndex + 1) % kHistorySize;
            if (historyIndex == 0) historyFilled = true;

            int count = historyFilled ? kHistorySize : historyIndex;
            if (count == 0) return pos;

            ImVec2 sum{0, 0};
            for (int i = 0; i < count; i++)
            {
                sum.x += posHistory[i].x;
                sum.y += posHistory[i].y;
            }
            return ImVec2(sum.x / count, sum.y / count);
        }
    };

    // Actor disposition
    enum class Disposition : std::uint8_t
    {
        Neutral,      ///< Neutral NPCs (white/gray)
        Enemy,        ///< Hostile NPCs (red)
        AllyOrFriend  ///< Friendly/allied NPCs (blue)
    };

    // Data for rendering a single actor's nameplate
    struct ActorDrawData
    {
        uint32_t formID{0};                       ///< Actor's form ID (unique identifier)
        RE::NiPoint3 worldPos{};                  ///< World position above actor's head
        std::string name;                         ///< Display name (capitalized)
        uint16_t level{0};                        ///< Actor's level
        float distToPlayer{0.0f};                 ///< Distance to player in units
        Disposition dispo{Disposition::Neutral};  ///< Disposition towards player
        bool isPlayer{false};                     ///< Whether this is the player character
        bool isOccluded{false};                   ///< Whether actor is occluded from view
    };

    /// Cache for smooth actor transitions, keyed by form ID
    static std::unordered_map<uint32_t, ActorCache> s_cache;
    /// Current frame counter for cache management
    static uint32_t s_frame = 0;

    /// Snapshot of actor data for rendering (thread-safe)
    static std::vector<ActorDrawData> s_snapshot;
    /// Mutex protecting the snapshot data
    static std::mutex s_snapshotLock;

    /// Flag indicating if an update is currently queued
    static std::atomic<bool> s_updateQueued{false};
    /// Frame counter for throttling updates
    static uint32_t s_updateTicker = 0;

    /// Tracks if we were previously in an invalid state
    static bool s_wasInInvalidState = true;
    /// Cooldown frames after loading before rendering starts
    static int s_postLoadCooldown = 0;

    static DebugOverlay::Stats s_debugStats;
    static float s_lastDebugUpdateTime = 0.0f;
    static int s_updateCounter = 0;
    static int s_lastUpdateCount = 0;

    // Hot reload state
    static bool s_reloadKeyWasDown = false;
    static float s_lastReloadTime = -10.0f;  // Time of last reload (for notification)
    static constexpr float kReloadNotificationDuration = RenderConstants::kReloadNotificationDuration;

    /// Atomic flag for whether overlay is allowed (checked by render thread)
    static std::atomic<bool> s_allowOverlay{false};
    /// Manual toggle flag (can disable rendering via console command)
    static std::atomic<bool> s_manualEnabled{true};

    bool IsOverlayAllowedRT() {
        return s_manualEnabled.load(std::memory_order_acquire) &&
               s_allowOverlay.load(std::memory_order_acquire);
    }

    bool ToggleEnabled() {
        bool newState = !s_manualEnabled.load(std::memory_order_acquire);
        s_manualEnabled.store(newState, std::memory_order_release);
        return newState;
    }

    // Get player character
    static RE::Actor *GetPlayer()
    {
        return RE::PlayerCharacter::GetSingleton();
    }

    // Capitalize text and trim whitespace
    static std::string Capitalize(const char *text)
    {
        if (!text || !*text)
            return "";
        std::string s = text;

        // Trim leading/trailing whitespace
        size_t first = s.find_first_not_of(" \t\r\n");
        if (std::string::npos == first)
            return "";
        size_t last = s.find_last_not_of(" \t\r\n");
        s = s.substr(first, (last - first + 1));

        // Title Case
        bool newWord = true;
        for (auto &c : s)
        {
            if (isspace(static_cast<unsigned char>(c)))
            {
                newWord = true;
            }
            else if (newWord)
            {
                c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
                newWord = false;
            }
        }
        return s;
    }

    // Get actor's fight reaction towards player
    static RE::FIGHT_REACTION GetReactionToPlayer(RE::Actor *a_actor, RE::Actor *a_player)
    {
        if (!a_actor || !a_player)
        {
            return RE::FIGHT_REACTION::kNeutral;
        }
        if (a_actor == a_player)
        {
            return RE::FIGHT_REACTION::kFriend;
        }

        // Hostility
        if (a_actor->IsHostileToActor(a_player) || a_player->IsHostileToActor(a_actor))
        {
            return RE::FIGHT_REACTION::kEnemy;
        }

        // Followers / teammates
        if (a_actor->IsPlayerTeammate())
        {
            return RE::FIGHT_REACTION::kAlly;
        }

        // Optional heuristic: "friendly" if they can do normal player dialogue
        if (a_actor->CanTalkToPlayer())
        {
            return RE::FIGHT_REACTION::kFriend;
        }

        return RE::FIGHT_REACTION::kNeutral;
    }

    // Determine actor's disposition for nameplate color
    static Disposition GetDisposition(RE::Actor *a, RE::Actor *player)
    {
        if (!a || !player)
        {
            return Disposition::Neutral;
        }

        // Hard override: Currently hostile (combat/crime/aggro/etc.)
        if (a->IsHostileToActor(player))
        {
            return Disposition::Enemy;
        }

        // Teammate is always "friendly enough" for UI
        if (a->IsPlayerTeammate())
        {
            return Disposition::AllyOrFriend;
        }

        // Baseline faction/relationship reaction
        switch (GetReactionToPlayer(a, player))
        {
            case RE::FIGHT_REACTION::kEnemy:
                return Disposition::Enemy;

            case RE::FIGHT_REACTION::kAlly:
            case RE::FIGHT_REACTION::kFriend:
                return Disposition::AllyOrFriend;

            case RE::FIGHT_REACTION::kNeutral:
            default:
                return Disposition::Neutral;
        }
    }

    // Project world position to screen coordinates
    bool WorldToScreen(const RE::NiPoint3 &worldPos, RE::NiPoint3 &screenPos, RE::NiPoint3 *cameraPosOut = nullptr)
    {
        // Get the main world camera
        auto *cam = RE::Main::WorldRootCamera();
        if (!cam)
        {
            return false;
        }

        // Get camera runtime data containing transformation matrices
        const auto &rt = cam->GetRuntimeData();    // Contains worldToCam matrix
        const auto &rt2 = cam->GetRuntimeData2();  // Contains viewport info

        // Optionally output camera position (used for distance calculations elsewhere)
        if (cameraPosOut)
        {
            *cameraPosOut = cam->world.translate;
        }

        // Project world coordinates to normalized screen coordinates [0,1]
        float x = 0.0f, y = 0.0f, z = 0.0f;

        // WorldPtToScreenPt3 returns false if point is behind camera or outside frustum
        // The last parameter (1e-5f) is the epsilon for near-plane clipping
        if (!RE::NiCamera::WorldPtToScreenPt3(rt.worldToCam, rt2.port, worldPos, x, y, z, 1e-5f))
        {
            return false;  // Point not visible (behind camera or clipped)
        }

        // Get actual screen resolution
        auto *renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer)
        {
            return false;
        }
        const auto ss = renderer->GetScreenSize();
        const float w = static_cast<float>(ss.width);
        const float h = static_cast<float>(ss.height);

        // Convert normalized coordinates [0,1] to pixel coordinates
        screenPos.x = x * w;           // Left to right: 0 to width
        screenPos.y = (1.0f - y) * h;  // Flip Y: top to bottom (screen space is inverted)
        screenPos.z = z;               // Depth value for Z-testing
        return true;
    }

    static void UpdateSnapshot_GameThread()
    {
        // RAII struct to ensure the update flag is cleared when function exits
        // This guarantees cleanup even if we early-return
        struct ClearFlag
        {
            ~ClearFlag() { s_updateQueued.store(false); }
        } _;

        // Check if we're allowed to draw the overlay (not in menus, loading, etc.)
        const bool allow = CanDrawOverlay();
        s_allowOverlay.store(allow, std::memory_order_release);

        if (!allow)
        {
            // Clear snapshot so rendering doesn't show stale data
            std::lock_guard<std::mutex> lock(s_snapshotLock);
            s_snapshot.clear();
            return;
        }

        // Get player and process lists (contains all active actors)
        auto *player = GetPlayer();
        auto *pl = RE::ProcessLists::GetSingleton();
        if (!player || !pl)
        {
            std::lock_guard<std::mutex> lock(s_snapshotLock);
            s_snapshot.clear();
            return;
        }

        // Limits to prevent performance issues in crowded areas
        constexpr int kMaxActors = RenderConstants::kMaxActors;
        constexpr int kMaxScan = RenderConstants::kMaxScan;
        const float kMaxDistSq = Settings::MaxScanDistance * Settings::MaxScanDistance;

        // Use temporary buffer to collect data without holding the snapshot lock
        // This minimizes lock contention with the render thread
        static std::vector<ActorDrawData> tempBuf;
        tempBuf.clear();
        tempBuf.reserve(kMaxActors);

        const auto playerPos = player->GetPosition();

        // Include the player character first
        if (!Settings::HidePlayer)
        {
            ActorDrawData d;
            d.formID = player->GetFormID();
            d.level = player->GetLevel();

            // Get player's display name
            const char *rawName = player->GetDisplayFullName();
            if (rawName)
            {
                d.name = Capitalize(rawName);  // Title case
            }
            else
            {
                d.name = "Player";  // Fallback if name lookup fails
            }

            // Position the nameplate above the player's head
            d.worldPos = playerPos;
            d.worldPos.z += player->GetHeight() + Settings::VerticalOffset;  // Configurable offset above head

            d.distToPlayer = 0.0f;  // Distance to self is zero
            d.isPlayer = true;      // Flag for special rendering (uses tier effects)
            tempBuf.push_back(std::move(d));
        }

        int added = 1;    // Track how many actors we've added (player counts as 1)
        int scanned = 0;  // Track how many we've iterated (for early exit)

        // Iterate through high-priority actors (NPCs in active processing)
        for (auto &h : pl->highActorHandles)
        {
            // Stop if we've hit our limits
            if (added >= kMaxActors || scanned++ >= kMaxScan)
            {
                break;
            }

            // Resolve the actor handle to a pointer
            auto aSP = h.get();
            auto *a = aSP.get();
            if (!a || a == player)
            {
                continue;  // Skip invalid or player
            }

            // Skip dead actors
            if (a->IsDead())
            {
                continue;
            }

            // Check distance, use squared distance to avoid expensive sqrt
            const float distSq = playerPos.GetSquaredDistance(a->GetPosition());
            if (distSq > kMaxDistSq)
            {
                continue;  // Too far away, skip
            }

            // Create actor data for rendering
            ActorDrawData d;
            d.formID = a->GetFormID();  // Unique ID for caching
            d.level = a->GetLevel();

            // Get NPC's display name
            const char *rawName = a->GetDisplayFullName();
            if (rawName)
            {
                d.name = Capitalize(rawName);  // Title case
            }
            else
            {
                d.name = "";  // Empty name for unnamed NPCs
            }

            // Position above NPC's head
            d.worldPos = a->GetPosition();
            d.worldPos.z += a->GetHeight() + Settings::VerticalOffset;  // Configurable offset above head

            // Calculate actual distance
            d.distToPlayer = std::sqrt(distSq);

            // Determine color scheme based on faction/hostility
            d.dispo = GetDisposition(a, player);
            d.isPlayer = false;  // NPCs don't use tier effects

            // Occlusion check with caching to avoid checking every frame
            if (Settings::EnableOcclusionCulling)
            {
                auto cacheIt = s_cache.find(d.formID);
                bool shouldCheck = true;

                if (cacheIt != s_cache.end() && cacheIt->second.initialized)
                {
                    // Only use cached result if:
                    // 1. Cache entry exists and is initialized
                    // 2. We checked recently
                    uint32_t framesSince = s_frame - cacheIt->second.lastOcclusionCheckFrame;
                    if (framesSince < static_cast<uint32_t>(Settings::OcclusionCheckInterval))
                    {
                        d.isOccluded = cacheIt->second.cachedOccluded;
                        shouldCheck = false;
                    }
                }

                if (shouldCheck)
                {
                    // Use the nameplate world position for occlusion check
                    // This is more accurate than actor's position
                    d.isOccluded = Occlusion::IsActorOccluded(a, player, d.worldPos);
                    // Update the cache with the fresh check result
                    // Note: We update the cache here in the game thread, not in render thread
                    if (cacheIt != s_cache.end())
                    {
                        cacheIt->second.lastOcclusionCheckFrame = s_frame;
                        cacheIt->second.cachedOccluded = d.isOccluded;
                    }
                }
            }

            tempBuf.push_back(std::move(d));
            ++added;
        }

        // Atomically swap the snapshot with our new data
        // This ensures the render thread sees a consistent state
        {
            std::lock_guard<std::mutex> lock(s_snapshotLock);
            s_snapshot = tempBuf;
        }
    }

    static void QueueSnapshotUpdate_RenderThread()
    {
        // Check if an update is already queued
        // If exchange returns true, an update is already pending, so skip
        if (s_updateQueued.exchange(true))
        {
            return;  // Already queued, don't queue again
        }

        // Schedule the update task on the game thread
        // We can't iterate actors safely from the render thread, so we
        // use SKSE's task interface to run on the game thread instead
        if (auto *task = SKSE::GetTaskInterface())
        {
            task->AddTask([](){ UpdateSnapshot_GameThread(); });
        }
        else
        {
            // Task interface not available, clear the flag
            s_updateQueued.store(false);
        }
    }

    static void PruneCacheToSnapshot(const std::vector<ActorDrawData> &snap)
    {
        // Grace period prevents jitter when actors briefly leave the snapshot
        constexpr uint32_t kCacheGraceFrames = RenderConstants::kCacheGraceFrames;

        for (auto it = s_cache.begin(); it != s_cache.end();)
        {
            bool inSnapshot = false;
            for (auto &d : snap)
            {
                if (d.formID == it->first)
                {
                    inSnapshot = true;
                    it->second.lastSeenFrame = s_frame;  // Update last seen
                    break;
                }
            }

            if (!inSnapshot)
            {
                // Check if grace period has expired
                uint32_t framesSinceLastSeen = s_frame - it->second.lastSeenFrame;
                if (framesSinceLastSeen > kCacheGraceFrames)
                {
                    it = s_cache.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    // Compute blend factor for frame-rate independent exponential smoothing.
    // Returns alpha in [0,1] for use with: current = lerp(current, target, alpha)
    static float ExpApproachAlpha(float dt, float settleTime, float epsilon = 0.01f)
    {
        dt = std::max(0.0f, dt);
        settleTime = std::max(1e-5f, settleTime);
        return std::clamp(1.0f - std::pow(epsilon, dt / settleTime), 0.0f, 1.0f);
    }

    static void ApplyTextEffect(
        ImDrawList *drawList,
        ImFont *font,
        float fontSize,
        ImVec2 pos,
        const char *text,
        const Settings::EffectParams &effect,
        ImU32 colL,
        ImU32 colR,
        ImU32 highlight,
        ImU32 outlineColor,
        float outlineWidth,
        float phase01,
        float strength,
        float textSizeScale,
        float alpha)
    {
        switch (effect.type)
        {
        case Settings::EffectType::None:
            // Solid color with outline
            TextEffects::AddTextOutline4(drawList, font, fontSize, pos, text,
                                         colL, outlineColor, outlineWidth);
            break;

        case Settings::EffectType::Gradient:
            TextEffects::AddTextOutline4Gradient(drawList, font, fontSize, pos, text,
                                                 colL, colR, outlineColor, outlineWidth);
            break;

        case Settings::EffectType::VerticalGradient:
            TextEffects::AddTextOutline4VerticalGradient(drawList, font, fontSize, pos, text,
                                                         colL, colR, outlineColor, outlineWidth);
            break;

        case Settings::EffectType::DiagonalGradient:
            TextEffects::AddTextOutline4DiagonalGradient(drawList, font, fontSize, pos, text,
                                                         colL, colR, ImVec2(effect.param1, effect.param2), outlineColor, outlineWidth);
            break;

        case Settings::EffectType::RadialGradient:
            TextEffects::AddTextOutline4RadialGradient(drawList, font, fontSize, pos, text,
                                                       colL, colR, outlineColor, outlineWidth, effect.param1);
            break;

        case Settings::EffectType::Shimmer:
            TextEffects::AddTextOutline4Shimmer(drawList, font, fontSize, pos, text,
                                                colL, colR, highlight, outlineColor, outlineWidth,
                                                phase01, effect.param1, effect.param2 > 0.0f ? effect.param2 * strength : strength);
            break;

        case Settings::EffectType::ChromaticShimmer:
            TextEffects::AddTextOutline4ChromaticShimmer(drawList, font, fontSize, pos, text,
                                                         colL, colR, highlight, outlineColor, outlineWidth,
                                                         phase01, effect.param1, effect.param2 * strength, effect.param3 * textSizeScale, effect.param4);
            break;

        case Settings::EffectType::PulseGradient:
        {
            float time = (float)ImGui::GetTime();
            TextEffects::AddTextOutline4PulseGradient(drawList, font, fontSize, pos, text,
                                                      colL, colR, time, effect.param1, effect.param2 * strength, outlineColor, outlineWidth);
        }
        break;

        case Settings::EffectType::RainbowWave:
            // Always draw outline first for consistency
            TextEffects::AddTextOutline4RainbowWave(drawList, font, fontSize, pos, text,
                                                    effect.param1, effect.param2, effect.param3, effect.param4, effect.param5,
                                                    alpha, outlineColor, outlineWidth, effect.useWhiteBase);
            break;

        case Settings::EffectType::ConicRainbow:
            TextEffects::AddTextOutline4ConicRainbow(drawList, font, fontSize, pos, text,
                                                     effect.param1, effect.param2, effect.param3, effect.param4, alpha,
                                                     outlineColor, outlineWidth, effect.useWhiteBase);
            break;

        case Settings::EffectType::Aurora:
            // Aurora effect: param1=speed, param2=waves, param3=intensity, param4=sway
            // Creates flowing northern lights effect with left/right colors
            TextEffects::AddTextOutline4Aurora(drawList, font, fontSize, pos, text,
                                               colL, colR, outlineColor, outlineWidth,
                                               effect.param1 > 0.0f ? effect.param1 : 0.5f,
                                               effect.param2 > 0.0f ? effect.param2 : 3.0f,
                                               effect.param3 > 0.0f ? effect.param3 : 1.0f,
                                               effect.param4 > 0.0f ? effect.param4 : 0.3f);
            break;

        case Settings::EffectType::Sparkle:
            // Sparkle effect: param1=density, param2=speed, param3=intensity
            // Uses highlight color for sparkles
            TextEffects::AddTextOutline4Sparkle(drawList, font, fontSize, pos, text,
                                                colL, colR, highlight, outlineColor, outlineWidth,
                                                effect.param1 > 0.0f ? effect.param1 : 0.3f,
                                                effect.param2 > 0.0f ? effect.param2 : 2.0f,
                                                effect.param3 > 0.0f ? effect.param3 * strength : strength);
            break;

        case Settings::EffectType::Plasma:
            // Plasma effect: param1=freq1, param2=freq2, param3=speed
            TextEffects::AddTextOutline4Plasma(drawList, font, fontSize, pos, text,
                                               colL, colR, outlineColor, outlineWidth,
                                               effect.param1 > 0.0f ? effect.param1 : 2.0f,
                                               effect.param2 > 0.0f ? effect.param2 : 3.0f,
                                               effect.param3 > 0.0f ? effect.param3 : 0.5f);
            break;

        case Settings::EffectType::Scanline:
            // Scanline effect: param1=speed, param2=width, param3=intensity
            // Uses highlight color for scanline
            TextEffects::AddTextOutline4Scanline(drawList, font, fontSize, pos, text,
                                                 colL, colR, highlight, outlineColor, outlineWidth,
                                                 effect.param1 > 0.0f ? effect.param1 : 0.5f,
                                                 effect.param2 > 0.0f ? effect.param2 : 0.15f,
                                                 effect.param3 > 0.0f ? effect.param3 * strength : strength);
            break;
        }
    }

    static void DrawLabel(const ActorDrawData &d, ImDrawList *drawList)
    {
        // Get or create cache entry for this actor (keyed by form ID)
        // The cache stores smoothing state for position, alpha, and text size
        auto it = s_cache.find(d.formID);
        if (it == s_cache.end())
        {
            ActorCache newEntry{};
            newEntry.lastSeenFrame = s_frame;  // Initialize to current frame
            it = s_cache.emplace(d.formID, newEntry).first;
        }
        auto &entry = it->second;
        entry.lastSeenFrame = s_frame;  // Always update last seen

        // Detect name changes (e.g., after showracemenu) and reset typewriter
        if (entry.cachedName != d.name)
        {
            entry.cachedName = d.name;
            entry.typewriterTime = 0.0f;
            entry.typewriterComplete = false;
        }

        // Reset typewriter when actor re-enters range after being unseen,
        // or when they become visible again after occlusion.
        constexpr uint32_t kReentryThreshold = 30;  // Frames before considering re-entry
        if (entry.initialized && entry.typewriterComplete)
        {
            uint32_t framesSinceLastSeen = s_frame - entry.lastSeenFrame;
            bool becameVisible = entry.wasOccluded && !d.isOccluded;
            if (framesSinceLastSeen >= kReentryThreshold || becameVisible)
            {
                entry.typewriterTime = 0.0f;
                entry.typewriterComplete = false;
            }
        }

        entry.lastSeenFrame = s_frame;  // Mark as seen this frame (for pruning)

        // Get camera position for camera-relative scaling
        RE::NiPoint3 cameraPos{};
        bool hasCameraPos = false;
        if (auto pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot)
        {
            cameraPos = pc->cameraRoot->world.translate;
            hasCameraPos = true;
        }

        const float dist = d.distToPlayer;          // Player-to-actor distance in game units
        const float dt = ImGui::GetIO().DeltaTime;  // Time since last frame

        // Calculate alpha target based on distance
        // Uses smooth interpolation to avoid harsh transitions
        float fadeT = TextEffects::SmoothStep((dist - Settings::FadeStartDistance) / (Settings::FadeEndDistance - Settings::FadeStartDistance));
        float alphaTarget = 1.0f - fadeT;         // Invert: 1.0 at near, 0.0 at far
        alphaTarget = alphaTarget * alphaTarget;  // Square for more opacity at near range

        // Calculate font size scale target based on distance
        // Names get smaller as actors move farther away
        float scaleT = TextEffects::Saturate((dist - Settings::ScaleStartDistance) / (Settings::ScaleEndDistance - Settings::ScaleStartDistance));

        // Apply gamma correction to scale transition
        // kScaleGamma < 1.0 makes names stay larger longer, then shrink quickly
        // kScaleGamma > 1.0 makes names shrink quickly, then stay small
        constexpr float kScaleGamma = 0.5f;
        scaleT = std::pow(scaleT, kScaleGamma);

        // Convert to actual font scale: 1.0 at near, MinimumScale at far
        float textScaleTarget = 1.0f + (Settings::MinimumScale - 1.0f) * scaleT;

        // Blend in camera-to-actor distance to react to camera zoom
        // If camera is closer than player, bias scale to stay larger
        if (hasCameraPos)
        {
            float camDist = std::sqrt(
                std::pow(d.worldPos.x - cameraPos.x, 2.0f) +
                std::pow(d.worldPos.y - cameraPos.y, 2.0f) +
                std::pow(d.worldPos.z - cameraPos.z, 2.0f));

            float camScaleT = TextEffects::Saturate((camDist - Settings::ScaleStartDistance) / (Settings::ScaleEndDistance - Settings::ScaleStartDistance));
            camScaleT = std::pow(camScaleT, kScaleGamma);
            float camTextScale = 1.0f + (Settings::MinimumScale - 1.0f) * camScaleT;

            // Blend player-distance scale and camera-distance scale
            // Use a weighted min to avoid popping when zooming in
            textScaleTarget = std::min(textScaleTarget, camTextScale);
        }

        // Project world position to screen space
        // Jitter reduction is handled by screen-space moving average below
        RE::NiPoint3 screenPos;
        if (!WorldToScreen(d.worldPos, screenPos))
        {
            return;  // Behind camera or outside field of view
        }

        // Occlusion target (1.0 = visible, 0.0 = occluded)
        float occlusionTarget = d.isOccluded ? 0.0f : 1.0f;

        if (!entry.initialized)
        {
            // First time seeing this actor: Initialize cache with current values
            entry.initialized = true;
            entry.alphaSmooth = alphaTarget;
            entry.textSizeScale = textScaleTarget;
            entry.smooth = ImVec2(screenPos.x, screenPos.y);

            // Fill position history buffer with current position to avoid initial jitter
            ImVec2 initPos(screenPos.x, screenPos.y);
            for (int i = 0; i < ActorCache::kHistorySize; i++)
            {
                entry.posHistory[i] = initPos;
            }
            entry.historyIndex = 0;
            entry.historyFilled = true;

            // Start actors as visible and let them fade out if occluded
            // This prevents the "never appears" bug when HasLineOfSight is unreliable
            // on zone load or when actors are still loading in
            entry.occlusionSmooth = 1.0f;

            // Initialize typewriter state
            entry.typewriterTime = 0.0f;
            entry.typewriterComplete = false;
        }
        else
        {
            // Subsequent frames smooth toward target values
            // Calculate interpolation factors
            float aLerp = ExpApproachAlpha(dt, Settings::AlphaSettleTime);
            float sLerp = ExpApproachAlpha(dt, Settings::ScaleSettleTime);

            // Player uses near-instant response, NPCs use smooth settling
            float pLerp = d.isPlayer ? ExpApproachAlpha(dt, 0.015f) : ExpApproachAlpha(dt, Settings::PositionSettleTime);
            float oLerp = ExpApproachAlpha(dt, Settings::OcclusionSettleTime);

            // Apply exponential smoothing for alpha and scale
            entry.alphaSmooth += (alphaTarget - entry.alphaSmooth) * aLerp;
            entry.textSizeScale += (textScaleTarget - entry.textSizeScale) * sLerp;
            entry.occlusionSmooth += (occlusionTarget - entry.occlusionSmooth) * oLerp;

            // Use moving average for position to filter out periodic jitter
            // This is more effective than exponential smoothing for regular oscillations
            ImVec2 targetPos(screenPos.x, screenPos.y);
            ImVec2 smoothedPos = entry.AddAndGetSmoothed(targetPos);

            // For large movements, blend more toward target for responsiveness
            float dx = targetPos.x - entry.smooth.x;
            float dy = targetPos.y - entry.smooth.y;
            float dist = std::sqrt(dx * dx + dy * dy);

            if (dist > 50.0f)
            {
                // Large movement - blend toward moving average quickly
                entry.smooth.x += (smoothedPos.x - entry.smooth.x) * 0.5f;
                entry.smooth.y += (smoothedPos.y - entry.smooth.y) * 0.5f;
            }
            else
            {
                // Normal movement - use moving average directly
                entry.smooth = smoothedPos;
            }

            // Update typewriter time, starts after delay
            if (Settings::EnableTypewriter && !entry.typewriterComplete)
            {
                entry.typewriterTime += dt;
            }
        }

        // Track occlusion state for next frame
        entry.wasOccluded = d.isOccluded;

        // Use smoothed values for rendering, combine alpha with occlusion
        const float alpha = entry.alphaSmooth * entry.occlusionSmooth;
        if (alpha <= 0.02f)
        {
            return;  // Too faded, skip rendering
        }

        const float textSizeScale = entry.textSizeScale;  // Note: Font scale is independent of alpha

        // Field of view culling, skip if completely off-screen
        // Allow some overflow (100px) so names can partially appear at screen edges
        const auto viewSize = RE::BSGraphics::Renderer::GetSingleton()->GetScreenSize();
        if (screenPos.z < 0 || screenPos.z > 1.0f ||
            screenPos.x < -100.0f || screenPos.x > viewSize.width + 100.0f ||
            screenPos.y < -100.0f || screenPos.y > viewSize.height + 100.0f)
        {
            return;  // Off-screen, skip
        }

        const float time = (float)ImGui::GetTime();  // For animations

        // Helper lambda, "Wash" colors toward white for a softer appearance
        // Higher wash value = more white, less saturated
        auto WashColor = [](ImVec4 base)
        {
            const float wash = Settings::ColorWashAmount;
            return ImVec4(
                base.x + (1.0f - base.x) * wash,  // Lerp R toward 1.0
                base.y + (1.0f - base.y) * wash,  // Lerp G toward 1.0
                base.z + (1.0f - base.z) * wash,  // Lerp B toward 1.0
                base.w);                          // Keep alpha unchanged
        };

        // Determine NPC disposition color
        ImVec4 dispoCol;
        if (d.dispo == Disposition::Enemy)
            dispoCol = WashColor(ImVec4(0.9f, 0.2f, 0.2f, alpha));  // Red for enemies
        else if (d.dispo == Disposition::AllyOrFriend)
            dispoCol = WashColor(ImVec4(0.2f, 0.6f, 1.0f, alpha));  // Blue for allies
        else
            dispoCol = WashColor(ImVec4(0.9f, 0.9f, 0.9f, alpha));  // Gray for neutral

        // Determine which tier this level falls into
        const uint16_t lv = (uint16_t)std::min<int>(d.level, 9999);  // Effectively no cap

        // Find matching tier by level range
        int tierIdx = 0;
        for (size_t i = 0; i < Settings::Tiers.size(); ++i)
        {
            if (lv >= Settings::Tiers[i].minLevel && lv <= Settings::Tiers[i].maxLevel)
            {
                tierIdx = static_cast<int>(i);
                break;
            }
        }
        tierIdx = std::clamp(tierIdx, 0, static_cast<int>(Settings::Tiers.size()) - 1);
        const Settings::TierDefinition &tier = Settings::Tiers[tierIdx];

        // Check for special title match
        // Special titles override normal tier styling for MMORPG-style nameplates
        const Settings::SpecialTitleDefinition* specialTitle = nullptr;
        {
            // Convert name to lowercase for case-insensitive matching
            std::string nameLower = d.name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            // Sort by priority and check each special title
            std::vector<const Settings::SpecialTitleDefinition*> sortedSpecials;
            for (const auto& st : Settings::SpecialTitles) {
                if (!st.keyword.empty()) {
                    sortedSpecials.push_back(&st);
                }
            }
            std::sort(sortedSpecials.begin(), sortedSpecials.end(),
                      [](const auto* a, const auto* b) { return a->priority > b->priority; });

            for (const auto* st : sortedSpecials) {
                std::string keywordLower = st->keyword;
                std::transform(keywordLower.begin(), keywordLower.end(), keywordLower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (nameLower.find(keywordLower) != std::string::npos) {
                    specialTitle = st;
                    break;
                }
            }
        }

        // Calculate position within tier [0, 1]
        // 0.0 = at min level of tier, 1.0 = at max level of tier
        float levelT = 0.0f;
        if (tier.maxLevel > tier.minLevel)
        {
            levelT = (lv <= tier.minLevel) 
            ? 0.0f 
            : (lv >= tier.maxLevel) 
                ? 1.0f
                : (float)(lv - tier.minLevel) / (float)(tier.maxLevel - tier.minLevel);
        }
        levelT = std::clamp(levelT, 0.0f, 1.0f);

        // Reduce effect intensity for levels <100
        const bool under100 = (lv < 100);
        const float tierIntensity = under100 ? 0.5f : 1.0f;  // 50% intensity for low levels

        // Pastelize colors based on level position in tier
        // At levelT = 0.0 (bottom of tier): more desaturated
        // At levelT = 1.0 (top of tier): full vibrant color
        auto Pastelize = [&](const float *c) -> ImVec4
        {
            const float t = Settings::NameColorMix + (1.0f - Settings::NameColorMix) * levelT;
            // Lerp from white (1.0) toward tier color (c) by factor t
            return ImVec4(
                1.0f + (c[0] - 1.0f) * t,
                1.0f + (c[1] - 1.0f) * t,
                1.0f + (c[2] - 1.0f) * t,
                1.0f);
        };

        // Apply pastelization to tier colors
        ImVec4 Lc = Pastelize(tier.leftColor);
        ImVec4 Rc = Pastelize(tier.rightColor);

        // Calculate alpha for visual effects (shimmers, etc.)
        // Scales with tier intensity and level position
        const float effectAlpha = alpha * tierIntensity * (Settings::EffectAlphaMin + Settings::EffectAlphaMax * levelT);

        // Mix color toward white
        // amount = 1.0 keeps original color, 0.0 gives pure white
        auto MixToWhite = [](ImVec4 c, float amount)
        {
            amount = std::clamp(amount, 0.0f, 1.0f);
            return ImVec4(
                1.0f + (c.x - 1.0f) * amount,  // Lerp toward white (1.0)
                1.0f + (c.y - 1.0f) * amount,
                1.0f + (c.z - 1.0f) * amount,
                c.w  // Preserve alpha
            );
        };

        // Calculate color intensity for low levels
        // Low levels get more whitened to avoid overly saturated colors
        const float baseColorAmount = under100 ? (0.35f + 0.65f * tierIntensity) : 1.0f;

        // Create color variations with progressive whitening:
        // Level, softened tier colors
        ImVec4 LcLevel = MixToWhite(Lc, baseColorAmount);
        ImVec4 RcLevel = MixToWhite(Rc, baseColorAmount);

        // Name, washed level colors
        ImVec4 LcName = WashColor(LcLevel);
        ImVec4 RcName = WashColor(RcLevel);

        // Title, extra washed
        ImVec4 LcTitle = WashColor(LcName);
        ImVec4 RcTitle = WashColor(RcName);

        // Special title glow color
        ImVec4 specialGlowColor(1.0f, 1.0f, 1.0f, 1.0f);

        // Override colors if this is a special title
        if (specialTitle) {
            // Use special title's custom colors
            ImVec4 specialCol(specialTitle->color[0], specialTitle->color[1], specialTitle->color[2], 1.0f);
            specialGlowColor = ImVec4(specialTitle->glowColor[0], specialTitle->glowColor[1], specialTitle->glowColor[2], 1.0f);

            // Apply the special color with slight variations for visual hierarchy
            Lc = specialCol;
            Rc = specialCol;
            LcLevel = specialCol;
            RcLevel = specialCol;
            LcName = specialCol;
            RcName = specialCol;
            // Title uses slightly brighter/washed version
            LcTitle = WashColor(specialCol);
            RcTitle = WashColor(specialCol);
        }

        // Convert to packed ImU32 format for rendering
        // Name colors
        ImU32 colL = ImGui::ColorConvertFloat4ToU32(ImVec4(LcName.x, LcName.y, LcName.z, alpha));
        ImU32 colR = ImGui::ColorConvertFloat4ToU32(ImVec4(RcName.x, RcName.y, RcName.z, alpha));

        // Reduced alpha for secondary text elements (title, level, separator)
        // This makes the name stand out more as the primary element
        const float titleAlpha = alpha * 0.8f;   // Title slightly subtle, above the name
        const float levelAlpha = alpha * 0.85f;  // Level/separator slightly subdued

        // Title colors
        ImU32 colLTitle = ImGui::ColorConvertFloat4ToU32(ImVec4(LcTitle.x, LcTitle.y, LcTitle.z, titleAlpha));
        ImU32 colRTitle = ImGui::ColorConvertFloat4ToU32(ImVec4(RcTitle.x, RcTitle.y, RcTitle.z, titleAlpha));

        // Level colors
        ImU32 colLLevel = ImGui::ColorConvertFloat4ToU32(ImVec4(LcLevel.x, LcLevel.y, LcLevel.z, levelAlpha));
        ImU32 colRLevel = ImGui::ColorConvertFloat4ToU32(ImVec4(RcLevel.x, RcLevel.y, RcLevel.z, levelAlpha));

        // Highlight color for shimmer/special effects
        ImU32 highlight = ImGui::ColorConvertFloat4ToU32(ImVec4(tier.highlightColor[0], tier.highlightColor[1], tier.highlightColor[2], effectAlpha));

        // Outline and shadow colors
        ImU32 outlineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 1));
        ImU32 shadowColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 1));

        // Base outline width from settings
        const float baseOutlineWidth = Settings::OutlineWidthMin + Settings::OutlineWidthMax;

        // Calculate outline width scaled proportionally to font size
        // Uses the configured NameFontSize as reference so outline looks correct at default zoom
        auto calcOutlineWidth = [=](float fontSize) {
            return baseOutlineWidth * (fontSize / Settings::NameFontSize);
        };

        // Get fractional part of float
        auto frac = [](float x)
        { return x - std::floor(x); };

        // Determine animation speed based on tier position
        // Higher tiers get slower, more "legendary" animations
        float tierAnimSpeed = Settings::AnimSpeedLowTier;  // Default for low tiers
        if (!Settings::Tiers.empty())
        {
            float tierRatio = static_cast<float>(tierIdx) / static_cast<float>(Settings::Tiers.size() - 1);
            if (tierRatio >= 0.9f)
            {
                tierAnimSpeed = Settings::AnimSpeedHighTier;  // Slowest for top tiers
            }
            else if (tierRatio >= 0.8f)
            {
                tierAnimSpeed = Settings::AnimSpeedMidTier;  // Medium for mid-high tiers
            }
        }
        // Further slow down animations for low levels
        if (under100)
            tierAnimSpeed *= 0.75f;

        // Calculate animation phase [0, 1] for this actor
        // Each actor gets a unique seed based on form ID to prevent synchronization
        const float phaseSeed = (d.formID & 1023) / 1023.0f;           // Unique seed per actor
        const float phase01 = frac(time * tierAnimSpeed + phaseSeed);  // Animated phase

        // Format string replacement
        // Replaces placeholders: %n = name, %l = level, %t = title
        auto FormatString = [&](const std::string &fmt, const char *nameVal, int levelVal, const char *titleVal = nullptr)
        {
            std::string s = fmt;

            // Replace %n with actor name
            size_t pos = 0;
            while ((pos = s.find("%n", pos)) != std::string::npos)
            {
                s.replace(pos, 2, nameVal);
                pos += strlen(nameVal);  // Move past replacement to avoid infinite loop
            }

            // Replace %l with actor level
            pos = 0;
            std::string lStr = std::to_string(levelVal);
            while ((pos = s.find("%l", pos)) != std::string::npos)
            {
                s.replace(pos, 2, lStr);
                pos += lStr.length();
            }

            // Replace %t with tier title
            if (titleVal)
            {
                pos = 0;
                while ((pos = s.find("%t", pos)) != std::string::npos)
                {
                    s.replace(pos, 2, titleVal);
                    pos += strlen(titleVal);
                }
            }
            return s;
        };

        // Render segment
        struct RenderSeg
        {
            std::string text;         ///< Formatted text to display
            std::string displayText;  ///< Text after typewriter truncation
            bool isLevel;             ///< Whether to use level font
            ImFont *font;             ///< Font to use for rendering
            float fontSize;           ///< Scaled font size
            ImVec2 size;              ///< Measured size of this segment
            ImVec2 displaySize;       ///< Measured size of displayText
        };

        // Calculate how many characters should be visible based on time elapsed
        int typewriterCharsToShow = -1;  // -1 = show all (disabled or complete)
        if (Settings::EnableTypewriter && !entry.typewriterComplete)
        {
            float effectiveTime = entry.typewriterTime - Settings::TypewriterDelay;
            if (effectiveTime > 0.0f)
            {
                typewriterCharsToShow = static_cast<int>(effectiveTime * Settings::TypewriterSpeed);
            }
            else
            {
                typewriterCharsToShow = 0;  // Still in delay period
            }
        }

        // Get fonts loaded in Hooks.cpp (Index 0 = name, 1 = level, 2 = title)
        ImFont *fontName = ImGui::GetIO().Fonts->Fonts[0];
        ImFont *fontLevel = ImGui::GetIO().Fonts->Fonts[1];
        ImFont *fontTitle = ImGui::GetIO().Fonts->Fonts[2];

        // Calculate scaled font sizes
        const float nameFontSize = fontName->FontSize * textSizeScale;
        const float levelFontSize = fontLevel->FontSize * textSizeScale;
        const float titleFontSize = fontTitle->FontSize * textSizeScale;

        // Per-font outline widths (scaled to font size)
        const float nameOutlineWidth = calcOutlineWidth(nameFontSize);
        const float levelOutlineWidth = calcOutlineWidth(levelFontSize);
        const float titleOutlineWidth = calcOutlineWidth(titleFontSize);

        // Primary outline width for layout calculations
        const float outlineWidth = nameOutlineWidth;

        // Use space if name is empty
        const char *safeName = d.name.empty() ? " " : d.name.c_str();

        // Build segments for the main line
        std::vector<RenderSeg> segments;
        float mainLineWidth = 0.0f;   // Total width of all segments
        float mainLineHeight = 0.0f;  // Height of tallest segment

        // Use default format if settings not loaded or empty
        // Default: "%n Lv.%l" (e.g., "Lydia Lv.42")
        const auto &fmtList = Settings::DisplayFormat.empty() ? std::vector<Settings::Segment>{{"%n", false}, {" Lv.%l", true}} : Settings::DisplayFormat;

        // Track total characters for typewriter effect
        int totalCharsProcessed = 0;

        // Process each segment in the format list
        for (const auto &fmt : fmtList)
        {
            RenderSeg seg;
            seg.text = FormatString(fmt.format, safeName, d.level);  // Replace placeholders
            seg.isLevel = fmt.useLevelFont;                          // Choose font
            seg.font = seg.isLevel ? fontLevel : fontName;
            seg.fontSize = seg.isLevel ? levelFontSize : nameFontSize;

            // Measure the rendered size of this segment
            seg.size = seg.font->CalcTextSizeA(seg.fontSize, FLT_MAX, 0.0f, seg.text.c_str());

            // Apply typewriter truncation if active
            if (typewriterCharsToShow >= 0)
            {
                size_t segCharCount = Utf8CharCount(seg.text.c_str());
                int charsRemaining = typewriterCharsToShow - totalCharsProcessed;
                if (charsRemaining <= 0)
                {
                    seg.displayText = "";  // No characters visible yet
                }
                else if (static_cast<size_t>(charsRemaining) >= segCharCount)
                {
                    seg.displayText = seg.text;  // Show full segment
                }
                else
                {
                    seg.displayText = Utf8Truncate(seg.text.c_str(), charsRemaining);
                }
                totalCharsProcessed += static_cast<int>(segCharCount);
            }
            else
            {
                seg.displayText = seg.text;  // Typewriter disabled, show full text
            }

            // Measure display text size
            seg.displaySize = seg.font->CalcTextSizeA(seg.fontSize, FLT_MAX, 0.0f, seg.displayText.c_str());

            segments.push_back(seg);
            mainLineWidth += seg.size.x;  // Use full width for layout
            if (seg.size.y > mainLineHeight)
                mainLineHeight = seg.size.y;  // Track tallest
        }

        // Add padding between segments
        // Total width = sum of segment widths + (n-1) * padding
        float segmentPadding = Settings::SegmentPadding;
        if (!segments.empty())
        {
            mainLineWidth += (segments.size() - 1) * segmentPadding;
        }

        // Format the title text, displayed above main line
        // Special titles override the tier title with their custom display title
        const char* titleToUse = specialTitle ? specialTitle->displayTitle.c_str() : tier.title.c_str();
        std::string titleStr = FormatString(Settings::TitleFormat, safeName, d.level, titleToUse);
        std::string titleDisplayStr = titleStr;

        // Apply typewriter truncation to title
        if (typewriterCharsToShow >= 0)
        {
            size_t titleCharCount = Utf8CharCount(titleStr.c_str());
            int charsRemainingForTitle = typewriterCharsToShow - totalCharsProcessed;
            if (charsRemainingForTitle <= 0)
            {
                titleDisplayStr = "";
            }
            else if (static_cast<size_t>(charsRemainingForTitle) >= titleCharCount)
            {
                titleDisplayStr = titleStr;
            }
            else
            {
                titleDisplayStr = Utf8Truncate(titleStr.c_str(), charsRemainingForTitle);
            }
            totalCharsProcessed += static_cast<int>(titleCharCount);

            // Check if typewriter is complete (all text revealed)
            if (!entry.typewriterComplete && typewriterCharsToShow >= totalCharsProcessed)
            {
                entry.typewriterComplete = true;
            }
        }

        const char *titleText = titleStr.c_str();
        const char *titleDisplayText = titleDisplayStr.c_str();

        // Calculate tight vertical bounds for precise positioning
        // Title bounds, relative to baseline
        float titleTop = 0.0f, titleBottom = 0.0f;
        if (titleText && *titleText)
        {
            CalcTightYBoundsFromTop(fontTitle, titleFontSize, titleText, titleTop, titleBottom);
        }
        ImVec2 titleSize = fontTitle->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleText);

        // Main line tight bounds
        // We need to account for vertical offset used during rendering
        float mainTop = +FLT_MAX;
        float mainBottom = -FLT_MAX;

        bool any = false;
        for (const auto &seg : segments)
        {
            float sTop = 0.0f, sBottom = 0.0f;
            CalcTightYBoundsFromTop(seg.font, seg.fontSize, seg.text.c_str(), sTop, sBottom);

            // Calculate vertical offset
            // This centers smaller text within the tallest segment's height
            float vOffset = (mainLineHeight - seg.size.y) * 0.5f;

            // Track extremes accounting for the offset
            mainTop = std::min(mainTop, vOffset + sTop);
            mainBottom = std::max(mainBottom, vOffset + sBottom);
            any = true;
        }

        if (!any)
        {
            mainTop = 0.0f;
            mainBottom = 0.0f;
        }

        // Include shadow and outline in bounds visual extents of the text
        const float titleShadowY = Settings::TitleShadowOffsetY;
        const float mainShadowY = Settings::MainShadowOffsetY;

        // Calculate actual drawn extents including effects
        float titleBottomDraw = titleBottom + titleShadowY;              // Shadow extends downward
        float mainTopDraw = mainTop - outlineWidth;                      // Outline extends upward
        float mainBottomDraw = mainBottom + outlineWidth + mainShadowY;  // Outline shadow

        // Anchor main line so its visual bottom is at Y=0
        float mainLineY = -mainBottomDraw;

        // Stack title above main line with no gap
        // Title's drawn bottom should touch main's drawn top
        float titleY = mainLineY + mainTopDraw - titleBottomDraw;

        // Final rendering setup
        // startPos is the anchor point
        ImVec2 startPos = entry.smooth;

        // Total width is the larger of main line or title
        float totalWidth = std::max(mainLineWidth, titleSize.x);

        // Calculate effect strength based on tier and level position
        // Higher levels in higher tiers get stronger effects
        float strength = tierIntensity * (Settings::StrengthMin + Settings::StrengthMax * levelT);

        // Calculate overall nameplate bounds for decorative effects
        // These bounds encompass both title and main line
        float nameplateTop = startPos.y + titleY + titleTop;
        float nameplateBottom = startPos.y + mainLineY + mainBottom;
        float nameplateLeft = startPos.x - totalWidth * 0.5f;
        float nameplateRight = startPos.x + totalWidth * 0.5f;
        float nameplateWidth = totalWidth;
        float nameplateHeight = nameplateBottom - nameplateTop;
        ImVec2 nameplatePos(nameplateLeft, nameplateTop);
        ImVec2 nameplateSize(nameplateWidth, nameplateHeight);
        ImVec2 nameplateCenter(startPos.x, (nameplateTop + nameplateBottom) * 0.5f);

        // Draw particles first so they appear behind everything else
        bool tierHasParticles = !tier.particleTypes.empty() && tier.particleTypes != "None";
        bool showParticles = (d.isPlayer && Settings::EnableParticleAura && tierHasParticles)
                          || (specialTitle && specialTitle->forceParticles);
        if (showParticles)
        {
            // Use special title's color for particles, or tier highlight color for normal
            ImU32 particleColor;
            if (specialTitle) {
                particleColor = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(specialTitle->color[0], specialTitle->color[1], specialTitle->color[2], 1.0f));
            } else {
                particleColor = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(tier.highlightColor[0], tier.highlightColor[1], tier.highlightColor[2], 1.0f));
            }

            // Particle spread based on nameplate size
            float spreadX = (nameplateWidth * 0.5f + Settings::ParticleSpread);
            float spreadY = (nameplateHeight * 0.5f + Settings::ParticleSpread * 0.6f);

            // Use tier-specific particle count if set, otherwise global
            int particleCount = (tier.particleCount > 0) ? tier.particleCount : Settings::ParticleCount;

            // Determine which particle types to render
            // If tier has specific types, use those; otherwise use global settings
            bool showOrbs = false, showWisps = false, showRunes = false, showSparks = false, showStars = false;

            if (tierHasParticles) {
                // Parse tier-specific particle types (comma-separated)
                showOrbs = tier.particleTypes.find("Orbs") != std::string::npos;
                showWisps = tier.particleTypes.find("Wisps") != std::string::npos;
                showRunes = tier.particleTypes.find("Runes") != std::string::npos;
                showSparks = tier.particleTypes.find("Sparks") != std::string::npos;
                showStars = tier.particleTypes.find("Stars") != std::string::npos;
            } else {
                // Use global settings
                showOrbs = Settings::EnableOrbs;
                showWisps = Settings::EnableWisps;
                showRunes = Settings::EnableRunes;
                showSparks = Settings::EnableSparks;
                showStars = Settings::EnableStars;
            }

            // Count enabled styles for alpha scaling
            int enabledStyles = (int)showOrbs + (int)showWisps + (int)showRunes + (int)showSparks + (int)showStars;
            int slot = 0;

            // Render each enabled particle type
            if (showOrbs)
            {
                TextEffects::DrawParticleAura(drawList, nameplateCenter, spreadX, spreadY,
                                              particleColor, Settings::ParticleAlpha * alpha * 0.7f,
                                              Settings::ParticleStyle::Orbs, particleCount,
                                              Settings::ParticleSize, Settings::ParticleSpeed, time,
                                              slot++, enabledStyles);
            }
            if (showWisps)
            {
                TextEffects::DrawParticleAura(drawList, nameplateCenter, spreadX * 1.1f, spreadY * 1.1f,
                                              particleColor, Settings::ParticleAlpha * alpha * 0.8f,
                                              Settings::ParticleStyle::Wisps, particleCount,
                                              Settings::ParticleSize, Settings::ParticleSpeed, time,
                                              slot++, enabledStyles);
            }
            if (showRunes)
            {
                TextEffects::DrawParticleAura(drawList, nameplateCenter, spreadX * 0.9f, spreadY * 0.7f,
                                              particleColor, Settings::ParticleAlpha * alpha,
                                              Settings::ParticleStyle::Runes, std::max(4, particleCount / 2),
                                              Settings::ParticleSize * 1.2f, Settings::ParticleSpeed * 0.6f, time,
                                              slot++, enabledStyles);
            }
            if (showSparks)
            {
                TextEffects::DrawParticleAura(drawList, nameplateCenter, spreadX, spreadY * 0.8f,
                                              particleColor, Settings::ParticleAlpha * alpha,
                                              Settings::ParticleStyle::Sparks, particleCount,
                                              Settings::ParticleSize * 0.7f, Settings::ParticleSpeed * 1.5f, time,
                                              slot++, enabledStyles);
            }
            if (showStars)
            {
                TextEffects::DrawParticleAura(drawList, nameplateCenter, spreadX, spreadY,
                                              particleColor, Settings::ParticleAlpha * alpha,
                                              Settings::ParticleStyle::Stars, particleCount,
                                              Settings::ParticleSize, Settings::ParticleSpeed, time,
                                              slot++, enabledStyles);
            }
        }

        // Determine which ornaments to use
        const std::string& leftOrns = (specialTitle && !specialTitle->leftOrnaments.empty())
            ? specialTitle->leftOrnaments : tier.leftOrnaments;
        const std::string& rightOrns = (specialTitle && !specialTitle->rightOrnaments.empty())
            ? specialTitle->rightOrnaments : tier.rightOrnaments;
        bool hasOrnaments = !leftOrns.empty() || !rightOrns.empty();
        bool showOrnaments = (d.isPlayer && Settings::EnableOrnaments && hasOrnaments)
                          || (specialTitle && specialTitle->forceOrnaments && hasOrnaments);
        if (showOrnaments && !Settings::OrnamentFontPath.empty())
        {
            // Get ornament font
            auto& io = ImGui::GetIO();
            if (io.Fonts->Fonts.Size >= 4)
            {
                ImFont* ornamentFont = io.Fonts->Fonts[3];
                if (ornamentFont)
                {
                    // Calculate ornament scale based on tier position
                    float ornamentScale = 0.75f;
                    if (Settings::Tiers.size() > 1) {
                        ornamentScale = 0.75f + 0.5f * (static_cast<float>(tierIdx) / static_cast<float>(Settings::Tiers.size() - 1));
                    }
                    float sizeMultiplier = (specialTitle != nullptr) ? ornamentScale * 1.3f : ornamentScale;
                    float ornamentSize = Settings::OrnamentFontSize * Settings::OrnamentScale * sizeMultiplier;

                    // Extra padding between ornaments and text
                    float extraPadding = ornamentSize * 0.15f;
                    float totalSpacing = Settings::OrnamentSpacing + extraPadding;

                    // Use same colors as name for ornaments
                    ImU32 ornColL = ImGui::ColorConvertFloat4ToU32(ImVec4(Lc.x, Lc.y, Lc.z, alpha));
                    ImU32 ornColR = ImGui::ColorConvertFloat4ToU32(ImVec4(Rc.x, Rc.y, Rc.z, alpha));
                    ImU32 ornHighlight = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(tier.highlightColor[0], tier.highlightColor[1], tier.highlightColor[2], alpha));
                    ImU32 ornOutline = IM_COL32(0, 0, 0, (int)(alpha * 255.0f));

                    // Calculate outline width scaled for ornament size
                    float ornOutlineWidth = outlineWidth * (ornamentSize / nameFontSize);

                    // Glow color for ornaments
                    ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(ImVec4(Lc.x, Lc.y, Lc.z, alpha));

                    // Use UTF-8 aware iteration for multi-byte characters
                    if (!leftOrns.empty())
                    {
                        auto leftChars = Utf8ToChars(leftOrns);
                        float cursorX = nameplateCenter.x - nameplateWidth * 0.5f - totalSpacing;
                        // Draw characters in reverse order
                        for (int i = static_cast<int>(leftChars.size()) - 1; i >= 0; --i)
                        {
                            const std::string& ch = leftChars[i];
                            ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, 0.0f, ch.c_str());
                            cursorX -= charSize.x;
                            ImVec2 charPos(cursorX, nameplateCenter.y - charSize.y * 0.5f);

                            // Draw glow
                            if (Settings::EnableGlow && Settings::GlowIntensity > 0.0f)
                            {
                                TextEffects::AddTextGlow(drawList, ornamentFont, ornamentSize, charPos,
                                                         ch.c_str(), glowColor, Settings::GlowRadius,
                                                         Settings::GlowIntensity, Settings::GlowSamples);
                            }

                            // Draw ornament with effect
                            ApplyTextEffect(drawList, ornamentFont, ornamentSize, charPos, ch.c_str(),
                                            tier.nameEffect, ornColL, ornColR, ornHighlight, ornOutline, ornOutlineWidth,
                                            phase01, strength, textSizeScale, alpha);
                        }
                    }

                    if (!rightOrns.empty())
                    {
                        auto rightChars = Utf8ToChars(rightOrns);
                        float cursorX = nameplateCenter.x + nameplateWidth * 0.5f + totalSpacing;
                        for (size_t i = 0; i < rightChars.size(); ++i)
                        {
                            const std::string& ch = rightChars[i];
                            ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, 0.0f, ch.c_str());
                            ImVec2 charPos(cursorX, nameplateCenter.y - charSize.y * 0.5f);

                            // Draw glow
                            if (Settings::EnableGlow && Settings::GlowIntensity > 0.0f)
                            {
                                TextEffects::AddTextGlow(drawList, ornamentFont, ornamentSize, charPos,
                                                         ch.c_str(), glowColor, Settings::GlowRadius,
                                                         Settings::GlowIntensity, Settings::GlowSamples);
                            }

                            // Draw ornament with effect
                            ApplyTextEffect(drawList, ornamentFont, ornamentSize, charPos, ch.c_str(),
                                            tier.nameEffect, ornColL, ornColR, ornHighlight, ornOutline, ornOutlineWidth,
                                            phase01, strength, textSizeScale, alpha);

                            cursorX += charSize.x;
                        }
                    }
                }
            }
        }

        // Render Title, if present and has visible characters
        if (titleDisplayText && *titleDisplayText)
        {
            // Center title horizontally within totalWidth
            float titleOffsetX = (totalWidth - titleSize.x) * 0.5f;
            ImVec2 titlePos(startPos.x - totalWidth * 0.5f + titleOffsetX,  // Centered X
                            startPos.y + titleY);                           // Calculated Y

            // Prepare colors for title
            ImU32 titleColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, alpha));
            ImU32 titleShadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, alpha * 0.5f));

            // Draw glow behind title
            if (Settings::EnableGlow && Settings::GlowIntensity > 0.0f)
            {
                // Use special glow color if available, otherwise tier left color
                ImVec4 glowColorVec = specialTitle
                    ? ImVec4(specialGlowColor.x, specialGlowColor.y, specialGlowColor.z, alpha)
                    : ImVec4(LcTitle.x, LcTitle.y, LcTitle.z, alpha);
                ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(glowColorVec);
                // Subtle glow boost for special titles
                float glowIntensity = specialTitle ? Settings::GlowIntensity * 1.15f : Settings::GlowIntensity;
                float glowRadius = specialTitle ? Settings::GlowRadius * 1.1f : Settings::GlowRadius;
                TextEffects::AddTextGlow(drawList, fontTitle, titleFontSize, titlePos,
                                         titleDisplayText, glowColor, glowRadius,
                                         glowIntensity, Settings::GlowSamples);
            }

            // Draw title shadow first
            drawList->AddText(fontTitle, titleFontSize,
                              ImVec2(titlePos.x + Settings::TitleShadowOffsetX,
                                     titlePos.y + Settings::TitleShadowOffsetY),
                              titleShadow, titleDisplayText);

            if (d.isPlayer)
            {
                // Apply tier-defined visual effect
                ApplyTextEffect(drawList, fontTitle, titleFontSize, titlePos, titleDisplayText,
                                tier.titleEffect, colLTitle, colRTitle, highlight, outlineColor, titleOutlineWidth,
                                phase01, strength, textSizeScale, titleAlpha);
            }
            else
            {
                // NPC, use disposition color with simple outline
                ImU32 dCol = ImGui::ColorConvertFloat4ToU32(WashColor(dispoCol));
                ImU32 npcOutline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, titleAlpha));
                TextEffects::AddTextOutline4(drawList, fontTitle, titleFontSize, titlePos, titleDisplayText, dCol, npcOutline, titleOutlineWidth);
            }
        }

        // Render Main Line
        // Position main line centered horizontally, below title
        ImVec2 currentPos;
        currentPos.x = startPos.x - totalWidth * 0.5f + (totalWidth - mainLineWidth) * 0.5f; // Center within totalWidth
        currentPos.y = startPos.y + mainLineY;                                           // Calculated Y position

        // Draw each segment sequentially
        for (const auto &seg : segments)
        {
            // Skip segments with no visible text, typewriter hasn't reached them yet
            if (seg.displayText.empty())
            {
                // Still advance position to maintain layout
                currentPos.x += seg.size.x + segmentPadding;
                continue;
            }

            // Calculate vertical offset to center this segment within mainLineHeight
            // Smaller segments get offset downward to align with larger ones
            float vOffset = (mainLineHeight - seg.size.y) * 0.5f;

            ImVec2 pos = ImVec2(currentPos.x, currentPos.y + vOffset);

            // Draw glow behind segment
            if (Settings::EnableGlow && Settings::GlowIntensity > 0.0f)
            {
                // Use special glow color if available, otherwise segment-appropriate color
                ImVec4 glowCol = specialTitle
                    ? ImVec4(specialGlowColor.x, specialGlowColor.y, specialGlowColor.z, alpha)
                    : (seg.isLevel ? ImVec4(LcLevel.x, LcLevel.y, LcLevel.z, alpha)
                                   : ImVec4(LcName.x, LcName.y, LcName.z, alpha));
                ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(glowCol);
                // Subtle glow boost for special titles
                float glowIntensity = specialTitle ? Settings::GlowIntensity * 1.15f : Settings::GlowIntensity;
                float glowRadius = specialTitle ? Settings::GlowRadius * 1.1f : Settings::GlowRadius;
                TextEffects::AddTextGlow(drawList, seg.font, seg.fontSize, pos,
                                         seg.displayText.c_str(), glowColor, glowRadius,
                                         glowIntensity, Settings::GlowSamples);
            }

            // Draw shadow first
            drawList->AddText(seg.font, seg.fontSize,
                              ImVec2(pos.x + Settings::MainShadowOffsetX,
                                     pos.y + Settings::MainShadowOffsetY),
                              shadowColor, seg.displayText.c_str());

            // Draw main text with appropriate styling
            // Use font-appropriate outline width
            float segOutlineWidth = seg.isLevel ? levelOutlineWidth : nameOutlineWidth;

            if (seg.isLevel)
            {
                // Level segment, apply tier-defined level effect
                // All actors use tier effects for level
                ApplyTextEffect(drawList, seg.font, seg.fontSize, pos, seg.displayText.c_str(),
                                tier.levelEffect, colLLevel, colRLevel, highlight, outlineColor, segOutlineWidth,
                                phase01, strength, textSizeScale, levelAlpha);
            }
            else
            {
                // Name segment: Different rendering for player vs NPCs
                if (d.isPlayer)
                {
                    // Apply tier-defined name effect
                    ApplyTextEffect(drawList, seg.font, seg.fontSize, pos, seg.displayText.c_str(),
                                    tier.nameEffect, colL, colR, highlight, outlineColor, segOutlineWidth,
                                    phase01, strength, textSizeScale, alpha);
                }
                else
                {
                    // NPC, simple outline with disposition color (enemy=red, friend=blue, etc.)
                    ImU32 dCol = ImGui::ColorConvertFloat4ToU32(dispoCol);
                    ImU32 npcOutline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, alpha));
                    TextEffects::AddTextOutline4(drawList, seg.font, seg.fontSize, pos, seg.displayText.c_str(), dCol, npcOutline, segOutlineWidth);
                }
            }

            // Move to next segment position
            currentPos.x += seg.size.x + segmentPadding;
        }
    }

    // Draw debug overlay with performance stats
    static void DrawDebugOverlay()
    {
        if (!Settings::EnableDebugOverlay)
            return;

        const float time = static_cast<float>(ImGui::GetTime());
        const float dt = ImGui::GetIO().DeltaTime;

        // Update frame timing stats
        DebugOverlay::UpdateFrameStats(
            s_debugStats, dt, time,
            s_lastDebugUpdateTime, s_updateCounter, s_lastUpdateCount
        );

        // Update cache stats
        s_debugStats.cacheSize = s_cache.size();

        // Build context and render
        DebugOverlay::Context ctx;
        ctx.stats = &s_debugStats;
        ctx.frameNumber = s_frame;
        ctx.postLoadCooldown = s_postLoadCooldown;
        ctx.lastReloadTime = s_lastReloadTime;
        ctx.actorCacheEntrySize = sizeof(ActorCache);
        ctx.actorDrawDataSize = sizeof(ActorDrawData);

        DebugOverlay::Render(ctx);
    }

    void Draw()
    {
        // Hot reload key detection
        if (Settings::ReloadKey > 0)
        {
            // Check if the reload key is currently pressed
            bool keyDown = (GetAsyncKeyState(Settings::ReloadKey) & 0x8000) != 0;

            // Trigger reload on key press
            if (keyDown && !s_reloadKeyWasDown)
            {
                Settings::Load();
                s_lastReloadTime = static_cast<float>(ImGui::GetTime());

                // Reset caches to apply new settings immediately
                s_cache.clear();

                // Re-apply appearance template if configured
                if (Settings::TemplateReapplyOnReload && Settings::UseTemplateAppearance)
                {
                    AppearanceTemplate::ResetAppliedFlag();
                    SKSE::GetTaskInterface()->AddTask([]() {
                        AppearanceTemplate::ApplyIfConfigured();
                    });
                }
            }
            s_reloadKeyWasDown = keyDown;
        }

        // Check if we're allowed to draw
        if (!CanDrawOverlay())
        {
            s_wasInInvalidState = true;
            return;
        }

        // If we just transitioned from invalid state, reset cooldown
        if (s_wasInInvalidState)
        {
            s_wasInInvalidState = false;
            s_postLoadCooldown = 300;  // Wait ~5 seconds before rendering
        }

        // Don't render during post-load cooldown
        // This prevents glitches during game load/fast travel transitions
        if (s_postLoadCooldown > 0)
        {
            --s_postLoadCooldown;
            return;
        }

        // Queue an update of actor data
        QueueSnapshotUpdate_RenderThread();

        // Get renderer for screen size
        auto *bsRenderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!bsRenderer)
            return;

        const auto viewSize = bsRenderer->GetScreenSize();

        // Increment frame counter
        ++s_frame;

        // Copy snapshot data under lock
        // Use static to avoid repeated allocations
        static std::vector<ActorDrawData> localSnap;
        {
            std::lock_guard<std::mutex> lock(s_snapshotLock);
            localSnap = s_snapshot;
        }

        // Nothing to draw
        if (localSnap.empty())
            return;

        // Create fullscreen overlay window
        // Positioned at (0, 0) with size matching screen resolution
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)viewSize.width, (float)viewSize.height));
        ImGui::Begin("whoisOverlay", nullptr,
                     ImGuiWindowFlags_NoBackground |               // Transparent background
                         ImGuiWindowFlags_NoDecoration |           // No title bar, borders, etc.
                         ImGuiWindowFlags_NoInputs |               // Don't capture mouse/keyboard
                         ImGuiWindowFlags_NoSavedSettings |        // Don't save window state
                         ImGuiWindowFlags_NoFocusOnAppearing |     // Don't steal focus
                         ImGuiWindowFlags_NoBringToFrontOnFocus);  // Don't reorder windows

        // Get draw list for custom rendering
        ImDrawList *drawList = ImGui::GetWindowDrawList();

        // Update debug stats before drawing
        if (Settings::EnableDebugOverlay)
        {
            s_debugStats.actorCount = static_cast<int>(localSnap.size());
            s_debugStats.visibleActors = 0;
            s_debugStats.occludedActors = 0;
            s_debugStats.playerVisible = 0;

            for (const auto &d : localSnap)
            {
                if (d.isPlayer)
                {
                    s_debugStats.playerVisible = 1;
                }
                if (d.isOccluded)
                {
                    s_debugStats.occludedActors++;
                }
                else
                {
                    s_debugStats.visibleActors++;
                }
            }
            ++s_updateCounter;
        }

        // Draw a label for each actor in the snapshot
        for (auto &d : localSnap)
        {
            DrawLabel(d, drawList);
        }

        ImGui::End();

        // Draw debug overlay
        DrawDebugOverlay();

        // Clean up cache entries for actors no longer in snapshot
        PruneCacheToSnapshot(localSnap);
    }

    void TickRT()
    {
        // Called every frame from render thread
        // Queues updates but keeps the actual work lightweight/throttled
        QueueSnapshotUpdate_RenderThread();
    }
}

// Forward declaration for appearance template check (defined in main.cpp)
extern void CheckPendingAppearanceTemplate();

// Called from Hooks.cpp after rendering
void Renderer_CheckAppearanceTemplate()
{
    CheckPendingAppearanceTemplate();
}
