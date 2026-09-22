# mod-forever-bank

An [AzerothCore](https://www.azerothcore.org/) module (WotLK 3.3.5a) that adds extra item
storage through a gossip NPC — a "vault" beyond the client's fixed bank size.

## What it does

Talk to the **Forever Vault** NPC to deposit and withdraw items:

- **Deposit** — move an item from your bags into the vault.
- **Withdraw** — take a stored item back into your bags.

Items keep their full instance data (enchantments, durability, charges, random properties)
because the module reuses the core's own item-move path rather than destroying and
recreating items. Storage is per character by default, or per account if configured.

## Setup

The NPC (`creature_template` entry `9000100`, "Forever Vault") ships as data but is not
pre-spawned. Add it in-game where you want it, e.g.:

```
.npc add 9000100
```

The module creates its storage table automatically on first start.

## Configuration

`conf/mod_forever_bank.conf.dist`:

| Key                     | Default | Description                              |
|-------------------------|---------|------------------------------------------|
| `ForeverBank.Enable`    | `1`     | Master on/off switch                     |
| `ForeverBank.MaxSlots`  | `200`   | Maximum stored items per owner           |
| `ForeverBank.PerAccount`| `0`     | `0` = per character, `1` = per account   |

## Installation

Clone into your AzerothCore `modules/` directory and rebuild the worldserver.

## License

Released under the GNU GPL v2 (or later).
