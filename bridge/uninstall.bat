@echo off
REM Uninstall the MultiOne bridge and restore the original FEIG DLL.
REM
REM Usage:
REM   uninstall.bat                         (auto-detect)
REM   uninstall.bat "C:\path\to\MultiOne"   (specify directory)

setlocal enabledelayedexpansion

echo ============================================================
echo  MultiOne Bridge Uninstaller
echo ============================================================
echo.

REM --- Find MultiOne directory ---
set "MODIR="
if not "%~1"=="" (
    set "MODIR=%~1"
) else (
    for %%D in (
        "%ProgramFiles%\Signify\MultiOne"
        "%ProgramFiles(x86)%\Signify\MultiOne"
        "%ProgramFiles%\MultiOne"
        "%ProgramFiles(x86)%\MultiOne"
    ) do (
        if exist "%%~D\NfcCommandsHandler.dll.original" (
            set "MODIR=%%~D"
            goto :found
        )
    )
)

:found
if "%MODIR%"=="" (
    echo ERROR: No bridge installation found.
    echo   ^(Looking for NfcCommandsHandler.dll.original^)
    exit /b 1
)

echo Found installation at: %MODIR%
echo.

REM --- Check MultiOne is not running ---
tasklist /FI "IMAGENAME eq MultiOne.exe" 2>nul | find /I "MultiOne.exe" >nul
if %ERRORLEVEL% == 0 (
    echo WARNING: MultiOne is running. It must be closed first.
    choice /M "Close MultiOne now"
    if errorlevel 2 (
        echo Aborted.
        exit /b 1
    )
    taskkill /F /IM MultiOne.exe >nul 2>&1
    timeout /t 2 >nul
)

REM --- Restore original DLL ---
echo Restoring original DLL...
copy /y "%MODIR%\NfcCommandsHandler.dll.original" "%MODIR%\NfcCommandsHandler.dll" >nul
if errorlevel 1 (
    echo ERROR: Failed to restore DLL. Run as Administrator?
    exit /b 1
)

REM --- Clean up bridge files ---
echo Cleaning up bridge files...
del /q "%MODIR%\multione_bridge.ini" 2>nul
del /q "%MODIR%\multione_bridge.log" 2>nul
del /q "%MODIR%\passwords.json" 2>nul
del /q "%MODIR%\NfcCommandsHandler.dll.original" 2>nul

echo.
echo ============================================================
echo  Uninstall complete! Original FEIG driver restored.
echo ============================================================

endlocal
