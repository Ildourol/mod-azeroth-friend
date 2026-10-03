"""Modular Declarative Plugin Engine for AzerothFriend Python Bridge.

Provides dynamic, manifest-driven extension loading (tools/plugins/*/plugin.json)
for custom companion tactical behaviors, event hooks, and RPC endpoints.
Adapted from agent-wow architectural innovations (MAF-060).
"""

import importlib.util
import json
import logging
import os
import sys
from typing import Any, Callable, Dict, List, Optional

logger = logging.getLogger("azeroth_friend.plugins")


class PluginBase:
    """Base class for mod-azeroth-friend plugins."""

    def __init__(self, manifest: Dict[str, Any], plugin_dir: str):
        self.manifest = manifest
        self.plugin_dir = plugin_dir
        self.name: str = manifest.get("name") or os.path.basename(plugin_dir)
        self.version: str = manifest.get("version") or "1.0.0"
        self.description: str = manifest.get("description") or ""

    def on_load(self, context: Dict[str, Any]) -> None:
        """Called once when bridge starts or plugin is dynamically loaded."""
        pass

    def on_event(self, event_type: str, payload: Dict[str, Any], bot_info: Dict[str, Any]) -> None:
        """Called on every event loop iteration for active companion events."""
        pass

    def before_shutdown(self) -> None:
        """Called during bridge 2-phase shutdown drain."""
        pass

    def get_rpc_methods(self) -> Dict[str, Callable[..., Any]]:
        """Return custom RPC methods to register with the JSON-RPC gateway."""
        return {}


class PluginManager:
    """Discovers, loads, and manages lifecycle of modular plugins."""

    def __init__(self, plugins_dir: Optional[str] = None):
        if plugins_dir is None:
            plugins_dir = os.path.join(os.path.dirname(__file__), "plugins")
        self.plugins_dir = plugins_dir
        self.plugins: Dict[str, PluginBase] = {}
        self._loaded = False

    def discover_and_load(self, context: Optional[Dict[str, Any]] = None) -> int:
        """Scan plugins directory for valid plugins with plugin.json manifests."""
        if not os.path.isdir(self.plugins_dir):
            try:
                os.makedirs(self.plugins_dir, exist_ok=True)
            except Exception:
                pass
            return 0

        context = context or {}
        count = 0

        for entry in os.listdir(self.plugins_dir):
            p_dir = os.path.join(self.plugins_dir, entry)
            if not os.path.isdir(p_dir):
                continue

            manifest_path = os.path.join(p_dir, "plugin.json")
            if not os.path.isfile(manifest_path):
                continue

            try:
                with open(manifest_path, "r", encoding="utf-8") as f:
                    manifest = json.load(f)

                entry_file = manifest.get("entrypoint") or "plugin.py"
                script_path = os.path.join(p_dir, entry_file)
                if not os.path.isfile(script_path):
                    logger.warning("Plugin '%s' missing entrypoint file: %s", entry, entry_file)
                    continue

                module_name = f"af_plugin_{entry}"
                spec = importlib.util.spec_from_file_location(module_name, script_path)
                if not spec or not spec.loader:
                    continue

                mod = importlib.util.module_from_spec(spec)
                sys.modules[module_name] = mod
                spec.loader.exec_module(mod)

                # Look for Plugin class or get_plugin() factory
                plugin_cls = getattr(mod, "Plugin", None)
                if plugin_cls and issubclass(plugin_cls, PluginBase):
                    instance = plugin_cls(manifest, p_dir)
                elif hasattr(mod, "get_plugin"):
                    instance = mod.get_plugin(manifest, p_dir)
                else:
                    logger.warning("Plugin '%s' does not export Plugin class or get_plugin()", entry)
                    continue

                instance.on_load(context)
                self.plugins[instance.name] = instance
                count += 1
                logger.info("Loaded plugin '%s' v%s: %s", instance.name, instance.version, instance.description)

            except Exception as e:
                logger.exception("Failed to load plugin from %s: %s", p_dir, e)

        self._loaded = True
        return count

    def dispatch_event(self, event_type: str, payload: Dict[str, Any], bot_info: Dict[str, Any]) -> None:
        """Forward an engine event to all registered plugins."""
        for name, plugin in self.plugins.items():
            try:
                plugin.on_event(event_type, payload, bot_info)
            except Exception as e:
                logger.error("Error in plugin '%s' handling event '%s': %s", name, event_type, e)

    def shutdown_all(self) -> None:
        """Gracefully drain and notify all plugins before process exit."""
        for name, plugin in self.plugins.items():
            try:
                plugin.before_shutdown()
            except Exception as e:
                logger.error("Error shutting down plugin '%s': %s", name, e)
        self.plugins.clear()

    def register_rpc_with_server(self, rpc_server: Any) -> int:
        """Expose all plugin-contributed RPC methods to the JSON-RPC server."""
        if not rpc_server or not hasattr(rpc_server, "register_method"):
            return 0
        added = 0
        for p_name, plugin in self.plugins.items():
            try:
                methods = plugin.get_rpc_methods()
                for m_name, func in methods.items():
                    # Prefix method if not already prefixed
                    rpc_name = m_name if "." in m_name else f"plugin.{p_name}.{m_name}"
                    rpc_server.register_method(rpc_name, func)
                    added += 1
            except Exception as e:
                logger.error("Failed registering RPC methods for plugin '%s': %s", p_name, e)
        return added
