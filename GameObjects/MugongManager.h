#pragma once
#include "../ServerCore.h"
#include <map>
#include <string>

struct sMugongTemplate {
    DWORD dwMugongID;
    BYTE bCharType;
    BYTE bType; 
    BYTE bKind;
    std::string szName;
};

struct sMugongList {
    DWORD dwMugongID;
    BYTE bLevel;
    BYTE bLimitLevel;
    BYTE bReadOnlyBook;
    DWORD dwNeedPoint;
    DWORD dwNeedMoney;
    DWORD dwDamageMul;  // wIncAtk from DB, used as flat damage for attack skills
    WORD wAttackRange;  // wDistance from DB
    DWORD dwCostMp;     // wReduceIP from DB
    
    // Buff / Passive properties (from MUGONG_LIST columns)
    DWORD dwKeepUpTime;
    WORD wIncAtk;
    WORD wIncAtkPerc;
    WORD wIncDef;
    WORD wIncDefPerc;
    WORD wIncRate;       // Hit rate bonus
    WORD wIncRatePerc;   // Hit rate % bonus
    WORD wIncHpMax;
    WORD wIncHpCur;      // Instant HP heal (e.g. MugongID 63)
    WORD wIncHpCurPerc;
    WORD wRecoverHp;     // HP regen over time
    WORD wRecoverHpPerc;
    WORD wIncIpMax;
    WORD wIncIpCur;      // Instant IP heal
    WORD wIncIpCurPerc;
    WORD wRecoverIp;
    WORD wRecoverIpPerc;
    WORD wIncCritical;
    WORD wIncCriticalPerc;
};

class MugongManager {
public:
    static MugongManager* GetInstance() {
        static MugongManager instance;
        return &instance;
    }

    void LoadMugongData();
    
    sMugongTemplate* GetTemplate(DWORD dwMugongID);
    sMugongList* GetMugongLevelData(DWORD dwMugongID, BYTE bLevel);
    void LoadPlayerMugongs(DWORD dwActualCharID, std::map<DWORD, BYTE>& learnedMugongs);
    int GetPlayerMugongLevel(DWORD charID, DWORD dwMugongID);
    bool CanLearnMugong(DWORD charID, DWORD dwMugongID, BYTE targetLevel);
    void LearnMugong(SOCKET clientSocket, DWORD charID, DWORD dwMugongID, BYTE targetLevel);
    void UpgradeMugong(SOCKET clientSocket, DWORD charID, DWORD dwMugongID);

private:
    MugongManager() {}
    ~MugongManager() {}

    std::map<DWORD, sMugongTemplate> m_MugongTemplates;
    std::map<DWORD, std::map<BYTE, sMugongList>> m_MugongLists;
};
