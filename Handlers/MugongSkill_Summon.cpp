#include "MugongHandler.h"
#include "MugongAttackContext.h"
#include "../GameObjects/MugongManager.h"
#include "../GameObjects/PlayerManager.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../DBHelper.h"
#include <atomic>
#include <algorithm>

extern std::map<DWORD, CMapInstance*> g_MapInstances;

// 全局原子计数器，从 850000001 开始递增，保证分身 ObjectID 唯一
static std::atomic<DWORD> g_nextBunsinID(850000001);

// 根据技能决定最大分身数量
static int GetBunsinCountForSkill(DWORD dwMugongID) {
    if (dwMugongID == 173) return 2;  // 真分身决：2个分身
    return 1;                          // 其余：1个分身
}

// 召唤/分身类技能 Handler (bType=4, bKind=9)
// 完整实现：创建分身实体、广播出生包、注册追踪表
bool HandleSummonSkill(MugongAttackContext& ctx)
{
    sMugongList* pMugongData = MugongManager::GetInstance()->GetMugongLevelData(ctx.dwMugongID, ctx.bMugongLevel);
    if (!pMugongData) {
        LOG("[Bunsin] No mugong data for skill " + std::to_string(ctx.dwMugongID) + " level " + std::to_string(ctx.bMugongLevel));
        return true;
    }

    if (!g_MapInstances.count(ctx.playerMapID)) return true;
    CMapInstance* mapInst = g_MapInstances[ctx.playerMapID];

    // === 1. IP 扣除 ===
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(ctx.dwAttackID);
        if (!pObj) {
            LOG("[Bunsin] Caster not found: " + std::to_string(ctx.dwAttackID));
            return true;
        }
        if (pMugongData->dwCostMp > 0) {
            if (pObj->wIpCur < pMugongData->dwCostMp) {
                LOG("[Bunsin] IP insufficient for skill " + std::to_string(ctx.dwMugongID));
                return true;
            }
            pObj->wIpCur -= pMugongData->dwCostMp;

            // 同步 HP/IP 面板
            std::vector<BYTE> hpBuf(4);
            auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
            push4(pObj->dwHpMax); push4(pObj->dwHpCur);
            push4(pObj->wIpMax); push4(pObj->wIpCur);
            hpBuf.push_back(0);
            PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
            hpHead->id = 0x3B0D; hpHead->payloadSize = hpBuf.size() - 4;
            EncryptPacket(hpBuf.data(), 0x42);
            SafeSend(ctx.clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);
        }
    }

    // === 2. 清理旧分身 + 3. 创建新分身 ===
    DWORD ownerCharID = ctx.dwAttackID - 400000000;
    int bunsinCount = GetBunsinCountForSkill(ctx.dwMugongID);

    for (int i = 0; i < bunsinCount; i++) {
        DWORD bunsinObjID = g_nextBunsinID.fetch_add(1);

        std::lock_guard<std::mutex> lock(mapInst->GetMutex());

        // 首轮循环时清理旧分身（在锁内执行，确保 RemovePlayer 线程安全）
        if (i == 0) {
            CleanupAllBunsins(ownerCharID, ctx.playerMapID);
        }

        sServerObject* pCaster = mapInst->GetPlayer(ctx.dwAttackID);
        if (!pCaster) break;

        // 快照攻击力和 HP（施放瞬间冻结，不动态跟随主人）
        DWORD snapshotAtk = pCaster->dwTotalAtk * pMugongData->wIncAtkPerc / 100;
        DWORD snapshotHp = pCaster->dwHpCur;  // 当前 HP 作为分身生命值
        if (snapshotHp == 0) snapshotHp = 1;   // 安全兜底

        // 创建分身 PlayerData
        PlayerData bunsin;
        bunsin.dwObjectID = bunsinObjID;
        bunsin.bObjectType = 4;               // OBJTYPE_PET
        bunsin.dwMapID = ctx.playerMapID;
        bunsin.bIsBunsin = true;
        bunsin.dwOwnerID = ownerCharID;
        if (ctx.dwMugongID == 71) {
            bunsin.bNpcType = 250;                // 客户端 NpcType 250 = 幻兽
        } else {
            bunsin.bNpcType = 251;                // 客户端 NpcType 251 = 剑影分身
        }
        bunsin.dwBunsinEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;

        // 位置：主人身旁偏移，多个分身错开
        int offsetX = (i == 0) ? 3 : -3;
        int offsetY = (i == 0) ? 2 : -2;
        bunsin.wPosX = pCaster->wPosX + offsetX;
        bunsin.wPosY = pCaster->wPosY + offsetY;
        bunsin.bHeight = pCaster->bHeight;
        bunsin.fPosX = (float)bunsin.wPosX;
        bunsin.fPosY = (float)bunsin.wPosY;

        // 属性快照
        bunsin.wWepAtk = snapshotAtk;
        bunsin.dwTotalAtk = snapshotAtk;
        bunsin.dwHpCur = snapshotHp;
        bunsin.dwHpMax = snapshotHp;
        bunsin.wWalkSpeed = pCaster->wWalkSpeed;
        bunsin.wLevel = pCaster->wLevel;
        bunsin.bRebirth = pCaster->bRebirth;
        bunsin.bPropType = pCaster->bPropType;

        // 复制主人装备外观（用于 PETINFO_ACK 中回传给客户端）
        for (int v = 0; v < 6 && v < 9; v++) {
            bunsin.wBunsinVisualID[v] = pCaster->wVisualID[v];
        }

        // 写入地图
        mapInst->AddPlayer(bunsin);

        // 写入追踪表
        g_BunsinMap[ownerCharID].push_back(bunsinObjID);

        LOG("[Bunsin] Created clone ObjID=" + std::to_string(bunsinObjID)
            + " for owner " + std::to_string(ownerCharID)
            + " ATK=" + std::to_string(snapshotAtk)
            + " HP=" + std::to_string(snapshotHp)
            + " Duration=" + std::to_string(pMugongData->dwKeepUpTime) + "s"
            + " at (" + std::to_string(bunsin.wPosX) + "," + std::to_string(bunsin.wPosY) + ")");

        // === 4. 广播 CS_NC_MAPENTER_ACK (0x3502) ===
        // 客户端收到后会自动发 PETINFO_REQ 请求详细信息
        {
            std::vector<BYTE> enterBuf(4);
            enterBuf.push_back(0); // bResult = 0
            // dwMapID
            DWORD mapID = ctx.playerMapID;
            enterBuf.push_back(mapID & 0xFF); enterBuf.push_back((mapID >> 8) & 0xFF);
            enterBuf.push_back((mapID >> 16) & 0xFF); enterBuf.push_back((mapID >> 24) & 0xFF);
            // dwObjectID
            enterBuf.push_back(bunsinObjID & 0xFF); enterBuf.push_back((bunsinObjID >> 8) & 0xFF);
            enterBuf.push_back((bunsinObjID >> 16) & 0xFF); enterBuf.push_back((bunsinObjID >> 24) & 0xFF);
            // bObjectType = 4 (PET)
            enterBuf.push_back(4);
            // wPosX, wPosY
            enterBuf.push_back(bunsin.wPosX & 0xFF); enterBuf.push_back(bunsin.wPosX >> 8);
            enterBuf.push_back(bunsin.wPosY & 0xFF); enterBuf.push_back(bunsin.wPosY >> 8);
            // bHeight
            enterBuf.push_back(bunsin.bHeight);
            // wDirection = 0
            enterBuf.push_back(0); enterBuf.push_back(0);
            // bStatus = 0 (Stand)
            enterBuf.push_back(0);
            // bSpeed
            enterBuf.push_back(bunsin.wWalkSpeed & 0xFF);

            PACKET_HEADER* enterHead = (PACKET_HEADER*)enterBuf.data();
            enterHead->id = 0x3502; // CS_NC_MAPENTER_ACK
            enterHead->payloadSize = enterBuf.size() - sizeof(PACKET_HEADER);
            EncryptPacket(enterBuf.data(), 0x42);
            BroadcastPacketToMap(ctx.playerMapID, enterBuf);
        }
    }

    // === 5. 广播 0x4016 施法动画 ===
    {
        std::vector<BYTE> ackBuf(4 + 38, 0);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0; // bResult = SUCCESS (0)
        *(DWORD*)(p + 1) = ctx.dwMugongID;
        p[5] = ctx.bMugongLevel;
        p[6] = 1; // bAtkType = PC
        *(DWORD*)(p + 7) = ctx.dwAttackID;
        *(WORD*)(p + 11) = ctx.wAttackPosX;
        *(WORD*)(p + 13) = ctx.wAttackPosY;
        p[15] = ctx.bAttackHeight;
        p[16] = ctx.bDefenseType;
        *(DWORD*)(p + 17) = ctx.dwDefenseID;

        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4016;
        head->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(ctx.playerMapID, ackBuf);
    }

    return true;
}

// === PETINFO_REQ Handler ===
// 客户端收到 MAPENTER_ACK(bObjectType=4) 后发来此请求
// payload: DWORD dwObjectID + DWORD dwMapID
void OnPetInfoReq(SOCKET clientSocket, BYTE* payload, WORD payloadSize)
{
    if (payloadSize < 8) return;

    DWORD dwObjectID = *(DWORD*)(payload);
    DWORD dwMapID = *(DWORD*)(payload + 4);

    if (!g_MapInstances.count(dwMapID)) return;
    CMapInstance* mapInst = g_MapInstances[dwMapID];

    std::lock_guard<std::mutex> lock(mapInst->GetMutex());
    PlayerData* pPet = mapInst->GetPlayer(dwObjectID);
    if (!pPet) {
        LOG("[PetInfo] Object " + std::to_string(dwObjectID) + " not found in map " + std::to_string(dwMapID));
        return;
    }

    bool isRealPet = (dwObjectID >= 800000000 && dwObjectID < 850000000);
    if (!isRealPet && !pPet->bIsBunsin) {
        LOG("[PetInfo] Object " + std::to_string(dwObjectID) + " is neither real pet nor bunsin");
        return;
    }

    std::string szPetName = "";
    if (isRealPet) {
        DWORD dwPetID = dwObjectID - 800000000;
        std::string q = "SELECT szName FROM CHAR_PET WHERE dwID = " + std::to_string(dwPetID);
        DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
            char nameBuf[32] = {0};
            SQLGetData(hStmt, 1, SQL_C_CHAR, nameBuf, sizeof(nameBuf), NULL);
            szPetName = nameBuf;
        });
        szPetName.erase(std::remove_if(szPetName.begin(), szPetName.end(), ::isspace), szPetName.end());
        if (szPetName.empty()) szPetName = "Pet";
    } else {
        if (pPet->bNpcType == 250) {
            szPetName = "\xbb\xc3\xca\xde"; // "幻兽" 的 GBK 编码
        } else {
            szPetName = "Clone";
        }
    }

    // 组装 PETINFO_ACK (0x3538)
    std::vector<BYTE> buf(4);
    auto pushByte = [&](BYTE b) { buf.push_back(b); };
    auto pushWord = [&](WORD w) { buf.push_back(w & 0xFF); buf.push_back((w >> 8) & 0xFF); };
    auto pushDWord = [&](DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back((d >> 24) & 0xFF); };
    auto pushString = [&](const std::string& s) {
        WORD len = (WORD)s.size();
        pushWord(len);
        buf.insert(buf.end(), s.begin(), s.end());
    };

    pushByte(0);                         // bResult = 0
    pushDWord(dwObjectID);               // dwID
    pushByte(pPet->bNpcType);            // bNpcType
    pushDWord(dwMapID);                  // dwMapID
    pushWord(pPet->wPosX);               // wPosX
    pushWord(pPet->wPosY);               // wPosY
    pushByte(pPet->bHeight);             // bHeight
    pushWord(0);                         // wDirection
    pushString(szPetName);               // szName
    pushByte(0);                         // bStatus = Stand
    pushWord(pPet->wPosX);               // wDesPosX
    pushWord(pPet->wPosY);               // wDesPosY
    pushByte(pPet->bHeight);             // bDesHeight
    pushDWord(pPet->dwHpMax);            // dwHpMax
    pushDWord(pPet->dwHpCur);            // dwHpCur
    pushByte(pPet->wWalkSpeed & 0xFF);   // bSpeed
    pushDWord(pPet->dwOwnerID + 400000000); // dwOwnerID (转回800M格式)
    pushByte(pPet->bRebirth);            // bRevolutionStep

    // wVisualID[6] —— 外观装备。如果是真实野生宠物，强制全充 0，避免客户端崩溃
    for (int i = 0; i < 6; i++) {
        if (isRealPet) {
            pushWord(0);
        } else {
            pushWord(pPet->wBunsinVisualID[i]);
        }
    }

    PACKET_HEADER* head = (PACKET_HEADER*)buf.data();
    head->id = 0x3538; // CS_NC_PETINFO_ACK = OFFSET_CS_NC + 55
    head->payloadSize = buf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), buf.size(), 0);

    if (isRealPet && pPet->bNeedTamingAck) {
        extern void SendTamingAck(SOCKET clientSocket, BYTE bResult, DWORD dwObjectID, BYTE bType);
        SendTamingAck(clientSocket, 0, dwObjectID, 0);
        pPet->bNeedTamingAck = false; // 发送完立即抹除，保证一生只发送一次
    }

    LOG("[PetInfo] Sent PETINFO_ACK for " + std::string(isRealPet ? "RealPet" : "Bunsin") + " ObjID=" + std::to_string(dwObjectID)
        + " Name=" + szPetName + " HP=" + std::to_string(pPet->dwHpCur) + "/" + std::to_string(pPet->dwHpMax)
        + " Owner=" + std::to_string(pPet->dwOwnerID));
}
