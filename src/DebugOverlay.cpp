#include "DebugOverlay.hpp"

#include <imgui.h>
#include <algorithm>

namespace DebugOverlay
{
void UpdateFrameStats(Stats& stats,
                      float deltaTime,
                      float currentTime,
                      float& lastUpdateTime,
                      int updateCounter,
                      int& lastUpdateCount)
{
    // Frame times go into a circular buffer. Per-frame FPS is too jittery to read.
    constexpr int SAMPLES = RenderConstants::FRAME_TIME_SAMPLES;
    stats.frameTimeHistory[stats.frameTimeIndex] = deltaTime * 1000.0f;
    stats.frameTimeIndex = (stats.frameTimeIndex + 1) % SAMPLES;

    // Rolling average over the whole buffer.
    float sum = .0f;
    for (int i = 0; i < SAMPLES; ++i)
    {
        sum += stats.frameTimeHistory[i];
    }
    stats.avgFrameTimeMs = sum / static_cast<float>(SAMPLES);

    // deltaTime is 0 while the game is paused, so the reciprocal is guarded.
    stats.frameTimeMs = deltaTime * 1000.0f;
    stats.fps = (deltaTime > .0f) ? (1.0f / deltaTime) : .0f;

    // Difference the update counter once per second. A per-frame figure is unreadable.
    if (currentTime - lastUpdateTime >= 1.0f)
    {
        stats.updatesPerSecond = updateCounter - lastUpdateCount;
        lastUpdateCount = updateCounter;
        lastUpdateTime = currentTime;
    }
}

// Called from the render thread within an active ImGui frame.
void Render(const Context& ctx)
{
    // The caller already checks EnableDebugOverlay; only a null stats pointer is handled here.
    if (!ctx.stats)
    {
        return;
    }

    const Stats& stats = *ctx.stats;
    const float time = static_cast<float>(ImGui::GetTime());

    // Top-left corner with a small margin. Height 0 auto-sizes the window to its content.
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(.75f);  // Semi-transparent so game is visible behind

    // No window chrome. NoMove prevents dragging the window during gameplay.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("glyph Debug", nullptr, flags))
    {
        ImGui::TextColored(ImVec4(.4f, .8f, 1.0f, 1.0f), "glyph Debug");

        // Green [Reloaded!] flash after a hot reload, fading out linearly over
        // RELOAD_NOTIFICATION_DURATION seconds.
        float timeSinceReload = time - ctx.lastReloadTime;
        if (timeSinceReload < RenderConstants::RELOAD_NOTIFICATION_DURATION)
        {
            ImGui::SameLine();
            float flashAlpha =
                1.0f - timeSinceReload / RenderConstants::RELOAD_NOTIFICATION_DURATION;
            ImGui::TextColored(ImVec4(.2f, 1.0f, .2f, flashAlpha), " [Reloaded!]");
        }

        ImGui::Separator();

        // Every section title below uses this orange.
        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Performance");
        ImGui::Text("FPS: %.1f", stats.fps);
        ImGui::Text("Frame: %.2f ms", stats.frameTimeMs);
        ImGui::Text("Avg:   %.2f ms", stats.avgFrameTimeMs);

        // ASCII FPS bar, full scale at 60 FPS.
        float fpsNorm = std::clamp(stats.fps / 60.0f, .0f, 1.0f);

        ImVec4 fpsColor = (stats.fps >= 60.0f)   ? ImVec4(.2f, .9f, .2f, 1.0f)  // Green - smooth
                          : (stats.fps >= 30.0f) ? ImVec4(.9f, .9f, .2f, 1.0f)  // Yellow - playable
                                                 : ImVec4(.9f, .2f, .2f, 1.0f);  // Red - laggy

        // 20-character bar: pipes up to the current FPS, dots for the remainder.
        ImGui::TextColored(fpsColor, "[");
        ImGui::SameLine(0, 0);
        int bars = static_cast<int>(fpsNorm * 20);
        for (int i = 0; i < 20; ++i)
        {
            if (i < bars)
            {
                ImGui::TextColored(fpsColor, "|");
            }
            else
            {
                ImGui::TextColored(ImVec4(.3f, .3f, .3f, 1.0f), ".");
            }
            ImGui::SameLine(0, 0);  // No spacing between characters
        }
        ImGui::TextColored(fpsColor, "]");

        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Actors");
        ImGui::Text("Total:    %d", stats.actorCount);      // Plate-drawing actors in the snapshot
        ImGui::Text("Visible:  %d", stats.visibleActors);   // Passed visibility checks
        ImGui::Text("Occluded: %d", stats.occludedActors);  // Hidden behind geometry
        ImGui::Text("Player:   %s", stats.playerVisible ? "Yes" : "No");  // Player nameplate state

        ImGui::Spacing();

        // The actor cache persists between frames, so repeated lookups are avoided.
        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Cache");
        ImGui::Text("Entries: %zu", stats.cacheSize);  // Cached actor count
        ImGui::Text("Frame:   %u", ctx.frameNumber);   // Current render frame number

        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Updates");
        ImGui::Text("Updates/sec: %d", stats.updatesPerSecond);  // Data refreshes per second
        ImGui::Text("Cooldown:    %d",
                    ctx.postLoadCooldown);  // Frames until full processing resumes

        ImGui::Spacing();

        // Current INI settings, for quick verification.
        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Settings");
        ImGui::Text("Occlusion: %s", ctx.occlusionEnabled ? "On" : "Off");
        ImGui::Text("Glow:      %s", ctx.glowEnabled ? "On" : "Off");
        ImGui::Text("Typewriter:%s", ctx.typewriterEnabled ? "On" : "Off");
        ImGui::Text("HidePlayer:%s", ctx.hidePlayer ? "On" : "Off");
        ImGui::Text("V.Offset:  %.1f", ctx.verticalOffset);  // Nameplate height offset
        ImGui::Text("Plate cap: %d", ctx.maxPlates);
        ImGui::Text("Scan cap:  %d", ctx.maxScanActors);
        ImGui::Text("Tiers:     %zu", ctx.tierCount);  // Color tier definitions
        if (ctx.reloadKey > 0)
        {
            // Windows virtual-key code for hot reload. The shipped glyph.ini uses
            // 118 decimal, which prints as 0x76 (F7).
            ImGui::Text("Reload Key: 0x%X", ctx.reloadKey);
        }
        else
        {
            ImGui::TextColored(ImVec4(.5f, .5f, .5f, 1.0f), "Reload Key: Disabled");
        }

        ImGui::Spacing();

        // Estimates from struct sizes only. Allocator overhead is not included, but the
        // trend is enough to spot a leak.
        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Memory (Est.)");
        size_t cacheMemory = stats.cacheSize * ctx.actorCacheEntrySize;
        size_t snapshotMemory = stats.actorCount * ctx.actorDrawDataSize;
        ImGui::Text("Cache:    ~%zu bytes", cacheMemory);     // Persistent actor cache
        ImGui::Text("Snapshot: ~%zu bytes", snapshotMemory);  // Per-frame draw data
    }
    ImGui::End();
}

}  // namespace DebugOverlay
