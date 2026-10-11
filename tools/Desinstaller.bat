@echo off
setlocal EnableDelayedExpansion
rem KenshiCoop uninstaller: removes the line "Plugin=KenshiCoop" from Plugins_x64.cfg and the DLL.
rem KenshiCoop.ini and the logs are kept. Usage: double-click, or: Desinstaller.bat "X:\...\Kenshi"

set "KDIR=%~1"
if defined KDIR goto check
if exist "%~dp0kenshi_x64.exe" set "KDIR=%~dp0" & goto check
set "STEAM="
for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| findstr /i SteamPath') do set "STEAM=%%b"
if not defined STEAM set "STEAM=%ProgramFiles(x86)%\Steam"
set "STEAM=%STEAM:/=\%"
if exist "%STEAM%\steamapps\common\Kenshi\kenshi_x64.exe" set "KDIR=%STEAM%\steamapps\common\Kenshi" & goto check
if exist "%STEAM%\steamapps\libraryfolders.vdf" (
    for /f usebackq^ tokens^=4^ delims^=^" %%p in (`findstr /c:"\"path\"" "%STEAM%\steamapps\libraryfolders.vdf"`) do (
        set "LIB=%%p"
        set "LIB=!LIB:\\=\!"
        if exist "!LIB!\steamapps\common\Kenshi\kenshi_x64.exe" set "KDIR=!LIB!\steamapps\common\Kenshi"
    )
)
if defined KDIR goto check
echo Kenshi introuvable : glissez le dossier de Kenshi sur Desinstaller.bat.
goto end

:check
if "%KDIR:~-1%"=="\" set "KDIR=%KDIR:~0,-1%"
tasklist /fi "imagename eq kenshi_x64.exe" 2>nul | find /i "kenshi_x64.exe" >nul && (echo Fermez Kenshi d'abord. & goto end)
set "CFG=%KDIR%\Plugins_x64.cfg"
if exist "%CFG%" (
    findstr /v /x /c:"Plugin=KenshiCoop" "%CFG%" > "%CFG%.tmp"
    move /y "%CFG%.tmp" "%CFG%" >nul
)
if exist "%KDIR%\KenshiCoop.dll" del /q "%KDIR%\KenshiCoop.dll"
if exist "%KDIR%\KenshiCoop.pdb" del /q "%KDIR%\KenshiCoop.pdb"
echo KenshiCoop retire (KenshiCoop.ini et les journaux sont gardes).

:end
echo.
pause
