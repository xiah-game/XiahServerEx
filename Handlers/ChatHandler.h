#pragma once

#include "../ServerCore.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>

// =========================================================
// Chat Type Constants (mirroring client XiahObjectType.h)
// =========================================================
#define CT_NORMAL           ((BYTE)0)
#define CT_WHISPER          ((BYTE)1)
#define CT_BROADCAST        ((BYTE)2)
#define CT_MUNJU            ((BYTE)3)
#define CT_BATTLE           ((BYTE)4)
#define CT_DAN              ((BYTE)5)
#define CT_MUNPA_BROADCAST  ((BYTE)6)
#define CT_MUNPA_MUNJUSHOUT ((BYTE)7)
#define CT_TIMEMESSAGE      ((BYTE)8)

// =========================================================
// Protocol IDs (verified via runtime)
// =========================================================
#define CS_CH_CHAT_REQ_ID   0x3E01
#define CS_CH_CHAT_ACK_ID   0x3E02

// =========================================================
// Chat Handler Functions
// =========================================================
void OnChatReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize);
