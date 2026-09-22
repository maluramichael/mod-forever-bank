/*
 * mod-forever-bank
 *
 * WoW-Forever: the 3.3.5a client bank is hard-capped (28 slots + 7 bags), so
 * this module adds EXTRA storage via a gossip NPC - a "virtual vault" where a
 * player deposits/withdraws items that are held in a DB table instead of any
 * live inventory slot.
 *
 * Item-move mechanism: deposited items are kept as real `item_instance` rows,
 * not destroyed and recreated, so enchants, durability, charges and the
 * random-property roll survive a deposit/withdraw round trip. This mirrors
 * two existing core code paths, studied before writing this file:
 *   - Mail attachments (src/server/game/Handlers/MailHandler.cpp,
 *     WorldSession::HandleSendMail): MoveItemFromInventory() to detach the
 *     item from the player, DeleteFromInventoryDB() + SaveToDB() to persist
 *     it standalone.
 *   - The guild bank (src/server/game/Guilds/Guild.cpp) and the auction
 *     house (src/server/game/AuctionHouse/AuctionHouseMgr.cpp): both
 *     reconstruct a full Item from an existing item_instance row via
 *     NewItemOrBag(proto) + Item::LoadFromDB(guid, ObjectGuid::Empty, fields,
 *     entry) instead of creating a brand-new item.
 *
 * mod-reagent-bank (this fork, modules/mod-reagent-bank) only stores
 * stackable trade-goods as (entry, subclass, amount) tuples and always
 * destroys/recreates the physical item via StoreNewItem() - fine for its
 * narrow use case (reagents have no enchants/durability/charges worth
 * keeping), but not good enough for a general-purpose vault that must also
 * hold gear, weapons and anything else a player might want to stash. This
 * module borrows mod-reagent-bank's gossip pagination shape (MAX_OPTIONS,
 * item icon/link helpers, Prev/Next/Back paging) but reuses the item-instance
 * preservation mechanism from mail/guild-bank/auction-house instead.
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the
 * License, or (at your option) any later version.
 */

#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "GossipDef.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"

#include "ForeverBank.h"

#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ForeverBank
{
    Config& GetConfig()
    {
        static Config cfg;
        return cfg;
    }
}

using ForeverBank::GetConfig;

namespace
{
    constexpr uint32 NPC_TEXT_ID = 4259; // pre-existing generic banker greeting (also used by mod-reagent-bank)
    constexpr uint32 MAX_OPTIONS = 20;   // items per gossip page, leaving room for Prev/Next/Back
    char const* const BACK_LABEL = "|TInterface/ICONS/Ability_Spy:30:30:-18:0|tBack...";

    enum GossipSender : uint32
    {
        SENDER_MAIN_MENU     = 1,
        SENDER_DEPOSIT_LIST  = 2,
        SENDER_WITHDRAW_LIST = 3,
        SENDER_DEPOSIT_ITEM  = 4,
        SENDER_WITHDRAW_ITEM = 5,
    };

    // Deposit-list selections encode the source bag/slot directly in the
    // gossip "action" field so the select handler does not have to re-derive
    // it from a rescan (bag contents cannot shift between opening the menu
    // and picking an entry within one gossip interaction).
    uint32 EncodeBagSlot(uint8 bag, uint8 slot)
    {
        return (static_cast<uint32>(bag) << 8) | static_cast<uint32>(slot);
    }

    void DecodeBagSlot(uint32 code, uint8& bag, uint8& slot)
    {
        bag  = static_cast<uint8>((code >> 8) & 0xFF);
        slot = static_cast<uint8>(code & 0xFF);
    }

    // Items that must never be handed off to the vault: containers (bags
    // hold nested item_instance rows that this simple table does not track)
    // and quest items (vanilla-style banks never accept those either).
    bool IsDepositable(Item const* item)
    {
        if (!item)
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return false;

        if (proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUEST)
            return false;

        return true;
    }
}

// Gossip-NPC + item-move logic for the vault.
class npc_forever_bank_vault : public CreatureScript
{
private:
    // -----------------------------------------------------------------
    // Small helpers copied/adapted from mod-reagent-bank's item icon/link
    // rendering (self-contained, no dependency on that module).
    // -----------------------------------------------------------------
    std::string GetItemLink(uint32 entry, WorldSession* session) const
    {
        int loc_idx = session->GetSessionDbLocaleIndex();
        ItemTemplate const* temp = sObjectMgr->GetItemTemplate(entry);
        std::string name = temp ? temp->Name1 : "Unknown Item";
        if (temp)
        {
            if (ItemLocale const* il = sObjectMgr->GetItemLocale(temp->ItemId))
                ObjectMgr::GetLocaleString(il->Name, loc_idx, name);
        }

        std::ostringstream oss;
        oss << "|c" << std::hex << ItemQualityColors[temp ? temp->Quality : ITEM_QUALITY_POOR] << std::dec <<
            "|Hitem:" << entry << ":" << (uint32)0 << "|h[" << name << "]|h|r";
        return oss.str();
    }

    std::string GetItemIcon(uint32 entry, uint32 width, uint32 height, int x, int y) const
    {
        std::ostringstream ss;
        ss << "|TInterface";
        ItemTemplate const* temp = sObjectMgr->GetItemTemplate(entry);
        ItemDisplayInfoEntry const* dispInfo = nullptr;
        if (temp)
        {
            dispInfo = sItemDisplayInfoStore.LookupEntry(temp->DisplayInfoID);
            if (dispInfo)
                ss << "/ICONS/" << dispInfo->inventoryIcon;
        }
        if (!dispInfo)
            ss << "/InventoryItems/WoWUnknownItem01";
        ss << ":" << width << ":" << height << ":" << x << ":" << y << "|t";
        return ss.str();
    }

    // Per-config: the vault is either per character (owner_guid = character
    // low GUID) or shared per account (owner_guid = account id).
    uint32 GetOwnerId(Player* player) const
    {
        if (GetConfig().PerAccount)
            return player->GetSession()->GetAccountId();

        return player->GetGUID().GetCounter();
    }

    // Deletes a `forever_bank` row that turned out to be orphaned (its
    // item_entry or item_instance row no longer exists) and tells the player.
    void PurgeOrphanRow(Player* player, Creature* creature, uint32 rowId, char const* reason)
    {
        CharacterDatabase.Execute("DELETE FROM `forever_bank` WHERE `id` = {}", rowId);
        ChatHandler(player->GetSession()).PSendSysMessage(reason);
        ShowWithdrawList(player, creature, 0);
    }

    uint32 CountStoredItems(uint32 ownerId) const
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM `forever_bank` WHERE `owner_guid` = {}", ownerId);
        if (!result)
            return 0;

        return static_cast<uint32>((*result)[0].Get<uint64>());
    }

    // -----------------------------------------------------------------
    // Menus
    // -----------------------------------------------------------------
    void ShowMainMenu(Player* player, Creature* creature)
    {
        if (!GetConfig().Enable)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("The Forever Vault is currently closed.");
            CloseGossipMenuFor(player);
            return;
        }

        AddGossipItemFor(player, GOSSIP_ICON_VENDOR, "Deposit an Item", SENDER_DEPOSIT_LIST, 0);
        AddGossipItemFor(player, GOSSIP_ICON_VENDOR, "Withdraw an Item", SENDER_WITHDRAW_LIST, 0);
        SendGossipMenuFor(player, NPC_TEXT_ID, creature->GetGUID());
    }

    void ShowDepositList(Player* player, Creature* creature, uint32 page)
    {
        std::vector<std::pair<uint8, uint8>> candidates;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* it = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (IsDepositable(it))
                    candidates.emplace_back(INVENTORY_SLOT_BAG_0, slot);

        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            Bag* pBag = player->GetBagByPos(bag);
            if (!pBag)
                continue;

            for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (Item* it = player->GetItemByPos(bag, slot))
                    if (IsDepositable(it))
                        candidates.emplace_back(bag, slot);
        }

        WorldSession* session = player->GetSession();
        uint32 startIdx = page * MAX_OPTIONS;

        for (uint32 i = startIdx; i < startIdx + MAX_OPTIONS && i < candidates.size(); ++i)
        {
            uint8 bag = candidates[i].first;
            uint8 slot = candidates[i].second;
            Item* it = player->GetItemByPos(bag, slot);
            if (!it)
                continue;

            std::string text = GetItemIcon(it->GetEntry(), 30, 30, -18, 0) + GetItemLink(it->GetEntry(), session) +
                " (" + std::to_string(it->GetCount()) + ")";
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, text, SENDER_DEPOSIT_ITEM, EncodeBagSlot(bag, slot));
        }

        if (page > 0)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Previous Page", SENDER_DEPOSIT_LIST, page - 1);
        if (startIdx + MAX_OPTIONS < candidates.size())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Next Page", SENDER_DEPOSIT_LIST, page + 1);

        AddGossipItemFor(player, GOSSIP_ICON_TALK, BACK_LABEL, SENDER_MAIN_MENU, 0);
        SendGossipMenuFor(player, NPC_TEXT_ID, creature->GetGUID());
    }

    void ShowWithdrawList(Player* player, Creature* creature, uint32 page)
    {
        uint32 ownerId = GetOwnerId(player);
        WorldSession* session = player->GetSession();

        QueryResult result = CharacterDatabase.Query(
            "SELECT `id`, `item_entry`, `item_count` FROM `forever_bank` WHERE `owner_guid` = {} ORDER BY `id`",
            ownerId);

        std::vector<std::tuple<uint32, uint32, uint32>> rows; // rowId, entry, count
        if (result)
        {
            do
            {
                Field* f = result->Fetch();
                uint32 rowId = static_cast<uint32>(f[0].Get<uint64>());
                uint32 entry = f[1].Get<uint32>();
                uint32 count = f[2].Get<uint32>();
                rows.emplace_back(rowId, entry, count);
            } while (result->NextRow());
        }

        uint32 startIdx = page * MAX_OPTIONS;

        for (uint32 i = startIdx; i < startIdx + MAX_OPTIONS && i < rows.size(); ++i)
        {
            uint32 rowId = std::get<0>(rows[i]);
            uint32 entry = std::get<1>(rows[i]);
            uint32 count = std::get<2>(rows[i]);

            std::string text = GetItemIcon(entry, 30, 30, -18, 0) + GetItemLink(entry, session) +
                " (" + std::to_string(count) + ")";
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, text, SENDER_WITHDRAW_ITEM, rowId);
        }

        if (page > 0)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Previous Page", SENDER_WITHDRAW_LIST, page - 1);
        if (startIdx + MAX_OPTIONS < rows.size())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Next Page", SENDER_WITHDRAW_LIST, page + 1);

        AddGossipItemFor(player, GOSSIP_ICON_TALK, BACK_LABEL, SENDER_MAIN_MENU, 0);
        SendGossipMenuFor(player, NPC_TEXT_ID, creature->GetGUID());
    }

    // -----------------------------------------------------------------
    // Actions
    // -----------------------------------------------------------------
    void DepositItem(Player* player, Creature* creature, uint8 bag, uint8 slot)
    {
        ForeverBank::Config const& cfg = GetConfig();
        if (!cfg.Enable)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("The Forever Vault is currently closed.");
            CloseGossipMenuFor(player);
            return;
        }

        Item* pItem = player->GetItemByPos(bag, slot);
        if (!pItem || !IsDepositable(pItem))
        {
            ChatHandler(player->GetSession()).PSendSysMessage("That item can no longer be deposited.");
            ShowDepositList(player, creature, 0);
            return;
        }

        uint32 ownerId = GetOwnerId(player);
        if (CountStoredItems(ownerId) >= cfg.MaxSlots)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Your Forever Vault is full.");
            ShowDepositList(player, creature, 0);
            return;
        }

        uint32 itemGuid = pItem->GetGUID().GetCounter();
        uint32 itemEntry = pItem->GetEntry();
        uint32 itemCount = pItem->GetCount();

        // Detach the item from the player and keep its item_instance row
        // standing alone, exactly like WorldSession::HandleSendMail does for
        // a mailed item - this is what preserves enchants/durability/charges
        // instead of destroying and recreating the item.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        player->MoveItemFromInventory(pItem->GetBagSlot(), pItem->GetSlot(), true);
        pItem->DeleteFromInventoryDB(trans);
        if (pItem->GetState() == ITEM_UNCHANGED)
            pItem->FSetState(ITEM_CHANGED);
        pItem->SaveToDB(trans);
        trans->Append(
            "INSERT INTO `forever_bank` (`owner_guid`, `item_entry`, `item_count`, `item_guid`) "
            "VALUES ({}, {}, {}, {})",
            ownerId, itemEntry, itemCount, itemGuid);
        CharacterDatabase.CommitTransaction(trans);

        delete pItem; // no longer tracked by the player; its state lives on in item_instance + forever_bank

        ChatHandler(player->GetSession()).PSendSysMessage("Item stored in the Forever Vault.");
        ShowDepositList(player, creature, 0);
    }

    void WithdrawItem(Player* player, Creature* creature, uint32 rowId)
    {
        ForeverBank::Config const& cfg = GetConfig();
        if (!cfg.Enable)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("The Forever Vault is currently closed.");
            CloseGossipMenuFor(player);
            return;
        }

        uint32 ownerId = GetOwnerId(player);

        QueryResult result = CharacterDatabase.Query(
            "SELECT `item_entry`, `item_count`, `item_guid` FROM `forever_bank` WHERE `id` = {} AND `owner_guid` = {}",
            rowId, ownerId);
        if (!result)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("That item is no longer in your vault.");
            ShowWithdrawList(player, creature, 0);
            return;
        }

        Field* row = result->Fetch();
        uint32 itemEntry = row[0].Get<uint32>();
        uint32 itemCount = row[1].Get<uint32>();
        uint32 itemGuid  = row[2].Get<uint32>();

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
        if (!proto)
        {
            PurgeOrphanRow(player, creature, rowId, "That item no longer exists and was removed from your vault.");
            return;
        }

        // Reconstruct the real item_instance row (enchants/durability/charges
        // intact), the same way the guild bank and the auction house load an
        // existing item on demand: NewItemOrBag() + Item::LoadFromDB().
        QueryResult instResult = CharacterDatabase.Query(
            "SELECT `creatorGuid`, `giftCreatorGuid`, `count`, `duration`, `charges`, `flags`, `enchantments`, "
            "`randomPropertyId`, `durability`, `playedTime`, `text` FROM `item_instance` WHERE `guid` = {}",
            itemGuid);
        if (!instResult)
        {
            PurgeOrphanRow(player, creature, rowId, "That item could not be found and was removed from your vault.");
            return;
        }

        Item* pItem = NewItemOrBag(proto);
        if (!pItem->LoadFromDB(itemGuid, ObjectGuid::Empty, instResult->Fetch(), itemEntry))
        {
            delete pItem;
            PurgeOrphanRow(player, creature, rowId, "That item could not be loaded and was removed from your vault.");
            return;
        }

        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, pItem, false);
        if (msg != EQUIP_ERR_OK)
        {
            player->SendEquipError(msg, nullptr, nullptr, itemEntry);
            delete pItem;
            ShowWithdrawList(player, creature, 0);
            return;
        }

        CharacterDatabase.Execute("DELETE FROM `forever_bank` WHERE `id` = {}", rowId);

        Item* stored = player->StoreItem(dest, pItem, true);
        player->SendNewItem(stored, itemCount, true, false);

        ShowWithdrawList(player, creature, 0);
    }

public:
    npc_forever_bank_vault() : CreatureScript("npc_forever_bank_vault") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ShowMainMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        player->PlayerTalkClass->ClearMenus();

        switch (sender)
        {
            case SENDER_MAIN_MENU:
                ShowMainMenu(player, creature);
                break;
            case SENDER_DEPOSIT_LIST:
                ShowDepositList(player, creature, action);
                break;
            case SENDER_WITHDRAW_LIST:
                ShowWithdrawList(player, creature, action);
                break;
            case SENDER_DEPOSIT_ITEM:
            {
                uint8 bag = 0;
                uint8 slot = 0;
                DecodeBagSlot(action, bag, slot);
                DepositItem(player, creature, bag, slot);
                break;
            }
            case SENDER_WITHDRAW_ITEM:
                WithdrawItem(player, creature, action);
                break;
            default:
                ShowMainMenu(player, creature);
                break;
        }

        return true;
    }
};

// =====================================================================
//  WorldScript: config load + schema.
// =====================================================================
class ForeverBankWorldScript : public WorldScript
{
public:
    ForeverBankWorldScript() : WorldScript("ForeverBank_WorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        ForeverBank::Config& cfg = GetConfig();
        cfg.Enable     = sConfigMgr->GetOption<bool>("ForeverBank.Enable", true);
        cfg.MaxSlots   = sConfigMgr->GetOption<uint32>("ForeverBank.MaxSlots", 200);
        cfg.PerAccount = sConfigMgr->GetOption<bool>("ForeverBank.PerAccount", false);
    }

    void OnStartup() override
    {
        // Deliberately NOT an SQL update file: on this fork a failing module
        // SQL aborts the whole worldserver boot (see mod-guild-tax for the
        // same convention), so the table is created programmatically here
        // and tolerated at runtime instead.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `forever_bank` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
            "`owner_guid` INT UNSIGNED NOT NULL, "
            "`item_entry` INT UNSIGNED NOT NULL, "
            "`item_count` INT UNSIGNED NOT NULL, "
            "`item_guid` INT UNSIGNED NOT NULL, "
            "PRIMARY KEY (`id`), "
            "UNIQUE KEY `idx_item_guid` (`item_guid`), "
            "KEY `idx_owner` (`owner_guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
    }
};

// =====================================================================
//  Registration
// =====================================================================
void AddForeverBankScripts()
{
    new npc_forever_bank_vault();
    new ForeverBankWorldScript();
}
