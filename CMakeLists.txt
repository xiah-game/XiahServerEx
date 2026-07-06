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
    Network/PacketBuilder.cpp
    Network/AuthCenter.cpp
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
    Handlers/MugongPreAttack.cpp
    Handlers/MugongSkill_Summon.cpp
    Handlers/MugongSkill_FiveElm.cpp
    Handlers/MugongSkill_PetCapture.cpp
    Handlers/PetGiveItemHandler.cpp
    Handlers/ChatHandler.cpp
    Handlers/PartyHandler.cpp
    Handlers/RebuildItemHandler.cpp
    Handlers/ItemSerializer.cpp
    Handlers/BankHandler.cpp
    Handlers/StatHandler.cpp
    Handlers/TradeHandler.cpp
    Handlers/ShopHandler.cpp
    Handlers/RepairHandler.cpp
    Handlers/MunpaHandler.cpp
    Handlers/FiveElmHandler.cpp
    Handlers/WorldTimeHandler.cpp
    GameObjects/DropManager.cpp
    GameObjects/ExpSystem.cpp
    DB/CharacterDB.cpp
    DB/ItemDB.cpp
    DB/GameDataDB.cpp
    GameObjects/MugongManager.cpp
    GameObjects/PlayerManager.cpp
    GameObjects/TitleManager.cpp
    GameObjects/MapInstance.cpp
)

add_executable(XiahServerEx ${SOURCES})

# Disable aggressive optimization in Release to avoid UB from uninitialized vars
if(MSVC)
    target_compile_options(XiahServerEx PRIVATE /W3 /utf-8)
    # Release: use /O1 (size opt) instead of /O2 (speed opt) for safer behavior
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O1")
endif()

# Link Winsock and ODBC
target_link_libraries(XiahServerEx wsock32 ws2_32 odbc32)
