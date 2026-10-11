@echo off
setlocal EnableDelayedExpansion
rem KenshiCoop installer: copies KenshiCoop.dll into the Kenshi folder and adds the line
rem "Plugin=KenshiCoop" to Plugins_x64.cfg (Kenshi then loads it as one of its Ogre plugins).
rem Nothing else: no PowerShell, no download, no registry write.
rem Usage: double-click, or: Installer.bat "D:\SteamLibrary\steamapps\common\Kenshi"

set "KDIR=%~1"
if defined KDIR goto check

rem 1. this folder, if the zip was unpacked inside Kenshi's
if exist "%~dp0kenshi_x64.exe" set "KDIR=%~dp0" & goto check

rem 2. Steam's own folder, then every Steam library listed in libraryfolders.vdf
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
echo Kenshi introuvable dans les bibliotheques Steam.
echo Glissez le dossier de Kenshi sur Installer.bat, ou copiez KenshiCoop.dll dans le dossier de Kenshi
echo et ajoutez la ligne  Plugin=KenshiCoop  a la fin de Plugins_x64.cfg.
goto end

:check
if "%KDIR:~-1%"=="\" set "KDIR=%KDIR:~0,-1%"
if not exist "%KDIR%\kenshi_x64.exe" echo kenshi_x64.exe absent de "%KDIR%". & goto end
tasklist /fi "imagename eq kenshi_x64.exe" 2>nul | find /i "kenshi_x64.exe" >nul && (echo Fermez Kenshi d'abord. & goto end)
if not exist "%~dp0KenshiCoop.dll" echo KenshiCoop.dll absent a cote de Installer.bat. & goto end

set "CFG=%KDIR%\Plugins_x64.cfg"
if not exist "%CFG%.before-kenshicoop" copy /y "%CFG%" "%CFG%.before-kenshicoop" >nul
copy /y "%~dp0KenshiCoop.dll" "%KDIR%\" >nul || (echo Copie impossible dans "%KDIR%". & goto end)
findstr /x /c:"Plugin=KenshiCoop" "%CFG%" >nul || (echo Plugin=KenshiCoop>>"%CFG%")
echo KenshiCoop installe dans "%KDIR%".
echo Reglages : "%KDIR%\KenshiCoop.ini" (cree au premier lancement).

:end
echo.
pause
