#pragma once
#include <winsock2.h>
#include <windows.h>

void OnExecFiveElmReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
void OnChangeFiveElmReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
void OnEndFiveElmReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
void OnExecStaminaReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
