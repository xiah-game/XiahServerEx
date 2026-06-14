#pragma once
#include <winsock2.h>
#include <windows.h>

// Grants EXP to a player, handling level-up and TP segment detection.
// Returns true if a level-up or TP-up occurred (caller should refresh stats).
bool GrantExpToPlayer(DWORD dwCharID, DWORD dwIncrExp, DWORD dwFiveElmExpGained = 0);
void SyncFiveElmStatus(SOCKET clientSocket, DWORD charID, struct PlayerData* pObj);
void SendStaminaSync(SOCKET clientSocket, DWORD dwGauge);

