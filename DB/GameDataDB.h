#pragma once
#include "../ServerCore.h"
#include "../DBHelper.h"
#include <vector>
#include <string>

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

private:
    GameDataDB() = default;
};
