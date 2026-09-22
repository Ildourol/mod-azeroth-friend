"""Thread-safe per-bot RAM caches for AzerothFriend.

This replaces the historical "single dict of everything, replayed forever" working
memory with bounded, lazily filled per-bot caches:

  * identity / personality, goals, capabilities and the latest summary are loaded
    lazily by the bridge and invalidated on explicit change notifications.
  * dialogue is bounded to six relevant lines and 90 seconds per bot.
  * completed action results are bounded to three entries for two minutes.
  * one compact blocker stays attached to the active goal until resolved.
  * transient memory is capped at 2 MiB per bot and 128 MiB globally; expired
    dialogue and completed observations are evicted first. Active control, goals
    and plan state are never silently evicted.
"""

import threading
import time
from collections import OrderedDict
from typing import Any, Dict, List, Optional

# Bounded transient memory (plan section 1).
MAX_BOT_BYTES = 2 * 1024 * 1024
MAX_GLOBAL_BYTES = 128 * 1024 * 1024
DIALOGUE_TTL_SECONDS = 90.0
DIALOGUE_MAX_LINES = 6
OUTCOME_TTL_SECONDS = 120.0
OUTCOME_MAX_ENTRIES = 3
SUMMARY_MAX_ENTRIES = 2
OBSERVATION_MAX_ENTRIES = 256


class BotMemoryCache:
    """Value-only cache for one companion. Owned by :class:`WorkingMemory`."""

    def __init__(self, bot_guid: int) -> None:
        self.bot_guid = int(bot_guid)
        self.state: Dict[str, Any] = {}
        self.personality: Optional[str] = None
        self.goals: Dict[str, Any] = {}
        self.capabilities: List[Dict[str, Any]] = []
        self.capability_revision: int = -1
        self.self_context: Optional[str] = None
        self.summaries: List[Dict[str, Any]] = []
        self.summary_dirty: bool = False
        self.conversation_history: List[Dict[str, str]] = []
        self.encounter: Optional[Dict[str, Any]] = None
        self.active_plan: List[str] = []
        self.blocker: str = ""
        self._dialogue: "OrderedDict[int, Dict[str, Any]]" = OrderedDict()
        self._outcomes: "OrderedDict[int, Dict[str, Any]]" = OrderedDict()
        self._observations: "OrderedDict[str, float]" = OrderedDict()
        self._dialogue_seq = 0
        self._outcome_seq = 0

    # --------------------------------------------------------------- transient
    def note_dialogue(self, text: str, speaker: str = "", channel: str = "",
                      is_master: bool = False, now: Optional[float] = None) -> None:
        text = str(text or "").strip()
        if not text:
            return
        current = time.time() if now is None else now
        self._dialogue_seq += 1
        self._dialogue[self._dialogue_seq] = {
            "text": text,
            "speaker": str(speaker or ""),
            "channel": str(channel or ""),
            "is_master": bool(is_master),
            "at": current,
        }
        self.expire(now=current)

    def get_dialogue(self, limit: int = DIALOGUE_MAX_LINES, now: Optional[float] = None) -> List[str]:
        current = time.time() if now is None else now
        self.expire(now=current)
        entries = list(self._dialogue.values())
        rendered: List[str] = []
        for entry in entries[-max(1, int(limit)):]:
            tag = "[MASTER] " if entry.get("is_master") else ""
            speaker = entry.get("speaker") or "someone"
            channel = entry.get("channel") or "say"
            rendered.append("%s%s (%s): \"%s\"" % (tag, speaker, channel, entry.get("text", "")))
        return rendered

    def note_outcome(self, text: str, now: Optional[float] = None) -> None:
        text = str(text or "").strip()
        if not text:
            return
        current = time.time() if now is None else now
        self._outcome_seq += 1
        self._outcomes[self._outcome_seq] = {"text": text, "at": current}
        self.expire(now=current)

    def get_outcomes(self, now: Optional[float] = None) -> List[str]:
        current = time.time() if now is None else now
        self.expire(now=current)
        return [entry["text"] for entry in self._outcomes.values()]

    def note_observation(self, key: str, now: Optional[float] = None) -> bool:
        """Consume an observation once. Returns True the first time it is seen."""
        key = str(key or "").strip()
        if not key:
            return True
        current = time.time() if now is None else now
        if key in self._observations:
            self._observations.move_to_end(key)
            return False
        self._observations[key] = current
        self.expire(now=current)
        return True

    def has_observation(self, key: str) -> bool:
        return str(key or "").strip() in self._observations

    # ------------------------------------------------------------------ expiry
    def expire(self, now: Optional[float] = None) -> None:
        """Drop expired dialogue and completed observations (never control state)."""
        current = time.time() if now is None else now
        for seq, entry in list(self._dialogue.items()):
            if current - float(entry.get("at", 0.0)) > DIALOGUE_TTL_SECONDS:
                self._dialogue.pop(seq, None)
        while len(self._dialogue) > DIALOGUE_MAX_LINES:
            self._dialogue.popitem(last=False)
        for seq, entry in list(self._outcomes.items()):
            if current - float(entry.get("at", 0.0)) > OUTCOME_TTL_SECONDS:
                self._outcomes.pop(seq, None)
        while len(self._outcomes) > OUTCOME_MAX_ENTRIES:
            self._outcomes.popitem(last=False)
        while len(self._observations) > OBSERVATION_MAX_ENTRIES:
            self._observations.popitem(last=False)

    def estimated_bytes(self) -> int:
        size = 256
        size += len(repr(self.state))
        size += len(repr(self.conversation_history))
        size += len(repr(self._dialogue))
        size += len(repr(self._outcomes))
        size += len(self.self_context or "")
        size += sum(len(repr(row)) for row in self.capabilities)
        size += sum(len(repr(row)) for row in self.summaries)
        return size


class WorkingMemory:
    """RAM session cache and per-bot cache registry."""

    def __init__(self, max_bot_bytes: int = MAX_BOT_BYTES, max_global_bytes: int = MAX_GLOBAL_BYTES) -> None:
        self._lock = threading.RLock()
        self._bots: Dict[int, BotMemoryCache] = {}
        self._max_bot_bytes = max(64 * 1024, int(max_bot_bytes))
        self._max_global_bytes = max(self._max_bot_bytes, int(max_global_bytes))

    # ------------------------------------------------------------- bot caches
    def bot(self, bot_guid: int) -> BotMemoryCache:
        with self._lock:
            cache = self._bots.get(int(bot_guid))
            if cache is None:
                cache = BotMemoryCache(int(bot_guid))
                self._bots[int(bot_guid)] = cache
            return cache

    def forget(self, bot_guid: int) -> None:
        with self._lock:
            self._bots.pop(int(bot_guid), None)

    # ------------------------------------------------------------- state cache
    def get_cached_state(self, bot_guid: int) -> Optional[Dict[str, Any]]:
        with self._lock:
            cache = self._bots.get(int(bot_guid))
            return dict(cache.state) if cache and cache.state else None

    def set_cached_state(self, bot_guid: int, state: Dict[str, Any]) -> None:
        with self._lock:
            cache = self.bot(bot_guid)
            old_state = cache.state
            if old_state and isinstance(state, dict):
                old_zone, new_zone = old_state.get("zone_id"), state.get("zone_id")
                old_map, new_map = old_state.get("map_id"), state.get("map_id")
                if ((new_zone is not None and old_zone is not None and new_zone != old_zone)
                        or (new_map is not None and old_map is not None and new_map != old_map)):
                    # Zone/map change: flush conversation context so cross-zone
                    # coordinates can never be hallucinated (MAF-005).
                    cache.conversation_history = []
                    cache.encounter = None
                    cache.active_plan = []
            cache.state = dict(state)

    # ------------------------------------------------------------ goals / id
    def set_goals(self, bot_guid: int, goals: Dict[str, Any]) -> None:
        with self._lock:
            self.bot(bot_guid).goals = dict(goals or {})

    def get_goals(self, bot_guid: int) -> Dict[str, Any]:
        with self._lock:
            return dict(self.bot(bot_guid).goals)

    def set_personality(self, bot_guid: int, personality: str) -> None:
        with self._lock:
            self.bot(bot_guid).personality = str(personality or "")

    def get_personality(self, bot_guid: int) -> str:
        with self._lock:
            return self.bot(bot_guid).personality or ""

    # ----------------------------------------------------------- capabilities
    def set_capabilities(self, bot_guid: int, catalog: List[Dict[str, Any]],
                         self_context: Optional[str] = None) -> None:
        with self._lock:
            cache = self.bot(bot_guid)
            cache.capabilities = list(catalog or [])
            if self_context is not None:
                cache.self_context = self_context

    def get_capabilities(self, bot_guid: int) -> List[Dict[str, Any]]:
        with self._lock:
            return list(self.bot(bot_guid).capabilities)

    def get_self_context(self, bot_guid: int) -> Optional[str]:
        with self._lock:
            return self.bot(bot_guid).self_context

    def invalidate_capabilities(self, bot_guid: int) -> None:
        with self._lock:
            cache = self.bot(bot_guid)
            cache.capabilities = []
            cache.self_context = None
            cache.capability_revision = -1

    def get_capability_revision(self, bot_guid: int) -> int:
        with self._lock:
            return self.bot(bot_guid).capability_revision

    def set_capability_revision(self, bot_guid: int, revision: int) -> None:
        with self._lock:
            self.bot(bot_guid).capability_revision = int(revision)

    # --------------------------------------------------------------- summaries
    def set_summaries(self, bot_guid: int, summaries: List[Dict[str, Any]]) -> None:
        with self._lock:
            cache = self.bot(bot_guid)
            cache.summaries = list(summaries or [])[-SUMMARY_MAX_ENTRIES:]
            cache.summary_dirty = False

    def get_summaries(self, bot_guid: int) -> List[str]:
        with self._lock:
            cache = self._bots.get(int(bot_guid))
            if not cache:
                return []
            rendered = []
            for summary in cache.summaries:
                content = summary.get("content") if isinstance(summary, dict) else summary
                if content:
                    rendered.append(str(content))
            return rendered

    def mark_summary_dirty(self, bot_guid: int) -> None:
        with self._lock:
            self.bot(bot_guid).summary_dirty = True

    def is_summary_dirty(self, bot_guid: int) -> bool:
        with self._lock:
            return self.bot(bot_guid).summary_dirty

    def dirty_bots(self) -> List[int]:
        with self._lock:
            return [guid for guid, cache in self._bots.items() if cache.summary_dirty]

    # ------------------------------------------------------- plan / blockers
    def set_active_plan(self, bot_guid: int, steps: List[str]) -> None:
        with self._lock:
            self.bot(bot_guid).active_plan = [str(step) for step in (steps or [])][:5]

    def get_active_plan(self, bot_guid: int) -> List[str]:
        with self._lock:
            return list(self.bot(bot_guid).active_plan)

    def set_blocker(self, bot_guid: int, blocker: str) -> None:
        with self._lock:
            self.bot(bot_guid).blocker = str(blocker or "")

    def clear_blocker(self, bot_guid: int) -> None:
        with self._lock:
            self.bot(bot_guid).blocker = ""

    def get_blocker(self, bot_guid: int) -> str:
        with self._lock:
            return self.bot(bot_guid).blocker

    # ------------------------------------------------------ chat / outcomes
    def note_dialogue(self, bot_guid: int, text: str, speaker: str = "", channel: str = "",
                      is_master: bool = False, now: Optional[float] = None) -> None:
        with self._lock:
            self.bot(bot_guid).note_dialogue(text, speaker, channel, is_master, now=now)

    def get_dialogue(self, bot_guid: int, limit: int = DIALOGUE_MAX_LINES,
                     now: Optional[float] = None) -> List[str]:
        with self._lock:
            return self.bot(bot_guid).get_dialogue(limit, now=now)

    def note_outcome(self, bot_guid: int, text: str, now: Optional[float] = None) -> None:
        with self._lock:
            self.bot(bot_guid).note_outcome(text, now=now)

    def get_outcomes(self, bot_guid: int, now: Optional[float] = None) -> List[str]:
        with self._lock:
            return self.bot(bot_guid).get_outcomes(now=now)

    def note_observation(self, bot_guid: int, key: str, now: Optional[float] = None) -> bool:
        with self._lock:
            return self.bot(bot_guid).note_observation(key, now=now)

    def has_observation(self, bot_guid: int, key: str) -> bool:
        with self._lock:
            return self.bot(bot_guid).has_observation(key)

    # ------------------------------------------------------- conversation log
    def get_conversation_history(self, bot_guid: int, max_turns: int = 6) -> List[Dict[str, str]]:
        with self._lock:
            history = self.bot(bot_guid).conversation_history
            return list(history[-max(0, int(max_turns)):])

    def append_conversation(self, bot_guid: int, role: str, content: str) -> None:
        with self._lock:
            cache = self.bot(bot_guid)
            cache.conversation_history.append({"role": str(role), "content": str(content)})
            if len(cache.conversation_history) > 12:
                cache.conversation_history = cache.conversation_history[-12:]

    def clear_conversation(self, bot_guid: int) -> None:
        with self._lock:
            self.bot(bot_guid).conversation_history = []

    # --------------------------------------------------------------- encounters
    def get_encounter(self, bot_guid: int) -> Optional[Dict[str, Any]]:
        with self._lock:
            cache = self._bots.get(int(bot_guid))
            if cache and cache.encounter:
                return dict(cache.encounter)
            return None

    def update_encounter(
        self,
        bot_guid: int,
        target_guid: int,
        target_name: str,
        target_hp_pct: float = 100.0,
        now: Optional[float] = None,
    ) -> Dict[str, Any]:
        """Update or initialize the combat encounter for the bot."""
        current_time = time.time() if now is None else now
        with self._lock:
            cache = self.bot(bot_guid)
            encounter = cache.encounter
            if encounter and encounter.get("target_guid") == target_guid and target_guid > 0:
                encounter["turn_count"] = int(encounter.get("turn_count", 1)) + 1
                encounter["last_turn_time"] = current_time
                encounter["last_hp_pct"] = target_hp_pct
                encounter["duration"] = max(0.0, current_time - encounter.get("start_time", current_time))
                encounter["is_initial"] = False
                return dict(encounter)

            new_encounter = {
                "target_guid": target_guid,
                "target_name": target_name or ("Creature #%d" % target_guid),
                "start_time": current_time,
                "last_turn_time": current_time,
                "turn_count": 1,
                "initial_hp_pct": target_hp_pct,
                "last_hp_pct": target_hp_pct,
                "duration": 0.0,
                "is_initial": True,
            }
            cache.encounter = new_encounter
            return dict(new_encounter)

    def clear_encounter(self, bot_guid: int) -> None:
        with self._lock:
            cache = self._bots.get(int(bot_guid))
            if cache:
                cache.encounter = None

    # ------------------------------------------------------------- bookkeeping
    def update_affinity(self, current_affinity: int, delta: int) -> int:
        return max(-100, min(100, current_affinity + delta))

    def apply_budget(self) -> int:
        """Evict expired transients. Returns the number of over-budget bots seen."""
        evicted = 0
        with self._lock:
            total = 0
            for cache in self._bots.values():
                cache.expire()
                size = cache.estimated_bytes()
                if size > self._max_bot_bytes:
                    evicted += 1
                total += size
            if total > self._max_global_bytes:
                # Coldest bots first: transients only, never control state.
                for _guid, cache in sorted(self._bots.items(),
                                           key=lambda item: item[1].estimated_bytes(), reverse=True):
                    cache.expire()
        return evicted

    def cache_stats(self) -> Dict[str, Any]:
        with self._lock:
            sizes = {guid: cache.estimated_bytes() for guid, cache in self._bots.items()}
            return {
                "bots": len(self._bots),
                "total_bytes": sum(sizes.values()),
                "max_global_bytes": self._max_global_bytes,
                "max_bot_bytes": self._max_bot_bytes,
                "largest_bot_bytes": max(sizes.values()) if sizes else 0,
            }
