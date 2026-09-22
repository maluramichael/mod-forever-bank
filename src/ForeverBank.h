/*
 * mod-forever-bank - shared declarations.
 *
 * A gossip-NPC "virtual vault" that gives players extra storage beyond the
 * 3.3.5a client's hard-capped bank (28 slots + 7 bags). Deposited items are
 * kept as real item_instance rows (not destroyed/recreated), so enchants,
 * durability, charges and randomProperty are preserved across a deposit /
 * withdraw round trip. See ForeverBank.cpp for the item-move mechanism,
 * mirrored from the core's own mail-attachment flow
 * (src/server/game/Handlers/MailHandler.cpp) and the guild bank
 * (src/server/game/Guilds/Guild.cpp).
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the
 * License, or (at your option) any later version.
 */

#ifndef MOD_FOREVER_BANK_H
#define MOD_FOREVER_BANK_H

#include <cstdint>

namespace ForeverBank
{
    // Cached config (populated in WorldScript::OnAfterConfigLoad).
    struct Config
    {
        bool     Enable     = true;  // module master switch
        uint32_t MaxSlots   = 200;   // max stored rows per owner (character or account)
        bool     PerAccount = false; // false = vault is per character, true = shared per account
    };

    Config& GetConfig();
}

#endif // MOD_FOREVER_BANK_H
