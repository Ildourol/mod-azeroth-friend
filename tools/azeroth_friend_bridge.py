#!/usr/bin/env python3
"""Main daemon bridge for mod-azeroth-friend.

Runs independently of worldserver, connecting via MySQL database and LLM endpoints.
Integrates Token Sharing and Sensory Reuse with mod-llm-chatter.
"""

import argparse
import json
import logging
import os
import sys
import time
import uuid
from typing import Any, Dict, Optional

import mysql.connector

from friend_co_processor import ChatterCoProcessor, TokenSavingsTracker
from friend_chatter_consumer import ChatterSensoryConsumer
from friend_context import ContextBudget, ContextBuilder
from friend_db import DatabaseManager, parse_conf_file
from friend_livestate import LiveStateCache, LiveStateTransport
from friend_llm import LLMClient
from friend_memory import WorkingMemory
from friend_mindset import MindsetManager
from friend_planner import CognitivePlanner
from friend_goals import GoalScheduler, select_bots, validate_goal_update, generate_starter_goals
from friend_summaries import SummaryEngine, goal_identity, outcomes_from_action_rows
from friend_spells import SpellDatabaseResolver
from friend_command_parser import parse_cast_command, parse_slash_intent, match_bag_item_request
from friend_action_catalog import event_authority
from spell_dbc import SpellNameIndex, find_spell_dbc

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] [%(name)s] %(message)s",
    handlers=[logging.StreamHandler(sys.stdout)],
)
logger = logging.getLogger("azeroth_friend.bridge")


def config_bool(config: Dict[str, str], key: str, default: bool = False) -> bool:
    """Read a permissive boolean without making bridge startup key-shape dependent."""
    fallback = "1" if default else "0"
    return str(config.get(key, fallback)).strip().lower() in ("1", "true", "yes", "on")


def event_command_text(event: Dict[str, Any]) -> str:
    """Pull the spoken/typed text out of an event payload."""
    payload = event.get("payload_json") or "{}"
    try:
        data = json.loads(payload) if isinstance(payload, str) else payload
    except Exception:
        return ""
    if not isinstance(data, dict):
        return ""
    return str(data.get("text") or data.get("command") or "").strip()


def format_thought(text: str, source: str = "deterministic") -> str:
    """Render a thought in the field format the addon parses.

    The Thoughts tab reads `Key: value` segments (Thought/Mindset/Source). Writing
    plain prose here made the panel silently ignore deterministic replies, so it
    kept showing a stale plan thought.
    """
    return "Thought: %s | Source: %s" % (str(text).strip(), source)


def event_memory(event: Dict[str, Any], state: Dict[str, Any]) -> Optional[tuple]:
    """Return (memory_type, content, importance) for events worth remembering.

    Nothing wrote to azeroth_friend_memory before, so the addon's episodic memory
    panel was permanently empty and prompts carried no history.
    """
    etype = event.get("event_type")
    payload = event.get("payload_json") or "{}"
    try:
        data = json.loads(payload) if isinstance(payload, str) else payload
    except Exception:
        data = {}
    if not isinstance(data, dict):
        data = {}

    name = str(event.get("source_name") or data.get("opponent_name") or "").strip()

    if etype == "combat_enter" and name:
        return ("combat", "Engaged %s." % name, 5)
    if etype == "duel_ended":
        opponent = str(data.get("opponent_name") or name or "the challenger")
        outcome = "Won" if data.get("bot_won") else "Lost"
        return ("duel", "%s a duel against %s." % (outcome, opponent), 6)
    if etype == "plan_interrupted":
        reason = str(data.get("reason") or "").lower()
        if "died" in reason or "death" in reason:
            return ("death", "Was defeated in combat and recovered.", 7)
    if etype == "trade_requested":
        return ("social", "Traded with %s." % (name or "the owner"), 4)
    return None


def attach_spell_dbc_index(db_mgr: DatabaseManager, config: Dict[str, str], config_path: str) -> None:
    """Point the DB layer at Spell.dbc so spell names are complete.

    The world database's spell_dbc table only covers a subset of spells on most
    repacks; without the DBC a large part of every spellbook has no name and
    cannot be matched from natural language.
    """
    if config.get("AzerothFriend.SpellSync.Enable", "1") != "1":
        return
    config_dir = os.path.dirname(os.path.abspath(config_path))
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [config.get("AzerothFriend.SpellSync.DbcPath", "")]
    for depth in ("..", os.path.join("..", ".."), os.path.join("..", "..", "..")):
        candidates.append(os.path.join(config_dir, depth, "data", "dbc"))
        candidates.append(os.path.join(config_dir, depth, "Azerothcore server", "Server", "data", "dbc"))
        candidates.append(os.path.join(config_dir, depth, "Server", "data", "dbc"))
        candidates.append(os.path.join(tools_dir, depth, "data", "dbc"))
        candidates.append(os.path.join(tools_dir, depth, "Azerothcore server", "Server", "data", "dbc"))
        candidates.append(os.path.join(tools_dir, depth, "Server", "data", "dbc"))
    path = find_spell_dbc(candidates)
    if not path:
        logger.info("Spell.dbc not found; spell names come from the world database only.")
        return
    try:
        db_mgr.spell_index = SpellNameIndex(path)
        logger.info("Spell.dbc name index attached: %s", path)
    except Exception as e:
        logger.warning("Could not read %s: %s", path, e)


def run_healthcheck(
    config: Dict[str, str],
    db_mgr: DatabaseManager,
    llm_client: LLMClient,
    co_processor: ChatterCoProcessor,
) -> bool:
    logger.info("=== Running AzerothFriend Preflight Diagnostics ===")
    success = True

    # 1. Test DB Connection
    try:
        db_mgr.connect()
        bots = db_mgr.fetch_registered_bots()
        logger.info("[PASS] MySQL connection successful. Active bot companions found: %d", len(bots))
    except Exception as e:
        logger.error("[FAIL] MySQL connection failed: %s", e)
        success = False

    # 2. Test LLM Endpoint & Thinking Capabilities
    try:
        diag = llm_client.get_thinking_diagnostics("player_command")
        logger.info(
            "[INFO] LLM Config: Provider=%s, Model=%s, ClassifiedType=%s, Mode=%s, Effort=%s (effective: %s), TokenReserve=%d",
            diag["provider"],
            diag["model"],
            diag["classified_type"],
            diag["mode"],
            diag["configured_effort"],
            diag["effective_effort"],
            diag["token_reserve"],
        )

        test_messages = [
            {"role": "system", "content": "You are a healthcheck probe. Respond in valid JSON."},
            {"role": "user", "content": "Return a plan containing a single emote action."},
        ]
        res = llm_client.generate_json_plan(test_messages, event_type="player_command")
        if res:
            logger.info("[PASS] LLM endpoint probe successful. Model: %s (ThinkingApplied=%s)",
                        llm_client.model, diag["thinking_applied"])
        else:
            logger.error("[FAIL] LLM endpoint returned empty response.")
            success = False
    except Exception as e:
        logger.error("[FAIL] LLM endpoint unreachable: %s", e)
        success = False

    # 3. Test mod-llm-chatter Token Sharing & Sensory Tables
    try:
        with db_mgr.get_connection() as conn:
            with conn.cursor() as cur:
                chatter_ok = co_processor.verify_chatter_tables(cur)
                if chatter_ok:
                    logger.info("[PASS] mod-llm-chatter tables verified (`llm_chatter_messages`, `llm_chatter_events`). Token Sharing: ACTIVE")
                else:
                    logger.info("[INFO] mod-llm-chatter tables not detected. Running in standalone action mode.")
    except Exception as e:
        logger.warning("[WARN] Failed to verify mod-llm-chatter tables: %s", e)

    if success:
        logger.info("=== All Diagnostics PASSED ===")
    else:
        logger.error("=== Preflight Diagnostics FAILED ===")
    return success


def main() -> int:
    parser = argparse.ArgumentParser(description="AzerothFriend Python AI Bridge")
    parser.add_argument("--config", type=str, required=True, help="Path to mod_azeroth_friend.conf")
    parser.add_argument("--healthcheck", action="store_true", help="Run preflight checks and exit")
    args = parser.parse_args()

    if not os.path.exists(args.config):
        logger.error("Config file not found: %s", args.config)
        return 1

    config = parse_conf_file(args.config)
    db_mgr = DatabaseManager(config)
    attach_spell_dbc_index(db_mgr, config, args.config)
    spell_sync_enabled = config.get("AzerothFriend.SpellSync.Enable", "1") == "1"
    SpellDatabaseResolver.get_instance(db_mgr, enabled=spell_sync_enabled)
    if not spell_sync_enabled:
        logger.info("AzerothFriend.SpellSync.Enable = 0: using the live spellbook only for spell resolution.")
    scheduler = GoalScheduler(int(config.get('AzerothFriend.Autonomy.HeartbeatSeconds', '60')),
                              int(config.get('AzerothFriend.Autonomy.MinimumPlanSeconds', '10')))
    llm_client = LLMClient(config)
    memory = WorkingMemory()

    tracker = TokenSavingsTracker()
    co_processor = ChatterCoProcessor(config, tracker)
    chatter_consumer = ChatterSensoryConsumer(config, tracker, db=db_mgr, memory=memory)
    chatter_consumer.observations_only = True

    # RAM-first live state: the C++ worker publishes value snapshots over a
    # loopback socket; SQL checkpoints are recovery data only.
    live_state_enabled = config.get("AzerothFriend.LiveState.Enable", "1") == "1"
    live_state_secret = config.get("AzerothFriend.LiveState.Secret", "")
    live_state_cache = LiveStateCache()
    live_state_transport = LiveStateTransport(
        host=config.get("AzerothFriend.LiveState.Host", "127.0.0.1"),
        port=int(config.get("AzerothFriend.LiveState.Port", "8377")),
        secret=live_state_secret,
        cache=live_state_cache,
        enabled=live_state_enabled,
    )
    # An unconfigured transport is an explicit, loud rollback to the SQL path -
    # never a silent switch to stale state.
    sql_compatibility_mode = (config.get("AzerothFriend.Context.SqlCompatibilityMode", "0") == "1"
                              or not (live_state_enabled and live_state_secret))
    if sql_compatibility_mode and config.get("AzerothFriend.Context.SqlCompatibilityMode", "0") != "1":
        logger.warning(
            "RAM-first transport is not configured (LiveState.Enable=%s, secret set=%s): "
            "using the SQL compatibility path. Set AzerothFriend.LiveState.Secret to enable RAM-first reads.",
            "1" if live_state_enabled else "0", "yes" if live_state_secret else "no",
        )
    require_live_state = config.get("AzerothFriend.LiveState.RequireFresh", "1") == "1"
    compact_context = config.get("AzerothFriend.Context.Compact.Enable", "1") == "1"
    context_builder = None
    if compact_context:
        context_builder = ContextBuilder(ContextBudget(
            target_input_tokens=int(config.get("AzerothFriend.Context.TargetInputTokens", "2000")),
            max_input_tokens=int(config.get("AzerothFriend.Context.MaxInputTokens", "3500")),
        ))
    summary_engine = SummaryEngine(
        db_mgr, memory,
        interval_seconds=float(config.get("AzerothFriend.Summary.IntervalSeconds", "60")),
    )

    session_context_reuse = config.get("AzerothFriend.SessionContextReuse.Enable", "1") == "1"
    acknowledge_commands = config.get("AzerothFriend.Chat.AcknowledgeCommands", "1") == "1"
    # 1 = mod-llm-chatter owns ambient banter (companion authors no lines).
    # 0 = the companion writes its own short lines for non-command events.
    delegate_ambient_speech = config.get("AzerothFriend.Chat.DelegateAmbientToLLMChatter", "1") == "1"
    mindset_enable = config.get("AzerothFriend.Mindset.Enable", "1") == "1"
    mindset_commitment = float(config.get("AzerothFriend.Mindset.CommitmentSeconds", "15"))
    mindset_opportunistic = config.get("AzerothFriend.Mindset.AllowOpportunistic", "1") == "1"
    mindset_manager = MindsetManager(
        enabled=mindset_enable,
        commitment_seconds=mindset_commitment,
        allow_opportunistic=mindset_opportunistic,
    )

    planner = CognitivePlanner(
        llm_client,
        memory,
        session_context_reuse=session_context_reuse,
        co_processor=co_processor,
        mindset_manager=mindset_manager,
        chatter_consumer=chatter_consumer,
        max_plan_steps=int(config.get("AzerothFriend.MultiActionPlan.MaxSteps", "5")),
        companion_speech_allowed=not delegate_ambient_speech,
        context_builder=context_builder,
        live_state=live_state_cache,
        require_live_state=require_live_state and not sql_compatibility_mode,
        max_context_tokens=int(config.get("AzerothFriend.Context.MaxInputTokens", "3500")),
        max_prompt_capabilities=int(config.get("AzerothFriend.BotApi.MaxPromptCapabilities", "90")),
        allow_raw_passthrough=config_bool(
            config, "AzerothFriend.PlayerbotActions.AllowRawPassthrough", False),
    )

    controlled_bots_raw = config.get("AzerothFriend.ControlledBots", "Friendbot,Ollamatest")
    max_controlled = int(config.get("AzerothFriend.MaxControlledBots", config.get("AzerothFriend.ControlledBots.MaxCount", "1")))
    allowed_bot_names = [name.strip().lower() for name in controlled_bots_raw.split(",") if name.strip()]
    if max_controlled > 0 and len(allowed_bot_names) > max_controlled:
        allowed_bot_names = allowed_bot_names[:max_controlled]

    if args.healthcheck:
        return 0 if run_healthcheck(config, db_mgr, llm_client, co_processor) else 1

    db_mgr.connect()
    db_mgr.verify_schema()
    try:
        db_mgr.verify_context_schema()
    except Exception as schema_err:
        logger.error(
            "RAM-first context migration is missing (%s). Import "
            "data/sql/characters/updates/2026_09_20_azeroth_friend_ram_first_context.sql",
            schema_err,
        )
        return 1
    if config.get('AzerothFriend.Enable', '1') != '1':
        logger.info('Module disabled in config; exiting without planning.')
        return 0
    if allowed_bot_names:
        db_mgr.auto_register_controlled_bots(allowed_bot_names)

    live_state_transport.start()
    if live_state_transport.enabled:
        logger.info("Live-state transport started towards %s:%d (RAM-first mode)",
                    live_state_transport.host, live_state_transport.port)
    else:
        logger.info("Live-state transport disabled; running in SQL compatibility mode.")
    try:
        chatter_consumer.configure_checkpoint(db_mgr.fetch_consumer_checkpoint("sensory"))
    except Exception as checkpoint_err:
        logger.debug("No chatter checkpoint available yet: %s", checkpoint_err)

    logger.info(
        "AzerothFriend Python Bridge started successfully with Token Sharing & Sensory Reuse. Controlled bots: %s (max: %d)",
          allowed_bot_names if allowed_bot_names else "NONE", max_controlled
    )

    last_sensory_poll = 0.0
    last_telemetry_sync = 0.0
    last_memory: Dict[tuple, float] = {}
    last_auto_reg_poll = 0.0
    last_bot_refresh = 0.0
    bot_refresh_seconds = float(config.get("AzerothFriend.Cache.BotRefreshSeconds", "15"))
    registered_bots: Dict[int, Dict[str, Any]] = {}
    last_summary_flush = 0.0
    last_retention_prune = 0.0
    last_diag_send = 0.0
    last_telemetry_signature: Optional[tuple] = None
    last_context_tokens = 0
    last_outcome_id_seen: Dict[int, int] = {}
    last_budget_diagnostic = ""

    try:
        while True:
            try:
                now = time.time()
                if allowed_bot_names and (now - last_auto_reg_poll >= 30.0):
                    last_auto_reg_poll = now
                    db_mgr.auto_register_controlled_bots(allowed_bot_names)

                # Durable identity/goals are read lazily: a full bot-table read only
                # happens on startup, on the slow refresh cadence, or when live state
                # reports a control revision that the cached record does not know.
                revision_changed = any(
                    int((live_state_cache.snapshot(guid) or {}).get("control_revision", info.get("control_revision", 0)))
                    != int(info.get("control_revision", 0))
                    for guid, info in registered_bots.items()
                )
                if not registered_bots or revision_changed or (now - last_bot_refresh >= bot_refresh_seconds):
                    last_bot_refresh = now
                    previous_bot_guids = set(registered_bots)
                    all_bots = db_mgr.fetch_registered_bots()
                    registered_bots = select_bots(all_bots, allowed_bot_names, max_controlled)
                    for disconnected_guid in previous_bot_guids - set(registered_bots):
                        memory.forget(disconnected_guid)
                        live_state_cache.remove(disconnected_guid)
                        chatter_consumer.forget_bot(disconnected_guid)
                        summary_engine.forget_bot(disconnected_guid)
                        last_outcome_id_seen.pop(disconnected_guid, None)
                        logger.info(
                            "[CLAIM] Bot %d removed from bridge caches (bridge disconnected)",
                            disconnected_guid,
                        )
                    for bot_guid, b_info in registered_bots.items():
                        memory.set_goals(bot_guid, b_info)
                        if b_info.get("personality"):
                            memory.set_personality(bot_guid, str(b_info.get("personality")))

                # Automatic starter goal generation for newly controlled bots
                if config.get("AzerothFriend.Goal.AutoGenerate.Enable", "1") == "1":
                    for bot_guid, b_info in registered_bots.items():
                        if (not b_info.get("current_goal") and not b_info.get("long_term_goal")
                                and db_mgr.control_is_current(
                                    bot_guid, int(b_info.get("control_revision") or 0))):
                            st_goal, lt_goal = generate_starter_goals(
                                b_info,
                                categories_config=config.get("AzerothFriend.Goal.AutoGenerate.Categories", "mixed"),
                                llm_client=llm_client
                            )
                            new_status = "active" if b_info.get("autonomy_enabled") else "paused"
                            try:
                                with db_mgr.get_connection() as conn:
                                    with conn.cursor() as cur:
                                        cur.execute(
                                            "UPDATE azeroth_friend_bots SET current_goal = %s, long_term_goal = %s, "
                                            "goal_status = %s, control_revision = control_revision + 1 "
                                            "WHERE bot_guid = %s AND enabled = 1 AND bridge_enabled = 1",
                                            (st_goal, lt_goal, new_status, bot_guid)
                                        )
                                        conn.commit()
                                b_info["current_goal"] = st_goal
                                b_info["long_term_goal"] = lt_goal
                                b_info["goal_status"] = new_status
                                b_info["control_revision"] = int(b_info.get("control_revision") or 0) + 1
                                logger.info("[GOAL] Initialized auto-generated goals for bot %d (%s): Short='%s', Long='%s', Status='%s'",
                                            bot_guid, b_info.get("bot_name"), st_goal, lt_goal, new_status)
                            except Exception as goal_err:
                                logger.debug("[GOAL] Failed to save starter goals: %s", goal_err)

                # 1. Zero-Token Sensory Consumption from mod-llm-chatter
                if now - last_sensory_poll >= 2.0:
                    last_sensory_poll = now
                    try:
                        chatter_consumer.set_live_states({
                            guid: live_state_cache.snapshot(guid)
                            for guid in registered_bots
                            if live_state_cache.snapshot(guid) is not None
                        })
                        with db_mgr.get_connection() as conn:
                            with conn.cursor() as cur:
                                # Passive listening cannot enqueue physical actions outside opt-in.
                                autonomous = {g: b for g, b in registered_bots.items()
                                              if b.get('autonomy_enabled') and b.get('goal_status') == 'active'}
                                if config.get('AzerothFriend.Autonomy.Enable', '1') == '1':
                                    chatter_consumer.process_chatter_sensory_events(cur, autonomous)
                                chatter_consumer.ingest_recent_chatter_messages(cur, registered_bots)
                    except Exception as sensory_err:
                        logger.debug("Sensory poll error: %s", sensory_err)

                # 2. Process High-Cognition Pending Events
                events = db_mgr.fetch_pending_events()
                if not events:
                    time.sleep(1.0)
                    continue

                deferred_live_state = False
                for event in events:
                    event_id = event["id"]
                    bot_guid = event["bot_guid"]

                    if bot_guid not in registered_bots:
                        db_mgr.mark_event_status(event_id, "skipped")
                        continue

                    # Filter out addon IPC packets (e.g. MBOT\t, AIO\t)
                    payload_raw = event.get("payload_json") or ""
                    if "MBOT\t" in payload_raw or "MBOT" in payload_raw or "AIO\t" in payload_raw:
                        db_mgr.mark_event_status(event_id, "ignored_addon")
                        continue

                    # MAF-019 Backlog shedding: if queue is congested, shed low-priority ambient events
                    if len(events) > 10 and event.get("event_type") in ("idle_tick", "dialogue_heard", "combat_leave"):
                        db_mgr.mark_event_status(event_id, "skipped")
                        continue

                    bot_info = registered_bots[bot_guid]
                    # Live surroundings come from RAM only. SQL checkpoints are
                    # recovery data and are consulted exclusively in the explicit
                    # SQL compatibility mode (never as "live" state).
                    state = live_state_cache.snapshot(bot_guid)
                    if state is None and sql_compatibility_mode:
                        sql_state = db_mgr.fetch_bot_state(bot_guid)
                        if sql_state:
                            state = dict(sql_state)
                            state["live_state"] = False
                    if not state:
                        # No live baseline yet (restart, reconnect, first sighting):
                        # leave the event pending and wait for fresh state instead of
                        # planning against a stale SQL checkpoint.
                        live_state_cache.request_refresh(bot_guid)
                        if event.get("event_type") != "player_command":
                            deferred_live_state = True
                            break
                        state = {"health_pct": 100, "in_combat": False, "live_state": False}

                    # Keep the lazy per-bot caches hydrated from the live control
                    # section, and persist goal changes into the durable record cache.
                    if state.get("control_revision") is not None:
                        bot_info = dict(bot_info)
                        bot_info["control_revision"] = int(state.get("control_revision") or bot_info.get("control_revision", 0))
                        for key in ("current_goal", "long_term_goal", "goal_status", "autonomy_enabled", "bridge_enabled"):
                            if state.get(key) is not None:
                                bot_info[key] = state.get(key)
                        memory.set_goals(bot_guid, bot_info)

                    if event.get('event_type') == 'player_command':
                        # Issue 2: A player command incremented control_revision in the database via InvalidateControl.
                        # Synchronize local revision with the database so the command is not falsely marked superseded.
                        try:
                            with db_mgr.get_connection() as conn, conn.cursor() as cur:
                                cur.execute("SELECT control_revision, bridge_enabled FROM azeroth_friend_bots WHERE bot_guid = %s", (bot_guid,))
                                crow = cur.fetchone()
                                if crow:
                                    if not crow[1]:
                                        logger.info("[COMMAND] Bot %d bridge is disabled by claim; skipping.", bot_guid)
                                        db_mgr.mark_event_status(event_id, 'skipped')
                                        continue
                                    revision = int(crow[0])
                                    bot_info = dict(bot_info)
                                    bot_info['control_revision'] = revision
                        except Exception as sync_err:
                            logger.debug("Failed to sync control_revision for bot %d: %s", bot_guid, sync_err)
                            revision = int(bot_info.get('control_revision', 0))
                    else:
                        revision = int(bot_info.get('control_revision', 0))

                    try:
                        origin_source_guid = int(event.get("source_guid") or 0)
                        master_guid = int(bot_info.get("master_guid") or 0)
                        if master_guid == 0 and state:
                            master_guid = int(state.get("master_guid") or (state.get("self_context") or {}).get("master", {}).get("guid") or 0)
                    except (TypeError, ValueError):
                        origin_source_guid, master_guid = 0, 0
                    action_authority = event_authority(
                        str(event.get("event_type") or ""), origin_source_guid, master_guid)
                    provenance = {
                        "origin_event_id": int(event_id),
                        "authority": action_authority,
                        "origin_source_guid": origin_source_guid or None,
                    }

                    if not db_mgr.control_is_current(bot_guid, revision):
                        db_mgr.mark_event_status(event_id, 'superseded')
                        continue
                    plan_active = db_mgr.has_active_plan(bot_guid)
                    active_plan = db_mgr.fetch_active_plan(bot_guid) if plan_active else []
                    state["active_plan"] = active_plan
                    # The exact pending/in-progress lookup already ran above, so the
                    # planner must not repeat a SQL probe for active-plan detection.
                    state["active_plan_checked"] = True
                    memory.set_active_plan(bot_guid, [
                        "%s (%s)" % (row.get("action_type"), row.get("status")) for row in active_plan
                    ])
                    history = db_mgr.fetch_verified_outcomes(bot_guid, limit=5)
                    # Feed newly verified outcomes into working memory and the
                    # zero-token summary engine instead of dumping detail in prompts.
                    for outcome in outcomes_from_action_rows(history, goal=str(bot_info.get("current_goal") or "")):
                        summary_engine.note_outcome(bot_guid, outcome)
                    if state.get("self_context") is not None:
                        memory.set_capabilities(
                            bot_guid, memory.get_capabilities(bot_guid), state.get("self_context"))

                    # Episodic memory: throttled so a busy fight cannot flood the table.
                    memory_entry = event_memory(event, state)
                    if memory_entry:
                        memory_key = (bot_guid, memory_entry[0])
                        if now - last_memory.get(memory_key, 0.0) >= 20.0:
                            last_memory[memory_key] = now
                            try:
                                db_mgr.insert_memory(bot_guid, memory_entry[0],
                                                     memory_entry[1], memory_entry[2])
                                logger.info("[MEMORY] Bot %d stored: %s", bot_guid, memory_entry[1])
                            except Exception as memory_error:
                                logger.debug("Memory write failed: %s", memory_error)

                    # Owner interactions: Keep trade window open instead of blind auto-accept.
                    if event.get('event_type') == 'trade_requested':
                        initiator = event.get('source_name') or "the owner"
                        thought = format_thought(f"Trade window opened with {initiator}. Waiting for trade items or instructions.", "trade")
                        plan_id = str(uuid.uuid4())[:8]
                        logger.info("[TRADE] Bot %d trade window opened with %s", bot_guid, initiator)
                        db_mgr.update_bot_thought(bot_guid, thought)
                        steps = []
                        if acknowledge_commands:
                            steps.append({"action": "say", "params": {"text": "Trade window open. Let me know what you need or ask me to link items."}})
                        if steps:
                            db_mgr.insert_action_plan(bot_guid, plan_id, steps, thought=thought,
                                                      revision=revision, **provenance)
                        db_mgr.mark_event_status(event_id, 'completed')
                        continue

                    # Combat reaction trigger: when master engages an enemy, immediately rush to assist on the fly!
                    if event.get('event_type') == 'master_engaged':
                        if not bot_info.get('autonomy_enabled'):
                            db_mgr.mark_event_status(event_id, 'skipped')
                            continue
                        enemy_name = event.get('source_name') or "enemy"
                        enemy_guid = event.get('source_guid') or 0
                        payload_raw = event.get('payload_json') or "{}"
                        try:
                            p_data = json.loads(payload_raw) if isinstance(payload_raw, str) else payload_raw
                            enemy_name = p_data.get('enemy_name') or enemy_name
                            enemy_guid = int(p_data.get('enemy_guid') or enemy_guid)
                        except Exception:
                            pass

                        thought = format_thought(f"Master engaged {enemy_name}! Moving to attack and assist immediately.", "combat")
                        plan_id = str(uuid.uuid4())[:8]
                        logger.info("[COMBAT] Bot %d master engaged %s; rushing to assist", bot_guid, enemy_name)
                        db_mgr.update_bot_thought(bot_guid, thought)
                        steps = [
                            {"action": "attack", "params": {"target": enemy_guid if enemy_guid else enemy_name}}
                        ]
                        db_mgr.insert_action_plan(bot_guid, plan_id, steps, thought=thought,
                                                  revision=revision, **provenance)
                        db_mgr.mark_event_status(event_id, 'completed')
                        continue

                    # Dialogue trigger: react to what was said when autonomous
                    if event.get('event_type') == 'dialogue_heard':
                        if not bot_info.get('autonomy_enabled'):
                            db_mgr.mark_event_status(event_id, 'skipped')
                            continue
                        speaker = event.get('source_name') or "someone"
                        payload_raw = event.get('payload_json') or "{}"
                        heard_text = ""
                        try:
                            p_data = json.loads(payload_raw) if isinstance(payload_raw, str) else payload_raw
                            heard_text = p_data.get('text') or ""
                        except Exception:
                            pass
                        if heard_text:
                            thought = format_thought(f"Overheard {speaker}: \"{heard_text}\". Considering context.", "social")
                            db_mgr.update_bot_thought(bot_guid, thought)
                        db_mgr.mark_event_status(event_id, 'completed')
                        continue

                    if event.get('event_type') != 'player_command' and config.get('AzerothFriend.Autonomy.Enable', '1') != '1':
                        db_mgr.mark_event_status(event_id, 'skipped')
                        continue
                    # The scheduler sees the exact pending/in-progress rows plus the
                    # verified outcomes, so active-plan detection never needs a
                    # "latest five actions" history scan.
                    if not scheduler.should_plan(bot_info, state, event, list(active_plan) + list(history)):
                        db_mgr.mark_event_status(event_id, 'skipped')
                        continue

                    # Relevant historical summaries are loaded lazily (max two,
                    # bound to the active goal identity). Legacy prose memories stay
                    # out of prompts unless the operator opted back in.
                    if not memory.get_summaries(bot_guid):
                        try:
                            memory.set_summaries(bot_guid, db_mgr.fetch_relevant_summaries(
                                bot_guid,
                                goal_identity(str(bot_info.get("current_goal") or ""),
                                              str(bot_info.get("goal_status") or "")),
                                2,
                            ))
                        except Exception as summary_err:
                            logger.debug("Summary lazy-load failed for bot %d: %s", bot_guid, summary_err)
                    memories = memory.get_summaries(bot_guid) + db_mgr.fetch_recent_memories(bot_guid)

                    # Owner commands that name a spell or slash action are translated straight
                    # and dispatched without a model call (0 tokens).
                    if event.get('event_type') == 'player_command':
                        cmd_text = event_command_text(event)
                        trade_info = state.get("vitals", {}).get("trade", {}) if isinstance(state, dict) else {}
                        is_trade_active = bool(trade_info.get("active", False))

                        # Live bag items lookup
                        bot_inventory = db_mgr.fetch_bot_inventory(bot_guid)
                        if isinstance(state, dict):
                            state["inventory_items"] = bot_inventory

                        # Check if player is requesting an item from bags by name, typo, or description
                        bag_match = match_bag_item_request(cmd_text, bot_inventory)
                        if bag_match:
                            matched_item, req_count = bag_match
                            matched_name = matched_item.get("name", "")
                            thought = format_thought(
                                f"Looking into my bags, I found {matched_name} x{req_count} for master. Opening trade.",
                                "trade"
                            )
                            plan_id = str(uuid.uuid4())[:8]
                            logger.info("[BAG_ITEM] Bot %d matched item '%s' (x%d) from carried inventory", bot_guid, matched_name, req_count)
                            db_mgr.update_bot_thought(bot_guid, thought)
                            steps = []
                            if not is_trade_active:
                                steps.append({"action": "playerbot_command", "params": {"command": "trade"}})
                            steps.append({"action": "trade_set_item", "params": {"item": matched_name, "count": req_count}})
                            if acknowledge_commands:
                                steps.append({"action": "say", "params": {"text": f"Found {matched_name} in my bags."}})
                            db_mgr.insert_action_plan(bot_guid, plan_id, steps, thought=thought,
                                                      revision=revision, **provenance)
                            db_mgr.mark_event_status(event_id, 'completed')
                            continue

                        slash_step = parse_slash_intent(
                            cmd_text,
                            speaker=event.get('source_name'),
                            is_trade_active=is_trade_active
                        )
                        if slash_step:
                            action_name = slash_step['action']
                            if action_name in ("stay", "hold", "stop"):
                                mindset_manager.set_mindset(bot_guid, "RESTING", custom_duration=300.0, thought="Holding position as ordered by master.")
                            elif action_name in ("follow",):
                                mindset_manager.set_mindset(bot_guid, "FOLLOWING", custom_duration=15.0, thought="Following master.")

                            thought = format_thought(
                                f"Owner command matched slash action '{action_name}'. No model call needed.",
                                "slash"
                            )
                            plan_id = str(uuid.uuid4())[:8]
                            logger.info("[SLASH] Bot %d resolved owner command to %s (0 tokens)", bot_guid, action_name)
                            db_mgr.update_bot_thought(bot_guid, thought)
                            steps = []
                            if not is_trade_active and action_name in ("trade_set_gold", "trade_set_item"):
                                steps.append({"action": "playerbot_command", "params": {"command": "trade"}})
                            steps.append(slash_step)
                            if acknowledge_commands and action_name not in ("trade_link_items", "say"):
                                steps.append({"action": "say", "params": {"text": "On it."}})
                            db_mgr.insert_action_plan(bot_guid, plan_id, steps, thought=thought,
                                                      revision=revision, **provenance)
                            db_mgr.mark_event_status(event_id, 'completed')
                            continue

                        step, cast_error = parse_cast_command(
                            cmd_text, SpellDatabaseResolver.get_instance(), bot_guid
                        )
                        if step:
                            spell_label = f"{step['params'].get('spell')} ({step['params'].get('spellid')})"
                            target_label = step['params'].get('target')
                            thought = "Owner cast request resolved to " + spell_label + (
                                " on " + target_label if target_label else ""
                            ) + ". No model call was needed."
                            thought = format_thought(thought, "cast")
                            plan_id = str(uuid.uuid4())[:8]
                            logger.info("[CAST] Bot %d resolved owner command to %s", bot_guid, spell_label)
                            db_mgr.update_bot_thought(bot_guid, thought)
                            steps = [step]
                            if acknowledge_commands:
                                steps.append({"action": "say",
                                              "params": {"text": "On it."}})
                            db_mgr.insert_action_plan(bot_guid, plan_id, steps, thought=thought,
                                                      revision=revision, **provenance)
                            db_mgr.mark_event_status(event_id, 'completed')
                            continue
                        if cast_error and (cast_error.startswith("unknown_spell:")
                                           or cast_error.startswith("ambiguous_spell:")):
                            if cast_error.startswith("unknown_spell:"):
                                missing = cast_error.split(":", 1)[1]
                                thought = f"Owner asked to cast '{missing}', which is not in my spellbook."
                                reply = f"I don't know a spell called '{missing}'."
                            else:
                                options = cast_error.split(":", 2)[2]
                                thought = f"Owner's cast request is ambiguous; I know: {options}."
                                reply = f"Which one? I know: {options}."
                            thought = format_thought(thought, "cast")
                            logger.info("[CAST] Bot %d: %s", bot_guid, thought)
                            db_mgr.update_bot_thought(bot_guid, thought)
                            if acknowledge_commands:
                                plan_id = str(uuid.uuid4())[:8]
                                db_mgr.insert_action_plan(bot_guid, plan_id,
                                                          [{"action": "say", "params": {"text": reply}}],
                                                          thought=thought, revision=revision, **provenance)
                            db_mgr.mark_event_status(event_id, 'completed')
                            continue

                    # Capabilities (action catalogue) are loaded once and refreshed
                    # only when the live-state capabilities section changes, instead
                    # of reading the catalogue on every processed event.
                    capability_revision = int((state.get("capabilities") or {}).get("revision", revision))
                    cached_catalog = memory.get_capabilities(bot_guid)
                    if not cached_catalog or memory.get_capability_revision(bot_guid) != capability_revision:
                        cached_catalog = db_mgr.fetch_action_catalog(bot_guid)
                        memory.set_capabilities(bot_guid, cached_catalog, state.get("self_context"))
                        memory.set_capability_revision(bot_guid, capability_revision)
                    bot_info['action_catalog'] = cached_catalog
                    bot_info['action_history'] = history

                    with db_mgr.get_connection() as conn:
                        with conn.cursor() as cur:
                            plan, thought, speech, api_context = planner.plan_next_actions(
                                bot_info, state, event, memories, db_cursor=cur
                            )

                    # Context overflow / stale-state deferrals are reported instead
                    # of submitting incomplete instructions to the model.
                    if api_context.get("context_overflow") or api_context.get("deferred"):
                        last_budget_diagnostic = str(api_context.get("diagnostic") or "")
                        if api_context.get("deferred"):
                            deferred_live_state = True
                        else:
                            logger.error("Bot %d context budget overflow: %s", bot_guid, last_budget_diagnostic)
                        db_mgr.mark_event_status(event_id, "skipped" if api_context.get("context_overflow") else "pending")
                        continue

                    if not db_mgr.control_is_current(bot_guid, revision):
                        db_mgr.mark_event_status(event_id, 'superseded')
                        continue
                    if event.get('event_type') != 'player_command':
                        goal_update = validate_goal_update(api_context.get('goal_update'), state, bot_info.get('current_goal', ''))
                        if goal_update:
                            db_mgr.update_goal(bot_guid, revision, goal_update)
                            previous_status = str(bot_info.get('goal_status') or 'active')
                            current_goal = str(bot_info.get('current_goal') or '')
                            summary_engine.note_goal(
                                bot_guid, current_goal, goal_update.get('status', 'active'),
                                goal_update.get('progress', ''), revision,
                            )
                            if goal_update.get('status') != previous_status:
                                summary_engine.note_goal_transition(
                                    bot_guid, previous_status, str(goal_update.get('status')),
                                    current_goal, revision,
                                )
                            if goal_update['status'] in ('completed', 'blocked'):
                                # The companion adopts its own next objective from the
                                # long-term goal. update_goal switched autonomy off for the
                                # finished goal, so replace_short_term_goal re-enables it;
                                # the revision guard means an owner command still wins.
                                next_goal = str(api_context.get('next_short_term_goal') or '').strip()
                                if next_goal and bot_info.get('autonomy_enabled'):
                                    if db_mgr.replace_short_term_goal(bot_guid, revision, next_goal):
                                        logger.info("[GOAL] Bot %d adopted next objective: %s", bot_guid, next_goal)
                                        db_mgr.update_bot_thought(
                                            bot_guid, format_thought("New objective: " + next_goal, "goal"))
                                        summary_engine.note_goal(bot_guid, next_goal, "active", "", revision)
                                        memory.set_goals(bot_guid, {
                                            **memory.get_goals(bot_guid),
                                            "current_goal": next_goal,
                                            "goal_status": "active",
                                        })
                                plan = None

                    # Compose full telemetry thought string using the same API context (0 extra tokens)
                    parts = []
                    # A companion-authored line only exists when ambient delegation is off; it is
                    # delivered through mod-llm-chatter's queue so one subsystem still owns the chat.
                    if speech:
                        try:
                            with db_mgr.get_connection() as conn:
                                with conn.cursor() as cur:
                                    if not co_processor.deliver_shared_speech(
                                        cur,
                                        bot_guid,
                                        bot_info.get('bot_name', ''),
                                        speech,
                                        channel=config.get('AzerothFriend.Chat.DefaultChannel', 'party'),
                                    ):
                                        logger.warning(
                                            "[SPEECH] Bot %d wrote a line but delivery to mod-llm-chatter failed", bot_guid
                                        )
                        except Exception as speech_err:
                            logger.debug("Shared speech delivery failed: %s", speech_err)

                    if thought:
                        parts.append(f"Thought: {thought}")
                    if speech:
                        parts.append(f"Speech: {speech}")
                    mindset = api_context.get("mindset")
                    if mindset:
                        parts.append(f"Mindset: {mindset}")
                    model_name = api_context.get("model")
                    if model_name:
                        parts.append(f"Model: {model_name}")
                    tokens = api_context.get("tokens")
                    if tokens and isinstance(tokens, dict) and tokens.get("total"):
                        tot = tokens.get("total", 0)
                        p_tok = tokens.get("prompt", 0)
                        c_tok = tokens.get("completion", 0)
                        r_tok = tokens.get("reasoning", 0)
                        if r_tok:
                            parts.append(f"Tokens: Total={tot} (P={p_tok}, C={c_tok}, R={r_tok})")
                        else:
                            parts.append(f"Tokens: Total={tot} (P={p_tok}, C={c_tok})")
                        activity_level = "HIGH" if tot > 3000 else "NORMAL"
                        parts.append(f"Activity: {activity_level}")

                    if event.get("event_type") in ("dialogue_heard", "player_command"):
                        source_name = event.get("source_name") or "Master"
                        payload_raw = event.get("payload_json") or "{}"
                        try:
                            p_dict = json.loads(payload_raw) if isinstance(payload_raw, str) else payload_raw
                            heard_txt = p_dict.get("text") or p_dict.get("command") or ""
                            heard_chan = p_dict.get("channel", "party")
                            if heard_txt:
                                parts.append(f"Heard: [{heard_chan.upper()}] {source_name}: {heard_txt}")
                        except Exception:
                            pass

                    telemetry_thought = " | ".join(parts) if parts else (thought or "")

                    if api_context.get("context_tokens"):
                        last_context_tokens = int(api_context.get("context_tokens") or 0)

                    if telemetry_thought:
                        db_mgr.update_bot_thought(bot_guid, telemetry_thought)

                    if plan:
                        plan_id = str(uuid.uuid4())[:8]
                        logger.info(
                            "[PLAN] Bot %d (%s) planned %d steps (ID: %s): %s",
                            bot_guid, bot_info.get("bot_name", "Unknown"), len(plan), plan_id,
                            [f"{s.get('action')}({s.get('params', {})})" for s in plan]
                        )
                        db_mgr.insert_action_plan(bot_guid, plan_id, plan, thought=telemetry_thought,
                                                  revision=revision, **provenance)
                        # Keep the compact active-plan view in RAM so the next
                        # planning gate never has to scan action history.
                        memory.set_active_plan(bot_guid, [
                            "%s" % step.get("action") for step in plan
                        ])
                        memory.mark_summary_dirty(bot_guid)
                        db_mgr.mark_event_status(event_id, "completed")
                    else:
                        db_mgr.mark_event_status(event_id, "completed")

                if deferred_live_state:
                    # Wait for the next heartbeat instead of spinning on the queue.
                    time.sleep(1.0)

                # 3. Dirty-only telemetry flush (every 10 seconds when changed).
                if now - last_telemetry_sync >= 10.0:
                    last_telemetry_sync = now
                    stats = tracker.get_stats()
                    signature = (stats["co_processed_events"], stats["sensory_reused_events"],
                                 stats["total_tokens_saved"])
                    if signature != last_telemetry_signature:
                        last_telemetry_signature = signature
                        db_mgr.record_telemetry(
                            stats["co_processed_events"],
                            stats["sensory_reused_events"],
                            stats["total_tokens_saved"],
                        )

                # 4. Dirty summaries every 60 seconds (and at goal transitions).
                if now - last_summary_flush >= summary_engine.interval_seconds:
                    last_summary_flush = now
                    flushed = summary_engine.maybe_flush(now=now)
                    if flushed:
                        logger.info("Flushed %d structured summary record(s)", flushed)

                # 5. Bounded diagnostic retention, after summary checkpointing.
                if now - last_retention_prune >= 600.0:
                    last_retention_prune = now
                    try:
                        removed = db_mgr.prune_diagnostic_records()
                        if removed:
                            logger.info("Pruned %d terminal diagnostic record(s) older than 7 days", removed)
                    except Exception as prune_err:
                        logger.debug("Diagnostic retention pass failed: %s", prune_err)

                # 6. Publish cache/transport/context diagnostics to the server so
                # the addon can render RAM freshness without any client-side maths.
                if now - last_diag_send >= 5.0:
                    last_diag_send = now
                    cache_stats = memory.cache_stats()
                    live_state_transport.send_diagnostics({
                        "context_tokens": last_context_tokens,
                        "cache_bytes": int(cache_stats.get("total_bytes", 0)),
                        "cache_bots": int(cache_stats.get("bots", 0)),
                        "summary_ts": int(last_summary_flush),
                        "diagnostic": last_budget_diagnostic[:180],
                    })

            except mysql.connector.Error as err:
                logger.error("MySQL Bridge error: %s", err)
                time.sleep(3.0)
            except Exception as e:
                logger.error("Unexpected error in Bridge event loop: %s", e, exc_info=True)
                time.sleep(2.0)

    except KeyboardInterrupt:
        # Orderly shutdown: flush dirty summaries and the chatter checkpoint, then
        # close the live-state transport so no half-written frame is left behind.
        try:
            summary_engine.flush_all(reason="shutdown")
        except Exception as flush_err:
            logger.debug("Shutdown summary flush failed: %s", flush_err)
        try:
            chatter_consumer.save_checkpoint()
        except Exception as checkpoint_err:
            logger.debug("Shutdown checkpoint save failed: %s", checkpoint_err)
        live_state_transport.stop()
        logger.info("AzerothFriend Python Bridge shutting down cleanly.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
