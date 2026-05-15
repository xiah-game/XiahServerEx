#pragma once
#include "../ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "../DBHelper.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../UnitServer.h"

// Bank (仓库) Handlers
void OnItemListInBankReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnDrawInBankReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnDrawOutBankReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnDrawMoveBankReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);

// Item Mall (物品店) Handlers
void OnItemListInMallReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnDrawOutMallReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);

void RegisterBankMallHandlers();
