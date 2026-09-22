"""Deterministic gates around the goal planner. No model calls here."""
import json
import re
import time


def select_bots(bots, names, limit=1):
    """An empty configuration selects nobody, including on event refresh."""
    ordered = list(dict.fromkeys(n.strip().casefold() for n in names if n.strip()))
    if limit > 0:
        ordered = ordered[:limit]
    by_name = {b['bot_name'].casefold(): b for b in bots}
    return {by_name[n]['bot_guid']: by_name[n] for n in ordered if n in by_name}


class GoalScheduler:
    def __init__(self, heartbeat=60, minimum=10):
        self.heartbeat = max(30, heartbeat)
        self.minimum = max(3, minimum)
        self.last = {}

    def should_plan(self, bot, state, event, history, now=None):
        if event.get('event_type') == 'player_command':
            return True
        if event.get('event_type') == 'dialogue_heard':
            return False
        if not bot.get('autonomy_enabled') or bot.get('goal_status') != 'active':
            return False
        if any(a['status'] in ('pending', 'in_progress') for a in history):
            return False
        if state.get('in_combat'):
            return False  # Tactical decisions belong to Playerbots.
        now = time.monotonic() if now is None else now
        environment = state.get('environment_json') or {}
        if isinstance(environment, str):
            environment = json.loads(environment)
        signature = json.dumps([
            bot.get('control_revision'), bot.get('current_goal'), state.get('level'),
            int(state.get('health_pct', 100)) // 20, state.get('map_id'),
            round(float(state.get('pos_x', 0)) / 20), round(float(state.get('pos_y', 0)) / 20),
            [(e.get('guid'), e.get('type')) for e in environment.get('nearby', [])],
            [(a['id'], a['status']) for a in history],
        ], sort_keys=True)
        previous, timestamp = self.last.get(bot['bot_guid'], (None, -float('inf')))
        if now - timestamp < self.minimum or (signature == previous and now - timestamp < self.heartbeat):
            return False
        self.last[bot['bot_guid']] = (signature, now)
        return True


def validate_goal_update(update, state, goal=''):
    """Completion needs a verifiable observation; prose alone is not evidence."""
    if not isinstance(update, dict):
        return None
    status = update.get('status', 'active')
    if status not in ('active', 'completed', 'blocked'):
        return None
    result = str(update.get('result', ''))[:1000]
    progress = str(update.get('progress', ''))[:1000]
    evidence = update.get('evidence') or {}
    if status == 'completed':
        kind = evidence.get('kind') if isinstance(evidence, dict) else None
        verified = False
        try:
            if kind == 'level':
                target = re.fullmatch(r'\s*(?:reach|get to)\s+level\s+(\d+)\s*[.!]?\s*', goal, re.I)
                verified = bool(target and int(evidence['value']) == int(target[1]) and
                                int(state.get('level', 0)) >= int(target[1]) > 0)
            elif kind == 'position':
                # Free-form destinations require owner confirmation until a trusted
                # destination contract can be captured when setting the goal.
                verified = False
        except (KeyError, TypeError, ValueError):
            pass
        if not verified:
            return {'status': 'active', 'progress': progress,
                    'result': 'Completion awaiting verifiable game state or owner confirmation.'}
    if status == 'blocked' and not result.strip():
        return None
    return {'status': status, 'progress': progress, 'result': result}


import logging
import random
from typing import Optional, Tuple, Dict, Any

logger = logging.getLogger("azeroth_friend.goals")

ABSTRACT_ARCHETYPES: Dict[str, Dict[str, Any]] = {
    "xp": {
        "theme": "Focus on combat progression, slaying creatures, and advancing character levels through adventuring.",
        "fallbacks": [
            ("Advance character combat power and gain a full experience level.", "Reach the maximum combat level and master class abilities."),
            ("Grind nearby hostile beasts and complete local quests for XP.", "Become a renowned veteran champion of our faction."),
        ]
    },
    "money": {
        "theme": "Focus on earning coin, harvesting trade goods, looting monsters, and accumulating wealth.",
        "fallbacks": [
            ("Loot valuable trade items and earn gold from vendor sales.", "Amass a personal fortune in gold to afford mounts and training."),
            ("Gather natural resources and sell junk to local merchants.", "Become an affluent trader with deep coin reserves."),
        ]
    },
    "loot": {
        "theme": "Focus on finding better gear, acquiring weapons and armor upgrades, and obtaining dungeon treasures.",
        "fallbacks": [
            ("Hunt for weapon and armor upgrades to improve combat gear.", "Equip a complete set of superior quality armor and enchanted weapons."),
            ("Defeat challenging enemies and bosses for rare equipment drops.", "Acquire the finest equipment available for our class specialization."),
        ]
    },
    "exploration": {
        "theme": "Focus on discovering new territories, clearing regional quest hubs, and mastering uncharted lands.",
        "fallbacks": [
            ("Explore surrounding zones, scouting map locations and quest outposts.", "Chart every corner of Azeroth and uncover forgotten territories."),
            ("Travel through adjacent regions and assist local settlements.", "Master all flight paths and regional territories across the continent."),
        ]
    }
}


def generate_starter_goals(
    bot_info: Dict[str, Any],
    categories_config: str = "mixed",
    llm_client: Optional[Any] = None
) -> Tuple[str, str]:
    """Generates an authentic World of Warcraft player goal pair (short_term, long_term) once.
    Uses abstract archetypes and queries the LLM if available; otherwise uses archetype fallbacks.
    """
    valid_categories = list(ABSTRACT_ARCHETYPES.keys())
    configured = [c.strip().lower() for c in (categories_config or "mixed").split(",") if c.strip()]
    if "mixed" in configured or "all" in configured or not configured:
        chosen_cat = random.choice(valid_categories)
    else:
        candidates = [c for c in configured if c in ABSTRACT_ARCHETYPES]
        chosen_cat = random.choice(candidates) if candidates else random.choice(valid_categories)

    archetype = ABSTRACT_ARCHETYPES[chosen_cat]
    theme = archetype["theme"]
    bot_name = bot_info.get("bot_name", "Companion")
    level = bot_info.get("level", 1)
    cls_name = bot_info.get("class_name", "Adventurer")

    # Try LLM generation if available
    if llm_client:
        prompt = (
            f"Generate an authentic World of Warcraft player goal pair for a bot companion.\n"
            f"Character: {bot_name}, Class: {cls_name}, Level: {level}.\n"
            f"Theme: {theme}\n\n"
            "Return JSON only with exact keys:\n"
            "{\n"
            '  "short_term_goal": "<concise 5-10 word actionable task>",\n'
            '  "long_term_goal": "<broad 8-15 word overarching purpose>"\n'
            "}"
        )
        try:
            res = llm_client.generate_json_plan(
                [{"role": "system", "content": "You create authentic WoW player goals."},
                 {"role": "user", "content": prompt}],
                event_type="goal_init"
            )
            data = res if isinstance(res, dict) else (json.loads(res) if isinstance(res, str) else None)
            if data and data.get("short_term_goal") and data.get("long_term_goal"):
                st = str(data["short_term_goal"]).strip()
                lt = str(data["long_term_goal"]).strip()
                if st and lt:
                    logger.info("[GOAL] AI generated goals for %s (Category: %s): Short='%s', Long='%s'",
                                bot_name, chosen_cat, st, lt)
                    return st, lt
        except Exception as e:
            logger.debug("[GOAL] AI goal generation exception, falling back to archetype: %s", e)

    pair = random.choice(archetype["fallbacks"])
    logger.info("[GOAL] Archetype goals assigned to %s (Category: %s): Short='%s', Long='%s'",
                bot_name, chosen_cat, pair[0], pair[1])
    return pair[0], pair[1]
