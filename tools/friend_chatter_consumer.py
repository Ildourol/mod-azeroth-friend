"""Chatter Sensory Consumer for AzerothFriend.

Listens to what mod-llm-chatter has already observed and generated in MySQL,
converting chatter events into physical bot companion actions (moving, looting,
gathering, resting, emoting) with ZERO additional LLM token calls.
"""

import json
import logging
import time
from typing import Any, Dict, List, Optional, Set

from friend_co_processor import TokenSavingsTracker
from friend_constants import DEFAULT_SENSORY_REUSE_ENABLE
from friend_action_catalog import VALID_EMOTES, normalize_plan

logger = logging.getLogger("azeroth_friend.chatter_consumer")


DIALOGUE_TTL_SECONDS = 90.0


class ChatterSensoryConsumer:
    def __init__(self, config: Dict[str, str], tracker: TokenSavingsTracker,
                 db: Optional[Any] = None, memory: Optional[Any] = None):
        raw_enable = config.get("AzerothFriend.Chatter.SensoryReuse", "1").strip().lower()
        self.enabled = raw_enable not in ("0", "false", "no", "off")
        self.hear_other_bots = config.get("AzerothFriend.Chatter.HearOtherBots", "1").strip().lower() not in ("0", "false", "no", "off")
        self.hear_npcs = config.get("AzerothFriend.Chatter.HearNPCs", "1").strip().lower() not in ("0", "false", "no", "off")
        self.max_recent_dialogue = int(config.get("AzerothFriend.Chatter.MaxRecentDialogue", "6"))
        # Incremental ingestion: a bounded overlap catches status/delivery changes
        # that reuse an insertion id, and a freshness filter keeps the consumer
        # from replaying stale backlog after a restart.
        self.overlap_seconds = int(config.get("AzerothFriend.Chatter.ConsumerOverlapSeconds", "30"))
        self.freshness_seconds = int(config.get("AzerothFriend.Chatter.FreshnessSeconds", "90"))
        self.sql_compat = config.get("AzerothFriend.Context.SqlCompatibilityMode", "0").strip().lower() in ("1", "true", "yes", "on")
        self.tracker = tracker
        self.db = db
        self.memory = memory
        self.observations_only = False
        self._processed_event_ids: Set[int] = set()
        self._processed_msg_ids: Set[int] = set()
        self._max_cache_size = 500
        self.recent_dialogue: List[Dict[str, Any]] = []
        self._last_emote_times: Dict[int, float] = {}
        self._last_event_id = 0
        self._last_message_id = 0
        self._live_states: Dict[int, Dict[str, Any]] = {}

    # ------------------------------------------------------------- checkpoints
    def configure_checkpoint(self, checkpoint: Optional[Dict[str, Any]]) -> None:
        """Adopt a durable cursor so restarts do not replay the whole backlog."""
        if not checkpoint:
            return
        self._last_event_id = int(checkpoint.get("last_event_id") or 0)
        self._last_message_id = int(checkpoint.get("last_message_id") or 0)
        if checkpoint.get("overlap_seconds"):
            self.overlap_seconds = max(0, int(checkpoint.get("overlap_seconds") or self.overlap_seconds))

    def checkpoint_state(self) -> Dict[str, int]:
        return {"last_event_id": self._last_event_id, "last_message_id": self._last_message_id}

    def save_checkpoint(self) -> None:
        if self.db is None:
            return
        try:
            self.db.save_consumer_checkpoint("sensory", self._last_event_id,
                                             self._last_message_id, self.overlap_seconds)
        except Exception as err:
            logger.debug("Could not persist chatter consumer checkpoint: %s", err)

    def set_live_states(self, states: Dict[int, Dict[str, Any]]) -> None:
        """Give the consumer the RAM live-state view (no SQL on the poll path)."""
        self._live_states = states or {}

    def get_recent_dialogue_formatted(self) -> List[str]:
        now = time.time()
        fresh = [
            d for d in self.recent_dialogue
            if d.get("at") is None or (now - float(d.get("at") or 0.0)) <= DIALOGUE_TTL_SECONDS
        ]
        return [
            f"[{d['time']}] [{d['channel'].upper()}] {d.get('tag', d['speaker'])}: \"{d['text']}\""
            for d in fresh[-self.max_recent_dialogue:]
        ]

    def _note_dialogue(self, bot_guid: int, speaker: str, tag: str, channel: str,
                       text: str, is_master: bool) -> None:
        """Record one line in the rolling console view and the per-bot cache."""
        self.recent_dialogue.append({
            "bot_guid": int(bot_guid),
            "speaker": speaker,
            "tag": tag,
            "channel": channel,
            "text": text,
            "time": time.strftime("%H:%M:%S"),
            "at": time.time(),
            "is_master": is_master,
        })
        if len(self.recent_dialogue) > self.max_recent_dialogue * 2:
            self.recent_dialogue.pop(0)
        if self.memory is not None:
            self.memory.note_dialogue(bot_guid, text, speaker=speaker, channel=channel,
                                      is_master=is_master)

    def forget_bot(self, bot_guid: int) -> None:
        """Drop transient observations when Claim disconnects one companion."""
        guid = int(bot_guid)
        self.recent_dialogue = [
            entry for entry in self.recent_dialogue
            if int(entry.get("bot_guid") or 0) != guid
        ]
        self._last_emote_times.pop(guid, None)
        self._live_states.pop(guid, None)

    def prune_cache_if_needed(self) -> None:
        if len(self._processed_event_ids) > self._max_cache_size:
            # Keep the newest half
            kept = sorted(list(self._processed_event_ids))[-250:]
            self._processed_event_ids = set(kept)
        if len(self._processed_msg_ids) > self._max_cache_size:
            kept_msgs = sorted(list(self._processed_msg_ids))[-250:]
            self._processed_msg_ids = set(kept_msgs)

    def process_chatter_sensory_events(self, cursor, registered_bots: Dict[int, Dict[str, Any]]) -> int:
        """Poll recent llm_chatter_events and convert observations & dialogue into actions/co-processing."""
        if not self.enabled or not registered_bots:
            return 0

        try:
            # Incremental ingestion: advance on the source id, keep a bounded
            # freshness overlap so status/delivery changes inside the window are
            # still observed, and never replay an old backlog after a restart.
            query = """
                SELECT id, event_type, subject_guid, target_guid, extra_data, created_at, subject_name,
                       `status`
                FROM `llm_chatter_events`
                WHERE `status` IN ('pending', 'processing', 'completed')
                  AND `event_type` IN (
                      'bot_group_nearby_object',
                      'bot_group_loot',
                      'bot_loot_item',
                      'bot_group_low_health',
                      'bot_group_oom',
                      'bot_group_emote_reaction',
                      'bot_group_player_msg',
                      'player_general_msg',
                      'proximity_say',
                      'proximity_player_say',
                      'proximity_conversation',
                      'proximity_player_conversation',
                      'proximity_player_emote',
                      'guild_player_message',
                      'monster_say',
                      'monster_yell',
                      'proximity_npc_say'
                  )
                  AND (`id` > %s OR `created_at` >= DATE_SUB(NOW(), INTERVAL %s SECOND))
                ORDER BY `id` DESC
                LIMIT 50
            """
            cursor.execute(query, (int(self._last_event_id), int(self.freshness_seconds)))
            rows = cursor.fetchall()

            if not rows:
                return 0

            # Process in chronological order
            rows = list(reversed(rows))
            actions_created = 0
            highest_event_id = self._last_event_id

            for row in rows:
                ev_id = row[0]
                ev_type = row[1]
                subj_guid = row[2]
                target_guid = row[3]
                raw_extra = row[4]
                subj_name = row[6] if len(row) > 6 else None
                # Delivery/status changes are tracked by (id, status) rather than
                # the insertion id alone, so a row that moves pending -> completed
                # inside the overlap window is still observed.
                row_status = str(row[7]) if len(row) > 7 else ""
                try:
                    highest_event_id = max(highest_event_id, int(ev_id))
                except (TypeError, ValueError):
                    pass

                dedup_key = (ev_id, row_status) if row_status else ev_id
                if dedup_key in self._processed_event_ids:
                    continue

                self._processed_event_ids.add(dedup_key)

                # Identify which registered bot should react. Brand new companions
                # have no event history yet, so group-wide observations fall back to
                # the most recently active registered bot instead of an arbitrary one.
                bot_guid = None
                if subj_guid and subj_guid in registered_bots:
                    bot_guid = subj_guid
                elif target_guid and target_guid in registered_bots:
                    bot_guid = target_guid
                elif len(registered_bots) == 1:
                    bot_guid = next(iter(registered_bots.keys()))
                else:
                    bot_guid = next(iter(registered_bots.keys()))

                bot_info = registered_bots[bot_guid]

                # The registered-bot dictionary refreshes on a slow cadence.
                # Re-check Claim before even recording this observation so a
                # disconnected companion does not listen through a stale cache.
                cursor.execute(
                    "SELECT control_revision FROM azeroth_friend_bots "
                    "WHERE bot_guid=%s AND enabled=1 AND bridge_enabled=1",
                    (bot_guid,),
                )
                connected_control = cursor.fetchone()
                if not connected_control:
                    continue
                control_revision = int(connected_control[0])

                if self.memory is not None:
                    # Source-ID deduplication shared with the per-bot cache, so the
                    # same observation is consumed exactly once per companion.
                    if not self.memory.note_observation(bot_guid, "chatter_event:%s" % ev_id):
                        continue

                # Goal planning owns actions; observations cannot interrupt the owner.
                if self.observations_only and ev_type in (
                    'bot_group_nearby_object', 'bot_group_loot', 'bot_loot_item',
                    'bot_group_low_health', 'bot_group_oom', 'bot_group_emote_reaction'):
                    continue

                extra: Dict[str, Any] = {}
                if raw_extra:
                    try:
                        extra = json.loads(raw_extra) if isinstance(raw_extra, str) else raw_extra
                    except Exception:
                        extra = {}

                # 1. Nearby Object (Chests, Mining Veins, Herbs, Quest Objects)
                if ev_type == "bot_group_nearby_object":
                    obj_guid = extra.get("guid") or extra.get("object_guid") or extra.get("entry")
                    pos_x = extra.get("x")
                    pos_y = extra.get("y")
                    pos_z = extra.get("z")

                    plan_steps = []
                    if pos_x is not None and pos_y is not None and pos_z is not None:
                        plan_steps.append({
                            "action": "move_to",
                            "params": {"x": float(pos_x), "y": float(pos_y), "z": float(pos_z), "range": 3.0}
                        })
                    if obj_guid:
                        plan_steps.append({
                            "action": "interact",
                            "params": {"guid": int(obj_guid)}
                        })

                    if plan_steps:
                        inserted = self._insert_action_batch(
                            cursor, bot_guid, plan_steps, control_revision)
                        if inserted:
                            actions_created += inserted
                            self.tracker.record_sensory_reuse(1)
                            logger.info(
                                "[SensoryReuse] Bot %s reacted to chatter nearby object %s (0 LLM tokens spent)",
                                bot_info.get("bot_name"), obj_guid
                            )

                # 2. Dead Enemy Loot
                elif ev_type in ("bot_group_loot", "bot_loot_item"):
                    plan_steps = [{"action": "loot", "params": {}}]
                    inserted = self._insert_action_batch(
                        cursor, bot_guid, plan_steps, control_revision)
                    if inserted:
                        actions_created += inserted
                        self.tracker.record_sensory_reuse(1)
                        logger.info(
                            "[SensoryReuse] Bot %s scheduled physical loot from chatter loot observation (0 LLM tokens spent)",
                            bot_info.get("bot_name")
                        )

                # 3. Low Health or Low Mana Downtime
                elif ev_type in ("bot_group_low_health", "bot_group_oom"):
                    plan_steps = [{"action": "eat_drink", "params": {}}]
                    inserted = self._insert_action_batch(
                        cursor, bot_guid, plan_steps, control_revision)
                    if inserted:
                        actions_created += inserted
                        self.tracker.record_sensory_reuse(1)
                        logger.info(
                            "[SensoryReuse] Bot %s scheduled rest/eat_drink from chatter health observation (0 LLM tokens spent)",
                            bot_info.get("bot_name")
                        )

                # 4. Emote Reaction (throttled to avoid spam)
                elif ev_type == "bot_group_emote_reaction":
                    import time
                    now_ts = time.time()
                    last_em = self._last_emote_times.get(bot_guid, 0.0)
                    if (now_ts - last_em) < 30.0:
                        continue  # Throttle: max 1 emote reaction per 30 seconds per bot

                    emote = extra.get("emote", "nod")
                    if not isinstance(emote, str) or not emote.strip():
                        emote = "nod"
                    emote = emote.strip().lower()
                    if emote not in VALID_EMOTES:
                        emote = "nod"
                    plan_steps = [{"action": "emote", "params": {"emote": emote}}]
                    inserted = self._insert_action_batch(
                        cursor, bot_guid, plan_steps, control_revision)
                    if inserted:
                        self._last_emote_times[bot_guid] = now_ts
                        actions_created += inserted
                        self.tracker.record_sensory_reuse(1)

                # 5. Dialogue / Conversations (Master, NPCs, or nearby bots)
                elif ev_type in (
                    "bot_group_player_msg",
                    "player_general_msg",
                    "proximity_say",
                    "proximity_player_say",
                    "proximity_conversation",
                    "proximity_player_conversation",
                    "proximity_player_emote",
                    "guild_player_message",
                    "monster_say",
                    "monster_yell",
                    "proximity_npc_say",
                ):
                    sender = (
                        extra.get("player_name")
                        or extra.get("speaker")
                        or extra.get("name")
                        or extra.get("author")
                        or subj_name
                        or "Adventurer"
                    )
                    msg_text = (
                        extra.get("player_message")
                        or extra.get("message")
                        or extra.get("text")
                        or extra.get("content")
                        or ""
                    )
                    channel = "party" if "group" in ev_type else ("guild" if "guild" in ev_type else "say")

                    import re
                    clean_sender = re.sub(r"\[.*?\]", "", sender).strip().lower()

                    # Master identity comes from the RAM live-state cache; SQL is
                    # only consulted in the explicit compatibility mode.
                    master_name = None
                    live_state = self._live_states.get(bot_guid) or {}
                    owner = live_state.get("owner") if isinstance(live_state, dict) else None
                    if isinstance(owner, dict):
                        master_name = owner.get("name")
                    if not master_name and getattr(self, "sql_compat", False):
                        try:
                            cursor.execute("SELECT environment_json FROM azeroth_friend_state WHERE bot_guid = %s", (bot_guid,))
                            st_row = cursor.fetchone()
                            if st_row and st_row[0]:
                                env_data = json.loads(st_row[0]) if isinstance(st_row[0], str) else st_row[0]
                                master_info = env_data.get("master")
                                if master_info:
                                    master_name = master_info.get("name")
                        except Exception:
                            pass

                    is_master = bool(master_name and clean_sender == master_name.lower())

                    # Check if message is from ANY companion bot or is a mechanical command ack (e.g. Following, Staying)
                    all_bot_names = {
                        str(b.get("bot_name", "")).strip().lower()
                        for b in registered_bots.values()
                    }
                    all_bot_guids = set(registered_bots.keys())
                    is_from_controlled_bot = (
                        subj_guid in all_bot_guids
                        or extra.get("bot_guid") in all_bot_guids
                        or clean_sender in all_bot_names
                    )

                    clean_msg = (msg_text or "").strip().lower()
                    is_bot_ack = any(
                        clean_msg.startswith(ack)
                        for ack in (
                            "following", "staying", "holding", "fleeing", "attacking",
                            "drinking", "eating", "looting", "casting", "moving to",
                            "resting", "ready", "waiting",
                        )
                    )

                    if not is_master and (is_from_controlled_bot or is_bot_ack):
                        cursor.execute(
                            "UPDATE `llm_chatter_events` SET `status` = 'completed', `drop_reason` = 'bot_ack_or_own_message' WHERE `id` = %s",
                            (ev_id,),
                        )
                        continue

                    # Determine speaker role and filtering
                    is_npc = ev_type in ("monster_say", "monster_yell", "proximity_npc_say") or extra.get("is_npc", False)
                    is_other_bot = False

                    if is_master:
                        speaker_tag = f"[MASTER] {sender}"
                    elif is_npc:
                        if not self.hear_npcs:
                            continue
                        speaker_tag = f"[NPC] {sender}"
                    else:
                        is_other_bot = True
                        if not self.hear_other_bots:
                            continue
                        speaker_tag = f"[PARTY BOT] {sender}" if channel == "party" else f"[NEARBY BOT] {sender}"

                    if msg_text:
                        clean_text = msg_text.strip().lower()

                        # Prevent duplicate entries in recent_dialogue context
                        is_recent_dup = any(
                            d.get("text", "").strip().lower() == clean_text
                            and d.get("speaker", "").lower() == sender.lower()
                            for d in self.recent_dialogue[-3:]
                        )
                        if not is_recent_dup:
                            self._note_dialogue(bot_guid, sender, speaker_tag, channel, msg_text, is_master)

                        # Player speech (master or party member) in party/whisper/say is ALREADY
                        # captured directly by AzerothFriendChatHook.cpp in C++ (preventing double hearing).
                        # Only insert dialogue_heard into azeroth_friend_events for NPC speech
                        # (e.g. monster_say, monster_yell, proximity_npc_say).
                        if is_master or not is_npc:
                            self.tracker.record_co_processed(1)
                            logger.debug(
                                "[TokenSharing] Dialogue from %s ('%s') recorded in context; skipping duplicate azeroth_friend_events queue (handled by C++ chat hook)",
                                speaker_tag, msg_text
                            )
                            continue

                        payload = json.dumps({
                            "speaker": speaker_tag,
                            "raw_speaker": sender,
                            "text": msg_text,
                            "channel": channel,
                            "is_master": is_master,
                            "original_event": ev_type,
                        })

                        ins_ev = """
                            INSERT INTO `azeroth_friend_events`
                            (`bot_guid`, `event_type`, `priority`, `source_guid`, `source_name`, `payload_json`, `status`)
                            SELECT %s, 'dialogue_heard', 2, %s, %s, %s, 'pending'
                            FROM `azeroth_friend_bots`
                            WHERE `bot_guid` = %s AND `enabled` = 1 AND `bridge_enabled` = 1
                              AND `control_revision` = %s
                        """
                        cursor.execute(
                            ins_ev,
                            (bot_guid, subj_guid or 0, sender, payload, bot_guid, control_revision),
                        )
                        rowcount = cursor.rowcount
                        inserted = rowcount > 0 if isinstance(rowcount, int) else True
                        if inserted:
                            self.tracker.record_co_processed(1)
                            actions_created += 1
                            logger.info(
                                "[TokenSharing] Observed NPC dialogue from %s ('%s') via %s (Priority 2, physical reaction queued)",
                                speaker_tag, msg_text, channel
                            )

            self._last_event_id = max(self._last_event_id, int(highest_event_id or 0))
            self.save_checkpoint()
            self.prune_cache_if_needed()
            return actions_created

        except Exception as e:
            logger.debug("Error in process_chatter_sensory_events: %s", e)
            return 0

    def ingest_recent_chatter_messages(self, cursor, registered_bots: Dict[int, Dict[str, Any]]) -> int:
        """Poll recent delivered dialogue from llm_chatter_messages to maintain conversational context."""
        if not self.enabled or not registered_bots:
            return 0
        try:
            query = """
                SELECT id, bot_guid, bot_name, message, channel, delivered_at
                FROM `llm_chatter_messages`
                WHERE `delivered` = 1
                  AND (`id` > %s OR `delivered_at` >= DATE_SUB(NOW(), INTERVAL %s SECOND))
                ORDER BY `id` DESC
                LIMIT 20
            """
            cursor.execute(query, (int(self._last_message_id), int(self.freshness_seconds)))
            rows = cursor.fetchall()
            if not rows:
                return 0
            count = 0
            highest_message_id = self._last_message_id
            for row in reversed(rows):
                msg_id = row[0]
                b_name = row[2]
                text = row[3]
                channel = row[4] or "party"
                try:
                    highest_message_id = max(highest_message_id, int(msg_id))
                except (TypeError, ValueError):
                    pass
                if msg_id in self._processed_msg_ids:
                    continue
                self._processed_msg_ids.add(msg_id)
                if not text or not text.strip():
                    continue
                clean_text = text.strip()
                if not any(d.get("text", "").strip().lower() == clean_text.lower() for d in self.recent_dialogue[-5:]):
                    self._note_dialogue(0, b_name, f"[BOT] {b_name}", channel, clean_text, False)
                    count += 1
            self._last_message_id = max(self._last_message_id, int(highest_message_id or 0))
            self.save_checkpoint()
            return count
        except Exception:
            return 0

    def _insert_action_batch(self, cursor, bot_guid: int, steps: List[Dict[str, Any]],
                             control_revision: Optional[int] = None) -> int:
        """Insert physical actions directly into azeroth_friend_actions."""
        import uuid

        validated, rejects = normalize_plan(steps, max_steps=len(steps) or 1)
        for reason in rejects:
            logger.warning("[SensoryReuse] Dropped reaction step for bot %s: %s", bot_guid, reason)
        if not validated:
            return 0

        # Claim is a durable bridge disconnect. Re-check it in the same database
        # session used for insertion so a stale registered-bot cache cannot queue
        # sensory work after the owner claims the companion.
        if control_revision is None:
            cursor.execute(
                "SELECT control_revision FROM azeroth_friend_bots "
                "WHERE bot_guid=%s AND enabled=1 AND bridge_enabled=1",
                (bot_guid,),
            )
            control = cursor.fetchone()
            if not control:
                return 0
            control_revision = int(control[0])

        plan_id = str(uuid.uuid4())[:8]
        query = """
            INSERT INTO `azeroth_friend_actions`
            (`bot_guid`, `plan_id`, `step_index`, `total_steps`, `action_type`, `params_json`, `status`)
            SELECT %s, %s, %s, %s, %s, %s, 'pending'
            FROM `azeroth_friend_bots`
            WHERE `bot_guid` = %s AND `enabled` = 1 AND `bridge_enabled` = 1
              AND `control_revision` = %s
        """
        total = len(validated)
        inserted = 0
        for idx, step in enumerate(validated):
            action_type = step.get("action")
            params_data = dict(step.get("params", {}))
            params_data["_control_revision"] = control_revision
            params = json.dumps(params_data)
            # 'pending' is the only status the C++ executor picks up; 'queued' is not in
            # the status enum and was rejected outright under STRICT_TRANS_TABLES.
            cursor.execute(
                query,
                (bot_guid, plan_id, idx, total, action_type, params,
                 bot_guid, control_revision),
            )
            rowcount = cursor.rowcount
            if rowcount > 0 if isinstance(rowcount, int) else True:
                inserted += 1
        return inserted
