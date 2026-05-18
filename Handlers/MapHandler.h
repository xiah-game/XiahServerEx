#pragma once
#include "../ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "../DBHelper.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/PlayerManager.h"

void OnMapLoadingSequenceReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize, WORD headerId);
void OnMapEnterReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnMapInfoReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnImReadyReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnCharStatusInfoReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnCharInfoReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnCharInfoListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnMapMoveReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);

