#pragma once
#include "../Network/SessionMgr.h"

namespace StatHandler {
    void OnExecSpReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
}
