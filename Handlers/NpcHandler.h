#pragma once
#include "../ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "../DBHelper.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/PlayerManager.h"
#include <unordered_set>

void OnNpcInfoReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnNpcInfoListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnFunctionalNpcInfoListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnFunctionalNpcItemListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnBuyItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnSellItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
