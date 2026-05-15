#pragma once
#include "ServerCore.h"

void MonsterAIThread();
void BroadcastPacketToMap(DWORD mapID, const std::vector<BYTE>& packet);
