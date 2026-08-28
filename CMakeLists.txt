cmake_minimum_required(VERSION 3.19)

project(glyph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enforce static runtime linking and Async Exception Handling (for __try)
# Also explicitly set C++23 standard for MSVC to ensure ranges support
if(MSVC)
    add_compile_options(/MT /EHa /std:c++latest)
else()
    add_compile_options(/MT /EHa)
endif()
add_link_options(/NODEFAULTLIB:MSVCRT)

# clang-tidy is run out-of-band by build.bat against the `compile-db`
# Ninja sidecar preset's compile_commands.json - it intentionally does
# NOT wrap the main MSVC build via CMAKE_CXX_CLANG_TIDY.

# Disable building tests for CommonLibSSE-NG
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# Enable Xbyak support for SKSE, needed for hook_function_prologue
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)

# Build one Address Library-backed DLL for Skyrim SE 1.5.x and AE 1.6.x
# (including GOG). Skyrim VR is intentionally unsupported.
set(ENABLE_SKYRIM_SE ON CACHE BOOL "Enable Skyrim SE runtime support" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "Enable Skyrim AE/GOG runtime support" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "Enable Skyrim VR runtime support" FORCE)

# Fetch CommonLibSSE-NG
include(FetchContent)
FetchContent_Declare(
    CommonLibSSE-NG
    GIT_REPOSITORY https://github.com/CharmedBaryon/CommonLibSSE-NG.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS NAMES CommonLibSSE
)
FetchContent_MakeAvailable(CommonLibSSE-NG)

# Clamp cl.exe to one source file at a time when compiling CommonLibSSE-NG.
# Its templates are heavy enough that the default /MP parallel compile can
# spawn workers that collectively exhaust RAM and trigger MSVC crashes
# (STATUS_ACCESS_VIOLATION / 0xC0000005 during "Generating Code..."). Costs
# a minute or two on a clean build; only this target is affected.
if(TARGET CommonLibSSE)
    target_compile_options(CommonLibSSE PRIVATE /MP1)
endif()

find_package(imgui CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(xbyak CONFIG REQUIRED)
find_package(NanoSVG CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_path(RAPIDCSV_INCLUDE_DIRS "rapidcsv.h") # Added for CommonLibSSE-NG dependency

# boost-unordered is header-only, find the include directory
find_path(BOOST_UNORDERED_INCLUDE_DIRS "boost_unordered.hpp")
if(NOT BOOST_UNORDERED_INCLUDE_DIRS)
    # Try common vcpkg locations
    find_path(BOOST_UNORDERED_INCLUDE_DIRS "boost_unordered.hpp" 
        PATHS
            "${CMAKE_CURRENT_SOURCE_DIR}/build/vcpkg_installed/x64-windows-static/include"
            "$ENV{VCPKG_ROOT}/installed/x64-windows-static/include"
    )
endif()
if(BOOST_UNORDERED_INCLUDE_DIRS)
    message(STATUS "Found boost-unordered at: ${BOOST_UNORDERED_INCLUDE_DIRS}")
else()
    message(WARNING "boost-unordered not found. Make sure it's installed via vcpkg.")
endif()

# srell is header-only, find the include directory
find_path(SRELL_INCLUDE_DIRS "srell.hpp")
if(NOT SRELL_INCLUDE_DIRS)
    # Try common vcpkg locations
    find_path(SRELL_INCLUDE_DIRS "srell.hpp" 
        PATHS
            "${CMAKE_CURRENT_SOURCE_DIR}/build/vcpkg_installed/x64-windows-static/include"
            "$ENV{VCPKG_ROOT}/installed/x64-windows-static/include"
    )
endif()
if(SRELL_INCLUDE_DIRS)
    message(STATUS "Found srell at: ${SRELL_INCLUDE_DIRS}")
else()
    message(WARNING "srell not found. Make sure it's installed via vcpkg.")
endif()

# ClibUtil is part of powerof3's CommonLibSSE (not CommonLibSSE-NG)
# Try to find it in common locations
find_path(CLIBUTIL_INCLUDE_DIRS 
    "ClibUtil/simpleINI.hpp" 
    PATHS 
        "${CMAKE_CURRENT_SOURCE_DIR}/extern/commonlibsse/include"
        "$ENV{CommonLibSSEPath}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/build/_deps/commonlibsse-ng-src/include"
    NO_DEFAULT_PATH
)
if(NOT CLIBUTIL_INCLUDE_DIRS)
    # If not found, try vcpkg/system paths
    find_path(CLIBUTIL_INCLUDE_DIRS "ClibUtil/simpleINI.hpp")
endif()
if(CLIBUTIL_INCLUDE_DIRS)
    message(STATUS "Found ClibUtil at: ${CLIBUTIL_INCLUDE_DIRS}")
else()
    message(WARNING "ClibUtil not found. You may need to set CommonLibSSEPath environment variable or add extern/commonlibsse submodule.")
endif()

add_library(glyph SHARED
    src/main.cpp
    src/PCH.hpp
    src/ConsoleCommands.hpp
    src/ConsoleCommands.cpp
    src/Settings.cpp
    src/Settings.hpp
    src/SettingsBinding.hpp
    src/Renderer.cpp
    src/Renderer.hpp
    src/RendererInternal.hpp
    src/RendererSnapshot.cpp
    src/RendererLayout.cpp
    src/RendererEffects.cpp
    src/GameState.hpp
    src/GameState.cpp
    src/Hooks.cpp
    src/Hooks.hpp
    src/TextEffects.hpp
    src/TextEffectsInternal.hpp
    src/TextEffectsCore.cpp
    src/TextEffectsGradient.cpp
    src/TextEffectsAnimated.cpp
    src/TextEffectsComplex.cpp
    src/TextEffectsParticle.cpp
    src/Occlusion.hpp
    src/Occlusion.cpp
    src/DebugOverlay.hpp
    src/DebugOverlay.cpp
    src/RenderConstants.hpp
    src/RasterQuality.hpp
    src/RenderSampling.hpp
    src/RenderSampling.cpp
    src/NameFit.hpp
    src/ParticleTextures.hpp
    src/ParticleTextures.cpp
    src/BadgeTextures.hpp
    src/BadgeTextures.cpp
    src/DeckUtils.hpp
    src/DeckUtils.cpp
    src/DeckPng.hpp
    src/DeckPng.cpp
    src/Deck.hpp
    src/Deck.cpp
    src/ProjectManifest.hpp
    src/ProjectManifest.cpp
    src/TextPostProcess.hpp
    src/TextPostProcess.cpp
    src/SceneMeter.hpp
    src/SceneMeter.cpp
    src/DepthClip.hpp
    src/DepthClip.cpp
    src/GraffitoMath.hpp
    src/GraffitoShaderContract.hpp
    src/Graffito.hpp
    src/Graffito.cpp
    src/TrueHUDAPI.h
    src/HudCompat.hpp
    src/HudCompat.cpp
    src/Utf8Utils.hpp
    src/Version.hpp
)

target_include_directories(glyph PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${RAPIDCSV_INCLUDE_DIRS}
)

# Add ClibUtil include directory if found
if(CLIBUTIL_INCLUDE_DIRS)
    target_include_directories(glyph PRIVATE ${CLIBUTIL_INCLUDE_DIRS})
endif()

# Add boost-unordered include directory if found
if(BOOST_UNORDERED_INCLUDE_DIRS)
    target_include_directories(glyph PRIVATE ${BOOST_UNORDERED_INCLUDE_DIRS})
endif()

# Add srell include directory if found
if(SRELL_INCLUDE_DIRS)
    target_include_directories(glyph PRIVATE ${SRELL_INCLUDE_DIRS})
endif()

target_link_libraries(glyph PRIVATE
    CommonLibSSE
    imgui::imgui
    spdlog::spdlog
    xbyak::xbyak
    NanoSVG::nanosvg
    NanoSVG::nanosvgrast
    nlohmann_json::nlohmann_json
    ole32
    windowscodecs
)
# Note: boost-unordered and srell are header-only, no linking needed

# ============================================================================
# Testing Support
# ============================================================================
option(BUILD_GLYPH_TESTS "Build glyph unit tests" ON)

if(BUILD_GLYPH_TESTS)
    find_package(GTest CONFIG REQUIRED)

    # Test executable for utility functions
    add_executable(glyph_test_utils tests/test_utils.cpp)
    target_include_directories(glyph_test_utils PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(glyph_test_utils PRIVATE cxx_std_17)
    target_link_libraries(glyph_test_utils PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(glyph_test_utils PRIVATE /W4)
    endif()

    # Test executable for settings parsing
    add_executable(glyph_test_settings tests/test_settings.cpp)
    target_compile_features(glyph_test_settings PRIVATE cxx_std_17)
    target_link_libraries(glyph_test_settings PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(glyph_test_settings PRIVATE /W4)
    endif()

    # Test executable for contextual label/token classification and FormatString
    add_executable(glyph_test_label_format tests/test_label_format.cpp)
    target_compile_features(glyph_test_label_format PRIVATE cxx_std_17)
    target_link_libraries(glyph_test_label_format PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(glyph_test_label_format PRIVATE /W4)
    endif()

    # Test executable for Graffito's runtime-independent plane math
    add_executable(glyph_test_graffito tests/test_graffito.cpp)
    target_include_directories(glyph_test_graffito PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(glyph_test_graffito PRIVATE cxx_std_17)
    target_link_libraries(glyph_test_graffito PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(glyph_test_graffito PRIVATE /W4)
    endif()

    # Test executable for runtime-independent card helpers
    add_executable(glyph_test_deck tests/test_deck.cpp src/DeckUtils.cpp src/DeckPng.cpp)
    target_include_directories(glyph_test_deck PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(glyph_test_deck PRIVATE cxx_std_23)
    target_link_libraries(glyph_test_deck PRIVATE
        GTest::gtest
        GTest::gtest_main
        ole32
        windowscodecs
    )
    if(MSVC)
        target_compile_options(glyph_test_deck PRIVATE /W4)
    endif()

    # Enable CTest integration with Google Test
    include(GoogleTest)
    enable_testing()
    gtest_discover_tests(glyph_test_utils)
    gtest_discover_tests(glyph_test_settings)
    gtest_discover_tests(glyph_test_label_format)
    gtest_discover_tests(glyph_test_graffito)
    gtest_discover_tests(glyph_test_deck)
endif()
