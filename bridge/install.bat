@echo off
REM Install the MultiOne bridge into a MultiOne installation directory.
REM
REM Usage:
REM   install.bat                         (auto-detect MultiOne location)
REM   install.bat "C:\path\to\MultiOne"   (specify directory)

setlocal enabledelayedexpansion

echo ============================================================
echo  MultiOne Bridge Installer
echo ============================================================
echo.

REM --- Find MultiOne directory ---
set "MODIR="
if not "%~1"=="" (
    set "MODIR=%~1"
) else (
    REM Try common install locations
    for %%D in (
        "%ProgramFiles%\Signify\MultiOne"
        "%ProgramFiles(x86)%\Signify\MultiOne"
        "%ProgramFiles%\MultiOne"
        "%ProgramFiles(x86)%\MultiOne"
    ) do (
        if exist "%%~D\MultiOne.exe" (
            set "MODIR=%%~D"
            goto :found
        )
    )

    REM Search Program Files
    for /d %%D in ("%ProgramFiles%\Signify\MultiOne*") do (
        if exist "%%~D\MultiOne.exe" (
            set "MODIR=%%~D"
            goto :found
        )
    )
    for /d %%D in ("%ProgramFiles(x86)%\Signify\MultiOne*") do (
        if exist "%%~D\MultiOne.exe" (
            set "MODIR=%%~D"
            goto :found
        )
    )
)

:found
if "%MODIR%"=="" (
    echo ERROR: MultiOne installation not found.
    echo.
    echo Please specify the MultiOne directory:
    echo   install.bat "C:\Program Files\Signify\MultiOne"
    exit /b 1
)

echo Found MultiOne at: %MODIR%
echo.

REM --- Check if bridge is already installed ---
if exist "%MODIR%\NfcCommandsHandler.dll.original" (
    echo Bridge appears to already be installed.
    echo   ^(NfcCommandsHandler.dll.original exists^)
    echo.
    choice /M "Reinstall"
    if errorlevel 2 exit /b 0
)

REM --- Check MultiOne is not running ---
tasklist /FI "IMAGENAME eq MultiOne.exe" 2>nul | find /I "MultiOne.exe" >nul
if %ERRORLEVEL% == 0 (
    echo WARNING: MultiOne is running. It must be closed to install the bridge.
    choice /M "Close MultiOne now"
    if errorlevel 2 (
        echo Aborted.
        exit /b 1
    )
    taskkill /F /IM MultiOne.exe >nul 2>&1
    timeout /t 2 >nul
)

REM --- Back up original DLL ---
if not exist "%MODIR%\NfcCommandsHandler.dll.original" (
    echo Backing up original DLL...
    copy /y "%MODIR%\NfcCommandsHandler.dll" "%MODIR%\NfcCommandsHandler.dll.original" >nul
    if errorlevel 1 (
        echo ERROR: Failed to back up original DLL. Run as Administrator?
        exit /b 1
    )
    echo   Saved as NfcCommandsHandler.dll.original
)

REM --- Install bridge DLL ---
echo Installing bridge DLL...
copy /y "%~dp0bin\NfcCommandsHandler.dll" "%MODIR%\NfcCommandsHandler.dll" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy bridge DLL. Run as Administrator?
    exit /b 1
)
echo   Installed NfcCommandsHandler.dll

REM --- Extract password ---
echo.
echo Extracting RF password from original DLL...
where python >nul 2>&1
if %ERRORLEVEL% == 0 (
    python "%~dp0..\cli\extract_passwords.py" "%MODIR%\NfcCommandsHandler.dll.original"
    if exist "passwords.json" (
        move /y passwords.json "%MODIR%\passwords.json" >nul
        echo   Password file installed.
    )
) else (
    echo WARNING: Python not found. Cannot auto-extract password.
    echo   Install Python and run:
    echo     python cli\extract_passwords.py "%MODIR%\NfcCommandsHandler.dll.original"
)

REM --- Create INI file ---
if not exist "%MODIR%\multione_bridge.ini" (
    echo.
    echo Creating multione_bridge.ini...
    echo port=auto> "%MODIR%\multione_bridge.ini"
    echo   Created multione_bridge.ini ^(port=auto^)
    echo   Edit %MODIR%\multione_bridge.ini to set your COM port if auto-detect fails.
)

echo.
echo ============================================================
echo  Installation complete!
echo.
echo  Next steps:
echo    1. Connect your ESP32+PN5180 via USB
echo    2. Launch MultiOne
echo    3. Place a SimpleSet LED driver on the NFC reader
echo ============================================================

endlocal
