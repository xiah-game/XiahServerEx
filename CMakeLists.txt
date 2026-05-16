cmake_minimum_required(VERSION 3.10)
project(XiahServerEx)

set(CMAKE_CXX_STANDARD 14)

# Set Windows definition
add_definitions(-DWIN32 -D_WINDOWS -DNDEBUG -D_CONSOLE)

# Include directories (allowing us to use client headers)
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/../XiahClient
)

# Source files
set(SOURCES
    main.cpp
    ServerCore.cpp
    AuthServer.cpp
    UnitServer.cpp
    MonsterAI.cpp
    DBHelper.cpp
    Network/PacketRouter.cpp
    Network/SessionMgr.cpp
    Network/SystemMessage.cpp
    Handlers/InitHandlers.cpp
    Handlers/LoginHandler.cpp
    Handlers/ItemHandler.cpp
    Handlers/SlotHandler.cpp
    Handlers/CharHandler.cpp
    Handlers/MapHandler.cpp
    Handlers/MoveHandler.cpp
    Handlers/CombatHandler.cpp
    Handlers/NpcHandler.cpp
    Handlers/MugongHandler.cpp
    Handlers/ChatHandler.cpp
    Handlers/PartyHandler.cpp
    Handlers/RebuildItemHandler.cpp
    Handlers/BankHandler.cpp
    GameObjects/DropManager.cpp
    GameObjects/MugongManager.cpp
    GameObjects/PlayerManager.cpp
    GameObjects/MapInstance.cpp
)

add_executable(XiahServerEx ${SOURCES})

# Disable aggressive optimization in Release to avoid UB from uninitialized vars
if(MSVC)
    target_compile_options(XiahServerEx PRIVATE /W3)
    # Release: use /O1 (size opt) instead of /O2 (speed opt) for safer behavior
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O1")
endif()

# Link Winsock and ODBC
target_link_libraries(XiahServerEx wsock32 ws2_32 odbc32)
