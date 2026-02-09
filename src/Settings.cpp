#include "Settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace Settings
{
    std::string TitleFormat;
    std::vector<Segment> DisplayFormat;

    // Tier Definitions
    std::vector<TierDefinition> Tiers;

    // Special Titles
    std::vector<SpecialTitleDefinition> SpecialTitles;

    // Distance & Visibility
    float FadeStartDistance;
    float FadeEndDistance;
    float ScaleStartDistance;
    float ScaleEndDistance;
    float MinimumScale;
    float MaxScanDistance;

    // Occlusion Settings
    bool  EnableOcclusionCulling = true;
    float OcclusionSettleTime = 0.58f;
    int   OcclusionCheckInterval = 3;

    // Visual Effects
    float TitleShadowOffsetX;
    float TitleShadowOffsetY;
    float MainShadowOffsetX;
    float MainShadowOffsetY;
    float SegmentPadding;
    
    // Outline Settings
    float OutlineWidthMin;
    float OutlineWidthMax;
    bool  FastOutlines = false;

    // Glow Settings
    bool  EnableGlow = false;
    float GlowRadius = 4.0f;
    float GlowIntensity = 0.5f;
    int   GlowSamples = 8;

    // Typewriter Settings
    bool  EnableTypewriter = false;
    float TypewriterSpeed = 30.0f;
    float TypewriterDelay = 0.0f;

    // Debug Settings
    bool  EnableDebugOverlay = false;

    // Side Ornaments
    bool  EnableOrnaments = true;
    float OrnamentScale = 1.0f;
    float OrnamentSpacing = 3.0f;

    // Particle Aura
    bool  EnableParticleAura = true;
    bool  UseParticleTextures = true;
    bool  EnableStars = true;
    bool  EnableSparks = false;
    bool  EnableWisps = false;
    bool  EnableRunes = false;
    bool  EnableOrbs = false;
    int   ParticleCount = 8;
    float ParticleSize = 3.0f;
    float ParticleSpeed = 1.0f;
    float ParticleSpread = 20.0f;
    float ParticleAlpha = 0.8f;
    // Textures loaded from subfolders: Data/SKSE/Plugins/whois/particles/<type>/

    // Display Options
    float VerticalOffset = 8.0f;
    bool  HidePlayer = false;
    int   ReloadKey = 0;  // 0 = disabled, 207 = End key

    // Animation
    float AnimSpeedLowTier;
    float AnimSpeedMidTier;
    float AnimSpeedHighTier;

    // Color & Effects
    float ColorWashAmount;
    float NameColorMix;
    float EffectAlphaMin;
    float EffectAlphaMax;
    float StrengthMin;
    float StrengthMax;

    // Smoothing, settle time in seconds
    float AlphaSettleTime;
    float ScaleSettleTime;
    float PositionSettleTime;

    VisualSettings& Visual() {
        static VisualSettings vs;
        return vs;
    }

    // Font Settings
    std::string NameFontPath;
    float NameFontSize;
    std::string LevelFontPath;
    float LevelFontSize;
    std::string TitleFontPath;
    float TitleFontSize;

    // Ornament font settings
    std::string OrnamentFontPath;
    float OrnamentFontSize;

    // Appearance template settings
    std::string TemplateFormID;
    std::string TemplatePlugin;
    bool UseTemplateAppearance;
    bool TemplateIncludeRace;
    bool TemplateIncludeBody;
    bool TemplateCopyFaceGen = true;       // Load and apply FaceGen mesh/tint
    bool TemplateCopySkin = false;         // Copy skin textures (risky for custom bodies)
    bool TemplateCopyOverlays = false;     // Copy RaceMenu overlays if available (requires NiOverride)
    bool TemplateCopyOutfit = false;       // Copy equipped armor from template actor
    bool TemplateReapplyOnReload = false;  // Re-apply appearance on hot reload
    std::string TemplateFaceGenPlugin;     // Optional override for FaceGen plugin (empty = auto-detect)

    // Helper function: Remove leading/trailing whitespace
    static std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (std::string::npos == first) return str;
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    // Helper function: Parse float with fallback default
    static float ParseFloat(const std::string& str, float defaultVal) {
        try {
            return std::stof(str);
        } catch (...) {
            // Return default if parsing fails
            return defaultVal;
        }
    }

    // Helper function: Parse integer with fallback default
    static int ParseInt(const std::string& str, int defaultVal) {
        try {
            return std::stoi(str);
        } catch (...) {
            return defaultVal;
        }
    }

    // Helper function: Parse boolean (true/false, 1/0, yes/no)
    static bool ParseBool(const std::string& str) {
        std::string lower = str;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return (lower == "true" || lower == "1" || lower == "yes");
    }

    // Helper function: Parse comma-separated RGB color (0.0-1.0)
    static void ParseColor3(const std::string& str, float out[3]) {
        std::istringstream ss(str);
        std::string token;
        int idx = 0;
        // Split by comma and parse each component
        while (std::getline(ss, token, ',') && idx < 3) {
            out[idx++] = ParseFloat(Trim(token), 1.0f);
        }
    }

    // Helper function: Parse effect type name to enum
    static EffectType ParseEffectType(const std::string& str) {
        std::string s = Trim(str);
        // Map string names to effect types
        if (s == "None") return EffectType::None;
        if (s == "Gradient") return EffectType::Gradient;
        if (s == "VerticalGradient") return EffectType::VerticalGradient;
        if (s == "DiagonalGradient") return EffectType::DiagonalGradient;
        if (s == "RadialGradient") return EffectType::RadialGradient;
        if (s == "Shimmer") return EffectType::Shimmer;
        if (s == "ChromaticShimmer") return EffectType::ChromaticShimmer;
        if (s == "PulseGradient") return EffectType::PulseGradient;
        if (s == "RainbowWave") return EffectType::RainbowWave;
        if (s == "ConicRainbow") return EffectType::ConicRainbow;
        if (s == "Aurora") return EffectType::Aurora;
        if (s == "Sparkle") return EffectType::Sparkle;
        if (s == "Plasma") return EffectType::Plasma;
        if (s == "Scanline") return EffectType::Scanline;
        // Default to simple gradient if unknown
        return EffectType::Gradient;
    }

    void Load()
    {
        // File is located in Skyrim's Data folder under SKSE plugins directory
        std::ifstream file("Data/SKSE/Plugins/whois.ini");
        if (!file.is_open()) return;  // Silently use defaults if file not found

        std::string line;
        std::string currentSection;    // Tracks current [Section] header for multi-section parsing
        int currentTier = -1;          // Tracks which tier we're parsing (-1 = global settings)
        int currentSpecialTitle = -1;  // Tracks which special title we're parsing (-1 = none)

        // Line-by-line parsing allows for flexible format with sections
        while (std::getline(file, line)) {
            line = Trim(line);

            // Skip empty lines and comments (both ; and # style for user convenience)
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;

            // Detect section headers like [Tier0], [Tier1], etc.
            // Section headers change the parsing context for subsequent kv pairs
            if (line.size() >= 2 && line[0] == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.size() - 2);

                // Tier numbers are 0-indexed and can be any non-negative integer
                if (currentSection.size() >= 4 && currentSection.substr(0, 4) == "Tier") {
                    std::string numStr = currentSection.substr(4);
                    currentTier = ParseInt(numStr, -1);
                    currentSpecialTitle = -1;  // Not in a special title section

                    if (currentTier < 0) {
                        currentTier = -1;  // Invalid tier number, treat as non-tier section
                    } else {
                        // Dynamically grow the Tiers vector to accommodate the specified tier
                        while (static_cast<int>(Tiers.size()) <= currentTier) {
                            TierDefinition newTier;
                            newTier.minLevel = 1;
                            newTier.maxLevel = 250;
                            newTier.title = "Unknown";
                            // Default to white colors
                            newTier.leftColor[0] = newTier.leftColor[1] = newTier.leftColor[2] = 1.0f;
                            newTier.rightColor[0] = newTier.rightColor[1] = newTier.rightColor[2] = 1.0f;
                            newTier.highlightColor[0] = newTier.highlightColor[1] = newTier.highlightColor[2] = 1.0f;
                            // Default effect is simple gradient
                            newTier.titleEffect.type = EffectType::Gradient;
                            newTier.nameEffect.type = EffectType::Gradient;
                            newTier.levelEffect.type = EffectType::Gradient;
                            // Default particle settings
                            newTier.particleTypes = "";
                            newTier.particleCount = 0;
                            Tiers.push_back(newTier);
                        }
                    }
                }
                // Special title sections like [SpecialTitle0], [SpecialTitle1], etc.
                else if (currentSection.size() >= 12 && currentSection.substr(0, 12) == "SpecialTitle") {
                    std::string numStr = currentSection.substr(12);
                    currentSpecialTitle = ParseInt(numStr, -1);
                    currentTier = -1;  // Not in a tier section

                    if (currentSpecialTitle >= 0) {
                        // Dynamically grow the SpecialTitles vector
                        while (static_cast<int>(SpecialTitles.size()) <= currentSpecialTitle) {
                            SpecialTitleDefinition newSpecial;
                            newSpecial.keyword = "";
                            newSpecial.displayTitle = "";
                            newSpecial.color[0] = newSpecial.color[1] = newSpecial.color[2] = 1.0f;
                            newSpecial.glowColor[0] = newSpecial.glowColor[1] = newSpecial.glowColor[2] = 1.0f;
                            newSpecial.forceOrnaments = true;
                            newSpecial.forceParticles = true;
                            newSpecial.priority = 0;
                            SpecialTitles.push_back(newSpecial);
                        }
                    }
                } else {
                    currentTier = -1;  // Non-tier section, switch to global context
                    currentSpecialTitle = -1;
                }
                continue;
            }

            // Parse kv pairs
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;  // No '=' found, skip malformed line

            std::string key = Trim(line.substr(0, eq));
            std::string val = Trim(line.substr(eq + 1));

            bool handled = false;

            // If we're inside a [TierN] section, all kv pairs apply to that tier
            // This allows each tier to have its own level range, colors, and effects
            if (currentTier >= 0 && currentTier < static_cast<int>(Tiers.size())) {
                if (key == "Name") {
                    // "Name" is displayed as the tier title
                    Tiers[currentTier].title = val;
                    handled = true;
                } else if (key == "MinLevel") {
                    // Inclusive lower bound for this tier's level range
                    Tiers[currentTier].minLevel = static_cast<uint16_t>(ParseInt(val, 1));
                    handled = true;
                } else if (key == "MaxLevel") {
                    // Inclusive upper bound for this tier's level range
                    Tiers[currentTier].maxLevel = static_cast<uint16_t>(ParseInt(val, 25));
                    handled = true;
                } else if (key == "LeftColor") {
                    // Left/start color for gradient effects (RGB floats 0.0-1.0)
                    ParseColor3(val, Tiers[currentTier].leftColor);
                    handled = true;
                } else if (key == "RightColor") {
                    // Right/end color for gradient effects
                    ParseColor3(val, Tiers[currentTier].rightColor);
                    handled = true;
                } else if (key == "HighlightColor") {
                    // Accent color for shimmer, sparkle, and scanline effects
                    ParseColor3(val, Tiers[currentTier].highlightColor);
                    handled = true;
                } else if (key == "TitleEffect" || key == "NameEffect" || key == "LevelEffect") {
                    // Effect syntax: "EffectType param1,param2,param3,param4,param5 [whiteBase]"
                    // Example: "RainbowWave 0.15,0.30,0.60,0.40,1.00 whiteBase"

                    std::istringstream ss(val);
                    std::string effectTypeName;
                    ss >> effectTypeName;  // Extract first word (effect type name)

                    // Select which effect struct to populate based on key name
                    EffectParams& effect = (key == "TitleEffect") ? Tiers[currentTier].titleEffect :
                                          (key == "NameEffect") ? Tiers[currentTier].nameEffect :
                                          Tiers[currentTier].levelEffect;

                    effect.type = ParseEffectType(effectTypeName);

                    // Get the rest of the line
                    std::string paramsStr;
                    std::getline(ss, paramsStr);
                    paramsStr = Trim(paramsStr);

                    // Check for "whiteBase" keyword
                    // When enabled, renders white text layer under effects
                    size_t wbPos = paramsStr.find("whiteBase");
                    if (wbPos != std::string::npos) {
                        effect.useWhiteBase = true;
                        paramsStr = paramsStr.substr(0, wbPos);  // Strip flag from params
                    }

                    // Parse comma-separated parameters (up to 5 floats)
                    // Different effects interpret these differently:
                    //   Shimmer: param1=bandWidth, param2=strength
                    //   Aurora: param1=speed, param2=waves, param3=intensity, param4=sway
                    //   RainbowWave: param1=baseHue, param2=hueSpread, param3=speed, param4=sat, param5=val
                    std::istringstream paramStream(paramsStr);
                    std::string token;
                    int paramIdx = 0;
                    while (std::getline(paramStream, token, ',') && paramIdx < 5) {
                        token = Trim(token);
                        if (token.empty()) continue;  // Skip empty tokens (e.g., "1.0,,2.0")

                        float v = ParseFloat(token, 0.0f);
                        switch(paramIdx) {
                            case 0: effect.param1 = v; break;
                            case 1: effect.param2 = v; break;
                            case 2: effect.param3 = v; break;
                            case 3: effect.param4 = v; break;
                            case 4: effect.param5 = v; break;
                        }
                        paramIdx++;
                    }
                    handled = true;
                } else if (key == "Ornaments") {
                    // Ornament format: "LEFT, RIGHT" (e.g., "AC, BD")
                    size_t commaPos = val.find(',');
                    if (commaPos != std::string::npos) {
                        // New format: split on comma
                        Tiers[currentTier].leftOrnaments = Trim(val.substr(0, commaPos));
                        Tiers[currentTier].rightOrnaments = Trim(val.substr(commaPos + 1));
                    } else if (val.length() >= 2) {
                        // Legacy format: first char = left, second char = right
                        Tiers[currentTier].leftOrnaments = val.substr(0, 1);
                        Tiers[currentTier].rightOrnaments = val.substr(1, 1);
                    } else {
                        Tiers[currentTier].leftOrnaments.clear();
                        Tiers[currentTier].rightOrnaments.clear();
                    }
                    handled = true;
                } else if (key == "ParticleTypes") {
                    // Particle types for this tier (e.g., "Stars,Wisps,Orbs")
                    Tiers[currentTier].particleTypes = val;
                    handled = true;
                } else if (key == "ParticleCount") {
                    // Number of particles for this tier (0 = use global)
                    Tiers[currentTier].particleCount = std::stoi(val);
                    handled = true;
                }
            }

            // If we're inside a [SpecialTitleN] section, parse special title properties
            if (!handled && currentSpecialTitle >= 0 && currentSpecialTitle < static_cast<int>(SpecialTitles.size())) {
                if (key == "Keyword") {
                    // Text to match in actor name (case-insensitive)
                    SpecialTitles[currentSpecialTitle].keyword = val;
                    handled = true;
                } else if (key == "DisplayTitle") {
                    // Title shown above name
                    SpecialTitles[currentSpecialTitle].displayTitle = val;
                    handled = true;
                } else if (key == "Color") {
                    // RGB color for nameplate
                    ParseColor3(val, SpecialTitles[currentSpecialTitle].color);
                    handled = true;
                } else if (key == "GlowColor") {
                    // RGB color for glow effect
                    ParseColor3(val, SpecialTitles[currentSpecialTitle].glowColor);
                    handled = true;
                } else if (key == "ForceOrnaments" || key == "ForceFlourishes") {
                    SpecialTitles[currentSpecialTitle].forceOrnaments = (val == "1" || val == "true" || val == "yes");
                    handled = true;
                } else if (key == "ForceParticles") {
                    // Always show particle aura
                    SpecialTitles[currentSpecialTitle].forceParticles = (val == "1" || val == "true" || val == "yes");
                    handled = true;
                } else if (key == "Priority") {
                    // Higher priority = checked first
                    SpecialTitles[currentSpecialTitle].priority = ParseInt(val, 0);
                    handled = true;
                } else if (key == "Ornaments") {
                    // Ornament format: "LEFT, RIGHT" (e.g., "AC, BD")
                    size_t commaPos = val.find(',');
                    if (commaPos != std::string::npos) {
                        SpecialTitles[currentSpecialTitle].leftOrnaments = Trim(val.substr(0, commaPos));
                        SpecialTitles[currentSpecialTitle].rightOrnaments = Trim(val.substr(commaPos + 1));
                    } else if (val.length() >= 2) {
                        SpecialTitles[currentSpecialTitle].leftOrnaments = val.substr(0, 1);
                        SpecialTitles[currentSpecialTitle].rightOrnaments = val.substr(1, 1);
                    } else {
                        SpecialTitles[currentSpecialTitle].leftOrnaments.clear();
                        SpecialTitles[currentSpecialTitle].rightOrnaments.clear();
                    }
                    handled = true;
                }
            }

            // Format key requires special parsing because it contains multiple quoted segments
            // Syntax: Format = "%t" "%n" "Lv.%l"
            if (!handled && key == "Format") {
                std::vector<Segment> newDisplayFormat;
                std::string newTitleFormat;
                bool titleFound = false;

                // Simple state machine to parse quoted strings
                // Handles escaped quotes (\") within strings
                bool inQuote = false;
                std::string current;
                for (size_t i = 0; i < val.size(); ++i) {
                    char c = val[i];

                    // Handle escape sequences (\" -> literal quote, etc.)
                    if (c == '\\' && i + 1 < val.size()) {
                        if (inQuote) {
                            // Inside quotes, add the escaped character literally
                            current += val[++i];
                        }
                        // Outside quotes, skip escape sequences
                        continue;
                    }

                    if (c == '"') {
                        if (inQuote) {
                            // Closing quote, finish current segment
                            if (current.find("%t") != std::string::npos) {
                                // Segment contains %t -> it's the title format, separate line
                                newTitleFormat = current;
                                titleFound = true;
                            } else {
                                // Regular segment -> add to main line
                                // Use level font if segment contains %l
                                bool isLevel = current.find("%l") != std::string::npos;
                                newDisplayFormat.push_back({current, isLevel});
                            }
                            current.clear();
                            inQuote = false;
                        } else {
                            // Opening quote, start new segment
                            inQuote = true;
                        }
                    } else if (inQuote) {
                        // Inside quotes, accumulate characters
                        current += c;
                    }
                    // Characters outside quotes are ignored
                }

                // Only update if we actually parsed something
                // This prevents empty Format= from clearing the defaults
                if (titleFound) {
                    TitleFormat = newTitleFormat;
                }
                if (!newDisplayFormat.empty()) {
                    DisplayFormat = newDisplayFormat;
                }
            }
            // Float value parsing
            else if (key == "FadeStartDistance") FadeStartDistance = ParseFloat(val, 0.0f);
            else if (key == "FadeEndDistance") FadeEndDistance = ParseFloat(val, 0.0f);
            else if (key == "ScaleStartDistance") ScaleStartDistance = ParseFloat(val, 0.0f);
            else if (key == "ScaleEndDistance") ScaleEndDistance = ParseFloat(val, 0.0f);
            else if (key == "MinimumScale") MinimumScale = ParseFloat(val, 0.0f);
            else if (key == "MaxScanDistance") MaxScanDistance = ParseFloat(val, 0.0f);
            // Occlusion Settings
            else if (key == "EnableOcclusionCulling") EnableOcclusionCulling = (ParseInt(val, 1) != 0);
            else if (key == "OcclusionSettleTime") OcclusionSettleTime = ParseFloat(val, 0.58f);
            else if (key == "OcclusionCheckInterval") OcclusionCheckInterval = ParseInt(val, 3);
            else if (key == "TitleShadowOffsetX") TitleShadowOffsetX = ParseFloat(val, 0.0f);
            else if (key == "TitleShadowOffsetY") TitleShadowOffsetY = ParseFloat(val, 0.0f);
            else if (key == "MainShadowOffsetX") MainShadowOffsetX = ParseFloat(val, 0.0f);
            else if (key == "MainShadowOffsetY") MainShadowOffsetY = ParseFloat(val, 0.0f);
            else if (key == "SegmentPadding") SegmentPadding = ParseFloat(val, 0.0f);
            else if (key == "OutlineWidthMin") OutlineWidthMin = ParseFloat(val, 0.0f);
            else if (key == "OutlineWidthMax") OutlineWidthMax = ParseFloat(val, 0.0f);
            else if (key == "FastOutlines") FastOutlines = (ParseInt(val, 0) != 0);
            // Glow Settings
            else if (key == "EnableGlow") EnableGlow = (ParseInt(val, 0) != 0);
            else if (key == "GlowRadius") GlowRadius = ParseFloat(val, 4.0f);
            else if (key == "GlowIntensity") GlowIntensity = ParseFloat(val, 0.5f);
            else if (key == "GlowSamples") GlowSamples = ParseInt(val, 8);
            // Typewriter Settings
            else if (key == "EnableTypewriter") EnableTypewriter = (ParseInt(val, 0) != 0);
            else if (key == "TypewriterSpeed") TypewriterSpeed = ParseFloat(val, 30.0f);
            else if (key == "TypewriterDelay") TypewriterDelay = ParseFloat(val, 0.0f);
            // Debug Settings
            else if (key == "EnableDebugOverlay") EnableDebugOverlay = (ParseInt(val, 0) != 0);
            // Side Ornaments
            else if (key == "EnableOrnaments" || key == "EnableFlourishes") EnableOrnaments = (ParseInt(val, 1) != 0);
            else if (key == "OrnamentScale" || key == "FlourishScale") OrnamentScale = ParseFloat(val, 1.0f);
            else if (key == "OrnamentSpacing" || key == "FlourishSpacing") OrnamentSpacing = ParseFloat(val, 6.0f);
            // Particle Aura
            else if (key == "EnableParticleAura") EnableParticleAura = (ParseInt(val, 1) != 0);
            else if (key == "EnableStars") EnableStars = (ParseInt(val, 1) != 0);
            else if (key == "EnableSparks") EnableSparks = (ParseInt(val, 0) != 0);
            else if (key == "EnableWisps") EnableWisps = (ParseInt(val, 0) != 0);
            else if (key == "EnableRunes") EnableRunes = (ParseInt(val, 0) != 0);
            else if (key == "EnableOrbs") EnableOrbs = (ParseInt(val, 0) != 0);
            else if (key == "ParticleCount") ParticleCount = ParseInt(val, 8);
            else if (key == "ParticleSize") ParticleSize = ParseFloat(val, 3.0f);
            else if (key == "ParticleSpeed") ParticleSpeed = ParseFloat(val, 1.0f);
            else if (key == "ParticleSpread") ParticleSpread = ParseFloat(val, 20.0f);
            else if (key == "ParticleAlpha") ParticleAlpha = ParseFloat(val, 0.8f);
            else if (key == "UseParticleTextures") UseParticleTextures = ParseBool(val);
            // Textures now auto-loaded from whois/particles/<type>/ folders
            // Display Options
            else if (key == "VerticalOffset") VerticalOffset = ParseFloat(val, 8.0f);
            else if (key == "HidePlayer") HidePlayer = (ParseInt(val, 0) != 0);
            else if (key == "ReloadKey") ReloadKey = ParseInt(val, 0);
            else if (key == "AnimSpeedLowTier") AnimSpeedLowTier = ParseFloat(val, 0.0f);
            else if (key == "AnimSpeedMidTier") AnimSpeedMidTier = ParseFloat(val, 0.0f);
            else if (key == "AnimSpeedHighTier") AnimSpeedHighTier = ParseFloat(val, 0.0f);
            else if (key == "ColorWashAmount") ColorWashAmount = ParseFloat(val, 0.0f);
            else if (key == "NameColorMix") NameColorMix = ParseFloat(val, 0.0f);
            else if (key == "EffectAlphaMin") EffectAlphaMin = ParseFloat(val, 0.0f);
            else if (key == "EffectAlphaMax") EffectAlphaMax = ParseFloat(val, 0.0f);
            else if (key == "StrengthMin") StrengthMin = ParseFloat(val, 0.0f);
            else if (key == "StrengthMax") StrengthMax = ParseFloat(val, 0.0f);
            else if (key == "AlphaSettleTime") AlphaSettleTime = ParseFloat(val, 0.46f);
            else if (key == "ScaleSettleTime") ScaleSettleTime = ParseFloat(val, 0.46f);
            else if (key == "PositionSettleTime") PositionSettleTime = ParseFloat(val, 0.38f);
            // Distance-Based Outline
            else if (key == "EnableDistanceOutlineScale") Visual().EnableDistanceOutlineScale = (ParseInt(val, 0) != 0);
            else if (key == "OutlineDistanceMin") Visual().OutlineDistanceMin = ParseFloat(val, 0.8f);
            else if (key == "OutlineDistanceMax") Visual().OutlineDistanceMax = ParseFloat(val, 1.5f);
            // Minimum Readable Size
            else if (key == "MinimumPixelHeight") Visual().MinimumPixelHeight = ParseFloat(val, 0.0f);
            // LOD by Distance
            else if (key == "EnableLOD") Visual().EnableLOD = (ParseInt(val, 0) != 0);
            else if (key == "LODFarDistance") Visual().LODFarDistance = ParseFloat(val, 1800.0f);
            else if (key == "LODMidDistance") Visual().LODMidDistance = ParseFloat(val, 800.0f);
            else if (key == "LODTransitionRange") Visual().LODTransitionRange = ParseFloat(val, 200.0f);
            // Visual Hierarchy
            else if (key == "TitleAlphaMultiplier") Visual().TitleAlphaMultiplier = ParseFloat(val, 0.80f);
            else if (key == "LevelAlphaMultiplier") Visual().LevelAlphaMultiplier = ParseFloat(val, 0.85f);
            // Overlap Prevention
            else if (key == "EnableOverlapPrevention") Visual().EnableOverlapPrevention = (ParseInt(val, 0) != 0);
            else if (key == "OverlapPaddingY") Visual().OverlapPaddingY = ParseFloat(val, 4.0f);
            else if (key == "OverlapIterations") Visual().OverlapIterations = ParseInt(val, 3);
            // Position Smoothing Tuning
            else if (key == "PositionSmoothingBlend") Visual().PositionSmoothingBlend = ParseFloat(val, 1.0f);
            else if (key == "LargeMovementThreshold") Visual().LargeMovementThreshold = ParseFloat(val, 50.0f);
            else if (key == "LargeMovementBlend") Visual().LargeMovementBlend = ParseFloat(val, 0.5f);
            // Tier Effect Gating
            else if (key == "EnableTierEffectGating") Visual().EnableTierEffectGating = (ParseInt(val, 0) != 0);
            else if (key == "GlowMinTier") Visual().GlowMinTier = ParseInt(val, 5);
            else if (key == "ParticleMinTier") Visual().ParticleMinTier = ParseInt(val, 10);
            else if (key == "OrnamentMinTier") Visual().OrnamentMinTier = ParseInt(val, 10);
            // Font Settings
            else if (key == "NameFontPath") NameFontPath = val;
            else if (key == "NameFontSize") NameFontSize = ParseFloat(val, 0.0f);
            else if (key == "LevelFontPath") LevelFontPath = val;
            else if (key == "LevelFontSize") LevelFontSize = ParseFloat(val, 0.0f);
            else if (key == "TitleFontPath") TitleFontPath = val;
            else if (key == "TitleFontSize") TitleFontSize = ParseFloat(val, 0.0f);
            // Ornament Font Settings
            else if (key == "OrnamentFontPath") OrnamentFontPath = val;
            else if (key == "OrnamentFontSize") OrnamentFontSize = ParseFloat(val, 64.0f);
            // Appearance Template Settings
            else if (key == "TemplateFormID") TemplateFormID = val;
            else if (key == "TemplatePlugin") TemplatePlugin = val;
            else if (key == "UseTemplateAppearance") UseTemplateAppearance = ParseBool(val);
            else if (key == "TemplateIncludeRace") TemplateIncludeRace = ParseBool(val);
            else if (key == "TemplateIncludeBody") TemplateIncludeBody = ParseBool(val);
            else if (key == "TemplateCopyFaceGen") TemplateCopyFaceGen = ParseBool(val);
            else if (key == "TemplateCopySkin") TemplateCopySkin = ParseBool(val);
            else if (key == "TemplateCopyOverlays") TemplateCopyOverlays = ParseBool(val);
            else if (key == "TemplateCopyOutfit") TemplateCopyOutfit = ParseBool(val);
            else if (key == "TemplateReapplyOnReload") TemplateReapplyOnReload = ParseBool(val);
            else if (key == "TemplateFaceGenPlugin") TemplateFaceGenPlugin = val;
        }
    }
}
