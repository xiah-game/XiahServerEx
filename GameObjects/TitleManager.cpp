#include "TitleManager.h"
#include "../DB/CharacterDB.h"
#include "../DBHelper.h"
#include "../ServerCore.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/PlayerManager.h"
#include <vector>
#include <algorithm>

static void SendSystemWarningChat(SOCKET clientSocket, const std::string& msg) {
    std::vector<BYTE> buf;
    buf.resize(4, 0);
    DWORD senderObjID = 0;
    buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
    buf.push_back(8); // CT_TIMEMESSAGE
    WORD len = (WORD)msg.size();
    buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
    buf.insert(buf.end(), msg.begin(), msg.end());
    WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
}

// 聚合计算并累加玩家所有处于激活状态且生效的称号加成属性
void TitleManager::GetTitleStats(DWORD dwCharID, 
                                  int& str, int& dex, int& vit, int& sus, 
                                  int& atk, int& def, int& hp, int& mp, 
                                  int& hit, int& dodge, int& crit, int& rHp, int& rMp,
                                  int& strPerc, int& dexPerc, int& vitPerc, int& susPerc,
                                  int& atkPerc, int& defPerc, int& hpPerc, int& mpPerc,
                                  int& expPerc, int& dropPerc) {
    str = dex = vit = sus = 0;
    atk = def = hp = mp = 0;
    hit = dodge = crit = rHp = rMp = 0;
    strPerc = dexPerc = vitPerc = susPerc = 0;
    atkPerc = defPerc = hpPerc = mpPerc = 0;
    expPerc = dropPerc = 0;

    std::string q = "SELECT "
                    "SUM(t.wStr), SUM(t.wDex), SUM(t.wVit), SUM(t.wSus), "
                    "SUM(t.wAtk), SUM(t.wDef), SUM(t.wHp), SUM(t.wMp), "
                    "SUM(t.wHit), SUM(t.wDodge), SUM(t.wCrit), SUM(t.wRestoreHp), SUM(t.wRestoreMp), "
                    "SUM(t.wStrPerc), SUM(t.wDexPerc), SUM(t.wVitPerc), SUM(t.wSusPerc), "
                    "SUM(t.wAtkPerc), SUM(t.wDefPerc), SUM(t.wHpPerc), SUM(t.wMpPerc), "
                    "SUM(t.wExpPerc), SUM(t.wDropPerc) "
                    "FROM CHAR_TITLE ct JOIN TITLE_TEMPLATE t ON ct.dwTitleID = t.dwTitleID "
                    "WHERE ct.dwCharID = " + std::to_string(dwCharID) + " AND ct.bActive = 1";

    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c[23];
        LONG vals[23];
        for (int i = 0; i < 23; i++) {
            SQLGetData(hStmt, i + 1, SQL_C_SLONG, &vals[i], 0, &c[i]);
            if (c[i] == SQL_NULL_DATA) vals[i] = 0;
        }
        str = (int)vals[0]; dex = (int)vals[1]; vit = (int)vals[2]; sus = (int)vals[3];
        atk = (int)vals[4]; def = (int)vals[5]; hp = (int)vals[6]; mp = (int)vals[7];
        hit = (int)vals[8]; dodge = (int)vals[9]; crit = (int)vals[10]; rHp = (int)vals[11]; rMp = (int)vals[12];
        strPerc = (int)vals[13]; dexPerc = (int)vals[14]; vitPerc = (int)vals[15]; susPerc = (int)vals[16];
        atkPerc = (int)vals[17]; defPerc = (int)vals[18]; hpPerc = (int)vals[19]; mpPerc = (int)vals[20];
        expPerc = (int)vals[21]; dropPerc = (int)vals[22];
    });
}

// 安全获取玩家当前激活且佩戴称号的 IconID
DWORD TitleManager::GetActiveTitleIconID(DWORD dwCharID) {
    DWORD activeIconID = 0;
    std::string qActiveIcon = "SELECT t.dwIconID FROM [CHAR_BASIC] c JOIN TITLE_TEMPLATE t ON c.dwActiveTitle = t.dwTitleID WHERE c.dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteQuery(qActiveIcon, [&](SQLHSTMT hStmt) {
        SQLGetData(hStmt, 1, SQL_C_LONG, &activeIconID, 0, NULL);
    });
    return activeIconID;
}

// 处理客户端 UI: 51 称号切换倒三角按钮的点击业务请求
void TitleManager::HandleSwitchTitle(SOCKET clientSocket, DWORD dwCharID) {
    std::vector<int> titleIDs;
    std::string qList = "SELECT dwTitleID FROM CHAR_TITLE WHERE dwCharID = " + std::to_string(dwCharID) + " ORDER BY dwTitleID ASC";
    DBHelper::GetInstance().ExecuteQuery(qList, [&](SQLHSTMT hStmt) {
        int tid = 0;
        SQLLEN cb;
        SQLGetData(hStmt, 1, SQL_C_LONG, &tid, 0, &cb);
        if (cb != SQL_NULL_DATA && tid > 0) titleIDs.push_back(tid);
    });

    if (titleIDs.empty()) {
        SendSystemWarningChat(clientSocket, "[称号系统] 您目前尚未获得任何称号，请先通过活动获得称号！");
        return;
    }

    int activeTitleID = 0;
    std::string qActive = "SELECT dwActiveTitle FROM [CHAR_BASIC] WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteQuery(qActive, [&](SQLHSTMT hStmt) {
        SQLGetData(hStmt, 1, SQL_C_LONG, &activeTitleID, 0, NULL);
    });

    int nextTitleID = 0;
    auto it = std::find(titleIDs.begin(), titleIDs.end(), activeTitleID);
    if (it == titleIDs.end()) {
        nextTitleID = titleIDs[0];
    } else {
        size_t idx = std::distance(titleIDs.begin(), it);
        if (idx + 1 < titleIDs.size()) {
            nextTitleID = titleIDs[idx + 1];
        } else {
            nextTitleID = 0; // 卸下称号
        }
    }

    std::string qUpdate = "UPDATE [CHAR_BASIC] SET dwActiveTitle = " + std::to_string(nextTitleID) + " WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(qUpdate);

    std::string newTitleName = "无";
    if (nextTitleID > 0) {
        std::string qTitleInfo = "SELECT szName FROM TITLE_TEMPLATE WHERE dwTitleID = " + std::to_string(nextTitleID);
        DBHelper::GetInstance().ExecuteQuery(qTitleInfo, [&](SQLHSTMT hStmt) {
            char szName[64] = {0};
            SQLGetData(hStmt, 1, SQL_C_CHAR, szName, sizeof(szName), NULL);
            newTitleName = szName;
        });
    }

    // 重新计算并广播称号同步包 (wVisualID[8])
    UpdatePlayerStatsAndSend(clientSocket, dwCharID);

    std::string msg = "[称号系统] 称号成功切换为: [" + newTitleName + "]";
    SendSystemWarningChat(clientSocket, msg);
}

// 处理客户端 UI: 50 称号信息文本框的点击查询业务请求
void TitleManager::HandleQueryTitle(SOCKET clientSocket, DWORD dwCharID) {
    int totalTitles = 0;
    std::string qCount = "SELECT COUNT(*) FROM CHAR_TITLE WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteQuery(qCount, [&](SQLHSTMT hStmt) {
        SQLGetData(hStmt, 1, SQL_C_LONG, &totalTitles, 0, NULL);
    });

    int activeTitleID = 0;
    std::string qActive = "SELECT dwActiveTitle FROM [CHAR_BASIC] WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteQuery(qActive, [&](SQLHSTMT hStmt) {
        SQLGetData(hStmt, 1, SQL_C_LONG, &activeTitleID, 0, NULL);
    });

    std::string activeTitleName = "无";
    if (activeTitleID > 0) {
        std::string qTitleName = "SELECT szName FROM TITLE_TEMPLATE WHERE dwTitleID = " + std::to_string(activeTitleID);
        DBHelper::GetInstance().ExecuteQuery(qTitleName, [&](SQLHSTMT hStmt) {
            char szName[64] = {0};
            SQLGetData(hStmt, 1, SQL_C_CHAR, szName, sizeof(szName), NULL);
            activeTitleName = szName;
        });
    }

    std::string msg = "[称号系统] 您目前共获得 " + std::to_string(totalTitles) + " 个称号。当前激活显示称号: [" + activeTitleName + "]";
    SendSystemWarningChat(clientSocket, msg);
}
