import os

code = """
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
        std::string insQ = "INSERT INTO CHAR_SLOT (dwCharID, dwValue1, dwValue2, dwValue3, dwValue4, dwValue5, dwValue6, dwValue7, dwValue8, dwValue9, dwValue10) VALUES (" + std::to_string(dwCharID) + ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)";
        DBHelper::GetInstance().ExecuteUpdate(insQ);
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
    head->id = 0x442F; // CS_IT_CHARSLOT_ACK
    head->payloadSize = ackBuf.size() - 4;
    EncryptPacket(ackBuf.data(), 0x42);
    send(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[UnitServer] Sent CS_IT_CHARSLOT_ACK for CharID " + std::to_string(dwCharID));
}
"""

with open(r'd:\xiahold\XiahServerEx\UnitServer.cpp', 'ab') as f:
    f.write(code.encode('utf-8'))
