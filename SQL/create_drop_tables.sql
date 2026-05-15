-- =============================================
-- Drop Group System Tables
-- =============================================

-- NPC_DROPGROUP: defines drop groups per NPC type
-- bNpcType: NPC type from NPCTEMPLATE
-- dwGroupID: unique group ID
-- wDropRate: 1/N chance (1=always, 100=1%, 0=disabled)
-- bMinDrop: min picks when triggered
-- bMaxDrop: max picks when triggered
CREATE TABLE NPC_DROPGROUP (
    bNpcType    TINYINT NOT NULL,
    dwGroupID   INT NOT NULL,
    wDropRate   SMALLINT NOT NULL DEFAULT 1,
    bMinDrop    TINYINT NOT NULL DEFAULT 1,
    bMaxDrop    TINYINT NOT NULL DEFAULT 1,
    PRIMARY KEY (bNpcType, dwGroupID)
);

-- NPC_DROPGROUPITEM: items within each drop group
-- dwGroupID: references NPC_DROPGROUP.dwGroupID
-- dwItemID: wRefID from ITEMTEMPLATE
-- wWeight: weighted random weight within group
CREATE TABLE NPC_DROPGROUPITEM (
    dwGroupID   INT NOT NULL,
    dwItemID    INT NOT NULL,
    wWeight     SMALLINT NOT NULL DEFAULT 100
);

-- =============================================
-- Example Data (remove before production)
-- =============================================

-- Example: NPC type 5 has 2 drop groups
-- Group 1: always triggers (wDropRate=1), picks 1~2 items
-- Group 2: 10% chance (wDropRate=10), picks 1 item
/*
INSERT INTO NPC_DROPGROUP (bNpcType, dwGroupID, wDropRate, bMinDrop, bMaxDrop) VALUES
(5, 1, 1, 1, 2),
(5, 2, 10, 1, 1);

INSERT INTO NPC_DROPGROUPITEM (dwGroupID, dwItemID, wWeight) VALUES
(1, 20001, 60),
(1, 20002, 30),
(1, 20003, 10),
(2, 30001, 80),
(2, 30002, 20);
*/

-- Remember: NPCLIST.wRootItem controls the master gate (1:N)
-- Example: UPDATE NPCLIST SET wRootItem = 2 WHERE bNpcType = 5
-- This means 1/2 = 50% chance to enter the drop group system
