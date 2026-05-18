#pragma once
#include "../ServerCore.h"
#include <vector>
#include <string>

class PacketBuilder {
public:
    explicit PacketBuilder(WORD opCode);

    PacketBuilder& WriteByte(BYTE b);
    PacketBuilder& WriteWord(WORD w);
    PacketBuilder& WriteDword(DWORD d);
    PacketBuilder& WriteInt64(long long int v);
    PacketBuilder& WriteString(const std::string& s);  // WORD len + data + null terminator
    PacketBuilder& WriteBytes(const BYTE* data, int len);

    // Finalize header + encrypt + send to socket
    void SendTo(SOCKET s);

    // Finalize header + encrypt + return buffer (for broadcast)
    std::vector<BYTE> Build();

    // Access raw buffer before finalization (for BroadcastPacketAOI)
    const std::vector<BYTE>& GetBuffer() const { return m_buf; }

    // Get current payload size (excluding header)
    size_t PayloadSize() const { return m_buf.size() - sizeof(PACKET_HEADER); }

private:
    std::vector<BYTE> m_buf;
    WORD m_opCode;
    bool m_finalized;

    void Finalize();
};
