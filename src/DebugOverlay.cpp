#include "DebugOverlay.h"
#include "Settings.h"

#include <imgui.h>
#include <algorithm>

namespace DebugOverlay
{
    void UpdateFrameStats(
        Stats& stats,
        float deltaTime,
        float currentTime,
        float& lastUpdateTime,
        int updateCounter,
        int& lastUpdateCount)
    {
        // Store current frame time in circular buffer for smoothing
        // Raw FPS values are jittery, so we average over multiple frames
        constexpr int kSamples = RenderConstants::kFrameTimeSamples;
        stats.frameTimeHistory[stats.frameTimeIndex] = deltaTime * 1000.0f;
        stats.frameTimeIndex = (stats.frameTimeIndex + 1) % kSamples;

        // Sum all samples to compute rolling average
        // This smooths out frame time spikes for a more readable display
        float sum = 0.0f;
        for (int i = 0; i < kSamples; ++i)
        {
            sum += stats.frameTimeHistory[i];
        }
        stats.avgFrameTimeMs = sum / static_cast<float>(kSamples);

        // Convert delta time to milliseconds and FPS
        // Guard against division by zero when game is paused
        stats.frameTimeMs = deltaTime * 1000.0f;
        stats.fps = (deltaTime > 0.0f) ? (1.0f / deltaTime) : 0.0f;

        // Track actor updates per second by comparing counters once per second
        // This avoids updating the display every frame which would be unreadable
        if (currentTime - lastUpdateTime >= 1.0f)
        {
            stats.updatesPerSecond = updateCounter - lastUpdateCount;
            lastUpdateCount = updateCounter;
            lastUpdateTime = currentTime;
        }
    }

    void Render(const Context& ctx)
    {
        // Early out if disabled or no stats available
        if (!Settings::EnableDebugOverlay || !ctx.stats)
            return;

        const Stats& stats = *ctx.stats;
        const float time = static_cast<float>(ImGui::GetTime());

        // Position in top-left corner with slight margin
        // Window auto-sizes to content height (height=0)
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.75f);  // Semi-transparent so game is visible behind

        // Minimal window chrome
        // NoMove prevents accidental dragging during gameplay
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoMove;

        if (ImGui::Begin("whois Debug", nullptr, flags))
        {
            // Cyan header for visual distinction
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "whois Debug");

            // Flash green "[Reloaded!]" text after hot reload
            // Fades out over kReloadNotificationDuration seconds
            float timeSinceReload = time - ctx.lastReloadTime;
            if (timeSinceReload < RenderConstants::kReloadNotificationDuration)
            {
                ImGui::SameLine();
                // Linear fade from full opacity to invisible
                float flashAlpha = 1.0f - timeSinceReload / RenderConstants::kReloadNotificationDuration;
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, flashAlpha), " [Reloaded!]");
            }

            ImGui::Separator();

            // Orange headers for section titles throughout
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Performance");
            ImGui::Text("FPS: %.1f", stats.fps);
            ImGui::Text("Frame: %.2f ms", stats.frameTimeMs);
            ImGui::Text("Avg:   %.2f ms", stats.avgFrameTimeMs);

            // ASCII-style FPS bar graph
            float fpsNorm = std::clamp(stats.fps / 60.0f, 0.0f, 1.0f);

            // Color-code by performance
            ImVec4 fpsColor = (stats.fps >= 60.0f)
                ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)       // Green - smooth
                : (stats.fps >= 30.0f)
                    ? ImVec4(0.9f, 0.9f, 0.2f, 1.0f)   // Yellow - playable
                    : ImVec4(0.9f, 0.2f, 0.2f, 1.0f);  // Red - laggy

            // Draw 20-character bar: Filled bars up to current FPS, dots for remainder
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
                    ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), ".");
                }
                ImGui::SameLine(0, 0);  // No spacing between characters
            }
            ImGui::TextColored(fpsColor, "]");

            ImGui::Spacing();

            // Shows how many NPCs are being tracked and their visibility state
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Actors");
            ImGui::Text("Total:    %d", stats.actorCount);                    // All tracked actors this frame
            ImGui::Text("Visible:  %d", stats.visibleActors);                 // Passed visibility checks
            ImGui::Text("Occluded: %d", stats.occludedActors);                // Hidden behind geometry
            ImGui::Text("Player:   %s", stats.playerVisible ? "Yes" : "No");  // Player nameplate state

            ImGui::Spacing();

            // Actor cache persists data between frames to avoid redundant lookups
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Cache");
            ImGui::Text("Entries: %zu", stats.cacheSize);  // Cached actor count
            ImGui::Text("Frame:   %u", ctx.frameNumber);   // Current render frame number

            ImGui::Spacing();

            // Tracks how often actor data is refreshed
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Updates");
            ImGui::Text("Updates/sec: %d", stats.updatesPerSecond);  // Data refreshes per second
            ImGui::Text("Cooldown:    %d", ctx.postLoadCooldown);    // Frames until full processing resumes

            ImGui::Spacing();

            // Shows current INI settings state for quick verification
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Settings");
            ImGui::Text("Occlusion: %s", Settings::EnableOcclusionCulling ? "On" : "Off");
            ImGui::Text("Glow:      %s", Settings::EnableGlow ? "On" : "Off");
            ImGui::Text("Typewriter:%s", Settings::EnableTypewriter ? "On" : "Off");
            ImGui::Text("HidePlayer:%s", Settings::HidePlayer ? "On" : "Off");
            ImGui::Text("V.Offset:  %.1f", Settings::VerticalOffset);  // Nameplate height offset
            ImGui::Text("Tiers:     %zu", Settings::Tiers.size());     // Color tier definitions
            if (Settings::ReloadKey > 0)
            {
                // Show virtual key code for hot reload (e.g., 0x74 = F5)
                ImGui::Text("Reload Key: 0x%X", Settings::ReloadKey);
            }
            else
            {
                // Grayed out when hot reload is disabled
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Reload Key: Disabled");
            }

            ImGui::Spacing();

            // Rough memory usage estimates based on struct sizes
            // Not exact due to allocator overhead, but useful for spotting leaks
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Memory (Est.)");
            size_t cacheMemory = stats.cacheSize * ctx.actorCacheEntrySize;
            size_t snapshotMemory = stats.actorCount * ctx.actorDrawDataSize;
            ImGui::Text("Cache:    ~%zu bytes", cacheMemory);     // Persistent actor cache
            ImGui::Text("Snapshot: ~%zu bytes", snapshotMemory);  // Per-frame draw data
        }
        ImGui::End();
    }

}
