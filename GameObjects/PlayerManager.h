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
    void RecalculateStats(DWORD dwCharID, bool sendPacket = true, bool sendIFPacket = true);

    // 渐进式折中方案：安全挂载与卸载 Buff API，统一在锁外安全执行锁隔离、属性重算与 0x402C/0x402E 广播
    void ApplyBuffSafe(DWORD dwCharID, DWORD dwMugongID, BYTE bLevel, DWORD dwDuration, bool bIsDebuff);
    void RemoveBuffSafe(DWORD dwCharID, DWORD dwMugongID);

    void SavePlayer(DWORD dwCharID);

private:
    PlayerManager() {}
    ~PlayerManager() {}
};

// --- Free functions (migrated from UnitServer) ---
void SendCharStatusInfoAck(SOCKET clientSocket, DWORD dwCharID, WORD opCode);
void UpdatePlayerStatsAndSend(SOCKET clientSocket, DWORD dwCharID);
void BroadcastPacketToMap(DWORD mapID, const std::vector<BYTE>& packet);
void ClearPlayerBuffsOnDeath(PlayerData& player, DWORD mapID);

