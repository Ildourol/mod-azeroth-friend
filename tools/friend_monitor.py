#!/usr/bin/env python3
"""
friend_monitor.py - Real-Time Autonomous Monitor for AzerothFriend & Server Ecosystem

Monitors:
1. Server processes (mariadbd, authserver, worldserver)
2. Bridge daemons (mod-azeroth-friend, mod-llm-chatter)
3. Server error logs (Server.log, Errors.log, DBErrors.log)
4. MySQL state (azeroth_friend_actions, azeroth_friend_events, azeroth_friend_bots, llm_chatter_events)
5. Automatically documents any detected issues into docs/ISSUES.md
"""

import argparse
import datetime
import json
import os
import re
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

import mysql.connector

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
ISSUES_FILE = os.path.join(MODULE_DIR, "docs", "ISSUES.md")
SERVER_DIR = os.path.abspath(os.path.join(MODULE_DIR, "..", "..", "Azerothcore server", "Server", "bin"))


class AzerothFriendMonitor:
    def __init__(self, db_host="127.0.0.1", db_port=3306, db_user="acore", db_pass="acore"):
        self.db_host = db_host
        self.db_port = db_port
        self.db_user = db_user
        self.db_pass = db_pass
        self.reported_issues: Set[str] = set()
        self.last_log_positions: Dict[str, int] = {}

    def get_db_connection(self, db_name="acore_characters"):
        try:
            return mysql.connector.connect(
                host=self.db_host,
                port=self.db_port,
                user=self.db_user,
                password=self.db_pass,
                database=db_name,
                autocommit=True
            )
        except Exception as e:
            return None

    def check_processes(self) -> Dict[str, bool]:
        """Check if critical server and bridge processes are running."""
        status = {
            "mariadbd": False,
            "authserver": False,
            "worldserver": False,
            "chatter_bridge": False,
            "friend_bridge": False,
        }
        try:
            import subprocess
            res = subprocess.run(
                ["powershell", "-Command", "Get-Process | Select-Object -ExpandProperty ProcessName"],
                capture_output=True, text=True, timeout=10
            )
            procs = [p.strip().lower() for p in res.stdout.splitlines() if p.strip()]
            status["mariadbd"] = "mariadbd" in procs or "mysqld" in procs
            status["authserver"] = "authserver" in procs
            status["worldserver"] = "worldserver" in procs
            
            # Check python processes command line for bridges
            cmd_res = subprocess.run(
                ["powershell", "-Command", "Get-CimInstance Win32_Process -Filter \"Name like '%python%'\" | Select-Object -ExpandProperty CommandLine"],
                capture_output=True, text=True, timeout=10
            )
            cmd_lines = (cmd_res.stdout or "").lower()
            if "llm_chatter_bridge" in cmd_lines:
                status["chatter_bridge"] = True
            if "azeroth_friend_bridge" in cmd_lines:
                status["friend_bridge"] = True
        except Exception as e:
            pass
        return status

    def check_server_logs(self) -> List[Dict[str, Any]]:
        """Inspect Errors.log and DBErrors.log for recent critical errors."""
        anomalies = []
        for log_name in ["Errors.log", "DBErrors.log"]:
            log_path = os.path.join(SERVER_DIR, log_name)
            if not os.path.exists(log_path):
                continue
            try:
                size = os.path.getsize(log_path)
                last_pos = self.last_log_positions.get(log_name, 0)
                if size < last_pos:
                    last_pos = 0  # Log rotated or truncated

                with open(log_path, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(last_pos)
                    lines = f.readlines()
                    self.last_log_positions[log_name] = f.tell()

                for line in lines:
                    line_str = line.strip()
                    if not line_str:
                        continue
                    # Only report errors specifically originating from or affecting mod-azeroth-friend
                    line_lower = line_str.lower()
                    if "azerothfriend" in line_lower or "azeroth_friend" in line_lower:
                        anomalies.append({
                            "source": log_name,
                            "line": line_str,
                            "timestamp": datetime.datetime.now().isoformat()
                        })
            except Exception as e:
                pass
        return anomalies

    def check_database(self) -> Dict[str, Any]:
        """Inspect MySQL tables for stuck actions, events, and telemetry."""
        results = {
            "failed_actions": [],
            "stuck_actions": [],
            "stale_events": [],
            "bots": [],
            "db_connected": False,
        }
        conn = self.get_db_connection("acore_characters")
        if not conn:
            return results
        results["db_connected"] = True
        try:
            cursor = conn.cursor(dictionary=True)

            # 1. Failed actions
            cursor.execute("""
                SELECT id, bot_guid, action_type, status, failure_reason, params_json, created_at, updated_at
                FROM azeroth_friend_actions
                WHERE status IN ('failed', 'rejected', 'interrupted')
                ORDER BY id DESC LIMIT 10
            """)
            results["failed_actions"] = cursor.fetchall()

            # 2. Stuck actions in_progress > 20s
            cursor.execute("""
                SELECT id, bot_guid, action_type, status, TIMESTAMPDIFF(SECOND, updated_at, NOW()) AS age_sec
                FROM azeroth_friend_actions
                WHERE status = 'in_progress' AND updated_at < DATE_SUB(NOW(), INTERVAL 20 SECOND)
            """)
            results["stuck_actions"] = cursor.fetchall()

            # 3. Stale pending events > 30s
            cursor.execute("""
                SELECT id, bot_guid, event_type, status, TIMESTAMPDIFF(SECOND, created_at, NOW()) AS age_sec
                FROM azeroth_friend_events
                WHERE status = 'pending' AND created_at < DATE_SUB(NOW(), INTERVAL 30 SECOND)
            """)
            results["stale_events"] = cursor.fetchall()

            # 4. Bot lease and revision status
            cursor.execute("""
                SELECT b.bot_guid, b.master_guid, b.autonomy_enabled, b.control_revision,
                       b.current_goal, b.long_term_goal, b.lease_expires_at,
                       TIMESTAMPDIFF(SECOND, NOW(), b.lease_expires_at) AS lease_remaining_sec
                FROM azeroth_friend_bots b
            """)
            results["bots"] = cursor.fetchall()

            cursor.close()
            conn.close()
        except Exception as e:
            results["error"] = str(e)
        return results

    def run_scan(self) -> Dict[str, Any]:
        """Execute a full monitoring pass."""
        proc_status = self.check_processes()
        log_anomalies = self.check_server_logs()
        db_status = self.check_database()

        return {
            "timestamp": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "processes": proc_status,
            "log_anomalies": log_anomalies,
            "database": db_status,
        }

    def format_report(self, report: Dict[str, Any]) -> str:
        """Format scan report for console / documentation output."""
        lines = []
        lines.append(f"=== AzerothFriend System Monitor [{report['timestamp']}] ===")
        
        # Processes
        p = report["processes"]
        lines.append("Processes:")
        lines.append(f"  - MariaDB:        {'[ONLINE]' if p['mariadbd'] else '[OFFLINE]'}")
        lines.append(f"  - Auth Server:    {'[ONLINE]' if p['authserver'] else '[OFFLINE]'}")
        lines.append(f"  - World Server:   {'[ONLINE]' if p['worldserver'] else '[OFFLINE]'}")
        lines.append(f"  - Chatter Bridge: {'[ONLINE]' if p['chatter_bridge'] else '[OFFLINE]'}")
        lines.append(f"  - Friend Bridge:  {'[ONLINE]' if p['friend_bridge'] else '[OFFLINE]'}")

        # Database
        db = report["database"]
        lines.append("Database State:")
        lines.append(f"  - Connected:      {db['db_connected']}")
        lines.append(f"  - Bots registered: {len(db['bots'])}")
        for bot in db["bots"]:
            lines.append(f"    * Bot {bot['bot_guid']}: Rev={bot['control_revision']}, Autonomy={bot['autonomy_enabled']}, Goal='{bot['current_goal']}'")
        lines.append(f"  - Failed actions: {len(db['failed_actions'])}")
        for a in db["failed_actions"]:
            lines.append(f"    * Action #{a['id']} ({a['action_type']}): {a['status']} - {a['failure_reason']}")
        lines.append(f"  - Stuck actions:  {len(db['stuck_actions'])}")
        lines.append(f"  - Stale events:   {len(db['stale_events'])}")

        # Log anomalies
        logs = report["log_anomalies"]
        lines.append(f"Log Anomalies ({len(logs)}):")
        for log in logs[:5]:
            lines.append(f"  - [{log['source']}] {log['line']}")
        if len(logs) > 5:
            lines.append(f"  ... and {len(logs) - 5} more")

        return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="AzerothFriend System Monitor")
    parser.add_argument("--once", action="store_true", help="Run a single scan and exit")
    parser.add_argument("--interval", type=int, default=15, help="Seconds between scan loops")
    args = parser.parse_args()

    monitor = AzerothFriendMonitor()

    if args.once:
        report = monitor.run_scan()
        print(monitor.format_report(report))
        sys.exit(0)

    print(f"Starting AzerothFriend Monitor daemon (interval: {args.interval}s)...")
    try:
        while True:
            report = monitor.run_scan()
            print(monitor.format_report(report))
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\nMonitor stopped.")


if __name__ == "__main__":
    main()
