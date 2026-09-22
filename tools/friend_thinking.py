"""Thinking and reasoning model policy for AzerothFriend.

Supports OpenAI reasoning models:
- Legacy reasoning models (e.g. o1, o1-mini, o3-mini, o4): always-on reasoning,
  no custom temperature parameter, requires max_completion_tokens, minimal collapses to low.
- Newer reasoning models (e.g. gpt-5, gpt-5-nano, gpt-5-mini): optional reasoning,
  supports minimal effort (fast tactical responses), customizable temperature.
- Standard chat models (e.g. gpt-4o, gpt-4o-mini): no reasoning_effort parameter.
"""

from dataclasses import dataclass
from enum import Enum
import logging
from typing import Any, Dict, Optional, Set

from friend_constants import (
    DEFAULT_THINKING_AUTODETECT,
    DEFAULT_THINKING_AUTOKINDS,
    DEFAULT_THINKING_EFFORT,
    DEFAULT_THINKING_MODE,
    DEFAULT_THINKING_MODEL_TYPE,
    DEFAULT_THINKING_TOKEN_RESERVE,
)

logger = logging.getLogger("azeroth_friend.thinking")


class ModelType(str, Enum):
    STANDARD_CHAT = "StandardChat"
    LEGACY_THINKING = "LegacyThinking"
    NEWER_THINKING = "NewerThinking"


class ThinkingMode(str, Enum):
    AUTO = "Auto"
    ON = "On"
    OFF = "Off"


@dataclass
class ThinkingDecision:
    apply: bool
    effort: str
    model_type: ModelType
    token_reserve: int
    reason: str


def extract_model_leaf(model: str) -> str:
    """Extract leaf model name without provider/repo prefix and normalize."""
    m = model.strip().lower()
    if "/" in m:
        m = m.split("/")[-1]
    return m


def is_reasoning_parameter_rejection(error_str: str) -> bool:
    """Check if an API error indicates rejection of reasoning parameters."""
    err = error_str.lower()
    names_control = (
        "reasoning_effort" in err
        or '"reasoning"' in err
        or "reasoning effort" in err
        or "thinking_budget" in err
        or "thinking_level" in err
    )
    says_rejected = (
        "unsupported" in err
        or "unknown" in err
        or "invalid" in err
        or "not supported" in err
        or "unrecognized" in err
        or "extra inputs are not permitted" in err
    )
    return names_control and says_rejected


def is_temperature_rejection(error_str: str) -> bool:
    """Check if an API error indicates custom temperature is rejected (common on reasoning models)."""
    err = error_str.lower()
    return "temperature" in err and (
        "does not support" in err or "unsupported value" in err or "only the default" in err
    )


class ThinkingPolicy:
    def __init__(self, config: Dict[str, str]):
        raw_mode = config.get("AzerothFriend.LLM.Thinking.Mode", DEFAULT_THINKING_MODE).strip().lower()
        if raw_mode == "on":
            self.mode = ThinkingMode.ON
        elif raw_mode == "off":
            self.mode = ThinkingMode.OFF
        else:
            self.mode = ThinkingMode.AUTO

        raw_autodetect = config.get("AzerothFriend.LLM.Thinking.AutoDetect", "1").strip().lower()
        self.autodetect = raw_autodetect not in ("0", "false", "no", "off")

        raw_type = config.get("AzerothFriend.LLM.Thinking.ModelType", DEFAULT_THINKING_MODEL_TYPE).strip().lower()
        if raw_type in ("legacythinking", "legacy_thinking", "legacy"):
            self.model_type_override = ModelType.LEGACY_THINKING
        elif raw_type in ("newerthinking", "newer_thinking", "newer", "modern"):
            self.model_type_override = ModelType.NEWER_THINKING
        elif raw_type in ("standardchat", "standard_chat", "standard", "chat"):
            self.model_type_override = ModelType.STANDARD_CHAT
        else:
            self.model_type_override = None  # Auto

        self.effort = config.get("AzerothFriend.LLM.Thinking.Effort", DEFAULT_THINKING_EFFORT).strip().lower()
        if self.effort not in ("minimal", "low", "medium", "high"):
            self.effort = "low"

        try:
            self.token_reserve = int(config.get("AzerothFriend.LLM.Thinking.TokenReserve", str(DEFAULT_THINKING_TOKEN_RESERVE)))
            if self.token_reserve < 0:
                self.token_reserve = 0
        except ValueError:
            self.token_reserve = DEFAULT_THINKING_TOKEN_RESERVE

        # Parse AutoKinds
        raw_kinds = config.get("AzerothFriend.LLM.Thinking.AutoKinds", "")
        if raw_kinds.strip():
            self.auto_kinds: Set[str] = {k.strip().lower() for k in raw_kinds.split(",") if k.strip()}
        else:
            self.auto_kinds = set(DEFAULT_THINKING_AUTOKINDS)

        # Cache of models rejected at runtime for reasoning parameters
        self._unsupported_models: Set[str] = set()

        logger.info(
            "ThinkingPolicy initialized: Mode=%s, AutoDetect=%s, ModelTypeOverride=%s, Effort=%s, TokenReserve=%d, AutoKinds=%s",
            self.mode.value,
            self.autodetect,
            self.model_type_override.value if self.model_type_override else "Auto",
            self.effort,
            self.token_reserve,
            sorted(list(self.auto_kinds)),
        )

    def mark_unsupported(self, model_name: str) -> None:
        """Mark a model as unsupported after an API parameter rejection."""
        leaf = extract_model_leaf(model_name)
        self._unsupported_models.add(leaf)
        logger.warning("Marked model '%s' as unsupported for reasoning in this session", leaf)

    def is_marked_unsupported(self, model_name: str) -> bool:
        leaf = extract_model_leaf(model_name)
        return leaf in self._unsupported_models

    def classify_model(self, model_name: str) -> ModelType:
        """Classify a model into StandardChat, LegacyThinking, or NewerThinking."""
        if self.model_type_override is not None:
            return self.model_type_override

        leaf = extract_model_leaf(model_name)
        if leaf in self._unsupported_models:
            return ModelType.STANDARD_CHAT

        if not self.autodetect:
            return ModelType.STANDARD_CHAT

        # Chat-only explicitly suffixed models do not reason
        if "-chat" in leaf:
            return ModelType.STANDARD_CHAT

        # GPT-4.1 or older GPT-4/3.5 models are standard chat
        if leaf.startswith("gpt-4") or leaf.startswith("gpt-3"):
            return ModelType.STANDARD_CHAT

        # Legacy reasoning models: o1, o3, o4 series
        if leaf.startswith("o1") or leaf.startswith("o3") or leaf.startswith("o4"):
            return ModelType.LEGACY_THINKING

        # Newer reasoning models: GPT-5 family (e.g. gpt-5, gpt-5-nano, gpt-5-mini)
        if leaf.startswith("gpt-5"):
            return ModelType.NEWER_THINKING

        return ModelType.STANDARD_CHAT

    def normalize_effort(self, effort: str, model_type: ModelType) -> str:
        """Normalize reasoning effort based on model capabilities."""
        eff = effort.strip().lower()
        if eff not in ("minimal", "low", "medium", "high"):
            eff = "low"

        # Legacy o-series models only support low/medium/high.
        # Minimal effort collapses to low on legacy thinking models.
        if eff == "minimal" and model_type == ModelType.LEGACY_THINKING:
            return "low"

        return eff

    def decide(self, event_type: Optional[str], model_name: str) -> ThinkingDecision:
        """Determine whether to apply thinking and compute parameters."""
        model_type = self.classify_model(model_name)

        # Standard models never receive thinking fields
        if model_type == ModelType.STANDARD_CHAT:
            return ThinkingDecision(
                apply=False,
                effort="",
                model_type=model_type,
                token_reserve=0,
                reason="standard-chat-unsupported",
            )

        eff = self.normalize_effort(self.effort, model_type)

        if self.mode == ThinkingMode.OFF:
            # For models whose reasoning cannot be disabled (legacy o1/o3),
            # send lowest documented effort ("low") without pretending reasoning is off.
            if model_type == ModelType.LEGACY_THINKING:
                return ThinkingDecision(
                    apply=True,
                    effort="low",
                    model_type=model_type,
                    token_reserve=self.token_reserve,
                    reason="legacy-always-on-lowest",
                )
            return ThinkingDecision(
                apply=False,
                effort="",
                model_type=model_type,
                token_reserve=0,
                reason="mode-off",
            )

        if self.mode == ThinkingMode.ON:
            return ThinkingDecision(
                apply=True,
                effort=eff,
                model_type=model_type,
                token_reserve=self.token_reserve,
                reason="mode-on",
            )

        # Mode == Auto: check event type against enabled AutoKinds
        evt = (event_type or "").strip().lower()
        if evt in self.auto_kinds:
            return ThinkingDecision(
                apply=True,
                effort=eff,
                model_type=model_type,
                token_reserve=self.token_reserve,
                reason="autokind-matched",
            )

        # If not matched, legacy models still reason (always on) with lowest level
        if model_type == ModelType.LEGACY_THINKING:
            return ThinkingDecision(
                apply=True,
                effort="low",
                model_type=model_type,
                token_reserve=self.token_reserve,
                reason="legacy-always-on-autokind-fallback",
            )

        return ThinkingDecision(
            apply=False,
            effort="",
            model_type=model_type,
            token_reserve=0,
            reason="autokind-skipped",
        )
