"""Database connection and query helpers for AzerothFriend."""

import json
import logging
import re
from typing import Any, Dict, List, Optional, Set
import mysql.connector
from mysql.connector import pooling

logger = logging.getLogger("azeroth_friend.db")

# Explicit column lists: never `SELECT *` on the hot path, so a schema change
# cannot silently change prompt payloads or break the bridge.
BOT_CONTROL_COLUMNS = (
    "bot_guid, bot_name, enabled, bridge_enabled, action_mode, mode, master_guid, master_name, "
    "personality, affinity, current_goal, long_term_goal, "
    "autonomy_enabled, goal_status, goal_progress, goal_result, control_revision"
)
STATE_COLUMNS = (
    "bot_guid, map_id, zone_id, area_id, pos_x, pos_y, pos_z, orientation, level, health_pct, "
    "power_pct, in_combat, target_guid, target_name, environment_json, last_thought"
)
ACTION_OUTCOME_COLUMNS = (
    "id, bot_guid, plan_id, step_index, action_type, status, failure_reason, result_json, "
    "completed_at, UNIX_TIMESTAMP(completed_at) AS completed_ts"
)
SUMMARY_COLUMNS = (
    "id, bot_guid, goal_identity, revision, window_start, window_end, content, payload_json, "
    "source_outcome_ids, summary_key, created_at"
)

# Diagnostic retention for terminal event/action detail (plan section 3).
DIAGNOSTIC_RETENTION_DAYS = 7
PRUNE_BATCH_SIZE = 500


def parse_conf_file(conf_path: str) -> Dict[str, str]:
    """Parse AzerothCore-style key = value .conf file."""
    config: Dict[str, str] = {}
    with open(conf_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            match = re.match(r"^([A-Za-z0-9_.]+)\s*=\s*(.*?)$", line)
            if match:
                key = match.group(1).strip()
                val = match.group(2).strip().strip('"').strip("'")
                config[key] = val
    return config


class DatabaseManager:
    def __init__(self, config: Dict[str, str]):
        self.host = config.get("AzerothFriend.Database.Host", "127.0.0.1")
        self.port = int(config.get("AzerothFriend.Database.Port", "3306"))
        self.user = config.get("AzerothFriend.Database.User", "acore")
        self.password = config.get("AzerothFriend.Database.Password", "acore")
        self.database = config.get("AzerothFriend.Database.CharactersDB", "acore_characters")
        self.world_db = config.get("AzerothFriend.Database.WorldDB", "acore_world")
        self.pool: Optional[pooling.MySQLConnectionPool] = None
        self._char_spell_columns: Optional[set] = None
        # Optional SpellNameIndex; when present it fills in names the world
        # database is missing (partial spell_dbc tables are common in repacks).
        self.spell_index: Optional[Any] = None
        # Legacy azeroth_friend_memory rows stay as diagnostic history and are
        # excluded from automatic prompts unless the operator opts back in.
        self.include_legacy_memory = self._flag(
            config.get("AzerothFriend.Context.IncludeLegacyMemory", "0"))
        self.sql_compatibility_mode = self._flag(
            config.get("AzerothFriend.Context.SqlCompatibilityMode", "0"))
        for identifier in (self.database, self.world_db):
            if not re.fullmatch(r"[A-Za-z0-9_]+", identifier):
                raise ValueError("Invalid database identifier")

    @staticmethod
    def _flag(value: Any, default: bool = False) -> bool:
        if value is None:
            return default
        return str(value).strip().lower() in ("1", "true", "yes", "on")

    def connect(self) -> None:
        try:
            self.pool = pooling.MySQLConnectionPool(
                pool_name="af_pool",
                pool_size=5,
                host=self.host,
                port=self.port,
                user=self.user,
                password=self.password,
                database=self.database,
                charset="utf8mb4",
                autocommit=True,
            )
            logger.info("Connected to MySQL database '%s' at %s:%d", self.database, self.host, self.port)
        except Exception as e:
            logger.error("Failed to connect to MySQL database: %s", e)
            raise

    def get_connection(self):
        try:
            if not self.pool:
                self.connect()
            conn = self.pool.get_connection()
            if not conn.is_connected():
                conn.reconnect(attempts=3, delay=1)
            return conn
        except Exception:
            # Recreate pool if stale or closed
            self.connect()
            return self.pool.get_connection()

    def auto_register_controlled_bots(self, bot_names: List[str]) -> None:
        """Ensure bots configured in AzerothFriend.ControlledBots exist in azeroth_friend_bots."""
        if not bot_names:
            return
        try:
            with self.get_connection() as conn:
                with conn.cursor(dictionary=True) as cursor:
                    format_strings = ','.join(['%s'] * len(bot_names))
                    cursor.execute(
                        f"SELECT guid, name FROM characters WHERE LOWER(name) IN ({format_strings})",
                        [n.lower() for n in bot_names]
                    )
                    found_chars = cursor.fetchall()
                    for char in found_chars:
                        c_guid = char["guid"]
                        c_name = char["name"]
                        cursor.execute("""
                            INSERT INTO azeroth_friend_bots (bot_guid, bot_name, enabled, mode, personality, current_goal)
                            VALUES (%s, %s, 1, 'companion', 'A loyal companion bot.', 'Assist master in adventures')
                            ON DUPLICATE KEY UPDATE bot_name = %s
                        """, (c_guid, c_name, c_name))
        except Exception as err:
            logger.warning("Auto-registration error: %s", err)

    def fetch_registered_bots(self) -> List[Dict[str, Any]]:
        query = ("SELECT " + BOT_CONTROL_COLUMNS + " FROM azeroth_friend_bots "
                 "WHERE enabled = 1 AND bridge_enabled = 1 ORDER BY bot_guid")
        with self.get_connection() as conn:
            with conn.cursor(dictionary=True) as cursor:
                cursor.execute(query)
                return cursor.fetchall()

    def verify_schema(self) -> None:
        """Fail fast when the module schema is incomplete.

        Each probe uses its own connection and drains its result set. Reusing one
        cursor for two statements raises mysql.connector's "Unread result found",
        which then poisons reset_session() when the pooled connection is returned.
        """
        checks = (
            ("azeroth_friend_bots",
             "bridge_enabled, autonomy_enabled, goal_status, goal_progress, goal_result, control_revision"),
            ("azeroth_friend_actions", "failure_reason, result_json"),
            ("azeroth_friend_actions", "origin_event_id, authority, origin_source_guid"),
            ("azeroth_friend_action_catalog",
             "api_version, params_schema_json, result_schema_json, binding_kind, native_binding, authority, preconditions, completion_policy, capability_revision"),
        )
        for table, columns in checks:
            with self.get_connection() as conn:
                with conn.cursor() as cur:
                    cur.execute(f"SELECT {columns} FROM {table} LIMIT 1")
                    cur.fetchall()

    def fetch_action_catalog(self, bot_guid: int) -> List[Dict[str, Any]]:
        with self.get_connection() as conn, conn.cursor(dictionary=True) as cur:
            cur.execute("SELECT action_name, category, source, params_schema, api_version, params_schema_json, "
                        "result_schema_json, binding_kind, native_binding, authority, preconditions, "
                        "completion_policy, capability_revision FROM azeroth_friend_action_catalog "
                        "WHERE bot_guid = %s AND safe = 1 ORDER BY source, action_name", (bot_guid,))
            return cur.fetchall()

    def fetch_action_history(self, bot_guid: int) -> List[Dict[str, Any]]:
        with self.get_connection() as conn, conn.cursor(dictionary=True) as cur:
            cur.execute("SELECT id, action_type, status, failure_reason, result_json FROM azeroth_friend_actions "
                        "WHERE bot_guid = %s ORDER BY id DESC LIMIT 5", (bot_guid,))
            return cur.fetchall()

    def has_active_plan(self, bot_guid: int) -> bool:
        """Exact pending/in-progress lookup used instead of "latest five actions"."""
        with self.get_connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT 1 FROM azeroth_friend_actions WHERE bot_guid = %s "
                        "AND status IN ('pending','in_progress') LIMIT 1", (bot_guid,))
            return cur.fetchone() is not None

    def fetch_active_plan(self, bot_guid: int, limit: int = 5) -> List[Dict[str, Any]]:
        """Compact in-progress plan, used for plan progress and reconciliation."""
        with self.get_connection() as conn, conn.cursor(dictionary=True) as cur:
            cur.execute(
                "SELECT id, plan_id, step_index, total_steps, action_type, status, params_json "
                "FROM azeroth_friend_actions WHERE bot_guid = %s "
                "AND status IN ('pending','in_progress') ORDER BY plan_id, step_index LIMIT %s",
                (bot_guid, int(limit)),
            )
            return cur.fetchall()

    def fetch_verified_outcomes(self, bot_guid: int, limit: int = 10) -> List[Dict[str, Any]]:
        """Newly completed action results, used for reconciliation and summaries."""
        with self.get_connection() as conn, conn.cursor(dictionary=True) as cur:
            cur.execute(
                "SELECT " + ACTION_OUTCOME_COLUMNS + " FROM azeroth_friend_actions "
                "WHERE bot_guid = %s AND status IN ('completed','failed') "
                "ORDER BY id DESC LIMIT %s",
                (bot_guid, int(limit)),
            )
            return cur.fetchall()

    def fetch_control_state(self, bot_guid: int) -> Optional[Dict[str, Any]]:
        """Durable identity/goal record for the lazy per-bot cache."""
        with self.get_connection() as conn, conn.cursor(dictionary=True) as cur:
            cur.execute(
                "SELECT " + BOT_CONTROL_COLUMNS + " FROM azeroth_friend_bots WHERE bot_guid = %s",
                (bot_guid,),
            )
            return cur.fetchone()

    def fetch_control_states(self, bot_guids: List[int]) -> Dict[int, Dict[str, Any]]:
        """Batched control lookup for the registered companion set."""
        if not bot_guids:
            return {}
        placeholders = ",".join(["%s"] * len(bot_guids))
        with self.get_connection() as conn, conn.cursor(dictionary=True) as cur:
            cur.execute(
                "SELECT " + BOT_CONTROL_COLUMNS + " FROM azeroth_friend_bots "
                "WHERE bot_guid IN (%s)" % placeholders,
                list(bot_guids),
            )
            rows = cur.fetchall()
        return {int(row["bot_guid"]): row for row in rows}

    def fetch_bot_inventory(self, bot_guid: int) -> List[Dict[str, Any]]:
        """Fetch all items currently carried in the bot's backpack and equipped bags."""
        query = (
            f"SELECT ii.itemEntry AS entry, ii.count, it.name, it.class AS item_class, "
            f"it.subclass AS item_subclass, it.Quality AS quality "
            f"FROM character_inventory ci "
            f"JOIN item_instance ii ON ci.item = ii.guid "
            f"JOIN `{self.world_db}`.item_template it ON ii.itemEntry = it.entry "
            f"WHERE ci.guid = %s AND ( "
            f"    (ci.bag = 0 AND ci.slot >= 23 AND ci.slot <= 38) "
            f"    OR (ci.bag >= 19 AND ci.bag <= 22) "
            f") "
            f"ORDER BY ci.bag, ci.slot"
        )
        try:
            with self.get_connection() as conn, conn.cursor(dictionary=True) as cur:
                cur.execute(query, (int(bot_guid),))
                return cur.fetchall() or []
        except Exception as err:
            logger.debug("Failed to fetch inventory for bot %d: %s", bot_guid, err)
            return []

    # ------------------------------------------------------------------ summaries
    def upsert_summary(self, record: Any) -> bool:
        """Idempotently persist one structured summary (unique summary_key)."""
        payload = json.dumps(getattr(record, "payload", lambda: {})())
        with self.get_connection() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO azeroth_friend_summaries "
                    "(bot_guid, goal_identity, revision, window_start, window_end, content, "
                    "payload_json, source_outcome_ids, summary_key) "
                    "VALUES (%s, %s, %s, FROM_UNIXTIME(%s), FROM_UNIXTIME(%s), %s, %s, %s, %s) "
                    "ON DUPLICATE KEY UPDATE content = VALUES(content), payload_json = VALUES(payload_json), "
                    "source_outcome_ids = VALUES(source_outcome_ids), window_end = VALUES(window_end), "
                    "revision = VALUES(revision)",
                    (
                        int(record.bot_guid),
                        str(record.goal_identity or ""),
                        int(record.revision or 0),
                        float(record.window_start or 0.0),
                        float(record.window_end or 0.0),
                        str(record.content or ""),
                        payload,
                        json.dumps(list(record.source_outcome_ids or [])),
                        str(record.summary_key or ""),
                    ),
                )
                return cur.rowcount > 0

    def fetch_relevant_summaries(self, bot_guid: int, goal_identity: str = "",
                                 limit: int = 2) -> List[Dict[str, Any]]:
        """Latest summaries for the active goal (never a live target source)."""
        with self.get_connection() as conn, conn.cursor(dictionary=True) as cur:
            if goal_identity:
                cur.execute(
                    "SELECT " + SUMMARY_COLUMNS + " FROM azeroth_friend_summaries "
                    "WHERE bot_guid = %s AND goal_identity = %s ORDER BY id DESC LIMIT %s",
                    (bot_guid, goal_identity, int(limit)),
                )
            else:
                cur.execute(
                    "SELECT " + SUMMARY_COLUMNS + " FROM azeroth_friend_summaries "
                    "WHERE bot_guid = %s ORDER BY id DESC LIMIT %s",
                    (bot_guid, int(limit)),
                )
            return cur.fetchall()

    # ---------------------------------------------------------------- checkpoints
    def fetch_consumer_checkpoint(self, consumer: str) -> Dict[str, Any]:
        """Incremental cursor state for a chatter consumer (bounded overlap)."""
        with self.get_connection() as conn, conn.cursor(dictionary=True) as cur:
            cur.execute(
                "SELECT consumer, last_event_id, last_message_id, overlap_seconds, updated_at "
                "FROM azeroth_friend_consumer_checkpoints WHERE consumer = %s",
                (consumer,),
            )
            row = cur.fetchone()
        if not row:
            return {"consumer": consumer, "last_event_id": 0, "last_message_id": 0, "overlap_seconds": 30}
        return row

    def save_consumer_checkpoint(self, consumer: str, last_event_id: int = 0,
                                 last_message_id: int = 0, overlap_seconds: int = 30) -> None:
        with self.get_connection() as conn, conn.cursor() as cur:
            cur.execute(
                "INSERT INTO azeroth_friend_consumer_checkpoints "
                "(consumer, last_event_id, last_message_id, overlap_seconds) VALUES (%s, %s, %s, %s) "
                "ON DUPLICATE KEY UPDATE last_event_id = GREATEST(last_event_id, VALUES(last_event_id)), "
                "last_message_id = GREATEST(last_message_id, VALUES(last_message_id)), "
                "overlap_seconds = VALUES(overlap_seconds)",
                (consumer, int(last_event_id), int(last_message_id), int(overlap_seconds)),
            )

    # ------------------------------------------------------------------ retention
    def prune_diagnostic_records(self, retention_days: int = DIAGNOSTIC_RETENTION_DAYS,
                                 batch_size: int = PRUNE_BATCH_SIZE) -> int:
        """Bounded-batch cleanup of terminal diagnostic detail only.

        Active queues and every bot/character/playerbot record are never touched.
        """
        removed = 0
        with self.get_connection() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "DELETE FROM azeroth_friend_events WHERE status IN "
                    "('completed','expired','skipped','superseded','ignored_addon') "
                    "AND created_at < DATE_SUB(NOW(), INTERVAL %s DAY) LIMIT %s",
                    (int(retention_days), int(batch_size)),
                )
                removed += cur.rowcount or 0
                cur.execute(
                    "DELETE FROM azeroth_friend_actions WHERE status IN ('completed','failed','interrupted') "
                    "AND COALESCE(completed_at, updated_at) < DATE_SUB(NOW(), INTERVAL %s DAY) LIMIT %s",
                    (int(retention_days), int(batch_size)),
                )
                removed += cur.rowcount or 0
        return removed

    def control_is_current(self, bot_guid: int, revision: int) -> bool:
        with self.get_connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT control_revision FROM azeroth_friend_bots "
                        "WHERE bot_guid=%s AND enabled=1 AND bridge_enabled=1", (bot_guid,))
            row = cur.fetchone()
            return row is not None and int(row[0]) == revision

    def update_goal(self, bot_guid: int, revision: int, update: Dict[str, Any]) -> None:
        with self.get_connection() as conn, conn.cursor() as cur:
            cur.execute("UPDATE azeroth_friend_bots SET goal_status=%s, goal_progress=%s, goal_result=%s, "
                        "autonomy_enabled=IF(%s IN ('completed','blocked','paused'),0,autonomy_enabled) "
                        "WHERE bot_guid=%s AND control_revision=%s AND goal_status='active' AND bridge_enabled=1",
                        (update['status'], update.get('progress', ''), update.get('result', ''),
                         update['status'], bot_guid, revision))

    def replace_short_term_goal(self, bot_guid: int, revision: int, text: str) -> bool:
        """Adopt the companion's own next short-term goal.

        Guarded by control_revision so an owner command issued while the plan was
        being formed always wins over an autonomously derived objective. Re-enables
        the loop, because a completed goal had switched autonomy off.
        """
        if not bot_guid or not text:
            return False
        query = ("UPDATE azeroth_friend_bots SET current_goal=%s, goal_status='active', "
                 "autonomy_enabled=1, goal_progress='', goal_result='' "
                 "WHERE bot_guid=%s AND control_revision=%s AND bridge_enabled=1")
        with self.get_connection() as conn:
            with conn.cursor() as cursor:
                cursor.execute(query, (str(text)[:255], bot_guid, revision))
                return cursor.rowcount > 0

    def fetch_bot_state(self, bot_guid: int) -> Optional[Dict[str, Any]]:
        query = "SELECT * FROM azeroth_friend_state WHERE bot_guid = %s"
        with self.get_connection() as conn:
            with conn.cursor(dictionary=True) as cursor:
                cursor.execute(query, (bot_guid,))
                return cursor.fetchone()

    def fetch_pending_events(self) -> List[Dict[str, Any]]:
        query = """
            SELECT e.id, e.bot_guid, e.event_type, e.priority, e.source_guid, e.source_name, e.payload_json
            FROM azeroth_friend_events e
            INNER JOIN azeroth_friend_bots b ON b.bot_guid = e.bot_guid
            WHERE e.status = 'pending' AND b.enabled = 1 AND b.bridge_enabled = 1
            ORDER BY e.priority ASC, e.created_at ASC
            LIMIT 10
        """
        with self.get_connection() as conn:
            with conn.cursor(dictionary=True) as cursor:
                cursor.execute(query)
                events = cursor.fetchall()

                # If a bot has a pending player_command, supersede older plan_interrupted or idle_tick events
                command_bots = {e["bot_guid"] for e in events if e.get("event_type") == "player_command"}
                if command_bots:
                    for b_guid in command_bots:
                        try:
                            cursor.execute("""
                                UPDATE azeroth_friend_events
                                SET status = 'superseded', processed_at = NOW()
                                WHERE bot_guid = %s AND status = 'pending' AND event_type IN ('plan_interrupted', 'idle_tick')
                            """, (b_guid,))
                        except mysql.connector.Error as err:
                            if "1265" in str(err) or "Data truncated" in str(err):
                                cursor.execute("""
                                    UPDATE azeroth_friend_events
                                    SET status = 'skipped', processed_at = NOW()
                                    WHERE bot_guid = %s AND status = 'pending' AND event_type IN ('plan_interrupted', 'idle_tick')
                                """, (b_guid,))
                            else:
                                raise

                # Deduplicate concurrent events for the same bot with identical text/command
                unique_events = []
                seen_dialogue: Set[tuple] = set()
                duplicate_ids: List[int] = []

                for ev in events:
                    b_id = ev["bot_guid"]
                    ev_type = ev.get("event_type")
                    if b_id in command_bots and ev_type in ('plan_interrupted', 'idle_tick'):
                        continue
                    payload = ev.get("payload_json") or "{}"
                    txt = ""
                    try:
                        p_dict = json.loads(payload) if isinstance(payload, str) else payload
                        txt = (p_dict.get("text") or p_dict.get("command") or "").strip().lower()
                    except Exception:
                        pass

                    if txt and ev_type in ("player_command", "dialogue_heard"):
                        key = (b_id, txt)
                        if key in seen_dialogue:
                            duplicate_ids.append(ev["id"])
                            continue
                        seen_dialogue.add(key)

                    unique_events.append(ev)

                if duplicate_ids:
                    for d_id in duplicate_ids:
                        try:
                            cursor.execute(
                                "UPDATE azeroth_friend_events SET status = 'superseded', processed_at = NOW() WHERE id = %s",
                                (d_id,)
                            )
                        except mysql.connector.Error as err:
                            if "1265" in str(err) or "Data truncated" in str(err):
                                cursor.execute(
                                    "UPDATE azeroth_friend_events SET status = 'skipped', processed_at = NOW() WHERE id = %s",
                                    (d_id,)
                                )
                            else:
                                raise
                    logger.info("Deduplicated %d duplicate event(s) in pending queue: %s", len(duplicate_ids), duplicate_ids)

                conn.commit()
                return unique_events

    def mark_event_status(self, event_id: int, status: str) -> None:
        query = "UPDATE azeroth_friend_events SET status = %s, processed_at = NOW() WHERE id = %s"
        with self.get_connection() as conn:
            with conn.cursor() as cursor:
                try:
                    cursor.execute(query, (status, event_id))
                except mysql.connector.Error as err:
                    if "1265" in str(err) or "Data truncated" in str(err):
                        # Fallback for un-migrated enum tables
                        cursor.execute(query, ("skipped", event_id))
                    else:
                        raise

    def insert_action_plan(self, bot_guid: int, plan_id: str, plan_steps: List[Dict[str, Any]],
                           thought: str = "", revision: Optional[int] = None,
                           origin_event_id: Optional[int] = None,
                           authority: str = "autonomous",
                           origin_source_guid: Optional[int] = None) -> None:
        query = """
            INSERT INTO azeroth_friend_actions
            (bot_guid, plan_id, step_index, total_steps, action_type, params_json, status, thought,
             control_revision, origin_event_id, authority, origin_source_guid)
            VALUES (%s, %s, %s, %s, %s, %s, 'pending', %s, %s, %s, %s, %s)
        """
        total = len(plan_steps)
        import json
        if authority not in ("autonomous", "owner_command", "gm_manual"):
            authority = "autonomous"
        with self.get_connection() as conn:
            conn.start_transaction()
            with conn.cursor() as cursor:
                cursor.execute("SELECT control_revision FROM azeroth_friend_bots "
                               "WHERE bot_guid=%s AND enabled=1 AND bridge_enabled=1 FOR UPDATE", (bot_guid,))
                row = cursor.fetchone()
                if row is None or (revision is not None and int(row[0]) != revision):
                    conn.rollback()
                    return
                for idx, step in enumerate(plan_steps):
                    action_type = step.get("action", "")
                    params = dict(step.get("params", {}))
                    params['_control_revision'] = int(row[0])
                    params_json = json.dumps(params)
                    cursor.execute(query, (
                        bot_guid, plan_id, idx, total, action_type, params_json, thought,
                        int(row[0]), origin_event_id, authority, origin_source_guid,
                    ))
            conn.commit()
        logger.info("Inserted %d plan steps for bot %d (plan_id: %s)", total, bot_guid, plan_id)

    def update_bot_thought(self, bot_guid: int, thought: str) -> None:
        query = ("UPDATE azeroth_friend_state s INNER JOIN azeroth_friend_bots b ON b.bot_guid=s.bot_guid "
                 "SET s.last_thought=%s WHERE s.bot_guid=%s AND b.enabled=1 AND b.bridge_enabled=1")
        with self.get_connection() as conn:
            with conn.cursor() as cursor:
                cursor.execute(query, (thought, bot_guid))

    def fetch_recent_memories(self, bot_guid: int, limit: int = 4) -> List[str]:
        """Legacy episodic memory rows.

        These predate structured summaries and can contain ambiguous prose, so
        they are excluded from automatic prompts unless the operator explicitly
        opts in with AzerothFriend.Context.IncludeLegacyMemory = 1.
        """
        if not getattr(self, "include_legacy_memory", False):
            return []
        query = ("SELECT content FROM azeroth_friend_memory WHERE bot_guid = %s "
                 "ORDER BY importance DESC, created_at DESC LIMIT %s")
        with self.get_connection() as conn:
            with conn.cursor() as cursor:
                cursor.execute(query, (bot_guid, limit))
                rows = cursor.fetchall()
                return [row[0] for row in rows]

    def insert_memory(self, bot_guid: int, memory_type: str, content: str,
                      importance: int = 5) -> None:
        """Persist one episodic memory. Nothing wrote to this table before, so the
        addon's memory panel was permanently empty and prompts had no history."""
        if not bot_guid or not content:
            return
        try:
            importance = max(1, min(10, int(importance)))
        except (TypeError, ValueError):
            importance = 5
        query = ("INSERT INTO azeroth_friend_memory (bot_guid, memory_type, content, importance) "
                 "SELECT %s, %s, %s, %s FROM azeroth_friend_bots "
                 "WHERE bot_guid=%s AND enabled=1 AND bridge_enabled=1")
        with self.get_connection() as conn:
            with conn.cursor() as cursor:
                cursor.execute(query, (bot_guid, str(memory_type)[:32], str(content)[:1000],
                                       importance, bot_guid))

    def record_telemetry(self, co_processed: int, sensory_reused: int, tokens_saved: int) -> None:
        """Flush aggregated telemetry counters (bridge calls this when dirty)."""
        query = """
            INSERT INTO azeroth_friend_telemetry (id, co_processed_events, sensory_reused_events, tokens_saved, updated_at)
            VALUES (1, %s, %s, %s, NOW())
            ON DUPLICATE KEY UPDATE
                co_processed_events = %s,
                sensory_reused_events = %s,
                tokens_saved = %s,
                updated_at = NOW()
        """
        try:
            with self.get_connection() as conn:
                with conn.cursor() as cursor:
                    # Schema creation lives in data/sql/characters (migrations own
                    # the schema); the runtime loop only writes counters.
                    cursor.execute(query, (co_processed, sensory_reused, tokens_saved, co_processed, sensory_reused, tokens_saved))
        except Exception as e:
            logger.debug("Failed to record telemetry: %s", e)

    def verify_context_schema(self) -> None:
        """Fail fast when the RAM-first context migration has not been applied."""
        checks = (
            ("azeroth_friend_summaries", "goal_identity, revision, content, source_outcome_ids, summary_key"),
            ("azeroth_friend_consumer_checkpoints", "consumer, last_event_id, last_message_id, overlap_seconds"),
        )
        for table, columns in checks:
            with self.get_connection() as conn:
                with conn.cursor() as cur:
                    cur.execute(f"SELECT {columns} FROM {table} LIMIT 1")
                    cur.fetchall()

    def fetch_bot_learned_spells(self, bot_guid: int) -> List[Dict[str, Any]]:
        """Fetch all spells learned by bot character from character_spell, enriched with name and ranks."""
        # character_spell gained/lost columns across AzerothCore revisions
        # (`disabled` in older cores, `active` in newer ones, neither in some
        # repacks). Referencing a missing column makes the whole query fail and
        # silently degrade to nameless rows, so filter only on what exists.
        columns = self.character_spell_columns()
        if "disabled" in columns:
            spell_filter = " AND cs.disabled = 0"
        elif "active" in columns:
            spell_filter = " AND cs.active = 1"
        else:
            spell_filter = ""
        query_enriched = f"""
            SELECT cs.spell, COALESCE(d.Name_Lang_enUS, '') AS name,
                   COALESCE(d.NameSubtext_Lang_enUS, '') AS rank_text,
                   0 AS rank_num, COALESCE(d.ManaCost, 0) AS mana_cost,
                   COALESCE(d.RecoveryTime, 0) AS cooldown_ms, 0 AS cast_time_ms
            FROM `{self.database}`.character_spell cs
            LEFT JOIN `{self.world_db}`.spell_dbc d ON cs.spell = d.ID
            WHERE cs.guid = %s{spell_filter}
            ORDER BY name, rank_num DESC;
        """
        fallback_query = f"SELECT spell FROM `{self.database}`.character_spell WHERE guid = %s"
        try:
            with self.get_connection() as conn:
                with conn.cursor(dictionary=True) as cursor:
                    try:
                        cursor.execute(query_enriched, (bot_guid,))
                        rows = cursor.fetchall()
                        if rows and self.spell_index is not None:
                            self._fill_missing_spell_names(rows)
                        return rows or []
                    except Exception:
                        cursor.execute(fallback_query, (bot_guid,))
                        return [{"spell": r["spell"], "name": "", "rank_text": "", "rank_num": 0} for r in cursor.fetchall()]
        except Exception as e:
            logger.warning("Error fetching bot learned spells for guid %d: %s", bot_guid, e)
            return []

    def _fill_missing_spell_names(self, rows: List[Dict[str, Any]]) -> None:
        """Backfill name/rank from Spell.dbc for spells the world DB does not describe."""
        missing = []
        for row in rows:
            if not str(row.get("name") or "").strip():
                try:
                    missing.append(int(row.get("spell") or 0))
                except (TypeError, ValueError):
                    continue
        if not missing:
            return
        try:
            resolved = self.spell_index.lookup_many(missing)
        except Exception as e:
            logger.debug("Spell.dbc lookup failed: %s", e)
            return
        for row in rows:
            try:
                info = resolved.get(int(row.get("spell") or 0))
            except (TypeError, ValueError):
                info = None
            if not info:
                continue
            if not str(row.get("name") or "").strip():
                row["name"] = info.get("name", "")
            if not str(row.get("rank_text") or "").strip():
                row["rank_text"] = info.get("rank_text", "")

    def character_spell_columns(self) -> set:
        """Column names present on character_spell, inspected once per process."""
        if self._char_spell_columns is None:
            columns = set()
            try:
                with self.get_connection() as conn:
                    with conn.cursor() as cursor:
                        cursor.execute(
                            "SELECT COLUMN_NAME FROM information_schema.COLUMNS "
                            "WHERE TABLE_SCHEMA = %s AND TABLE_NAME = 'character_spell'",
                            (self.database,),
                        )
                        columns = {str(row[0]).lower() for row in cursor.fetchall()}
            except Exception as e:
                logger.debug("Could not inspect character_spell schema: %s", e)
            self._char_spell_columns = columns
        return self._char_spell_columns

    def query_spell_by_name(self, spell_name: str, rank: Optional[int] = None) -> Optional[Dict[str, Any]]:
        """Query spell definition by name (case-insensitive) and optional rank."""
        if not spell_name:
            return None
        clean_name = spell_name.strip()
        try:
            with self.get_connection() as conn:
                with conn.cursor(dictionary=True) as cursor:
                    # 1. Try azeroth_friend_spells table first
                    try:
                        if rank is not None and rank > 0:
                            q = f"SELECT * FROM `{self.world_db}`.azeroth_friend_spells WHERE LOWER(name) = LOWER(%s) AND rank_num = %s LIMIT 1"
                            cursor.execute(q, (clean_name, rank))
                        else:
                            q = f"SELECT * FROM `{self.world_db}`.azeroth_friend_spells WHERE LOWER(name) = LOWER(%s) ORDER BY rank_num DESC LIMIT 1"
                            cursor.execute(q, (clean_name,))
                        row = cursor.fetchone()
                        if row:
                            return row
                    except Exception:
                        pass

                    # 2. Fallback to spell_dbc table in world_db
                    try:
                        if rank is not None and rank > 0:
                            q = f"SELECT ID AS spell_id, Name_Lang_enUS AS name, NameSubtext_Lang_enUS AS rank_text, ManaCost AS mana_cost, RecoveryTime AS cooldown_ms FROM `{self.world_db}`.spell_dbc WHERE LOWER(Name_Lang_enUS) = LOWER(%s) AND NameSubtext_Lang_enUS LIKE %s LIMIT 1"
                            cursor.execute(q, (clean_name, f"%{rank}%"))
                        else:
                            q = f"SELECT ID AS spell_id, Name_Lang_enUS AS name, NameSubtext_Lang_enUS AS rank_text, ManaCost AS mana_cost, RecoveryTime AS cooldown_ms FROM `{self.world_db}`.spell_dbc WHERE LOWER(Name_Lang_enUS) = LOWER(%s) ORDER BY ID DESC LIMIT 1"
                            cursor.execute(q, (clean_name,))
                        row = cursor.fetchone()
                        if row:
                            return row
                    except Exception:
                        pass
        except Exception as e:
            logger.debug("Database query for spell '%s' failed: %s", clean_name, e)
        return None

    def query_spell_by_id(self, spell_id: int) -> Optional[Dict[str, Any]]:
        """Query spell metadata by spell ID."""
        if not spell_id or spell_id <= 0:
            return None
        try:
            with self.get_connection() as conn:
                with conn.cursor(dictionary=True) as cursor:
                    try:
                        cursor.execute(f"SELECT * FROM `{self.world_db}`.azeroth_friend_spells WHERE spell_id = %s LIMIT 1", (spell_id,))
                        row = cursor.fetchone()
                        if row:
                            return row
                    except Exception:
                        pass
                    try:
                        cursor.execute(f"SELECT ID AS spell_id, Name_Lang_enUS AS name, NameSubtext_Lang_enUS AS rank_text, ManaCost AS mana_cost, RecoveryTime AS cooldown_ms FROM `{self.world_db}`.spell_dbc WHERE ID = %s LIMIT 1", (spell_id,))
                        row = cursor.fetchone()
                        if row:
                            return row
                    except Exception:
                        pass
        except Exception as e:
            logger.debug("Database query for spell_id %d failed: %s", spell_id, e)
        return None
