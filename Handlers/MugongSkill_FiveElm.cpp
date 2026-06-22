#include "MugongHandler.h"
#include "MugongAttackContext.h"
#include "../GameObjects/MugongManager.h"
#include "../GameObjects/PlayerManager.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../GameObjects/ExpSystem.h"
#include "../DB/CharacterDB.h"

extern std::map<DWORD, CMapInstance*> g_MapInstances;

// 五行蓄气阈值：满蓄气=5000，释放必杀技需消耗全部蓄气
static const DWORD FIVE_ELM_GAUGE_COST = 5000;

// 五行必杀技 Handler (bType=0, bKind=80~84)
// 职责：校验蓄气值 → 扣除蓄气 → DB落盘 → 同步客户端进度条
// 伤害计算仍走通用 PvE/PvP 路径（本函数不处理）
//
// 返回值:
//   true  = 蓄气校验通过，调用方继续走通用伤害路径
//   false = 蓄气不足，技能被拦截，调用方应 return
bool HandleFiveElmUltimate(MugongAttackContext& ctx)
{
    LOG("[MugongSkill_FiveElm] HandleFiveElmUltimate: Skill " + std::to_string(ctx.dwMugongID)
        + " Level " + std::to_string(ctx.bMugongLevel)
        + " by " + std::to_string(ctx.dwAttackID));

    // charID 已经是 +400000000 的 ObjectID 格式
    // DB 操作需要原始 charID（不含偏移）
    DWORD rawCharID = ctx.charID - 400000000;

    // === 蓄气值校验 ===
    PlayerData* pAttacker = nullptr;

    if (g_MapInstances.count(ctx.playerMapID)) {
        CMapInstance* mapInst = g_MapInstances[ctx.playerMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        pAttacker = mapInst->GetPlayer(ctx.charID);

        if (pAttacker && pAttacker->dwFiveElmGauge >= FIVE_ELM_GAUGE_COST) {
            pAttacker->dwFiveElmGauge -= FIVE_ELM_GAUGE_COST;

            LOG("[MugongSkill_FiveElm] Gauge deducted: " + std::to_string(pAttacker->dwFiveElmGauge + FIVE_ELM_GAUGE_COST)
                + " -> " + std::to_string(pAttacker->dwFiveElmGauge));
        } else {
            DWORD currentGauge = pAttacker ? pAttacker->dwFiveElmGauge : 0;
            LOG("[MugongSkill_FiveElm] Gauge insufficient: " + std::to_string(currentGauge)
                + " < " + std::to_string(FIVE_ELM_GAUGE_COST) + ", blocking skill " + std::to_string(ctx.dwMugongID));
            return false;
        }
    } else {
        LOG("[MugongSkill_FiveElm] Map not found: " + std::to_string(ctx.playerMapID));
        return false;
    }

    // === 蓄气值 DB 落盘 + 客户端同步 ===
    if (pAttacker) {
        CharacterDB::ExpData expData;
        if (CharacterDB::GetInstance().GetExpData(rawCharID, expData)) {
            CharacterDB::GetInstance().UpdateFiveElm(
                rawCharID,
                expData.wFiveElmPoint, expData.wFiveElmPointCnt,
                expData.dwFiveElmPower, pAttacker->dwFiveElmGauge,
                expData.wFireExp, expData.wWaterExp, expData.wWoodExp,
                expData.wMetalExp, expData.wEarthExp);
        }
        SyncFiveElmStatus(ctx.clientSocket, rawCharID, pAttacker);
    }

    return true;
}
