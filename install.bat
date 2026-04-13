@echo off
REM SimpleSet NFC Configurator — Full Installer
REM
REM Checks for MultiOne, extracts passwords, and installs the bridge DLL.
REM Also sets up the CLI tool with the extracted password.
REM
REM Run as Administrator if MultiOne is in Program Files.

setlocal enabledelayedexpansion

echo ============================================================
echo  SimpleSet NFC Configurator — Installer
echo ============================================================
echo.

REM --- Check Python ---
where python >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: Python is not installed or not in PATH.
    echo   Download from https://python.org/downloads
    echo   Make sure to check "Add to PATH" during install.
    exit /b 1
)
echo [OK] Python found

REM --- Check pyserial ---
python -c "import serial" >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [..] Installing pyserial...
    pip install pyserial >nul 2>&1
    python -c "import serial" >nul 2>&1
    if %ERRORLEVEL% neq 0 (
        echo ERROR: Failed to install pyserial.
        echo   Run: pip install pyserial
        exit /b 1
    )
)
echo [OK] pyserial installed

REM --- Find MultiOne ---
set "MODIR="
for %%D in (
    "%ProgramFiles%\Signify\MultiOne"
    "%ProgramFiles(x86)%\Signify\MultiOne"
    "%ProgramFiles%\MultiOne"
    "%ProgramFiles(x86)%\MultiOne"
) do (
    if exist "%%~D\MultiOne.exe" (
        set "MODIR=%%~D"
        goto :mo_found
    )
)

echo.
echo WARNING: MultiOne is not installed.
echo   The CLI tool will still work, but you need to extract the
echo   RF password manually if you have the original DLL.
echo.
echo   To install MultiOne, download it from Signify's website.
echo.
goto :cli_only

:mo_found
echo [OK] MultiOne found at: %MODIR%

REM --- Find original DLL ---
set "ORIG_DLL="
if exist "%MODIR%\NfcCommandsHandler.dll.original" (
    set "ORIG_DLL=%MODIR%\NfcCommandsHandler.dll.original"
) else if exist "%MODIR%\NfcCommandsHandler.dll" (
    set "ORIG_DLL=%MODIR%\NfcCommandsHandler.dll"
)

if "%ORIG_DLL%"=="" (
    echo ERROR: NfcCommandsHandler.dll not found in MultiOne directory.
    exit /b 1
)

REM --- Extract password ---
echo.
echo Extracting RF password...
python "%~dp0cli\extract_passwords.py" "%ORIG_DLL%"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Password extraction failed.
    exit /b 1
)

REM --- Copy passwords.json to CLI dir ---
if exist "passwords.json" (
    copy /y "passwords.json" "%~dp0cli\passwords.json" >nul
    echo [OK] Password file saved to cli\passwords.json
)

REM --- Install bridge ---
echo.
choice /M "Install the bridge DLL into MultiOne (allows MultiOne to use ESP32)"
if errorlevel 2 goto :cli_only

call "%~dp0bridge\install.bat" "%MODIR%"
goto :done

:cli_only
echo.
echo CLI-only setup. To use the CLI tool:
echo   cd cli
echo   python simpleset_cli.py info
echo.
echo If you have the original NfcCommandsHandler.dll, extract the password:
echo   python cli\extract_passwords.py path\to\NfcCommandsHandler.dll

:done
echo.
echo ============================================================
echo  Setup complete!
echo.
echo  CLI usage:
echo    cd cli
echo    python simpleset_cli.py info
echo    python simpleset_cli.py getcurrent
echo    python simpleset_cli.py setcurrent 800
echo ============================================================

endlocal
