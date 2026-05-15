#pragma once
#include "../ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "../DBHelper.h"
#include "../Network/PacketRouter.h"

void OnNewCharacterReq(SOCKET clientSocket, const std::string& clientAccountName, BYTE* payload, WORD totalSize);
void OnDelCharacterReq(SOCKET clientSocket, const std::string& clientAccountName, BYTE* payload, WORD totalSize);
