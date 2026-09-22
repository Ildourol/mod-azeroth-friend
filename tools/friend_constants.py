"""Constants and default configuration parameters for AzerothFriend."""

DEFAULT_OPENAI_BASE_URL = "https://api.openai.com/v1"
DEFAULT_OLLAMA_BASE_URL = "http://localhost:11434/v1"
DEFAULT_OPENROUTER_BASE_URL = "https://openrouter.ai/api/v1"

DEFAULT_MODEL = "gpt-4o-mini"
DEFAULT_MAX_TOKENS = 800
DEFAULT_TEMPERATURE = 0.7

# Thinking / Reasoning model defaults
DEFAULT_THINKING_MODE = "Auto"
DEFAULT_THINKING_AUTODETECT = True
DEFAULT_THINKING_MODEL_TYPE = "Auto"
DEFAULT_THINKING_EFFORT = "low"
DEFAULT_THINKING_TOKEN_RESERVE = 1024
DEFAULT_THINKING_AUTOKINDS = {
    "player_command",
    "combat_enter",
    "duel_requested",
    "trade_requested",
    "plan_interrupted",
}

# mod-llm-chatter Token Sharing & Sensory Reuse defaults
DEFAULT_TOKEN_SHARING_ENABLE = True
DEFAULT_SENSORY_REUSE_ENABLE = True
DEFAULT_ESTIMATED_TOKENS_PER_CALL = 1200
DEFAULT_CHATTER_SENSORY_POLL_INTERVAL = 2.0
DEFAULT_HEAR_OTHER_BOTS = True
DEFAULT_HEAR_NPCS = True
DEFAULT_MAX_RECENT_DIALOGUE = 6

# The action vocabulary is owned by friend_action_catalog (mirroring the C++
# AzerothFriendActionRegistry). Anything not in that catalogue is refused by the
# server-side executor, so the planner validates against it before queueing steps.
from friend_action_catalog import CATALOG, VALID_EMOTES as _CATALOG_EMOTES  # noqa: E402

VALID_ACTIONS = set(CATALOG.keys())
VALID_EMOTES = set(_CATALOG_EMOTES)

# Cognitive Mindset & Commitment Latency
DEFAULT_MINDSET_ENABLE = True
DEFAULT_MINDSET_COMMITMENT_SECONDS = 15.0
DEFAULT_MINDSET_ALLOW_OPPORTUNISTIC = True

VALID_MINDSETS = {
    "FOLLOWING",
    "COMBAT",
    "LOOTING",
    "RESTING",
    "EXPLORING",
    "SOCIAL",
    "IDLE",
}

# Situational Action Modes (Tactical Toggles)
VALID_ACTION_MODES = {
    "combat",
    "travel",
    "idle",
    "social",
}

ACTION_MODE_DEFAULT_MINDSETS = {
    "combat": "COMBAT",
    "travel": "FOLLOWING",
    "idle": "RESTING",
    "social": "SOCIAL",
}

MINDSET_INTERRUPT_EVENTS = {
    "player_command",
    "combat_enter",
    "health_critical",
    "duel_requested",
    "trade_requested",
    "plan_interrupted",
}
