# Repository Guidelines

## Rules

Ask when unclear. If intent, architecture, or requirements are ambiguous, ask before coding.

Flag uncertainty. If an approach, dependency, or technical detail is uncertain, say so before proceeding.

Challenge bad direction. If my request conflicts with settled practice or likely long-term maintainability, point it out and suggest a better path.

End with omissions. After each task, state what you changed and what you intentionally did not do.

## Documentation

Document the code using ASD-STE100-inspired Simplified Technical English: use short, direct sentences, one term per concept, active voice, explicit conditions, and avoid idioms, unnecessary synonyms, or ambiguous wording. Focus documentation on intent, constraints, side effects, and non-obvious behavior;

## Project Structure & Module Organization

`src/` contains the C++23 SKSE plugin. Keep subsystem declarations and implementations paired as `PascalCase.hpp`/`.cpp`; rendering is split across `Renderer*` and `TextEffects*`, while configuration lives in `Settings*`. GoogleTest suites are in `tests/`. Runtime defaults and manifests are `glyph.ini` and `glyph.project.json`. `scripts/` supports builds and documentation; `docs/` and `site/` are generated. `specs/` and `plans/` record design decisions. Treat `build/`, `build-cdb/`, and `graphify-out/` as generated output.

## Build, Test, and Development Commands

- `.\build.bat` formats C++ sources, configures CMake/vcpkg, runs clang-tidy, builds `build\Release\glyph.dll`, and generates docs when tools are available.
- `.\build.bat --skip-tidy` runs the same pipeline without static analysis.
- `cmake --preset default` configures the Visual Studio 2022 build directly.
- `cmake --build build --config Release --target glyph` performs a focused plugin build.
- `.\test.bat` builds and runs all GoogleTest executables. For CI-style output, use `ctest --test-dir build -C Release --output-on-failure` after building tests.

## Coding Style & Naming Conventions

Run `clang-format -i src\*.cpp src\*.hpp` before review. `.clang-format` specifies four spaces, no tabs, a 100-column limit, and Allman braces. Use `PascalCase` for files, types, namespaces, and functions; `camelCase` for locals, parameters, and plain-struct fields; `m_PascalCase` for class members; and `UPPER_SNAKE_CASE` for constants/macros. Headers use `#pragma once`. Follow `CONTRIBUTING.md` for include ordering and the header/source Doxygen split. Use `.clang-tidy` for advisory static analysis.

## Testing Guidelines

Add tests for non-trivial parsing, math, serialization, state, API, and bug-fix changes. Name files `test_<area>.cpp` and cases descriptively, for example `TEST(DeckLayoutTest, UsesTheLimitingDimension)`. There is no numeric coverage threshold; regression behavior should be covered, or the PR should explain why testing is impractical.

## Commit & Pull Request Guidelines

History uses a relevant emoji followed by an imperative summary, such as `✨ Add scale-aware centering`, `✅ Cover color rules`, or `🔧 Update settings bindings`. Keep commits and PRs focused. PR descriptions should explain what changed, why, tradeoffs, and validation; link applicable issues and include screenshots for visible rendering changes. Avoid unrelated formatting churn.

## Assets & Local Configuration

Do not commit ignored runtime assets, proprietary Font Awesome files, generated binaries, or local Skyrim/MO2 paths. Review `deploy.bat` paths locally before deployment.
