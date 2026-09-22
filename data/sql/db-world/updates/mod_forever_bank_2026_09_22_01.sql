-- Forever Vault (9000100) world spawn in Stormwind, placed by Michael 2026-09-22.
-- Idempotent (DELETE by guid + INSERT), safe to re-apply. Fixed guid in the reserved
-- 9100011+ spawn range so the placement survives a full world-DB reset / redeploy.
DELETE FROM `creature` WHERE `guid` = 9100033;
INSERT INTO `creature`
(`guid`,`id`,`map`,`zoneId`,`areaId`,`spawnMask`,`phaseMask`,`equipment_id`,`position_x`,`position_y`,`position_z`,`orientation`,`spawntimesecs`,`wander_distance`,`currentwaypoint`,`curhealth`,`curmana`,`MovementType`,`npcflag`,`unit_flags`,`dynamicflags`,`ScriptName`,`VerifiedBuild`,`CreateObject`,`Comment`) VALUES
(9100033,9000100,0,0,0,1,1,0,-8819.53,652.034,94.897,4.64085,300,0,0,42,0,0,0,0,0,'',NULL,0,NULL);
