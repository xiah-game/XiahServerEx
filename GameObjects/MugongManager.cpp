#include "MugongManager.h"
#include "../DB/CharacterDB.h"
#include "../DBHelper.h"
#include <cstring>

void MugongManager::LoadMugongData() {
    m_MugongTemplates.clear();
    m_MugongLists.clear();

    std::string qMugongTpl = "SELECT dwMugongID, bCharType, bType, bKind, szName FROM MUGONG_TEMPLATE";
    auto mtplCallback = [&](SQLHSTMT hStmt) {
        int mid = 0; int ctype = 0, type = 0, kind = 0; char nameBuf[64]; SQLLEN c[5];
        memset(nameBuf, 0, sizeof(nameBuf));
        SQLGetData(hStmt, 1, SQL_C_SLONG, &mid, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &ctype, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &type, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &kind, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c[4]);
        
        sMugongTemplate tpl;
        tpl.dwMugongID = mid; tpl.bCharType = (BYTE)ctype; tpl.bType = (BYTE)type; tpl.bKind = (BYTE)kind;
        if (c[4] != SQL_NULL_DATA) tpl.szName = nameBuf;
        m_MugongTemplates[mid] = tpl;
    };
    DBHelper::GetInstance().ExecuteQuery(qMugongTpl, mtplCallback);
    LOG("[MugongManager] Loaded " + std::to_string(m_MugongTemplates.size()) + " Skills from MUGONG_TEMPLATE.");

    std::string qMugongLst = "SELECT dwMugongID, bLevel, bLimitLevel, bReadOnlyBook, wNeedTP, wIncAtk, wDistance, wReduceIP, dwKeepUpTime, wIncAtkPerc, wIncDef, wIncDefPerc, wIncRate, wIncRatePerc, wIncHpMax, wIncHpCur, wIncHpCurPerc, wRecoverHp, wRecoverHpPerc, wIncIpMax, wIncIpCur, wIncIpCurPerc, wRecoverIp, wRecoverIpPerc, wIncCritical, wIncCriticalPerc FROM MUGONG_LIST";
    auto mlstCallback = [&](SQLHSTMT hStmt) {
        int vals[26] = {0}; SQLLEN c[26];
        for (int i = 0; i < 26; i++) {
            SQLGetData(hStmt, i + 1, SQL_C_SLONG, &vals[i], 0, &c[i]);
            if (c[i] == SQL_NULL_DATA) vals[i] = 0;
        }
        
        sMugongList lst;
        lst.dwMugongID   = vals[0];
        lst.bLevel       = (BYTE)vals[1];
        lst.bLimitLevel  = (BYTE)vals[2];
        lst.bReadOnlyBook= (BYTE)vals[3];
        lst.dwNeedPoint  = (DWORD)vals[4];
        lst.dwNeedMoney  = 0;
        lst.dwDamageMul  = (DWORD)vals[5];  // wIncAtk used as flat damage for attacks
        lst.wAttackRange = (WORD)vals[6];    // wDistance
        lst.dwCostMp     = (DWORD)vals[7];   // wReduceIP
        lst.dwKeepUpTime = (DWORD)vals[8];
        lst.wIncAtk      = (WORD)vals[5];    // Same column, dual use
        lst.wIncAtkPerc  = (WORD)vals[9];
        lst.wIncDef      = (WORD)vals[10];
        lst.wIncDefPerc  = (WORD)vals[11];
        lst.wIncRate     = (WORD)vals[12];
        lst.wIncRatePerc = (WORD)vals[13];
        lst.wIncHpMax    = (WORD)vals[14];
        lst.wIncHpCur    = (WORD)vals[15];
        lst.wIncHpCurPerc= (WORD)vals[16];
        lst.wRecoverHp   = (WORD)vals[17];
        lst.wRecoverHpPerc=(WORD)vals[18];
        lst.wIncIpMax    = (WORD)vals[19];
        lst.wIncIpCur    = (WORD)vals[20];
        lst.wIncIpCurPerc= (WORD)vals[21];
        lst.wRecoverIp   = (WORD)vals[22];
        lst.wRecoverIpPerc=(WORD)vals[23];
        lst.wIncCritical = (WORD)vals[24];
        lst.wIncCriticalPerc=(WORD)vals[25];

        m_MugongLists[lst.dwMugongID][lst.bLevel] = lst;
    };
    DBHelper::GetInstance().ExecuteQuery(qMugongLst, mlstCallback);
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
    learnedMugongs.clear();
    std::string qMg = "SELECT dwMugongID, bMugongLevel FROM CHAR_MUGONG WHERE dwCharID = " + std::to_string(dwActualCharID);
    auto mgCallback = [&](SQLHSTMT hStmt) {
        int mid = 0; int lvl = 0; SQLLEN c1, c2;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &mid, 0, &c1);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &lvl, 0, &c2);
        if (c1 != SQL_NULL_DATA && c2 != SQL_NULL_DATA) {
            learnedMugongs[mid] = (BYTE)lvl;
        }
    };
    DBHelper::GetInstance().ExecuteQuery(qMg, mgCallback);
}

int MugongManager::GetPlayerMugongLevel(DWORD charID, DWORD dwMugongID) {
    int currentLevel = 0;
    std::string q = "SELECT bMugongLevel FROM CHAR_MUGONG WHERE dwCharID = " + std::to_string(charID) + " AND dwMugongID = " + std::to_string(dwMugongID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &currentLevel, 0, &c);
    });
    return currentLevel;
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
}
