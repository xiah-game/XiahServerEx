#include "SlotHandler.h"
#include "../DB/CharacterDB.h"
#include "../Network/SessionMgr.h"
#include <vector>
#include <string>
#include <iostream>

void SendCharSlotInfoAck(SOCKET clientSocket, DWORD dwCharID) {
    if (!clientSocket || dwCharID == 0) return;

    std::vector<BYTE> ackBuf(4);
    
    std::string q = "SELECT dwValue1, dwValue2, dwValue3, dwValue4, dwValue5, dwValue6, dwValue7, dwValue8, dwValue9, dwValue10 FROM CHAR_SLOT WHERE dwCharID = " + std::to_string(dwCharID);
    
    bool found = false;
    std::vector<DWORD> slots(10, 0);

    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        found = true;
        SQLLEN c;
        for (int i = 0; i < 10; ++i) {
            SQLGetData(hStmt, i + 1, SQL_C_ULONG, &slots[i], 0, &c);
        }
    });

    if (!found) {
        CharacterDB::GetInstance().InitializeSlot(dwCharID);
    }

    auto pushDword = [&](DWORD val) {
        ackBuf.push_back(val & 0xFF);
        ackBuf.push_back((val >> 8) & 0xFF);
        ackBuf.push_back((val >> 16) & 0xFF);
        ackBuf.push_back((val >> 24) & 0xFF);
    };

    for (int i = 0; i < 10; ++i) {
        pushDword(slots[i]);
    }

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x442E; // CS_IT_CHARSLOT_ACK (OFFSET_CS_IT + 45 = 0x442E)
    head->payloadSize = ackBuf.size() - 4;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    std::string slotValues = "";
    for (int i = 0; i < 10; ++i) slotValues += std::to_string(slots[i]) + " ";
    LOG("[SlotHandler] Sent CS_IT_CHARSLOT_ACK for CharID " + std::to_string(dwCharID) + " values: " + slotValues);
}

void OnSetSlotReq(SOCKET clientSocket, std::string& clientAccountName, BYTE* payload, WORD totalSize) {
    DWORD dwCharID = SessionMgr::GetInstance().GetCharID(clientSocket);
    if (dwCharID == 0) return;
    
    DWORD dwValue = *(DWORD*)(payload);
    BYTE bSlot = payload[4];
    
    if (bSlot >= 1 && bSlot <= 10) {
        CharacterDB::GetInstance().SetSlotValue(dwCharID, bSlot, dwValue);
    }
    
    std::vector<BYTE> ackBuf(4);
    ackBuf.push_back(0); // bResult = 0 (SUCCESS)
    ackBuf.push_back(bSlot);
    ackBuf.push_back(dwValue & 0xFF);
    ackBuf.push_back((dwValue >> 8) & 0xFF);
    ackBuf.push_back((dwValue >> 16) & 0xFF);
    ackBuf.push_back((dwValue >> 24) & 0xFF);
    
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4432; // CS_IT_SETSLOT_ACK (OFFSET_CS_IT + 49 = 0x4432)
    head->payloadSize = ackBuf.size() - 4;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[SlotHandler] Saved slot " + std::to_string(bSlot) + " with value " + std::to_string(dwValue) + " for CharID " + std::to_string(dwCharID));
}
