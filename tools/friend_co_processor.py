"""Token sharing and co-processing engine between mod-azeroth-friend and mod-llm-chatter."""

import logging
import threading
from typing import Any, Dict, Optional

from friend_constants import (
    DEFAULT_ESTIMATED_TOKENS_PER_CALL,
    DEFAULT_TOKEN_SHARING_ENABLE,
)

logger = logging.getLogger("azeroth_friend.co_processor")


class TokenSavingsTracker:
    """Thread-safe telemetry accumulator tracking token savings from chatter co-processing and sensory reuse."""

    def __init__(self, estimated_tokens_per_call: int = DEFAULT_ESTIMATED_TOKENS_PER_CALL):
        self._lock = threading.Lock()
        self.estimated_tokens_per_call = estimated_tokens_per_call
        self.co_processed_count = 0
        self.sensory_reused_count = 0
        self.standalone_plans_count = 0

    def record_co_processed(self, count: int = 1) -> None:
        with self._lock:
            self.co_processed_count += count

    def record_sensory_reuse(self, count: int = 1) -> None:
        with self._lock:
            self.sensory_reused_count += count

    def record_standalone_plan(self, count: int = 1) -> None:
        with self._lock:
            self.standalone_plans_count += count

    @property
    def total_tokens_saved(self) -> int:
        with self._lock:
            total_reused = self.co_processed_count + self.sensory_reused_count
            return total_reused * self.estimated_tokens_per_call

    def get_stats(self) -> Dict[str, Any]:
        with self._lock:
            total_reused = self.co_processed_count + self.sensory_reused_count
            return {
                "co_processed_events": self.co_processed_count,
                "sensory_reused_events": self.sensory_reused_count,
                "total_shared_interactions": total_reused,
                "standalone_plans": self.standalone_plans_count,
                "estimated_tokens_per_call": self.estimated_tokens_per_call,
                "total_tokens_saved": total_reused * self.estimated_tokens_per_call,
            }


class ChatterCoProcessor:
    """Manages token sharing and bidirectional message delivery with mod-llm-chatter."""

    def __init__(self, config: Dict[str, str], tracker: Optional[TokenSavingsTracker] = None):
        raw_enable = config.get("AzerothFriend.Chatter.TokenSharing", "1").strip().lower()
        self.enabled = raw_enable not in ("0", "false", "no", "off")

        try:
            self.estimated_tokens_per_call = int(
                config.get("AzerothFriend.Chatter.EstimatedTokensPerCall", str(DEFAULT_ESTIMATED_TOKENS_PER_CALL))
            )
        except ValueError:
            self.estimated_tokens_per_call = DEFAULT_ESTIMATED_TOKENS_PER_CALL

        self.tracker = tracker or TokenSavingsTracker(self.estimated_tokens_per_call)
        self._chatter_tables_verified = False

    def verify_chatter_tables(self, cursor) -> bool:
        """Check if mod-llm-chatter tables exist in the current database."""
        try:
            cursor.execute("SHOW TABLES LIKE 'llm_chatter_messages'")
            res_messages = cursor.fetchone()
            cursor.execute("SHOW TABLES LIKE 'llm_chatter_events'")
            res_events = cursor.fetchone()
            self._chatter_tables_verified = bool(res_messages and res_events)
            return self._chatter_tables_verified
        except Exception as e:
            logger.debug("Failed to check mod-llm-chatter tables: %s", e)
            self._chatter_tables_verified = False
            return False

    def deliver_shared_speech(
        self,
        cursor,
        bot_guid: int,
        bot_name: str,
        speech: str,
        channel: str = "party",
        player_guid: Optional[int] = None,
    ) -> bool:
        """Route dialogue from a co-processed plan directly into llm_chatter_messages."""
        if not self.enabled or not speech or not speech.strip():
            return False

        if not self._chatter_tables_verified:
            if not self.verify_chatter_tables(cursor):
                return False

        try:
            owner_subsystem = "group"
            chan_clean = (channel or "party").lower()
            if chan_clean in ("say", "yell"):
                owner_subsystem = "proximity"
            elif chan_clean == "guild":
                owner_subsystem = "guild"

            query = """
                INSERT INTO `llm_chatter_messages`
                (`bot_guid`, `bot_name`, `message`, `channel`, `owner_subsystem`, `delivered`, `player_guid`, `deliver_at`)
                VALUES (%s, %s, %s, %s, %s, 0, %s, NOW())
            """
            cursor.execute(query, (bot_guid, bot_name, speech.strip(), chan_clean, owner_subsystem, player_guid))
            self.tracker.record_co_processed(1)
            logger.info(
                "[TokenSharing] Dispatched companion speech for %s (%d) to llm_chatter_messages: '%s' via %s (%s)",
                bot_name, bot_guid, speech.strip(), chan_clean, owner_subsystem
            )
            return True
        except Exception as e:
            logger.error("Failed to insert into llm_chatter_messages: %s", e)
            return False

    def deduplicate_chatter_events(self, cursor, bot_guid: int, event_type: str) -> int:
        """Mark matching pending events in llm_chatter_events as co-processed to prevent duplicate LLM calls."""
        if not self.enabled or not self._chatter_tables_verified:
            return 0

        # Map friend events to chatter event types
        chatter_event_map = {
            "dialogue_heard": "bot_group_player_msg",
            "player_command": "bot_group_player_msg",
            "combat_enter": "bot_group_combat",
            "combat_leave": "bot_group_combat",
            "trade_requested": "bot_group_general_reaction",
            "duel_requested": "bot_group_general_reaction",
        }

        mapped_type = chatter_event_map.get(event_type)
        if not mapped_type:
            return 0

        try:
            update_query = """
                UPDATE `llm_chatter_events`
                SET `status` = 'completed', `drop_reason` = 'co_processed_by_azeroth_friend'
                WHERE `status` = 'pending'
                  AND `event_type` = %s
                  AND (`subject_guid` = %s OR `target_guid` = %s OR `subject_guid` IS NULL)
                LIMIT 2
            """
            cursor.execute(update_query, (mapped_type, bot_guid, bot_guid))
            affected = cursor.rowcount
            if affected > 0:
                logger.debug(
                    "[TokenSharing] Deduplicated %d event(s) in llm_chatter_events for bot %d",
                    affected, bot_guid
                )
            return affected
        except Exception as e:
            logger.debug("Failed to deduplicate llm_chatter_events: %s", e)
            return 0
