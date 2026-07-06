#pragma once
#include <winsock2.h>
#include <windows.h>

void OnPetGiveItemReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size);
