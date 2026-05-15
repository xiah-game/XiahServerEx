#pragma once

#include <string>
#include <vector>
#include <winsock2.h>
#include "../ServerCore.h"

class SystemMessage {
public:
    // Enumeration for standard system messages (from 1080 PCAP)
    enum class MsgType : WORD {
        PICK_ITEM = 1,
        BOSS_KILL = 2
    };

    // Sends a CS_IF_HELPMESSAGE_ACK (0x3B3F) to the client
    // Useful for showing "Gained Money", "Picked up item" etc.
    static void SendHelpMessage(SOCKET clientSocket, MsgType type, const std::string& szName, DWORD dwAmount);
};
