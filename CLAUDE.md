# CLAUDE.md

## Rules

Ask when unclear. If intent, architecture, or requirements are ambiguous, ask before coding.

Flag uncertainty. If an approach, dependency, or technical detail is uncertain, say so before proceeding.

Challenge bad direction. If my request conflicts with settled practice or likely long-term maintainability, point it out and suggest a better path.

End with omissions. After each task, state what you changed and what you intentionally did not do.

## Documentation

Document the code using ASD-STE100-inspired Simplified Technical English: use short, direct sentences, one term per concept, active voice, explicit conditions, and avoid idioms, unnecessary synonyms, or ambiguous wording. Focus documentation on intent, constraints, side effects, and non-obvious behavior;

## Project

`glyph` is an SKSE64 plugin (C++23, MSVC, x64) for Skyrim SE 1.5.97 / AE 1.6.x (Steam + GOG) that
draws floating actor nameplates as an ImGui overlay injected into the game's D3D11 pipeline. It
builds to a single Address Library-backed `glyph.dll`. Skyrim VR is deliberately unsupported.

## Commands

Requires `VCPKG_ROOT` set; all presets use the `x64-windows-static` triplet and the static MSVC
runtime (`/MT`, `/EHa`).

```powershell
.\build.bat                 # clang-format -i, cmake configure, clang-tidy, Release build, doxide+mkdocs
.\build.bat --skip-tidy     # same, minus static analysis (much faster iteration)
.\test.bat                  # configure if needed, build all 5 gtest targets, run them
.\deploy.bat                # stage dll + ini + glyph.project.json + assets/ into an MO2 mod folder
```

Finer-grained (what the batch files wrap):

```powershell
cmake --preset default                                        # VS 2022 generator -> build/
cmake --build build --config Release --target glyph -- /m:1   # plugin only
ctest --test-dir build -C Release --output-on-failure         # all tests
ctest --test-dir build -C Release -R GraffitoBasis            # tests matching a regex
build\Release\glyph_test_settings.exe --gtest_filter=Foo.Bar  # one gtest case
clang-format --dry-run --Werror src/*.cpp src/*.hpp           # what CI's format gate runs
```

Notes:
- `-- /m:1` is intentional. `CommonLibSSE-NG` is compiled `/MP1` (see `CMakeLists.txt`) because its
  templates exhaust RAM under parallel `cl.exe` and crash with `STATUS_ACCESS_VIOLATION`. Clean
  builds are slow; incremental ones are fine.
- clang-tidy runs out-of-band against a **Ninja sidecar** compile database in `build-cdb/`
  (`cmake --preset compile-db` + `scripts/_normalize_compile_db.py`), never via
  `CMAKE_CXX_CLANG_TIDY`. The VS generator does not emit `compile_commands.json`, which is also why
  `.clangd` points at `build-cdb`. Regenerate the sidecar after adding or removing sources.
- `docs/` and `site/` are generated (gitignored) — don't hand-edit them. The one exception is
  `docs/main.html`: `.gitignore` un-ignores it (`docs/*` then `!docs/main.html`) because it is the
  hand-written MkDocs Material theme override that `custom_dir: docs` loads. It is source, not output.
- The `windows-tests` / `ci-windows-tests` build presets list only 3 of the 5 test targets;
  `test.bat` and `ctest` cover all five. Add new test targets to both places.

## Architecture

### Two-thread producer/consumer contract (the central invariant)

Everything in `Renderer` is split by thread affinity, and mixing them up is the main source of
crashes here.

- **Game thread** (`RendererSnapshot.cpp`, entered via `SKSE::GetTaskInterface()->AddTask()`):
  the only place `RE::*` game objects may be dereferenced. `UpdateSnapshot_GameThread()` scans
  `RE::ProcessLists`, resolves names/levels/factions/relationships, runs occlusion and
  TrueHUD/moreHUD queries, and publishes a `std::vector<ActorDrawData>` of **plain data** under
  `snapshotLock`, plus the `allowOverlay` / `allowDeck` atomics.
  There is one deliberate, documented exception: `Occlusion::GetCameraInfo()` reads
  `RE::PlayerCamera` and is called from the render thread every frame for focus selection and
  Graffito targeting, where a torn read is a benign one-frame glitch. `Occlusion.hpp` states
  this. Do not widen the exception without the same kind of written argument.
- **Render thread** (`Renderer.cpp`, `RendererLayout.cpp`, `RendererEffects.cpp`): copies the
  snapshot under the lock, then projects, smooths, lays out and draws. It never touches game state.
- `QueueSnapshotUpdate_RenderThread()` requests the next update; `updateQueued` coalesces so at most
  one task is ever in flight.

Practical rules:
- A function suffixed `_GameThread` / `_RenderThread`, or `...RT`, states its affinity — respect it.
  `GameState::CanDrawOverlay()` is game-thread only; the render thread reads its cached result via
  `Renderer::IsOverlayAllowedRT()`.
- Anything the render thread needs about an actor must be added to `ActorDrawData` in
  `RendererInternal.hpp` and filled in on the game thread. Do not put an `RE::Actor*` in it.

### Renderer translation units

`Renderer.hpp` is the only public surface (`Draw`, `TickRT`, `IsOverlayAllowedRT`, …).
`RendererInternal.hpp` holds all shared types and state. Four renderer TUs include it:
`Renderer.cpp` (frame orchestration, `Draw()`, focus selection, hot-reload driver),
`RendererSnapshot.cpp` (game thread), `RendererLayout.cpp` (measurement, `FormatString`, badges),
`RendererEffects.cpp` (outline/glow/particles, per-effect dispatch). `Deck.cpp` includes it as
well, so renderer-internal state is reachable from five TUs, not four. Mutable state lives in
`RendererState` / `SnapshotState` behind `GetState()` / `GetSnapshotState()` function-local
statics — not namespace-scope globals.

### Hooks and frame entry

`Hooks::Install()` hooks `BSGraphics::Renderer::CreateD3DAndSwapChain` (thunk call) for D3D11 init,
`HUDMenu::PostDisplay` (vtable[6]) for the normal per-frame draw, and `IDXGISwapChain::Present`
(COM vtable[8]) as a fallback for upscaler stacks (DLSS/FSR) that skip `PostDisplay`. ImGui init is
lazy and re-entrancy-safe (`EnsureOverlayInitialized`: `initialized` acquire-load, 5s retry backoff,
CAS guard) because either hook can fire first. Fonts, particle textures, badge SVGs, and the GPU
post-process / DepthClip / SceneMeter / Graffito pipelines are all created on the first frame that
has a device, and re-created after a device change.

### ImGui draw-callback bracketing

The GPU-side features do not fork ImGui; they bracket draws with `ImDrawList::AddCallback` pairs
that save and restore *only* the state they touch, so they compose:

`SceneMeter::CaptureCallback` (before any glyph draws — the meter must never read the overlay's own
text back) → `TextPostProcess::BeginGlowCapture` / `BeginDivideCapture` → per-plate
`Graffito::Apply/RestoreCallback` (vertex shader + VS CB slot 0 only) and
`DepthClip::Apply/RestoreCallback` (pixel shader) → `ParticleTextures` blend/sampler callbacks →
`End...AndComposite`. When adding a pass, keep the same discipline: narrow save/restore, and a
failure path that leaves the frame rendering exactly as it did before the feature existed — every
one of these degrades to a no-op rather than hard-failing.

### Settings, hot reload, and the binding table

- `Settings::Load()` parses `Data/SKSE/Plugins/glyph.ini` (~1400 lines: `[General]`, `[Tier0..19]`,
  `[SpecialTitleN]`, `[HonorificN]`, `[RegisterN]`, `[Icons]`, `[Graffito]`, `[Deck]`, …). Scalar
  keys are matched by name regardless of which section they appear in.
- Scalars are declared **once** in the `kSettings` descriptor table (shape in `SettingsBinding.hpp`,
  table in `Settings.cpp`): key, alias, target pointer, default, validation rule. `ResetToDefaults()`,
  `ClampAndValidate()` and the parser all walk that table. Add new scalars there — struct member
  initializers in `Settings.hpp` are compile-time placeholders, not the operative defaults.
- Accessors return references (`Settings::Distance()`, `Settings::Glow()`, …) guarded by
  `Settings::Mutex()` (a `shared_mutex`). The render thread does **not** read them per-draw: `Draw()`
  captures a `RenderSettingsSnapshot` once per frame when `Settings::Generation()` changes, and every
  render-thread function takes that snapshot by const ref.
- Hot reload (`ReloadKey`, F7): the render thread sets `reloadRequested`, snapshot updates pause
  (`pauseSnapshotUpdates`), `Settings::Load()` runs off the render thread, then `reloadCompleted`
  releases the pause and clears the caches.

### Assets

Custom assets ship GUID-obfuscated and are resolved by `glyph.project.json` via `ProjectManifest`
(fonts by role, tier badges in rank order, particle sprites by style token). `assets/` is gitignored
— it ships in the release archive, not the repo. `duotone/` (the Font Awesome icon library) is
deliberately *not* obfuscated because `[Icons]` selects icons by semantic name. Every loader falls
back gracefully when the manifest is missing (FA icons for badges, procedural sprites for particles,
INI `*FontPath` keys for fonts).

## Conventions

`CONTRIBUTING.md` is the authoritative style guide (naming, ownership, error handling, doc-comment
rules) and is expected to be followed. Formatting is mechanical — run clang-format, don't argue with
it. Repo-specific points that are easy to get wrong:

- **Doc comments are split by file kind.** Headers use `/** */` blocks, `///` one-liners and trailing
  `///<`, with Doxygen commands; `.cpp` files use plain `//` only, with `// @author` as the single
  exception. Moving prose from a header into a `.cpp` means stripping the `@p` / `@c` / `@ref` markup.
- **Do not use `@name` / `@{` / `@}` member groups in `src/*.hpp`.** doxide 0.9.0 does not understand
  them: it copies the commands into the generated Markdown as literal text and glues the group prose
  onto the *next* member's summary row, so the member table is wrong (verified — `ActorCache::sawAlive`
  absorbed the whole "Last Rites" blurb plus a stray `@{`). It has also crashed `doxide build` on large
  headers. Use plain `//` section comments between members (the existing idiom in `Settings.hpp`).
- **Keep the brief line plain.** `@p` or a square bracket in a doc block's brief (the `@brief` line, or
  the first prose line when `@brief` is omitted) makes doxide 0.9.0 log `unrecognized command: brief`,
  drop that file's type pages, and exit non-zero — `build.bat` then fails at the docs step. Both are
  safe in prose, `@param` and `@return` text. Same for `@par`, `@ref`, `@f[ ... @f]` and `@f$ ... @f$`,
  which leak literally anywhere they appear: use `$$ ... $$` for math and fenced `mermaid` for diagrams.
- **`@ingroup` targets are declared in `doxide.yml`**, not via `@addtogroup` — there is no
  group-definitions header in this project.
- **An explicit INI value always wins over a derived default.** Branch on "did the user set it"
  (`std::optional`), never on "does it equal a sentinel". Derivation is a fallback for empty values
  only.
- **A new `ParticleStyle` is not done until a tier uses it.** The C++ side is `static_assert`-guarded
  to cover every style, so a style no tier lists compiles fine and renders never. Add its token to a
  thematically-fitting `[TierN] ParticleTypes` in `glyph.ini`, keep the distribution even, and keep
  the per-tier count and type-count curves monotonic up the tier ladder.
- Prefer function-local statics over namespace-scope mutable state; the renderer's `GetState()`
  pattern exists for exactly this reason.

## Tests

`tests/` links **no game code** — CommonLibSSE, ImGui and `RE::Actor` cannot link in the harness. Two
patterns coexist:

- **Direct**: runtime-independent code is tested for real — `test_graffito.cpp` includes
  `GraffitoMath.hpp` / `GraffitoShaderContract.hpp`, and `glyph_test_deck` compiles
  `src/DeckUtils.cpp` + `src/DeckPng.cpp` into the target. Prefer this. Factoring pure logic out into
  a runtime-free header or TU so it can be tested directly is the right move.
- **Mirrored**: `test_settings.cpp`, `test_utils.cpp` and `test_label_format.cpp` re-implement the
  logic under test (INI parsing helpers, color/easing math, `ClassifyDelta` / `FormatString` /
  `LabelFor`). These mirrors go stale silently — when you change the production code, update the
  mirror in the same change and say so.

Register new gtest targets in three places: `CMakeLists.txt` (with `gtest_discover_tests`), `test.bat`,
and the `targets` arrays of the `windows-tests` / `ci-windows-tests` build presets in
`CMakePresets.json`. CI builds through `ci-windows-tests`, so a target missing from that array is never
built in CI even though it passes locally.

## Design docs

`plans/` and `specs/` hold dated implementation plans and design documents (e.g.
`specs/2026-07-20-player-icon-badge-rendering-design.md`) for larger features. They are untracked
working notes, useful as background on the systems they describe.
