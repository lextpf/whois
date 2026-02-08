@echo off
REM ============================================================================
REM build.bat - Complete build pipeline for whois
REM ============================================================================
REM This script:
REM   1. Configures the project using CMake with the vs2022-windows preset
REM   2. Builds the Release configuration
REM   3. Generates API documentation with doxide (if available)
REM   4. Builds the documentation site with mkdocs (if available)
REM ============================================================================

setlocal enabledelayedexpansion

echo ============================================================================
echo                             WHOIS BUILD PIPELINE
echo ============================================================================
echo.

REM ============================================================================
REM STEP 1: CMake Configuration
REM ============================================================================
echo [1/4] Configuring with CMake...
echo ----------------------------------------------------------------------------
cmake --preset vs2022-windows
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake configuration failed
    exit /b %ERRORLEVEL%
)
echo.

REM ============================================================================
REM STEP 2: Build Release
REM ============================================================================
echo [2/4] Building Release...
echo ----------------------------------------------------------------------------
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo ERROR: Build failed
    exit /b %ERRORLEVEL%
)
echo.

REM ============================================================================
REM STEP 3: Generate API Documentation (doxide)
REM ============================================================================
echo [3/4] Generating API documentation...
echo ----------------------------------------------------------------------------
where doxide >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo SKIP: doxide not found in PATH
) else (
    doxide build
    if %ERRORLEVEL% neq 0 (
        echo ERROR: Doxide failed
        exit /b %ERRORLEVEL%
    )
    python scripts/clean_docs.py
)
echo.

REM ============================================================================
REM STEP 4: Build Documentation Site (mkdocs)
REM ============================================================================
echo [4/4] Building documentation site...
echo ----------------------------------------------------------------------------
where mkdocs >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo SKIP: mkdocs not found in PATH
) else (
    mkdocs build
    if %ERRORLEVEL% neq 0 (
        echo ERROR: MkDocs failed
        exit /b %ERRORLEVEL%
    )
)
echo.

REM ============================================================================
REM SUMMARY
REM ============================================================================
echo ============================================================================
echo                           BUILD PIPELINE COMPLETE
echo ============================================================================
echo.
echo Build Output:
echo   - DLL: build\Release\whois.dll
echo.
echo Documentation:
echo   - API:  docs\api\  (if doxide available)
echo   - Site: site\      (if mkdocs available)
echo.
echo  *** Run deploy.bat to copy the built files to the Skyrim directory ***
echo.
echo ============================================================================

endlocal
pause