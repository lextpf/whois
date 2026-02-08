cmake_minimum_required(VERSION 3.19)

project(whois LANGUAGES CXX)

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

# Disable building tests for CommonLibSSE-NG
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# Enable Xbyak support for SKSE, needed for hook_function_prologue
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)

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

find_package(imgui CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)
find_package(xbyak CONFIG REQUIRED)
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

add_library(whois SHARED
    src/main.cpp
    src/PCH.h
    src/Settings.cpp
    src/Settings.h
    src/Renderer.cpp
    src/Renderer.h
    src/Hooks.cpp
    src/Hooks.h
    src/TextEffects.h
    src/TextEffects.cpp
    src/Occlusion.h
    src/Occlusion.cpp
    src/DebugOverlay.h
    src/DebugOverlay.cpp
    src/RenderConstants.h
    src/AppearanceTemplate.h
    src/AppearanceTemplate.cpp
    src/ParticleTextures.h
    src/ParticleTextures.cpp
    src/Version.h
)

target_include_directories(whois PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external
    ${CMAKE_CURRENT_SOURCE_DIR}/external/skse64
    ${RAPIDCSV_INCLUDE_DIRS}
)

# Add ClibUtil include directory if found
if(CLIBUTIL_INCLUDE_DIRS)
    target_include_directories(whois PRIVATE ${CLIBUTIL_INCLUDE_DIRS})
endif()

# Add boost-unordered include directory if found
if(BOOST_UNORDERED_INCLUDE_DIRS)
    target_include_directories(whois PRIVATE ${BOOST_UNORDERED_INCLUDE_DIRS})
endif()

# Add srell include directory if found
if(SRELL_INCLUDE_DIRS)
    target_include_directories(whois PRIVATE ${SRELL_INCLUDE_DIRS})
endif()

# Define Skyrim version (1.5.97 SE)
target_compile_definitions(whois PRIVATE
    SKYRIM_SE
)

target_link_libraries(whois PRIVATE
    CommonLibSSE
    imgui::imgui
    spdlog::spdlog
    minhook::minhook
    xbyak::xbyak
)
# Note: boost-unordered and srell are header-only, no linking needed

# ============================================================================
# Testing Support
# ============================================================================
option(BUILD_WHOIS_TESTS "Build whois unit tests" ON)

if(BUILD_WHOIS_TESTS)
    find_package(GTest CONFIG REQUIRED)

    # Test executable for utility functions
    add_executable(whois_test_utils tests/test_utils.cpp)
    target_compile_features(whois_test_utils PRIVATE cxx_std_17)
    target_link_libraries(whois_test_utils PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(whois_test_utils PRIVATE /W4)
    endif()

    # Test executable for settings parsing
    add_executable(whois_test_settings tests/test_settings.cpp)
    target_compile_features(whois_test_settings PRIVATE cxx_std_17)
    target_link_libraries(whois_test_settings PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(whois_test_settings PRIVATE /W4)
    endif()

    # Enable CTest integration with Google Test
    include(GoogleTest)
    enable_testing()
    gtest_discover_tests(whois_test_utils)
    gtest_discover_tests(whois_test_settings)
endif()
