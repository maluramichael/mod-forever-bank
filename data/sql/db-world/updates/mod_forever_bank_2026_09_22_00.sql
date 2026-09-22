--
-- mod-forever-bank: content SQL for the "Forever Vault" gossip NPC (Kanboard #793).
--
-- Idempotent DELETE + INSERT, safe to re-apply. Column layout copied from
-- mod-reagent-bank's own NPC SQL (modules/mod-reagent-bank/data/sql/db-world/base/reagent_bank_NPC.sql)
-- to match this fork's WotLK-era creature_template schema.
--
-- No `creature` spawn row is included (matching mod-reagent-bank, which also
-- ships unspawned): summon the vault keeper in-game with `.npc add 9000100`
-- wherever you want it, e.g. next to a bank in a capital city.
--

SET
@Entry = 9000100,
@Name = "Forever Vault";

DELETE FROM `creature_template` WHERE `entry` = @Entry;
DELETE FROM `creature_template_model` WHERE `CreatureID` = @Entry;

INSERT INTO `creature_template`
    (`entry`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`,
     `npcflag`, `rank`, `dmgschool`, `baseattacktime`, `rangeattacktime`, `unit_class`, `unit_flags`, `type`,
     `type_flags`, `lootid`, `pickpocketloot`, `skinloot`, `AIName`, `MovementType`, `HoverHeight`,
     `RacialLeader`, `movementId`, `RegenHealth`, `flags_extra`, `ScriptName`)
VALUES
    (@Entry, @Name, 'Vault Keeper', NULL, 0, 1, 1, 0, 35, 1, 0, 0, 2000, 0, 1, 0, 7, 138412032, 0, 0, 0, '', 0,
     1, 0, 0, 1, 2, 'npc_forever_bank_vault');

INSERT INTO `creature_template_model`
    (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
VALUES
    (@Entry, 0, 15965, 1.0, 1.0, NULL);
