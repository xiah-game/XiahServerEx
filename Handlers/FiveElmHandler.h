#pragma once
#include <winsock2.h>
#include <windows.h>

void OnExecFiveElmReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
void OnChangeFiveElmReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
void OnEndFiveElmReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
void OnExecStaminaReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);

// 二期五行战斗算法公式
float CalculateFiveElmCounter(BYTE attFE, BYTE defFE, BYTE attFELevel, BYTE defFELevel, bool ignoreCounter);
DWORD CalculateFiveElmDamage(DWORD baseAtkExp, BYTE attFELevel, float fCounter, DWORD defenderLevel, WORD defFEExp, bool ignoreDef);

