#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <functional>

struct PlayerData;

// CharacterDB: Abstracts all CHAR_DATA, CHAR_POWER, CHAR_BASIC, CHAR_STATUS queries.
// Eliminates raw SQL from Handlers for character-related operations.
class CharacterDB {
public:
    static CharacterDB& GetInstance() {
        static CharacterDB instance;
        return instance;
    }

    // ---- CHAR_POWER (Stats / Level / EXP) ----

    struct CharPower {
        WORD wLevel = 0, wStr = 0, wSus = 0, wDex = 0, wVit = 0;
        BYTE bCharType = 0;
        DWORD wIpMax = 0, wIpCur = 0;
        DWORD dwHpMax = 0, dwHpCur = 0;
        long long int dwExp = 0;
        DWORD dwTotalSp = 0, dwTotalTp = 0;
        WORD wRemainSp = 0, wRemainTp = 0;
        DWORD dwMoney = 0, dwFame = 0;
    };

    struct CharBasic {
        DWORD dwCharID = 0;
        BYTE bCharType = 0;
        std::string szCharName;
    };

    // Authenticate user login (returns accountId, sets exists/passMatch)
    DWORD AuthenticateUser(const std::string& username, const std::string& password,
                           const std::string& dbAccount, bool& accountExists, bool& passMatch);

    // Fetch full CHAR_DATA view (joins CHAR_POWER + CHAR_BASIC)
    bool GetCharData(DWORD dwCharID, CharPower& out);

    // Fetch CHAR_BASIC info
    bool GetCharBasic(DWORD dwCharID, CharBasic& out);

    // Update stat points after SP spend
    void UpdateStatPoints(DWORD dwCharID, WORD wStr, WORD wSus, WORD wDex, WORD wVit, WORD wRemainSp);

    // Set absolute money
    void SetMoney(DWORD dwCharID, DWORD dwMoney);

    // Relative money operations
    void AddMoney(DWORD dwCharID, DWORD amount);
    void SubtractMoney(DWORD dwCharID, DWORD amount);

    // Get current money
    INT64 GetMoney(DWORD dwCharID);

    // Update HP/IP in database
    void UpdateHpIp(DWORD dwCharID, DWORD dwHpCur, DWORD wIpCur);

    // Restore HP/IP to max in database (used on level-up)
    void RestoreHpIpToMax(DWORD dwCharID);

    // Update EXP + Level (used by ExpSystem)
    void UpdateExpAndLevel(DWORD dwCharID, long long int newExp, WORD wLevel,
                           WORD wRemainSp, DWORD dwTotalSp,
                           WORD wRemainTp, DWORD dwTotalTp,
                           bool levelUp, bool tpUp);

    // Fetch EXP data for GrantExp (from CHAR_POWER + CHAR_BASIC)
    struct ExpData {
        long long int dwExp = 0;
        WORD wLevel = 0;
        DWORD dwTotalTp = 0;
        WORD wRemainTp = 0, wRemainSp = 0;
        DWORD dwTotalSp = 0;
        BYTE bCharType = 1;
        WORD wFiveElmPoint = 0;
        WORD wFiveElmPointCnt = 0;
        DWORD dwFiveElmPower = 0;
        DWORD dwFiveElmGauge = 0;
        WORD wFireExp = 0;
        WORD wWaterExp = 0;
        WORD wWoodExp = 0;
        WORD wMetalExp = 0;
        WORD wEarthExp = 0;
    };
    bool GetExpData(DWORD dwCharID, ExpData& out);
    void UpdateFiveElm(DWORD dwCharID, WORD wFiveElmPoint, WORD wFiveElmPointCnt, DWORD dwFiveElmPower, DWORD dwFiveElmGauge,
                        WORD wFireExp, WORD wWaterExp, WORD wWoodExp, WORD wMetalExp, WORD wEarthExp);

    // Get current position from CHAR_STATUS
    bool GetCharPosition(DWORD dwCharID, int& wPosX, int& wPosY, int& bHeight);

    // Get current map ID from CHAR_STATUS
    DWORD GetCharMapID(DWORD dwCharID);

    // Get account name from CHAR_ACCOUNT
    std::string GetAccountName(DWORD dwCharID);

    // 获取账号 VIP 等级 (0-5)
    int GetVipLevel(DWORD dwCharID);

    // Get name and char type from CHAR_VISUAL view
    bool GetCharVisual(DWORD dwCharID, std::string& szNickName, BYTE& bCharType);

    // Save position on disconnect
    void SavePosition(DWORD dwCharID, WORD wPosX, WORD wPosY, DWORD dwMapID);

    // Full status data from CHAR_DATA (for SendCharStatusInfoAck)
    struct CharFullStatus {
        WORD wLevel=0, wStr=0, wSus=0, wDex=0, wVit=0;
        WORD wIpMax=0, wIpCur=0, wRemainSp=0, wRemainTp=0;
        DWORD dwHpMax=0, dwHpCur=0, dwTotalSp=0, dwTotalTp=0, dwMoney=0, dwFame=0;
        long long int dwExp=0;
    };
    bool GetCharFullStatus(DWORD dwCharID, CharFullStatus& out);

    // Get learned mugongs for a character
    void GetCharMugongs(DWORD dwCharID, std::map<DWORD, BYTE>& out);

    // ---- Character List / Login ----
    
    // Get all characters for an account
    struct CharListEntry {
        DWORD dwCharID = 0;
        std::string szCharName;
        BYTE bCharType = 0;
        WORD wLevel = 0;
        WORD wStr = 0, wSus = 0, wDex = 0, wVit = 0;
        DWORD dwMapID = 0;
        WORD wPosX = 0, wPosY = 0;
    };
    void GetCharacterList(const std::string& accountName, std::vector<CharListEntry>& out);

    // Full character select screen data
    struct EquipSlot { WORD wVis = 0; BYTE bRar = 0; BYTE bStx = 0; };
    struct CharSelectEntry {
        int dwCharID = 0; char szNickName[256] = {0}; char bCharType = 0;
        int dwBirthDate = 0; int dwMapID = 0; short wLevel = 0;
        int dwHpCur = 0; int dwHpMax = 0; int wIpCur = 0; int wIpMax = 0;
        short wVit = 0; short wStr = 0; short wSus = 0; short wDex = 0;
        char bRebirth = 0;
        EquipSlot items[9];
    };
    bool GetCharSelectList(const std::string& accountName, std::vector<CharSelectEntry>& out);

    // ---- Mugong (Skill Learning) ----
    
    // Deduct TP for skill learning
    void DeductTP(DWORD dwCharID, WORD amount);

    // Deduct TP from CHAR_DATA (wRemainTp field, used by MugongManager)
    void DeductTpFromCharData(DWORD dwCharID, DWORD amount);

    // Insert new mugong skill
    void InsertMugong(DWORD dwCharID, DWORD dwMugongID, BYTE bLevel);

    // Update mugong skill level
    void UpdateMugongLevel(DWORD dwCharID, DWORD dwMugongID, BYTE bLevel);

    // ---- Slot (Quick Bar) ----

    // Get spawn position from LOCATION table
    void GetSpawnPosition(DWORD dwMapID, int& wPosX, int& wPosY);

    // Get default stats from CHAR_DEFAULT
    struct CharDefault { int wStr, wDex, wVit, wSus, bIncHp, bIncIp; };
    bool GetCharDefault(BYTE bCharType, CharDefault& out);

    // 初始装备配置项（从 CHAR_STARTITEM 表读取）
    struct StartItem { WORD wRefID; BYTE bSackPos; WORD wAmount; };
    std::vector<StartItem> GetStartItems(BYTE bCharType);

    // Get slot values
    bool GetSlotValues(DWORD dwCharID, std::vector<DWORD>& slots);

    // ---- Character Lifecycle ----

    // Create full character record (CHAR_BASIC+ACCOUNT+STATUS+POWER+OPTION+RANK)
    // Returns new dwCharID, or 0 on failure
    DWORD CreateCharacterRecord(const std::string& accountName, const std::string& nickName, BYTE bCharType,
                                WORD wStr, WORD wSus, WORD wDex, WORD wVit,
                                int wHp, int wIp, WORD wPosX, WORD wPosY);

    // Soft-delete character (set bActive=9)
    bool SoftDeleteCharacter(DWORD dwCharID, const std::string& accountName);

    // Check if account owns character
    bool AccountOwnsCharacter(DWORD dwCharID, const std::string& accountName);

    // Count active characters for account
    int CountActiveCharacters(const std::string& accountName);

    // Check duplicate nickname
    bool NicknameExists(const std::string& nickName);

    // ---- Slot (Quick Bar) ----
    
    // Initialize empty slot row
    void InitializeSlot(DWORD dwCharID);

    // Set a specific slot value (bSlot 1-10)
    void SetSlotValue(DWORD dwCharID, BYTE bSlot, DWORD dwValue);

    // Get mugong level for a character's skill
    int GetMugongLevel(DWORD dwCharID, DWORD dwMugongID);

    // ---- Munpa (Guild / Sect) ----
    bool GetCharMunpaInfo(DWORD dwCharID, DWORD& dwMunpaID, DWORD& dwMunpaOrder, std::string& szMunpaName, std::string& szMunpaNickName, DWORD& dwMarkID);
    bool ExecCreateMunpa(const std::string& szMunpaName, DWORD dwCreatorID, DWORD dwCurrentTime, BYTE bMunpaLevel, BYTE& bResult, DWORD& dwMunpaID, WORD& wTotalTp, WORD& wRemainTp);
    bool ExecDeleteMunpa(DWORD dwMunpaID, BYTE& bResult);
    bool ExecAddMunwon(DWORD dwMunpaID, DWORD dwOrderID, DWORD dwMunwonID, BYTE& bResult);
    bool ExecChangeMunwon(DWORD dwMunpaID, DWORD dwMunwonID, DWORD dwOldOrderID, DWORD dwNewOrderID, BYTE& bResult);
    bool ExecChangeMunpaNick(DWORD dwMunpaID, DWORD dwMunwonID, const std::string& szMunpaNick, BYTE& bResult);
    bool ExecMunpaMarkReg(BYTE bRegType, DWORD dwMunpaID, const std::string& szMarkImage, DWORD& dwMarkID, BYTE& bResult);
    bool ExecGainMunpaStone(DWORD dwMunjuID, DWORD dwMunpaID, DWORD dwStoneID, DWORD dwCurrentTime, int& bResult, BYTE& bChannelID, DWORD& dwMapID);

    // Visual equipment and Fame sync
    void LoadVisualEquipAndFame(DWORD dwCharID, PlayerData* pObj);

private:
    CharacterDB() {}
    ~CharacterDB() {}
};
