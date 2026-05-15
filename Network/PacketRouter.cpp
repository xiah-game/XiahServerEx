#include "PacketRouter.h"

std::map<WORD, PacketHandlerFunc> g_PacketHandlers;

void RegisterHandler(WORD id, PacketHandlerFunc func) {
    g_PacketHandlers[id] = func;
}

bool RoutePacket(SOCKET clientSocket, PACKET_HEADER* header, std::vector<BYTE>& fullPacket) {
    auto it = g_PacketHandlers.find(header->id);
    if (it != g_PacketHandlers.end()) {
        it->second(clientSocket, fullPacket.data() + sizeof(PACKET_HEADER), header->payloadSize);
        return true;
    }
    return false;
}
