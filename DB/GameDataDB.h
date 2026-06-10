#pragma once
#include "../ServerCore.h"
#include "../DBHelper.h"
#include "../GameObjects/DropManager.h"
#include "../GameObjects/MugongManager.h"
#include <vector>
#include <string>
#include <map>

class GameDataDB {
public:
    static GameDataDB& GetInstance() { static GameDataDB inst; return inst; }

    // Map metadata from LINKMAP
    struct MapInfo {
        std::string szName = "Map";
        WORD wWidth = 2048, wHeight = 2048;
        BYTE bType = 1;
    };
    bool GetMapInfo(DWORD dwMapID, MapInfo& out);

    // Portal list from LINKMAPLIST
    struct LinkMapEntry {
        DWORD dwLinkMapID = 0;
        WORD wPortalPosX = 0, wPortalPosY = 0;
        WORD wPortalWidth = 0, wPortalHeight = 0;
        BYTE bLinkType = 0;
        WORD wStartPosX = 0, wStartPosY = 0;
    };
    void GetLinkMapList(DWORD dwMapID, std::vector<LinkMapEntry>& out);

    // Portal destination from LINKPORTAL
    struct PortalDest {
        DWORD dwDestMapID = 0;
        WORD wDestX = 0, wDestY = 0;
    };
    bool GetPortalDest(DWORD dwMapID, DWORD dwLinkMapID, PortalDest& out);

    // ---- Startup Loaders ----
    // All methods below load data into the provided global containers.
    // They are called once at server startup.

    void LoadRebuildConfig(std::map<int, sRebuildConfig>& out);
    void LoadRealm(const std::string& dbServerInfo, std::string& realmNameOut);
    void LoadChannels(const std::string& dbServerInfo, const std::map<int, WORD>& channelPorts, std::vector<sChannelInfo>& out);
    void LoadLevelTemplates(std::map<WORD, std::map<BYTE, sLevelTemplate>>& out);
    void LoadItemTemplates(std::map<WORD, sItemTemplate>& out);
    void LoadNpcTemplates(std::map<BYTE, sNpcTemplate>& out);
    void LoadNpcMugongTemplates(std::map<BYTE, sNpcTemplate>& npcTemplates);
    void LoadFunctionalNpcs(std::vector<MonsterData>& out);
    void LoadNpcList(std::vector<MonsterData>& out, const std::map<BYTE, sNpcTemplate>& npcTemplates);
    void LoadFunctionalNpcItems(std::vector<MonsterData>& worldObjects);

    // 从 xiah_server.VersionInfo 表读取客户端最低版本号
    int LoadRequiredClientVersion();
    // DropManager startup loaders
    void LoadRootItems(std::map<BYTE, std::vector<sRootItem>>& out);
    void LoadDropGroups(std::map<BYTE, std::vector<sDropGroup>>& out);
    void LoadDropGroupItems(std::map<BYTE, std::vector<sDropGroup>>& groups);

    // MugongManager startup loaders
    void LoadMugongTemplates(std::map<DWORD, sMugongTemplate>& out);
    void LoadMugongList(std::map<DWORD, std::map<BYTE, sMugongList>>& out);

private:
    GameDataDB() = default;
};
