#include "MugongManager.h"
#include "../DB/CharacterDB.h"
#include "../DB/GameDataDB.h"
#include "../DBHelper.h"
#include <cstring>
#include "PlayerManager.h"
#include "MapInstance.h"
#include "../Network/SessionMgr.h"

extern std::map<DWORD, CMapInstance*> g_MapInstances;

void MugongManager::LoadMugongData() {
    GameDataDB::GetInstance().LoadMugongTemplates(m_MugongTemplates);
    LOG("[MugongManager] Loaded " + std::to_string(m_MugongTemplates.size()) + " Skills from MUGONG_TEMPLATE.");

    GameDataDB::GetInstance().LoadMugongList(m_MugongLists);
    LOG("[MugongManager] Loaded skills levels from MUGONG_LIST.");
}

sMugongTemplate* MugongManager::GetTemplate(DWORD dwMugongID) {
    if (m_MugongTemplates.count(dwMugongID)) {
        return &m_MugongTemplates[dwMugongID];
    }
    return nullptr;
}

sMugongList* MugongManager::GetMugongLevelData(DWORD dwMugongID, BYTE bLevel) {
    if (m_MugongLists.count(dwMugongID) && m_MugongLists[dwMugongID].count(bLevel)) {
        return &m_MugongLists[dwMugongID][bLevel];
    }
    return nullptr;
}

void MugongManager::LoadPlayerMugongs(DWORD dwActualCharID, std::map<DWORD, BYTE>& learnedMugongs) {
    CharacterDB::GetInstance().GetCharMugongs(dwActualCharID, learnedMugongs);
}

int MugongManager::GetPlayerMugongLevel(DWORD charID, DWORD dwMugongID) {
    return CharacterDB::GetInstance().GetMugongLevel(charID, dwMugongID);
}

bool MugongManager::CanLearnMugong(DWORD charID, DWORD dwMugongID, BYTE targetLevel) {
    if (dwMugongID == 0) return false;
    
    sMugongTemplate* mg = GetTemplate(dwMugongID);
    if (!mg) return false;
    
    sMugongList* lst = GetMugongLevelData(dwMugongID, targetLevel);
    if (!lst) return false;

    int playerLevel = 0, playerType = 0;
    CharacterDB::CharPower cp;
    if (CharacterDB::GetInstance().GetCharData(charID, cp)) {
        playerLevel = cp.wLevel; playerType = cp.bCharType;
    }
    
    // 1. Check Char Type (Assuming 0 is universal, or 5 is universal, adjust as needed)
    if (mg->bCharType != 0 && mg->bCharType != playerType && mg->bCharType != 5) {
        LOG("[MugongManager] CharType mismatch! Player: " + std::to_string(playerType) + " Skill: " + std::to_string(mg->bCharType));
        return false;
    }
    
    // 2. Check Level
    if (playerLevel < lst->bLimitLevel) {
        LOG("[MugongManager] Level too low! Player: " + std::to_string(playerLevel) + " Required: " + std::to_string(lst->bLimitLevel));
        return false;
    }

    return true;
}

void MugongManager::LearnMugong(SOCKET clientSocket, DWORD charID, DWORD dwMugongID, BYTE targetLevel) {
    if (dwMugongID == 0) {
        LOG("[MugongManager] WARNING! LearnMugong called with dwMugongID = 0! Check ItemTemplate data loading!");
    }
    
    int currentLvl = GetPlayerMugongLevel(charID, dwMugongID);
    if (currentLvl == 0) {
        CharacterDB::GetInstance().InsertMugong(charID, dwMugongID, (BYTE)targetLevel);
    } else {
        CharacterDB::GetInstance().UpdateMugongLevel(charID, dwMugongID, (BYTE)targetLevel);
    }
    
    std::vector<BYTE> ackBuf(4);
    ackBuf.push_back(0); // bResult = SUCCESS
    ackBuf.push_back(dwMugongID & 0xFF);
    ackBuf.push_back((dwMugongID >> 8) & 0xFF);
    ackBuf.push_back((dwMugongID >> 16) & 0xFF);
    ackBuf.push_back((dwMugongID >> 24) & 0xFF);
    ackBuf.push_back(targetLevel); // bLevel
    ackBuf.push_back(0); // wUsedTP
    ackBuf.push_back(0);
    ackBuf.push_back(0); // wRemainedTP
    ackBuf.push_back(0);
    
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4024; // CS_BT_LEARNMUGONG_ACK
    head->payloadSize = ackBuf.size() - 4;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    
    LOG("[MugongManager] Character " + std::to_string(charID) + " learned Mugong " + std::to_string(dwMugongID) + " at Level " + std::to_string(targetLevel));

    {
        // 在线玩家实体内存同步更新
        DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        DWORD dwObjectID = (charID < 800000000) ? (charID + 400000000) : charID;
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwObjectID);
            if (pObj) {
                pObj->learnedMugongs[dwMugongID] = targetLevel;
            }
        }
    }
    // 即时重新计算面板属性并同步更新客户端
    PlayerManager::GetInstance().RecalculateStats(charID, true);
}

void MugongManager::UpgradeMugong(SOCKET clientSocket, DWORD charID, DWORD dwMugongID) {
    if (dwMugongID == 0) return;
    
    int currentLvl = GetPlayerMugongLevel(charID, dwMugongID);
    if (currentLvl == 0) {
        LOG("[MugongManager] Cannot upgrade unlearned skill!");
        return;
    }
    
    BYTE targetLvl = (BYTE)(currentLvl + 1);
    sMugongList* targetData = GetMugongLevelData(dwMugongID, targetLvl);
    if (!targetData) {
        LOG("[MugongManager] Skill max level reached or data missing for level " + std::to_string(targetLvl));
        return;
    }
    
    if (targetData->bReadOnlyBook == 1) {
        LOG("[MugongManager] This level can only be learned by reading a book! Level: " + std::to_string(targetLvl));
        return;
    }
    
    int playerLevel = 0, playerTP = 0;
    CharacterDB::CharPower cp2;
    if (CharacterDB::GetInstance().GetCharData(charID, cp2)) {
        playerLevel = cp2.wLevel; playerTP = cp2.wRemainTp;
    }
    
    if (playerLevel < targetData->bLimitLevel) {
        LOG("[MugongManager] Player level too low to upgrade! Required: " + std::to_string(targetData->bLimitLevel));
        return;
    }
    
    if (playerTP < targetData->dwNeedPoint) {
        LOG("[MugongManager] Not enough TP to upgrade! Required: " + std::to_string(targetData->dwNeedPoint));
        return;
    }
    
    // Deduct TP
    CharacterDB::GetInstance().DeductTpFromCharData(charID, targetData->dwNeedPoint);
    
    // Update Mugong Level
    CharacterDB::GetInstance().UpdateMugongLevel(charID, dwMugongID, (BYTE)targetLvl);
    
    // Send ACK
    std::vector<BYTE> ackBuf(4);
    ackBuf.push_back(0); // bResult = SUCCESS
    ackBuf.push_back(dwMugongID & 0xFF);
    ackBuf.push_back((dwMugongID >> 8) & 0xFF);
    ackBuf.push_back((dwMugongID >> 16) & 0xFF);
    ackBuf.push_back((dwMugongID >> 24) & 0xFF);
    ackBuf.push_back(targetLvl); // bLevel
    ackBuf.push_back(targetData->dwNeedPoint & 0xFF); // wUsedTP
    ackBuf.push_back((targetData->dwNeedPoint >> 8) & 0xFF);
    int newTP = playerTP - targetData->dwNeedPoint;
    ackBuf.push_back(newTP & 0xFF); // wRemainedTP
    ackBuf.push_back((newTP >> 8) & 0xFF);
    
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4024; // CS_BT_LEARNMUGONG_ACK
    head->payloadSize = ackBuf.size() - 4;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    
    LOG("[MugongManager] Skill upgraded to " + std::to_string(targetLvl) + " with TP " + std::to_string(targetData->dwNeedPoint));

    {
        // 在线玩家实体内存同步更新
        DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        DWORD dwObjectID = (charID < 800000000) ? (charID + 400000000) : charID;
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwObjectID);
            if (pObj) {
                pObj->learnedMugongs[dwMugongID] = targetLvl;
            }
        }
    }
    // 即时重新计算面板属性并同步更新客户端
    PlayerManager::GetInstance().RecalculateStats(charID, true);
}
