@echo off
REM ============================================================================
REM test.bat - Run whois plugin unit tests using Google Test
REM ============================================================================
REM This script:
REM   1. Configures CMake if needed
REM   2. Builds the test executables
REM   3. Runs all Google Test executables
REM ============================================================================

setlocal

echo ============================================================================
echo                            WHOIS TEST RUNNER
echo ============================================================================
echo.

REM ============================================================================
REM STEP 1: Configure CMake
REM ============================================================================
echo [1/3] Checking CMake configuration...
echo ----------------------------------------------------------------------------
if not exist "build\CMakeCache.txt" (
    echo   Configuring CMake...
    cmake --preset vs2022-windows
    if errorlevel 1 (
        echo ERROR: CMake configuration failed!
        pause
        exit /b 1
    )
) else (
    echo   Found existing configuration
)
echo.

REM ============================================================================
REM STEP 2: Build Tests
REM ============================================================================
echo [2/3] Building test executables...
echo ----------------------------------------------------------------------------
cmake --build build --config Release --target whois_test_utils whois_test_settings
if errorlevel 1 (
    echo ERROR: Build failed!
    pause
    exit /b 1
)
echo.

REM ============================================================================
REM STEP 3: Run Tests
REM ============================================================================
echo [3/3] Running tests...
echo ----------------------------------------------------------------------------
echo.

set ALL_PASSED=1

REM Run utils tests
echo === whois_test_utils ===
if exist "build\Release\whois_test_utils.exe" (
    build\Release\whois_test_utils.exe --gtest_color=yes
    if errorlevel 1 set ALL_PASSED=0
) else if exist "build\whois_test_utils.exe" (
    build\whois_test_utils.exe --gtest_color=yes
    if errorlevel 1 set ALL_PASSED=0
) else (
    echo ERROR: whois_test_utils.exe not found!
    set ALL_PASSED=0
)
echo.

REM Run settings tests
echo === whois_test_settings ===
if exist "build\Release\whois_test_settings.exe" (
    build\Release\whois_test_settings.exe --gtest_color=yes
    if errorlevel 1 set ALL_PASSED=0
) else if exist "build\whois_test_settings.exe" (
    build\whois_test_settings.exe --gtest_color=yes
    if errorlevel 1 set ALL_PASSED=0
) else (
    echo ERROR: whois_test_settings.exe not found!
    set ALL_PASSED=0
)
echo.

REM ============================================================================
REM SUMMARY
REM ============================================================================
echo ============================================================================
if %ALL_PASSED%==1 (
    echo                            ALL TESTS PASSED
) else (
    echo                            SOME TESTS FAILED
)
echo ============================================================================

pause
if %ALL_PASSED%==1 (
    exit /b 0
) else (
    exit /b 1
)
