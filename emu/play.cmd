@echo off
rem Launch the interactive player from anywhere: cd to the project root first,
rem because the emulator loads roms/ relative to the working directory.
setlocal
cd /d "%~dp0.."
if not exist "emu\digimon-player.exe" (
	echo emu\digimon-player.exe is missing - build it with:
	echo   powershell -NoProfile -ExecutionPolicy Bypass -File .\emu\build.ps1
	exit /b 1
)
start "" "emu\digimon-player.exe" >nul 2>nul
endlocal
