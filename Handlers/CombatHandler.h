#pragma once
#include "../ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../UnitServer.h"

void OnPreAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnAttackHitReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
