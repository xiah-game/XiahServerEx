#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>

struct sQuestTemplate {
    DWORD dwQuestID = 0;
    BYTE bType = 0;
    BYTE bKind = 0;
    WORD wRatio = 0;
    std::string szName;
    std::string szDescription;
};

struct sQuestCondition {
    DWORD dwQuestID = 0;
    BYTE bConditionType = 0;
    BYTE bConditionKind = 0; // 33=preQuest, 40=level, 41=charType, 69=item
    DWORD dwData1 = 0;
    DWORD dwData2 = 0;
    DWORD dwData3 = 0;
};

struct sQuestProcess {
    DWORD dwQuestID = 0;
    BYTE bProcessType = 0; // step index (0, 1, 2...)
    BYTE bProcessKind = 0; // 10=monType, 11=monID, 16=npcType, 17=npcID, 30=time, 70=item, 74=gold
    DWORD dwData1 = 0;
    DWORD dwData2 = 0;     // required count / target
    DWORD dwData3 = 0;
};

struct sQuestResult {
    DWORD dwQuestID = 0;
    BYTE bResultType = 0; // step index
    BYTE bResultKind = 0; // 42=fame/flag, 60=exp, 61=sp/tp, 62=fame, 63=stat, 70/72/73=item, 74=gold, 78=delItem
    DWORD dwData1 = 0;    // amount or itemID
    DWORD dwData2 = 0;    // itemCount
    DWORD dwData3 = 0;
};

struct sPlayerQuest {
    DWORD dwID = 0;
    DWORD dwCharID = 0;
    DWORD dwQuestID = 0;
    BYTE bStatus = 0;     // 0=New, 1=Stopped, 2=Started, 3=Deleted, 4=Success
    BYTE bCount = 0;      // complete repeat count
    BYTE bProcessType = 0;// current step / phase
    DWORD dwData[10] = {0}; // progress counters for current step
    std::string dateCreate;
};

class QuestManager {
private:
    QuestManager() = default;
    ~QuestManager() = default;
    QuestManager(const QuestManager&) = delete;
    QuestManager& operator=(const QuestManager&) = delete;

    std::mutex m_mutex;

    // Static Templates
    std::map<DWORD, sQuestTemplate> m_quests;
    std::map<DWORD, std::vector<sQuestCondition>> m_conditions; // dwQuestID -> conditions
    std::map<DWORD, std::vector<sQuestProcess>> m_processes;     // dwQuestID -> processes
    std::map<DWORD, std::vector<sQuestResult>> m_results;       // dwQuestID -> results
    std::map<DWORD, std::string> m_funcNpcNames;                // dwID -> szName

    // Cache of active player quests: dwCharID -> map<dwQuestID, sPlayerQuest>
    std::map<DWORD, std::map<DWORD, sPlayerQuest>> m_playerQuests;

public:
    static QuestManager& GetInstance() {
        static QuestManager instance;
        return instance;
    }

    void Initialize();
    void LoadTemplates();

    // Player Quest Journal
    void LoadPlayerQuests(DWORD dwCharID);
    void UnloadPlayerQuests(DWORD dwCharID);
    void CheckAndAssignEligibleQuests(DWORD dwCharID, BYTE bCharType, int wLevel);

    // Queries
    DWORD GetPlayerFame(DWORD dwCharID);
    std::vector<sPlayerQuest> GetPlayerQuestList(DWORD dwCharID);
    const sQuestTemplate* GetQuestTemplate(DWORD dwQuestID);
    std::vector<sQuestProcess> GetProcessesForStep(DWORD dwQuestID, BYTE bProcessType);
    std::vector<sQuestResult> GetResultsForStep(DWORD dwQuestID, BYTE bProcessType);
    std::string GetProcessTargetName(const sQuestProcess& proc);
    std::string GetResultRewardName(const sQuestResult& res);

    // Player Actions
    BYTE StartQuest(DWORD dwCharID, DWORD dwQuestID);
    BYTE StopQuest(DWORD dwCharID, DWORD dwQuestID);
    BYTE DeleteQuest(DWORD dwCharID, DWORD dwQuestID);

    // Gameplay Triggers
    void OnMonsterKilled(DWORD dwCharID, BYTE bPropType, DWORD dwNpcListID);
    void OnNpcTalk(DWORD dwCharID, BYTE bFuncNpcType, DWORD dwFuncNpcID);
    void OnItemAcquired(DWORD dwCharID, WORD wRefID, int count);
    void OnLevelUp(DWORD dwCharID, BYTE bCharType, int newLevel);

    // Advancement & Settlement
    bool CheckStepCompletion(DWORD dwCharID, sPlayerQuest& pq);
    void CompleteQuest(DWORD dwCharID, DWORD dwQuestID);
    void DistributeRewards(DWORD dwCharID, DWORD dwQuestID, BYTE bProcessType);

    // Save
    void SavePlayerQuest(const sPlayerQuest& pq);
};
