#pragma once
#include <winsock2.h>
#include <windows.h>
#include "../ServerCore.h"

class PlayerManager {
public:
    static PlayerManager& GetInstance() {
        static PlayerManager instance;
        return instance;
    }

    // Recalculates total stats by combining Base + Equip + Buffs
    // Sends the CS_IT_CHARSTATUSINFO_ACK packet optionally if `sendPacket` is true
    void RecalculateStats(DWORD dwCharID, bool sendPacket = true);

    void SavePlayer(DWORD dwCharID);

private:
    PlayerManager() {}
    ~PlayerManager() {}
};
