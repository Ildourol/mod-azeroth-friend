set(mod_azeroth_friend_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendActionDispatcher.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendActionRegistry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendAiControl.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendBotController.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendChatHook.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendCommand.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendConfig.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendEnvironment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendLiveState.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendPlayerbotActions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendScriptLoader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendShared.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AzerothFriendSnapshotMemory.cpp
)

if(TARGET modules)
    target_sources(modules PRIVATE ${mod_azeroth_friend_SOURCES})
    target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
endif()
