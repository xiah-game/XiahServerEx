#include "QuestManager.h"
#include "../ServerCore.h"
#include "../DBHelper.h"
#include "../DB/CharacterDB.h"
#include "../DB/ItemDB.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/ExpSystem.h"
#include "../GameObjects/PlayerManager.h"
#include "../Handlers/QuestHandler.h"
#include "../Handlers/ItemSerializer.h"
#include <sstream>
#include <algorithm>

static void pushByte(std::vector<BYTE>& buf, BYTE val) {
    buf.push_back(val);
}

static void pushWord(std::vector<BYTE>& buf, WORD val) {
    buf.push_back((BYTE)(val & 0xFF));
    buf.push_back((BYTE)((val >> 8) & 0xFF));
}

extern std::map<WORD, sItemTemplate> g_ItemTemplates;
extern std::map<BYTE, sNpcTemplate> g_NpcTemplates;

void QuestManager::Initialize() {
    LoadTemplates();
}

void QuestManager::LoadTemplates() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_quests.clear();
    m_conditions.clear();
    m_processes.clear();
    m_results.clear();
    m_funcNpcNames.clear();

    // 1. Load Quest table
    std::string qQuest = "SELECT dwQuestID, bType, bKind, wRatio, szName, szDescription FROM Quest ORDER BY dwQuestID";
    DBHelper::GetInstance().ExecuteQuery(qQuest, [&](SQLHSTMT hStmt) {
        sQuestTemplate qt;
        int qid = 0, type = 0, kind = 0, ratio = 0;
        char nameBuf[64] = {0};
        char descBuf[1024] = {0};
        SQLLEN c[6] = {0};

        SQLGetData(hStmt, 1, SQL_C_SLONG, &qid, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &type, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &kind, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &ratio, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c[4]);
        SQLGetData(hStmt, 6, SQL_C_CHAR, descBuf, sizeof(descBuf), &c[5]);

        qt.dwQuestID = (DWORD)qid;
        qt.bType = (BYTE)type;
        qt.bKind = (BYTE)kind;
        qt.wRatio = (WORD)ratio;
        if (c[4] != SQL_NULL_DATA) qt.szName = nameBuf;
        if (c[5] != SQL_NULL_DATA) qt.szDescription = descBuf;

        m_quests[qt.dwQuestID] = qt;
    });

    // 2. Load Quest_Condition table
    std::string qCond = "SELECT dwQuestID, bConditionType, bConditionKind, dwData1, dwData2, dwData3 FROM Quest_Condition ORDER BY dwQuestID, bConditionType";
    DBHelper::GetInstance().ExecuteQuery(qCond, [&](SQLHSTMT hStmt) {
        sQuestCondition qc;
        int qid = 0, cType = 0, cKind = 0, d1 = 0, d2 = 0, d3 = 0;
        SQLLEN c[6] = {0};

        SQLGetData(hStmt, 1, SQL_C_SLONG, &qid, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &cType, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &cKind, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &d1, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &d2, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_SLONG, &d3, 0, &c[5]);

        qc.dwQuestID = (DWORD)qid;
        qc.bConditionType = (BYTE)cType;
        qc.bConditionKind = (BYTE)cKind;
        qc.dwData1 = (DWORD)d1;
        qc.dwData2 = (DWORD)d2;
        qc.dwData3 = (DWORD)d3;

        m_conditions[qc.dwQuestID].push_back(qc);
    });

    // 3. Load Quest_Process table
    std::string qProc = "SELECT dwQuestID, bProcessType, bProcessKind, dwData1, dwData2, dwData3 FROM Quest_Process ORDER BY dwQuestID, bProcessType";
    DBHelper::GetInstance().ExecuteQuery(qProc, [&](SQLHSTMT hStmt) {
        sQuestProcess qp;
        int qid = 0, pType = 0, pKind = 0, d1 = 0, d2 = 0, d3 = 0;
        SQLLEN c[6] = {0};

        SQLGetData(hStmt, 1, SQL_C_SLONG, &qid, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &pType, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &pKind, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &d1, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &d2, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_SLONG, &d3, 0, &c[5]);

        qp.dwQuestID = (DWORD)qid;
        qp.bProcessType = (BYTE)pType;
        qp.bProcessKind = (BYTE)pKind;
        qp.dwData1 = (DWORD)d1;
        qp.dwData2 = (DWORD)d2;
        qp.dwData3 = (DWORD)d3;

        m_processes[qp.dwQuestID].push_back(qp);
    });

    // 4. Load Quest_Result table
    std::string qRes = "SELECT dwQuestID, bResultType, bResultKind, dwData1, dwData2, dwData3 FROM Quest_Result ORDER BY dwQuestID, bResultType";
    DBHelper::GetInstance().ExecuteQuery(qRes, [&](SQLHSTMT hStmt) {
        sQuestResult qr;
        int qid = 0, rType = 0, rKind = 0, d1 = 0, d2 = 0, d3 = 0;
        SQLLEN c[6] = {0};

        SQLGetData(hStmt, 1, SQL_C_SLONG, &qid, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &rType, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &rKind, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &d1, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &d2, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_SLONG, &d3, 0, &c[5]);

        qr.dwQuestID = (DWORD)qid;
        qr.bResultType = (BYTE)rType;
        qr.bResultKind = (BYTE)rKind;
        qr.dwData1 = (DWORD)d1;
        qr.dwData2 = (DWORD)d2;
        qr.dwData3 = (DWORD)d3;

        m_results[qr.dwQuestID].push_back(qr);
    });

    // 5. Load FunctionalNpcList names for NPC-related steps
    std::string qNpc = "SELECT dwID, szName FROM FunctionalNpcList";
    DBHelper::GetInstance().ExecuteQuery(qNpc, [&](SQLHSTMT hStmt) {
        int nid = 0;
        char nameBuf[64] = {0};
        SQLLEN c[2] = {0};
        SQLGetData(hStmt, 1, SQL_C_SLONG, &nid, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c[1]);
        if (c[1] != SQL_NULL_DATA) {
            m_funcNpcNames[(DWORD)nid] = nameBuf;
        }
    });

    LOG("[QuestManager] Loaded " + std::to_string(m_quests.size()) + " Quests, " +
        std::to_string(m_conditions.size()) + " Condition groups, " +
        std::to_string(m_processes.size()) + " Process groups, " +
        std::to_string(m_results.size()) + " Result groups.");
}

void QuestManager::LoadPlayerQuests(DWORD dwCharID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_playerQuests[dwCharID].clear();

    std::string q = "SELECT dwID, dwCharID, dwQuestID, bStatus, bCount, "
                    "dwData1, dwData2, dwData3, dwData4, dwData5, "
                    "dwData6, dwData7, dwData8, dwData9, dwData10 "
                    "FROM QuestList WHERE dwCharID = " + std::to_string(dwCharID);

    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        sPlayerQuest pq;
        int id=0, cid=0, qid=0, status=0, cnt=0;
        int d[10] = {0};
        SQLLEN cl[15] = {0};

        SQLGetData(hStmt, 1, SQL_C_SLONG, &id, 0, &cl[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &cid, 0, &cl[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &qid, 0, &cl[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &status, 0, &cl[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &cnt, 0, &cl[4]);

        for (int i = 0; i < 10; ++i) {
            SQLGetData(hStmt, 6 + i, SQL_C_SLONG, &d[i], 0, &cl[5 + i]);
            pq.dwData[i] = (DWORD)d[i];
        }

        pq.dwID = (DWORD)id;
        pq.dwCharID = (DWORD)cid;
        pq.dwQuestID = (DWORD)qid;
        pq.bStatus = (BYTE)status;
        pq.bCount = (BYTE)cnt;

        // Determine current bProcessType from processes and progress data
        BYTE curStep = 0;
        if (m_processes.count(pq.dwQuestID)) {
            const auto& procs = m_processes[pq.dwQuestID];
            // If all processes have bProcessType == 0, curStep stays 0.
            // Otherwise, find the lowest step index where objectives are not complete.
            std::vector<BYTE> stepIndices;
            for (const auto& pr : procs) {
                if (pr.bProcessType > 0 && std::find(stepIndices.begin(), stepIndices.end(), pr.bProcessType) == stepIndices.end()) {
                    stepIndices.push_back(pr.bProcessType);
                }
            }
            std::sort(stepIndices.begin(), stepIndices.end());

            if (!stepIndices.empty()) {
                curStep = stepIndices.front();
                for (BYTE st : stepIndices) {
                    // Check if step `st` is done. Step `st` progress corresponds to dwData[st - 1] (or dwData[st])
                    int dataIdx = (st > 0 && st <= 10) ? (st - 1) : 0;
                    DWORD reqAmount = 1;
                    for (const auto& pr : procs) {
                        if (pr.bProcessType == st) {
                            reqAmount = (pr.dwData2 > 0) ? pr.dwData2 : 1;
                            break;
                        }
                    }
                    if (pq.dwData[dataIdx] < reqAmount) {
                        curStep = st;
                        break;
                    }
                    curStep = st; // up to latest completed
                }
            }
        }
        pq.bProcessType = curStep;

        m_playerQuests[dwCharID][pq.dwQuestID] = pq;
    });

    LOG("[QuestManager] Loaded " + std::to_string(m_playerQuests[dwCharID].size()) + " quests for CharID " + std::to_string(dwCharID));
}

void QuestManager::UnloadPlayerQuests(DWORD dwCharID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_playerQuests.erase(dwCharID);
}

void QuestManager::CheckAndAssignEligibleQuests(DWORD dwCharID, BYTE bCharType, int wLevel) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& pQuests = m_playerQuests[dwCharID];

    std::vector<DWORD> newlyAssigned;

    for (const auto& kv : m_quests) {
        DWORD qid = kv.first;
        if (pQuests.count(qid)) continue; // Already in player's journal

        // Check conditions
        bool eligible = true;
        if (m_conditions.count(qid)) {
            for (const auto& cond : m_conditions.at(qid)) {
                if (cond.bConditionKind == 41) { // Class restriction
                    if (bCharType != cond.dwData1 && bCharType != cond.dwData2 && bCharType != cond.dwData3) {
                        eligible = false;
                        break;
                    }
                } else if (cond.bConditionKind == 40) { // Level restriction
                    if ((DWORD)wLevel < cond.dwData1) {
                        eligible = false;
                        break;
                    }
                    if (cond.dwData2 > 0 && (DWORD)wLevel > cond.dwData2) {
                        eligible = false;
                        break;
                    }
                } else if (cond.bConditionKind == 33) { // Pre-requisite quest
                    DWORD preQid = cond.dwData1;
                    if (!pQuests.count(preQid) || pQuests[preQid].bStatus != QUEST_STATUS_SUCCESS) {
                        eligible = false;
                        break;
                    }
                } else if (cond.bConditionKind == 69) { // Item requirement (only grant if player has item or box)
                    eligible = false; // Triggered externally via item use
                    break;
                }
            }
        }

        if (eligible) {
            sPlayerQuest pq;
            pq.dwCharID = dwCharID;
            pq.dwQuestID = qid;
            pq.bStatus = QUEST_STATUS_NEW; // 0 = New / Available
            pq.bCount = 0;
            pq.bProcessType = 0;
            memset(pq.dwData, 0, sizeof(pq.dwData));

            // Determine starting step
            if (m_processes.count(qid) && !m_processes.at(qid).empty()) {
                pq.bProcessType = m_processes.at(qid).front().bProcessType;
            }

            // Save to DB
            std::string qInsert = "INSERT INTO QuestList (dwCharID, dwQuestID, bStatus, bCount, dateCreate) "
                                  "VALUES (" + std::to_string(dwCharID) + ", " + std::to_string(qid) + ", 0, 0, GETDATE())";
            DBHelper::GetInstance().ExecuteUpdate(qInsert);

            // Fetch generated dwID
            std::string qGetID = "SELECT dwID FROM QuestList WHERE dwCharID = " + std::to_string(dwCharID) + 
                                 " AND dwQuestID = " + std::to_string(qid);
            DBHelper::GetInstance().ExecuteQuery(qGetID, [&](SQLHSTMT hStmt) {
                int nid = 0;
                SQLGetData(hStmt, 1, SQL_C_SLONG, &nid, 0, NULL);
                pq.dwID = (DWORD)nid;
            });

            pQuests[qid] = pq;
            newlyAssigned.push_back(qid);
        }
    }

    if (!newlyAssigned.empty()) {
        LOG("[QuestManager] Assigned " + std::to_string(newlyAssigned.size()) + " new eligible quests to CharID " + std::to_string(dwCharID));
    }
}

DWORD QuestManager::GetPlayerFame(DWORD dwCharID) {
    DWORD fame = 0;
    CharacterDB::CharPower cp;
    if (CharacterDB::GetInstance().GetCharData(dwCharID, cp)) {
        fame = cp.dwFame;
    }
    return fame;
}

std::vector<sPlayerQuest> QuestManager::GetPlayerQuestList(DWORD dwCharID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<sPlayerQuest> res;
    if (m_playerQuests.count(dwCharID)) {
        for (const auto& kv : m_playerQuests[dwCharID]) {
            res.push_back(kv.second);
        }
    }
    return res;
}

const sQuestTemplate* QuestManager::GetQuestTemplate(DWORD dwQuestID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_quests.find(dwQuestID);
    if (it != m_quests.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<sQuestProcess> QuestManager::GetProcessesForStep(DWORD dwQuestID, BYTE bProcessType) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<sQuestProcess> res;
    if (m_processes.count(dwQuestID)) {
        for (const auto& p : m_processes[dwQuestID]) {
            if (p.bProcessType == bProcessType) {
                res.push_back(p);
            }
        }
        // Fallback: if no exact match on bProcessType, return all if single-step
        if (res.empty() && bProcessType == 0) {
            res = m_processes[dwQuestID];
        }
    }
    return res;
}

std::vector<sQuestResult> QuestManager::GetResultsForStep(DWORD dwQuestID, BYTE bProcessType) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<sQuestResult> res;
    if (m_results.count(dwQuestID)) {
        for (const auto& r : m_results[dwQuestID]) {
            if (r.bResultType == bProcessType || r.bResultType == 0) {
                res.push_back(r);
            }
        }
    }
    return res;
}

std::string QuestManager::GetProcessTargetName(const sQuestProcess& proc) {
    switch (proc.bProcessKind) {
        case 10: { // Monster Type
            BYTE monType = (BYTE)proc.dwData1;
            if (g_NpcTemplates.count(monType)) {
                return g_NpcTemplates[monType].szName;
            }
            return "指定妖兽";
        }
        case 11: { // Monster ID
            // dwData1 is monster refID or NPC ID
            if (proc.dwData1 < 256 && g_NpcTemplates.count((BYTE)proc.dwData1)) {
                return g_NpcTemplates[(BYTE)proc.dwData1].szName;
            }
            return "头领怪物";
        }
        case 12: return "驯服妖兽";
        case 14: return "训练妖兽";
        case 16: // NPC Type
        case 17: { // NPC ID
            DWORD nid = proc.dwData1;
            if (m_funcNpcNames.count(nid)) {
                return m_funcNpcNames[nid];
            }
            return "指引人";
        }
        case 30: return "修炼时间";
        case 70: // Collect Item
        case 73: {
            WORD wRefID = (WORD)proc.dwData1;
            if (g_ItemTemplates.count(wRefID)) {
                return g_ItemTemplates[wRefID].szName;
            }
            return "任务道具";
        }
        case 74: return "金钱";
        case 75: return "天地属性";
        default: return "达成目标";
    }
}

std::string QuestManager::GetResultRewardName(const sQuestResult& res) {
    switch (res.bResultKind) {
        case 42: return "奇缘名望";
        case 60: return "经验值";
        case 61: return "武功点数";
        case 62: return "名望";
        case 63: return "属性点数";
        case 70:
        case 72:
        case 73: {
            WORD wRefID = (WORD)res.dwData1;
            if (g_ItemTemplates.count(wRefID)) {
                return g_ItemTemplates[wRefID].szName;
            }
            return "道具奖励";
        }
        case 74: return "金钱";
        case 78: {
            WORD wRefID = (WORD)res.dwData1;
            if (g_ItemTemplates.count(wRefID)) {
                return g_ItemTemplates[wRefID].szName + "(回收)";
            }
            return "上缴道具";
        }
        default: return "奖励";
    }
}

BYTE QuestManager::StartQuest(DWORD dwCharID, DWORD dwQuestID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_playerQuests.count(dwCharID) || !m_playerQuests[dwCharID].count(dwQuestID)) {
        return QUESTRESULT_CANT_START;
    }

    sPlayerQuest& pq = m_playerQuests[dwCharID][dwQuestID];
    if (pq.bStatus == QUEST_STATUS_SUCCESS || pq.bStatus == QUEST_STATUS_STARTED) {
        return QUESTRESULT_OK;
    }

    pq.bStatus = QUEST_STATUS_STARTED; // 2 = Started / In progress
    SavePlayerQuest(pq);

    LOG("[QuestManager] Player " + std::to_string(dwCharID) + " started Quest " + std::to_string(dwQuestID));
    return QUESTRESULT_OK;
}

BYTE QuestManager::StopQuest(DWORD dwCharID, DWORD dwQuestID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_playerQuests.count(dwCharID) || !m_playerQuests[dwCharID].count(dwQuestID)) {
        return QUESTRESULT_CANT_STOP;
    }

    sPlayerQuest& pq = m_playerQuests[dwCharID][dwQuestID];
    if (pq.bStatus != QUEST_STATUS_STARTED) {
        return QUESTRESULT_CANT_STOP;
    }

    pq.bStatus = QUEST_STATUS_STOPPED; // 1 = Stopped / Paused
    SavePlayerQuest(pq);

    LOG("[QuestManager] Player " + std::to_string(dwCharID) + " paused Quest " + std::to_string(dwQuestID));
    return QUESTRESULT_OK;
}

BYTE QuestManager::DeleteQuest(DWORD dwCharID, DWORD dwQuestID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_playerQuests.count(dwCharID) || !m_playerQuests[dwCharID].count(dwQuestID)) {
        return QUESTRESULT_CANT_DELETE;
    }

    sPlayerQuest& pq = m_playerQuests[dwCharID][dwQuestID];
    pq.bStatus = QUEST_STATUS_DELETED; // 3 = Deleted
    SavePlayerQuest(pq);

    LOG("[QuestManager] Player " + std::to_string(dwCharID) + " deleted Quest " + std::to_string(dwQuestID));
    return QUESTRESULT_OK;
}

void QuestManager::OnMonsterKilled(DWORD dwCharID, BYTE bPropType, DWORD dwNpcListID) {
    std::vector<DWORD> affectedQuests;
    std::vector<DWORD> completedQuests;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_playerQuests.count(dwCharID)) return;

        auto& pQuests = m_playerQuests[dwCharID];
        for (auto& kv : pQuests) {
            sPlayerQuest& pq = kv.second;
            if (pq.bStatus != QUEST_STATUS_STARTED) continue;

            if (!m_processes.count(pq.dwQuestID)) continue;
            const auto& procs = m_processes[pq.dwQuestID];

            bool changed = false;
            int procIdx = 0;

            for (const auto& pr : procs) {
                if (pr.bProcessType == pq.bProcessType || (pq.bProcessType == 0 && pr.bProcessType == 0)) {
                    bool match = false;
                    if (pr.bProcessKind == 10 && pr.dwData1 == (DWORD)bPropType) {
                        match = true;
                    } else if (pr.bProcessKind == 11 && (pr.dwData1 == dwNpcListID || pr.dwData1 == (DWORD)bPropType)) {
                        match = true;
                    }

                    if (match) {
                        int dataSlot = (procIdx >= 0 && procIdx < 10) ? procIdx : 0;
                        DWORD required = (pr.dwData2 > 0) ? pr.dwData2 : 1;
                        if (pq.dwData[dataSlot] < required) {
                            pq.dwData[dataSlot]++;
                            changed = true;
                        }
                    }
                    procIdx++;
                }
            }

            if (changed) {
                SavePlayerQuest(pq);
                affectedQuests.push_back(pq.dwQuestID);

                // Check step completion
                if (CheckStepCompletion(dwCharID, pq)) {
                    // Check if there are further steps
                    BYTE nextStep = pq.bProcessType + 1;
                    bool hasNext = false;
                    for (const auto& pr : procs) {
                        if (pr.bProcessType == nextStep) {
                            hasNext = true;
                            break;
                        }
                    }

                    if (hasNext) {
                        pq.bProcessType = nextStep;
                        SavePlayerQuest(pq);
                    } else {
                        // All steps finished!
                        completedQuests.push_back(pq.dwQuestID);
                    }
                }
            }
        }
    }

    SOCKET s = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
    if (s != INVALID_SOCKET) {
        for (DWORD qid : affectedQuests) {
            SendQuestChangeAck(s, dwCharID, qid);
        }
        for (DWORD qid : completedQuests) {
            CompleteQuest(dwCharID, qid);
        }
    }
}

void QuestManager::OnNpcTalk(DWORD dwCharID, BYTE bFuncNpcType, DWORD dwFuncNpcID) {
    std::vector<DWORD> affectedQuests;
    std::vector<DWORD> completedQuests;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_playerQuests.count(dwCharID)) return;

        auto& pQuests = m_playerQuests[dwCharID];
        for (auto& kv : pQuests) {
            sPlayerQuest& pq = kv.second;
            if (pq.bStatus != QUEST_STATUS_STARTED) continue;

            if (!m_processes.count(pq.dwQuestID)) continue;
            const auto& procs = m_processes[pq.dwQuestID];

            bool changed = false;
            int procIdx = 0;

            for (const auto& pr : procs) {
                if (pr.bProcessType == pq.bProcessType || (pq.bProcessType == 0 && pr.bProcessType == 0)) {
                    bool match = false;
                    if (pr.bProcessKind == 16 && pr.dwData1 == (DWORD)bFuncNpcType) {
                        match = true;
                    } else if (pr.bProcessKind == 17 && pr.dwData1 == dwFuncNpcID) {
                        match = true;
                    }

                    if (match) {
                        int dataSlot = (procIdx >= 0 && procIdx < 10) ? procIdx : 0;
                        if (pq.dwData[dataSlot] < 1) {
                            pq.dwData[dataSlot] = 1;
                            changed = true;
                        }
                    }
                    procIdx++;
                }
            }

            if (changed) {
                SavePlayerQuest(pq);
                affectedQuests.push_back(pq.dwQuestID);

                if (CheckStepCompletion(dwCharID, pq)) {
                    BYTE nextStep = pq.bProcessType + 1;
                    bool hasNext = false;
                    for (const auto& pr : procs) {
                        if (pr.bProcessType == nextStep) {
                            hasNext = true;
                            break;
                        }
                    }

                    if (hasNext) {
                        pq.bProcessType = nextStep;
                        SavePlayerQuest(pq);
                    } else {
                        completedQuests.push_back(pq.dwQuestID);
                    }
                }
            }
        }
    }

    SOCKET s = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
    if (s != INVALID_SOCKET) {
        for (DWORD qid : affectedQuests) {
            SendQuestChangeAck(s, dwCharID, qid);
        }
        for (DWORD qid : completedQuests) {
            CompleteQuest(dwCharID, qid);
        }
    }
}

void QuestManager::OnItemAcquired(DWORD dwCharID, WORD wRefID, int count) {
    // Collect item checks can be processed similarly when items enter sack
}

void QuestManager::OnLevelUp(DWORD dwCharID, BYTE bCharType, int newLevel) {
    CheckAndAssignEligibleQuests(dwCharID, bCharType, newLevel);
}

bool QuestManager::CheckStepCompletion(DWORD dwCharID, sPlayerQuest& pq) {
    if (!m_processes.count(pq.dwQuestID)) return false;
    const auto& procs = m_processes[pq.dwQuestID];

    int procIdx = 0;
    bool allDone = true;

    for (const auto& pr : procs) {
        if (pr.bProcessType == pq.bProcessType || (pq.bProcessType == 0 && pr.bProcessType == 0)) {
            int dataSlot = (procIdx >= 0 && procIdx < 10) ? procIdx : 0;
            DWORD required = (pr.dwData2 > 0) ? pr.dwData2 : 1;
            if (pq.dwData[dataSlot] < required) {
                allDone = false;
                break;
            }
            procIdx++;
        }
    }

    return allDone;
}

void QuestManager::CompleteQuest(DWORD dwCharID, DWORD dwQuestID) {
    BYTE finalStep = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_playerQuests.count(dwCharID) || !m_playerQuests[dwCharID].count(dwQuestID)) {
            return;
        }

        sPlayerQuest& pq = m_playerQuests[dwCharID][dwQuestID];
        pq.bStatus = QUEST_STATUS_SUCCESS; // 4 = Success
        pq.bCount++;
        finalStep = pq.bProcessType;
        SavePlayerQuest(pq);
        LOG("[QuestManager] Player " + std::to_string(dwCharID) + " completed Quest " + std::to_string(dwQuestID) + "!");
    }

    // Distribute rewards outside lock
    DistributeRewards(dwCharID, dwQuestID, finalStep);

    SOCKET s = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
    if (s != INVALID_SOCKET) {
        SendQuestChangeAck(s, dwCharID, dwQuestID);
    }
}

void QuestManager::DistributeRewards(DWORD dwCharID, DWORD dwQuestID, BYTE bProcessType) {
    std::vector<sQuestResult> rewards;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_results.count(dwQuestID)) {
            for (const auto& r : m_results[dwQuestID]) {
                if (r.bResultType == bProcessType || r.bResultType == 0) {
                    rewards.push_back(r);
                }
            }
        }
    }

    SOCKET clientSocket = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);

    for (const auto& r : rewards) {
        switch (r.bResultKind) {
            case 60: { // EXP
                DWORD expAmount = r.dwData1;
                if (expAmount > 0) {
                    GrantExpToPlayer(dwCharID, expAmount, 0);
                    LOG("[QuestManager] Rewarded " + std::to_string(expAmount) + " EXP to CharID " + std::to_string(dwCharID));
                }
                break;
            }
            case 74: { // Gold / Money
                DWORD gold = r.dwData1;
                if (gold > 0) {
                    CharacterDB::GetInstance().AddMoney(dwCharID, gold);
                    if (clientSocket != INVALID_SOCKET) {
                        SendCharStatusInfoAck(clientSocket, dwCharID, 0x3B02);
                    }
                    LOG("[QuestManager] Rewarded " + std::to_string(gold) + " Money to CharID " + std::to_string(dwCharID));
                }
                break;
            }
            case 61: { // Skill Points (TP / SP)
                DWORD sp = r.dwData1;
                if (sp > 0) {
                    std::string q = "UPDATE CHAR_POWER SET wRemainTp = wRemainTp + " + std::to_string(sp) + 
                                    " WHERE dwCharID = " + std::to_string(dwCharID);
                    DBHelper::GetInstance().ExecuteUpdate(q);
                    if (clientSocket != INVALID_SOCKET) {
                        SendCharStatusInfoAck(clientSocket, dwCharID, 0x3B02);
                    }
                    LOG("[QuestManager] Rewarded " + std::to_string(sp) + " TP/SP to CharID " + std::to_string(dwCharID));
                }
                break;
            }
            case 62: { // Fame
                DWORD fame = r.dwData1;
                if (fame > 0) {
                    std::string q = "UPDATE CHAR_POWER SET dwFame = dwFame + " + std::to_string(fame) + 
                                    " WHERE dwCharID = " + std::to_string(dwCharID);
                    DBHelper::GetInstance().ExecuteUpdate(q);
                    if (clientSocket != INVALID_SOCKET) {
                        SendCharStatusInfoAck(clientSocket, dwCharID, 0x3B02);
                    }
                    LOG("[QuestManager] Rewarded " + std::to_string(fame) + " Fame to CharID " + std::to_string(dwCharID));
                }
                break;
            }
            case 63: { // Free Stat Points
                DWORD pts = r.dwData1;
                if (pts > 0) {
                    std::string q = "UPDATE CHAR_POWER SET wRemainPower = wRemainPower + " + std::to_string(pts) + 
                                    " WHERE dwCharID = " + std::to_string(dwCharID);
                    DBHelper::GetInstance().ExecuteUpdate(q);
                    if (clientSocket != INVALID_SOCKET) {
                        SendCharStatusInfoAck(clientSocket, dwCharID, 0x3B02);
                    }
                    LOG("[QuestManager] Rewarded " + std::to_string(pts) + " Stat Points to CharID " + std::to_string(dwCharID));
                }
                break;
            }
            case 70:
            case 72:
            case 73: { // Items
                WORD wRefID = (WORD)r.dwData1;
                WORD count = (WORD)(r.dwData2 > 0 ? r.dwData2 : 1);
                if (wRefID > 0) {
                    BYTE bcx = 1, bcy = 1;
                    if (g_ItemTemplates.count(wRefID)) {
                        bcx = g_ItemTemplates[wRefID].bCX;
                        bcy = g_ItemTemplates[wRefID].bCY;
                    }
                    for (WORD c = 0; c < count; ++c) {
                        DWORD newItemID = ItemDB::GetInstance().CreateItemFromTemplate(wRefID);
                        if (newItemID > 0) {
                            BYTE sackID = 1;
                            BYTE freePos = FindFreeSackPos(dwCharID, 1, bcx, bcy);
                            if (freePos == 255) { sackID = 2; freePos = FindFreeSackPos(dwCharID, 2, bcx, bcy); }
                            if (freePos == 255) { sackID = 3; freePos = FindFreeSackPos(dwCharID, 3, bcx, bcy); }

                            if (freePos != 255) {
                                ItemDB::GetInstance().AddToSack(dwCharID, freePos, newItemID);
                                LOG("[QuestManager] Rewarded item wRefID=" + std::to_string(wRefID) + " to CharID " + std::to_string(dwCharID));
                                
                                if (clientSocket != INVALID_SOCKET) {
                                    int startPos = (sackID == 1) ? 20 : (sackID == 2) ? 60 : 100;
                                    BYTE relPos = freePos - startPos;
                                    ItemDB::FullItemRow row;
                                    if (ItemDB::GetInstance().GetFullItemData(newItemID, row)) {
                                        std::vector<BYTE> bi(4);
                                        pushByte(bi, sackID);
                                        pushByte(bi, relPos);
                                        SerializeItemData(row, bi);
                                        pushWord(bi, (WORD)(row.nData25 > 0 ? row.nData25 : 0));
                                        
                                        PACKET_HEADER* addHead = (PACKET_HEADER*)bi.data();
                                        addHead->id = 0x420A;
                                        addHead->payloadSize = (WORD)(bi.size() - 4);
                                        EncryptPacket(bi.data(), 0x42);
                                        SafeSend(clientSocket, (const char*)bi.data(), (int)bi.size(), 0);
                                    }
                                }
                            } else {
                                LOG("[QuestManager] Sack full, reward item dropped for CharID " + std::to_string(dwCharID));
                            }
                        }
                    }
                }
                break;
            }
            case 78: { // Remove item from backpack (handing over quest item)
                WORD wRefID = (WORD)r.dwData1;
                std::string q = "SELECT TOP 1 s.dwItemID, s.bSackPos FROM SACKITEM s JOIN ITEM i ON s.dwItemID = i.dwItemID "
                                "WHERE s.dwCharID = " + std::to_string(dwCharID) + " AND i.wRefID = " + std::to_string(wRefID);
                DWORD itemToDelete = 0;
                BYTE sackPosToDelete = 255;
                DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
                    int iid = 0, spos = 0;
                    SQLGetData(hStmt, 1, SQL_C_SLONG, &iid, 0, NULL);
                    SQLGetData(hStmt, 2, SQL_C_SHORT, &spos, 0, NULL);
                    itemToDelete = (DWORD)iid;
                    sackPosToDelete = (BYTE)spos;
                });
                if (itemToDelete > 0) {
                    ItemDB::GetInstance().DeleteItemCascade(itemToDelete);
                    LOG("[QuestManager] Deducted quest item wRefID=" + std::to_string(wRefID) + " from CharID " + std::to_string(dwCharID));
                    if (clientSocket != INVALID_SOCKET && sackPosToDelete != 255) {
                        BYTE sackID = (sackPosToDelete >= 100) ? 3 : (sackPosToDelete >= 60) ? 2 : 1;
                        int startPos = (sackID == 1) ? 20 : (sackID == 2) ? 60 : 100;
                        BYTE relPos = sackPosToDelete - startPos;
                        std::vector<BYTE> rmBuf(7);
                        PACKET_HEADER* rmHead = (PACKET_HEADER*)rmBuf.data();
                        rmHead->id = 0x4208; // PKT_REMOVEFROMSACK_ACK
                        rmHead->payloadSize = 3;
                        rmBuf[4] = sackID;
                        rmBuf[5] = relPos;
                        rmBuf[6] = 0; // reason
                        EncryptPacket(rmBuf.data(), 0x42);
                        SafeSend(clientSocket, (const char*)rmBuf.data(), (int)rmBuf.size(), 0);
                    }
                }
                break;
            }
            default:
                break;
        }
    }
}

void QuestManager::SavePlayerQuest(const sPlayerQuest& pq) {
    std::string q = "UPDATE QuestList SET bStatus = " + std::to_string(pq.bStatus) + 
                    ", bCount = " + std::to_string(pq.bCount) + 
                    ", dwData1 = " + std::to_string(pq.dwData[0]) + 
                    ", dwData2 = " + std::to_string(pq.dwData[1]) + 
                    ", dwData3 = " + std::to_string(pq.dwData[2]) + 
                    ", dwData4 = " + std::to_string(pq.dwData[3]) + 
                    ", dwData5 = " + std::to_string(pq.dwData[4]) + 
                    ", dwData6 = " + std::to_string(pq.dwData[5]) + 
                    ", dwData7 = " + std::to_string(pq.dwData[6]) + 
                    ", dwData8 = " + std::to_string(pq.dwData[7]) + 
                    ", dwData9 = " + std::to_string(pq.dwData[8]) + 
                    ", dwData10 = " + std::to_string(pq.dwData[9]) + 
                    " WHERE dwCharID = " + std::to_string(pq.dwCharID) + 
                    " AND dwQuestID = " + std::to_string(pq.dwQuestID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}
