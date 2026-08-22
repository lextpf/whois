#include <SKSE/SKSE.h>

#include "ProjectManifest.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ProjectManifest
{
namespace
{
struct Data
{
    bool loaded = false;
    std::string fontName;
    std::string fontLevel;
    std::string fontTitle;
    std::string fontOrnament;
    std::vector<std::string> tierBadges;
    std::unordered_map<std::string, std::vector<std::string>> particles;
    std::string bubblePop;
};

Data& State()
{
    static Data d;
    return d;
}

const std::vector<std::string> kEmptyVec{};
const std::string kEmptyStr{};

// Resolve a manifest entry to a full path. An absolute path (drive letter or leading slash)
// passes through unchanged; everything else is taken relative to the manifest's directory.
std::string Resolve(const std::string& baseDir, const std::string& rel)
{
    if (rel.empty())
    {
        return {};
    }
    const bool absolute = (rel.size() > 1 && rel[1] == ':') || rel[0] == '/' || rel[0] == '\\';
    return absolute ? rel : baseDir + rel;
}
}  // namespace

bool Load(const std::string& path)
{
    Data& d = State();
    d = Data{};

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        SKSE::log::info("ProjectManifest: no manifest at '{}' - using built-in asset defaults",
                        path);
        return false;
    }

    nlohmann::json j;
    try
    {
        in >> j;
    }
    catch (const std::exception& e)
    {
        SKSE::log::error("ProjectManifest: failed to parse '{}': {}", path, e.what());
        return false;
    }

    // Paths in the manifest are relative to the manifest's own folder.
    std::string baseDir;
    if (const auto slash = path.find_last_of("/\\"); slash != std::string::npos)
    {
        baseDir = path.substr(0, slash + 1);
    }

    try
    {
        if (const auto it = j.find("fonts"); it != j.end() && it->is_object())
        {
            const auto& f = *it;
            const auto get = [&](const char* key) -> std::string
            {
                const auto e = f.find(key);
                return (e != f.end() && e->is_string()) ? Resolve(baseDir, e->get<std::string>())
                                                        : std::string{};
            };
            d.fontName = get("name");
            d.fontLevel = get("level");
            d.fontTitle = get("title");
            d.fontOrnament = get("ornament");
        }

        if (const auto it = j.find("tierBadges"); it != j.end() && it->is_array())
        {
            for (const auto& e : *it)
            {
                if (e.is_string())
                {
                    d.tierBadges.push_back(Resolve(baseDir, e.get<std::string>()));
                }
            }
        }

        if (const auto it = j.find("particles"); it != j.end() && it->is_object())
        {
            for (const auto& [token, arr] : it->items())
            {
                if (!arr.is_array())
                {
                    continue;
                }
                std::vector<std::string> paths;
                for (const auto& e : arr)
                {
                    if (e.is_string())
                    {
                        paths.push_back(Resolve(baseDir, e.get<std::string>()));
                    }
                }
                if (!paths.empty())
                {
                    d.particles.emplace(token, std::move(paths));
                }
            }
        }

        if (const auto it = j.find("bubblePop"); it != j.end() && it->is_string())
        {
            d.bubblePop = Resolve(baseDir, it->get<std::string>());
        }
    }
    catch (const std::exception& e)
    {
        SKSE::log::error("ProjectManifest: malformed manifest '{}': {}", path, e.what());
        d = Data{};
        return false;
    }

    d.loaded = true;
    SKSE::log::info(
        "ProjectManifest: loaded '{}' - {} tier emblem(s), {} particle style(s), fonts {}",
        path,
        d.tierBadges.size(),
        d.particles.size(),
        d.fontName.empty() ? "unmapped" : "mapped");
    return true;
}

bool IsLoaded()
{
    return State().loaded;
}

const std::string& FontName()
{
    return State().fontName;
}
const std::string& FontLevel()
{
    return State().fontLevel;
}
const std::string& FontTitle()
{
    return State().fontTitle;
}
const std::string& FontOrnament()
{
    return State().fontOrnament;
}

const std::vector<std::string>& TierBadges()
{
    return State().tierBadges;
}

const std::vector<std::string>& ParticleVariants(const std::string& token)
{
    const auto& particles = State().particles;
    const auto it = particles.find(token);
    return (it != particles.end()) ? it->second : kEmptyVec;
}

const std::string& BubblePop()
{
    return State().bubblePop;
}
}  // namespace ProjectManifest
