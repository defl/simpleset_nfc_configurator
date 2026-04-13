@echo off
REM Build NfcCommandsHandler.dll bridge (32-bit DLL)
REM
REM Usage:
REM   Option 1 (MSVC): Open "x86 Native Tools Command Prompt" and run this.
REM   Option 2 (MinGW): Ensure i686-w64-mingw32-gcc is on PATH.
REM
REM Output: bin\NfcCommandsHandler.dll (32-bit)

setlocal

set SRC=multione_bridge.c
set DEF=multione_bridge.def
set OUT=bin\NfcCommandsHandler.dll

if not exist bin mkdir bin

REM --- Try MSVC ---
where cl >nul 2>&1
if %ERRORLEVEL% == 0 (
    echo [BUILD] Using MSVC cl.exe
    cl /nologo /O2 /LD /W3 %SRC% /Fe:%OUT% /link /DEF:%DEF% /OUT:%OUT% kernel32.lib user32.lib advapi32.lib
    if %ERRORLEVEL% == 0 (
        echo [BUILD] Success: %OUT%
        del /q multione_bridge.obj multione_bridge.exp multione_bridge.lib 2>nul
        del /q bin\NfcCommandsHandler.exp bin\NfcCommandsHandler.lib 2>nul
        goto :done
    )
    echo [BUILD] MSVC failed, trying MinGW...
)

REM --- Try MinGW-w64 (32-bit) ---
where i686-w64-mingw32-gcc >nul 2>&1
if %ERRORLEVEL% == 0 (
    echo [BUILD] Using MinGW i686-w64-mingw32-gcc
    i686-w64-mingw32-gcc -shared -O2 -Wall -o %OUT% %SRC% %DEF% -lkernel32
    if %ERRORLEVEL% == 0 (
        echo [BUILD] Success: %OUT%
        goto :done
    )
)

REM --- Try native gcc ---
where gcc >nul 2>&1
if %ERRORLEVEL% == 0 (
    echo [BUILD] Using native gcc
    gcc -shared -O2 -Wall -o %OUT% %SRC% %DEF% -lkernel32
    if %ERRORLEVEL% == 0 (
        echo [BUILD] Success: %OUT%
        goto :done
    )
)

echo [BUILD] ERROR: No compiler found.
echo [BUILD] Install Visual Studio Build Tools or MinGW-w64.
exit /b 1

:done
echo.
echo Output: %OUT%
endlocal
