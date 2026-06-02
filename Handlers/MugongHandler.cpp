#include "MugongHandler.h"#include "PartyHandler.h"

#include "../GameObjects/MugongManager.h"
#include "../GameObjects/PlayerManager.h"
#include "../GameObjects/ExpSystem.h"
#include "../Network/SessionMgr.h"
#include "../MonsterAI.h"
#include "../GameObjects/DropManager.h"
#include "../GameObjects/MapInstance.h"
#include "../GameObjects/PlayerManager.h"
#include "../GameObjects/ExpSystem.h"
#include "../DB/CharacterDB.h"
#include <cmath>

extern std::map<DWORD, CMapInstance*> g_MapInstances;

void OnMugongListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 1) return;
    BYTE bMugongType = payload[0];
    LOG("[MugongHandler] Received CS_IT_MUGONGLIST_REQ (0x4418) with type: " + std::to_string(bMugongType));

    std::vector<BYTE> ackBuf(4);
    std::map<DWORD, BYTE> tempLearned;
    MugongManager::GetInstance()->LoadPlayerMugongs(charID, tempLearned);
    
    std::vector<std::pair<DWORD, BYTE>> filteredSkills;
    
    // Filter skills by type
    // The client strictly uses ID ranges to map skills to UI tabs based on .idx files:
    // IDs 1-29 are Passive (Ingong), IDs 30-149 are General (Outgong), IDs 191+ are Awakening
    for (auto& s : tempLearned) {
        DWORD id = s.first;
        bool isPassive = (id >= 1 && id <= 29);
        bool isGeneral = (id >= 30 && id <= 149);
        
        if (bMugongType == 0 && isGeneral) filteredSkills.push_back(s); // GENERAL/ACTIVE
        else if (bMugongType == 1 && isPassive) filteredSkills.push_back(s); // PASSIVE
    }
    
    ackBuf.push_back((BYTE)filteredSkills.size()); // bNumOfMugong
    
    for (auto& s : filteredSkills) {
        DWORD dwMugongID = s.first;
        BYTE bMugongLevel = s.second;
        sMugongTemplate* tpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);
        std::string name = tpl ? tpl->szName : "UnknownSkill";
        BYTE bKind = tpl ? tpl->bKind : 0;
        
        ackBuf.push_back(dwMugongID & 0xFF); ackBuf.push_back((dwMugongID >> 8) & 0xFF); ackBuf.push_back((dwMugongID >> 16) & 0xFF); ackBuf.push_back((dwMugongID >> 24) & 0xFF);
        
        if (bMugongType == 0) {
            // General Mugong requires string name
            WORD nameLen = (WORD)name.length();
            ackBuf.push_back(nameLen & 0xFF); ackBuf.push_back((nameLen >> 8) & 0xFF);
            for (char c : name) ackBuf.push_back(c);
        }
        
        ackBuf.push_back(bKind); // bKind
        ackBuf.push_back(bMugongLevel); // bMugongLevel
        ackBuf.push_back(0); // bMacroSeq
        
        LOG("[DEBUG] Sending Mugong ID: " + std::to_string(dwMugongID) + " Name: " + name + " bKind: " + std::to_string(bKind) + " bLevel: " + std::to_string(bMugongLevel));
    }
    
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    if (bMugongType == 0) ackHead->id = 0x441A; // CS_IT_GENERALMUGONGLIST_ACK
    else if (bMugongType == 1) ackHead->id = 0x441C; // CS_IT_PASSIVEMUGONGLIST_ACK
    else if (bMugongType == 2) ackHead->id = 0x441D; // CS_IT_ACTIVEMUGONGLIST_ACK
    else return;
    
    ackHead->payloadSize = (WORD)(ackBuf.size() - 4);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[MugongHandler] Sent Mugong List Type " + std::to_string(bMugongType) + " with " + std::to_string(filteredSkills.size()) + " skills.");

    // The client comments out SendCS_IT_MUGONGLIST_REQ(2) (Active) and SendCS_IT_MUGONGLIST_REQ(3).
    // So we must automatically send the Active Mugong List (0x441C) when the client requests Passive (1)
    if (bMugongType == 1) {
        std::vector<std::pair<DWORD, BYTE>> activeSkills;
        for (auto& s : tempLearned) {
            // Send Awakening skills (typically IDs 191-198 or specific higher tier skills)
            // that the client expects in 0x441C. For now, we assume ID >= 191 are Awakening.
            if (s.first >= 191) {
                activeSkills.push_back(s);
            }
        }
        
        std::vector<BYTE> actBuf(4);
        actBuf.push_back((BYTE)activeSkills.size());
        
        for (auto& s : activeSkills) {
            DWORD dwMugongID = s.first;
            BYTE bMugongLevel = s.second;
            sMugongTemplate* tpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);
            BYTE bKind = tpl ? tpl->bKind : 0;
            
            actBuf.push_back(dwMugongID & 0xFF); actBuf.push_back((dwMugongID >> 8) & 0xFF); actBuf.push_back((dwMugongID >> 16) & 0xFF); actBuf.push_back((dwMugongID >> 24) & 0xFF);
            actBuf.push_back(bKind);
            actBuf.push_back(bMugongLevel);
            actBuf.push_back(0); // bMacroSeq
        }
        
        PACKET_HEADER* actHead = (PACKET_HEADER*)actBuf.data();
        actHead->id = 0x441D; // CS_IT_ACTIVEMUGONGLIST_ACK
        actHead->payloadSize = (WORD)(actBuf.size() - 4);
        EncryptPacket(actBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)actBuf.data(), actBuf.size(), 0);
        LOG("[MugongHandler] Sent Active Mugong List (0x441C) automatically with " + std::to_string(activeSkills.size()) + " skills.");
    }
}



void OnMugongLearnReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    LOG("[MugongHandler] OnMugongLearnReq Received! Size: " + std::to_string(totalSize));
    if (totalSize < 4) return;
    DWORD dwMugongID = *(DWORD*)payload;
    LOG("[MugongHandler] Player " + std::to_string(charID) + " requests to upgrade skill: " + std::to_string(dwMugongID));
    MugongManager::GetInstance()->UpgradeMugong(clientSocket, charID, dwMugongID);
}

void OnSelMugongReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 6) return;

    BYTE bType = payload[0];
    DWORD dwMugongID = *(DWORD*)(payload + 1);
    BYTE bIndex = payload[5];

    LOG("[MugongHandler] OnSelMugongReq: charID " + std::to_string(charID) + " set skill " + std::to_string(dwMugongID) + " as active S slot.");

    std::vector<BYTE> ackBuf(4 + 7); // 4 for header, 7 for payload
    BYTE* p = ackBuf.data() + 4;
    p[0] = 0; // bResult = 0 (Success)
    p[1] = bType;
    p[2] = dwMugongID & 0xFF;
    p[3] = (dwMugongID >> 8) & 0xFF;
    p[4] = (dwMugongID >> 16) & 0xFF;
    p[5] = (dwMugongID >> 24) & 0xFF;
    p[6] = bIndex;

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4018; // CS_BT_SELMUGONG_ACK
    head->payloadSize = ackBuf.size() - 4;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
}

void OnMugongPreAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 24) return;
    
    DWORD dwMugongID    = *(DWORD*)(payload);
    BYTE bAttackType    = payload[4];
    DWORD dwAttackID    = *(DWORD*)(payload + 5);
    WORD wAttackPosX    = *(WORD*)(payload + 9);
    WORD wAttackPosY    = *(WORD*)(payload + 11);
    BYTE bAttackHeight  = payload[13];
    BYTE bDefenseType   = payload[14];
    DWORD dwDefenseID   = *(DWORD*)(payload + 15);
    WORD wTargetPosX    = *(WORD*)(payload + 19);
    WORD wTargetPosY    = *(WORD*)(payload + 21);
    BYTE bTargetHeight  = payload[23];

    LOG("[MugongHandler] OnMugongPreAttackReq: Skill " + std::to_string(dwMugongID) + " by " + std::to_string(dwAttackID) + " against " + std::to_string(dwDefenseID));

    std::vector<BYTE> ackBuf(4 + 29);
    BYTE* p = ackBuf.data() + 4;

    // Find Mugong level
    BYTE bMugongLevel = 1;
    int currentLvl = MugongManager::GetInstance()->GetPlayerMugongLevel(charID, dwMugongID);
    if(currentLvl > 0) bMugongLevel = currentLvl;

    sMugongList* pd = MugongManager::GetInstance()->GetMugongLevelData(dwMugongID, bMugongLevel);
    bool isBuff = (pd && pd->dwKeepUpTime > 0);

    DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);    // 检查控制类 Debuff (定身/冰冻/眩晕) 拦截技能起手
    if (g_MapInstances.count(playerMapID)) {
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
        if (pObj) {
            bool isCC = false;
            for (const auto& bf : pObj->activeBuffs) {
                if (bf.second.bIsDebuff) {
                    DWORD mugID = bf.second.dwMugongID;
                    if (mugID == 94 || mugID == 95 || mugID == 35 || mugID == 65 || mugID == 125) {
                        isCC = true;
                        break;
                    }
                }
            }
            if (isCC) {
                std::vector<BYTE> buf; buf.resize(4, 0);
                DWORD senderObjID = 0;
                buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
                buf.push_back(8); // CT_TIMEMESSAGE
                std::string msg = "[Control] You are frozen, stunned or immobilized and cannot cast skills!";
                WORD len = (WORD)msg.size();
                buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
                buf.insert(buf.end(), msg.begin(), msg.end());
                WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
                WORD payloadSize = (WORD)(buf.size() - 4);
                memcpy(&buf[0], &packetID, 2);
                memcpy(&buf[2], &payloadSize, 2);
                EncryptPacket(buf.data(), 0x42);
                SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
                return;
            }
        }
    }
    // 检查控制类 Debuff (定身/冰冻/眩晕) 拦截技能起手
    if (g_MapInstances.count(playerMapID)) {
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
        if (pObj) {
            bool isCC = false;
            for (const auto& bf : pObj->activeBuffs) {
                if (bf.second.bIsDebuff) {
                    DWORD mugID = bf.second.dwMugongID;
                    if (mugID == 94 || mugID == 95 || mugID == 35 || mugID == 65 || mugID == 125) {
                        isCC = true;
                        break;
                    }
                }
            }
            if (isCC) {
                std::vector<BYTE> buf; buf.resize(4, 0);
                DWORD senderObjID = 0;
                buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
                buf.push_back(8); // CT_TIMEMESSAGE
                std::string msg = "[Control] You are frozen, stunned or immobilized and cannot cast skills!";
                WORD len = (WORD)msg.size();
                buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
                buf.insert(buf.end(), msg.begin(), msg.end());
                WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
                WORD payloadSize = (WORD)(buf.size() - 4);
                memcpy(&buf[0], &packetID, 2);
                memcpy(&buf[2], &payloadSize, 2);
                EncryptPacket(buf.data(), 0x42);
                SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
                return;
            }
        }
    }


    if (isBuff) {
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
            if (pObj) {
                // Verify IP cost
                if (pObj->wIpCur < pd->dwCostMp) {
                    LOG("[MugongHandler] Buff " + std::to_string(dwMugongID) + " blocked: IP " 
                        + std::to_string(pObj->wIpCur) + " < cost " + std::to_string(pd->dwCostMp));
                    // Send 0x4016 with bResult=3 (IP insufficient) so client shows error message
                    std::vector<BYTE> failBuf(4 + 38, 0);
                    BYTE* fp = failBuf.data() + 4;
                    fp[0] = 3; // bResult = 3 (IDS_SHORT_INLIFE)
                    *(DWORD*)(fp + 1) = dwMugongID;
                    fp[5] = bMugongLevel;
                    fp[6] = 1; 
                    *(DWORD*)(fp + 7) = dwAttackID;
                    *(WORD*)(fp + 11) = wAttackPosX;
                    *(WORD*)(fp + 13) = wAttackPosY;
                    fp[15] = bAttackHeight;
                    fp[16] = 1; // bDefType = OBJTYPE_PC
                    *(DWORD*)(fp + 17) = dwAttackID; 
                    PACKET_HEADER* fh = (PACKET_HEADER*)failBuf.data();
                    fh->id = 0x4016;
                    fh->payloadSize = 38;
                    EncryptPacket(failBuf.data(), 0x42);
                    SafeSend(clientSocket, (const char*)failBuf.data(), failBuf.size(), 0);
                    return;
                }
                
                // Deduct IP
                pObj->wIpCur -= pd->dwCostMp;
                
                // Send 0x3B0D to update client HP/IP bars
                std::vector<BYTE> hpBuf(4);
                auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                push4(pObj->dwHpMax);
                push4(pObj->dwHpCur);
                push4(pObj->wIpMax);
                push4(pObj->wIpCur);
                hpBuf.push_back(0); // bType
                PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                hpHead->id = 0x3B0D;
                hpHead->payloadSize = hpBuf.size() - 4;
                EncryptPacket(hpBuf.data(), 0x42);
                SafeSend(clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);

                // Add or update buff
                sServerObject::sActiveBuff newBuff;
                newBuff.dwMugongID = dwMugongID;
                newBuff.bLevel = bMugongLevel;
                newBuff.dwEndTime = GetTickCount() + pd->dwKeepUpTime * 1000;
                newBuff.bIsDebuff = false;
                pObj->activeBuffs[dwMugongID] = newBuff;
            }
        }

        // Broadcast 0x402C KeepUpMugongStartAck
        std::vector<BYTE> buffAck(4 + 11);
        BYTE* bp = buffAck.data() + 4;
        bp[0] = 0; // bResult
        *(DWORD*)(bp + 1) = dwAttackID;
        bp[5] = 1; // bObjectType (Player)
        *(DWORD*)(bp + 6) = dwMugongID;
        bp[10] = bMugongLevel;
        
        PACKET_HEADER* headB = (PACKET_HEADER*)buffAck.data();
        headB->id = 0x402C;
        headB->payloadSize = 11;
        EncryptPacket(buffAck.data(), 0x42);
        BroadcastPacketToMap(playerMapID, buffAck);

        LOG("[MugongHandler] Applied BUFF " + std::to_string(dwMugongID) + " to player " + std::to_string(dwAttackID) + " in PreAttackReq");

        // Immediately recalculate stats so buff bonuses take effect (do NOT send 0x4414 to prevent clearing client visuals)
        PlayerManager::GetInstance().RecalculateStats(charID, false);
    } else {
        // é Buff ¨¤±èè·è·è§é¤éè 178
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
            if (pObj && pObj->activeBuffs.count(178) > 0) {
                pObj->activeBuffs[178].dwEndTime = 0; // Mark for instant expiry
                LOG("[MugongHandler] Player " + std::to_string(dwAttackID) + " pre-attacked with damage skill " + std::to_string(dwMugongID) + ". Expiring Stealth.");
            }
        }
    }

    // bResult (Always 0 to allow buff refreshing)
    p[0] = 0; 
    
    // Pack dwMugongID
    *(DWORD*)(p + 1) = dwMugongID;
    
    // Pack remaining bytes
    p[5] = bMugongLevel;
    // Determine if this is a buff skill - buff skills are intercepted by our client-side
    // patch in XiahGame_Handler_BT_Rcv.cpp to skip animation and combat sequence locks.
    // To ensure the client-side patch is reached, we must always set p[6] = 1 (OBJTYPE_PC)
    // so that FindXiahObject successfully locates the player character.
    p[6] = 1;
    *(DWORD*)(p + 7) = dwAttackID;
    *(WORD*)(p + 11) = wAttackPosX;
    *(WORD*)(p + 13) = wAttackPosY;
    p[15] = bAttackHeight;
    // For self-buff/no-target skills, set defender to self with OBJTYPE_PC
    if (dwDefenseID == 0) {
        dwDefenseID = dwAttackID;
        bDefenseType = 1; // OBJTYPE_PC
    }
    p[16] = bDefenseType;
    *(DWORD*)(p + 17) = dwDefenseID;
    *(WORD*)(p + 21) = wTargetPosX;
    *(WORD*)(p + 23) = wTargetPosY;
    p[25] = bTargetHeight;
    *(WORD*)(p + 26) = 0; // wLifeTime
    p[28] = 0; // bAttackMode

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4014; // CS_BT_MUGONGPREATTACK_ACK
    head->payloadSize = 29;
    EncryptPacket(ackBuf.data(), 0x42);
    
    // Broadcast to map so everyone sees the pre-attack animation
    BroadcastPacketToMap(playerMapID, ackBuf);
}

bool IsAoeSkill(DWORD dwMugongID, sMugongTemplate* tpl) {
    if (!tpl) return false;
    // 1. è±§¤§±è (bKind 80-84) ±¤
    if (tpl->bKind >= 80 && tpl->bKind <= 84) return true;
    
    // 2. éé¤ 80~120 ¨°éè§
    // ¨¤±è (bType = 3) è¤§±¤¤§± (bKind = 0) ¤¤
    if (tpl->bType == 3 && tpl->bKind == 0) return true;
    
    return false;
}

void OnMugongAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 24) return;
    
    DWORD dwMugongID    = *(DWORD*)(payload);
    BYTE bAttackType    = payload[4];
    DWORD dwAttackID    = *(DWORD*)(payload + 5);
    WORD wAttackPosX    = *(WORD*)(payload + 9);
    WORD wAttackPosY    = *(WORD*)(payload + 11);
    BYTE bAttackHeight  = payload[13];
    BYTE bDefenseType   = payload[14];
    DWORD dwDefenseID   = *(DWORD*)(payload + 15);
    WORD wTargetPosX    = *(WORD*)(payload + 19);
    WORD wTargetPosY    = *(WORD*)(payload + 21);
    BYTE bTargetHeight  = payload[23];

    LOG("[MugongHandler] OnMugongAttackReq: Skill " + std::to_string(dwMugongID) + " by " + std::to_string(dwAttackID) + " against " + std::to_string(dwDefenseID));

    BYTE bMugongLevel = 1;
    int currentLvl = MugongManager::GetInstance()->GetPlayerMugongLevel(charID, dwMugongID);
    if(currentLvl > 0) bMugongLevel = currentLvl;

    DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);

    if (g_MapInstances.count(playerMapID)) {
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwAttackID);        if (pObj) {
            bool isCC = false;
            for (const auto& bf : pObj->activeBuffs) {
                if (bf.second.bIsDebuff) {
                    DWORD mugID = bf.second.dwMugongID;
                    if (mugID == 94 || mugID == 95 || mugID == 35 || mugID == 65 || mugID == 125) {
                        isCC = true;
                        break;
                    }
                }
            }
            if (isCC) {
                std::vector<BYTE> buf; buf.resize(4, 0);
                DWORD senderObjID = 0;
                buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
                buf.push_back(8); // CT_TIMEMESSAGE
                std::string msg = "[Control] You are frozen, stunned or immobilized and cannot cast skills!";
                WORD len = (WORD)msg.size();
                buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
                buf.insert(buf.end(), msg.begin(), msg.end());
                WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
                WORD payloadSize = (WORD)(buf.size() - 4);
                memcpy(&buf[0], &packetID, 2);
                memcpy(&buf[2], &payloadSize, 2);
                EncryptPacket(buf.data(), 0x42);
                SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
                return;
            }
        }
        if (pObj) {
            bool isCC = false;
            for (const auto& bf : pObj->activeBuffs) {
                if (bf.second.bIsDebuff) {
                    DWORD mugID = bf.second.dwMugongID;
                    if (mugID == 94 || mugID == 95 || mugID == 35 || mugID == 65 || mugID == 125) {
                        isCC = true;
                        break;
                    }
                }
            }
            if (isCC) {
                std::vector<BYTE> buf; buf.resize(4, 0);
                DWORD senderObjID = 0;
                buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
                buf.push_back(8); // CT_TIMEMESSAGE
                std::string msg = "[Control] You are frozen, stunned or immobilized and cannot cast skills!";
                WORD len = (WORD)msg.size();
                buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
                buf.insert(buf.end(), msg.begin(), msg.end());
                WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
                WORD payloadSize = (WORD)(buf.size() - 4);
                memcpy(&buf[0], &packetID, 2);
                memcpy(&buf[2], &payloadSize, 2);
                EncryptPacket(buf.data(), 0x42);
                SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
                return;
            }
        }

        if (pObj && pObj->activeBuffs.count(130) > 0) {
            pObj->activeBuffs[130].dwEndTime = 0; // Mark for instant expiry in MonsterAI loop
            LOG("[MugongHandler] Player " + std::to_string(dwAttackID) + " used skill " + std::to_string(dwMugongID) + ". Expiring Turtle Breath.");
        }
        sMugongList* tempPd = MugongManager::GetInstance()->GetMugongLevelData(dwMugongID, bMugongLevel);
        bool tempIsBuff = (tempPd && tempPd->dwKeepUpTime > 0);
        if (!tempIsBuff && pObj && pObj->activeBuffs.count(178) > 0) {
            pObj->activeBuffs[178].dwEndTime = 0; // Mark for instant expiry in MonsterAI loop
            LOG("[MugongHandler] Player " + std::to_string(dwAttackID) + " used damage skill " + std::to_string(dwMugongID) + ". Expiring Stealth.");
        }
    }

    // 1. Get the skill data
    sMugongList* pMugongData = MugongManager::GetInstance()->GetMugongLevelData(dwMugongID, bMugongLevel);
    
    // 2. Determine if it's a buff or heal
    bool isBuff = (pMugongData && pMugongData->dwKeepUpTime > 0);
    bool isHeal = (!isBuff && pMugongData && (pMugongData->wIncHpCur > 0 || pMugongData->wIncHpCurPerc > 0 || pMugongData->wIncIpCur > 0 || pMugongData->wIncIpCurPerc > 0) && dwMugongID != 37 && dwMugongID != 70);

    // 2.5. Perform early IP cost validation and deduction for all non-healing skills
    if (!isHeal && dwMugongID != 63 && pMugongData && pMugongData->dwCostMp > 0) {
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
            if (pObj) {
                if (pObj->wIpCur < pMugongData->dwCostMp) {
                    LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " blocked: IP " 
                        + std::to_string(pObj->wIpCur) + " < cost " + std::to_string(pMugongData->dwCostMp));
                    // Send 0x4016 with bResult=5 (IP insufficient) so client shows error message
                    std::vector<BYTE> failBuf(4 + 38, 0);
                    BYTE* fp = failBuf.data() + 4;
                    fp[0] = 3; // bResult = 3 (IDS_SHORT_INLIFE / IP insufficient)
                    *(DWORD*)(fp + 1) = dwMugongID;
                    fp[5] = bMugongLevel;
                    fp[6] = 1; // bAtkType = OBJTYPE_PC (client uses this in FindXiahObject)
                    *(DWORD*)(fp + 7) = dwAttackID;
                    *(WORD*)(fp + 11) = wAttackPosX;
                    *(WORD*)(fp + 13) = wAttackPosY;
                    fp[15] = bAttackHeight;
                    fp[16] = 1; // bDefType = OBJTYPE_PC
                    *(DWORD*)(fp + 17) = dwAttackID; // dwDefID = self
                    PACKET_HEADER* fh = (PACKET_HEADER*)failBuf.data();
                    fh->id = 0x4016;
                    fh->payloadSize = 38;
                    EncryptPacket(failBuf.data(), 0x42);
                    SafeSend(clientSocket, (const char*)failBuf.data(), failBuf.size(), 0);
                    return;
                }
                WORD oldIp = pObj->wIpCur;
                pObj->wIpCur -= pMugongData->dwCostMp;

                // Send 0x3B0D to update client HP/IP bars
                std::vector<BYTE> hpBuf(4);
                auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                push4(pObj->dwHpMax);
                push4(pObj->dwHpCur);
                push4(pObj->wIpMax);
                push4(pObj->wIpCur);
                hpBuf.push_back(0); // bType
                PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                hpHead->id = 0x3B0D;
                hpHead->payloadSize = hpBuf.size() - 4;
                EncryptPacket(hpBuf.data(), 0x42);
                SafeSend(clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);

                LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " IP cost deducted early: " 
                    + std::to_string(oldIp) + " - " + std::to_string(pMugongData->dwCostMp) 
                    + " = " + std::to_string(pObj->wIpCur));
            }
        }
    }

    // 3. Class 4: Healing / HP & IP Recovery Skills
    if (isHeal || dwMugongID == 63) {
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID); // è
            if (pObj) {
                // 1. IP Cost deduction first from caster (pObj)
                DWORD cost = pMugongData ? pMugongData->dwCostMp : 0;
                if (pObj->wIpCur < cost) {
                    LOG("[MugongHandler] Heal Skill " + std::to_string(dwMugongID) + " blocked: IP " 
                        + std::to_string(pObj->wIpCur) + " < cost " + std::to_string(cost));
                    return;
                }
                pObj->wIpCur -= cost;

                // 2. è (è·± bKind=0 § bKind=1/2 )
                sServerObject* pTarget = nullptr;
                SOCKET targetSocket = clientSocket;
                
                sMugongTemplate* pTpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);
                BYTE bKind = pTpl ? pTpl->bKind : 0;

                // §è(bKind==12éè¤°è)èè
                if (bKind == 1 || bKind == 2) {
                    if (bDefenseType == 1 && dwDefenseID > 0 && dwDefenseID != dwAttackID) {
                        sServerObject* pPotentialTarget = mapInst->GetPlayer(dwDefenseID);
                        if (pPotentialTarget) {
                            // éè· (èé wAttackRange  > 0)
                            float dx = (float)pObj->wPosX - (float)pPotentialTarget->wPosX;
                            float dy = (float)pObj->wPosY - (float)pPotentialTarget->wPosY;
                            float dist = sqrtf(dx * dx + dy * dy);
                            
                            if (pMugongData && pMugongData->wAttackRange > 0 && dist > (float)pMugongData->wAttackRange) {
                                LOG("[MugongHandler] Heal Skill " + std::to_string(dwMugongID) + " target out of range: " 
                                    + std::to_string((int)dist) + " > " + std::to_string(pMugongData->wAttackRange));
                                return; // èè·
                            }
                            
                            pTarget = pPotentialTarget;
                            targetSocket = SessionMgr::GetInstance().GetSocketByCharID(dwDefenseID - 400000000);
                        }
                    }
                }
                
                // éé§èèèè·±(bKind==0)è¨è·±è
                if (!pTarget) {
                    pTarget = pObj;
                    targetSocket = clientSocket;
                }

                // 3. Calculate Heal Amounts
                DWORD hpHeal = 0;
                if (pMugongData) {
                    hpHeal = pMugongData->wIncHpCur;
                    if (pMugongData->wIncHpCurPerc > 0) {
                        hpHeal += (pTarget->dwHpMax * pMugongData->wIncHpCurPerc / 100);
                    }
                }
                if (dwMugongID == 63 && hpHeal == 0) {
                    hpHeal = 200 + bMugongLevel * 100; // Fallback for level-based heal
                }

                DWORD ipHeal = 0;
                if (pMugongData) {
                    ipHeal = pMugongData->wIncIpCur;
                    if (pMugongData->wIncIpCurPerc > 0) {
                        ipHeal += (pTarget->wIpMax * pMugongData->wIncIpCurPerc / 100);
                    }
                }

                // 4. Apply Heals to target player (Self or Coterie Member)
                pTarget->dwHpCur = (std::min)(pTarget->dwHpMax, pTarget->dwHpCur + hpHeal);
                pTarget->wIpCur  = (std::min)(pTarget->wIpMax, (DWORD)(pTarget->wIpCur + ipHeal));

                // 5. Send 0x3B0D (HP/IP status sync) to the healed player character
                std::vector<BYTE> hpBuf(4);
                auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                push4(pTarget->dwHpMax);
                push4(pTarget->dwHpCur);
                push4(pTarget->wIpMax);
                push4(pTarget->wIpCur);
                hpBuf.push_back(0); // bType = 0
                PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                hpHead->id = 0x3B0D;
                hpHead->payloadSize = hpBuf.size() - 4;
                EncryptPacket(hpBuf.data(), 0x42);
                SafeSend(targetSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);

                // 6. èè·±èèé¤è·± 0x3B0D °èè
                if (pTarget != pObj && cost > 0) {
                    std::vector<BYTE> selfIpBuf(4);
                    selfIpBuf.push_back(pObj->dwHpMax & 0xFF); selfIpBuf.push_back((pObj->dwHpMax >> 8) & 0xFF); selfIpBuf.push_back((pObj->dwHpMax >> 16) & 0xFF); selfIpBuf.push_back(pObj->dwHpMax >> 24);
                    selfIpBuf.push_back(pObj->dwHpCur & 0xFF); selfIpBuf.push_back((pObj->dwHpCur >> 8) & 0xFF); selfIpBuf.push_back((pObj->dwHpCur >> 16) & 0xFF); selfIpBuf.push_back(pObj->dwHpCur >> 24);
                    selfIpBuf.push_back(pObj->wIpMax & 0xFF); selfIpBuf.push_back((pObj->wIpMax >> 8) & 0xFF); selfIpBuf.push_back((pObj->wIpMax >> 16) & 0xFF); selfIpBuf.push_back(pObj->wIpMax >> 24);
                    selfIpBuf.push_back(pObj->wIpCur & 0xFF); selfIpBuf.push_back((pObj->wIpCur >> 8) & 0xFF); selfIpBuf.push_back((pObj->wIpCur >> 16) & 0xFF); selfIpBuf.push_back(pObj->wIpCur >> 24);
                    selfIpBuf.push_back(0);
                    PACKET_HEADER* selfHead = (PACKET_HEADER*)selfIpBuf.data();
                    selfHead->id = 0x3B0D;
                    selfHead->payloadSize = selfIpBuf.size() - 4;
                    EncryptPacket(selfIpBuf.data(), 0x42);
                    SafeSend(clientSocket, (const char*)selfIpBuf.data(), selfIpBuf.size(), 0);
                }

                LOG("[MugongHandler] Heal Skill " + std::to_string(dwMugongID) + " healed Target " + std::to_string(pTarget->dwObjectID) 
                    + " HP: +" + std::to_string(hpHeal) + " (Cur: " + std::to_string(pTarget->dwHpCur) + "/" + std::to_string(pTarget->dwHpMax) + ")");
            }
        }

        // 7. Broadcast AttackAck (0x4016) so everyone sees the visual healing flashing effect
        std::vector<BYTE> ackBuf(4 + 38);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0; // SUCCESS
        *(DWORD*)(p + 1) = dwMugongID;
        p[5] = bMugongLevel;
        p[6] = 1; // bAtkType = OBJTYPE_PC
        *(DWORD*)(p + 7) = dwAttackID;
        *(WORD*)(p + 11) = wAttackPosX;
        *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight;
        
        // ¨èèè§è§è¨°
        CMapInstance* mapInst2 = g_MapInstances.count(playerMapID) ? g_MapInstances[playerMapID] : nullptr;
        if (mapInst2) {
            std::lock_guard<std::mutex> lock(mapInst2->GetMutex());
            sServerObject* pObj = mapInst2->GetPlayer(dwAttackID);
            sServerObject* pTarget = nullptr;
            if (bDefenseType == 1 && dwDefenseID > 0 && dwDefenseID != dwAttackID) {
                pTarget = mapInst2->GetPlayer(dwDefenseID);
            }
            if (!pTarget) pTarget = pObj;

            if (pTarget) {
                p[16] = pTarget->bObjectType; // ¨± (é 1=PC)
                *(DWORD*)(p + 17) = pTarget->dwObjectID;
                *(DWORD*)(p + 21) = pTarget->dwHpMax;
                *(DWORD*)(p + 25) = pTarget->dwHpCur;
            } else {
                p[16] = 1;
                *(DWORD*)(p + 17) = dwAttackID;
                *(DWORD*)(p + 21) = 0;
                *(DWORD*)(p + 25) = 0;
            }
        } else {
            p[16] = 1;
            *(DWORD*)(p + 17) = dwAttackID;
            *(DWORD*)(p + 21) = 0;
            *(DWORD*)(p + 25) = 0;
        }
        
        *(DWORD*)(p + 29) = 0; // finalDmg = 0
        *(DWORD*)(p + 33) = 0; // deadExp = 0
        p[37] = 0; // bCritHit = 0

        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4016; // CS_BT_MUGONGATTACK_ACK
        head->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return; // Early return for healing skills
    }

    // 1.5. Distance validation (only for targeted skills with wDistance > 0)
    if (pMugongData && pMugongData->wAttackRange > 0 && dwDefenseID != 0) {
        float dx = (float)wAttackPosX - (float)wTargetPosX;
        float dy = (float)wAttackPosY - (float)wTargetPosY;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > (float)pMugongData->wAttackRange) {
            LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " blocked: distance " 
                + std::to_string((int)dist) + " > range " + std::to_string(pMugongData->wAttackRange));
            return;
        }
    }
    
    // 3. If it's a buff skill (e.g. 64) or a special dragon skill (IDs 164-171), broadcast KeepUpMugongStartAck (0x402C)
    bool isDragonSkill = (dwMugongID >= 164 && dwMugongID <= 171);
    if (isBuff || dwMugongID == 64 || isDragonSkill) {
        bool isDuplicate = false;
        
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
            if (pObj) {
                // If buff already exists, reset the timer and let it broadcast 0x402C to refresh UI
                if (pObj->activeBuffs.count(dwMugongID) > 0) {
                    pObj->activeBuffs[dwMugongID].dwEndTime = GetTickCount() + (isDragonSkill ? 3000 : (pMugongData ? pMugongData->dwKeepUpTime * 1000 : 300000));
                    isDuplicate = true; // Avoid sending CS_BT_KEEPUPMUGONGSTART_ACK again
                    LOG("[MugongHandler] Buff/Dragon " + std::to_string(dwMugongID) + " already exists. Reset internal timer. UI/Visuals will be refreshed.");
                } else {
                    // Add new buff
                    sServerObject::sActiveBuff newBuff;
                    newBuff.dwMugongID = dwMugongID;
                    newBuff.bLevel = bMugongLevel;
                    newBuff.dwEndTime = GetTickCount() + (isDragonSkill ? 3000 : (pMugongData ? pMugongData->dwKeepUpTime * 1000 : 300000));
                    newBuff.bIsDebuff = false;
                    pObj->activeBuffs[dwMugongID] = newBuff;
                    LOG("[MugongHandler] Applied BUFF/Dragon " + std::to_string(dwMugongID) + " to player " + std::to_string(dwAttackID));
                }
            } else {
                LOG("[MugongHandler] WARNING: mapInst->GetPlayer() IS NULL! dwAttackID=" + std::to_string(dwAttackID));
            }
        }

        // Allow recasting so client gets 0x402C and visual effects play every time

        std::vector<BYTE> buffAck(4 + 11);
        BYTE* bp = buffAck.data() + 4;
        bp[0] = 0; // bResult
        *(DWORD*)(bp + 1) = dwAttackID;
        bp[5] = 1; // bObjectType (Player)
        *(DWORD*)(bp + 6) = dwMugongID;
        bp[10] = bMugongLevel;
        
        PACKET_HEADER* headB = (PACKET_HEADER*)buffAck.data();
        headB->id = 0x402C;
        headB->payloadSize = 11;
        EncryptPacket(buffAck.data(), 0x42);
        BroadcastPacketToMap(playerMapID, buffAck);
        
        LOG("[MugongHandler] Applied BUFF " + std::to_string(dwMugongID) + " to player " + std::to_string(dwAttackID));

        // Immediately recalculate stats so buff bonuses take effect (do NOT send 0x4414 to prevent clearing client visuals)
        DWORD buffCharID = dwAttackID - 400000000;
        PlayerManager::GetInstance().RecalculateStats(buffCharID, false);
    }

    // IP cost was already verified and deducted early at step 2.5

    // 4. Broadcast AttackAck (0x4016)
    // IMPORTANT: We must ALWAYS send 0x4016, even for buff skills.
    // The client's attack state machine requires this ACK to complete the sequence.
    // Without it, the client gets stuck and all subsequent skills fail to render effects.
    // For skills with no target (dwDefenseID==0, including self-buffs), set defender to self
    // with OBJTYPE_PC(1) to prevent client NULL dereference in OnCS_BT_MUGONGATTACK_ACK.
    bool bNoRealTarget = false;
    if (dwDefenseID == 0) {
        dwDefenseID = dwAttackID;
        bDefenseType = 1; // OBJTYPE_PC
        bNoRealTarget = true;
    }
    std::vector<BYTE> ackBuf(4 + 38);
    BYTE* p = ackBuf.data() + 4;
    p[0] = 0; // default bResult
    *(DWORD*)(p + 1) = dwMugongID;
    p[5] = bMugongLevel;
    p[6] = 1; // bAtkType = OBJTYPE_PC (client uses this in FindXiahObject)
    *(DWORD*)(p + 7) = dwAttackID;
    *(WORD*)(p + 11) = wAttackPosX;
    *(WORD*)(p + 13) = wAttackPosY;
    p[15] = bAttackHeight;
    p[16] = bDefenseType;
    *(DWORD*)(p + 17) = dwDefenseID;
    
    DWORD dwDefHpMax = 60000;
    DWORD dwDefHpCur = 60000;
    WORD  dwDefIpMax = 0;
    WORD  dwDefIpCur = 0;
    DWORD finalDmg = 0;
    DWORD deadExp = 0;
    bool isDead = false;
    BYTE bCritHit = 0;

    // Struct to store dead monsters to process EXP/animations outside the map lock safely
    struct sDeadEntity {
        DWORD dwObjectID;
        DWORD dwExp;
        std::string szName;
    };
    std::vector<sDeadEntity> deadEntities;

    if (!isBuff && dwMugongID != 64 && bDefenseType == 3) {
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            MonsterData* pTarget = mapInst->GetMonster(dwDefenseID);
            if (pTarget) {
                bool sIsReturning = pTarget->bIsReturning;
                // Use template defense since MonsterData doesn't store wWepDef
                DWORD monsterDef = g_NpcTemplates.count(pTarget->bPropType) ? g_NpcTemplates[pTarget->bPropType].dwDefInit : 0;
                
                // ¨¨°é Debuff ±§
                for (auto& bf : pTarget->activeBuffs) {
                    sMugongList* bd = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                    if (bd) {
                        if (bd->wIncDefPerc > 0) {
                            monsterDef = monsterDef * bd->wIncDefPerc / 100;
                        }
                        if (bd->wIncDef > 0) {
                            if (monsterDef > bd->wIncDef) monsterDef -= bd->wIncDef; else monsterDef = 0;
                        }
                    }
                }

                WORD monsterAvoid = pTarget->wAvoidRatio;

                PlayerData* pAttacker = mapInst->GetPlayer(dwAttackID);
                
                // Sacrifice Cost Logic (Class 5 skills)
                DWORD hpSacrificed = 0;
                bool isSacrifice = (dwMugongID == 37 || (pMugongData && pMugongData->wIncHpCurPerc > 0 && pMugongData->wIncHpCurPerc < 100));
                if (isSacrifice && pAttacker) {
                    BYTE pct = pMugongData ? pMugongData->wIncHpCurPerc : 20;
                    hpSacrificed = pAttacker->dwHpCur * pct / 100;
                    if (hpSacrificed > 0) {
                        pAttacker->dwHpCur = (std::max)(1UL, pAttacker->dwHpCur - hpSacrificed);
                        LOG("[MugongHandler] Sacrifice Skill " + std::to_string(dwMugongID) + " consumed " 
                            + std::to_string(hpSacrificed) + " HP (New HP: " + std::to_string(pAttacker->dwHpCur) + ")");
                        
                        // Send 0x3B0D to update client HP/IP bars
                        std::vector<BYTE> hpBuf(4);
                        auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                        auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                        push4(pAttacker->dwHpMax);
                        push4(pAttacker->dwHpCur);
                        push4(pAttacker->wIpMax);
                        push4(pAttacker->wIpCur);
                        hpBuf.push_back(0); // bType
                        PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                        hpHead->id = 0x3B0D;
                        hpHead->payloadSize = hpBuf.size() - 4;
                        EncryptPacket(hpBuf.data(), 0x42);
                        SafeSend(clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);
                    }
                }

                // Dodge Logic
                DWORD playerAtkRating = 50; 
                if (pAttacker) {
                    playerAtkRating += pAttacker->dwTotalHit;
                }
                
                float hitChance = (float)playerAtkRating / (float)(playerAtkRating + monsterAvoid);
                float dodgeRoll = (float)(rand() % 10000) / 10000.0f;
                
                if (sIsReturning || (monsterAvoid > 0 && dodgeRoll > hitChance)) {
                    p[0] = 1; // 1 = MISS
                    finalDmg = 0;
                } else {
                    p[0] = 2; // 2 = HIT
                    
                    DWORD playerAtk = 50; // Default fallback
                    if (pAttacker) {
                        playerAtk = pAttacker->dwTotalAtk;
                    }

                    DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0; // Loaded from wIncAtk
                    DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0; 

                    // final = (playerAtk * ((100 + skillPerc) / 100.0f)) + skillFlatDmg
                    float rawDmg = (playerAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg;
                    
                    // Add sacrifice HP bonus
                    if (hpSacrificed > 0) {
                        rawDmg += hpSacrificed * 2.0f; // 2x HP sacrificed added as flat bonus damage!
                    }

                    // Random float 90% ~ 110%
                    float roll = 0.9f + ((float)(rand() % 2000) / 10000.0f); // 0.9 + (0 ~ 0.2) = 0.9 ~ 1.1
                    rawDmg *= roll;

                    // Critical Hit (uses player wCritical stat, fallback 5%)
                    WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                    if ((WORD)(rand() % 100) < critRate) {
                        rawDmg *= 1.5f;
                        bCritHit = 1; // Critical hit!
                    }

                    if (rawDmg > monsterDef) rawDmg -= monsterDef;
                    else rawDmg = 1; // Minimum 1 damage
                    
                    finalDmg = (DWORD)rawDmg;
                    
                    if (pTarget->dwHpCur > finalDmg) {
                        pTarget->dwHpCur -= finalDmg;
                    } else {
                        finalDmg = pTarget->dwHpCur;
                        pTarget->dwHpCur = 0;
                        isDead = true;
                        pTarget->dwDeadTime = GetTickCount();
                        pTarget->dwTargetID = 0;
                        deadExp = pTarget->dwExp; // Use computed Init+Inc value
                        
                        deadEntities.push_back({dwDefenseID, deadExp, g_NpcTemplates[pTarget->bPropType].szName});
                        DropManager::GetInstance()->GenerateDrops(dwAttackID, *pTarget);
                        LOG("[MugongHandler] Monster " + g_NpcTemplates[pTarget->bPropType].szName + " died from skill " + std::to_string(dwMugongID) + "!");
                    }
                }
                
                // Debuff  (è¤èéé)
                if (pMugongData && pMugongData->dwKeepUpTime > 0 && p[0] == 2) {
                    PlayerData::sActiveBuff debuff;
                    debuff.dwMugongID = dwMugongID;
                    debuff.bLevel = bMugongLevel;
                    debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;
                    debuff.bIsDebuff = true;
                    pTarget->activeBuffs[dwMugongID] = debuff;
                    LOG("[MugongHandler] Applied debuff skill " + std::to_string(dwMugongID) + " to monster " + std::to_string(dwDefenseID));
                }
                
                // AOE / Splash Damage Logic (Class 6 skills) - ° Debuff 
                sMugongTemplate* sTpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);
                if (IsAoeSkill(dwMugongID, sTpl)) {
                    // é¨èèèé·è¤èé
                    WORD wAoeCenterX = pAttacker ? pAttacker->wPosX : wAttackPosX;
                    WORD wAoeCenterY = pAttacker ? pAttacker->wPosY : wAttackPosY;
                    float splashRange = (pMugongData && pMugongData->wAttackRange > 0) ? (float)pMugongData->wAttackRange : 20.0f;
                    
                    LOG("[MugongHandler] [AOE-Monster] Skill ID: " + std::to_string(dwMugongID) + " Attacker OID: " + std::to_string(dwAttackID) + " Pos: (" + std::to_string(wAoeCenterX) + "," + std::to_string(wAoeCenterY) + ")");

                    // 1. °
                    std::vector<MonsterData*> aoiMonsters = mapInst->GetMonstersInAOI(wAoeCenterX, wAoeCenterY);
                    std::vector<MonsterData*> splashMonsters;
                    
                    LOG("[MugongHandler] [AOE-Monster] Scanned " + std::to_string(aoiMonsters.size()) + " potential monsters in AOI");
                    for (MonsterData* pMon : aoiMonsters) {
                        if (!pMon || pMon->dwObjectID == dwDefenseID || pMon->dwHpCur == 0 || pMon->bIsReturning) continue;
                        float dx = (float)pMon->wPosX - (float)wAoeCenterX;
                        float dy = (float)pMon->wPosY - (float)wAoeCenterY;
                        float dist = sqrtf(dx * dx + dy * dy);
                        
                        LOG("[MugongHandler] [AOE-Monster] -> Mon OID: " + std::to_string(pMon->dwObjectID) + " Pos: (" + std::to_string(pMon->wPosX) + "," + std::to_string(pMon->wPosY) + ") Dist: " + std::to_string(dist));
                        if (dist <= splashRange) {
                            splashMonsters.push_back(pMon);
                            if (splashMonsters.size() >= 5) break; // ¤°5
                        }
                    }
                    LOG("[MugongHandler] [AOE-Monster] -> Splash monsters count selected: " + std::to_string(splashMonsters.size()));

                    for (MonsterData* pSplashMon : splashMonsters) {
                        DWORD sMonDef = g_NpcTemplates.count(pSplashMon->bPropType) ? g_NpcTemplates[pSplashMon->bPropType].dwDefInit : 0;
                        
                        // ¨¨èè¤ Debuff 
                        for (auto& bf : pSplashMon->activeBuffs) {
                            sMugongList* bd = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                            if (bd) {
                                if (bd->wIncDefPerc > 0) sMonDef = sMonDef * bd->wIncDefPerc / 100;
                                if (bd->wIncDef > 0) {
                                    if (sMonDef > bd->wIncDef) sMonDef -= bd->wIncDef; else sMonDef = 0;
                                }
                            }
                        }

                        WORD sMonAvoid = pSplashMon->wAvoidRatio;
                        DWORD sPlayerAtkRating = 50 + (pAttacker ? pAttacker->dwTotalHit : 0);
                        float sHitChance = (float)sPlayerAtkRating / (float)(sPlayerAtkRating + sMonAvoid);
                        float sRoll = (float)(rand() % 10000) / 10000.0f;
                        
                        BYTE sCritHit = 0;
                        DWORD sFinalDmg = 0;
                        BYTE sResult = 2; // HIT
                        
                        if (sMonAvoid > 0 && sRoll > sHitChance) {
                            sResult = 1; // MISS
                            sFinalDmg = 0;
                        } else {
                            DWORD pAtk = pAttacker ? pAttacker->dwTotalAtk : 50;
                            DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0;
                            DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0;
                            
                            // ¤¤°¤°éè 70%
                            float rawDmg = ((pAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg) * 0.7f;
                            float var = 0.9f + ((float)(rand() % 2000) / 10000.0f);
                            rawDmg *= var;
                            
                            WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                            if ((WORD)(rand() % 100) < critRate) {
                                rawDmg *= 1.5f;
                                sCritHit = 1;
                            }
                            
                            if (rawDmg > sMonDef) rawDmg -= sMonDef;
                            else rawDmg = 1;
                            
                            sFinalDmg = (DWORD)rawDmg;
                            
                            if (pSplashMon->dwHpCur > sFinalDmg) {
                                pSplashMon->dwHpCur -= sFinalDmg;
                            } else {
                                sFinalDmg = pSplashMon->dwHpCur;
                                pSplashMon->dwHpCur = 0;
                                pSplashMon->dwDeadTime = GetTickCount();
                                pSplashMon->dwTargetID = 0;
                                
                                deadEntities.push_back({pSplashMon->dwObjectID, pSplashMon->dwExp, g_NpcTemplates[pSplashMon->bPropType].szName});
                                DropManager::GetInstance()->GenerateDrops(dwAttackID, *pSplashMon);
                                LOG("[MugongHandler] Splash target " + g_NpcTemplates[pSplashMon->bPropType].szName + " died from skill " + std::to_string(dwMugongID) + "!");
                            }
                        }

                        // Debuff ±è¨è°°
                        if (pMugongData && pMugongData->dwKeepUpTime > 0 && sResult == 2) {
                            PlayerData::sActiveBuff debuff;
                            debuff.dwMugongID = dwMugongID;
                            debuff.bLevel = bMugongLevel;
                            debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;
                            debuff.bIsDebuff = true;
                            pSplashMon->activeBuffs[dwMugongID] = debuff;
                            LOG("[MugongHandler] Applied splash debuff skill " + std::to_string(dwMugongID) + " to monster " + std::to_string(pSplashMon->dwObjectID));
                        }
                        
                        if (pSplashMon->dwAttackPattern != 0 && pSplashMon->dwHpCur > 0) {
                            pSplashMon->dwTargetID = dwAttackID;
                        }
                        
                        // è° 0x4016 ¤¨
                        std::vector<BYTE> splashAckBuf(4 + 38);
                        BYTE* sp = splashAckBuf.data() + 4;
                        sp[0] = sResult; 
                        *(DWORD*)(sp + 1) = dwMugongID;
                        sp[5] = bMugongLevel;
                        sp[6] = 1; 
                        *(DWORD*)(sp + 7) = dwAttackID;
                        *(WORD*)(sp + 11) = wAttackPosX;
                        *(WORD*)(sp + 13) = wAttackPosY;
                        sp[15] = bAttackHeight;
                        sp[16] = 3; 
                        *(DWORD*)(sp + 17) = pSplashMon->dwObjectID;
                        *(DWORD*)(sp + 21) = pSplashMon->dwHpMax;
                        *(DWORD*)(sp + 25) = pSplashMon->dwHpCur;
                        *(DWORD*)(sp + 29) = sFinalDmg;
                        *(DWORD*)(sp + 33) = 0; 
                        sp[37] = sCritHit;
                        
                        PACKET_HEADER* sHead = (PACKET_HEADER*)splashAckBuf.data();
                        sHead->id = 0x4016;
                        sHead->payloadSize = 38;
                        EncryptPacket(splashAckBuf.data(), 0x42);
                        BroadcastPacketToMap(playerMapID, splashAckBuf);
                    }

                    // 2. ° PC  (PVP)
                    std::vector<PlayerData*> aoiPlayers = mapInst->GetPlayersInAOI(wAoeCenterX, wAoeCenterY);
                    std::vector<PlayerData*> splashPlayers;
                    
                    LOG("[MugongHandler] [AOE-Monster] Scanned " + std::to_string(aoiPlayers.size()) + " potential players in AOI");
                    for (PlayerData* pPl : aoiPlayers) {
                        if (!pPl || pPl->dwObjectID == dwAttackID || pPl->dwObjectID == dwDefenseID || pPl->dwHpCur == 0 || pPl->dwInvulnerableUntil > GetTickCount()) continue;
                        float dx = (float)pPl->wPosX - (float)wAoeCenterX;
                        float dy = (float)pPl->wPosY - (float)wAoeCenterY;
                        float dist = sqrtf(dx * dx + dy * dy);
                        
                        LOG("[MugongHandler] [AOE-Monster] -> Player OID: " + std::to_string(pPl->dwObjectID) + " Name: " + pPl->szName + " Pos: (" + std::to_string(pPl->wPosX) + "," + std::to_string(pPl->wPosY) + ") Dist: " + std::to_string(dist));
                        if (dist <= splashRange) {
                            splashPlayers.push_back(pPl);
                            if (splashPlayers.size() >= 5) break; // ¤°5
                        }
                    }
                    LOG("[MugongHandler] [AOE-Monster] -> Splash players count selected: " + std::to_string(splashPlayers.size()));

                    for (PlayerData* pSplashPlayer : splashPlayers) {
                        DWORD sPlDef = pSplashPlayer->dwTotalDef;
                        WORD sPlDodge = pSplashPlayer->dwTotalDodge;
                        
                        DWORD sPlayerAtkRating = 50 + (pAttacker ? pAttacker->dwTotalHit : 0);
                        float sHitChance = (float)sPlayerAtkRating / (float)(sPlayerAtkRating + sPlDodge);
                        float sRoll = (float)(rand() % 10000) / 10000.0f;
                        
                        BYTE sCritHit = 0;
                        DWORD sFinalDmg = 0;
                        BYTE sResult = 2; // HIT
                        
                        if (sPlDodge > 0 && sRoll > sHitChance) {
                            sResult = 1; // MISS
                            sFinalDmg = 0;
                        } else {
                            DWORD pAtk = pAttacker ? pAttacker->dwTotalAtk : 50;
                            DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0;
                            DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0;
                            
                            float rawDmg = ((pAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg) * 0.7f;
                            float var = 0.9f + ((float)(rand() % 2000) / 10000.0f);
                            rawDmg *= var;
                            
                            WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                            if ((WORD)(rand() % 100) < critRate) {
                                rawDmg *= 1.5f;
                                sCritHit = 1;
                            }
                            
                            if (rawDmg > sPlDef) rawDmg -= sPlDef;
                            else rawDmg = 1;
                            
                            sFinalDmg = (DWORD)rawDmg;
                            
                            if (pSplashPlayer->dwHpCur > sFinalDmg) {
                                pSplashPlayer->dwHpCur -= sFinalDmg;
                            } else {
                                sFinalDmg = pSplashPlayer->dwHpCur;
                                pSplashPlayer->dwHpCur = 0;
                                pSplashPlayer->dwDeadTime = GetTickCount();
                                LOG("[MugongHandler] PvP Splash target player " + pSplashPlayer->szName + " died from skill " + std::to_string(dwMugongID) + "!");
                            }
                        }

                        // Debuff ±è¨è°° PC 
                        if (pMugongData && pMugongData->dwKeepUpTime > 0 && sResult == 2) {
                            sServerObject::sActiveBuff debuff;
                            debuff.dwMugongID = dwMugongID;
                            debuff.bLevel = bMugongLevel;
                            debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;
                            debuff.bIsDebuff = true;
                            pSplashPlayer->activeBuffs[dwMugongID] = debuff;
                            LOG("[MugongHandler] Applied PvP splash debuff skill " + std::to_string(dwMugongID) + " to player " + std::to_string(pSplashPlayer->dwObjectID));
                        }
                        
                        // è° 0x4016 ¤¨
                        std::vector<BYTE> splashAckBuf(4 + 38);
                        BYTE* sp = splashAckBuf.data() + 4;
                        sp[0] = sResult; 
                        *(DWORD*)(sp + 1) = dwMugongID;
                        sp[5] = bMugongLevel;
                        sp[6] = 1; 
                        *(DWORD*)(sp + 7) = dwAttackID;
                        *(WORD*)(sp + 11) = wAttackPosX;
                        *(WORD*)(sp + 13) = wAttackPosY;
                        sp[15] = bAttackHeight;
                        sp[16] = 1; // bDefenseType = 1 (Player)
                        *(DWORD*)(sp + 17) = pSplashPlayer->dwObjectID;
                        *(DWORD*)(sp + 21) = pSplashPlayer->dwHpMax;
                        *(DWORD*)(sp + 25) = pSplashPlayer->dwHpCur;
                        *(DWORD*)(sp + 29) = sFinalDmg;
                        *(DWORD*)(sp + 33) = 0; 
                        sp[37] = sCritHit;
                        
                        PACKET_HEADER* sHead = (PACKET_HEADER*)splashAckBuf.data();
                        sHead->id = 0x4016;
                        sHead->payloadSize = 38;
                        EncryptPacket(splashAckBuf.data(), 0x42);
                        BroadcastPacketToMap(playerMapID, splashAckBuf);

                        // è°· 0x3B0D èé±§ ()
                        DWORD splashCharID = pSplashPlayer->dwObjectID - 400000000;
                        SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(splashCharID);
                        if (targetSock != INVALID_SOCKET) {
                            std::vector<BYTE> hpBuf(4);
                            auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                            auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                            push4(pSplashPlayer->dwHpMax);
                            push4(pSplashPlayer->dwHpCur);
                            push4(pSplashPlayer->wIpMax);
                            push4(pSplashPlayer->wIpCur);
                            hpBuf.push_back(0); 
                            PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                            hpHead->id = 0x3B0D;
                            hpHead->payloadSize = hpBuf.size() - 4;
                            EncryptPacket(hpBuf.data(), 0x42);
                            SafeSend(targetSock, (const char*)hpBuf.data(), hpBuf.size(), 0);

                            PlayerManager::GetInstance().RecalculateStats(splashCharID, true);
                        }
                    }
                }
                
                if (pTarget->dwAttackPattern != 0 && pTarget->dwHpCur > 0 && !pTarget->bIsReturning) {
                    pTarget->dwTargetID = dwAttackID;
                }
                dwDefHpMax = pTarget->dwHpMax;
                dwDefHpCur = pTarget->dwHpCur;
            }
        }
    }
    else if (!isBuff && dwMugongID != 64 && bDefenseType == 1 && !bNoRealTarget) {
        // PvP (Player vs Player) ¤¤
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pTarget = mapInst->GetPlayer(dwDefenseID);
            if (pTarget) {
                PlayerData* pAttacker = mapInst->GetPlayer(dwAttackID);

                // 防止群攻自杀：若目标是自己，跳过主目标伤害，仅执行 AOE 溅射
                if (dwDefenseID == dwAttackID) goto pvp_aoe_only;
                
                // §±èè
                DWORD hpSacrificed = 0;
                bool isSacrifice = (dwMugongID == 37 || (pMugongData && pMugongData->wIncHpCurPerc > 0 && pMugongData->wIncHpCurPerc < 100));
                if (isSacrifice && pAttacker) {
                    BYTE pct = pMugongData ? pMugongData->wIncHpCurPerc : 20;
                    hpSacrificed = pAttacker->dwHpCur * pct / 100;
                    if (hpSacrificed > 0) {
                        pAttacker->dwHpCur = (std::max)(1UL, pAttacker->dwHpCur - hpSacrificed);
                        LOG("[MugongHandler] PvP Sacrifice Skill " + std::to_string(dwMugongID) + " consumed " 
                            + std::to_string(hpSacrificed) + " HP");
                        
                        // ·°èèè
                        std::vector<BYTE> hpBuf(4);
                        auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                        auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                        push4(pAttacker->dwHpMax);
                        push4(pAttacker->dwHpCur);
                        push4(pAttacker->wIpMax);
                        push4(pAttacker->wIpCur);
                        hpBuf.push_back(0); 
                        PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                        hpHead->id = 0x3B0D;
                        hpHead->payloadSize = hpBuf.size() - 4;
                        EncryptPacket(hpBuf.data(), 0x42);
                        SafeSend(clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);
                    }
                }

                // PvP ¤ééèè
                DWORD playerAtkRating = 50; 
                if (pAttacker) {
                    playerAtkRating += pAttacker->dwTotalHit;
                }
                DWORD targetDodge = pTarget->dwTotalDodge;
                
                float hitChance = (float)playerAtkRating / (float)(playerAtkRating + targetDodge);
                float dodgeRoll = (float)(rand() % 10000) / 10000.0f;
                
                if (targetDodge > 0 && dodgeRoll > hitChance) {
                    p[0] = 1; // MISS
                    finalDmg = 0;
                } else if (pTarget->activeBuffs.count(130) > 0) {
                    // 130 é¤§ PVP é¤
                    p[0] = 1; // MISS (¨)
                    finalDmg = 0;
                    LOG("[MugongPvp] Player " + pTarget->szName + " protected by Turtle Breath. Zero Damage.");
                } else {
                    // ééè 178 é
                    if (pTarget->activeBuffs.count(178) > 0) {
                        pTarget->activeBuffs[178].dwEndTime = 0; // ¤é
                        LOG("[MugongPvp] Player " + pTarget->szName + " hit while stealth. Expiring Stealth.");
                    }
                    p[0] = 2; // HIT
                    
                    DWORD playerAtk = 50;
                    if (pAttacker) {
                        playerAtk = pAttacker->dwTotalAtk;
                    }

                    DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0;
                    DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0; 

                    float rawDmg = (playerAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg;
                    if (hpSacrificed > 0) {
                        rawDmg += hpSacrificed * 2.0f; // °
                    }

                    float roll = 0.9f + ((float)(rand() % 2000) / 10000.0f);
                    rawDmg *= roll;

                    WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                    if ((WORD)(rand() % 100) < critRate) {
                        rawDmg *= 1.5f;
                        bCritHit = 1; // 
                    }

                    // é¤éé
                    DWORD targetDef = pTarget->dwTotalDef;
                    if (rawDmg > targetDef) rawDmg -= targetDef;
                    else rawDmg = 1;
                    
                    finalDmg = (DWORD)rawDmg;
                    
                    if (pTarget->dwHpCur > finalDmg) {
                        pTarget->dwHpCur -= finalDmg;
                    } else {
                        finalDmg = pTarget->dwHpCur;
                        pTarget->dwHpCur = 0;
                        isDead = true;
                        pTarget->dwDeadTime = GetTickCount();
                        LOG("[MugongHandler] PvP Player " + pTarget->szName + " died from skill " + std::to_string(dwMugongID));
                    }
                }
                
                // Debuff  (è¤èéé)
                if (pMugongData && pMugongData->dwKeepUpTime > 0 && p[0] == 2) {
                    sServerObject::sActiveBuff debuff;
                    debuff.dwMugongID = dwMugongID;
                    debuff.bLevel = bMugongLevel;
                    debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;
                    debuff.bIsDebuff = true;
                    pTarget->activeBuffs[dwMugongID] = debuff;
                    LOG("[MugongHandler] PvP Debuff Applied skill " + std::to_string(dwMugongID) + " to target player " + std::to_string(dwDefenseID));
                }

                pvp_aoe_only:
                // AOE / Splash Damage Logic (Class 6 skills) - PVP Target AOE (° Debuff )
                sMugongTemplate* sTpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);
                if (IsAoeSkill(dwMugongID, sTpl)) {
                    // é¨èèèé·è¤PC/èé
                    WORD wAoeCenterX = pAttacker ? pAttacker->wPosX : wAttackPosX;
                    WORD wAoeCenterY = pAttacker ? pAttacker->wPosY : wAttackPosY;
                    float splashRange = (pMugongData && pMugongData->wAttackRange > 0) ? (float)pMugongData->wAttackRange : 20.0f;
                    
                    LOG("[MugongHandler] [AOE-PvP] Skill ID: " + std::to_string(dwMugongID) + " Attacker OID: " + std::to_string(dwAttackID) + " Pos: (" + std::to_string(wAoeCenterX) + "," + std::to_string(wAoeCenterY) + ")");

                    // 1. °
                    std::vector<MonsterData*> aoiMonsters = mapInst->GetMonstersInAOI(wAoeCenterX, wAoeCenterY);
                    std::vector<MonsterData*> splashMonsters;
                    
                    LOG("[MugongHandler] [AOE-PvP] Scanned " + std::to_string(aoiMonsters.size()) + " potential monsters in AOI");
                    for (MonsterData* pMon : aoiMonsters) {
                        if (!pMon || pMon->dwObjectID == dwDefenseID || pMon->dwHpCur == 0 || pMon->bIsReturning) continue;
                        float dx = (float)pMon->wPosX - (float)wAoeCenterX;
                        float dy = (float)pMon->wPosY - (float)wAoeCenterY;
                        float dist = sqrtf(dx * dx + dy * dy);
                        
                        LOG("[MugongHandler] [AOE-PvP] -> Mon OID: " + std::to_string(pMon->dwObjectID) + " Pos: (" + std::to_string(pMon->wPosX) + "," + std::to_string(pMon->wPosY) + ") Dist: " + std::to_string(dist));
                        if (dist <= splashRange) {
                            splashMonsters.push_back(pMon);
                            if (splashMonsters.size() >= 5) break;
                        }
                    }
                    LOG("[MugongHandler] [AOE-PvP] -> Splash monsters count selected: " + std::to_string(splashMonsters.size()));

                    for (MonsterData* pSplashMon : splashMonsters) {
                        DWORD sMonDef = g_NpcTemplates.count(pSplashMon->bPropType) ? g_NpcTemplates[pSplashMon->bPropType].dwDefInit : 0;
                        
                        // ¨¨ Debuff é
                        for (auto& bf : pSplashMon->activeBuffs) {
                            sMugongList* bd = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                            if (bd) {
                                if (bd->wIncDefPerc > 0) sMonDef = sMonDef * bd->wIncDefPerc / 100;
                                if (bd->wIncDef > 0) {
                                    if (sMonDef > bd->wIncDef) sMonDef -= bd->wIncDef; else sMonDef = 0;
                                }
                            }
                        }

                        WORD sMonAvoid = pSplashMon->wAvoidRatio;
                        DWORD sPlayerAtkRating = 50 + (pAttacker ? pAttacker->dwTotalHit : 0);
                        float sHitChance = (float)sPlayerAtkRating / (float)(sPlayerAtkRating + sMonAvoid);
                        float sRoll = (float)(rand() % 10000) / 10000.0f;
                        
                        BYTE sCritHit = 0;
                        DWORD sFinalDmg = 0;
                        BYTE sResult = 2; // HIT
                        
                        if (sMonAvoid > 0 && sRoll > sHitChance) {
                            sResult = 1; // MISS
                            sFinalDmg = 0;
                        } else {
                            DWORD pAtk = pAttacker ? pAttacker->dwTotalAtk : 50;
                            DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0;
                            DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0;
                            
                            float rawDmg = ((pAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg) * 0.7f;
                            float var = 0.9f + ((float)(rand() % 2000) / 10000.0f);
                            rawDmg *= var;
                            
                            WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                            if ((WORD)(rand() % 100) < critRate) {
                                rawDmg *= 1.5f;
                                sCritHit = 1;
                            }
                            
                            if (rawDmg > sMonDef) rawDmg -= sMonDef;
                            else rawDmg = 1;
                            
                            sFinalDmg = (DWORD)rawDmg;
                            
                            if (pSplashMon->dwHpCur > sFinalDmg) {
                                pSplashMon->dwHpCur -= sFinalDmg;
                            } else {
                                sFinalDmg = pSplashMon->dwHpCur;
                                pSplashMon->dwHpCur = 0;
                                pSplashMon->dwDeadTime = GetTickCount();
                                pSplashMon->dwTargetID = 0;
                                
                                deadEntities.push_back({pSplashMon->dwObjectID, pSplashMon->dwExp, g_NpcTemplates[pSplashMon->bPropType].szName});
                                DropManager::GetInstance()->GenerateDrops(dwAttackID, *pSplashMon);
                                LOG("[MugongHandler] Splash target " + g_NpcTemplates[pSplashMon->bPropType].szName + " died from skill " + std::to_string(dwMugongID) + "!");
                            }
                        }

                        // Debuff ±è¨èè°
                        if (pMugongData && pMugongData->dwKeepUpTime > 0 && sResult == 2) {
                            PlayerData::sActiveBuff debuff;
                            debuff.dwMugongID = dwMugongID;
                            debuff.bLevel = bMugongLevel;
                            debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;
                            debuff.bIsDebuff = true;
                            pSplashMon->activeBuffs[dwMugongID] = debuff;
                            LOG("[MugongHandler] Applied splash debuff skill " + std::to_string(dwMugongID) + " to monster " + std::to_string(pSplashMon->dwObjectID));
                        }
                        
                        if (pSplashMon->dwAttackPattern != 0 && pSplashMon->dwHpCur > 0) {
                            pSplashMon->dwTargetID = dwAttackID;
                        }
                        
                        //  0x4016 ¤¨
                        std::vector<BYTE> splashAckBuf(4 + 38);
                        BYTE* sp = splashAckBuf.data() + 4;
                        sp[0] = sResult; 
                        *(DWORD*)(sp + 1) = dwMugongID;
                        sp[5] = bMugongLevel;
                        sp[6] = 1; 
                        *(DWORD*)(sp + 7) = dwAttackID;
                        *(WORD*)(sp + 11) = wAttackPosX;
                        *(WORD*)(sp + 13) = wAttackPosY;
                        sp[15] = bAttackHeight;
                        sp[16] = 3; 
                        *(DWORD*)(sp + 17) = pSplashMon->dwObjectID;
                        *(DWORD*)(sp + 21) = pSplashMon->dwHpMax;
                        *(DWORD*)(sp + 25) = pSplashMon->dwHpCur;
                        *(DWORD*)(sp + 29) = sFinalDmg;
                        *(DWORD*)(sp + 33) = 0; 
                        sp[37] = sCritHit;
                        
                        PACKET_HEADER* sHead = (PACKET_HEADER*)splashAckBuf.data();
                        sHead->id = 0x4016;
                        sHead->payloadSize = 38;
                        EncryptPacket(splashAckBuf.data(), 0x42);
                        BroadcastPacketToMap(playerMapID, splashAckBuf);
                    }

                    // 2. ° PC  (PVP)
                    std::vector<PlayerData*> aoiPlayers = mapInst->GetPlayersInAOI(wAoeCenterX, wAoeCenterY);
                    std::vector<PlayerData*> splashPlayers;
                    
                    LOG("[MugongHandler] [AOE-PvP] Scanned " + std::to_string(aoiPlayers.size()) + " potential players in AOI");
                    for (PlayerData* pPl : aoiPlayers) {
                        if (!pPl || pPl->dwObjectID == dwAttackID || pPl->dwObjectID == dwDefenseID || pPl->dwHpCur == 0 || pPl->dwInvulnerableUntil > GetTickCount()) continue;
                        float dx = (float)pPl->wPosX - (float)wAoeCenterX;
                        float dy = (float)pPl->wPosY - (float)wAoeCenterY;
                        float dist = sqrtf(dx * dx + dy * dy);
                        
                        LOG("[MugongHandler] [AOE-PvP] -> Player OID: " + std::to_string(pPl->dwObjectID) + " Name: " + pPl->szName + " Pos: (" + std::to_string(pPl->wPosX) + "," + std::to_string(pPl->wPosY) + ") Dist: " + std::to_string(dist));
                        if (dist <= splashRange) {
                            splashPlayers.push_back(pPl);
                            if (splashPlayers.size() >= 5) break;
                        }
                    }
                    LOG("[MugongHandler] [AOE-PvP] -> Splash players count selected: " + std::to_string(splashPlayers.size()));

                    for (PlayerData* pSplashPlayer : splashPlayers) {
                        DWORD sPlDef = pSplashPlayer->dwTotalDef;
                        WORD sPlDodge = pSplashPlayer->dwTotalDodge;
                        
                        DWORD sPlayerAtkRating = 50 + (pAttacker ? pAttacker->dwTotalHit : 0);
                        float sHitChance = (float)sPlayerAtkRating / (float)(sPlayerAtkRating + sPlDodge);
                        float sRoll = (float)(rand() % 10000) / 10000.0f;
                        
                        BYTE sCritHit = 0;
                        DWORD sFinalDmg = 0;
                        BYTE sResult = 2; // HIT
                        
                        if (sPlDodge > 0 && sRoll > sHitChance) {
                            sResult = 1; // MISS
                            sFinalDmg = 0;
                        } else {
                            DWORD pAtk = pAttacker ? pAttacker->dwTotalAtk : 50;
                            DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0;
                            DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0;
                            
                            float rawDmg = ((pAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg) * 0.7f;
                            float var = 0.9f + ((float)(rand() % 2000) / 10000.0f);
                            rawDmg *= var;
                            
                            WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                            if ((WORD)(rand() % 100) < critRate) {
                                rawDmg *= 1.5f;
                                sCritHit = 1;
                            }
                            
                            if (rawDmg > sPlDef) rawDmg -= sPlDef;
                            else rawDmg = 1;
                            
                            sFinalDmg = (DWORD)rawDmg;
                            
                            if (pSplashPlayer->dwHpCur > sFinalDmg) {
                                pSplashPlayer->dwHpCur -= sFinalDmg;
                            } else {
                                sFinalDmg = pSplashPlayer->dwHpCur;
                                pSplashPlayer->dwHpCur = 0;
                                pSplashPlayer->dwDeadTime = GetTickCount();
                                LOG("[MugongHandler] PvP Splash target player " + pSplashPlayer->szName + " died from skill " + std::to_string(dwMugongID) + "!");
                            }
                        }

                        // Debuff ±è¨èè° PC 
                        if (pMugongData && pMugongData->dwKeepUpTime > 0 && sResult == 2) {
                            sServerObject::sActiveBuff debuff;
                            debuff.dwMugongID = dwMugongID;
                            debuff.bLevel = bMugongLevel;
                            debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;
                            debuff.bIsDebuff = true;
                            pSplashPlayer->activeBuffs[dwMugongID] = debuff;
                            LOG("[MugongHandler] Applied PvP splash debuff skill " + std::to_string(dwMugongID) + " to player " + std::to_string(pSplashPlayer->dwObjectID));
                        }
                        
                        //  0x4016 ¤¨
                        std::vector<BYTE> splashAckBuf(4 + 38);
                        BYTE* sp = splashAckBuf.data() + 4;
                        sp[0] = sResult; 
                        *(DWORD*)(sp + 1) = dwMugongID;
                        sp[5] = bMugongLevel;
                        sp[6] = 1; 
                        *(DWORD*)(sp + 7) = dwAttackID;
                        *(WORD*)(sp + 11) = wAttackPosX;
                        *(WORD*)(sp + 13) = wAttackPosY;
                        sp[15] = bAttackHeight;
                        sp[16] = 1; // bDefenseType = 1 (Player)
                        *(DWORD*)(sp + 17) = pSplashPlayer->dwObjectID;
                        *(DWORD*)(sp + 21) = pSplashPlayer->dwHpMax;
                        *(DWORD*)(sp + 25) = pSplashPlayer->dwHpCur;
                        *(DWORD*)(sp + 29) = sFinalDmg;
                        *(DWORD*)(sp + 33) = 0; 
                        sp[37] = sCritHit;
                        
                        PACKET_HEADER* sHead = (PACKET_HEADER*)splashAckBuf.data();
                        sHead->id = 0x4016;
                        sHead->payloadSize = 38;
                        EncryptPacket(splashAckBuf.data(), 0x42);
                        BroadcastPacketToMap(playerMapID, splashAckBuf);

                        // è°· 0x3B0D é stats ±§
                        DWORD splashCharID = pSplashPlayer->dwObjectID - 400000000;
                        SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(splashCharID);
                        if (targetSock != INVALID_SOCKET) {
                            std::vector<BYTE> hpBuf(4);
                            auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                            auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                            push4(pSplashPlayer->dwHpMax);
                            push4(pSplashPlayer->dwHpCur);
                            push4(pSplashPlayer->wIpMax);
                            push4(pSplashPlayer->wIpCur);
                            hpBuf.push_back(0); 
                            PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                            hpHead->id = 0x3B0D;
                            hpHead->payloadSize = hpBuf.size() - 4;
                            EncryptPacket(hpBuf.data(), 0x42);
                            SafeSend(targetSock, (const char*)hpBuf.data(), hpBuf.size(), 0);

                            PlayerManager::GetInstance().RecalculateStats(splashCharID, true);
                        }
                    }
                }

                dwDefHpMax = pTarget->dwHpMax;
                dwDefHpCur = pTarget->dwHpCur;
                dwDefIpMax = pTarget->wIpMax;
                dwDefIpCur = pTarget->wIpCur;
            }
        }
    }

    *(DWORD*)(p + 21) = dwDefHpMax;
    *(DWORD*)(p + 25) = dwDefHpCur;
    *(DWORD*)(p + 29) = finalDmg;
    *(DWORD*)(p + 33) = 0; // dwExp in ack doesn't seem to give it directly
    p[37] = bCritHit; // bHitFlag (1 = Critical, 0 = Normal)

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4016; // CS_BT_MUGONGATTACK_ACK
    head->payloadSize = 38;
    EncryptPacket(ackBuf.data(), 0x42);

    // We must ALWAYS broadcast 0x4016 (MugongAttackAck), even for buff/self-cast skills.
    // The client's combat animation/effect state machine expects this to complete the sequence.
    // If suppressed, the client gets stuck in a state where it drops subsequent skill effects.
    // Using bResult = 0 ensures no self-hit stagger animation is played.
    BroadcastPacketToMap(playerMapID, ackBuf);

    // PvP éè§° Debuff ±§·°
    if (bDefenseType == 1 && dwDefenseID > 0) {
        DWORD defenderCharID = dwDefenseID - 400000000;
        SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(defenderCharID);
        if (targetSock != INVALID_SOCKET) {
            std::vector<BYTE> hpBuf(4);
            auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
            auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
            push4(dwDefHpMax);
            push4(dwDefHpCur);
            push2(dwDefIpMax);
            push2(dwDefIpCur);
            hpBuf.push_back(0); 
            PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
            hpHead->id = 0x3B0D;
            hpHead->payloadSize = hpBuf.size() - 4;
            EncryptPacket(hpBuf.data(), 0x42);
            SafeSend(targetSock, (const char*)hpBuf.data(), hpBuf.size(), 0);

            // è§é±§é Debuff 
            PlayerManager::GetInstance().RecalculateStats(defenderCharID, true);
        }
    }

    // Process exp granting, level-ups, and death animations for all dead monsters
    for (auto& de : deadEntities) {
        DWORD attackerCharID = dwAttackID - 400000000; // 800000301 -> 400000301 (matches DB/SessionMgr format)
        bool needRefresh = GrantExpToPlayer(attackerCharID, de.dwExp);
        if (needRefresh) {
            UpdatePlayerStatsAndSend(clientSocket, attackerCharID);
            
            // Restore HP/IP to max after level-up
            DWORD dwObjID = attackerCharID + 400000000;
            CMapInstance* mapInst2 = g_MapInstances.count(playerMapID) ? g_MapInstances[playerMapID] : nullptr;
            if (mapInst2) {
                {
                    std::lock_guard<std::mutex> lock2(mapInst2->GetMutex());
                    sServerObject* pObj = mapInst2->GetPlayer(dwObjID);
                    if (pObj) {
                        pObj->dwHpCur = pObj->dwHpMax;
                        pObj->wIpCur = pObj->wIpMax;
                    }
                }
                CharacterDB::GetInstance().RestoreHpIpToMax(attackerCharID);
                
                {
                    std::lock_guard<std::mutex> lock3(mapInst2->GetMutex());
                    sServerObject* pObj = mapInst2->GetPlayer(dwObjID);
                    if (pObj) {
                        std::vector<BYTE> hpBuf(4);
                        auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                        auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                        push4(pObj->dwHpMax);
                        push4(pObj->dwHpCur);
                        push4(pObj->wIpMax);
                        push4(pObj->wIpCur);
                        hpBuf.push_back(1);
                        PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                        hpHead->id = 0x3B0D;
                        hpHead->payloadSize = hpBuf.size() - 4;
                        EncryptPacket(hpBuf.data(), 0x42);
                        SafeSend(clientSocket, (char*)hpBuf.data(), hpBuf.size(), 0);
                    }
                }
            }
        }

        // Send CS_NC_STATUSCHANGE_ACK (0x3510) to trigger Death Animation
        auto pushDW = [](std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); };
        std::vector<BYTE> animBuf; animBuf.resize(4);
        animBuf.push_back(3); // bObjectType (Monster)
        DWORD oid = de.dwObjectID;
        pushDW(animBuf, oid);
        animBuf.push_back(3); // bStatus = 3 (Dead)
        animBuf.push_back(0); // wValue1 L
        animBuf.push_back(0); // wValue1 H
        animBuf.push_back(0xFF); // bValue2
        
        PACKET_HEADER* animHead = (PACKET_HEADER*)animBuf.data();
        animHead->id = 0x3510;
        animHead->payloadSize = animBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(animBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, animBuf);
        
        // Send CS_BT_KILLSUCCESS_ACK (0x4034) to attacker (trigger kill VFX)
        // Client reads: bObjectType(BYTE) + dwObjectID(DWORD) + dwExp(DWORD)
        std::vector<BYTE> killBuf; killBuf.resize(4);
        killBuf.push_back(3); // bObjectType (Monster)
        pushDW(killBuf, oid);
        pushDW(killBuf, de.dwExp);
        
        PACKET_HEADER* killHead = (PACKET_HEADER*)killBuf.data();
        killHead->id = 0x4034;
        killHead->payloadSize = killBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(killBuf.data(), 0x42);
        SafeSend(clientSocket, (char*)killBuf.data(), killBuf.size(), 0);
    }
}
// ----------------------------------------------------------------------------
// 选项设置处理器 (同步客户端安全保护设置: 所有角色/本门派成员/无对象)
// ----------------------------------------------------------------------------
void OnSetOptionReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 1) return;
    BYTE opt = payload[0];
    DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (g_MapInstances.count(playerMapID)) {
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(charID + 400000000);
        if (pObj) {
            pObj->bSafeMode = opt;
            LOG("[MugongHandler] OnSetOptionReq: Player " + std::to_string(charID + 400000000) + " set bSafeMode to " + std::to_string(opt));
        }
    }

    std::vector<BYTE> ackBuf(4 + 1);
    BYTE* p = ackBuf.data() + 4;
    p[0] = opt;
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x3A01; // CS_OP_SETOPTION_ACK
    head->payloadSize = 1;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
}
