@echo off
title AzerothFriend LLM Bridge
cd /d "%~dp0tools"
echo ===================================================
echo     Starting mod-azeroth-friend LLM Bridge
echo ===================================================
echo.
set "CONFIG_PATH=..\..\..\Azerothcore server\Server\etc\modules\mod_azeroth_friend.conf"
if exist "..\conf\mod_azeroth_friend.conf" set "CONFIG_PATH=..\conf\mod_azeroth_friend.conf"
if exist "..\..\..\Azerothcore server\Server\bin\configs\modules\mod_azeroth_friend.conf" set "CONFIG_PATH=..\..\..\Azerothcore server\Server\bin\configs\modules\mod_azeroth_friend.conf"

python azeroth_friend_bridge.py --config "%CONFIG_PATH%"
pause
