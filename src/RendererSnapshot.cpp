#include "RendererInternal.hpp"

#include "GameState.hpp"
#include "GraffitoMath.hpp"
#include "HudCompat.hpp"
#include "Occlusion.hpp"

#include <SKSE/SKSE.h>

#include <chrono>

namespace Renderer
{
// ============================================================================
// Snapshot state accessor (defined here, declared in RendererInternal.hpp)
// ============================================================================

SnapshotState& GetSnapshotState()
{
    static SnapshotState instance;
    return instance;
}

// ============================================================================
// Helper functions
// ============================================================================

static RE::Actor* GetPlayer()
{
    return RE::PlayerCharacter::GetSingleton();
}

// Trim surrounding whitespace and title-case the text, ASCII only.
// Multi-byte UTF-8 codepoints pass through unchanged because std::toupper is not
// Unicode-aware and would corrupt multi-byte sequences.
std::string Capitalize(const char* text)
{
    if (!text || !*text)
    {
        return "";
    }
    std::string s = text;

    // Trim leading/trailing whitespace
    size_t first = s.find_first_not_of(" \t\r\n");
    if (std::string::npos == first)
    {
        return "";
    }
    size_t last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, (last - first + 1));

    std::string result;
    result.reserve(s.size());
    bool newWord = true;
    const char* p = s.c_str();
    while (*p)
    {
        size_t len = Utf8Utils::Utf8CharLen(p);
        if (len == 1)
        {
            unsigned char c = static_cast<unsigned char>(*p);
            if (isspace(c))
            {
                result += *p;
                newWord = true;
            }
            else if (newWord)
            {
                result += static_cast<char>(toupper(c));
                newWord = false;
            }
            else
            {
                result += *p;
            }
        }
        else
        {
            // Multi-byte UTF-8 character: append as-is
            result.append(p, len);
            newWord = false;
        }
        p += len;
    }
    return result;
}

// @author Claude (https://github.com/claude)
// Check whether an actor is a humanoid NPC rather than a creature or animal.
//
// The authoritative signal is the ActorTypeNPC keyword (Skyrim.esm 0x13794), looked
// up lazily and cached: the data handler is not guaranteed to be ready at plugin
// load, but always is by the time the renderer asks about live actors.
//
// Mods disagree on where they tag NPC-ness, so probe three places in order:
//   1. Actor instance keywords  - some scripts inject the tag at runtime.
//   2. Actor base (TESNPC)      - the most common author location.
//   3. Race keywords            - vanilla Bethesda content tags it here.
// If none match, treat as creature and let HideCreatures filter it out. If the keyword
// itself never resolves, warn once and fall back to a teammate-or-talkable heuristic.
static bool IsHumanoidNPC(RE::Actor* actor)
{
    if (!actor)
    {
        return false;
    }

    if (!GetSnapshotState().npcKeywordLookupAttempted)
    {
        GetSnapshotState().npcKeywordLookupAttempted = true;
        if (auto* dataHandler = RE::TESDataHandler::GetSingleton(); dataHandler)
        {
            GetSnapshotState().npcKeyword =
                dataHandler->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm");
        }
        if (!GetSnapshotState().npcKeyword && !GetSnapshotState().npcKeywordMissingLogged)
        {
            GetSnapshotState().npcKeywordMissingLogged = true;
            logger::warn(
                "Renderer: ActorTypeNPC keyword lookup failed, using creature filter "
                "fallback heuristic");
        }
    }

    if (GetSnapshotState().npcKeyword)
    {
        if (actor->HasKeyword(GetSnapshotState().npcKeyword))
        {
            return true;
        }
        if (auto* actorBase = actor->GetActorBase(); actorBase)
        {
            if (actorBase->HasKeyword(GetSnapshotState().npcKeyword))
            {
                return true;
            }
            if (auto* race = actorBase->GetRace(); race)
            {
                if (race->HasKeyword(GetSnapshotState().npcKeyword))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Conservative fallback if ActorTypeNPC keyword is unavailable.
    return actor->IsPlayerTeammate() || actor->CanTalkToPlayer();
}

// Map an actor-vs-player level delta to a bucket using INI thresholds.
// Strictly ordered: Weak < Strong < Deadly (validated in Settings::ClampAndValidate).
static LevelDelta ClassifyDelta(
    int actorLv, int playerLv, int weakAtOrBelow, int strongAtOrAbove, int deadlyAtOrAbove)
{
    const int delta = actorLv - playerLv;
    if (delta >= deadlyAtOrAbove)
    {
        return LevelDelta::Deadly;
    }
    if (delta >= strongAtOrAbove)
    {
        return LevelDelta::Strong;
    }
    if (delta <= weakAtOrBelow)
    {
        return LevelDelta::Weak;
    }
    return LevelDelta::Even;
}

// Coarse creature classification via `ActorTypeX` keywords.
// Probe most-specific-first because Dragon/Daedra often also carry the
// generic ActorTypeCreature tag.
static CreatureKind ClassifyCreature(RE::Actor* actor)
{
    if (!actor)
    {
        return CreatureKind::NPC;
    }
    if (actor->HasKeywordString("ActorTypeDragon"))
    {
        return CreatureKind::Dragon;
    }
    if (actor->HasKeywordString("ActorTypeDaedra"))
    {
        return CreatureKind::Daedra;
    }
    if (actor->HasKeywordString("ActorTypeUndead"))
    {
        return CreatureKind::Undead;
    }
    if (actor->HasKeywordString("ActorTypeCreature") || actor->HasKeywordString("ActorTypeAnimal"))
    {
        return CreatureKind::Beast;
    }
    return CreatureKind::NPC;
}

// Invulnerability classification for the protection badge.  Essential
// overrides Protected when both flags are set.
static ProtectionKind ClassifyProtection(RE::Actor* actor)
{
    if (!actor)
    {
        return ProtectionKind::Mortal;
    }
    if (actor->IsEssential())
    {
        return ProtectionKind::Essential;
    }
    if (actor->IsProtected())
    {
        return ProtectionKind::Protected;
    }
    return ProtectionKind::Mortal;
}

// Social-role classification for the role badge.  Guard takes priority over
// merchant; "merchant" is membership at rank 0 or above in any vendor-flagged
// faction.  The faction walk stops at the first vendor faction it finds.
static RoleKind ClassifyRole(RE::Actor* actor)
{
    if (!actor)
    {
        return RoleKind::Commoner;
    }
    if (actor->IsGuard())
    {
        return RoleKind::Guard;
    }
    bool vendor = false;
    actor->VisitFactions(
        [&vendor](RE::TESFaction* faction, std::int8_t rank)
        {
            if (faction && rank >= 0 && faction->IsVendor())
            {
                vendor = true;
                return true;  // stop visiting
            }
            return false;
        });
    return vendor ? RoleKind::Merchant : RoleKind::Commoner;
}

// Awareness classification for the engagement badge.  Combat supersedes alert.
// "Alert" is a proxy - weapon drawn while detecting the player - because no clean
// "aware of the player" API exists.
static EngagementKind ClassifyEngagement(RE::Actor* actor, RE::Actor* player)
{
    if (!actor)
    {
        return EngagementKind::Idle;
    }
    if (actor->IsInCombat())
    {
        return EngagementKind::Combat;
    }
    // IsWeaponDrawn lives on ActorState (not re-exposed on Actor in NG) --
    // reach it through AsActorState().
    if (player && actor->AsActorState()->IsWeaponDrawn() &&
        actor->RequestDetectionLevel(player) > 0)
    {
        return EngagementKind::Alert;
    }
    return EngagementKind::Idle;
}

// True when the player owes a current bounty (crime gold) to any faction.
// Reads PlayerCharacter::crimeGoldMap; "current" excludes infamy.
static bool PlayerHasBounty()
{
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc)
    {
        return false;
    }
    for (const auto& entry : pc->GetCrimeValue().crimeGoldMap)
    {
        if (entry.second.violentCur + entry.second.nonViolentCur > .0f)
        {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Honorific resolution (game-thread only)
// ============================================================================

// Runtime cache for honorific matching: resolved faction pointers, parallel to
// Settings::Honorifics() and rebuilt when the settings generation changes, plus a
// per-actor match cache refreshed on an interval, because faction ranks change on
// quest completion and not per frame.
struct HonorificRuntime
{
    uint32_t settingsGen = 0;               // Settings generation of `factions`
    std::vector<RE::TESFaction*> factions;  // Parallel to Settings::Honorifics()

    struct CacheEntry
    {
        uint32_t lastCheckFrame = 0;  // Snapshot frame of the last match
        int index = -1;               // Matched honorific index (-1 = none)
    };
    std::unordered_map<uint32_t, CacheEntry> perActor;  // keyed by formID
};

static HonorificRuntime& GetHonorificRuntime()
{
    static HonorificRuntime instance;
    return instance;
}

// Parse "0xFORMID" or "0xFORMID@Plugin.esp" (plugin defaults to Skyrim.esm)
// and resolve the faction against the load order.
static RE::TESFaction* ResolveFactionSpec(const std::string& spec)
{
    if (spec.empty())
    {
        return nullptr;
    }

    std::string idPart = spec;
    std::string plugin = "Skyrim.esm";
    if (const size_t at = spec.find('@'); at != std::string::npos)
    {
        idPart = spec.substr(0, at);
        plugin = spec.substr(at + 1);
    }

    const uint32_t rawID = static_cast<uint32_t>(std::strtoul(idPart.c_str(), nullptr, 16));
    if (rawID == 0)
    {
        return nullptr;
    }

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler)
    {
        return nullptr;
    }
    return dataHandler->LookupForm<RE::TESFaction>(rawID, plugin);
}

// Rebuild the resolved-faction table when settings change (load, F7 reload).
static void RefreshHonorificRuntime()
{
    auto& rt = GetHonorificRuntime();
    const uint32_t gen = Settings::Generation().load(std::memory_order_acquire);
    if (rt.settingsGen == gen)
    {
        return;
    }
    rt.settingsGen = gen;
    rt.perActor.clear();
    rt.factions.clear();

    const auto& defs = Settings::Honorifics();
    rt.factions.reserve(defs.size());
    for (const auto& defn : defs)
    {
        RE::TESFaction* faction = ResolveFactionSpec(defn.factionSpec);
        if (!faction && !defn.factionSpec.empty())
        {
            logger::warn("Honorific: faction '{}' did not resolve (title '{}')",
                         defn.factionSpec,
                         defn.title);
        }
        rt.factions.push_back(faction);
    }
}

// Snapshot updates between per-actor honorific refreshes.
static constexpr uint32_t HONORIFIC_REFRESH_FRAMES = 90;

// Resolve the highest-priority honorific an actor has earned, or empty.
// One faction walk per refresh; matches are cached per formID. lastCheckFrame == 0
// means never checked: the snapshot counter is pre-incremented, so 0 is never a valid
// frame value. Highest priority wins. On a priority tie the first match found wins,
// because the comparison is strictly greater-than: inside one faction that is the
// lower Honorifics() index, but between two factions it is whichever faction
// VisitFactions reaches first, which is not index order.
static std::string ResolveHonorific(RE::Actor* actor, bool isPlayer, uint32_t snapshotFrame)
{
    const auto& defs = Settings::Honorifics();
    if (defs.empty() || !actor)
    {
        return {};
    }

    auto& rt = GetHonorificRuntime();
    auto& entry = rt.perActor[actor->GetFormID()];
    const uint32_t framesSince = snapshotFrame - entry.lastCheckFrame;
    if (entry.lastCheckFrame == 0 || framesSince >= HONORIFIC_REFRESH_FRAMES)
    {
        entry.lastCheckFrame = snapshotFrame;
        entry.index = -1;
        int bestPriority = (std::numeric_limits<int>::min)();
        actor->VisitFactions(
            [&](RE::TESFaction* faction, std::int8_t rank)
            {
                if (!faction || rank < 0)
                {
                    return false;
                }
                for (size_t i = 0; i < defs.size(); ++i)
                {
                    if (rt.factions[i] != faction)
                    {
                        continue;
                    }
                    const auto& defn = defs[i];
                    if (defn.title.empty() || rank < defn.minRank ||
                        (defn.playerOnly && !isPlayer) || (defn.npcOnly && isPlayer))
                    {
                        continue;
                    }
                    if (entry.index < 0 || defn.priority > bestPriority)
                    {
                        entry.index = static_cast<int>(i);
                        bestPriority = defn.priority;
                    }
                }
                return false;  // keep visiting
            });
    }

    return entry.index >= 0 ? defs[static_cast<size_t>(entry.index)].title : std::string{};
}

// ============================================================================
// Scene-context evaluation for [RegisterN] selection (game-thread only)
// ============================================================================

// Compute the current scene-context predicate mask.  All predicates read
// game state, so this runs on the game thread once per snapshot.
//
// The predicate definitions are fixed, not configurable: Night is game-clock hour
// < 6 or >= 20, City is a current location carrying LocTypeCity or LocTypeTown, and
// Dialogue is a live MenuTopicManager speaker.  Only the Crowded plate threshold is
// an INI value (RegisterConfig().CrowdedThreshold).
static uint32_t ComputeContextMask(RE::Actor* player, int visiblePlateCount)
{
    uint32_t mask = 0;

    if (auto* cell = player->GetParentCell(); cell && cell->IsInteriorCell())
    {
        mask |= Settings::Context::Interior;
    }

    if (auto* calendar = RE::Calendar::GetSingleton())
    {
        const float hour = calendar->GetHour();
        if (hour < 6.0f || hour >= 20.0f)
        {
            mask |= Settings::Context::Night;
        }
    }

    if (auto* location = player->GetCurrentLocation())
    {
        if (location->HasKeywordString("LocTypeCity") || location->HasKeywordString("LocTypeTown"))
        {
            mask |= Settings::Context::City;
        }
    }

    if (player->IsSneaking())
    {
        mask |= Settings::Context::Sneaking;
    }

    if (auto* topicManager = RE::MenuTopicManager::GetSingleton();
        topicManager && topicManager->speaker.get())
    {
        mask |= Settings::Context::Dialogue;
    }

    if (visiblePlateCount >= Settings::RegisterConfig().CrowdedThreshold)
    {
        mask |= Settings::Context::Crowded;
    }

    return mask;
}

// Pick the highest-priority register matching the context mask, or -1.
// An empty When (both masks zero) matches every scene: a base register.
// Entries backfilled to close an index gap are unconfigured and never match. Without
// that guard a phantom [Register0] would shadow the user's real register on a
// priority tie, because the lower index wins a tie.
static int PickRegister(uint32_t ctxMask)
{
    const auto& regs = Settings::Registers();
    int best = -1;
    int bestPriority = (std::numeric_limits<int>::min)();
    for (size_t i = 0; i < regs.size(); ++i)
    {
        const auto& r = regs[i];
        if (!r.configured || (ctxMask & r.whenMask) != r.whenMask || (ctxMask & r.whenNotMask) != 0)
        {
            continue;
        }
        if (best < 0 || r.priority > bestPriority)
        {
            best = static_cast<int>(i);
            bestPriority = r.priority;
        }
    }
    return best;
}

// Check occlusion for an actor, reusing the game-thread-local cached result.
// checkInterval counts snapshot updates, not render frames, so the line-of-sight
// refresh rate follows the game-thread task cadence and not the display frame rate.
// lastCheckFrame == 0 means never checked: the snapshot counter is pre-incremented,
// so 0 is never a valid frame value, and the counter resets to 0 whenever the
// occlusion cache is cleared. The cache is pruned against seenFormIDs after the
// actor pass.
static void UpdateOcclusionForActor(ActorDrawData& d,
                                    RE::Actor* a,
                                    RE::Actor* player,
                                    uint32_t snapshotFrame,
                                    uint32_t checkInterval)
{
    auto& entry = GetOcclusionCache()[d.formID];
    if (entry.lastCheckFrame != 0)
    {
        const uint32_t framesSince = snapshotFrame - entry.lastCheckFrame;
        if (framesSince < checkInterval)
        {
            d.isOccluded = entry.cachedOccluded;
            return;
        }
    }

    // Perform fresh occlusion check using nameplate world position
    d.isOccluded = Occlusion::IsActorOccluded(a, player, d.worldPos, Settings::Occlusion().Enabled);
    entry.lastCheckFrame = snapshotFrame;
    entry.cachedOccluded = d.isOccluded;
}

static double PoseClockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The player plate anchors to the full rendered-head position. Taking only the head
// node's Z and keeping Actor::GetPosition X/Y steps the self-plate at the
// simulation-root cadence while the skeleton moves smoothly, so the player must take
// all three axes from the node. NPC plates keep the stable root X/Y. Both paths fall
// back to a height-derived anchor (feet + Actor::GetHeight) when 3D is absent or the
// head node is implausible, notably for the first-person player skeleton.
//
// "Implausible" means any non-finite component, a head that is not above the feet, a
// head 512 units or more above them, or a head 256 units or more away from them
// laterally. All distances are Skyrim world units.
//
//   head node found and plausible        no 3D, or head rejected
//   ------------------------------       -----------------------
//   z = head.z + 12 + VerticalOffset     z = feet.z + height + VerticalOffset
//   x/y = head x/y  (player)             x/y = feet x/y
//   x/y = feet x/y  (NPC)
//
// height is Actor::GetHeight(), replaced by 128 units when it is not finite or not
// above 1. The 12-unit headroom clears the crown, because the node sits at the head
// base. VerticalOffset is read live from Settings, not from the render snapshot.
static RE::NiPoint3 ComputeAnchorPosition(RE::Actor* a,
                                          const RE::NiPoint3& feet,
                                          bool useRenderedHeadXY)
{
    if (auto* root = a->Get3D())
    {
        if (auto* head = root->GetObjectByName("NPC Head [Head]"))
        {
            RE::NiPoint3 anchor = head->world.translate;
            const float dx = anchor.x - feet.x;
            const float dy = anchor.y - feet.y;
            const float dz = anchor.z - feet.z;
            constexpr float kMaxHeadOffset = 256.0f;
            if (std::isfinite(anchor.x) && std::isfinite(anchor.y) && std::isfinite(anchor.z) &&
                dz > .0f && dz < 512.0f && dx * dx + dy * dy < kMaxHeadOffset * kMaxHeadOffset)
            {
                if (!useRenderedHeadXY)
                {
                    // NPC plates keep the stable actor-root X/Y; only the player
                    // self-plate needs render-interpolated lateral motion.
                    anchor.x = feet.x;
                    anchor.y = feet.y;
                }
                constexpr float kHeadHeadroom = 12.0f;  // node sits at head base; clear the crown
                anchor.z += kHeadHeadroom + Settings::Display().VerticalOffset;
                return anchor;
            }
        }
    }

    RE::NiPoint3 anchor = feet;
    const float rawHeight = a->GetHeight();
    const float height = std::isfinite(rawHeight) && rawHeight > 1.0f ? rawHeight : 128.0f;
    anchor.z += height + Settings::Display().VerticalOffset;
    return anchor;
}

static void CaptureActorPose(RE::Actor* actor, ActorDrawData& out, bool useRenderedHeadXY = false)
{
    out.feetPos = actor->GetPosition();
    out.worldPos = ComputeAnchorPosition(actor, out.feetPos, useRenderedHeadXY);
    out.poseSampleTime = PoseClockSeconds();
}

// Capture a render-thread-safe targeting sphere while actor 3D is safe to inspect on
// the game thread. The rendered root bound handles differently proportioned
// creatures; the height-derived sphere keeps actors with an unloaded or invalid root
// targetable without carrying an RE object across threads.
static void CaptureActorRaycastBounds(RE::Actor* actor, RE::NiPoint3& outCenter, float& outRadius)
{
    constexpr float MIN_BOUND_RADIUS = 4.0f;
    constexpr float MAX_BOUND_RADIUS = 4096.0f;
    if (actor)
    {
        if (auto* root = actor->Get3D())
        {
            const auto& bound = root->worldBound;
            if (std::isfinite(bound.center.x) && std::isfinite(bound.center.y) &&
                std::isfinite(bound.center.z) && std::isfinite(bound.radius) &&
                bound.radius >= MIN_BOUND_RADIUS && bound.radius <= MAX_BOUND_RADIUS)
            {
                outCenter = bound.center;
                outRadius = bound.radius;
                return;
            }
        }

        const float rawHeight = actor->GetHeight();
        const float height = std::isfinite(rawHeight) && rawHeight > 1.0f ? rawHeight : 128.0f;
        outCenter = actor->GetPosition();
        outCenter.z += height * .5f;
        outRadius = std::clamp(height * .5f, 18.0f, MAX_BOUND_RADIUS);
        return;
    }

    outCenter = {};
    outRadius = .0f;
}

// @author Claude (https://github.com/claude)
// Game thread ONLY, scheduled through SKSE::GetTaskInterface(). CommonLibSSE's RE::*
// types (ProcessLists, Actor, TESDataHandler) are not render-thread safe.
//
// The render thread consumes a self-contained std::vector<ActorDrawData> snapshot
// under snapshotLock and otherwise stays off actor and gameplay RE::*. The snapshot
// holds no engine pointer: every field is a plain value, RE::NiPoint3 included, so
// nothing in it can be dereferenced after the game thread moves on. The one exception
// to the rule is a lock-free world-camera read for projection and camera-forward
// targeting, where a torn read is a benign one-frame glitch.
//
// ActorDrawData is not trivially copyable: it owns std::string members, so the
// per-frame snapshot copy in Draw() deep-copies every name and honorific.
//
// Everything else the renderer needs (FormID, name, bounds, world position, level,
// relationship, occlusion result) is precomputed here, so the render-thread copy
// needs no further game-thread trips.
//
// Pipeline. The scan is split in two so the expensive derivation runs only for the
// actors that will get a plate:
//
//   gates      pauseSnapshotUpdates -> clear both gates, snapshot, target, return
//              neither overlay nor Deck allowed -> clear snapshot and target, return
//              no player or no ProcessLists -> clear allowDeck, snapshot, target, return
//                |
//   player     self-plate DTO: name, pose, sneak/combat/encumbered/bounty, honorific
//                |
//   cheap pass  every highActorHandles entry, capped by MaxScanActors. Applies the
//              corpse, HideCreatures and MaxScanDistance gates and captures the
//              targeting sphere. No name, relationship, badge or occlusion work.
//                |
//   retain     inject the Deck crosshair target if the plate filters dropped it,
//              pick the Graffito camera-ray target, then partial_sort the keep set:
//              camera-ray target first, Deck target second, nearest first after that
//                |
//   full pass  per kept actor: name, relationship, level delta, creature kind, role,
//              protection, engagement, honorific, HUD-compat yield, occlusion. A
//              corpse stops after the bare marker fields.
//                |
//   publish    prune the occlusion and honorific caches to the actors just seen,
//              pick the active register, re-sample the player pose last, then swap
//              snapshot and crosshairTarget under snapshotLock
void UpdateSnapshot_GameThread()
{
    // Clears the in-flight flags on every exit path from this task.
    struct UpdateScope
    {
        UpdateScope() { GetState().snapshotUpdateRunning.store(true, std::memory_order_release); }

        ~UpdateScope()
        {
            GetState().snapshotUpdateRunning.store(false, std::memory_order_release);
            GetState().updateQueued.store(false, std::memory_order_release);
        }
    } _;

    if (GetState().pauseSnapshotUpdates.load(std::memory_order_acquire))
    {
        GetState().allowOverlay.store(false, std::memory_order_release);
        GetState().allowDeck.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot.clear();
        GetState().crosshairTarget = 0;
        return;
    }

    if (GetState().clearOcclusionCacheRequested.exchange(false, std::memory_order_acq_rel))
    {
        GetOcclusionCache().clear();
        GetSnapshotState().frame = 0;
    }

    // Nameplates are suppressed in combat, but Deck supports portraits of actors in
    // combat. Actor facts keep publishing while the lighter Deck world gate is open,
    // so an F8 capture never depends on label visibility.
    bool deckEnabled = false;
    {
        const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
        deckEnabled = Settings::Deck().Enabled;
    }
    const bool allow = GameState::CanDrawOverlay();
    const bool allowDeck = deckEnabled && GameState::CanCaptureDeck();
    GetState().allowOverlay.store(allow, std::memory_order_release);
    GetState().allowDeck.store(allowDeck, std::memory_order_release);

    if (!allow && !allowDeck)
    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot.clear();
        GetState().crosshairTarget = 0;
        return;
    }

    auto* player = GetPlayer();
    auto* pl = RE::ProcessLists::GetSingleton();
    if (!player || !pl)
    {
        GetState().allowDeck.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot.clear();
        GetState().crosshairTarget = 0;
        return;
    }

    const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
    RefreshHonorificRuntime();
    const auto actorLimits = RenderConstants::ClampActorLimits(Settings::Display().MaxPlates,
                                                               Settings::Display().MaxScanActors);
    const int maxPlates = actorLimits.maxPlates;
    const int maxScanActors = actorLimits.maxScanActors;
    const float kMaxDistSq =
        Settings::Distance().MaxScanDistance * Settings::Distance().MaxScanDistance;
    const uint32_t checkInterval =
        static_cast<uint32_t>(std::max(1, Settings::Occlusion().CheckInterval));
    const uint32_t snapshotFrame = ++GetSnapshotState().frame;

    // Snapshot level-delta thresholds once; used for every actor classification.
    const auto& labelSettings = Settings::Labels();
    const int playerLevel = static_cast<int>(player->GetLevel());
    const int weakBelow = labelSettings.WeakAtOrBelow;
    const int strongAbove = labelSettings.StrongAtOrAbove;
    const int deadlyAbove = labelSettings.DeadlyAtOrAbove;

    std::vector<ActorDrawData> tempBuf;
    tempBuf.reserve(static_cast<size_t>(maxPlates) + 2);
    std::unordered_set<uint32_t> seenFormIDs;
    seenFormIDs.reserve(static_cast<size_t>(maxPlates) + 2);
    // Scan candidate: the cheap distance pass collects only what it needs to rank
    // actors by proximity. The expensive per-actor derivation (name, relationship,
    // badges, honorific, occlusion) is deferred to the nearest maxPlates below, so it
    // never runs for an actor that will not get a plate.
    struct NearActor
    {
        RE::Actor* actor{nullptr};
        float distSq{.0f};
        bool dead{false};
        bool plateEligible{true};
        RE::NiPoint3 raycastCenter{};
        float raycastRadius{.0f};
    };
    std::vector<NearActor> nearActors;
    nearActors.reserve(static_cast<size_t>(maxScanActors) + 1);

    const auto playerPos = player->GetPosition();

    // Which actors do other HUD mods already cover?
    const bool yieldToTrueHUD = Settings::Compat().YieldToTrueHUD && HudCompat::HasTrueHUD();
    const bool yieldToMoreHUD = Settings::Compat().YieldLevelToMoreHUD && HudCompat::HasMoreHUD();
    const bool graffitoEnabled = Settings::Graffito().Enabled;
    const bool playerPlateVisible = !Settings::Display().HidePlayer || graffitoEnabled;
    const uint32_t crosshairID =
        (yieldToMoreHUD || deckEnabled) ? HudCompat::CrosshairTargetFormID() : 0;

    // Graffito always keeps a readable self-plate. Deck also retains a private
    // player DTO when the ordinary billboard setting hides it.
    if (playerPlateVisible || deckEnabled)
    {
        ActorDrawData d;
        d.formID = player->GetFormID();
        d.level = player->GetLevel();
        // Prefer the actor-base (TESNPC) full name for the player: a RaceMenu rename
        // writes the base name immediately, while GetDisplayFullName() can keep
        // serving a stale ExtraTextDisplayData string from the player reference until
        // a save and reload rebuilds it, which would drop in-session renames.
        const char* rawName = nullptr;
        if (auto* base = player->GetActorBase())
        {
            rawName = base->GetFullName();
        }
        if (!rawName || !*rawName)
        {
            rawName = player->GetDisplayFullName();
        }
        d.name = (rawName && *rawName) ? Capitalize(rawName) : "Player";
        CaptureActorPose(player, d, true);
        CaptureActorRaycastBounds(player, d.raycastCenter, d.raycastRadius);
        d.headingRadians = player->GetAngleZ();
        d.distToPlayer = .0f;
        d.isPlayer = true;
        d.isUnique = true;
        // The player has no meaningful relation to itself, so these are stable
        // defaults. They still feed the %r/%d/%c text tokens; the player's badge set
        // is a separate branch in ComposeBadges, driven by the facts below.
        d.relationship = RelationshipKind::Follower;
        d.levelDelta = LevelDelta::Even;
        d.creatureKind = CreatureKind::NPC;

        // Player status badge facts (always-on player slots).  The detection scan
        // runs only while sneaking, so it costs nothing otherwise.
        const bool sneaking = player->IsSneaking();
        bool detected = false;
        if (sneaking)
        {
            for (auto& handle : pl->highActorHandles)
            {
                auto otherSP = handle.get();
                auto* other = otherSP.get();
                if (!other || other == player || other->IsDead())
                {
                    continue;
                }
                if (other->IsHostileToActor(player) && other->RequestDetectionLevel(player) > 0)
                {
                    detected = true;
                    break;
                }
            }
        }
        d.sneak = sneaking ? (detected ? SneakKind::Detected : SneakKind::Hidden) : SneakKind::Off;
        d.playerInCombat = player->IsInCombat();
        d.encumbered = player->IsOverEncumbered();
        d.wanted = PlayerHasBounty();
        d.honorific = ResolveHonorific(player, true, snapshotFrame);

        tempBuf.push_back(std::move(d));
        seenFormIDs.insert(player->GetFormID());
    }

    // A player retained only for Deck does not consume an NPC plate slot.
    const int added = playerPlateVisible ? static_cast<int>(tempBuf.size()) : 0;

    // Cheap pass: distance-filter EVERY high-process actor. Only proximity ranking
    // data is gathered here - no name, relationship, badge or occlusion work - so
    // iterating the whole list is negligible. Iteration must not be cut short by list
    // POSITION: in a crowded area (a long highActorHandles list, for example after
    // passing many NPCs) a near actor sitting past such a cutoff would never be
    // examined and would get no plate. highActorHandles is bounded by the engine's
    // high-process budget; MaxScanActors is a configurable runaway guard that should
    // not trip in normal play.
    int scanned = 0;
    for (auto& h : pl->highActorHandles)
    {
        if (++scanned > maxScanActors)
        {
            break;
        }
        auto aSP = h.get();
        auto* a = aSP.get();
        if (!a || a == player)
        {
            continue;
        }

        // Dead actors stay in the snapshot as bare death markers: the render thread
        // plays a one-shot exit animation for a corpse it saw alive, replaying that
        // actor's last live facts, and ignores every other corpse.
        const bool dead = a->IsDead();
        if (dead && !Settings::DeathRite().Enabled)
        {
            continue;
        }

        // Skip creatures/animals if HideCreatures is enabled.
        if (Settings::Display().HideCreatures && !IsHumanoidNPC(a))
        {
            continue;
        }

        const float distSq = playerPos.GetSquaredDistance(a->GetPosition());
        if (distSq > kMaxDistSq)
        {
            continue;
        }

        NearActor candidate{a, distSq, dead};
        CaptureActorRaycastBounds(a, candidate.raycastCenter, candidate.raycastRadius);
        nearActors.push_back(candidate);
    }

    // Nameplate filters are not Deck filters. If the aimed actor was excluded
    // above (for example, a hidden creature or an actor beyond plate range),
    // inject one private candidate without changing the ambient plate set.
    bool targetInjectedForDeck = false;
    if (deckEnabled && crosshairID != 0 && crosshairID != player->GetFormID())
    {
        const bool targetAlreadyPresent =
            std::any_of(nearActors.begin(),
                        nearActors.end(),
                        [crosshairID](const NearActor& candidate)
                        {
                            return !candidate.dead && candidate.actor &&
                                   candidate.actor->GetFormID() == crosshairID;
                        });
        if (!targetAlreadyPresent)
        {
            if (auto* target = RE::TESForm::LookupByID<RE::Actor>(crosshairID);
                target && !target->IsDead())
            {
                const float distSq = playerPos.GetSquaredDistance(target->GetPosition());
                NearActor candidate{target, distSq, false, false};
                CaptureActorRaycastBounds(target, candidate.raycastCenter, candidate.raycastRadius);
                nearActors.push_back(candidate);
                targetInjectedForDeck = true;
            }
        }
    }

    // Graffito targeting is independent of CrosshairPickData. Cast the ray
    // perpendicular to the camera plane (camera forward) through every
    // nameplate-eligible actor bound so an aimed actor is retained even when
    // it would otherwise fall outside the nearest-actor budget.
    uint32_t cameraRayID = 0;
    if (graffitoEnabled)
    {
        RE::NiPoint3 cameraPosition{};
        RE::NiPoint3 cameraForward{};
        if (Occlusion::GetCameraInfo(cameraPosition, cameraForward))
        {
            const auto toMath = [](const RE::NiPoint3& point)
            {
                return Graffito::Math::Vec3{static_cast<double>(point.x),
                                            static_cast<double>(point.y),
                                            static_cast<double>(point.z)};
            };
            const float maxDistance = Settings::Graffito().MaxDistance;
            const float maxDistanceSq = maxDistance * maxDistance;
            double bestHit = std::numeric_limits<double>::infinity();
            for (const auto& candidate : nearActors)
            {
                if (!candidate.actor || candidate.dead || !candidate.plateEligible ||
                    candidate.raycastRadius <= .0f ||
                    (maxDistance > .0f && candidate.distSq > maxDistanceSq))
                {
                    continue;
                }

                const double hit = Graffito::Math::RaySphereHitDistance(
                    toMath(cameraPosition),
                    toMath(cameraForward),
                    toMath(candidate.raycastCenter),
                    static_cast<double>(candidate.raycastRadius));
                const uint32_t formID = candidate.actor->GetFormID();
                if (hit < bestHit || (hit == bestHit && formID < cameraRayID))
                {
                    bestHit = hit;
                    cameraRayID = formID;
                }
            }
        }
    }

    // Keep the nearest (maxPlates - added) in-range actors. If Deck's exact
    // crosshair target falls outside that set, retain it as one extra private
    // DTO so targeting never displaces an ambient nameplate. The camera-ray
    // target itself receives first priority inside the ordinary plate budget.
    const int remainingSlots = std::max(0, maxPlates - added);
    bool targetNeedsExtraSlot = targetInjectedForDeck;
    if (deckEnabled && crosshairID != 0 && !targetInjectedForDeck)
    {
        const auto targetIt = std::find_if(nearActors.begin(),
                                           nearActors.end(),
                                           [crosshairID](const NearActor& candidate)
                                           {
                                               return !candidate.dead && candidate.actor &&
                                                      candidate.actor->GetFormID() == crosshairID;
                                           });
        if (targetIt != nearActors.end())
        {
            const auto closerCount = std::count_if(nearActors.begin(),
                                                   nearActors.end(),
                                                   [targetIt](const NearActor& candidate)
                                                   { return candidate.distSq < targetIt->distSq; });
            bool cameraRayDisplacesTarget = false;
            if (cameraRayID != 0 && cameraRayID != crosshairID)
            {
                const auto cameraRayIt = std::find_if(
                    nearActors.begin(),
                    nearActors.end(),
                    [cameraRayID](const NearActor& candidate)
                    { return candidate.actor && candidate.actor->GetFormID() == cameraRayID; });
                cameraRayDisplacesTarget =
                    cameraRayIt != nearActors.end() && cameraRayIt->distSq >= targetIt->distSq;
            }
            targetNeedsExtraSlot =
                closerCount + (cameraRayDisplacesTarget ? 1 : 0) >= remainingSlots;
        }
    }
    const int slotsToKeep = remainingSlots + (targetNeedsExtraSlot ? 1 : 0);
    const size_t keep = std::min(static_cast<size_t>(slotsToKeep), nearActors.size());
    std::partial_sort(
        nearActors.begin(),
        nearActors.begin() + static_cast<std::ptrdiff_t>(keep),
        nearActors.end(),
        [cameraRayID, crosshairID, targetNeedsExtraSlot](const NearActor& lhs, const NearActor& rhs)
        {
            const bool lhsCameraRay =
                cameraRayID != 0 && lhs.actor && lhs.actor->GetFormID() == cameraRayID;
            const bool rhsCameraRay =
                cameraRayID != 0 && rhs.actor && rhs.actor->GetFormID() == cameraRayID;
            if (lhsCameraRay != rhsCameraRay)
            {
                return lhsCameraRay;
            }
            if (targetNeedsExtraSlot)
            {
                const bool lhsTarget =
                    !lhs.dead && lhs.actor && lhs.actor->GetFormID() == crosshairID;
                const bool rhsTarget =
                    !rhs.dead && rhs.actor && rhs.actor->GetFormID() == crosshairID;
                if (lhsTarget != rhsTarget)
                {
                    return lhsTarget;
                }
            }
            return lhs.distSq < rhs.distSq;
        });

    // Expensive pass: full per-actor derivation only for the plates we will show.
    for (size_t i = 0; i < keep; ++i)
    {
        RE::Actor* a = nearActors[i].actor;
        const bool dead = nearActors[i].dead;

        ActorDrawData d;
        d.formID = a->GetFormID();
        d.level = a->GetLevel();
        const char* rawName = a->GetDisplayFullName();
        d.name = rawName ? Capitalize(rawName) : "";
        CaptureActorPose(a, d);
        d.raycastCenter = nearActors[i].raycastCenter;
        d.raycastRadius = nearActors[i].raycastRadius;
        d.headingRadians = a->GetAngleZ();
        d.distToPlayer = std::sqrt(nearActors[i].distSq);
        d.isPlayer = false;
        d.deckOnly = targetNeedsExtraSlot && d.formID == crosshairID && d.formID != cameraRayID;
        if (const auto* actorBase = a->GetActorBase())
        {
            d.isUnique = actorBase->IsUnique();
        }
        d.isDead = dead;
        if (dead)
        {
            // The exit animation renders from the last live draw data, so contextual
            // facts (relationship, badges, occlusion) are not re-derived for a
            // corpse: a hostile's name must not flip to neutral part-way through.
            seenFormIDs.insert(d.formID);
            tempBuf.push_back(std::move(d));
            continue;
        }

        // Relationship derivation - drives the %r token, badges, and NPC text color.
        const bool hostile = a->IsHostileToActor(player);
        const bool teammate = a->IsPlayerTeammate();
        const bool canTalk = !hostile && !teammate && a->CanTalkToPlayer();
        d.relationship = hostile    ? RelationshipKind::Hostile
                         : teammate ? RelationshipKind::Follower
                         : canTalk  ? RelationshipKind::Ally
                                    : RelationshipKind::Neutral;
        d.levelDelta = ClassifyDelta(
            static_cast<int>(d.level), playerLevel, weakBelow, strongAbove, deadlyAbove);
        d.creatureKind = ClassifyCreature(a);

        // Status badge facts (always-on NPC slots).
        d.protection = ClassifyProtection(a);
        d.role = ClassifyRole(a);
        d.engagement = ClassifyEngagement(a, player);

        d.honorific = ResolveHonorific(a, false, snapshotFrame);

        // Yield to a HUD mod that already covers this actor.
        d.yieldPlate = yieldToTrueHUD && HudCompat::TrueHUDShowsBarFor(a);
        d.yieldLevel = yieldToMoreHUD && d.formID == crosshairID;

        if (Settings::Occlusion().Enabled)
        {
            UpdateOcclusionForActor(d, a, player, snapshotFrame, checkInterval);
        }

        seenFormIDs.insert(d.formID);
        tempBuf.push_back(std::move(d));
    }

    for (auto it = GetOcclusionCache().begin(); it != GetOcclusionCache().end();)
    {
        if (seenFormIDs.find(it->first) == seenFormIDs.end())
        {
            it = GetOcclusionCache().erase(it);
        }
        else
        {
            ++it;
        }
    }

    auto& honorificCache = GetHonorificRuntime().perActor;
    for (auto it = honorificCache.begin(); it != honorificCache.end();)
    {
        if (seenFormIDs.find(it->first) == seenFormIDs.end())
        {
            it = honorificCache.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Evaluate the [RegisterN] scene predicates against the plate count just
    // gathered, then publish the active profile for the render thread to ease toward.
    // Crowded must count only actors that actually get a nameplate, so the private
    // Deck DTOs are subtracted: every deckOnly actor, plus the player entry when it
    // was retained for Deck alone.
    int activeRegister = -1;
    if (Settings::RegisterConfig().Enabled && !Settings::Registers().empty())
    {
        const int privateDeckActors = static_cast<int>(std::count_if(
            tempBuf.begin(), tempBuf.end(), [](const ActorDrawData& d) { return d.deckOnly; }));
        const int visiblePlateCount =
            static_cast<int>(tempBuf.size()) - privateDeckActors -
            ((deckEnabled && !playerPlateVisible && !tempBuf.empty()) ? 1 : 0);
        activeRegister = PickRegister(ComputeContextMask(player, visiblePlateCount));
    }
    GetState().activeRegister.store(activeRegister, std::memory_order_release);

    // Relationship queries, faction walks, and occlusion casts make the NPC scan
    // slow enough to matter. Refresh the player immediately before publication so
    // its self-plate does not inherit that work as visible motion latency.
    if (auto playerData = std::find_if(tempBuf.begin(),
                                       tempBuf.end(),
                                       [](const ActorDrawData& actor) { return actor.isPlayer; });
        playerData != tempBuf.end())
    {
        CaptureActorPose(player, *playerData, true);
    }

    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot = std::move(tempBuf);
        GetState().crosshairTarget = crosshairID;
    }
}

void QueueSnapshotUpdate_RenderThread()
{
    if (GetState().pauseSnapshotUpdates.load(std::memory_order_acquire))
    {
        return;
    }

    // exchange returns true when an update is already pending; coalesce so at most
    // one update is ever in flight.
    if (GetState().updateQueued.exchange(true))
    {
        return;
    }

    // Actors cannot be iterated from the render thread, so the update runs on the
    // game thread through SKSE's task interface.
    if (auto* task = SKSE::GetTaskInterface())
    {
        task->AddTask([]() { UpdateSnapshot_GameThread(); });
    }
    else
    {
        // Task interface not available, clear the flag
        GetState().updateQueued.store(false);
    }
}

}  // namespace Renderer
