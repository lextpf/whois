# Contributing to whois

Thank you for your interest in contributing to whois! This document provides guidelines and coding standards to help you get started.

## Getting Started

1. **Fork the repository** on GitHub.
2. **Clone your fork** locally.
3. **Build the project** with `.\build.bat` (see [README](README.md#building)).
4. **Deploy** with `.\deploy.bat` (adjust `DEPLOY_PATH` for your Skyrim install).
5. Use MSVC 2022 with C++23 enabled. Builds must produce a valid SKSE plugin DLL.

## How to Contribute

### Reporting Bugs

Before submitting a bug report:
- Check if the issue already exists in [GitHub Issues](https://github.com/lextpf/whois/issues)
- Try to reproduce with the latest version

When reporting, include:
- Steps to reproduce the issue
- Expected behavior vs actual behavior
- Skyrim version, SKSE version, and mod list (if relevant)
- Contents of `whois.log` from `Documents/My Games/Skyrim Special Edition/SKSE/`

### Suggesting Features

Feature requests are welcome! Please:
- Check existing issues to avoid duplicates
- Describe the use case and why it would be valuable
- Be open to discussion about implementation approaches

### Submitting Code

1. **Create a feature branch**: `git checkout -b feature/your-feature-name`
2. **Make your changes** following the coding standards below
3. **Test thoroughly** - load the plugin in-game, verify nameplates render, check all tiers
4. **Commit with clear messages**: `git commit -m "Add feature: description"`
5. **Push to your fork**: `git push origin feature/your-feature-name`
6. **Open a Pull Request** with a clear description of your changes

## Coding Standards

### Language

- **C++23** - prefer standard library facilities (`std::optional`, `std::string_view`, `std::format`) where appropriate.
- Compile with warnings enabled and treat warnings as errors when possible.
- All game engine types come from **CommonLibSSE-NG** (`RE::` namespace).

### Naming Conventions

| Element           | Style                        | Example                              |
|-------------------|------------------------------|--------------------------------------|
| Classes/Structs   | PascalCase                   | `ActorCache`, `ActorDrawData`        |
| Enums             | PascalCase with `enum class` | `enum class EffectType`              |
| Enum values       | PascalCase                   | `EffectType::RainbowWave`            |
| Methods/Functions | PascalCase                   | `WorldToScreen()`, `ApplyEffect()`   |
| Namespaces        | PascalCase                   | `TextEffects`, `Renderer`            |
| Local variables   | camelCase                    | `deltaTime`, `actorHandle`           |
| Parameters        | camelCase                    | `float distance`, `int tierIndex`    |
| Constants/Macros  | UPPER_SNAKE                  | `MAX_DISTANCE`, `TWO_PI`            |

### Brace Style

We use **Allman style** - opening braces on their own line:

```cpp
namespace Occlusion
{
    bool IsBehindCamera(const RE::NiPoint3& worldPos)
    {
        if (distance < 0.001f)
        {
            return false;
        }

        return dot > 0.0f;
    }
}
```

### Include Order

Organize includes in this order, with blank lines between groups:

```cpp
#include "Occlusion.h"         // 1. Corresponding header (for .cpp)

#include "Settings.h"          // 2. Project headers
#include "TextEffects.h"

#include <RE/A/Actor.h>        // 3. CommonLib / external libraries
#include <RE/P/PlayerCamera.h>

#include <atomic>              // 4. Standard library
#include <vector>
```

For headers, use `PCH.h` as the precompiled header:

```cpp
#pragma once

#include "PCH.h"
```

### Header Files

- Use `#pragma once` for header guards
- Put Doxygen documentation in headers, not in .cpp files
- Keep headers minimal - forward declare when possible
- Include `PCH.h` for CommonLib types

```cpp
#pragma once

#include "PCH.h"

/**
 * @namespace Occlusion
 * @brief Raycast visibility checks for nameplates.
 * @ingroup Utilities
 */
namespace Occlusion
{
    bool HasLineOfSight(RE::Actor* actor);
}
```

### Documentation

**In header files**, use Doxygen comments for public APIs:

```cpp
/**
 * @brief Calculate screen position for an actor.
 *
 * @param actor Target actor reference.
 * @param outPos Output screen coordinates.
 *
 * @return `true` if position is on screen.
 */
bool WorldToScreen(RE::Actor* actor, ImVec2& outPos);
```

Use `///` for brief enum/member documentation:

```cpp
enum class ParticleStyle
{
    Stars,    ///< Twinkling star points
    Sparks,   ///< Fast, erratic fire-like sparks
    Wisps,    ///< Slow, flowing ethereal wisps
    Runes,    ///< Small magical rune symbols
    Orbs,     ///< Soft glowing orbs
};
```

**In .cpp files**, use regular `//` comments for implementation details:

```cpp
// Framerate-independent exponential smoothing
float alpha = 1.0f - std::exp(-dt / settleTime);
```

### Modern C++ Guidelines

- Prefer `enum class` over plain `enum`
- Use `nullptr` instead of `NULL`
- Use `auto` when the type is obvious or unwieldy
- Prefer range-based for loops
- Mark things `const` and `constexpr` when possible
- Use `std::atomic` for cross-thread flags
- Use `#define` sparingly and only for numeric constants (`TWO_PI`, `PI`)

### Things to Avoid

- Don't use raw `new`/`delete` - use smart pointers or containers
- Don't access D3D11/ImGui from the game thread - use the render hook
- Don't block the render thread with heavy computation
- Don't commit debug code (sleep calls, excessive logging, hardcoded FormIDs)
- Don't modify CommonLib headers - work with the API as-is

## Architecture Guidelines

### Thread Safety

whois uses a producer-consumer pattern. The **game thread** collects actor data and the **render thread** draws nameplates. Never access game objects from the render thread:

```cpp
// Good - schedule work on game thread
SKSE::GetTaskInterface()->AddTask([](){ /* access RE:: types here */ });

// Bad - accessing game data from render callback
RE::PlayerCharacter::GetSingleton();  // unsafe from render thread
```

### Adding New Effects

1. Add the enum value to `Settings::EffectType`
2. Implement the effect function in `TextEffects.cpp`
3. Add the case to `ApplyEffect()` in `TextEffects.cpp`
4. Add INI parsing in `Settings.cpp`
5. Document parameters in `whois.ini`

### Adding New Particle Styles

1. Add the enum value to `Settings::ParticleStyle`
2. Add texture generation in `ParticleTextures.cpp`
3. Add the INI toggle in `Settings.cpp` (`EnableStyleName`)
4. Place sprite PNGs in `skse/plugins/whois/particles/stylename/`

## Testing Your Changes

Before submitting a PR:

1. **Build successfully** with `.\build.bat`
2. **Deploy and launch** Skyrim with the plugin loaded
3. **Check the log** - `whois.log` should show no errors
4. **Verify nameplates** render for NPCs at various distances
5. **Test tier effects** - use `set level` console commands to check different tiers
6. **Hot reload** - press F7 to verify INI changes take effect
7. **Toggle overlay** - type `whois` in console to verify on/off works
8. **Check performance** - enable debug overlay to monitor frame impact

## Commit Messages

Use conventional commits:

```
feat: add aurora text effect
fix: correct projection for ultrawide monitors
docs: update configuration examples
refactor: simplify actor caching
perf: optimize distance calculations
```

## Code Review Process

1. A maintainer will review your PR
2. Address any feedback or questions
3. Once approved, your PR will be merged

## Questions?

- Open a [GitHub Issue](https://github.com/lextpf/whois/issues) for bugs or features
- Start a [GitHub Discussion](https://github.com/lextpf/whois/discussions) for questions

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).

Thank you for helping improve whois!
