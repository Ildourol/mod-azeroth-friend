set(mod_azeroth_friend_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendActionDispatcher.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendActionRegistry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendAiControl.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendAutopilot.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendAutopilotSchema.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendBotController.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendBuffs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendChatHook.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendCommand.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendConfig.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendEnvironment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendErrands.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendExpression.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendLiveState.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendMonitor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendMovement.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendPlayerbotActions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendProgress.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendRoute.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendScriptLoader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendShared.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendSnapshotMemory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendStrategy.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendTravel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendWorldIndex.cpp
)

if(TARGET modules)
    target_sources(modules PRIVATE ${mod_azeroth_friend_SOURCES})
    target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
endif()
