#pragma once
#include "../ServerCore.h"

void OnMugongListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnMugongAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnMugongLearnReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnSelMugongReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnMugongPreAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnSetOptionReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
