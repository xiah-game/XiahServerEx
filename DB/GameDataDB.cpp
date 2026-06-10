#include "GameDataDB.h"

bool GameDataDB::GetMapInfo(DWORD dwMapID, MapInfo& out) {
    bool found = false;
    std::string q = "SELECT szName, wWidth, wHeight, bType FROM LINKMAP WHERE dwMapID = " + std::to_string(dwMapID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        char name[256] = {0}; SQLLEN cb1, cb2, cb3, cb4;
        SQLGetData(hStmt, 1, SQL_C_CHAR, name, sizeof(name), &cb1);
        if (cb1 != SQL_NULL_DATA) out.szName = name;
        int w, h, t;
        SQLGetData(hStmt, 2, SQL_C_SLONG, &w, 0, &cb2);
        if (cb2 != SQL_NULL_DATA) out.wWidth = w;
        SQLGetData(hStmt, 3, SQL_C_SLONG, &h, 0, &cb3);
        if (cb3 != SQL_NULL_DATA) out.wHeight = h;
        SQLGetData(hStmt, 4, SQL_C_SLONG, &t, 0, &cb4);
        if (cb4 != SQL_NULL_DATA) out.bType = t;
        found = true;
    });
    return found;
}

void GameDataDB::GetLinkMapList(DWORD dwMapID, std::vector<LinkMapEntry>& out) {
    std::string q = "SELECT dwLinkMapID, wPosX, wPosY, wWidth, wHeight, bLinkType, ISNULL(wStartPosX,0), ISNULL(wStartPosY,0) FROM LINKMAPLIST WHERE dwMapID = " + std::to_string(dwMapID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        LinkMapEntry lm; SQLLEN c[8];
        SQLGetData(hStmt, 1, SQL_C_ULONG, &lm.dwLinkMapID, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_USHORT, &lm.wPortalPosX, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_USHORT, &lm.wPortalPosY, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_USHORT, &lm.wPortalWidth, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_USHORT, &lm.wPortalHeight, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_TINYINT, &lm.bLinkType, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_USHORT, &lm.wStartPosX, 0, &c[6]);
        SQLGetData(hStmt, 8, SQL_C_USHORT, &lm.wStartPosY, 0, &c[7]);
        out.push_back(lm);
    });
}

bool GameDataDB::GetPortalDest(DWORD dwMapID, DWORD dwLinkMapID, PortalDest& out) {
    bool found = false;
    std::string q = "SELECT dwDestMapID, wDestX, wDestY FROM LINKPORTAL WHERE dwMapID = " + std::to_string(dwMapID) + " AND dwLinkMapID = " + std::to_string(dwLinkMapID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c[3];
        SQLGetData(hStmt, 1, SQL_C_ULONG, &out.dwDestMapID, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_USHORT, &out.wDestX, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_USHORT, &out.wDestY, 0, &c[2]);
        found = true;
    });
    return found;
}

void GameDataDB::LoadRebuildConfig(std::map<int, sRebuildConfig>& out) {
    out.clear();
    DBHelper::GetInstance().ExecuteQuery("SELECT RebuildLevel, Wujing_Attr, Sujing_Attr, LevelMultiplier, BaseRate, BreakChance FROM REBUILD_CONFIG", [&](SQLHSTMT hStmt) {
        int lvl = 0; sRebuildConfig cfg; SQLLEN c[6];
        SQLGetData(hStmt, 1, SQL_C_SLONG, &lvl, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &cfg.wAttr, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &cfg.sAttr, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &cfg.lMulti, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &cfg.baseRate, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_SLONG, &cfg.breakChance, 0, &c[5]);
        out[lvl] = cfg;
    });
}

void GameDataDB::LoadRealm(const std::string& dbServerInfo, std::string& realmNameOut) {
    realmNameOut = "XiahEmu Local";
    std::string q = "SELECT TOP 1 szworldname FROM " + dbServerInfo + ".dbo.worldlist";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        char buf[128] = {0}; SQLLEN cb = 0;
        if (SQL_SUCCEEDED(SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &cb)) && cb > 0 && cb != SQL_NULL_DATA)
            realmNameOut = buf;
    });
}

void GameDataDB::LoadChannels(const std::string& dbServerInfo, const std::map<int, WORD>& channelPorts, std::vector<sChannelInfo>& out) {
    out.clear();
    std::string q = "SELECT bchannelid, szChannelName, szChannelDes, wmaxuser, btype, bage FROM " + dbServerInfo + ".dbo.CHANNELLIST";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        sChannelInfo ci;
        SQLLEN cbId=0, cbName=0, cbDesc=0, cbCap=0, cbPvp=0, cbAge=0;
        char nameBuf[128] = {0}; char descBuf[128] = {0};
        SQLGetData(hStmt, 1, SQL_C_LONG, &ci.id, 0, &cbId);
        SQLGetData(hStmt, 2, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &cbName);
        SQLGetData(hStmt, 3, SQL_C_CHAR, descBuf, sizeof(descBuf), &cbDesc);
        SQLGetData(hStmt, 4, SQL_C_LONG, &ci.capacity, 0, &cbCap);
        SQLGetData(hStmt, 5, SQL_C_LONG, &ci.pvp, 0, &cbPvp);
        SQLGetData(hStmt, 6, SQL_C_LONG, &ci.age, 0, &cbAge);
        if (cbName != SQL_NULL_DATA && cbName > 0) ci.name = nameBuf;
        if (cbDesc != SQL_NULL_DATA && cbDesc > 0) ci.desc = descBuf;
        if (channelPorts.find(ci.id) != channelPorts.end())
            out.push_back(ci);
    });
}

void GameDataDB::LoadLevelTemplates(std::map<WORD, std::map<BYTE, sLevelTemplate>>& out) {
    out.clear();
    std::string q = "SELECT wLevel, bCharType, dwNeedExp, bSp, bTp FROM LEVELTEMPLATE";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        WORD wLevel=0; BYTE bCharType=0, bSp=0, bTp=0; long long int dwNeedExp=0; SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_USHORT, &wLevel, 0, &c);
        SQLGetData(hStmt, 2, SQL_C_UTINYINT, &bCharType, 0, &c);
        SQLGetData(hStmt, 3, SQL_C_SBIGINT, &dwNeedExp, 0, &c);
        SQLGetData(hStmt, 4, SQL_C_UTINYINT, &bSp, 0, &c);
        SQLGetData(hStmt, 5, SQL_C_UTINYINT, &bTp, 0, &c);
        sLevelTemplate tpl;
        tpl.wLevel = wLevel; tpl.bCharType = bCharType; tpl.dwNeedExp = dwNeedExp; tpl.bSp = bSp; tpl.bTp = bTp;
        out[wLevel][bCharType] = tpl;
    });
}

void GameDataDB::LoadItemTemplates(std::map<WORD, sItemTemplate>& out) {
    out.clear();
    std::string q = "SELECT wRefID, bType, bKind, wVisualID, szName, dwCost, wLevel, bCharType, wAmount, nData1, nData2, nData3, nData4, nData5, nData6, nData7, nData8, nData9, nData10, nData11, nData12, nData13, nBasicData1, nBasicData2, nBasicData3, nBasicData4, nBasicData5, bCX, bCY FROM ITEMTEMPLATE";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        WORD ref=0, vis=0, lvl=0, amt=0; BYTE type=0, kind=0, charType=0, bCX=1, bCY=1; DWORD cost=0; char nameBuf[64]; SQLLEN c[29];
        int d1=0,d2=0,d3=0,d4=0,d5=0,d6=0,d7=0,d8=0,d9=0,d10=0,d11=0,d12=0,d13=0,b1=0,b2=0,b3=0,b4=0,b5=0;
        memset(nameBuf, 0, sizeof(nameBuf));
        SQLGetData(hStmt, 1, SQL_C_USHORT, &ref, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_UTINYINT, &type, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_UTINYINT, &kind, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_USHORT, &vis, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c[4]);
        SQLGetData(hStmt, 6, SQL_C_ULONG, &cost, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_USHORT, &lvl, 0, &c[6]);
        SQLGetData(hStmt, 8, SQL_C_UTINYINT, &charType, 0, &c[7]);
        SQLGetData(hStmt, 9, SQL_C_USHORT, &amt, 0, &c[8]);
        // Read nData columns individually (d1-d10 are separate stack vars, NOT a contiguous array)
        SQLGetData(hStmt, 10, SQL_C_SLONG, &d1, 0, &c[9]);
        SQLGetData(hStmt, 11, SQL_C_SLONG, &d2, 0, &c[10]);
        SQLGetData(hStmt, 12, SQL_C_SLONG, &d3, 0, &c[11]);
        SQLGetData(hStmt, 13, SQL_C_SLONG, &d4, 0, &c[12]);
        SQLGetData(hStmt, 14, SQL_C_SLONG, &d5, 0, &c[13]);
        SQLGetData(hStmt, 15, SQL_C_SLONG, &d6, 0, &c[14]);
        SQLGetData(hStmt, 16, SQL_C_SLONG, &d7, 0, &c[15]);
        SQLGetData(hStmt, 17, SQL_C_SLONG, &d8, 0, &c[16]);
        SQLGetData(hStmt, 18, SQL_C_SLONG, &d9, 0, &c[17]);
        SQLGetData(hStmt, 19, SQL_C_SLONG, &d10, 0, &c[18]);
        SQLGetData(hStmt, 20, SQL_C_SLONG, &d11, 0, &c[19]);
        SQLGetData(hStmt, 21, SQL_C_SLONG, &d12, 0, &c[20]);
        SQLGetData(hStmt, 22, SQL_C_SLONG, &d13, 0, &c[21]);
        SQLGetData(hStmt, 23, SQL_C_SLONG, &b1, 0, &c[22]);
        SQLGetData(hStmt, 24, SQL_C_SLONG, &b2, 0, &c[23]);
        SQLGetData(hStmt, 25, SQL_C_SLONG, &b3, 0, &c[24]);
        SQLGetData(hStmt, 26, SQL_C_SLONG, &b4, 0, &c[25]);
        SQLGetData(hStmt, 27, SQL_C_SLONG, &b5, 0, &c[26]);
        SQLGetData(hStmt, 28, SQL_C_UTINYINT, &bCX, 0, &c[27]);
        SQLGetData(hStmt, 29, SQL_C_UTINYINT, &bCY, 0, &c[28]);
        sItemTemplate tpl;
        tpl.wRefID = ref; tpl.bType = type; tpl.bKind = kind; tpl.wVisualID = vis;
        if (c[4] != SQL_NULL_DATA) tpl.szName = nameBuf;
        tpl.dwCost = cost; tpl.wLevel = lvl; tpl.bCharType = charType; tpl.wAmount = amt;
        tpl.bCX = bCX; tpl.bCY = bCY;
        if (tpl.bCX == 0) tpl.bCX = 1;
        if (tpl.bCY == 0) tpl.bCY = 1;
        tpl.nData1=d1; tpl.nData2=d2; tpl.nData3=d3; tpl.nData4=d4; tpl.nData5=d5;
        tpl.nData6=d6; tpl.nData7=d7; tpl.nData8=d8; tpl.nData9=d9; tpl.nData10=d10; tpl.nData11=d11; tpl.nData12=d12; tpl.nData13=d13;
        tpl.nBasicData1=b1; tpl.nBasicData2=b2; tpl.nBasicData3=b3; tpl.nBasicData4=b4; tpl.nBasicData5=b5;
        out[ref] = tpl;
    });
}

void GameDataDB::LoadNpcTemplates(std::map<BYTE, sNpcTemplate>& out) {
    out.clear();
    std::string q = "SELECT bType, dwHpInit, dwPwrInit, dwDefInit, wWalkSpeed100, wSightRangeInit, wWanderRangeInit, wMeleeAtkRangeInit, wShotAtkRangeInit, szName, wAtkRatio, wAvoidRatio, wRegen, dwExpInit, wHealPoint, bIdleRatio, bLevel, bWalkSpeed, wAtkInterval, wStaggerTime FROM NPCTEMPLATE";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        int t=0,hp=0,pwr=0,def1=0,wspeed=0,sight=0,wander=0,melee=0,shot=0,atkratio=0,avoidratio=0,regen=0,expinit=0,healpoint=0,idleratio=0,lvl=0,walkspdbyte=0,atkinterval=1500,staggertime=500;
        char nameBuf[64] = {0}; SQLLEN c[20] = {0};
        SQLGetData(hStmt,1,SQL_C_SLONG,&t,0,&c[0]); SQLGetData(hStmt,2,SQL_C_SLONG,&hp,0,&c[1]);
        SQLGetData(hStmt,3,SQL_C_SLONG,&pwr,0,&c[2]); SQLGetData(hStmt,4,SQL_C_SLONG,&def1,0,&c[3]);
        SQLGetData(hStmt,5,SQL_C_SLONG,&wspeed,0,&c[4]); SQLGetData(hStmt,6,SQL_C_SLONG,&sight,0,&c[5]);
        SQLGetData(hStmt,7,SQL_C_SLONG,&wander,0,&c[6]); SQLGetData(hStmt,8,SQL_C_SLONG,&melee,0,&c[7]);
        SQLGetData(hStmt,9,SQL_C_SLONG,&shot,0,&c[8]); SQLGetData(hStmt,10,SQL_C_CHAR,nameBuf,sizeof(nameBuf),&c[9]);
        SQLGetData(hStmt,11,SQL_C_SLONG,&atkratio,0,&c[10]); SQLGetData(hStmt,12,SQL_C_SLONG,&avoidratio,0,&c[11]);
        SQLGetData(hStmt,13,SQL_C_SLONG,&regen,0,&c[12]); SQLGetData(hStmt,14,SQL_C_SLONG,&expinit,0,&c[13]);
        SQLGetData(hStmt,15,SQL_C_SLONG,&healpoint,0,&c[14]); SQLGetData(hStmt,16,SQL_C_SLONG,&idleratio,0,&c[15]);
        SQLGetData(hStmt,17,SQL_C_SLONG,&lvl,0,&c[16]); SQLGetData(hStmt,18,SQL_C_SLONG,&walkspdbyte,0,&c[17]);
        SQLGetData(hStmt,19,SQL_C_SLONG,&atkinterval,0,&c[18]); SQLGetData(hStmt,20,SQL_C_SLONG,&staggertime,0,&c[19]);
        sNpcTemplate tpl;
        tpl.bType = t; tpl.dwHpInit = hp; tpl.dwPwrInit = pwr; tpl.dwDefInit = def1;
        tpl.bLevel = (c[16]!=SQL_NULL_DATA) ? lvl : 1;
        tpl.bWalkSpeed = (c[17]!=SQL_NULL_DATA) ? walkspdbyte : 8;
        tpl.dwExpInit = (c[13]!=SQL_NULL_DATA) ? expinit : 0;
        tpl.wWalkSpeed100 = wspeed; tpl.wSightRangeInit = sight; tpl.wWanderRangeInit = wander;
        tpl.wMeleeAtkRangeInit = melee; tpl.wShotAtkRangeInit = shot;
        tpl.wAtkRatio = (c[10]!=SQL_NULL_DATA) ? atkratio : 0;
        tpl.wAvoidRatio = (c[11]!=SQL_NULL_DATA) ? avoidratio : 0;
        tpl.wRegen = (c[12]!=SQL_NULL_DATA) ? regen : 10;
        tpl.wHealPoint = (c[14]!=SQL_NULL_DATA) ? healpoint : 0;
        tpl.bIdleRatio = (c[15]!=SQL_NULL_DATA) ? idleratio : 50;
        tpl.wAtkInterval = (c[18]!=SQL_NULL_DATA) ? atkinterval : 1500;
        tpl.wStaggerTime = (c[19]!=SQL_NULL_DATA) ? staggertime : 500;
        if (c[9] != SQL_NULL_DATA) tpl.szName = nameBuf;
        out[t] = tpl;
    });
}

void GameDataDB::LoadNpcMugongTemplates(std::map<BYTE, sNpcTemplate>& npcTemplates) {
    std::string q = "SELECT bNpcType, dwMugongID, bMugongLevel, bSelectID, wSelectParam1, wSelectParam2, wAttackRange FROM NPC_MUGONGTEMPLATE";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        int t,mid,mlvl,sel,p1,p2,arange; SQLLEN c[7];
        SQLGetData(hStmt,1,SQL_C_SLONG,&t,0,&c[0]); SQLGetData(hStmt,2,SQL_C_SLONG,&mid,0,&c[1]);
        SQLGetData(hStmt,3,SQL_C_SLONG,&mlvl,0,&c[2]); SQLGetData(hStmt,4,SQL_C_SLONG,&sel,0,&c[3]);
        SQLGetData(hStmt,5,SQL_C_SLONG,&p1,0,&c[4]); SQLGetData(hStmt,6,SQL_C_SLONG,&p2,0,&c[5]);
        SQLGetData(hStmt,7,SQL_C_SLONG,&arange,0,&c[6]);
        if (npcTemplates.count(t)) {
            sNpcMugong mg;
            mg.dwMugongID=mid; mg.bMugongLevel=mlvl; mg.bSelectID=sel;
            mg.wSelectParam1=p1; mg.wSelectParam2=p2; mg.wAttackRange=arange;
            npcTemplates[t].mugongs.push_back(mg);
        }
    });
}

void GameDataDB::LoadFunctionalNpcs(std::vector<MonsterData>& out) {
    std::string q = "SELECT dwID, dwMapID, wPosX, wPosY, bHeight, bType, szName FROM FunctionalNpcList";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        int id,mapid,x,y,h,t; char nameBuf[64]; SQLLEN c[7];
        SQLGetData(hStmt,1,SQL_C_SLONG,&id,0,&c[0]); SQLGetData(hStmt,2,SQL_C_SLONG,&mapid,0,&c[1]);
        SQLGetData(hStmt,3,SQL_C_SLONG,&x,0,&c[2]); SQLGetData(hStmt,4,SQL_C_SLONG,&y,0,&c[3]);
        SQLGetData(hStmt,5,SQL_C_SLONG,&h,0,&c[4]); SQLGetData(hStmt,6,SQL_C_SLONG,&t,0,&c[5]);
        SQLGetData(hStmt,7,SQL_C_CHAR,nameBuf,sizeof(nameBuf),&c[6]);
        MonsterData obj;
        obj.dwObjectID = id + 100000; obj.bObjectType = 5;
        obj.dwMapID = mapid; obj.wPosX = x; obj.wPosY = y;
        obj.fPosX = (float)x; obj.fPosY = (float)y; obj.wSpawnX = x; obj.wSpawnY = y;
        obj.bHeight = h; obj.bPropType = t;
        if (c[6] != SQL_NULL_DATA) obj.szName = nameBuf;
        out.push_back(obj);
    });
}

void GameDataDB::LoadFunctionalNpcItems(std::vector<MonsterData>& worldObjects) {
    std::string q = "SELECT dwObjectID, dwItemID, dwPrice, wAmount, bRarity, bPos, bSackCnt FROM FunctionalNpcItem";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        int objid=0,itemid=0,price=0,amt=0,rarity=0,pos=0,sackcnt=0; SQLLEN c[7];
        SQLGetData(hStmt,1,SQL_C_SLONG,&objid,0,&c[0]); SQLGetData(hStmt,2,SQL_C_SLONG,&itemid,0,&c[1]);
        SQLGetData(hStmt,3,SQL_C_SLONG,&price,0,&c[2]); SQLGetData(hStmt,4,SQL_C_SLONG,&amt,0,&c[3]);
        SQLGetData(hStmt,5,SQL_C_SLONG,&rarity,0,&c[4]); SQLGetData(hStmt,6,SQL_C_SLONG,&pos,0,&c[5]);
        SQLGetData(hStmt,7,SQL_C_SLONG,&sackcnt,0,&c[6]);
        DWORD actualObjID = objid + 100000;
        for (auto& obj : worldObjects) {
            if (obj.dwObjectID == actualObjID && obj.bObjectType == 5) {
                sFunctionalNpcItem item;
                item.dwItemID=itemid; item.dwPrice=price; item.wAmount=amt;
                item.bRarity=rarity; item.bPos=pos; item.bSackCnt=sackcnt;
                obj.npcItems.push_back(item);
                break;
            }
        }
    });
}

void GameDataDB::LoadNpcList(std::vector<MonsterData>& out, const std::map<BYTE, sNpcTemplate>& npcTemplates) {
    std::string q = "SELECT dwID, dwMapID, wPosX, wPosY, bNpcType, szName, dwMovePattern, dwAttackPattern, wWanderRangeInc, wCount, wPosRange, wSightRangeInc, wHpInc, wPwrInc, wDefInc, dwExpInc, wMeleeAtkRangeInc, wShotAtkRangeInc, wRootItem, wRootMoney, wRootRes, wRootBook, wLevel, bGroupOrder, wAtkRatioInc, wAvoidRatioInc FROM NPCLIST";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        int id=0,mapid=0,x=0,y=0,t=0,mp=0,ap=0,wander=0,count=0,range=0,sightInc=0,hpInc=0,pwrInc=0,defInc=0,expInc=0,meleeInc=0,shotInc=0,rItem=0,rMoney=0,rRes=0,rBook=0,npcLevel=0,groupOrder=0,atkRatioInc=0,avoidRatioInc=0;
        char nameBuf[64] = {0}; SQLLEN c[26] = {0};
        SQLGetData(hStmt,1,SQL_C_SLONG,&id,0,&c[0]); SQLGetData(hStmt,2,SQL_C_SLONG,&mapid,0,&c[1]);
        SQLGetData(hStmt,3,SQL_C_SLONG,&x,0,&c[2]); SQLGetData(hStmt,4,SQL_C_SLONG,&y,0,&c[3]);
        SQLGetData(hStmt,5,SQL_C_SLONG,&t,0,&c[4]); SQLGetData(hStmt,6,SQL_C_CHAR,nameBuf,sizeof(nameBuf),&c[5]);
        SQLGetData(hStmt,7,SQL_C_SLONG,&mp,0,&c[6]); SQLGetData(hStmt,8,SQL_C_SLONG,&ap,0,&c[7]);
        SQLGetData(hStmt,9,SQL_C_SLONG,&wander,0,&c[8]); SQLGetData(hStmt,10,SQL_C_SLONG,&count,0,&c[9]);
        SQLGetData(hStmt,11,SQL_C_SLONG,&range,0,&c[10]); SQLGetData(hStmt,12,SQL_C_SLONG,&sightInc,0,&c[11]);
        SQLGetData(hStmt,13,SQL_C_SLONG,&hpInc,0,&c[12]); SQLGetData(hStmt,14,SQL_C_SLONG,&pwrInc,0,&c[13]);
        SQLGetData(hStmt,15,SQL_C_SLONG,&defInc,0,&c[14]); SQLGetData(hStmt,16,SQL_C_SLONG,&expInc,0,&c[15]);
        SQLGetData(hStmt,17,SQL_C_SLONG,&meleeInc,0,&c[16]); SQLGetData(hStmt,18,SQL_C_SLONG,&shotInc,0,&c[17]);
        SQLGetData(hStmt,19,SQL_C_SLONG,&rItem,0,&c[18]); SQLGetData(hStmt,20,SQL_C_SLONG,&rMoney,0,&c[19]);
        SQLGetData(hStmt,21,SQL_C_SLONG,&rRes,0,&c[20]); SQLGetData(hStmt,22,SQL_C_SLONG,&rBook,0,&c[21]);
        SQLGetData(hStmt,23,SQL_C_SLONG,&npcLevel,0,&c[22]); SQLGetData(hStmt,24,SQL_C_SLONG,&groupOrder,0,&c[23]);
        SQLGetData(hStmt,25,SQL_C_SLONG,&atkRatioInc,0,&c[24]); SQLGetData(hStmt,26,SQL_C_SLONG,&avoidRatioInc,0,&c[25]);
        if (c[9]==SQL_NULL_DATA||count<=0) count=1;
        if (c[10]==SQL_NULL_DATA) range=0;
        for (int i=0; i<count; ++i) {
            MonsterData obj;
            obj.dwObjectID = id + 200000 + (i * 1000000); obj.bObjectType = 3;
            obj.dwMapID = mapid;
            int offsetX=0, offsetY=0;
            if (range>0) { offsetX=(rand()%(range*2+1))-range; offsetY=(rand()%(range*2+1))-range; }
            obj.wPosX = x+offsetX; if(obj.wPosX<0)obj.wPosX=0;
            obj.wPosY = y+offsetY; if(obj.wPosY<0)obj.wPosY=0;
            obj.fPosX=(float)obj.wPosX; obj.fPosY=(float)obj.wPosY;
            obj.wSpawnX=x; obj.wSpawnY=y; obj.wSpawnRange=range;
            obj.bHeight=1; obj.bPropType=t; obj.dwMovePattern=mp; obj.dwAttackPattern=ap;
            obj.wRootItem = (c[18]!=SQL_NULL_DATA)?rItem:0;
            obj.wRootMoney = (c[19]!=SQL_NULL_DATA)?rMoney:0;
            obj.wRootRes = (c[20]!=SQL_NULL_DATA)?rRes:0;
            obj.wRootBook = (c[21]!=SQL_NULL_DATA)?rBook:0;
            if (c[5]!=SQL_NULL_DATA) obj.szName=nameBuf;
            if (obj.szName.empty() && npcTemplates.count(t)) obj.szName=npcTemplates.at(t).szName;
            if (npcTemplates.count(t)) {
                const sNpcTemplate& tpl = npcTemplates.at(t);
                obj.dwHpMax = tpl.dwHpInit + (c[12]!=SQL_NULL_DATA?hpInc:0);
                obj.dwHpCur = obj.dwHpMax;
                obj.wWepAtk = tpl.dwPwrInit + (c[13]!=SQL_NULL_DATA?pwrInc:0);
                obj.wSightRange = tpl.wSightRangeInit + (c[11]!=SQL_NULL_DATA?sightInc:0);
                obj.wWanderRange = tpl.wWanderRangeInit + (c[8]!=SQL_NULL_DATA?wander:0);
                obj.wMeleeAtkRange = tpl.wMeleeAtkRangeInit + (c[16]!=SQL_NULL_DATA?meleeInc:0);
                obj.wShotAtkRange = tpl.wShotAtkRangeInit + (c[17]!=SQL_NULL_DATA?shotInc:0);
                obj.dwExp = tpl.dwExpInit + (c[15]!=SQL_NULL_DATA?expInc:0);
                if (c[22]!=SQL_NULL_DATA&&npcLevel>0) obj.wLevel=(WORD)npcLevel; else obj.wLevel=(WORD)tpl.bLevel;
                if (c[23]!=SQL_NULL_DATA) obj.bGroupOrder=(BYTE)groupOrder; else obj.bGroupOrder=0;
                obj.wAtkRatio = tpl.wAtkRatio + (c[24]!=SQL_NULL_DATA?atkRatioInc:0);
                obj.wAvoidRatio = tpl.wAvoidRatio + (c[25]!=SQL_NULL_DATA?avoidRatioInc:0);
                obj.bWalkSpeedByte = tpl.bWalkSpeed;
            }
            out.push_back(obj);
        }
    });
}

void GameDataDB::LoadRootItems(std::map<BYTE, std::vector<sRootItem>>& out) {
    out.clear();
    DBHelper::GetInstance().ExecuteQuery("SELECT bNpcType, dwItemID, wItemRatio FROM NPC_ROOTITEM", [&](SQLHSTMT stmt) {
        BYTE bType; DWORD itemID; WORD ratio; SQLLEN cb[3];
        SQLGetData(stmt,1,SQL_C_UTINYINT,&bType,0,&cb[0]);
        SQLGetData(stmt,2,SQL_C_ULONG,&itemID,0,&cb[1]);
        SQLGetData(stmt,3,SQL_C_USHORT,&ratio,0,&cb[2]);
        sRootItem ri; ri.dwItemID=itemID; ri.wItemRatio=ratio;
        out[bType].push_back(ri);
    });
}

void GameDataDB::LoadDropGroups(std::map<BYTE, std::vector<sDropGroup>>& out) {
    out.clear();
    DBHelper::GetInstance().ExecuteQuery("SELECT bNpcType, dwGroupID, wDropRate, bMinDrop, bMaxDrop FROM NPC_DROPGROUP ORDER BY bNpcType, dwGroupID", [&](SQLHSTMT stmt) {
        BYTE bNpcType=0; int dwGroupID=0,wDropRate=1,bMinDrop=1,bMaxDrop=1; SQLLEN c[5];
        SQLGetData(stmt,1,SQL_C_UTINYINT,&bNpcType,0,&c[0]);
        SQLGetData(stmt,2,SQL_C_SLONG,&dwGroupID,0,&c[1]);
        SQLGetData(stmt,3,SQL_C_SLONG,&wDropRate,0,&c[2]);
        SQLGetData(stmt,4,SQL_C_SLONG,&bMinDrop,0,&c[3]);
        SQLGetData(stmt,5,SQL_C_SLONG,&bMaxDrop,0,&c[4]);
        sDropGroup group;
        group.bNpcType=bNpcType; group.dwGroupID=(DWORD)dwGroupID;
        group.wDropRate=(c[2]!=SQL_NULL_DATA)?(WORD)wDropRate:1;
        group.bMinDrop=(c[3]!=SQL_NULL_DATA)?(BYTE)bMinDrop:1;
        group.bMaxDrop=(c[4]!=SQL_NULL_DATA)?(BYTE)bMaxDrop:1;
        out[bNpcType].push_back(group);
    });
}

void GameDataDB::LoadDropGroupItems(std::map<BYTE, std::vector<sDropGroup>>& groups) {
    std::map<DWORD, sDropGroup*> idx;
    for (auto& pair : groups) for (auto& g : pair.second) idx[g.dwGroupID] = &g;
    DBHelper::GetInstance().ExecuteQuery("SELECT dwGroupID, dwItemID, wWeight FROM NPC_DROPGROUPITEM ORDER BY dwGroupID", [&](SQLHSTMT stmt) {
        int gid=0,iid=0,w=100; SQLLEN c[3];
        SQLGetData(stmt,1,SQL_C_SLONG,&gid,0,&c[0]);
        SQLGetData(stmt,2,SQL_C_SLONG,&iid,0,&c[1]);
        SQLGetData(stmt,3,SQL_C_SLONG,&w,0,&c[2]);
        auto it = idx.find((DWORD)gid);
        if (it != idx.end()) {
            sDropGroupItem item; item.dwItemID=(DWORD)iid; item.wWeight=(c[2]!=SQL_NULL_DATA)?(WORD)w:100;
            it->second->items.push_back(item);
        }
    });
}

void GameDataDB::LoadMugongTemplates(std::map<DWORD, sMugongTemplate>& out) {
    out.clear();
    DBHelper::GetInstance().ExecuteQuery("SELECT dwMugongID, bCharType, bType, bKind, szName FROM MUGONG_TEMPLATE", [&](SQLHSTMT hStmt) {
        int mid=0,ctype=0,type=0,kind=0; char nameBuf[64]; SQLLEN c[5];
        memset(nameBuf,0,sizeof(nameBuf));
        SQLGetData(hStmt,1,SQL_C_SLONG,&mid,0,&c[0]); SQLGetData(hStmt,2,SQL_C_SLONG,&ctype,0,&c[1]);
        SQLGetData(hStmt,3,SQL_C_SLONG,&type,0,&c[2]); SQLGetData(hStmt,4,SQL_C_SLONG,&kind,0,&c[3]);
        SQLGetData(hStmt,5,SQL_C_CHAR,nameBuf,sizeof(nameBuf),&c[4]);
        sMugongTemplate tpl;
        tpl.dwMugongID=mid; tpl.bCharType=(BYTE)ctype; tpl.bType=(BYTE)type; tpl.bKind=(BYTE)kind;
        if (c[4]!=SQL_NULL_DATA) tpl.szName=nameBuf;
        out[mid] = tpl;
    });
}

void GameDataDB::LoadMugongList(std::map<DWORD, std::map<BYTE, sMugongList>>& out) {
    out.clear();
    DBHelper::GetInstance().ExecuteQuery("SELECT dwMugongID, bLevel, bLimitLevel, bReadOnlyBook, wNeedTP, wIncAtk, wDistance, wReduceIP, dwKeepUpTime, wIncAtkPerc, wIncDef, wIncDefPerc, wIncRate, wIncRatePerc, wIncHpMax, wIncHpCur, wIncHpCurPerc, wRecoverHp, wRecoverHpPerc, wIncIpMax, wIncIpCur, wIncIpCurPerc, wRecoverIp, wRecoverIpPerc, wIncCritical, wIncCriticalPerc FROM MUGONG_LIST", [&](SQLHSTMT hStmt) {
        int vals[26]={0}; SQLLEN c[26];
        for (int i=0;i<26;i++) { SQLGetData(hStmt,i+1,SQL_C_SLONG,&vals[i],0,&c[i]); if(c[i]==SQL_NULL_DATA) vals[i]=0; }
        sMugongList lst;
        lst.dwMugongID=vals[0]; lst.bLevel=(BYTE)vals[1]; lst.bLimitLevel=(BYTE)vals[2]; lst.bReadOnlyBook=(BYTE)vals[3];
        lst.dwNeedPoint=(DWORD)vals[4]; lst.dwNeedMoney=0; lst.dwDamageMul=(DWORD)vals[5]; lst.wAttackRange=(WORD)vals[6];
        lst.dwCostMp=(DWORD)vals[7]; lst.dwKeepUpTime=(DWORD)vals[8];
        lst.wIncAtk=(WORD)vals[5]; lst.wIncAtkPerc=(WORD)vals[9]; lst.wIncDef=(WORD)vals[10]; lst.wIncDefPerc=(WORD)vals[11];
        lst.wIncRate=(WORD)vals[12]; lst.wIncRatePerc=(WORD)vals[13]; lst.wIncHpMax=(WORD)vals[14];
        lst.wIncHpCur=(WORD)vals[15]; lst.wIncHpCurPerc=(WORD)vals[16]; lst.wRecoverHp=(WORD)vals[17];
        lst.wRecoverHpPerc=(WORD)vals[18]; lst.wIncIpMax=(WORD)vals[19]; lst.wIncIpCur=(WORD)vals[20];
        lst.wIncIpCurPerc=(WORD)vals[21]; lst.wRecoverIp=(WORD)vals[22]; lst.wRecoverIpPerc=(WORD)vals[23];
        lst.wIncCritical=(WORD)vals[24]; lst.wIncCriticalPerc=(WORD)vals[25];
        out[lst.dwMugongID][lst.bLevel] = lst;
    });
}

int GameDataDB::LoadRequiredClientVersion() {
    int version = 1081;
    std::string q = "SELECT TOP 1 dwVersionID FROM " + g_Config.dbServerInfo + ".dbo.VersionInfo WHERE bFlag = 1";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN cb = 0;
        int v = 0;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &v, 0, &cb);
        if (cb != SQL_NULL_DATA && v > 0) {
            version = v;
        }
    });
    return version;
}
