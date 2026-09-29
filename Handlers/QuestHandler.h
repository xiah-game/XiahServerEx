#pragma once
#include "../ServerCore.h"

void RegisterQuestHandlers();

void OnQuestListReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size);
void OnQuestStartReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size);
void OnQuestStopReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size);
void OnQuestDeleteReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size);

void SendQuestChangeAck(SOCKET s, DWORD dwCharID, DWORD dwQuestID);
