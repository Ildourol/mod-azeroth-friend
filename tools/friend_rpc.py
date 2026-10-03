"""JSON-RPC 2.0 Gateway Server for AzerothFriend Python Bridge.

Exposes a lightweight, loopback HTTP JSON-RPC 2.0 endpoint (127.0.0.1:8378)
for administrative inspection, CLI tools (af_ctl.py), health diagnostics,
and runtime companion control.
Adapted from agent-wow architectural innovations (MAF-060).
"""

import http.server
import json
import logging
import socketserver
import threading
import time
from typing import Any, Callable, Dict, List, Optional

logger = logging.getLogger("azeroth_friend.rpc")


class JSONRPCError(Exception):
    def __init__(self, code: int, message: str, data: Any = None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.data = data

    def to_dict(self) -> Dict[str, Any]:
        res: Dict[str, Any] = {"code": self.code, "message": self.message}
        if self.data is not None:
            res["data"] = self.data
        return res


class JSONRPCServer:
    """Threaded loopback JSON-RPC 2.0 server."""

    def __init__(self, host: str = "127.0.0.1", port: int = 8378, bridge: Any = None):
        self.host = host
        self.port = port
        self.bridge = bridge
        self.methods: Dict[str, Callable[..., Any]] = {}
        self._server: Optional[socketserver.TCPServer] = None
        self._thread: Optional[threading.Thread] = None
        self._running = False
        self._start_time = time.monotonic()
        self._register_default_methods()

    def register_method(self, name: str, func: Callable[..., Any]) -> None:
        self.methods[name] = func

    def _register_default_methods(self) -> None:
        self.register_method("session.status", self._rpc_session_status)
        self.register_method("session.health", self._rpc_session_health)
        self.register_method("session.shutdown", self._rpc_session_shutdown)
        self.register_method("companion.list", self._rpc_companion_list)
        self.register_method("companion.status", self._rpc_companion_status)
        self.register_method("companion.action_invoke", self._rpc_companion_action_invoke)
        self.register_method("companion.goal_set", self._rpc_companion_goal_set)
        self.register_method("companion.mode_set", self._rpc_companion_mode_set)
        self.register_method("companion.autonomy_set", self._rpc_companion_autonomy_set)
        self.register_method("world.search_zone", self._rpc_world_search_zone)
        self.register_method("world.catalog_stats", self._rpc_world_catalog_stats)

    def dispatch(self, request_bytes: bytes) -> bytes:
        try:
            req = json.loads(request_bytes.decode("utf-8"))
        except Exception as e:
            return json.dumps({
                "jsonrpc": "2.0",
                "error": {"code": -32700, "message": f"Parse error: {e}"},
                "id": None
            }).encode("utf-8")

        if not isinstance(req, dict):
            return json.dumps({
                "jsonrpc": "2.0",
                "error": {"code": -32600, "message": "Invalid Request"},
                "id": None
            }).encode("utf-8")

        req_id = req.get("id")
        method_name = req.get("method")
        params = req.get("params")

        if not method_name or not isinstance(method_name, str):
            return json.dumps({
                "jsonrpc": "2.0",
                "error": {"code": -32600, "message": "Invalid method name"},
                "id": req_id
            }).encode("utf-8")

        handler = self.methods.get(method_name)
        if not handler:
            return json.dumps({
                "jsonrpc": "2.0",
                "error": {"code": -32601, "message": f"Method not found: {method_name}"},
                "id": req_id
            }).encode("utf-8")

        try:
            if params is None:
                result = handler()
            elif isinstance(params, list):
                result = handler(*params)
            elif isinstance(params, dict):
                result = handler(**params)
            else:
                result = handler(params)

            return json.dumps({
                "jsonrpc": "2.0",
                "result": result,
                "id": req_id
            }, ensure_ascii=False).encode("utf-8")
        except JSONRPCError as err:
            return json.dumps({
                "jsonrpc": "2.0",
                "error": err.to_dict(),
                "id": req_id
            }).encode("utf-8")
        except Exception as err:
            logger.exception("Error executing RPC method %s", method_name)
            return json.dumps({
                "jsonrpc": "2.0",
                "error": {"code": -32000, "message": str(err)},
                "id": req_id
            }).encode("utf-8")

    # --- Default RPC Handlers ---

    def _rpc_session_status(self) -> Dict[str, Any]:
        uptime_sec = round(time.monotonic() - self._start_time, 1)
        bots_count = 0
        livestate_connected = False
        if self.bridge:
            if hasattr(self.bridge, "state_store"):
                bots_count = len(getattr(self.bridge.state_store, "_bots", {}))
            if hasattr(self.bridge, "livestate_client"):
                client = getattr(self.bridge, "livestate_client")
                livestate_connected = bool(client and getattr(client, "connected", False))
        return {
            "uptime_seconds": uptime_sec,
            "managed_bots": bots_count,
            "livestate_connected": livestate_connected,
            "rpc_methods": sorted(list(self.methods.keys()))
        }

    def _rpc_session_health(self) -> Dict[str, Any]:
        health: Dict[str, Any] = {"status": "ok", "checks": {}}
        if self.bridge and hasattr(self.bridge, "db_mgr"):
            try:
                db = self.bridge.db_mgr
                with db.get_connection():
                    health["checks"]["mysql"] = "connected"
            except Exception as e:
                health["checks"]["mysql"] = f"error: {e}"
                health["status"] = "degraded"
        return health

    def _rpc_session_shutdown(self) -> Dict[str, str]:
        def do_stop():
            time.sleep(0.5)
            if self.bridge and hasattr(self.bridge, "stop"):
                self.bridge.stop()

        threading.Thread(target=do_stop, daemon=True).start()
        return {"status": "shutting_down"}

    def _rpc_companion_list(self) -> List[Dict[str, Any]]:
        if not self.bridge or not hasattr(self.bridge, "db_mgr"):
            return []
        try:
            bots = self.bridge.db_mgr.fetch_all_controlled_bots()
            return bots or []
        except Exception as e:
            raise JSONRPCError(-32000, f"Database query failed: {e}")

    def _rpc_companion_status(self, bot_guid: int) -> Dict[str, Any]:
        if not self.bridge or not hasattr(self.bridge, "db_mgr"):
            raise JSONRPCError(-32000, "Bridge not attached")
        try:
            bot = self.bridge.db_mgr.fetch_bot_info(int(bot_guid))
            if not bot:
                raise JSONRPCError(-32004, f"Bot GUID {bot_guid} not found")
            return bot
        except JSONRPCError:
            raise
        except Exception as e:
            raise JSONRPCError(-32000, f"Query failed: {e}")

    def _rpc_companion_action_invoke(self, bot_guid: int, action: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        if not self.bridge or not hasattr(self.bridge, "db_mgr"):
            raise JSONRPCError(-32000, "Bridge not attached")
        params = params or {}
        try:
            plan_id = "rpc_" + str(int(time.time()))[-6:]
            step = {"action": action, "params": params}
            bot = self.bridge.db_mgr.fetch_bot_info(int(bot_guid))
            rev = int((bot or {}).get("control_revision") or 1)
            self.bridge.db_mgr.insert_action_plan(
                int(bot_guid), plan_id, [step],
                thought=f"Manual RPC action '{action}' submitted by operator.",
                authority="owner_command",
                revision=rev
            )
            return {"status": "dispatched", "plan_id": plan_id, "step": step}
        except Exception as e:
            raise JSONRPCError(-32000, f"Failed to invoke action: {e}")

    def _rpc_companion_goal_set(self, bot_guid: int, goal: str, goal_type: str = "short") -> Dict[str, Any]:
        if not self.bridge or not hasattr(self.bridge, "db_mgr"):
            raise JSONRPCError(-32000, "Bridge not attached")
        try:
            bot = self.bridge.db_mgr.fetch_bot_info(int(bot_guid))
            if not bot:
                raise JSONRPCError(-32004, f"Bot GUID {bot_guid} not found")
            rev = int(bot.get("control_revision") or 1)
            self.bridge.db_mgr.update_goal(int(bot_guid), rev, {"goal": goal, "status": "active"})
            return {"status": "updated", "bot_guid": bot_guid, "goal": goal}
        except JSONRPCError:
            raise
        except Exception as e:
            raise JSONRPCError(-32000, f"Failed to set goal: {e}")

    def _rpc_companion_mode_set(self, bot_guid: int, mode: str) -> Dict[str, Any]:
        mode = mode.lower().strip()
        if mode not in ("combat", "travel", "idle", "social"):
            raise JSONRPCError(-32602, f"Invalid mode '{mode}'. Choose from: combat, travel, idle, social")
        return self._rpc_companion_action_invoke(bot_guid, "set_action_mode", {"mode": mode})

    def _rpc_companion_autonomy_set(self, bot_guid: int, enabled: bool) -> Dict[str, Any]:
        val = "on" if enabled else "off"
        return self._rpc_companion_action_invoke(bot_guid, "set_autonomy", {"value": val})

    def _rpc_world_search_zone(self, query: str, limit: int = 10) -> List[Dict[str, Any]]:
        try:
            from friend_catalogs import WorldCatalog
            cat = WorldCatalog.get_instance()
            return cat.search_zones(query, limit=int(limit))
        except Exception as e:
            raise JSONRPCError(-32000, f"Catalog search failed: {e}")

    def _rpc_world_catalog_stats(self) -> Dict[str, Any]:
        try:
            from friend_catalogs import WorldCatalog
            cat = WorldCatalog.get_instance()
            return cat.get_stats()
        except Exception as e:
            raise JSONRPCError(-32000, f"Stats failed: {e}")

    # --- Server Lifecycle ---

    def start(self) -> None:
        if self._running:
            return

        rpc_server_inst = self

        class _Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                if self.path != "/rpc":
                    self.send_response(404)
                    self.end_headers()
                    return
                content_len = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(content_len)
                response = rpc_server_inst.dispatch(body)
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

            def log_message(self, format, *args):
                # Suppress normal HTTP access logs from polluting console
                pass

        try:
            socketserver.TCPServer.allow_reuse_address = True
            self._server = socketserver.TCPServer((self.host, self.port), _Handler)
            self._running = True
            self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
            self._thread.start()
            logger.info("JSON-RPC 2.0 gateway listening on http://%s:%d/rpc", self.host, self.port)
        except Exception as e:
            logger.warning("Could not bind JSON-RPC gateway on %s:%d: %s", self.host, self.port, e)
            self._running = False

    def stop(self) -> None:
        if not self._running:
            return
        self._running = False
        if self._server:
            try:
                self._server.shutdown()
                self._server.server_close()
            except Exception:
                pass
        if self._thread:
            self._thread.join(timeout=1.0)
        logger.info("JSON-RPC 2.0 gateway stopped")
