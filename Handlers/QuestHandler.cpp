#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "QuestHandler.h"
#include "../GameObjects/QuestManager.h"
#include "../ServerCore.h"
#include "../Network/SessionMgr.h"
#include "../Network/PacketRouter.h"
#include "../DB/CharacterDB.h"
#include <vector>
#include <string>
#include <algorithm>

static void pushByte(std::vector<BYTE>& buf, BYTE val) {
    buf.push_back(val);
}

static void pushWord(std::vector<BYTE>& buf, WORD val) {
    buf.push_back((BYTE)(val & 0xFF));
    buf.push_back((BYTE)((val >> 8) & 0xFF));
}

static void pushDWord(std::vector<BYTE>& buf, DWORD val) {
    buf.push_back((BYTE)(val & 0xFF));
    buf.push_back((BYTE)((val >> 8) & 0xFF));
    buf.push_back((BYTE)((val >> 16) & 0xFF));
    buf.push_back((BYTE)((val >> 24) & 0xFF));
}

static void pushString(std::vector<BYTE>& buf, const std::string& str) {
    WORD len = (WORD)str.length();
    pushWord(buf, len);
    for (char c : str) {
        buf.push_back((BYTE)c);
    }
}

// 0x3203: CS_QS_LIST_REQ -> Reply 0x3204: CS_QS_LIST_ACK
void OnQuestListReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size) {
    LOG("[QuestHandler] Received CS_QS_LIST_REQ (0x3203) for CharID: " + std::to_string(dwCharID));

    // Ensure player's quests are loaded
    QuestManager::GetInstance().LoadPlayerQuests(dwCharID);

    // Also check and assign any newly eligible quests
    CharacterDB::CharPower cp;
    CharacterDB::CharBasic cb;
    if (CharacterDB::GetInstance().GetCharData(dwCharID, cp) && CharacterDB::GetInstance().GetCharBasic(dwCharID, cb)) {
        QuestManager::GetInstance().CheckAndAssignEligibleQuests(dwCharID, cb.bCharType, cp.wLevel);
    }

    DWORD dwFame = QuestManager::GetInstance().GetPlayerFame(dwCharID);
    std::vector<sPlayerQuest> playerQuests = QuestManager::GetInstance().GetPlayerQuestList(dwCharID);

    // Sort to prioritize active quests first, then paused, then new, then completed
    std::sort(playerQuests.begin(), playerQuests.end(), [](const sPlayerQuest& a, const sPlayerQuest& b) {
        auto priority = [](BYTE st) {
            if (st == QUEST_STATUS_STARTED) return 1;
            if (st == QUEST_STATUS_STOPPED) return 2;
            if (st == QUEST_STATUS_NEW) return 3;
            if (st == QUEST_STATUS_SUCCESS) return 4;
            return 5;
        };
        return priority(a.bStatus) < priority(b.bStatus);
    });

    // Cap at 200 so bNumQuest fits within BYTE (max 255) and packet fits within MTU/64KB
    BYTE bNumQuest = (BYTE)(std::min)((size_t)200, playerQuests.size());

    std::vector<BYTE> ackBuf(4);
    pushDWord(ackBuf, dwFame);
    pushByte(ackBuf, bNumQuest);

    for (BYTE i = 0; i < bNumQuest; ++i) {
        const sPlayerQuest& pq = playerQuests[i];
        const sQuestTemplate* tpl = QuestManager::GetInstance().GetQuestTemplate(pq.dwQuestID);

        std::string qName = tpl ? tpl->szName : ("奇缘 " + std::to_string(pq.dwQuestID));
        std::string qDesc = tpl ? tpl->szDescription : "";

        pushDWord(ackBuf, pq.dwQuestID);
        pushByte(ackBuf, pq.bStatus);
        pushString(ackBuf, qName);
        pushString(ackBuf, qDesc);
        pushByte(ackBuf, 0); // bRepeat
        pushByte(ackBuf, pq.bProcessType);

        auto procs = QuestManager::GetInstance().GetProcessesForStep(pq.dwQuestID, pq.bProcessType);
        auto results = QuestManager::GetInstance().GetResultsForStep(pq.dwQuestID, pq.bProcessType);

        BYTE bProcNum = (BYTE)(std::min)((size_t)10, procs.size());
        BYTE bResNum = (BYTE)(std::min)((size_t)10, results.size());

        pushByte(ackBuf, bProcNum);
        pushByte(ackBuf, bResNum);

        for (BYTE p = 0; p < bProcNum; ++p) {
            const auto& pr = procs[p];
            std::string pName = QuestManager::GetInstance().GetProcessTargetName(pr);
            DWORD currAmount = pq.dwData[p];
            DWORD totalAmount = (pr.dwData2 > 0) ? pr.dwData2 : 1;

            pushString(ackBuf, pName);
            pushDWord(ackBuf, currAmount);
            pushDWord(ackBuf, totalAmount);
        }

        for (BYTE r = 0; r < bResNum; ++r) {
            const auto& res = results[r];
            std::string rName = QuestManager::GetInstance().GetResultRewardName(res);
            DWORD rAmount = (res.bResultKind >= 70 && res.bResultKind <= 73) ? (res.dwData2 > 0 ? res.dwData2 : 1) : res.dwData1;

            pushString(ackBuf, rName);
            pushDWord(ackBuf, rAmount);
        }
    }

    PACKET_HEADER* h = (PACKET_HEADER*)ackBuf.data();
    h->id = 0x3204; // CS_QS_LIST_ACK
    h->payloadSize = (WORD)(ackBuf.size() - 4);

    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);

    LOG("[QuestHandler] Sent CS_QS_LIST_ACK (0x3204) with " + std::to_string(bNumQuest) + " quests, Fame=" + std::to_string(dwFame));
}

// 0x3205: CS_QS_START_REQ -> Reply 0x3206: CS_QS_START_ACK + 0x3202: CS_QS_CHANGE_ACK
void OnQuestStartReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size) {
    if (size < 4) return;
    DWORD dwQuestID = *(DWORD*)payload;

    BYTE bResult = QuestManager::GetInstance().StartQuest(dwCharID, dwQuestID);

    std::vector<BYTE> ackBuf(4);
    pushByte(ackBuf, bResult);
    pushDWord(ackBuf, dwQuestID);

    PACKET_HEADER* h = (PACKET_HEADER*)ackBuf.data();
    h->id = 0x3206; // CS_QS_START_ACK
    h->payloadSize = (WORD)(ackBuf.size() - 4);

    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);

    LOG("[QuestHandler] Sent CS_QS_START_ACK (0x3206) result=" + std::to_string(bResult) + " qid=" + std::to_string(dwQuestID));

    if (bResult == 0) {
        SendQuestChangeAck(s, dwCharID, dwQuestID);
    }
}

// 0x3207: CS_QS_STOP_REQ -> Reply 0x3208: CS_QS_STOP_ACK + 0x3202: CS_QS_CHANGE_ACK
void OnQuestStopReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size) {
    if (size < 4) return;
    DWORD dwQuestID = *(DWORD*)payload;

    BYTE bResult = QuestManager::GetInstance().StopQuest(dwCharID, dwQuestID);

    std::vector<BYTE> ackBuf(4);
    pushByte(ackBuf, bResult);
    pushDWord(ackBuf, dwQuestID);

    PACKET_HEADER* h = (PACKET_HEADER*)ackBuf.data();
    h->id = 0x3208; // CS_QS_STOP_ACK
    h->payloadSize = (WORD)(ackBuf.size() - 4);

    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);

    LOG("[QuestHandler] Sent CS_QS_STOP_ACK (0x3208) result=" + std::to_string(bResult) + " qid=" + std::to_string(dwQuestID));

    if (bResult == 0) {
        SendQuestChangeAck(s, dwCharID, dwQuestID);
    }
}

// 0x3209: CS_QS_DELETE_REQ -> Reply 0x320A: CS_QS_DELETE_ACK
void OnQuestDeleteReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size) {
    if (size < 4) return;
    DWORD dwQuestID = *(DWORD*)payload;

    BYTE bResult = QuestManager::GetInstance().DeleteQuest(dwCharID, dwQuestID);

    std::vector<BYTE> ackBuf(4);
    pushByte(ackBuf, bResult);
    pushDWord(ackBuf, dwQuestID);

    PACKET_HEADER* h = (PACKET_HEADER*)ackBuf.data();
    h->id = 0x320A; // CS_QS_DELETE_ACK
    h->payloadSize = (WORD)(ackBuf.size() - 4);

    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);

    LOG("[QuestHandler] Sent CS_QS_DELETE_ACK (0x320A) result=" + std::to_string(bResult) + " qid=" + std::to_string(dwQuestID));
}

// 0x3202: CS_QS_CHANGE_ACK (Push single quest status / progress change)
void SendQuestChangeAck(SOCKET s, DWORD dwCharID, DWORD dwQuestID) {
    auto pQuests = QuestManager::GetInstance().GetPlayerQuestList(dwCharID);
    const sPlayerQuest* pTargetQuest = nullptr;
    for (const auto& pq : pQuests) {
        if (pq.dwQuestID == dwQuestID) {
            pTargetQuest = &pq;
            break;
        }
    }
    if (!pTargetQuest) return;

    DWORD dwFame = QuestManager::GetInstance().GetPlayerFame(dwCharID);
    const sQuestTemplate* tpl = QuestManager::GetInstance().GetQuestTemplate(dwQuestID);

    std::string qName = tpl ? tpl->szName : ("奇缘 " + std::to_string(dwQuestID));
    std::string qDesc = tpl ? tpl->szDescription : "";

    std::vector<BYTE> ackBuf(4);
    pushDWord(ackBuf, dwFame);
    pushDWord(ackBuf, pTargetQuest->dwQuestID);
    pushByte(ackBuf, pTargetQuest->bStatus);
    pushString(ackBuf, qName);
    pushString(ackBuf, qDesc);
    pushByte(ackBuf, 0); // bRepeat
    pushByte(ackBuf, pTargetQuest->bProcessType);

    auto procs = QuestManager::GetInstance().GetProcessesForStep(dwQuestID, pTargetQuest->bProcessType);
    auto results = QuestManager::GetInstance().GetResultsForStep(dwQuestID, pTargetQuest->bProcessType);

    BYTE bProcNum = (BYTE)(std::min)((size_t)10, procs.size());
    BYTE bResNum = (BYTE)(std::min)((size_t)10, results.size());

    pushByte(ackBuf, bProcNum);
    pushByte(ackBuf, bResNum);

    for (BYTE p = 0; p < bProcNum; ++p) {
        const auto& pr = procs[p];
        std::string pName = QuestManager::GetInstance().GetProcessTargetName(pr);
        DWORD currAmount = pTargetQuest->dwData[p];
        DWORD totalAmount = (pr.dwData2 > 0) ? pr.dwData2 : 1;

        pushString(ackBuf, pName);
        pushDWord(ackBuf, currAmount);
        pushDWord(ackBuf, totalAmount);
    }

    for (BYTE r = 0; r < bResNum; ++r) {
        const auto& res = results[r];
        std::string rName = QuestManager::GetInstance().GetResultRewardName(res);
        DWORD rAmount = (res.bResultKind >= 70 && res.bResultKind <= 73) ? (res.dwData2 > 0 ? res.dwData2 : 1) : res.dwData1;

        pushString(ackBuf, rName);
        pushDWord(ackBuf, rAmount);
    }

    PACKET_HEADER* h = (PACKET_HEADER*)ackBuf.data();
    h->id = 0x3202; // CS_QS_CHANGE_ACK
    h->payloadSize = (WORD)(ackBuf.size() - 4);

    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);

    LOG("[QuestHandler] Sent CS_QS_CHANGE_ACK (0x3202) for qid=" + std::to_string(dwQuestID) + " status=" + std::to_string(pTargetQuest->bStatus));
}

void RegisterQuestHandlers() {
    RegisterHandler(0x3203, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (charID) OnQuestListReq(s, charID, p, size);
    });
    RegisterHandler(0x3205, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (charID) OnQuestStartReq(s, charID, p, size);
    });
    RegisterHandler(0x3207, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (charID) OnQuestStopReq(s, charID, p, size);
    });
    RegisterHandler(0x3209, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (charID) OnQuestDeleteReq(s, charID, p, size);
    });
    LOG("[QuestHandler] Registered Quest Handlers (0x3203, 0x3205, 0x3207, 0x3209)");
}
