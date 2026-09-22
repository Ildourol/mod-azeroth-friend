"""LLM client interface supporting OpenAI-compatible endpoints, thinking models, Ollama, and OpenRouter."""

import json
import logging
from typing import Any, Dict, List, Optional
from openai import OpenAI

from friend_constants import (
    DEFAULT_MAX_TOKENS,
    DEFAULT_MODEL,
    DEFAULT_OLLAMA_BASE_URL,
    DEFAULT_OPENAI_BASE_URL,
    DEFAULT_OPENROUTER_BASE_URL,
    DEFAULT_TEMPERATURE,
)
from friend_thinking import (
    ModelType,
    ThinkingDecision,
    ThinkingPolicy,
    is_reasoning_parameter_rejection,
    is_temperature_rejection,
)

logger = logging.getLogger("azeroth_friend.llm")


class LLMClient:
    def __init__(self, config: Dict[str, str]):
        self.provider = config.get("AzerothFriend.LLM.Provider", "openai").lower()
        self.model = config.get("AzerothFriend.LLM.Model", DEFAULT_MODEL)
        self.max_tokens = int(config.get("AzerothFriend.LLM.MaxTokens", str(DEFAULT_MAX_TOKENS)))
        self.temperature = float(config.get("AzerothFriend.LLM.Temperature", str(DEFAULT_TEMPERATURE)))
        self.api_key = config.get("AzerothFriend.LLM.ApiKey", "dummy-key")

        # Thinking policy and model classification
        self.thinking_policy = ThinkingPolicy(config)

        # Determine base URL
        default_url = DEFAULT_OPENAI_BASE_URL
        if self.provider == "ollama":
            default_url = DEFAULT_OLLAMA_BASE_URL
        elif self.provider == "openrouter":
            default_url = DEFAULT_OPENROUTER_BASE_URL

        self.base_url = config.get("AzerothFriend.LLM.BaseUrl", default_url)

        logger.info(
            "Initializing LLM client: Provider=%s, BaseUrl=%s, Model=%s, MaxTokens=%d, Temperature=%.2f",
            self.provider,
            self.base_url,
            self.model,
            self.max_tokens,
            self.temperature,
        )

        self.client = OpenAI(
            api_key=self.api_key if self.api_key else "ollama",
            base_url=self.base_url,
        )

    def _build_completion_kwargs(
        self,
        messages: List[Dict[str, str]],
        decision: ThinkingDecision,
        fallback_standard: bool = False,
        omit_temperature: bool = False,
    ) -> Dict[str, Any]:
        """Construct the request parameters tailored to the model type and thinking decision."""
        kwargs: Dict[str, Any] = {
            "model": self.model,
            "messages": messages,
            "response_format": {"type": "json_object"},
        }

        # Effective token budget (with reasoning token reserve for reasoning models)
        is_reasoning_model = decision.model_type in (ModelType.LEGACY_THINKING, ModelType.NEWER_THINKING)
        token_reserve = decision.token_reserve if decision.token_reserve > 0 else 1024
        effective_tokens = self.max_tokens + (token_reserve if (is_reasoning_model and not fallback_standard) else 0)
        if is_reasoning_model and effective_tokens < 2048:
            effective_tokens = 2048

        # Standard OpenAI and modern gateways use max_completion_tokens
        kwargs["max_completion_tokens"] = effective_tokens

        # Temperature handling:
        # OpenAI reasoning models (o1, o3, gpt-5) strictly reject custom temperature (causes 400 error).
        # Only pass custom temperature for standard chat models, unless omit_temperature is requested.
        if not omit_temperature:
            if decision.model_type == ModelType.STANDARD_CHAT:
                kwargs["temperature"] = self.temperature
            elif self.temperature == 1.0:
                kwargs["temperature"] = 1.0

        # Reasoning effort handling:
        if not fallback_standard:
            if decision.apply and decision.effort:
                kwargs["reasoning_effort"] = decision.effort
            elif is_reasoning_model and decision.model_type == ModelType.NEWER_THINKING:
                kwargs["reasoning_effort"] = "low"

        return kwargs

    def _parse_completion_response(self, response: Any) -> Optional[Dict[str, Any]]:
        """Extract structured JSON and execution telemetry from completion response."""
        choice = response.choices[0]
        content = choice.message.content
        if not content:
            finish_reason = getattr(choice, "finish_reason", "unknown")
            usage_obj = getattr(response, "usage", None)
            reasoning_tokens = 0
            if usage_obj and hasattr(usage_obj, "completion_tokens_details") and usage_obj.completion_tokens_details:
                reasoning_tokens = getattr(usage_obj.completion_tokens_details, "reasoning_tokens", 0)
            logger.warning(
                "Empty response from LLM (finish_reason=%s, reasoning_tokens=%s, completion_tokens=%s)",
                finish_reason,
                reasoning_tokens,
                getattr(usage_obj, "completion_tokens", 0) if usage_obj else 0,
            )
            return None

        data = json.loads(content)
        if not isinstance(data, dict):
            data = {"thought": str(data), "plan": []}

        meta: Dict[str, Any] = {
            "model": getattr(response, "model", self.model),
        }
        msg_obj = choice.message
        reasoning_content = getattr(msg_obj, "reasoning_content", None)
        if not reasoning_content and hasattr(msg_obj, "model_extra") and isinstance(msg_obj.model_extra, dict):
            reasoning_content = msg_obj.model_extra.get("reasoning_content")
        if reasoning_content:
            meta["reasoning_content"] = str(reasoning_content).strip()

        usage = getattr(response, "usage", None)
        if usage:
            details = getattr(usage, "completion_tokens_details", None)
            reasoning_toks = getattr(details, "reasoning_tokens", 0) if details else 0
            meta["tokens"] = {
                "prompt": getattr(usage, "prompt_tokens", 0),
                "completion": getattr(usage, "completion_tokens", 0),
                "reasoning": reasoning_toks,
                "total": getattr(usage, "total_tokens", 0),
            }

        data["_meta"] = meta
        return data

    def generate_json_plan(
        self,
        messages: List[Dict[str, str]],
        event_type: Optional[str] = None,
    ) -> Optional[Dict[str, Any]]:
        """Call the LLM endpoint requesting structured JSON output with adaptive thinking controls."""
        decision = self.thinking_policy.decide(event_type, self.model)
        kwargs = self._build_completion_kwargs(messages, decision)

        logger.debug(
            "Generating plan with model=%s, event=%s, thinking_apply=%s, effort=%s, model_type=%s, tokens=%s",
            self.model,
            event_type,
            decision.apply,
            decision.effort or "none",
            decision.model_type.value,
            kwargs.get("max_completion_tokens") or kwargs.get("max_tokens"),
        )

        try:
            response = self.client.chat.completions.create(**kwargs)
            return self._parse_completion_response(response)

        except Exception as e:
            err_str = str(e)

            # 1. Detect custom temperature rejection (reasoning models enforce default 1.0)
            if is_temperature_rejection(err_str) and "temperature" in kwargs:
                logger.warning(
                    "LLM endpoint rejected custom temperature (%s). Retrying without temperature parameter...",
                    err_str,
                )
                retry_kwargs = dict(kwargs)
                retry_kwargs.pop("temperature", None)
                try:
                    retry_resp = self.client.chat.completions.create(**retry_kwargs)
                    return self._parse_completion_response(retry_resp)
                except Exception as retry_err:
                    err_str = str(retry_err)

            # 2. Detect reasoning parameter rejection (e.g. reasoning_effort unsupported)
            if is_reasoning_parameter_rejection(err_str):
                logger.warning(
                    "LLM rejected reasoning parameters (%s). Marking model '%s' unsupported and retrying without reasoning...",
                    err_str,
                    self.model,
                )
                self.thinking_policy.mark_unsupported(self.model)

                fallback_decision = self.thinking_policy.decide(event_type, self.model)
                fallback_kwargs = self._build_completion_kwargs(
                    messages, fallback_decision, fallback_standard=True
                )

                try:
                    retry_resp = self.client.chat.completions.create(**fallback_kwargs)
                    return self._parse_completion_response(retry_resp)
                except Exception as retry_err:
                    err_retry_str = str(retry_err)
                    # If older gateway strictly requires legacy max_tokens
                    if "max_completion_tokens" in err_retry_str.lower() and "unsupported" in err_retry_str.lower():
                        legacy_kwargs = dict(fallback_kwargs)
                        legacy_kwargs.pop("max_completion_tokens", None)
                        legacy_kwargs["max_tokens"] = self.max_tokens
                        try:
                            legacy_resp = self.client.chat.completions.create(**legacy_kwargs)
                            return self._parse_completion_response(legacy_resp)
                        except Exception as legacy_err:
                            logger.error("LLM legacy fallback failed: %s", legacy_err)
                            return None
                    logger.error("LLM fallback completion failed: %s", retry_err)
                    return None

            # 3. Detect legacy gateway rejecting max_completion_tokens
            if "max_completion_tokens" in err_str.lower() and "unsupported" in err_str.lower():
                logger.warning("Gateway rejected max_completion_tokens. Retrying with legacy max_tokens...")
                legacy_kwargs = dict(kwargs)
                legacy_kwargs.pop("max_completion_tokens", None)
                legacy_kwargs["max_tokens"] = self.max_tokens
                try:
                    legacy_resp = self.client.chat.completions.create(**legacy_kwargs)
                    return self._parse_completion_response(legacy_resp)
                except Exception as legacy_err:
                    logger.error("LLM legacy max_tokens fallback failed: %s", legacy_err)
                    return None

            logger.error("LLM completion failed: %s", e)
            return None

    def get_thinking_diagnostics(self, event_type: str = "player_command") -> Dict[str, Any]:
        """Return diagnostic metadata regarding model classification and thinking decisions."""
        decision = self.thinking_policy.decide(event_type, self.model)
        return {
            "model": self.model,
            "provider": self.provider,
            "base_url": self.base_url,
            "classified_type": decision.model_type.value,
            "mode": self.thinking_policy.mode.value,
            "configured_effort": self.thinking_policy.effort,
            "effective_effort": decision.effort or "none",
            "thinking_applied": decision.apply,
            "token_reserve": decision.token_reserve if decision.apply else 0,
            "decision_reason": decision.reason,
            "auto_kinds": sorted(list(self.thinking_policy.auto_kinds)),
            "marked_unsupported": self.thinking_policy.is_marked_unsupported(self.model),
        }
