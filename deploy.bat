@echo off
REM ============================================================================
REM deploy.bat - Deploy whois plugin to Skyrim
REM ============================================================================
REM This script:
REM   1. Verifies the build exists
REM   2. Creates target directories if needed
REM   3. Copies the plugin DLL
REM   4. Copies the configuration INI
REM   5. Copies resources
REM ============================================================================

setlocal

echo ============================================================================
echo                            WHOIS DEPLOY SCRIPT
echo ============================================================================
echo.

:: MO2 overwrite folder path (override with WHOIS_DEPLOY_PATH env var)
if defined WHOIS_DEPLOY_PATH (
    set "DEPLOY_PATH=%WHOIS_DEPLOY_PATH%"
) else (
    set "DEPLOY_PATH=D:\Nolvus\Instance\MODS\overwrite"
)

REM ============================================================================
REM STEP 1: Verify Build
REM ============================================================================
echo [1/4] Verifying build...
echo ----------------------------------------------------------------------------
if not exist "build\Release\whois.dll" (
    echo ERROR: build\Release\whois.dll not found
    echo Run build.bat first
    exit /b 1
)
echo   Found: build\Release\whois.dll
echo.

REM ============================================================================
REM STEP 2: Copy Plugin DLL
REM ============================================================================
echo [2/4] Copying plugin DLL...
echo ----------------------------------------------------------------------------

:: Create target directories if needed
if not exist "%DEPLOY_PATH%\SKSE\Plugins\whois" (
    mkdir "%DEPLOY_PATH%\SKSE\Plugins\whois"
)

copy /Y "build\Release\whois.dll" "%DEPLOY_PATH%\SKSE\Plugins\whois.dll"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to copy DLL
    exit /b %ERRORLEVEL%
)
echo.

REM ============================================================================
REM STEP 3: Copy Configuration
REM ============================================================================
echo [3/4] Copying configuration...
echo ----------------------------------------------------------------------------
copy /Y "skse\plugins\whois.ini" "%DEPLOY_PATH%\SKSE\Plugins\whois.ini"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to copy INI
    exit /b %ERRORLEVEL%
)
echo.

REM ============================================================================
REM STEP 4: Copy Resources
REM ============================================================================
echo [4/4] Copying resources...
echo ----------------------------------------------------------------------------
xcopy /E /I /Y "skse\plugins\whois" "%DEPLOY_PATH%\SKSE\Plugins\whois"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to copy resources
    exit /b %ERRORLEVEL%
)
echo.

REM ============================================================================
REM SUMMARY
REM ============================================================================
echo ============================================================================
echo                            DEPLOY COMPLETE
echo ============================================================================
echo.
echo Target: %DEPLOY_PATH%
echo.
echo Deployed Files:
echo   - SKSE\Plugins\whois.dll
echo   - SKSE\Plugins\whois.ini
echo   - SKSE\Plugins\whois\  (assets)
echo.
echo  *** Launch Skyrim through MO2 to test the plugin ***
echo.
echo ============================================================================

endlocal
pause
