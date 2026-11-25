"""Autopilot cognitive integration module for mod-azeroth-friend.

Handles vocabulary normalisation, compound errand sequencing, and
measurable goal verification between the decoupled Python cognitive bridge
and the AzerothCore engine.
"""

from typing import Any, Dict, List, Optional, Tuple
import logging

LOG = logging.getLogger("azeroth_friend.autopilot")

AUTOPILOT_GOAL_KINDS = {
    "reach_level",
    "reach_skill",
    "earn_gold",
    "explore_zone",
    "complete_quests",
    "run_dungeon",
    "free",
}

AUTOPILOT_SERVICES = {
    "repair",
    "vendor",
    "trainer",
    "profession",
    "inn",
    "flightmaster",
    "bank",
    "auction",
}


def normalize_autopilot_goal(goal_kind: str, target: str, target_id: int = 0, value: int = 0) -> Dict[str, Any]:
    """Normalize a measurable autopilot goal structure."""
    kind = (goal_kind or "free").strip().lower()
    if kind not in AUTOPILOT_GOAL_KINDS:
        kind = "free"

    return {
        "kind": kind,
        "target": target.strip() if target else "",
        "target_id": int(target_id),
        "value": int(value),
    }


def is_goal_satisfied(goal: Dict[str, Any], current_state: Dict[str, Any]) -> bool:
    """Evaluate whether an in-game measurable goal has been satisfied by engine facts."""
    kind = goal.get("kind", "free")
    if kind == "free":
        return False

    target_val = int(goal.get("value", 0))

    if kind == "reach_level":
        return int(current_state.get("level", 0)) >= target_val
    elif kind == "reach_skill":
        skills = current_state.get("skills", {})
        target_id = goal.get("target_id", 0)
        return int(skills.get(target_id, 0)) >= target_val
    elif kind == "earn_gold":
        return int(current_state.get("money", 0)) >= target_val
    elif kind == "complete_quests":
        return int(current_state.get("quests_rewarded", 0)) >= target_val
    elif kind == "run_dungeon":
        return int(current_state.get("dungeons_total", 0)) >= target_val

    return False


def build_errand_plan(errand_text: str) -> List[Dict[str, Any]]:
    """Convert natural-language or structured errand string into sequenced actions.

    Example: "goto vendor then repair and sell junk"
    Produces: [
        {"action": "goto_service", "params": {"service": "vendor"}},
        {"action": "repair", "params": {}},
        {"action": "sell", "params": {"all_junk": True}}
    ]
    """
    actions: List[Dict[str, Any]] = []
    text = errand_text.strip().lower()

    if "repair" in text:
        actions.append({"action": "goto_service", "params": {"service": "repair"}})
        actions.append({"action": "repair", "params": {}})

    if "vendor" in text or "sell" in text:
        if not any(a["action"] == "goto_service" and a["params"].get("service") in ("vendor", "repair") for a in actions):
            actions.append({"action": "goto_service", "params": {"service": "vendor"}})
        actions.append({"action": "sell", "params": {"all_junk": True}})

    if "flight" in text or "flightmaster" in text:
        actions.append({"action": "goto_service", "params": {"service": "flightmaster"}})

    if "train" in text or "trainer" in text:
        actions.append({"action": "goto_service", "params": {"service": "trainer"}})
        actions.append({"action": "trainer_learn", "params": {}})

    if "bank" in text:
        actions.append({"action": "goto_service", "params": {"service": "bank"}})

    if "inn" in text or "hearth" in text:
        actions.append({"action": "goto_service", "params": {"service": "inn"}})

    return actions
