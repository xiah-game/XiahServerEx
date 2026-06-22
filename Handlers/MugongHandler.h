#pragma once
#include "../ServerCore.h"
#include <unordered_map>
#include <vector>

void OnMugongListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnMugongAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnMugongLearnReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnSelMugongReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnMugongPreAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
void OnSetOptionReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);

// 全局分身追踪表：ownerCharID -> 该玩家所有存活分身的 ObjectID 列表
extern std::unordered_map<DWORD, std::vector<DWORD>> g_BunsinMap;

// 根据技能等级返回分身上限数量：1-3=1, 4-6=1, 7-9=2, 10-13=3
int GetMaxBunsinCount(BYTE bMugongLevel);

// 清理指定玩家的所有分身（广播 0x3510 DIE + RemovePlayer）
void CleanupAllBunsins(DWORD ownerCharID, DWORD mapID);

// 清理单个分身（被打死时调用）
void CleanupSingleBunsin(DWORD ownerCharID, DWORD bunsinObjID, DWORD mapID);
