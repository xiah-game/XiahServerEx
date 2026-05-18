#pragma once
#include "../ServerCore.h"
#include <map>
#include <functional>
#include <vector>

typedef std::function<void(SOCKET, BYTE*, WORD)> PacketHandlerFunc;

extern std::map<WORD, PacketHandlerFunc> g_PacketHandlers;

// NOTE: g_SocketToChar, g_SocketToMap, g_SocketsMutex have been moved into SessionMgr.
// Use SessionMgr::GetInstance() to access session data.

void InitPacketHandlers();
void RegisterHandler(WORD id, PacketHandlerFunc func);
bool RoutePacket(SOCKET clientSocket, PACKET_HEADER* header, std::vector<BYTE>& fullPacket);
