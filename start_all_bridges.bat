@echo off
title Azeroth AI Bridges Launcher (Dual-Bridge Orchestrator)
setlocal enabledelayedexpansion

echo ===============================================================================
echo       AzerothCore AI Dual-Bridge Orchestrator
echo       1. mod-llm-chatter Bridge   (Fast Ambient Dialogue / Banter)
echo       2. mod-azeroth-friend Bridge (Tactical Planning / Fast RAM Cache)
echo ===============================================================================
echo.

set "SCRIPT_DIR=%~dp0"
set "CHATTER_DIR=%~dp0..\mod-llm-chatter"
set "FRIEND_DIR=%~dp0"

:: 1. Verify Python availability
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Python was not found in your system PATH!
    echo Please ensure Python 3.8+ is installed and added to PATH.
    pause
    exit /b 1
)

:: 2. Check mod-llm-chatter existence
if not exist "%CHATTER_DIR%\tools\llm_chatter_bridge.py" (
    echo [WARNING] mod-llm-chatter tools not found at "%CHATTER_DIR%\tools"!
    echo Starting mod-azeroth-friend bridge in standalone action mode...
    start "Azeroth Friend Bridge (Planning)" cmd /k "cd /d "%FRIEND_DIR%" && call start_bridge.bat"
    pause
    exit /b 0
)

echo [OK] Both module directories detected.
echo.
echo Launching Bridge 1: mod-llm-chatter (Dialogue and Ambient Observations)...
start "LLM Chatter Bridge (Conversational Model)" cmd /k "cd /d "%CHATTER_DIR%" && call start_bridge.bat"

echo Launching Bridge 2: mod-azeroth-friend (Planning and Fast Sensory Reuse)...
start "Azeroth Friend Bridge (Planning & Action Model)" cmd /k "cd /d "%FRIEND_DIR%" && call start_bridge.bat"

echo.
echo ===============================================================================
echo  Both bridges are now running in separate, fault-isolated console processes!
echo  - Close or Ctrl+C either window independently to restart or adjust configs.
echo  - Coexistence telemetry can be verified in-game with: .af status
echo ===============================================================================
echo.
timeout /t 5
