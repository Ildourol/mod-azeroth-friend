"""RAM-first live state transport and cache for AzerothFriend.

The C++ module runs a loopback TCP listener on a dedicated background worker.
This module connects to it, authenticates with the shared configuration secret,
and keeps a bounded, thread-safe cache of the latest value snapshots the world
thread published.

Design rules (see docs/plans/2026-09-20-ram-first-context-and-sql-efficiency-redesign.md):
  * Versioned, length-prefixed JSON frames; frames are capped at 256 KiB.
  * Every frame carries session id, bot guid, sequence, observation timestamp,
    control revision, type and payload.
  * Old sessions, stale revisions, malformed and oversized frames are rejected.
  * Sequence gaps mark the bot stale and request a complete refresh instead of
    guessing with partial state.
  * After ``HEARTBEAT_TIMEOUT_SECONDS`` without a valid heartbeat the state is
    no longer fresh, which suspends new LLM planning.
  * SQL checkpoints are recovery/debug data and never qualify as live state.
"""

import json
import logging
import socket
import struct
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Tuple

logger = logging.getLogger("azeroth_friend.livestate")

PROTOCOL_VERSION = 1
MAX_FRAME_BYTES = 256 * 1024
LENGTH_PREFIX = struct.Struct("!I")
HEARTBEAT_TIMEOUT_SECONDS = 5.0
RECONNECT_DELAY_SECONDS = 2.0
AUTH_TIMEOUT_SECONDS = 5.0

# Sections the C++ publisher can send. ``full`` is the initial complete state.
STATE_SECTIONS = ("vitals", "surroundings", "owner", "control", "capabilities", "outcomes")
VALID_SECTIONS = frozenset(STATE_SECTIONS)

FRAME_STATE = "state"
FRAME_HEARTBEAT = "heartbeat"
FRAME_HELLO = "hello"
FRAME_AUTH = "auth"
FRAME_REFRESH = "refresh"
FRAME_ERROR = "error"
FRAME_DIAG = "diag"

# Frame outcome codes returned by LiveStateCache.apply_frame.
ACCEPTED = "accepted"
REJECTED_MALFORMED = "malformed"
REJECTED_VERSION = "unsupported_version"
REJECTED_SESSION = "stale_session"
REJECTED_REVISION = "stale_revision"
REJECTED_DUPLICATE = "duplicate_sequence"
GAP_DETECTED = "sequence_gap"

# Per-bot cache ceiling from the plan (2 MiB). Optional sections are dropped
# before mandatory control state when the ceiling is hit.
MAX_BOT_CACHE_BYTES = 2 * 1024 * 1024


def encode_frame(payload: Dict[str, Any]) -> bytes:
    """Encode one protocol frame as a length-prefixed UTF-8 JSON document."""
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    if len(body) > MAX_FRAME_BYTES:
        raise ValueError("frame exceeds %d bytes" % MAX_FRAME_BYTES)
    return LENGTH_PREFIX.pack(len(body)) + body


def decode_frames(buffer: bytes) -> Tuple[List[Dict[str, Any]], bytes, bool]:
    """Decode as many complete frames as ``buffer`` holds.

    Returns ``(frames, remaining, protocol_error)``. A malformed length prefix or
    oversized frame sets ``protocol_error`` so the caller can drop the connection.
    Malformed JSON bodies are reported as errors too: a broken stream must never
    be interpreted as an empty state.
    """
    frames: List[Dict[str, Any]] = []
    offset = 0
    total = len(buffer)
    while total - offset >= LENGTH_PREFIX.size:
        (length,) = LENGTH_PREFIX.unpack_from(buffer, offset)
        if length == 0 or length > MAX_FRAME_BYTES:
            return frames, buffer[offset:], True
        if total - offset - LENGTH_PREFIX.size < length:
            break
        start = offset + LENGTH_PREFIX.size
        raw = buffer[start:start + length]
        offset = start + length
        try:
            frame = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return frames, buffer[offset:], True
        if not isinstance(frame, dict):
            return frames, buffer[offset:], True
        frames.append(frame)
    return frames, buffer[offset:], False


def validate_frame(frame: Any) -> Optional[str]:
    """Return a rejection code when ``frame`` is not a valid protocol frame."""
    if not isinstance(frame, dict):
        return REJECTED_MALFORMED
    if frame.get("v") != PROTOCOL_VERSION:
        return REJECTED_VERSION
    message_type = frame.get("type")
    if message_type not in (FRAME_STATE, FRAME_HEARTBEAT, FRAME_HELLO):
        return REJECTED_MALFORMED
    session = frame.get("session")
    if not isinstance(session, str) or not session:
        return REJECTED_MALFORMED
    if not isinstance(frame.get("seq"), int) or int(frame["seq"]) <= 0:
        return REJECTED_MALFORMED
    if message_type == FRAME_STATE:
        section = frame.get("section")
        if section not in VALID_SECTIONS:
            return REJECTED_MALFORMED
        if not isinstance(frame.get("bot"), int) or int(frame["bot"]) <= 0:
            return REJECTED_MALFORMED
        if not isinstance(frame.get("payload"), dict):
            return REJECTED_MALFORMED
    return None


@dataclass
class BotLiveState:
    """Bounded value snapshot for a single companion."""

    bot_guid: int
    session: str = ""
    sequence: int = 0
    revision: int = -1
    observation_ts: float = 0.0
    received_at: float = 0.0
    heartbeat_at: float = 0.0
    complete: bool = False
    sections: Dict[str, Dict[str, Any]] = field(default_factory=dict)
    refresh_requested: bool = False
    gap_count: int = 0

    def apply_section(self, section: str, payload: Dict[str, Any]) -> None:
        self.sections[section] = payload

    def estimated_bytes(self) -> int:
        total = 0
        for payload in self.sections.values():
            try:
                total += len(json.dumps(payload, separators=(",", ":")))
            except (TypeError, ValueError):
                total += 256
        return total

    def merged(self) -> Dict[str, Any]:
        """Flatten the sections into the state shape the bridge already uses."""
        merged: Dict[str, Any] = {
            "live_state": True,
            "live_state_ts": self.observation_ts,
            "live_state_received_at": self.received_at,
            "live_state_sequence": self.sequence,
            "control_revision": self.revision,
            "live_complete": self.complete,
        }
        vitals = self.sections.get("vitals") or {}
        merged.update(vitals)

        owner = self.sections.get("owner") or {}
        if owner:
            merged["owner"] = owner
        control = self.sections.get("control") or {}
        if control:
            merged["control"] = control
            merged.setdefault("control_revision", control.get("control_revision", self.revision))
            if "current_goal" in control:
                merged["current_goal"] = control.get("current_goal")
            if "long_term_goal" in control:
                merged["long_term_goal"] = control.get("long_term_goal")
            if "goal_status" in control:
                merged["goal_status"] = control.get("goal_status")
            if "autonomy_enabled" in control:
                merged["autonomy_enabled"] = control.get("autonomy_enabled")
            if "bridge_enabled" in control:
                merged["bridge_enabled"] = control.get("bridge_enabled")
        capabilities = self.sections.get("capabilities") or {}
        if capabilities:
            merged["capabilities"] = capabilities
            if "self_context" in capabilities:
                merged["self_context"] = capabilities.get("self_context")
        outcomes = self.sections.get("outcomes") or {}
        if outcomes:
            merged["outcomes"] = outcomes
        surroundings = self.sections.get("surroundings") or {}
        if surroundings:
            merged["environment"] = surroundings
            try:
                merged["environment_json"] = json.dumps(surroundings, separators=(",", ":"))
            except (TypeError, ValueError):
                merged["environment_json"] = "{}"
        return merged


class LiveStateCache:
    """Thread-safe RAM cache of published live state."""

    def __init__(self, max_bot_bytes: int = MAX_BOT_CACHE_BYTES) -> None:
        self._lock = threading.RLock()
        self._bots: Dict[int, BotLiveState] = {}
        self._session: str = ""
        self._server_session: str = ""
        self._last_frame_at: float = 0.0
        self._max_bot_bytes = max(64 * 1024, int(max_bot_bytes))

    # ---------------------------------------------------------------- session
    def note_hello(self, session: str, server_session: str = "") -> None:
        """Record the server session id handed out during the handshake."""
        with self._lock:
            self._session = str(session or "")
            self._server_session = str(server_session or self._session)
            self._last_frame_at = time.monotonic()

    @property
    def session(self) -> str:
        with self._lock:
            return self._session

    # ------------------------------------------------------------------ frame
    def apply_frame(self, frame: Any, now: Optional[float] = None) -> str:
        """Validate and merge one frame. Returns an outcome code."""
        timestamp = time.monotonic() if now is None else now
        rejection = validate_frame(frame)
        if rejection:
            logger.debug("Rejected live-state frame: %s", rejection)
            return rejection

        session = str(frame.get("session"))
        message_type = str(frame.get("type"))
        sequence = int(frame.get("seq"))

        with self._lock:
            if message_type == FRAME_HELLO:
                self.note_hello(session, str(frame.get("server_session") or session))
                return ACCEPTED

            if self._session and session != self._session:
                logger.info("Rejecting live-state frame from stale session %s (active %s)", session, self._session)
                return REJECTED_SESSION

            self._last_frame_at = timestamp

            if message_type == FRAME_HEARTBEAT:
                # A heartbeat is global unless it names a bot.
                bot_guid = int(frame.get("bot") or 0)
                if bot_guid:
                    bot = self._bots.get(bot_guid)
                    if bot is not None:
                        bot.heartbeat_at = timestamp
                else:
                    for bot in self._bots.values():
                        bot.heartbeat_at = timestamp
                return ACCEPTED

            bot_guid = int(frame.get("bot"))
            revision = int(frame.get("revision") or 0)
            section = str(frame.get("section"))
            payload = frame.get("payload") or {}
            observation_ts = float(frame.get("ts") or 0.0)

            bot = self._bots.get(bot_guid)
            if bot is None:
                bot = BotLiveState(bot_guid=bot_guid, session=session)
                self._bots[bot_guid] = bot

            if bot.session and bot.session != session:
                # New server session: accept and start over at the new baseline.
                bot.sections.clear()
                bot.complete = False
                bot.session = session
                bot.sequence = 0
                bot.revision = -1

            if sequence <= bot.sequence:
                return REJECTED_DUPLICATE

            if sequence != bot.sequence + 1:
                bot.gap_count += 1
                bot.refresh_requested = True
                logger.warning(
                    "Live-state sequence gap for bot %d (expected %d, got %d); requesting full refresh",
                    bot_guid, bot.sequence + 1, sequence,
                )
                bot.sequence = sequence
                return GAP_DETECTED

            if bot.revision >= 0 and revision < bot.revision:
                logger.info(
                    "Rejecting stale live-state revision for bot %d (have %d, frame %d)",
                    bot_guid, bot.revision, revision,
                )
                return REJECTED_REVISION

            bot.sequence = sequence
            bot.revision = revision
            bot.observation_ts = observation_ts
            bot.received_at = timestamp
            bot.heartbeat_at = timestamp
            bot.apply_section(section, payload)
            if section == "vitals":
                # The full publish always opens with vitals after a refresh.
                if not bot.complete and all(name in bot.sections for name in ("vitals", "surroundings", "control")):
                    bot.complete = True
            self._enforce_budget(bot)
            return ACCEPTED

    def _enforce_budget(self, bot: BotLiveState) -> None:
        """Drop optional sections before mandatory control state when over budget."""
        if bot.estimated_bytes() <= self._max_bot_bytes:
            return
        for optional in ("outcomes", "surroundings", "owner", "capabilities"):
            if bot.estimated_bytes() <= self._max_bot_bytes:
                break
            if optional in bot.sections:
                logger.warning(
                    "Live-state cache for bot %d exceeded %d bytes; dropping optional '%s' section",
                    bot.bot_guid, self._max_bot_bytes, optional,
                )
                bot.sections.pop(optional, None)
                bot.refresh_requested = True

    # -------------------------------------------------------------- read side
    def snapshot(self, bot_guid: int, now: Optional[float] = None) -> Optional[Dict[str, Any]]:
        """Return merged live state, or None when the cache has nothing usable."""
        with self._lock:
            bot = self._bots.get(int(bot_guid))
            if bot is None or not bot.sections:
                return None
            merged = bot.merged()
            merged["live_state_age"] = max(0.0, (time.monotonic() if now is None else now) - bot.heartbeat_at)
            return merged

    def is_fresh(self, bot_guid: int, now: Optional[float] = None) -> bool:
        """True when a valid heartbeat arrived within the freshness window."""
        with self._lock:
            bot = self._bots.get(int(bot_guid))
            if bot is None or not bot.sections or bot.refresh_requested:
                return False
            current = time.monotonic() if now is None else now
            return (current - bot.heartbeat_at) <= HEARTBEAT_TIMEOUT_SECONDS

    def is_transport_fresh(self, now: Optional[float] = None) -> bool:
        """True when any valid frame arrived within the freshness window."""
        with self._lock:
            if not self._last_frame_at:
                return False
            current = time.monotonic() if now is None else now
            return (current - self._last_frame_at) <= HEARTBEAT_TIMEOUT_SECONDS

    def diagnostics(self, bot_guid: int, now: Optional[float] = None) -> Dict[str, Any]:
        """Small, addon-safe snapshot of transport/cache health (no secrets)."""
        with self._lock:
            bot = self._bots.get(int(bot_guid))
            current = time.monotonic() if now is None else now
            transport_age = (current - self._last_frame_at) if self._last_frame_at else None
            if bot is None:
                return {
                    "transport": "connected" if self.is_transport_fresh(current) else "waiting",
                    "session": self._session,
                    "cache_fresh": False,
                    "cache_age": None,
                    "sequence": 0,
                    "revision": -1,
                    "gaps": 0,
                    "sections": [],
                }
            return {
                "transport": "connected" if (transport_age is not None and transport_age <= HEARTBEAT_TIMEOUT_SECONDS) else "stale",
                "session": self._session,
                "cache_fresh": self.is_fresh(bot_guid, current),
                "cache_age": round(max(0.0, current - bot.heartbeat_at), 1) if bot.heartbeat_at else None,
                "sequence": bot.sequence,
                "revision": bot.revision,
                "gaps": bot.gap_count,
                "sections": sorted(bot.sections.keys()),
            }

    def take_refresh_requests(self) -> List[int]:
        """Return and clear the bots that need a complete refresh."""
        with self._lock:
            pending = [guid for guid, bot in self._bots.items() if bot.refresh_requested]
            for guid in pending:
                self._bots[guid].refresh_requested = False
            return pending

    def request_refresh(self, bot_guid: int) -> None:
        with self._lock:
            bot = self._bots.get(int(bot_guid))
            if bot is not None:
                bot.refresh_requested = True

    def remove(self, bot_guid: int) -> None:
        """Forget all live observations for one bridge-disconnected bot."""
        with self._lock:
            self._bots.pop(int(bot_guid), None)

    def mark_stale(self) -> None:
        """Mark every bot for a fresh baseline (transport loss, session change)."""
        with self._lock:
            for bot in self._bots.values():
                bot.refresh_requested = True

    def clear(self) -> None:
        with self._lock:
            self._bots.clear()
            self._session = ""


class LiveStateTransport:
    """Reconnecting loopback client for the C++ live-state worker.

    All socket I/O happens on the transport thread; ``cache.apply_frame`` is the
    only entry point into shared state and it is lock protected.
    """

    def __init__(
        self,
        host: str,
        port: int,
        secret: str,
        cache: LiveStateCache,
        enabled: bool = True,
        on_status: Optional[Callable[[str], None]] = None,
    ) -> None:
        self.host = host
        self.port = int(port)
        self.secret = secret or ""
        self.cache = cache
        self.enabled = bool(enabled and self.secret)
        self._on_status = on_status
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._sock: Optional[socket.socket] = None
        self._connected = False
        self._status = "disabled" if not self.enabled else "stopped"
        self._outbox: "deque[Dict[str, Any]]" = deque(maxlen=32)
        self._outbox_lock = threading.Lock()

    def send_diagnostics(self, payload: Dict[str, Any]) -> None:
        """Queue a bounded diagnostics frame for the C++ side (addon HUD)."""
        if not self.enabled:
            return
        frame = {"v": PROTOCOL_VERSION, "type": FRAME_DIAG, "ts": int(time.time())}
        frame.update(payload or {})
        with self._outbox_lock:
            self._outbox.append(frame)

    def _drain_outbox(self) -> List[Dict[str, Any]]:
        with self._outbox_lock:
            frames = list(self._outbox)
            self._outbox.clear()
        return frames

    # --------------------------------------------------------------- lifecycle
    def start(self) -> None:
        if not self.enabled or self._thread is not None:
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="af-livestate", daemon=True)
        self._thread.start()

    def stop(self, timeout: float = 3.0) -> None:
        self._stop.set()
        sock = self._sock
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass
        if self._thread is not None:
            self._thread.join(timeout=timeout)
            self._thread = None
        self._set_status("stopped")

    @property
    def connected(self) -> bool:
        return self._connected

    @property
    def status(self) -> str:
        return self._status

    def _set_status(self, status: str) -> None:
        if status == self._status:
            return
        self._status = status
        if self._on_status is not None:
            try:
                self._on_status(status)
            except Exception:  # pragma: no cover - status hook must never kill the thread
                logger.debug("Status hook failed", exc_info=True)

    # ------------------------------------------------------------------ thread
    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self._connect_and_pump()
            except Exception as err:  # pragma: no cover - defensive
                logger.debug("Live-state transport error: %s", err)
            finally:
                self._connected = False
                self.cache.mark_stale()
                self._set_status("reconnecting")
            if self._stop.wait(RECONNECT_DELAY_SECONDS):
                break

    def _connect_and_pump(self) -> None:
        sock = socket.create_connection((self.host, self.port), timeout=AUTH_TIMEOUT_SECONDS)
        sock.settimeout(1.0)
        self._sock = sock
        self._set_status("connecting")
        try:
            self._send(sock, {
                "v": PROTOCOL_VERSION,
                "type": FRAME_AUTH,
                "secret": self.secret,
                "role": "bridge",
            })
            self._handshake(sock)
            self._connected = True
            self._set_status("connected")
            self._pump(sock)
        finally:
            try:
                sock.close()
            except OSError:
                pass
            self._sock = None

    def _send(self, sock: socket.socket, payload: Dict[str, Any]) -> None:
        sock.sendall(encode_frame(payload))

    def _handshake(self, sock: socket.socket) -> None:
        deadline = time.monotonic() + AUTH_TIMEOUT_SECONDS
        buffer = b""
        while time.monotonic() < deadline:
            buffer = self._read_some(sock, buffer)
            frames, buffer, protocol_error = decode_frames(buffer)
            if protocol_error:
                raise ConnectionError("malformed handshake frame")
            for frame in frames:
                if frame.get("type") == FRAME_HELLO:
                    session = str(frame.get("session") or "")
                    if not session:
                        raise ConnectionError("handshake without session id")
                    self.cache.note_hello(session, str(frame.get("server_session") or session))
                    logger.info("Live-state transport authenticated (session %s)", session)
                    return
                if frame.get("type") == FRAME_ERROR:
                    raise ConnectionError("rejected by live-state listener: %s" % frame.get("reason"))

    def _pump(self, sock: socket.socket) -> None:
        buffer = b""
        last_refresh_poll = 0.0
        while not self._stop.is_set():
            buffer = self._read_some(sock, buffer)
            frames, buffer, protocol_error = decode_frames(buffer)
            if protocol_error:
                logger.warning("Malformed live-state frame; dropping connection for a clean reconnect")
                return
            for frame in frames:
                outcome = self.cache.apply_frame(frame)
                if outcome in (REJECTED_VERSION, REJECTED_MALFORMED):
                    logger.warning("Discarding incompatible live-state frame (%s)", outcome)

            for frame in self._drain_outbox():
                try:
                    self._send(sock, frame)
                except OSError:
                    return

            now = time.monotonic()
            if now - last_refresh_poll >= 1.0:
                last_refresh_poll = now
                # A gap in the sequence needs a complete refresh rather than a guess.
                pending = self.cache.take_refresh_requests()
                # Nothing received for a full freshness window means the stream is
                # suspect even though the socket still looks open: ask for a baseline.
                if not pending and not self.cache.is_transport_fresh(now):
                    pending = ["stale"]
                if not pending:
                    continue
                try:
                    self._send(sock, {
                        "v": PROTOCOL_VERSION,
                        "type": FRAME_REFRESH,
                        "bots": [bot for bot in pending if isinstance(bot, int)],
                        "reason": "sequence_gap" if any(isinstance(bot, int) for bot in pending) else "stale",
                        "client_ts": int(time.time()),
                    })
                except OSError:
                    return

    def _read_some(self, sock: socket.socket, buffer: bytes) -> bytes:
        try:
            chunk = sock.recv(64 * 1024)
        except socket.timeout:
            return buffer
        if not chunk:
            raise ConnectionError("live-state connection closed")
        return buffer + chunk
