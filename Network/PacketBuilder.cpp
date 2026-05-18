#include "PacketBuilder.h"
#include "../ServerCore.h"

PacketBuilder::PacketBuilder(WORD opCode)
    : m_opCode(opCode), m_finalized(false)
{
    // Reserve space for PACKET_HEADER (4 bytes: WORD id + WORD payloadSize)
    m_buf.resize(sizeof(PACKET_HEADER), 0);
}

PacketBuilder& PacketBuilder::WriteByte(BYTE b) {
    m_buf.push_back(b);
    return *this;
}

PacketBuilder& PacketBuilder::WriteWord(WORD w) {
    m_buf.push_back(w & 0xFF);
    m_buf.push_back((w >> 8) & 0xFF);
    return *this;
}

PacketBuilder& PacketBuilder::WriteDword(DWORD d) {
    WriteWord((WORD)(d & 0xFFFF));
    WriteWord((WORD)(d >> 16));
    return *this;
}

PacketBuilder& PacketBuilder::WriteInt64(long long int v) {
    WriteDword((DWORD)(v & 0xFFFFFFFF));
    WriteDword((DWORD)((v >> 32) & 0xFFFFFFFF));
    return *this;
}

PacketBuilder& PacketBuilder::WriteString(const std::string& s) {
    // Xiah protocol: WORD length (including null) + data + null terminator
    WORD len = (WORD)(s.size() + 1);
    WriteWord(len);
    for (size_t i = 0; i < s.size(); i++) {
        m_buf.push_back((BYTE)s[i]);
    }
    m_buf.push_back(0); // null terminator
    return *this;
}

PacketBuilder& PacketBuilder::WriteBytes(const BYTE* data, int len) {
    for (int i = 0; i < len; i++) {
        m_buf.push_back(data[i]);
    }
    return *this;
}

void PacketBuilder::Finalize() {
    if (m_finalized) return;
    PACKET_HEADER* head = (PACKET_HEADER*)m_buf.data();
    head->id = m_opCode;
    head->payloadSize = (WORD)(m_buf.size() - sizeof(PACKET_HEADER));
    EncryptPacket(m_buf.data(), 0x42);
    m_finalized = true;
}

void PacketBuilder::SendTo(SOCKET s) {
    Finalize();
    SafeSend(s, (const char*)m_buf.data(), (int)m_buf.size(), 0);
}

std::vector<BYTE> PacketBuilder::Build() {
    Finalize();
    return m_buf;
}
