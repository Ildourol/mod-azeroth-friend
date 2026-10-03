#!/usr/bin/env python3
"""af_ctl.py - AzerothFriend Bridge CLI Remote Controller.

Communicates with the running mod-azeroth-friend Python bridge via loopback
JSON-RPC 2.0 (http://127.0.0.1:8378/rpc).
Adapted from agent-wow architectural innovations (MAF-060).

Usage:
    python tools/af_ctl.py status
    python tools/af_ctl.py health
    python tools/af_ctl.py list
    python tools/af_ctl.py bot <guid>
    python tools/af_ctl.py mode <guid> <combat|travel|idle|social>
    python tools/af_ctl.py goal <guid> "New goal text"
    python tools/af_ctl.py autonomy <guid> <on|off>
    python tools/af_ctl.py invoke <guid> <action> [params_json]
    python tools/af_ctl.py zone <query>
    python tools/af_ctl.py stats
"""

import json
import sys
import urllib.error
import urllib.request
from typing import Any, Dict

DEFAULT_URL = "http://127.0.0.1:8378/rpc"


def rpc_call(method: str, params: Any = None, url: str = DEFAULT_URL) -> Dict[str, Any]:
    payload = {
        "jsonrpc": "2.0",
        "method": method,
        "params": params,
        "id": 1
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=5.0) as resp:
            body = resp.read().decode("utf-8")
            return json.loads(body)
    except urllib.error.URLError as e:
        print(f"Error: Could not connect to bridge RPC at {url}: {e}", file=sys.stderr)
        print("Ensure the AzerothFriend bridge is running with RPC enabled.", file=sys.stderr)
        sys.exit(1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(0)

    cmd = sys.argv[1].lower().strip()

    if cmd == "status":
        res = rpc_call("session.status")
        print(json.dumps(res.get("result", res), indent=2))

    elif cmd == "health":
        res = rpc_call("session.health")
        print(json.dumps(res.get("result", res), indent=2))

    elif cmd == "list":
        res = rpc_call("companion.list")
        bots = res.get("result", [])
        if not bots:
            print("No managed bots currently registered.")
        else:
            print(f"Managed Companion Bots ({len(bots)}):")
            for b in bots:
                guid = b.get("guid") or b.get("bot_guid")
                name = b.get("name") or b.get("bot_name")
                goal = b.get("current_goal") or b.get("goal") or "None"
                mode = b.get("action_mode") or "idle"
                print(f"  - [{guid}] {name} (Mode: {mode}, Goal: '{goal}')")

    elif cmd == "bot":
        if len(sys.argv) < 3:
            print("Usage: af_ctl.py bot <guid>")
            sys.exit(1)
        guid = int(sys.argv[2])
        res = rpc_call("companion.status", {"bot_guid": guid})
        print(json.dumps(res.get("result", res), indent=2))

    elif cmd == "mode":
        if len(sys.argv) < 4:
            print("Usage: af_ctl.py mode <guid> <combat|travel|idle|social>")
            sys.exit(1)
        guid = int(sys.argv[2])
        mode = sys.argv[3]
        res = rpc_call("companion.mode_set", {"bot_guid": guid, "mode": mode})
        print(json.dumps(res.get("result", res), indent=2))

    elif cmd == "goal":
        if len(sys.argv) < 4:
            print("Usage: af_ctl.py goal <guid> <goal_text>")
            sys.exit(1)
        guid = int(sys.argv[2])
        goal = sys.argv[3]
        res = rpc_call("companion.goal_set", {"bot_guid": guid, "goal": goal})
        print(json.dumps(res.get("result", res), indent=2))

    elif cmd == "autonomy":
        if len(sys.argv) < 4:
            print("Usage: af_ctl.py autonomy <guid> <on|off>")
            sys.exit(1)
        guid = int(sys.argv[2])
        enabled = sys.argv[3].lower() in ("1", "true", "on", "yes")
        res = rpc_call("companion.autonomy_set", {"bot_guid": guid, "enabled": enabled})
        print(json.dumps(res.get("result", res), indent=2))

    elif cmd == "invoke":
        if len(sys.argv) < 4:
            print("Usage: af_ctl.py invoke <guid> <action> [params_json]")
            sys.exit(1)
        guid = int(sys.argv[2])
        action = sys.argv[3]
        params = {}
        if len(sys.argv) >= 5:
            try:
                params = json.loads(sys.argv[4])
            except Exception as e:
                print(f"Error parsing params JSON: {e}", file=sys.stderr)
                sys.exit(1)
        res = rpc_call("companion.action_invoke", {"bot_guid": guid, "action": action, "params": params})
        print(json.dumps(res.get("result", res), indent=2))

    elif cmd == "zone":
        if len(sys.argv) < 3:
            print("Usage: af_ctl.py zone <query>")
            sys.exit(1)
        query = sys.argv[2]
        res = rpc_call("world.search_zone", {"query": query})
        print(json.dumps(res.get("result", res), indent=2))

    elif cmd == "stats":
        res = rpc_call("world.catalog_stats")
        print(json.dumps(res.get("result", res), indent=2))

    elif cmd == "shutdown":
        res = rpc_call("session.shutdown")
        print(json.dumps(res.get("result", res), indent=2))

    else:
        print(f"Unknown command '{cmd}'. Run af_ctl.py without arguments for usage.")
        sys.exit(1)


if __name__ == "__main__":
    main()
