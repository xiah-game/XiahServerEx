#pragma once
#include "../ServerCore.h"

// OnMugongAttackReq 解包后的共享上下文，传递给各子模块
struct MugongAttackContext {
    SOCKET clientSocket;
    DWORD charID;

    // 从 payload 解包的字段
    DWORD dwMugongID;
    BYTE bAttackType;
    DWORD dwAttackID;
    WORD wAttackPosX, wAttackPosY;
    BYTE bAttackHeight;
    BYTE bDefenseType;
    DWORD dwDefenseID;
    WORD wTargetPosX, wTargetPosY;
    BYTE bTargetHeight;

    // 查表得到的字段
    BYTE bMugongLevel;
    DWORD playerMapID;
    struct sMugongList* pMugongData;
    struct sMugongTemplate* pTemplate;  // MUGONG_TEMPLATE 行（bType, bKind, bCharType）
    bool isBuff;
    bool isHeal;
};

// === 各子模块的处理函数 ===

// 技能 41 分身创建（MugongSkill_Bunsin.cpp）
void HandleBunsinAttack(MugongAttackContext& ctx);

// 回血/治疗技能（MugongSkill_Buff.cpp）
void HandleHealSkill(MugongAttackContext& ctx);

// PvE 伤害技能 — 攻击怪物（MugongAttack_PvE.cpp）
void HandleDamageSkill_PvE(MugongAttackContext& ctx);

// PvP 伤害技能 — 攻击玩家（MugongAttack_PvP.cpp）
void HandleDamageSkill_PvP(MugongAttackContext& ctx);

// PreAttack 相关（MugongPreAttack.cpp）
void OnSelMugongReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnMugongPreAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);

// AOE 技能判定（MugongPreAttack.cpp）
bool IsAoeSkill(DWORD dwMugongID, struct sMugongTemplate* tpl);

// === 特殊技能 Handler（按 bType/bKind 数据驱动路由） ===
// 所有 Handler 统一签名：接收 MugongAttackContext& ctx
// 返回 true = 已完全处理（拦截通用路径）
// 返回 false = 继续走通用路径

// 召唤/分身类 (bType=4, bKind=9)
// 实现文件: MugongSkill_Summon.cpp
bool HandleSummonSkill(MugongAttackContext& ctx);

// PETINFO_REQ handler：客户端请求分身详细信息
// 实现文件: MugongSkill_Summon.cpp
void OnPetInfoReq(SOCKET clientSocket, BYTE* payload, WORD payloadSize);

// 五行必杀技 (bType=0, bKind=80~84)
// 实现文件: MugongSkill_FiveElm.cpp
// 返回 true=蓄气校验通过（继续走通用伤害）, false=蓄气不足（拦截释放）
bool HandleFiveElmUltimate(MugongAttackContext& ctx);

// 宠物捕捉类 (bType=2, bKind=17)
// 实现文件: MugongSkill_PetCapture.cpp
bool HandlePetCapture(MugongAttackContext& ctx);

