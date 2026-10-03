"""Sample tactics plugin demonstrating plugin lifecycle and custom RPC."""

import logging
from typing import Any, Callable, Dict
from friend_plugins import PluginBase

logger = logging.getLogger("azeroth_friend.plugins.sample_tactics")


class Plugin(PluginBase):
    def on_load(self, context: Dict[str, Any]) -> None:
        logger.info("SampleTactics plugin loaded into bridge runtime")
        self.event_counter = 0

    def on_event(self, event_type: str, payload: Dict[str, Any], bot_info: Dict[str, Any]) -> None:
        self.event_counter += 1

    def before_shutdown(self) -> None:
        logger.info("SampleTactics shutting down cleanly. Total events observed: %d", self.event_counter)

    def get_rpc_methods(self) -> Dict[str, Callable[..., Any]]:
        return {
            "tactics.status": self._rpc_tactics_status
        }

    def _rpc_tactics_status(self) -> Dict[str, Any]:
        return {
            "plugin": self.name,
            "version": self.version,
            "events_observed": self.event_counter,
            "active_strategy": "adaptive_defense"
        }
