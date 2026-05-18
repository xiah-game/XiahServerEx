#pragma once
#include "../ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "../DBHelper.h"
#include "../Network/PacketRouter.h"
#include "../GameObjects/PlayerManager.h"

void SendCharSlotInfoAck(SOCKET clientSocket, DWORD dwCharID);
void OnSetSlotReq(SOCKET clientSocket, std::string& clientAccountName, BYTE* payload, WORD totalSize);
