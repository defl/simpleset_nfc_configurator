@echo off
REM SimpleSet NFC Configurator — Uninstaller
REM
REM Restores the original FEIG NFC DLL in MultiOne.
REM Does not remove the CLI tool or firmware.

setlocal

echo ============================================================
echo  SimpleSet NFC Configurator — Uninstaller
echo ============================================================
echo.

REM --- Find bridge installation ---
set "MODIR="
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

:found
if "%MODIR%"=="" (
    echo No bridge installation found. Nothing to uninstall.
    exit /b 0
)

echo Found bridge at: %MODIR%
echo.

call "%~dp0bridge\uninstall.bat" "%MODIR%"

REM --- Clean up local password files ---
del /q "%~dp0cli\passwords.json" 2>nul
del /q "%~dp0passwords.json" 2>nul

echo.
echo Local password files cleaned up.
echo.

endlocal
