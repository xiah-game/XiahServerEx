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

// 号角物品聊天类型（bType=32, ITEMTYPE_GISDURABLITY）
#define CT_SAYITEM_CELL     ((BYTE)10)  // 百里传音 — AoI 小范围
#define CT_SAYITEM_MAP      ((BYTE)11)  // 千里传音 — 全地图
#define CT_SAYITEM_CHANNEL  ((BYTE)12)  // 万里传音 — 全频道
#define CT_SAYITEM_GOLD     ((BYTE)15)  // 黄金号角 — 全频道（金色特效）

// =========================================================
// Protocol IDs (verified via runtime)
// =========================================================
#define CS_CH_CHAT_REQ_ID   0x3E01
#define CS_CH_CHAT_ACK_ID   0x3E02

// =========================================================
// Chat Handler Functions
// =========================================================
void OnChatReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize);
