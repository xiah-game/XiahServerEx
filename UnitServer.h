#pragma once
#include <winsock2.h>
#include <windows.h>

void RunUnitSvr();
void SendCharStatusInfoAck(SOCKET clientSocket, DWORD dwCharID, WORD opCode);
void UpdatePlayerStatsAndSend(SOCKET clientSocket, DWORD dwCharID);
bool GrantExpToPlayer(DWORD dwCharID, DWORD dwIncrExp);
