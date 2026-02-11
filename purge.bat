@echo off
REM ============================================================================
REM purge.bat - Remove whois plugin from deployment path
REM ============================================================================
REM This script removes ONLY the whois plugin files:
REM   - SKSE\Plugins\whois.dll
REM   - SKSE\Plugins\whois.ini
REM   - SKSE\Plugins\whois\
REM ============================================================================

setlocal

echo ============================================================================
echo                            WHOIS PURGE SCRIPT
echo ============================================================================
echo.

:: MO2 overwrite folder path (override with WHOIS_DEPLOY_PATH env var)
if defined WHOIS_DEPLOY_PATH (
    set "DEPLOY_PATH=%WHOIS_DEPLOY_PATH%"
) else (
    set "DEPLOY_PATH=D:\Nolvus\Instance\MODS\overwrite"
)

echo Target: %DEPLOY_PATH%
echo.
echo This will remove:
echo   - SKSE\Plugins\whois.dll
echo   - SKSE\Plugins\whois.ini
echo   - SKSE\Plugins\whois\ (assets)
echo.
echo ============================================================================
choice /C YN /M "Are you sure you want to purge the whois plugin"
if %ERRORLEVEL% neq 1 (
    echo.
    echo Purge cancelled.
    goto :end
)
echo.

REM ============================================================================
REM Remove Plugin DLL
REM ============================================================================
echo [1/3] Removing whois.dll...
if exist "%DEPLOY_PATH%\SKSE\Plugins\whois.dll" (
    del /Q "%DEPLOY_PATH%\SKSE\Plugins\whois.dll"
    if %ERRORLEVEL% equ 0 (
        echo   Removed: whois.dll
    ) else (
        echo   ERROR: Failed to remove whois.dll
    )
) else (
    echo   Skipped: whois.dll not found
)

REM ============================================================================
REM Remove Configuration INI
REM ============================================================================
echo [2/3] Removing whois.ini...
if exist "%DEPLOY_PATH%\SKSE\Plugins\whois.ini" (
    del /Q "%DEPLOY_PATH%\SKSE\Plugins\whois.ini"
    if %ERRORLEVEL% equ 0 (
        echo   Removed: whois.ini
    ) else (
        echo   ERROR: Failed to remove whois.ini
    )
) else (
    echo   Skipped: whois.ini not found
)

REM ============================================================================
REM Remove Resources Folder
REM ============================================================================
echo [3/3] Removing whois folder...
if exist "%DEPLOY_PATH%\SKSE\Plugins\whois" (
    rmdir /S /Q "%DEPLOY_PATH%\SKSE\Plugins\whois"
    if %ERRORLEVEL% equ 0 (
        echo   Removed: whois\ folder
    ) else (
        echo   ERROR: Failed to remove whois\ folder
    )
) else (
    echo   Skipped: whois\ folder not found
)

echo.
echo ============================================================================
echo                            PURGE COMPLETE
echo ============================================================================
echo.

:end
endlocal
pause
