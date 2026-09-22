"""Authoritative companion action catalogue for the Python bridge.

Mirrors ``AzerothFriendActionRegistry`` in the C++ module: the executor refuses any
action name that is not in that registry, so the planner must only ever emit these.
Keeping the list here (and mirroring it in C++) means the LLM can never be told to
use an action the server cannot execute.
Indexed across 10 semantic domains for high-performance cognitive planning.
"""

import re
from typing import Any, Dict, List, Optional, Tuple

# name -> (category, params, description, passthrough)
CATALOG: Dict[str, Tuple[str, str, str, bool]] = {
    # ------------------------------------------------------------------ movement
    "move_to": ("movement", "x,y,z,[range]", "Walk to exact world coordinates.", False),
    "go_to": ("movement", "guid|name,[range]", "Walk to an entity from the surroundings list.", False),
    "follow": ("movement", "", "Follow the master with full pathfinding.", False),
    "stay": ("movement", "", "Halt and hold position.", False),
    "stop": ("movement", "", "Alias of stay.", False),
    "mount": ("movement", "", "Mount up when possible.", False),
    "dismount": ("movement", "", "Dismount.", False),
    "flee": ("movement", "", "Flee from the current attacker.", False),
    "runaway": ("movement", "", "Kite mob away from danger.", False),
    "grind": ("movement", "", "Hunt nearby hostile mobs autonomously.", False),
    "wander": ("movement", "", "Random idle roam around the current spot.", False),
    "travel_to": ("movement", "destination", "Use playerbots travel to a named destination.", False),
    "taxi": ("movement", "destination", "Take a known flight path through playerbots.", False),
    "summon": ("movement", "", "Summon bot to master's position.", False),
    "disperse": ("movement", "[distance]", "Maintain distance of X yards between bots.", False),
    "disperse_disable": ("movement", "", "Reset disperse distance to default.", False),

    # -------------------------------------------------------------------- combat
    "attack": ("combat", "guid|name|master_target", "Engage a target.", False),
    "attack_my_target": ("combat", "", "Overrides current actions and attacks master's target immediately.", False),
    "assist": ("combat", "[role]", "Combat assist: dps|tank|aoe.", False),
    "aoe": ("combat", "", "Area damage assist.", False),
    "pull": ("combat", "", "Pull the current target.", False),
    "pull_back": ("combat", "", "Tank pulls mob with ranged skill and returns to starting point.", False),
    "wait_for_attack": ("combat", "[seconds]", "Wait N seconds before attacking or healing in combat.", False),
    "mark_rti": ("combat", "", "Mark lowest health combat attacker with raid target icon.", False),
    "behind": ("combat", "", "Move behind target's back (rear flank).", False),
    "tank_face": ("combat", "", "Face target away from ranged group members.", False),
    "focus": ("combat", "", "Stop AoE/multi-target debuffs and focus on single target.", False),
    "threat": ("combat", "", "Actively manage threat to avoid pulling aggro from tank.", False),
    "boost": ("combat", "", "Use major burst and cooldown abilities.", False),
    "cc": ("combat", "", "Use crowd control ability on marked RTI target.", False),
    "cast": ("combat", "spellid|spell,[guid|target]", "Cast a spell by ID or name, optionally on target.", False),
    "cast_on": ("combat", "spell,target", "Cast a named spell on a specific player/friendly target.", False),
    "spell_exclude": ("combat", "action,[spellid]", "Manage excluded spells list (+id, -id, reset).", False),
    "pet_attack": ("combat", "", "Send the pet at the current target.", False),
    "use_trinket": ("combat", "", "Use the best equipped trinket.", False),
    "racial": ("combat", "", "Use the racial combat ability.", False),

    # ------------------------------------------------------------------ strategy
    "strategy": ("strategy", "add|remove|spec,[state]", "Change playerbots strategies.", True),
    "set_action_mode": ("strategy", "mode", "Set companion action mode (combat, travel, idle, social).", False),
    "pet_summon": ("strategy", "pet", "Select active pet (imp, voidwalker, succubus, felhunter, felguard).", False),
    "soulstone": ("strategy", "target", "Use soulstone on target (master, self, tank, healer).", False),

    # -------------------------------------------------------------- quests / RPG
    "talk_to": ("quest", "guid|name,[kind]", "Walk to an NPC and talk.", False),
    "interact": ("quest", "guid", "Walk to and use a creature or game object.", False),
    "use_object": ("quest", "guid", "Use a game object.", False),
    "accept_quest": ("quest", "id", "Accept a quest.", False),
    "accept_all_quests": ("quest", "", "Accept every available quest from nearby quest givers.", False),
    "turn_in_quest": ("quest", "id", "Turn in a completed quest and take the reward.", False),
    "choose_reward": ("quest", "item", "Choose quest reward by item link or name.", False),
    "share_quest": ("quest", "id", "Share a quest with the group.", False),
    "drop_quest": ("quest", "id", "Abandon a quest.", False),
    "quest_summary": ("quest", "[all]", "Show quest log summary or list all quests with links.", False),
    "trainer": ("quest", "", "Show what can be learned from selected trainer.", False),
    "trainer_learn": ("quest", "", "Learn available abilities from the selected trainer.", False),
    "talents": ("quest", "", "Auto-assign talent points.", False),
    "rpg_status": ("rpg", "state,[id]", "Set RPG state (idle, rest, wander_random, wander_npc, go_grind, go_camp, travel_flight, outdoor_pvp, do_quest).", False),
    "rpg_do_quest": ("rpg", "id", "Switch to RPG quest state on any valid quest ID or link.", False),

    # ---------------------------------------------------------- loot / gathering
    "loot": ("loot", "", "Loot the nearest lootable corpse.", False),
    "loot_all": ("loot", "", "Enable auto loot and loot everything reachable.", False),
    "loot_nearest": ("loot", "[radius]", "Walk to and loot the nearest corpse.", False),
    "gather": ("loot", "", "Gather nearby herbs, veins and resource nodes.", False),
    "open_loot": ("loot", "", "Open the current loot window.", False),
    "loot_filter": ("loot", "filter,[item]", "Set loot filter (all, normal, gray, quest, skill) or add/remove item.", False),

    # ------------------------------------------------------------ economy / gear
    "sell": ("economy", "[item|all_junk]", "Sell grey junk or a named item to a vendor.", False),
    "buy": ("economy", "item,[count]", "Buy an item from a vendor.", False),
    "repair": ("economy", "", "Repair all equipment.", False),
    "bank": ("economy", "[action],[item]", "Deposit or withdraw items from the bank.", False),
    "guild_bank": ("economy", "action,item", "Deposit or withdraw items from guild bank.", False),
    "mail": ("economy", "", "Check and handle mail.", False),
    "send_mail": ("economy", "item|money", "Send one item or money to the master through a mailbox.", False),
    "craft": ("economy", "item,[count]", "Set a playerbot crafting request for a known recipe.", False),
    "equip": ("economy", "item", "Equip an item by name or id.", False),
    "unequip": ("economy", "item|slot", "Unequip an item by name or equipment slot.", False),
    "equip_upgrades": ("economy", "", "Auto-equip upgrades found in bags.", False),
    "open_items": ("economy", "", "Open items in inventory that have loot (satchels, boxes).", False),
    "use_item": ("economy", "item", "Use an item from inventory.", False),
    "use_item_on": ("economy", "item,target", "Use an item on a target (e.g. gem on item).", False),
    "destroy_item": ("economy", "item", "Destroy an item from inventory.", False),
    "give_gold": ("economy", "gold,[silver],[copper]", "Give gold to the master.", False),
    "outfit": ("economy", "name,action,[item]", "Manage outfits (equip, replace, update, reset, add, remove).", False),
    "maintenance": ("economy", "", "Run full playerbots maintenance.", False),

    # ----------------------------------------------------------------- survival
    "food": ("survival", "", "Eat food to restore health.", False),
    "drink": ("survival", "", "Drink to restore mana.", False),
    "eat_drink": ("survival", "", "Sit and both eat and drink.", False),
    "tavern_rest": ("survival", "", "Living downtime: sit in inn or capital city and rest.", False),
    "campfire_cook": ("survival", "", "Living downtime: pitch campfire and rest in wilderness.", False),
    "healing_potion": ("survival", "", "Use a healing potion.", False),
    "mana_potion": ("survival", "", "Use a mana potion.", False),
    "healthstone": ("survival", "", "Use a healthstone.", False),
    "hearthstone": ("survival", "", "Use the hearthstone.", False),
    "revive": ("survival", "", "Recover the corpse / accept resurrection.", False),
    "release": ("survival", "", "Release the spirit after death.", False),

    # ------------------------------------------------------------------- social
    "emote": ("social", "emote", "Play a body-language emote.", False),
    "say": ("social", "text,[channel]", "Speak in game (gated by the speech fallback).", False),
    "greet": ("social", "", "Greet nearby players/bots.", False),
    "invite": ("social", "[name]", "Invite a nearby player to the group.", False),
    "leave_group": ("social", "", "Leave the current group.", False),
    "give_leader": ("social", "", "Pass group/raid leader to master.", False),
    "lfg": ("social", "[size]", "Join LFG queue or fill party/raid (size = 5|10|20|25|40).", False),
    "ready": ("social", "", "Answer a ready check.", False),
    "duel_start": ("social", "guid|name", "Challenge a player to a duel.", False),
    "duel_accept": ("social", "", "Accept a pending duel.", False),
    "duel_decline": ("social", "", "Decline a pending duel.", False),
    "attack_duel_opponent": ("social", "guid", "Fight the duel opponent.", False),
    "trade": ("social", "[guid|name|target]", "Initiate trade dialog with target or master.", False),
    "trade_start": ("social", "[guid|name|target]", "Initiate trade dialog with target or master.", False),
    "trade_accept": ("social", "", "Accept the current trade.", False),
    "trade_cancel": ("social", "", "Cancel the current trade.", False),
    "trade_set_item": ("social", "item,[slot],[count]", "Place an item into the active trade window.", False),
    "trade_clear_item": ("social", "[slot],[item]", "Remove an item from the active trade window.", False),
    "trade_set_gold": ("social", "gold|copper", "Set the gold offered in the active trade window.", False),
    "trade_link_items": ("social", "[category]", "Link inventory items in chat (consumable, potion, trade_goods, equipment, all).", False),
    "inspect": ("social", "[guid|name|target]", "Inspect target or master.", False),
    "target": ("social", "guid|name", "Set selection target by name or guid.", False),
    "pvp": ("social", "", "Toggle PvP flag through the native playerbot command surface.", False),
    "roll": ("social", "[item]", "Roll on pending loot or linked item.", False),

    # --------------------------------------------------------------------- meta
    "playerbot_action": ("meta", "action,[param]", "Escape hatch: run ANY native mod-playerbots action (validated server side).", True),
    "playerbot_command": ("meta", "command", "Escape hatch: raw playerbot chat command (separator aware, denylist filtered).", True),
    "wait": ("meta", "seconds", "Pause the plan for N seconds.", False),
}

BOT_API_VERSION = 1
OWNER_COMMAND_ACTIONS = {
    "buy", "sell", "bank", "guild_bank", "mail", "send_mail", "craft",
    "destroy_item", "give_gold", "outfit", "drop_quest", "trainer_learn", "talents",
    "invite", "leave_group", "give_leader", "lfg", "duel_start", "duel_accept", "pvp",
    "trade", "trade_start", "trade_accept", "trade_set_item", "trade_clear_item",
    "trade_set_gold", "playerbot_action", "playerbot_command", "strategy",
}
OWNER_AUTHORITY_EVENT_TYPES = frozenset({"player_command", "trade_requested", "duel_requested"})


def event_authority(event_type: str, source_guid: Any, master_guid: Any) -> str:
    """Derive authority from trusted event provenance, never model-authored data."""
    try:
        source = int(source_guid or 0)
        master = int(master_guid or 0)
    except (TypeError, ValueError):
        return "autonomous"
    if event_type in OWNER_AUTHORITY_EVENT_TYPES and master > 0 and source == master:
        return "owner_command"
    return "autonomous"


def required_authority(action_name: str) -> str:
    """Return the server-enforced authority tier for a canonical action."""
    return "owner_command" if canonicalize(action_name) in OWNER_COMMAND_ACTIONS else "autonomous"


def authority_allows(action_name: str, granted: str) -> bool:
    required = required_authority(action_name)
    return required == "autonomous" or granted in ("owner_command", "gm_manual")


def params_json_schema(action_name: str) -> Dict[str, Any]:
    """Build the bootstrap JSON schema mirrored by the C++ catalogue export.

    Runtime planning consumes the C++-exported schema. This generator keeps the
    offline/healthcheck catalogue typed without introducing a second hand-written
    schema for every action.
    """
    entry = CATALOG.get(canonicalize(action_name))
    params_dsl = entry[1] if entry else ""
    properties: Dict[str, Any] = {}
    required: List[str] = []
    alternative_requirements: List[Dict[str, Any]] = []
    integer_keys = {
        "id", "quest_id", "questid", "guid", "spellid", "spell_id", "count", "slot",
        "seconds", "radius", "size", "distance", "gold", "silver", "copper",
    }
    number_keys = {"x", "y", "z", "range"}
    boolean_keys = {"all", "all_junk", "master_target", "override"}
    array_keys = {"add", "remove"}
    for raw_token in params_dsl.split(",") if params_dsl else []:
        token = raw_token.strip()
        optional = token.startswith("[") and token.endswith("]")
        if optional:
            token = token[1:-1]
        choices = [part.strip() for part in token.split("|") if part.strip()]
        real_choices = choices
        for key in real_choices:
            if key in integer_keys:
                properties[key] = {"type": "integer", "minimum": 0}
            elif key in number_keys:
                properties[key] = {"type": "number"}
            elif key in boolean_keys:
                properties[key] = {"type": "boolean"}
            elif key in array_keys:
                properties[key] = {"type": "array", "items": {"type": "string"}}
            else:
                properties[key] = {"type": "string"}
        if not optional and len(real_choices) == 1:
            required.append(real_choices[0])
        elif not optional and len(real_choices) > 1:
            alternative_requirements.append({
                "anyOf": [{"required": [choice]} for choice in real_choices]
            })
    schema: Dict[str, Any] = {"type": "object", "properties": properties, "additionalProperties": False}
    if required:
        schema["required"] = required
    if alternative_requirements:
        schema["allOf"] = alternative_requirements
    return schema

# Semantic domain index mapping
# The model regularly tries to report goal progress as an action. Progress belongs in
# the planner's top-level `goal_update` object, never in the plan, so these pseudo
# actions are refused by name instead of being dispatched as unsupported native
# playerbots actions (which produced a steady stream of failed action rows).
PSEUDO_PLAYERBOT_ACTIONS = {
    "goal_update", "goal_update_continue", "goal_update_complete", "goal_update_blocked",
    "update_goal", "set_goal", "progress_update", "goal_progress",
}

COMMAND_INDEX: Dict[str, List[str]] = {
    "movement": [
        "move_to", "go_to", "follow", "stay", "stop", "mount", "dismount", "flee",
        "runaway", "grind", "wander", "travel_to", "taxi", "summon", "disperse", "disperse_disable",
    ],
    "combat": [
        "attack", "attack_my_target", "assist", "aoe", "pull", "pull_back", "wait_for_attack",
        "cc", "focus", "threat", "boost", "tank_face", "behind", "mark_rti",
        "pet_attack", "cast", "cast_on", "spell_exclude", "use_trinket", "racial",
    ],
    "strategy": [
        "strategy", "set_action_mode", "pet_summon", "soulstone",
    ],
    "quest": [
        "talk_to", "interact", "use_object", "accept_quest", "accept_all_quests",
        "turn_in_quest", "choose_reward", "share_quest", "drop_quest", "quest_summary",
        "trainer", "trainer_learn", "talents",
    ],
    "rpg": [
        "rpg_status", "rpg_do_quest",
    ],
    "loot": [
        "loot", "loot_all", "loot_nearest", "gather", "open_loot", "loot_filter",
    ],
    "economy": [
        "sell", "buy", "repair", "bank", "guild_bank", "mail", "send_mail", "craft", "equip", "unequip",
        "equip_upgrades", "open_items", "use_item", "use_item_on", "destroy_item",
        "give_gold", "outfit", "maintenance",
    ],
    "survival": [
        "food", "drink", "eat_drink", "tavern_rest", "campfire_cook", "healing_potion", "mana_potion", "healthstone",
        "hearthstone", "revive", "release",
    ],
    "social": [
        "emote", "say", "greet", "invite", "leave_group", "give_leader", "lfg", "ready",
        "duel_start", "duel_accept", "duel_decline", "attack_duel_opponent",
        "trade", "trade_start", "trade_accept", "trade_cancel",
        "trade_set_item", "trade_clear_item", "trade_set_gold", "trade_link_items",
        "inspect", "target", "pvp", "roll",
    ],
    "meta": [
        "playerbot_action", "playerbot_command", "wait",
    ],
}

# Frequency-weighted Slash Command Index (from docs/Slash commands.md)
# Ranked into 3 tiers by daily companion usage frequency for cognitive prioritization.
SLASH_COMMAND_INDEX: Dict[str, List[str]] = {
    "tier_1_daily_utility": [
        "trade", "trade_set_item", "trade_clear_item", "trade_set_gold", "trade_link_items",
        "follow", "attack", "assist", "duel_start", "inspect", "target", "say", "emote", "invite", "loot", "cast",
    ],
    "tier_2_tactical_group": [
        "dismount", "mount", "equip", "use_item", "roll", "ready", "give_leader", "leave_group", "pvp", "flee", "stay",
    ],
    "tier_3_economy_quests": [
        "repair", "buy", "sell", "bank", "guild_bank", "trainer_learn", "accept_quest", "turn_in_quest",
    ],
}

# Legacy / conversational / playerbot chat command aliases
ALIASES: Dict[str, str] = {
    # General shortcuts
    "duel": "duel_start",
    "challenge": "duel_start",
    "trade_with": "trade",
    "inspect_player": "inspect",
    "set_target": "target",
    "talk": "talk_to",
    "speak": "say",
    "go": "go_to",
    "goto": "go_to",
    "move": "move_to",
    "walk": "move_to",
    "flight": "taxi",
    "vendor": "sell",
    "rest": "eat_drink",
    "recover": "eat_drink",
    "looting": "loot",
    "loot_corpse": "loot",
    "attack_target": "attack",
    "kill": "attack",
    "engage": "attack",
    "do_attack_my_target": "attack_my_target",
    "do_attack": "attack_my_target",
    "hold": "stay",
    "dismiss": "stay",
    "target": "attack",
    "heal_self": "healing_potion",
    "drink_potion": "mana_potion",
    "leave": "leave_group",

    # Quest / RPG aliases
    "quest": "accept_all_quests",
    "quests": "quest_summary",
    "quests_all": "quest_summary",
    "quest_accept": "accept_quest",
    "quest_turn_in": "turn_in_quest",
    "quest_turnin": "turn_in_quest",
    "r": "choose_reward",
    "reward": "choose_reward",
    "rpg_status_do_quest": "rpg_do_quest",

    # Loot & Inventory aliases
    "ll": "loot_filter",
    "lootfilter": "loot_filter",
    "open_item": "open_items",
    "open_satchel": "open_items",
    "u": "use_item",
    "use": "use_item",
    "ue": "unequip",
    "destroy": "destroy_item",
    "gold": "give_gold",
    "gb": "guild_bank",
    "sendmail": "send_mail",

    # Combat & Strategy aliases
    "pullback": "pull_back",
    "waitforattack": "wait_for_attack",
    "markrti": "mark_rti",
    "ss": "spell_exclude",
    "learn": "trainer_learn",
    "pet": "pet_summon",

    # Party / Movement aliases
    "disperse_set": "disperse",

    # Category-prefixed aliases emitted by LLM (MAF-004)
    "trade_item": "trade_set_item",
    "trade_put_item": "trade_set_item",
    "trade_add_item": "trade_set_item",
    "trade_remove_item": "trade_clear_item",
    "trade_gold": "trade_set_gold",
    "trade_set_money": "trade_set_gold",
    "trade_money": "trade_set_gold",
    "link_items": "trade_link_items",
    "link_inventory": "trade_link_items",
    "social_trade_set_item": "trade_set_item",
    "social_trade_clear_item": "trade_clear_item",
    "social_trade_set_gold": "trade_set_gold",
    "social_trade_link_items": "trade_link_items",
    "social_trade_accept": "trade_accept",
    "social_trade_cancel": "trade_cancel",
    "social_duel_accept": "duel_accept",
    "social_duel_decline": "duel_decline",
    "social_duel_refuse": "duel_decline",
    "social_duel_start": "duel_start",
    "social_emote": "emote",
    "social_say": "say",
    "social_greet": "greet",
    "social_invite": "invite",
    "social_leave_group": "leave_group",
    "social_give_leader": "give_leader",
    "social_lfg": "lfg",
    "social_ready": "ready",
    "social_roll": "roll",
    "combat_attack": "attack",
    "combat_attack_my_target": "attack_my_target",
    "combat_assist": "assist",
    "combat_aoe": "aoe",
    "combat_pull": "pull",
    "combat_pull_back": "pull_back",
    "combat_wait_for_attack": "wait_for_attack",
    "combat_mark_rti": "mark_rti",
    "combat_behind": "behind",
    "combat_tank_face": "tank_face",
    "combat_focus": "focus",
    "combat_threat": "threat",
    "combat_boost": "boost",
    "combat_cc": "cc",
    "combat_cast": "cast",
    "combat_cast_on": "cast_on",
    "combat_spell_exclude": "spell_exclude",
    "combat_pet_attack": "pet_attack",
    "strategy_strategy": "strategy",
    "strategy_pet_summon": "pet_summon",
    "strategy_soulstone": "soulstone",
    "movement_move_to": "move_to",
    "movement_go_to": "go_to",
    "movement_follow": "follow",
    "movement_stay": "stay",
    "movement_stop": "stay",
    "movement_mount": "mount",
    "movement_dismount": "dismount",
    "movement_flee": "flee",
    "movement_runaway": "runaway",
    "movement_grind": "grind",
    "movement_wander": "wander",
    "movement_travel_to": "travel_to",
    "movement_summon": "summon",
    "movement_disperse": "disperse",
    "movement_disperse_disable": "disperse_disable",
    "quest_talk_to": "talk_to",
    "quest_interact": "interact",
    "quest_use_object": "use_object",
    "quest_accept_quest": "accept_quest",
    "quest_accept_all_quests": "accept_all_quests",
    "quest_turn_in_quest": "turn_in_quest",
    "quest_choose_reward": "choose_reward",
    "quest_share_quest": "share_quest",
    "quest_drop_quest": "drop_quest",
    "quest_quest_summary": "quest_summary",
    "quest_trainer": "trainer",
    "quest_trainer_learn": "trainer_learn",
    "quest_talents": "talents",
    "rpg_rpg_status": "rpg_status",
    "rpg_rpg_do_quest": "rpg_do_quest",
    "loot_loot": "loot",
    "loot_loot_all": "loot_all",
    "loot_loot_nearest": "loot_nearest",
    "loot_gather": "gather",
    "loot_open_loot": "open_loot",
    "loot_loot_filter": "loot_filter",
    "economy_sell": "sell",
    "economy_buy": "buy",
    "economy_repair": "repair",
    "economy_bank": "bank",
    "economy_guild_bank": "guild_bank",
    "economy_mail": "mail",
    "economy_equip": "equip",
    "economy_unequip": "unequip",
    "economy_equip_upgrades": "equip_upgrades",
    "economy_open_items": "open_items",
    "economy_use_item": "use_item",
    "economy_use_item_on": "use_item_on",
    "economy_destroy_item": "destroy_item",
    "economy_give_gold": "give_gold",
    "economy_outfit": "outfit",
    "economy_maintenance": "maintenance",
    "survival_food": "food",
    "survival_drink": "drink",
    "survival_eat_drink": "eat_drink",
    "survival_healing_potion": "healing_potion",
    "survival_mana_potion": "mana_potion",
    "survival_healthstone": "healthstone",
    "survival_hearthstone": "hearthstone",
    "survival_revive": "revive",
    "survival_release": "release",
}

VALID_EMOTES = {
    "wave", "nod", "no", "point", "cheer", "bow", "salute", "sit", "laugh", "cower",
    "stealth", "dance", "kneel", "clap", "shy", "cry", "hello", "thank", "wait",
    "charge", "roar", "flex", "shrug", "talk", "question", "exclamation",
}


def canonicalize(action_name: str) -> str:
    """Resolve aliases and normalise an action name."""
    name = (action_name or "").strip().lower().replace(" ", "_")
    if name in CATALOG:
        return name
    if name in ALIASES:
        return ALIASES[name]
    # Strip known category prefixes if present (e.g. "social_trade_accept" -> "trade_accept")
    for cat in ("social_", "combat_", "strategy_", "movement_", "quest_", "rpg_", "loot_", "economy_", "survival_", "meta_"):
        if name.startswith(cat):
            trimmed = name[len(cat):]
            if trimmed in CATALOG:
                return trimmed
            if trimmed in ALIASES:
                return ALIASES[trimmed]
    return name


def is_valid(action_name: str) -> bool:
    return canonicalize(action_name) in CATALOG


def is_passthrough(action_name: str) -> bool:
    entry = CATALOG.get(canonicalize(action_name))
    return bool(entry and entry[3])


def get_domain_actions(category: str) -> List[str]:
    return COMMAND_INDEX.get(category.lower(), [])


def _as_float(value: Any) -> Optional[float]:
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _as_int(value: Any) -> Optional[int]:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def validate_step(step: Dict[str, Any]) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    """Return (normalised_step, error). Never throws."""
    if not isinstance(step, dict):
        return None, "step is not an object"

    name = canonicalize(str(step.get("action", "")))
    if name not in CATALOG:
        return None, f"unknown action '{step.get('action')}'"

    raw_params = step.get("params")
    params: Dict[str, Any] = dict(raw_params) if isinstance(raw_params, dict) else {}
    if not isinstance(raw_params, dict) and raw_params not in (None, "", []):
        return None, f"params for '{name}' must be an object"

    params.pop("reason", None)

    def require_number(key: str):
        value = _as_float(params.get(key))
        if value is None:
            return None
        params[key] = value
        return value

    def require_int(key: str):
        value = _as_int(params.get(key))
        if value is None or value <= 0:
            return None
        params[key] = value
        return value

    # ------------------------------------------------------------------ Movement
    if name == "move_to":
        x, y, z = require_number("x"), require_number("y"), require_number("z")
        if x is None or y is None or z is None:
            return None, "move_to requires numeric x, y and z"
        if params.get("range") is not None:
            params["range"] = _as_float(params.get("range")) or 2.0

    elif name in ("go_to", "interact", "use_object", "talk_to"):
        guid = require_int("guid")
        target_name = str(params.get("name", "")).strip()
        if guid is None and not target_name:
            return None, f"{name} requires a guid or name from the surroundings list"
        if guid is not None:
            params["guid"] = guid
        if target_name:
            params["name"] = target_name
        if params.get("range") is not None:
            params["range"] = _as_float(params.get("range")) or 3.0

    elif name in ("travel_to", "taxi"):
        destination = str(params.get("destination", params.get("param", ""))).strip()
        if not destination:
            return None, f"{name} requires a destination"
        params["destination"] = destination

    elif name == "disperse":
        dist = _as_int(params.get("distance", params.get("x", 10))) or 10
        params["distance"] = max(1, min(100, dist))

    # -------------------------------------------------------------------- Combat
    elif name == "attack":
        guid = require_int("guid")
        target_name = str(params.get("name", "")).strip()
        if guid is not None:
            params["guid"] = guid
        if target_name:
            params["name"] = target_name
        if guid is None and not target_name:
            params["master_target"] = True

    elif name == "attack_my_target":
        params["master_target"] = True
        params["override"] = True

    elif name == "wait_for_attack":
        secs = _as_int(params.get("seconds", params.get("time", 5))) or 5
        params["seconds"] = max(1, min(60, secs))

    elif name == "cast":
        spell_id = require_int("spellid") or require_int("spell_id")
        spell_name = str(params.get("spell", params.get("name", ""))).strip()
        params.setdefault('spell_reference', spell_name or spell_id)
        if spell_id is None and not spell_name:
            return None, "cast requires a spellid or spell name"

        try:
            from friend_spells import SpellDatabaseResolver
            resolver = SpellDatabaseResolver.get_instance()
            if spell_id is None and spell_name:
                resolved = resolver.resolve(spell_name)
                if resolved:
                    spell_id = resolved.get("spell_id")
                    spell_name = resolved.get("name") or spell_name
            elif spell_id is not None and not spell_name:
                resolved = resolver.resolve(spell_id)
                if resolved:
                    spell_name = resolved.get("name", "")
        except Exception:
            pass

        if spell_id is not None:
            params["spellid"] = spell_id
        if spell_name:
            params["spell"] = spell_name
        guid = require_int("guid")
        if guid is not None:
            params["guid"] = guid

    elif name == "cast_on":
        spell = str(params.get("spell", params.get("spell_name", ""))).strip()
        params.setdefault('spell_reference', spell)
        target = str(params.get("target", params.get("player", ""))).strip()
        if not spell or not target:
            return None, "cast_on requires both spell and target"
        try:
            from friend_spells import SpellDatabaseResolver
            resolved = SpellDatabaseResolver.get_instance().resolve(spell)
            if resolved:
                params["spellid"] = resolved.get("spell_id")
                params["spell"] = resolved.get("name") or spell
            else:
                params["spell"] = spell
        except Exception:
            params["spell"] = spell
        params["target"] = target

    elif name == "spell_exclude":
        action = str(params.get("action", "add")).strip().lower()
        params["action"] = action if action in ("add", "remove", "reset") else "add"
        if params["action"] in ("add", "remove"):
            spell_id = require_int("spellid") or require_int("spell_id") or require_int("id")
            spell_name = str(params.get("spell", params.get("name", ""))).strip()
            clean_lookup = re.sub(r"^(?:ss\s*)?[\+\-]\s*", "", spell_name, flags=re.IGNORECASE).strip()
            if spell_id is None and clean_lookup:
                try:
                    from friend_spells import SpellDatabaseResolver
                    resolved = SpellDatabaseResolver.get_instance().resolve(clean_lookup)
                    if resolved:
                        spell_id = resolved.get("spell_id")
                        params["spell"] = resolved.get("name") or clean_lookup
                except Exception:
                    pass
            if spell_id is not None:
                params["spellid"] = spell_id
            else:
                return None, "spell_exclude add/remove requires a valid spell id or resolvable spell name"

    # ------------------------------------------------------------------ Strategy
    elif name == "strategy":
        spec = str(params.get("spec", "")).strip()
        adds = params.get("add") or []
        removes = params.get("remove") or []
        if not spec and not adds and not removes:
            return None, "strategy requires spec, add or remove"
        if isinstance(adds, str):
            adds = [adds]
        if isinstance(removes, str):
            removes = [removes]
        params["add"] = [str(a) for a in adds]
        params["remove"] = [str(r) for r in removes]
        if spec:
            params["spec"] = spec
        if str(params.get("state", "")).lower() == "combat":
            params["state"] = "combat"
        else:
            params.pop("state", None)

    elif name == "pet_summon":
        pet = str(params.get("pet", params.get("name", ""))).strip().lower()
        if not pet:
            return None, "pet_summon requires pet name (imp, voidwalker, succubus, felhunter, felguard)"
        params["pet"] = pet

    elif name == "soulstone":
        target = str(params.get("target", "master")).strip().lower()
        params["target"] = target if target in ("master", "self", "tank", "healer") else "master"

    elif name == "set_action_mode":
        mode = str(params.get("mode", "")).strip().lower()
        if mode == "grind": mode = "combat"
        elif mode == "follow": mode = "travel"
        elif mode in ("stay", "rest"): mode = "idle"
        elif mode == "rpg": mode = "social"
        if mode not in ("combat", "travel", "idle", "social"):
            return None, f"set_action_mode requires valid mode (combat, travel, idle, social), got '{mode}'"
        params["mode"] = mode

    # -------------------------------------------------------------- Quests & RPG
    elif name in ("accept_quest", "turn_in_quest", "share_quest", "drop_quest", "rpg_do_quest"):
        quest_id = require_int("id") or require_int("quest_id") or require_int("questid")
        if quest_id is None:
            return None, f"{name} requires a numeric quest id"
        params["id"] = quest_id

    elif name == "choose_reward":
        item = str(params.get("item", params.get("reward", ""))).strip()
        if not item:
            return None, "choose_reward requires item link or item name"
        params["item"] = item

    elif name == "rpg_status":
        state = str(params.get("state", params.get("status", "idle"))).strip().lower()
        params["state"] = state
        q_id = require_int("id") or require_int("quest_id")
        if q_id is not None:
            params["id"] = q_id

    # ---------------------------------------------------------- Loot & Gathering
    elif name == "loot_nearest":
        if params.get("radius") is not None:
            params["radius"] = _as_float(params.get("radius")) or 20.0

    elif name == "loot_filter":
        filt = str(params.get("filter", params.get("mode", "all"))).strip().lower()
        params["filter"] = filt
        item = str(params.get("item", "")).strip()
        if item:
            params["item"] = item

    # ------------------------------------------------------------ Economy & Gear
    elif name == "buy":
        item = str(params.get("item", "")).strip()
        if not item:
            return None, "buy requires an item"
        params["item"] = item
        params["count"] = _as_int(params.get("count")) or 1

    elif name == "sell":
        item = str(params.get("item", "")).strip()
        if item:
            params["item"] = item
        else:
            params["all_junk"] = True

    elif name in ("equip", "unequip", "use_item", "destroy_item"):
        item = str(params.get("item", params.get("name", ""))).strip()
        if not item:
            return None, f"{name} requires an item"
        params["item"] = item

    elif name == "use_item_on":
        item = str(params.get("item", "")).strip()
        target = str(params.get("target", "")).strip()
        if not item or not target:
            return None, "use_item_on requires both item and target"
        params["item"] = item
        params["target"] = target

    elif name == "give_gold":
        gold = _as_int(params.get("gold", 0)) or 0
        silver = _as_int(params.get("silver", 0)) or 0
        copper = _as_int(params.get("copper", 0)) or 0
        if gold <= 0 and silver <= 0 and copper <= 0:
            gold = 1
        params["gold"] = gold
        params["silver"] = silver
        params["copper"] = copper

    elif name == "guild_bank":
        action = str(params.get("action", "deposit")).strip().lower()
        item = str(params.get("item", "")).strip()
        if not item:
            return None, "guild_bank requires an item"
        params["action"] = action
        params["item"] = item

    elif name == "outfit":
        outfit_name = str(params.get("name", "")).strip()
        action = str(params.get("action", "equip")).strip().lower()
        if not outfit_name and action not in ("list", "help", "?"):
            return None, "outfit requires outfit name"
        params["name"] = outfit_name
        params["action"] = action
        item = str(params.get("item", "")).strip()
        if item:
            params["item"] = item

    elif name == "send_mail":
        item = str(params.get("item", "")).strip()
        money = str(params.get("money", "")).strip()
        if not item and not money:
            return None, "send_mail requires one item or a money amount"
        if item:
            params["item"] = item
            params.pop("money", None)
        else:
            params["money"] = money

    elif name == "craft":
        item = str(params.get("item", params.get("id", ""))).strip()
        if not item:
            return None, "craft requires an item id or item link"
        params["item"] = item
        if params.get("count") is not None:
            params["count"] = max(1, _as_int(params.get("count")) or 1)

    # -------------------------------------------------------------------- Social
    elif name == "emote":
        emote = str(params.get("emote", "nod")).strip().lower()
        params["emote"] = emote if emote in VALID_EMOTES else "nod"

    elif name == "say":
        text = str(params.get("text", "")).strip()
        if not text:
            return None, "say requires text"
        params["text"] = text
        channel = str(params.get("channel", "")).strip().lower()
        if channel:
            params["channel"] = channel

    elif name == "lfg":
        size = _as_int(params.get("size"))
        if size in (5, 10, 20, 25, 40):
            params["size"] = size

    elif name == "duel_start":
        guid = require_int("guid")
        target_name = str(params.get("name", "")).strip()
        if guid is None and not target_name:
            return None, "duel_start requires a guid or name"
        if guid is not None:
            params["guid"] = guid
        if target_name:
            params["name"] = target_name

    elif name == "trade_set_item":
        item = str(params.get("item", params.get("name", ""))).strip()
        if not item:
            return None, "trade_set_item requires an item"
        params["item"] = item
        if params.get("count") is not None:
            params["count"] = max(1, _as_int(params.get("count")) or 1)
        if params.get("slot") is not None:
            slot = _as_int(params.get("slot"))
            if slot is not None and 0 <= slot <= 5:
                params["slot"] = slot

    elif name == "trade_clear_item":
        if params.get("slot") is not None:
            slot = _as_int(params.get("slot"))
            if slot is not None and 0 <= slot <= 5:
                params["slot"] = slot
        item = str(params.get("item", params.get("name", ""))).strip()
        if item:
            params["item"] = item

    elif name == "trade_set_gold":
        copper = _as_int(params.get("copper"))
        gold_str = str(params.get("gold", params.get("money", ""))).strip()
        if copper is not None and copper >= 0:
            params["copper"] = copper
        elif gold_str:
            params["gold"] = gold_str
        else:
            params["gold"] = "0g"

    elif name == "trade_link_items":
        category = str(params.get("category", params.get("filter", "all"))).strip().lower()
        if not category:
            category = "all"
        params["category"] = category

    # ---------------------------------------------------------------------- Meta
    elif name == "playerbot_action":
        native = str(params.get("action", params.get("name", ""))).strip()
        if not native:
            return None, "playerbot_action requires an action name"
        if native.casefold() in PSEUDO_PLAYERBOT_ACTIONS:
            return None, ("pseudo_action:%s (goal progress belongs in goal_update, not in the plan)"
                          % native)
        params["action"] = native
        if params.get("param") is not None:
            params["param"] = str(params.get("param"))

    elif name == "playerbot_command":
        command = str(params.get("command", "")).strip()
        if not command:
            return None, "playerbot_command requires a command"
        params["command"] = command

    elif name == "wait":
        seconds = _as_int(params.get("seconds")) or 2
        params["seconds"] = max(1, min(60, seconds))

    # Passive/verification hints survive untouched.
    normalised: Dict[str, Any] = {"action": name, "params": params}
    if "expect" in step:
        normalised["expect"] = step["expect"]
    if "timeout" in step:
        try:
            normalised["timeout"] = max(1, min(60, int(step["timeout"])))
        except (TypeError, ValueError):
            pass

    return normalised, None


def normalize_plan(plan: Any, max_steps: int = 5, authority: str = "gm_manual",
                   allow_raw_passthrough: bool = True) -> Tuple[List[Dict[str, Any]], List[str]]:
    """Validate a whole plan. Returns (valid_steps, rejection_reasons)."""
    steps: List[Dict[str, Any]] = []
    rejects: List[str] = []

    if not isinstance(plan, list):
        return steps, ["plan is not a list"]

    HANDOFF_ACTIONS = {"follow", "grind", "wander", "travel_to", "taxi", "rpg_do_quest", "runaway", "set_action_mode"}

    for raw in plan:
        if len(steps) >= max_steps:
            rejects.append(f"plan truncated at {max_steps} steps")
            break

        step, error = validate_step(raw)
        if step is None:
            rejects.append(error or "invalid step")
            continue
        if not authority_allows(step["action"], authority):
            rejects.append("authority_denied:%s requires %s" %
                           (step["action"], required_authority(step["action"])))
            continue
        if step["action"] in ("playerbot_action", "playerbot_command") and not allow_raw_passthrough:
            rejects.append("raw_passthrough_disabled:%s" % step["action"])
            continue
        steps.append(step)

        if step["action"] in HANDOFF_ACTIONS:
            # Long-lived handoff actions take over bot control; subsequent steps would be superseded.
            break

    return steps, rejects


def prompt_catalog_text(authority: str = "autonomous", allow_raw_passthrough: bool = False) -> str:
    """Compact, token-friendly catalogue listing organized by semantic domain."""
    lines = []
    for domain, action_names in COMMAND_INDEX.items():
        formatted_actions = []
        for name in action_names:
            if name in ("playerbot_action", "playerbot_command") and not allow_raw_passthrough:
                continue
            entry = CATALOG.get(name)
            if entry and authority_allows(name, authority):
                params = entry[1]
                formatted_actions.append(f"{name}({params})" if params else name)
            elif not entry:
                formatted_actions.append(name)
        if formatted_actions:
            lines.append(f"- {domain.upper()}: " + ", ".join(formatted_actions))
    return "\n".join(lines)


# Which semantic domains matter for a given trigger. Sending the whole live
# playerbots catalogue (1,000+ actions) on every call was costing ~53k prompt
# tokens and driving endpoint rate limits, so the live view is scoped per event.
EVENT_DOMAINS: Dict[str, tuple] = {
    "combat_enter": ("combat", "movement", "strategy", "meta"),
    "enemy_attacked": ("combat", "movement", "meta"),
    "health_critical": ("combat", "rest", "meta"),
    "loot_available": ("loot", "movement", "meta"),
    "idle_tick": ("movement", "quest", "loot", "social", "meta"),
    "plan_interrupted": ("movement", "combat", "rest", "meta"),
    "dialogue_heard": ("social", "meta"),
    "trade_requested": ("social", "meta"),
    "duel_requested": ("combat", "social", "meta"),
    "quest_event": ("quest", "movement", "meta"),
}

_DEFAULT_DOMAINS = ("movement", "combat", "quest", "social", "meta")
_CURATED_BY_DOMAIN: Dict[str, set] = {
    domain: set(names) for domain, names in COMMAND_INDEX.items()
}


def live_catalog_parts(rows, event_type: str, max_native: int = 90,
                       granted_authority: Optional[str] = None,
                       allow_raw_passthrough: bool = False) -> Tuple[str, str]:
    """Return (curated_schema_text, native_actions_text) for a trigger.

    Curated actions are listed by domain; native playerbots actions/strategies are
    listed as bare names (capped) with a note that any of them can be invoked by
    name through playerbot_action. Owner commands see the widest selection because
    the request itself can be about anything.

    Splitting the two lets the context builder keep the curated (safe) schemas in
    the mandatory block and treat the large native list as a lazily trimmed block.
    """
    domains = EVENT_DOMAINS.get(event_type, _DEFAULT_DOMAINS)
    granted_authority = granted_authority or "autonomous"
    if event_type == "player_command":
        domains = tuple(COMMAND_INDEX.keys())

    wanted: set = set()
    for domain in domains:
        wanted |= _CURATED_BY_DOMAIN.get(domain, set())

    curated, native = [], []
    for row in rows or []:
        name = str(row.get("action_name") or "").strip()
        if not name:
            continue
        source = str(row.get("source") or "curated")
        params = str(row.get("params_schema") or "").strip()
        required = str(row.get("authority") or required_authority(name))
        if name in ("playerbot_action", "playerbot_command") and not allow_raw_passthrough:
            continue
        if required == "owner_command" and granted_authority != "owner_command":
            continue
        if source == "curated" and name in wanted:
            curated.append(f"{name}({params})" if params else name)
        elif (allow_raw_passthrough and event_type == "player_command" and
              granted_authority in ("owner_command", "gm_manual") and
              source in ("playerbots", "strategy", "command")):
            native.append(name)

    lines = []
    if curated:
        lines.append("- ALLOWED ACTIONS: " + ", ".join(sorted(set(curated))))
    native_lines = []
    if native:
        native = sorted(set(native))
        shown = native[:max_native]
        native_lines.append("- NATIVE PLAYERBOTS (call with playerbot_action{action:NAME}): " + ", ".join(shown))
        if len(native) > len(shown):
            native_lines.append(f"- ...and {len(native) - len(shown)} more native actions, available by name.")
    return "\n".join(lines), "\n".join(native_lines)


def live_catalog_for_event(rows, event_type: str, max_native: int = 90,
                           granted_authority: Optional[str] = None,
                           allow_raw_passthrough: bool = False) -> str:
    """Compact, event-scoped rendering of the exported playerbots catalogue."""
    curated, native = live_catalog_parts(rows, event_type, max_native=max_native,
                                         granted_authority=granted_authority,
                                         allow_raw_passthrough=allow_raw_passthrough)
    return "\n".join(part for part in (curated, native) if part)


def describe(action_name: str) -> str:
    entry = CATALOG.get(canonicalize(action_name))
    if not entry:
        return ""
    return f"{canonicalize(action_name)} ({entry[1]}): {entry[2]}"
