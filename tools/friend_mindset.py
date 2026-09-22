"""Cognitive Mindset and Commitment Latency Manager (Preventing AI ADHD)."""

import logging
import threading
import time
from dataclasses import dataclass
from typing import Dict, Optional, Tuple

from friend_constants import (
    DEFAULT_MINDSET_ALLOW_OPPORTUNISTIC,
    DEFAULT_MINDSET_COMMITMENT_SECONDS,
    DEFAULT_MINDSET_ENABLE,
    MINDSET_INTERRUPT_EVENTS,
    VALID_MINDSETS,
)

logger = logging.getLogger("azeroth_friend.mindset")


@dataclass
class BotMindsetState:
    bot_guid: int
    current_mindset: str = "IDLE"
    start_time: float = 0.0
    commitment_duration: float = DEFAULT_MINDSET_COMMITMENT_SECONDS
    locked_until: float = 0.0
    last_thought: str = ""
    switch_count: int = 0
    suppressed_switches: int = 0


class MindsetManager:
    """Manages bot cognitive momentum, goal commitment windows, and interrupt overrides."""

    def __init__(
        self,
        enabled: bool = DEFAULT_MINDSET_ENABLE,
        commitment_seconds: float = DEFAULT_MINDSET_COMMITMENT_SECONDS,
        allow_opportunistic: bool = DEFAULT_MINDSET_ALLOW_OPPORTUNISTIC,
    ):
        self.enabled = enabled
        self.default_commitment = float(commitment_seconds)
        self.allow_opportunistic = allow_opportunistic
        self._states: Dict[int, BotMindsetState] = {}
        self._lock = threading.RLock()

    def get_state(self, bot_guid: int) -> BotMindsetState:
        with self._lock:
            if bot_guid not in self._states:
                self._states[bot_guid] = BotMindsetState(
                    bot_guid=bot_guid,
                    current_mindset="IDLE",
                    start_time=time.time(),
                    commitment_duration=self.default_commitment,
                    locked_until=0.0,
                )
            return self._states[bot_guid]

    def get_mindset(self, bot_guid: int) -> str:
        state = self.get_state(bot_guid)
        return state.current_mindset

    def get_remaining_commitment(self, bot_guid: int, now: Optional[float] = None) -> float:
        state = self.get_state(bot_guid)
        current_time = now if now is not None else time.time()
        remaining = state.locked_until - current_time
        return max(0.0, remaining)

    def is_committed(self, bot_guid: int, now: Optional[float] = None) -> bool:
        if not self.enabled:
            return False
        return self.get_remaining_commitment(bot_guid, now) > 0.0

    def can_switch_mindset(
        self,
        bot_guid: int,
        proposed_mindset: str,
        event_type: str,
        priority: int = 5,
        now: Optional[float] = None,
    ) -> Tuple[bool, str]:
        """Check whether a bot is permitted to switch mindset or if latency lock prevents it."""
        if not self.enabled:
            return True, "mindset latency disabled"

        # Emergency interrupt preemption immediately breaks latency lock
        if event_type in MINDSET_INTERRUPT_EVENTS or priority <= 2:
            return True, f"emergency interrupt '{event_type}' (priority {priority})"

        state = self.get_state(bot_guid)
        current_time = now if now is not None else time.time()
        remaining = state.locked_until - current_time

        # If not currently locked, free to switch
        if remaining <= 0.0:
            return True, "commitment window expired"

        # If staying in the same mindset, permitted
        clean_proposed = proposed_mindset.strip().upper() if proposed_mindset else ""
        if clean_proposed == state.current_mindset:
            return True, f"retaining active mindset '{state.current_mindset}'"

        # Suppressed due to active commitment lock
        with self._lock:
            state.suppressed_switches += 1

        reason = (
            f"locked in mindset '{state.current_mindset}' for {remaining:.1f}s more; "
            f"non-interrupt event '{event_type}' (priority {priority}) cannot force switch to '{clean_proposed}'"
        )
        return False, reason

    def set_mindset(
        self,
        bot_guid: int,
        mindset: str,
        custom_duration: Optional[float] = None,
        thought: str = "",
        now: Optional[float] = None,
    ) -> str:
        """Commit bot to a mindset and start the commitment latency window."""
        clean_mindset = mindset.strip().upper() if mindset else "IDLE"
        if clean_mindset not in VALID_MINDSETS:
            logger.warning("Unrecognized mindset '%s' for bot %d; falling back to IDLE", mindset, bot_guid)
            clean_mindset = "IDLE"

        current_time = now if now is not None else time.time()
        duration = float(custom_duration) if custom_duration is not None else self.default_commitment

        with self._lock:
            state = self.get_state(bot_guid)
            old_mindset = state.current_mindset
            state.current_mindset = clean_mindset
            state.start_time = current_time
            state.commitment_duration = duration
            state.locked_until = current_time + duration
            state.last_thought = thought
            if old_mindset != clean_mindset:
                state.switch_count += 1
                logger.info(
                    "Bot %d switched mindset: '%s' -> '%s' (committed for %.1fs)",
                    bot_guid, old_mindset, clean_mindset, duration
                )

        return clean_mindset

    def format_mindset_context(self, bot_guid: int, now: Optional[float] = None) -> str:
        """Format current mindset and commitment timer for prompt injection."""
        state = self.get_state(bot_guid)
        current_time = now if now is not None else time.time()
        remaining = max(0.0, state.locked_until - current_time)
        active_for = max(0.0, current_time - state.start_time)

        if not self.enabled:
            return f"Current Mindset: {state.current_mindset} (Latency: disabled)"

        if remaining > 0.0:
            return (
                f"Current Mindset: {state.current_mindset} "
                f"(Active for {active_for:.1f}s, {remaining:.1f}s remaining in commitment window)"
            )
        else:
            return f"Current Mindset: {state.current_mindset} (Active for {active_for:.1f}s, commitment expired / open)"
