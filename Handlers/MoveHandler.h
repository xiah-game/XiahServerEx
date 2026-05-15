#pragma once
#include "../ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../UnitServer.h"

void OnMoveReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize, WORD headerId);
