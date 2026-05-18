#pragma once
#include "../ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "../DBHelper.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/PlayerManager.h"

void OnSackItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize); // 0x4413
void OnEquipItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize); // 0x4419
void OnItemListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize); // 0x4417
void OnItemMoveReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize); // 0x420D
void OnItemDropReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize); // 0x442D
void OnUseItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize); // 0x4219
