// MugongPreAttack.cpp — OnSelMugongReq / OnMugongPreAttackReq / IsAoeSkill
// 从 MugongHandler.cpp 拆分而来，纯搬迁不改逻辑
#include <unordered_map>
#include "MugongHandler.h"
#include "MugongAttackContext.h"
#include "../GameObjects/MugongManager.h"
#include "../GameObjects/PlayerManager.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"

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

    if (dwMugongID == 41) {
        auto SendSystemMessage = [&](const std::string& text) {
            std::vector<BYTE> buf; buf.resize(4, 0);
            DWORD senderObjID = 0;
            buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
            buf.push_back(8); // CT_TIMEMESSAGE
            WORD len = (WORD)text.size();
            buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
            buf.insert(buf.end(), text.begin(), text.end());
            WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
            WORD payloadSize = (WORD)(buf.size() - 4);
            memcpy(&buf[0], &packetID, 2);
            memcpy(&buf[2], &payloadSize, 2);
            EncryptPacket(buf.data(), 0x42);
            SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
        };
        SendSystemMessage("[DEBUG] 服务端已收到分光破影(41)预施法包(0x4013)");
        LOG("[MugongHandler] 0x4013 OnMugongPreAttackReq triggered for Skill 41!");
    }


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

    bool isBuff = (pd && pd->dwKeepUpTime > 0 && dwMugongID != 41);

    DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);

    // 检查五行必杀蓄气�?
    /*
    if (dwMugongID >= 150 && dwMugongID <= 154) {
        bool canCast = false;
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            PlayerData* pObj = mapInst->GetPlayer(charID + 400000000);
            if (pObj && pObj->dwFiveElmGauge >= 5000) {
                canCast = true;
            }
        }
        if (!canCast) {
            LOG("[MugongHandler] 蓄气值不�?000，无法释放五行必杀技! ID=" + std::to_string(dwMugongID));
            // 发送失败包 (bResult = 1, 代表内功/蓄力不足)
            std::vector<BYTE> failBuf(4 + 29, 0);
            failBuf[4] = 1; // bResult = 1
            *(DWORD*)(failBuf.data() + 5) = dwMugongID;
            PACKET_HEADER* head = (PACKET_HEADER*)failBuf.data();
            head->id = 0x4014; // PKT_MUGONGPREATTACK_ACK
            head->payloadSize = failBuf.size() - 4;
            EncryptPacket(failBuf.data(), 0x42);
            SafeSend(clientSocket, (const char*)failBuf.data(), failBuf.size(), 0);
            return;
        }
    }
    */
    // �������� Debuff (����/����/ѣ��) ���ؼ�������
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
    // �������� Debuff (����/����/ѣ��) ���ؼ�������
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

        // �� Buff ������������������쨦�訦��?178

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

    // 1. �����������?(bKind 80-84) ����

    if (tpl->bKind >= 80 && tpl->bKind <= 84) return true;

    

    // 2. ������ 80~120 ���㨦����

    // �������?(bType = 3) �����������?(bKind = 0) ���?

    if (tpl->bType == 3 && tpl->bKind == 0) return true;

    

    return false;

}
