"""Zero-token structured summaries for AzerothFriend.

Summaries are derived from *verified* outcomes (completed/failed action rows and
deterministic goal transitions) - never from free-form model prose and never from
legacy memory rows, which stay as diagnostic history only.

Each summary carries the goal identity, revision, time range and the source
outcome ids so updates are idempotent and auditable. At most two summaries
relevant to the active goal are offered to the context builder, inside a
250-token allowance, and they can never supply live targets or obsolete modes.
"""

import hashlib
import json
import logging
import time
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Sequence

from friend_context import estimate_tokens

logger = logging.getLogger("azeroth_friend.summaries")

SUMMARY_INTERVAL_SECONDS = 60.0
MAX_SUMMARY_TOKENS = 250
MAX_SUMMARY_CHARS = MAX_SUMMARY_TOKENS * 4
MAX_GOAL_SUMMARIES = 2

# Outcome kinds that count as resolved/verified progress.
VERIFIED_KINDS = frozenset({"completed", "failed", "goal_transition"})


@dataclass
class VerifiedOutcome:
    """One durable, verified outcome that may feed a summary."""

    outcome_id: int
    kind: str
    text: str
    goal_identity: str = ""
    revision: int = 0
    at: float = field(default_factory=time.time)


@dataclass
class SummaryRecord:
    """A durable summary row."""

    bot_guid: int
    goal_identity: str
    revision: int
    window_start: float
    window_end: float
    content: str
    source_outcome_ids: List[int]
    summary_key: str
    milestones: List[str] = field(default_factory=list)
    obstacles: List[str] = field(default_factory=list)
    progress: str = ""
    latest_transition: str = ""

    def payload(self) -> Dict[str, Any]:
        return {
            "milestones": self.milestones,
            "progress": self.progress,
            "obstacles": self.obstacles,
            "latest_transition": self.latest_transition,
        }


def goal_identity(goal: str, status: str = "") -> str:
    """Stable identity for a goal so summaries bind to the right objective."""
    base = (str(goal or "").strip() + "|" + str(status or "").strip()).lower()
    return hashlib.sha1(base.encode("utf-8")).hexdigest()[:16]


class SummaryEngine:
    """Collects verified outcomes and flushes dirty summaries on a cadence."""

    def __init__(self, db: Any, memory: Any, interval_seconds: float = SUMMARY_INTERVAL_SECONDS) -> None:
        self.db = db
        self.memory = memory
        self.interval_seconds = max(5.0, float(interval_seconds))
        self._buffers: Dict[int, List[VerifiedOutcome]] = {}
        self._last_flush: Dict[int, float] = {}
        self._goal_cache: Dict[int, Dict[str, Any]] = {}

    # ------------------------------------------------------------ collection
    def note_outcome(self, bot_guid: int, outcome: VerifiedOutcome) -> None:
        """Record a verified outcome and mark the summary dirty."""
        if outcome.kind not in VERIFIED_KINDS:
            return
        if not str(outcome.text or "").strip():
            return
        self._buffers.setdefault(int(bot_guid), []).append(outcome)
        self.memory.note_outcome(bot_guid, outcome.text)
        self.memory.mark_summary_dirty(bot_guid)
        if outcome.kind == "failed" or "blocked" in str(outcome.text).lower():
            self.memory.set_blocker(bot_guid, outcome.text)
        elif outcome.kind == "completed":
            self.memory.clear_blocker(bot_guid)

    def note_outcomes(self, bot_guid: int, outcomes: Iterable[VerifiedOutcome]) -> None:
        for outcome in outcomes:
            self.note_outcome(bot_guid, outcome)

    def note_goal(self, bot_guid: int, goal: str, status: str = "", progress: str = "",
                  revision: int = 0) -> None:
        self._goal_cache[int(bot_guid)] = {
            "goal": str(goal or ""),
            "status": str(status or ""),
            "progress": str(progress or ""),
            "revision": int(revision or 0),
            "identity": goal_identity(goal, status),
        }

    def note_goal_transition(self, bot_guid: int, previous_status: str, new_status: str,
                             goal: str = "", revision: int = 0) -> None:
        """A goal transition always flushes: it is a durable lifecycle boundary."""
        self.note_goal(bot_guid, goal, new_status, revision=revision)
        self.note_outcome(bot_guid, VerifiedOutcome(
            outcome_id=0,
            kind="goal_transition",
            text="Goal '%s' moved %s -> %s" % (goal or "current objective", previous_status or "unknown", new_status),
            goal_identity=goal_identity(goal, new_status),
            revision=int(revision or 0),
        ))
        self.flush_bot(bot_guid, reason="goal_transition", force=True)

    def note_goal_progress(self, bot_guid: int, progress: str, goal: str = "", revision: int = 0) -> None:
        self.note_outcome(bot_guid, VerifiedOutcome(
            outcome_id=0,
            kind="completed",
            text="Progress on '%s': %s" % (goal or "current objective", progress),
            goal_identity=goal_identity(goal),
            revision=int(revision or 0),
        ))

    def forget_bot(self, bot_guid: int) -> None:
        """Discard transient summary work when Claim disconnects a companion."""
        guid = int(bot_guid)
        self._buffers.pop(guid, None)
        self._last_flush.pop(guid, None)
        self._goal_cache.pop(guid, None)

    # --------------------------------------------------------------- flushing
    def maybe_flush(self, now: Optional[float] = None, reason: str = "interval") -> int:
        """Flush dirty summaries whose cadence elapsed (default: 60 seconds)."""
        current = time.time() if now is None else now
        flushed = 0
        for bot_guid in list(self.memory.dirty_bots()):
            last = self._last_flush.get(bot_guid, 0.0)
            if reason == "interval" and (current - last) < self.interval_seconds:
                continue
            if self.flush_bot(bot_guid, reason=reason, now=current):
                flushed += 1
        return flushed

    def flush_bot(self, bot_guid: int, reason: str = "manual", force: bool = False,
                  now: Optional[float] = None) -> bool:
        """Build and persist one structured summary. Idempotent per outcome set."""
        current = time.time() if now is None else now
        bot_guid = int(bot_guid)
        buffer = self._buffers.get(bot_guid, [])
        goal = self._goal_cache.get(bot_guid, {})
        if not buffer and not self.memory.is_summary_dirty(bot_guid) and not force:
            return False
        if not buffer and not goal:
            try:
                self.memory.set_summaries(
                    bot_guid, self.db.fetch_relevant_summaries(bot_guid, "", MAX_GOAL_SUMMARIES))
            except Exception:
                pass
            return False

        milestones = [o.text for o in buffer if o.kind == "completed"][:4]
        obstacles = [o.text for o in buffer if o.kind == "failed" or "blocked" in o.text.lower()][:2]
        transitions = [o.text for o in buffer if o.kind == "goal_transition"]
        latest_transition = transitions[-1] if transitions else (buffer[-1].text if buffer else "")
        progress = goal.get("progress") or (
            "" if not goal.get("goal") else "goal '%s' is %s" % (goal.get("goal"), goal.get("status") or "active"))

        content = self._render(milestones, progress, obstacles, latest_transition)
        window_start = min((o.at for o in buffer), default=current)
        window_end = max((o.at for o in buffer), default=current)
        outcome_ids = [int(o.outcome_id) for o in buffer if int(o.outcome_id or 0) > 0]
        identity = goal.get("identity") or (buffer[-1].goal_identity if buffer else "")
        key = self._summary_key(bot_guid, identity, outcome_ids, window_start, content)

        record = SummaryRecord(
            bot_guid=bot_guid,
            goal_identity=identity,
            revision=int(goal.get("revision") or 0),
            window_start=window_start,
            window_end=window_end,
            content=content,
            source_outcome_ids=outcome_ids,
            summary_key=key,
            milestones=milestones,
            obstacles=obstacles,
            progress=progress,
            latest_transition=latest_transition,
        )
        stored = False
        try:
            stored = bool(self.db.upsert_summary(record))
        except Exception as err:
            logger.warning("Summary write failed for bot %d: %s", bot_guid, err)
            return False

        self._buffers[bot_guid] = []
        self._last_flush[bot_guid] = current
        try:
            relevant = self.db.fetch_relevant_summaries(bot_guid, identity, MAX_GOAL_SUMMARIES)
        except Exception:
            relevant = []
        if not relevant and stored:
            relevant = [{"content": content, "goal_identity": identity}]
        self.memory.set_summaries(bot_guid, relevant)
        logger.info("Flushed %s summary for bot %d (%d source outcomes)", reason, bot_guid, len(outcome_ids))
        return stored

    def flush_all(self, reason: str = "shutdown") -> int:
        """Flush every dirty summary (logout, orderly shutdown)."""
        return self.maybe_flush(reason=reason)

    # ---------------------------------------------------------------- helpers
    @staticmethod
    def _render(milestones: Sequence[str], progress: str, obstacles: Sequence[str],
                latest_transition: str) -> str:
        parts = []
        if milestones:
            parts.append("Milestones: " + "; ".join(milestones))
        if progress:
            parts.append("Progress: " + progress)
        if obstacles:
            parts.append("Obstacles: " + "; ".join(obstacles))
        if latest_transition:
            parts.append("Latest: " + latest_transition)
        text = " | ".join(parts) if parts else "No verified progress recorded yet."
        if len(text) > MAX_SUMMARY_CHARS:
            text = text[:MAX_SUMMARY_CHARS - 1].rstrip() + "..."
        return text

    @staticmethod
    def _summary_key(bot_guid: int, identity: str, outcome_ids: Sequence[int],
                     window_start: float, content: str) -> str:
        digest = hashlib.sha1()
        digest.update(str(bot_guid).encode("utf-8"))
        digest.update(identity.encode("utf-8"))
        digest.update(",".join(str(i) for i in sorted(outcome_ids)).encode("utf-8"))
        digest.update(("%.0f" % window_start).encode("utf-8"))
        digest.update(content.encode("utf-8"))
        return digest.hexdigest()[:40]

    @staticmethod
    def render_for_prompt(summaries: Sequence[Dict[str, Any]],
                          allowance_tokens: int = MAX_SUMMARY_TOKENS) -> List[str]:
        """Trim summary text to the configured allowance."""
        rendered: List[str] = []
        used = 0
        for summary in summaries:
            content = summary.get("content") if isinstance(summary, dict) else str(summary)
            if not content:
                continue
            cost = estimate_tokens(content)
            if used + cost > allowance_tokens:
                break
            used += cost
            rendered.append(content)
        return rendered


def outcomes_from_action_rows(rows: Sequence[Dict[str, Any]], goal: str = "") -> List[VerifiedOutcome]:
    """Convert verified action rows into outcomes for the summary engine."""
    outcomes: List[VerifiedOutcome] = []
    for row in rows or []:
        status = str(row.get("status") or "")
        if status not in ("completed", "failed"):
            continue
        result_payload: Dict[str, Any] = {}
        raw_result = row.get("result_json")
        if isinstance(raw_result, dict):
            result_payload = raw_result
        elif raw_result:
            try:
                result_payload = json.loads(str(raw_result))
            except (TypeError, ValueError):
                result_payload = {}
        # Successful dispatch acknowledgements are useful diagnostics but are
        # not verified world outcomes and must never enter zero-token memory.
        if status == "completed" and result_payload.get("verified") is not True:
            continue
        action = str(row.get("action_type") or "action")
        failure = str(row.get("failure_reason") or "").strip()
        text = "%s %s" % (action, status)
        if failure:
            text += " (%s)" % failure
        outcomes.append(VerifiedOutcome(
            outcome_id=int(row.get("id") or 0),
            kind="failed" if status == "failed" else "completed",
            text=text,
            goal_identity=goal_identity(goal),
            revision=int(row.get("control_revision") or 0),
            at=float(row.get("completed_ts") or time.time()),
        ))
    return outcomes
