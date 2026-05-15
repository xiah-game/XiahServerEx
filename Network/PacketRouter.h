#pragma once
#include "../ServerCore.h"
#include <map>
#include <functional>
#include <vector>

typedef std::function<void(SOCKET, BYTE*, WORD)> PacketHandlerFunc;

extern std::map<WORD, PacketHandlerFunc> g_PacketHandlers;
extern std::map<SOCKET, DWORD> g_SocketToChar;
extern std::map<SOCKET, DWORD> g_SocketToMap;
extern std::mutex g_SocketsMutex;

void InitPacketHandlers();
void RegisterHandler(WORD id, PacketHandlerFunc func);
bool RoutePacket(SOCKET clientSocket, PACKET_HEADER* header, std::vector<BYTE>& fullPacket);
