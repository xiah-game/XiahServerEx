    // 3. Class 4: Healing / HP & IP Recovery Skills

    if (isHeal || dwMugongID == 63) {

        if (g_MapInstances.count(playerMapID)) {

            CMapInstance* mapInst = g_MapInstances[playerMapID];

            std::lock_guard<std::mutex> lock(mapInst->GetMutex());

            sServerObject* pObj = mapInst->GetPlayer(dwAttackID); // ��

            if (pObj) {

                // 1. IP Cost deduction first from caster (pObj)

                DWORD cost = pMugongData ? pMugongData->dwCostMp : 0;

                if (pObj->wIpCur < cost) {

                    LOG("[MugongHandler] Heal Skill " + std::to_string(dwMugongID) + " blocked: IP " 

                        + std::to_string(pObj->wIpCur) + " < cost " + std::to_string(cost));

                    return;

                }

                pObj->wIpCur -= cost;



                // 2. �� (������ bKind=0 �� bKind=1/2 )

                sServerObject* pTarget = nullptr;

                SOCKET targetSocket = clientSocket;

                

                sMugongTemplate* pTpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);

                BYTE bKind = pTpl ? pTpl->bKind : 0;



                // �쨨(bKind==12������㨨)����

                if (bKind == 1 || bKind == 2) {

                    if (bDefenseType == 1 && dwDefenseID > 0 && dwDefenseID != dwAttackID) {

                        sServerObject* pPotentialTarget = mapInst->GetPlayer(dwDefenseID);

                        if (pPotentialTarget) {

                            // ������ (���� wAttackRange  > 0)

                            float dx = (float)pObj->wPosX - (float)pPotentialTarget->wPosX;

                            float dy = (float)pObj->wPosY - (float)pPotentialTarget->wPosY;

                            float dist = sqrtf(dx * dx + dy * dy);

                            

                            if (pMugongData && pMugongData->wAttackRange > 0 && dist > (float)pMugongData->wAttackRange) {

                                LOG("[MugongHandler] Heal Skill " + std::to_string(dwMugongID) + " target out of range: " 

                                    + std::to_string((int)dist) + " > " + std::to_string(pMugongData->wAttackRange));

                                return; // ������

                            }

                            

                            pTarget = pPotentialTarget;

                            targetSocket = SessionMgr::GetInstance().GetSocketByCharID(dwDefenseID - 400000000);

                        }

                    }

                }

                

                // �����쨨����������(bKind==0)������������

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



                // 6. ���������������訨���� 0x3B0D �㨨��

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

        

        // ���������쨨�쨨����

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

                p[16] = pTarget->bObjectType; // ���� (�� 1=PC)

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