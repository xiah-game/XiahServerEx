#include "SystemMessage.h"

void SystemMessage::SendHelpMessage(SOCKET clientSocket, MsgType type, const std::string& szName, DWORD dwAmount) {
    std::vector<BYTE> buf;
    buf.resize(4); // Space for PACKET_HEADER
    
    WORD wMsgIndex = static_cast<WORD>(type);
    
    // 1. wMsgIndex
    buf.push_back(wMsgIndex & 0xFF);
    buf.push_back(wMsgIndex >> 8);
    
    // 2. szName (Length prefix + string characters)
    WORD wNameLen = static_cast<WORD>(szName.length() + 1); // length including null terminator
    buf.push_back(wNameLen & 0xFF);
    buf.push_back(wNameLen >> 8);
    for (char c : szName) {
        buf.push_back(c);
    }
    buf.push_back(0); // null terminator
    
    // 3. dwAmount
    buf.push_back(dwAmount & 0xFF);
    buf.push_back((dwAmount >> 8) & 0xFF);
    buf.push_back((dwAmount >> 16) & 0xFF);
    buf.push_back((dwAmount >> 24) & 0xFF);
    
    // Set Header
    PACKET_HEADER* head = (PACKET_HEADER*)buf.data();
    head->id = 0x3B3F; // CS_IF_HELPMESSAGE_ACK for 1080 Client
    head->payloadSize = buf.size() - sizeof(PACKET_HEADER);
    
    // Encrypt and Send
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), buf.size(), 0);
}
