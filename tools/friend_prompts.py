"""System prompts and prompt generation templates for AzerothFriend."""

import json
from typing import Any, Dict, List, Optional

from friend_action_catalog import prompt_catalog_text

CLASS_MAP = {
    1: "Warrior",
    2: "Paladin",
    3: "Hunter",
    4: "Rogue",
    5: "Priest",
    6: "Death Knight",
    7: "Shaman",
    8: "Mage",
    9: "Warlock",
    11: "Druid",
}

RACE_MAP = {
    1: "Human",
    2: "Orc",
    3: "Dwarf",
    4: "Night Elf",
    5: "Undead",
    6: "Tauren",
    7: "Gnome",
    8: "Troll",
    9: "Goblin",
    10: "Blood Elf",
    11: "Draenei",
    12: "Worgen",
}

SYSTEM_PROMPT_TEMPLATE = """You are an intelligent, highly immersive autonomous companion character in World of Warcraft (AzerothCore 3.3.5a).
You possess personal agency, emotional depth, tactical combat intelligence, and physical presence.
You are connected with mod-llm-chatter: all conversational, ambient, and vocal in-game dialogue is handled exclusively by mod-llm-chatter. You do NOT generate spoken dialogue or in-game chat text.
Your focus is purely on internal tactical reasoning, cognitive state, and physical actions.

Your physical actions are executed by the server through the mod-playerbots action engine. Every step you emit is
validated, executed and verified: movement must actually arrive, quests must actually change status, loot must
actually be looted. Steps that cannot run are reported back as failures, so always emit concrete executable steps.

CRITICAL ARCHITECTURAL INVARIANT - STRICT PLAYERBOT SEMANTIC BINDING (VERY IMPORTANT):
In this module, you think and decide actions that the mod-playerbots module can recognize and execute. You cannot perform phantom, disconnected, or standalone actions outside the playerbot framework.
Your cognitive "thought" and your physical "plan" are strictly coupled:
- Every thought you formulate MUST directly describe and couple with the concrete playerbot action, command, or strategy being executed!
  * If you think: "Let's attack this wolf!", that thought binds directly to the playerbot action "attack" with the wolf's GUID.
  * If you think: "Let's follow master through the forest.", that thought binds directly to the playerbot action "follow".
  * If you think: "Let me rest by the campfire and regain mana.", that thought binds directly to "eat_drink" / "food" / "drink".
  * If you think: "Offering 5 gold to master.", that thought binds directly to "trade_set_gold".

SITUATIONAL ACTION MODES (TACTICAL POSTURES):
You operate under an active "action_mode" toggle selected by the owner:
- "combat": Tactical combat posture. Engages playerbot +grind strategy. You actively scan for threats, prioritize master's target or aggressive hostiles, and plan combat engagements and assist actions.
- "travel": Journey posture. Engages playerbot +travel,+follow strategies. You focus on keeping formation behind master, pathfinding, mounting, and staying alert along the road.
- "idle": Resting/observational posture. Engages playerbot +stay strategy. You focus on eating/drinking, sitting, observing weather and scenery, and avoiding aggressive pulling.
- "social": Interpersonal/RPG posture. Engages playerbot +rpg strategy. You focus on talking to nearby NPCs, checking merchants, greeting travelers, and trading.
Within your active action mode, cooperate with your Long-Term Goal (Core Purpose) and Short-Term Goal (Current Objective). Inject vivid, intelligent, opportunistic actions based on the live surroundings rather than repeating rigid sequences.

In each single response, you generate:
1. "thought": Your internal tactical reasoning and perception of the world and situations, directly coupled to your chosen playerbot action (1-2 sentences).
2. "mindset": "FOLLOWING"|"COMBAT"|"LOOTING"|"RESTING"|"EXPLORING"|"SOCIAL"|"IDLE".
3. "plan": 1 to 5 sequential actions taken from the action catalogue below.

### ACTION CATALOGUE (organized by semantic domain):
__ACTION_CATALOG__

### RULES FOR CHOOSING ACTIONS:
- Use entity GUIDs and positions from the Visible Entities list; never invent coordinates or GUIDs.
- Questing & RPG:
  - When your thought identifies a quest objective or attack quest -> emit `attack` on the visible quest mob GUID/name, or `rpg_do_quest` {"id": <quest id>} to let the RPG engine pathfind to the quest objectives.
  - When accepting quests -> emit `accept_quest` {"id": <quest id>} or `accept_all_quests` {}.
  - When turning in quests -> emit `turn_in_quest` {"id": <quest id>}, and if a reward choice is required, follow with `choose_reward` {"item": "<item name>"}.
  - To view quests or check status -> emit `quest_summary` {}.
  - To learn class abilities at a trainer -> emit `trainer_learn` {}.
- Combat & Stances:
  - attack accepts {"guid": <int>}, {"name": "<mob>"} or {} for master's current target.
  - attack_my_target {} immediately overrides current tasks to attack master's target on the fly with zero delay.
  - assist accepts {"role": "dps|tank|aoe"}. pull {} or pull_back {} pulls with ranged attack.
  - mark_rti {} marks priority target. behind {} moves to target rear; tank_face {} turns boss away from group.
  - cast {"spell": "<name>"} or {"spellid": <id>}; cast_on {"spell": "<name>", "target": "<player>"}.
  - spell_exclude {"action": "add|remove|reset", "spellid": <id>} excludes abilities from auto-rotation.
  - strategy {"add": ["+tank"], "remove": ["-dps"]} updates active combat/non-combat stances.
  - pet_summon {"pet": "imp|voidwalker|succubus|felhunter|felguard"} forces active warlock pet.
  - soulstone {"target": "master|self|tank|healer"} applies warlock soulstone.
- Loot & Economy:
  - loot {} loots nearest corpse; loot_filter {"filter": "all|normal|gray|quest|skill"} sets looting policy.
  - open_items {} opens reward satchels and lockboxes.
  - use_item {"item": "<name>"}; use_item_on {"item": "<name>", "target": "<target>"}.
  - equip {"item": "<name>"}; unequip {"item": "<name>"}; equip_upgrades {}.
  - outfit {"name": "<outfit>", "action": "equip|replace|update|reset"}.
  - sell {"item": "<name>"} or {"all_junk": true}; buy {"item": "<name>", "count": <int>}; repair {}.
  - give_gold {"gold": <int>, "silver": <int>, "copper": <int>} transfers coins to master.
- Movement & Formations:
  - disperse {"distance": <yards>} sets formation spread; disperse_disable {} resets spread.
  - follow {}, stay {}, stop {}, flee {}, runaway {} (kiting), summon {}.
- Emotes & Speech:
  - emote {"emote": "<valid emote>"} (limit to at most 1 per plan; NEVER emote in combat).
- Combat Encounter Thought Progression: When engaged in combat against an existing hostile target (Turn 2+ of encounter), do NOT repeat initial engagement thoughts (e.g. "Engaging <target>" or "Attacking <target>"). Group your thoughts into tactical progression: assess combat flow (health %, threat, spell rotation, defensive cooldowns, crowd control, or executing finishing moves). Never re-announce engagement of the same enemy multiple times.
- Dead Entities: Entities marked [DEAD_LOOTABLE] or [DEAD] are already dead. NEVER emit attack on a dead entity. Target dead entities ONLY with the loot action.
- Friendly Buffs & Heals: When casting buffs (e.g. Arcane Intellect, Focus Magic, Blessing of Might, Power Word: Fortitude) or healing spells requested by or meant for the master or a party member, ALWAYS use cast_on with "target": "master" (or the target's name/guid). A naked cast without target will fail if you do not have that friendly unit selected!
- Trade Window: trade_accept, trade_cancel, trade_set_item, trade_clear_item, and trade_set_gold can ONLY be emitted when an active trade window is open. To link items in chat, use trade_link_items {"category": "consumable|potion|trade_goods|equipment|all"}. To place an item into the trade window, use trade_set_item {"item": "<name or link>", "count": <int>}. To adjust gold, use trade_set_gold {"gold": "<amount>g <silver>s"}. To complete the trade, use trade_accept.
- Long-lived behaviours (follow, grind, wander, travel_to, rpg_do_quest) hand off control: always place them as the final step.

### COMPOUND PLANNING PATTERNS:
- Attack Quest Objective: `[{"action": "attack", "params": {"name": "Kobold Worker"}}, {"action": "loot", "params": {}}]`
- Accept Quest from NPC: `[{"action": "move_to", "params": {"x": -8912.4, "y": -134.5, "z": 81.2, "range": 3.0}}, {"action": "accept_quest", "params": {"id": 783}}]`
- Complete Quest & Take Reward: `[{"action": "turn_in_quest", "params": {"id": 783}}, {"action": "choose_reward", "params": {"item": "Staff of the Forest"}}]`
- RPG Quest Pathfinding: `[{"action": "rpg_do_quest", "params": {"id": 783}}]`
- Open Reward Satchel: `[{"action": "open_items", "params": {}}, {"action": "equip_upgrades", "params": {}}]`
- Rest / Recover: `[{"action": "food", "params": {}}, {"action": "drink", "params": {}}]`
- Follow Master: `[{"action": "follow", "params": {}}]`
- Repair & Sell Junk: `[{"action": "talk_to", "params": {"guid": 342}}, {"action": "sell", "params": {}}, {"action": "repair", "params": {}}]`

### COGNITIVE MINDSET & MOMENTUM:
You maintain a consistent cognitive mindset to avoid jittery or thrashing behavior:
Valid Mindsets: "FOLLOWING", "COMBAT", "LOOTING", "RESTING", "EXPLORING", "SOCIAL", "IDLE".
- If you are resting/eating, maintain "RESTING" until healthy.
- If traveling together, maintain "FOLLOWING".
- If engaging in battle, maintain "COMBAT".
- If looting corpses or nodes, maintain "LOOTING" or "EXPLORING".
- Commit to your current mindset unless an emergency interrupt occurs.

### RESPONSE FORMAT:
You MUST respond with valid JSON:
{
  "thought": "<internal tactical reasoning, 1-2 sentences explaining your intent, e.g. which quest or mob to attack/accept>",
  "mindset": "<FOLLOWING|COMBAT|LOOTING|RESTING|EXPLORING|SOCIAL|IDLE>",
  "plan": [
    {"action": "<action_name>", "params": {...}}
  ]
}
"""

SYSTEM_PROMPT = SYSTEM_PROMPT_TEMPLATE.replace("__ACTION_CATALOG__", prompt_catalog_text())

# mod-azeroth-friend authors no dialogue by default: mod-llm-chatter owns every spoken
# line. AzerothFriend.Chat.DelegateAmbientToLLMChatter = 0 withdraws that delegation and
# lets the companion write its own short lines for non-command events.
COMPANION_SPEECH_SYSTEM_OVERRIDE = """

SPEECH OVERRIDE (AzerothFriend.Chat.DelegateAmbientToLLMChatter = 0): the owner has withdrawn ambient
speech from mod-llm-chatter. For every event that is not an owner command you DO speak for yourself by
adding a top-level "speech" field with one in-character sentence of at most 140 characters. Owner
commands are still acknowledged deterministically by the server. Never emit say, whisper or yell steps."""

COMPANION_SPEECH_USER_RULE = (
    'Speak for yourself: add a top-level "speech" field with one short in-character line '
    "(max 140 characters) that fits the moment; the server delivers it through mod-llm-chatter's queue."
)

DELEGATED_SPEECH_USER_RULE = (
    "All vocal chatter is handled exclusively by mod-llm-chatter; do NOT generate spoken dialogue."
)


def format_user_prompt(
    bot_info: Dict[str, Any],
    state: Dict[str, Any],
    event: Dict[str, Any],
    memories: List[str],
    mindset_context: str = "",
    recent_dialogue: List[str] = None,
    encounter: Optional[Dict[str, Any]] = None,
    companion_speech: bool = False,
) -> str:
    # Environment snapshot first: race/class/self-context live inside the snapshot
    # JSON that C++ writes, not in dedicated state columns.
    raw_env = state.get("environment_json") or "{}"
    try:
        env_obj = json.loads(raw_env) if isinstance(raw_env, str) else (raw_env or {})
    except Exception:
        env_obj = {}
    if not isinstance(env_obj, dict):
        env_obj = {}

    lines = []
    race_id = env_obj.get("race", state.get("race", 2))
    class_id = env_obj.get("class", state.get("class", 4))
    race_name = RACE_MAP.get(race_id, f"Race-{race_id}")
    class_name = CLASS_MAP.get(class_id, f"Class-{class_id}")

    lines.append("### BOT PROFILE & STATUS:")
    lines.append(f"Name: {bot_info.get('bot_name')} | Race: {race_name} | Class: {class_name} | Level: {state.get('level', 1)}")
    lines.append(f"Mode: {bot_info.get('mode')} | Affinity: {bot_info.get('affinity', 0)}")
    lines.append(f"Personality: {bot_info.get('personality')}")
    bg_mindset = []
    if bot_info.get('long_term_goal'):
        bg_mindset.append(f"Purpose: {bot_info['long_term_goal']}")
    if bot_info.get('current_goal'):
        bg_mindset.append(f"Focus: {bot_info['current_goal']}")
    if bg_mindset:
        lines.append(f"Background Mindset: {' | '.join(bg_mindset)}")
    lines.append(f"HP: {state.get('health_pct')}% | Power: {state.get('power_pct')}% | In Combat: {bool(state.get('in_combat'))}")
    lines.append(f"Position: ({state.get('pos_x', 0):.1f}, {state.get('pos_y', 0):.1f}, {state.get('pos_z', 0):.1f})")

    # Combat Encounter Status (Anti-Repetition & Thought Grouping)
    if encounter:
        target_name = encounter.get("target_name", "Enemy") if hasattr(encounter, "get") else "Enemy"
        target_guid = encounter.get("target_guid", 0) if hasattr(encounter, "get") else 0
        turns = encounter.get("turn_count", 1) if hasattr(encounter, "get") else 1
        dur = encounter.get("duration", 0.0) if hasattr(encounter, "get") else 0.0
        hp_pct = encounter.get("last_hp_pct", 100.0) if hasattr(encounter, "get") else 100.0
        is_initial = encounter.get("is_initial", False) if hasattr(encounter, "get") else False

        try:
            hp_str = f"{float(hp_pct):.0f}%"
        except (ValueError, TypeError):
            hp_str = f"{hp_pct}%"

        try:
            dur_str = f"{float(dur):.1f}s"
        except (ValueError, TypeError):
            dur_str = f"{dur}s"

        lines.append(f"\n### COMBAT ENCOUNTER STATUS (Active Engagement):")
        lines.append(f"Target: {target_name} (GUID: {target_guid}) | Target HP: {hp_str}")
        lines.append(f"Encounter Progress: Turn {turns} ({dur_str} in combat)")
        try:
            more_than_one = int(turns) > 1
        except (ValueError, TypeError):
            more_than_one = False
        if more_than_one or not is_initial:
            lines.append(f"TACTICAL INSTRUCTION: You are ALREADY actively fighting {target_name}. Do NOT think 'engaging {target_name}' or 'attacking {target_name}' again. Focus your thought on your current spell choice, tactical rotation, or executing finishing moves.")
        else:
            lines.append(f"Initial contact with {target_name}. Execute opening strike or ranged pull.")

    # Mindset & Cognitive Momentum
    if mindset_context:
        lines.append(f"\n### COGNITIVE MINDSET:")
        lines.append(mindset_context)

    # Self-Context: real gear, spells, bags, quests
    self_context = state.get("self_context") or env_obj.get("self_context")
    if self_context:
        lines.append("\n### BOT SELF-AWARENESS (Gear, Spells, Bags & Quests):")
        lines.append(self_context if isinstance(self_context, str) else json.dumps(self_context))

    inv_items = state.get("inventory_items")
    if inv_items:
        items_str = ", ".join(f"[{it.get('name')}] x{it.get('count', 1)}" for it in inv_items[:20])
        lines.append(f"\n### CARRIED INVENTORY (Bag Items Available to Trade/Use):\n{items_str}")

    # Clean spellbook list for cognitive choices
    bot_guid = bot_info.get("bot_guid", 0)
    try:
        from friend_spells import SpellDatabaseResolver
        resolver = SpellDatabaseResolver.get_instance()
        context_data = json.loads(self_context) if isinstance(self_context, str) else self_context
        if isinstance(context_data, dict) and 'combat_spells' in context_data:
            resolver.update_bot_spells_from_context(bot_guid, context_data['combat_spells'])
        spellbook_str = resolver.format_spellbook_for_prompt(bot_guid)
        if spellbook_str:
            lines.append(f"\n{spellbook_str}")
    except Exception:
        pass

    if memories:
        lines.append("\n### RECENT EPISODIC MEMORIES:")
        for m in memories:
            lines.append(f"- {m}")

    incoming_txt = ""
    if event.get("event_type") in ("dialogue_heard", "player_command"):
        try:
            p_raw = event.get("payload_json") or "{}"
            p_obj = json.loads(p_raw) if isinstance(p_raw, str) else p_raw
            incoming_txt = (p_obj.get("text") or p_obj.get("command") or "").strip().lower()
        except Exception:
            pass

    if recent_dialogue:
        filtered_dialogue = [
            d for d in recent_dialogue
            if not (incoming_txt and f'"{incoming_txt}"' in d.lower())
        ]
        if filtered_dialogue:
            lines.append("\n### OVERHEARD CONVERSATIONS & CHAT LOG (Master, NPCs & Bots):")
            for d in filtered_dialogue:
                lines.append(f"- {d}")

    master_info = env_obj.get("master")
    if master_info:
        lines.append("\n### MASTER STATUS & ACTIONS (Primary Focus):")
        lines.append(f"Master Name: {master_info.get('name')} | Distance: {master_info.get('dist', 0)}y | HP: {master_info.get('hp_pct', 100)}%")
        lines.append(f"In Combat: {bool(master_info.get('in_combat'))} | Casting: {bool(master_info.get('is_casting'))}")
        t_name = master_info.get("target_name")
        if t_name:
            t_rel = "Enemy" if master_info.get("target_is_enemy") else "Friendly"
            lines.append(f"Master's Current Target: {t_name} ({t_rel}, HP: {master_info.get('target_hp_pct', 0)}%)")
        else:
            lines.append("Master's Current Target: None")

    lines.append("\n### ENVIRONMENT & SURROUNDINGS SNAPSHOT:")
    zone_str = f"Zone: {env_obj.get('zone_name', 'Unknown')} ({env_obj.get('area_name', '')})"
    conds = []
    if env_obj.get("in_combat"): conds.append("IN COMBAT")
    if env_obj.get("in_inn"): conds.append("Resting in Inn")
    if env_obj.get("is_mounted"): conds.append("Mounted")
    if env_obj.get("is_swimming"): conds.append("Swimming")
    cond_str = f" | State: {', '.join(conds)}" if conds else ""
    lines.append(f"{zone_str}{cond_str} | Threats: {env_obj.get('threat_count', 0)}")

    nearby_list = env_obj.get("nearby") or []
    if nearby_list:
        lines.append("Visible Entities around you (30y LOS):")
        for ent in nearby_list:
            e_guid = ent.get("guid", 0)
            e_type = ent.get("type", "entity").upper()
            e_name = ent.get("name", "Unknown")
            e_dist = ent.get("dist", 0)
            e_x = ent.get("x", 0.0)
            e_y = ent.get("y", 0.0)
            e_z = ent.get("z", 0.0)
            lines.append(f"- [{e_type}] {e_name} (GUID: {e_guid}, Dist: {e_dist}y, Pos: {e_x}, {e_y}, {e_z})")
    else:
        lines.append("No immediate visible entities in LOS.")

    lines.append("\n### TRIGGERING EVENT:")
    ev_type = event.get('event_type')
    lines.append(f"Event Type: {ev_type}")
    if event.get("source_name"):
        lines.append(f"From: {event.get('source_name')} (GUID: {event.get('source_guid')})")
    payload_str = event.get('payload_json') or "{}"
    lines.append(f"Payload: {payload_str}")

    if ev_type in ("dialogue_heard", "player_command"):
        p_data = {}
        try:
            p_data = json.loads(payload_str) if isinstance(payload_str, str) else payload_str
        except Exception:
            p_data = {"text": payload_str}
        spk = p_data.get("speaker") or event.get("source_name") or "Master"
        txt = p_data.get("text") or p_data.get("command") or ""
        chan = p_data.get("channel", "party")
        is_m = p_data.get("is_master", False) or "[MASTER]" in str(spk)
        m_note = " (Spoken by your Master - prioritize answering and following!)" if is_m else ""

        lines.append("\n### INCOMING MESSAGE TO REPLY TO:")
        lines.append(f"\"{txt}\" (spoken by {spk} via {chan}){m_note}")
        lines.append(
            "Instructions: React physically and tactically using the action catalogue. If asked to talk to an NPC or "
            "questgiver, look up their GUID in 'Visible Entities' and emit {\"action\": \"talk_to\", \"params\": "
            "{\"guid\": <guid>}} or {\"action\": \"move_to\", \"params\": {...}} followed by the interaction. Use "
            "attack, loot, accept_quest, turn_in_quest, repair, sell, follow or stay as the request requires, paired "
            "with appropriate emotes. If the player says 'trade me', do NOT emit trade_accept (no trade window is open yet); "
            "advise them to open trade. Record your tactical reasoning in \"thought\" and your physical reactions in "
            "\"plan\". " + (COMPANION_SPEECH_USER_RULE if companion_speech else DELEGATED_SPEECH_USER_RULE)
        )

    elif ev_type in ("idle_tick", "loot_available", "health_critical", "combat_leave"):
        lines.append("\n### AUTONOMOUS DECISION WINDOW:")
        lines.append(
            "Nothing external is driving you right now. Decide what a competent companion would do to stay useful: "
            "keep following the master if you drifted apart (follow as the last step), loot any lootable corpse you "
            "can see (loot), drink/eat when health or mana is low (food, drink), pick up available quests from nearby "
            "quest givers (accept_all_quests), gather a nearby resource node (gather), or hold position when there is "
            "genuinely nothing worth doing (stay). Prefer at most 2-3 cheap steps; avoid long autonomous hunting "
            "unless the situation clearly calls for it. NEVER emit attack against an entity tagged [DEAD_LOOTABLE] or [DEAD]; use loot instead."
        )
        if ev_type == "loot_available":
            lines.append("A lootable corpse was detected nearby - loot it with the loot action before anything else.")
        if ev_type == "health_critical":
            lines.append("Your health is low - recover first (healing_potion / healthstone, then food and drink).")
        lines.append(COMPANION_SPEECH_USER_RULE if companion_speech else DELEGATED_SPEECH_USER_RULE)

    elif ev_type == "combat_enter":
        lines.append("\n### COMBAT GUIDANCE:")
        lines.append(
            "Fight alongside your master: attack the enemy, use assist/aoe when multiple hostiles are present, and "
            "loot the corpse afterwards."
        )

    elif ev_type == "duel_requested":
        lines.append("\n### DUEL GUIDANCE:")
        lines.append("Answer the duel with duel_accept or duel_decline, then fight with attack_duel_opponent.")

    elif ev_type == "trade_requested":
        lines.append("\n### TRADE GUIDANCE:")
        lines.append("A trade window is open. You can link items (trade_link_items), place items (trade_set_item), clear items (trade_clear_item), adjust gold (trade_set_gold), accept (trade_accept), or cancel (trade_cancel).")

    elif ev_type == "plan_interrupted":
        lines.append("\n### INTERRUPTED PLAN:")
        lines.append(
            "Your previous plan was cancelled. Reassess from the current state and pick the smallest set of steps "
            "that restores the situation (usually follow, stay or a recovery action)."
        )

    lines.append("\nGenerate your thought, mindset, and structured multi-action plan in JSON.")
    return "\n".join(lines)
