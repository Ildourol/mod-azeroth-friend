# AzerothFriend (mod-azeroth-friend)

An autonomous AI companion module and client interface for AzerothCore (WoW 3.3.5a), bridging modern Large Language Models with in-game agency through mod-playerbots.

![AzerothFriend Companion Banner](assets/azeroth_friend_banner.jpg)

---

## Table of Contents

- [Overview](#overview)
- [Architectural Topology](#architectural-topology)
- [Key Features](#key-features)
  - [Dual-Channel Asynchronous Architecture](#dual-channel-asynchronous-architecture)
  - [Strict Thought-Action Coupling](#strict-thought-action-coupling)
  - [4-Tier Cognitive Hierarchy and Action Modes](#4-tier-cognitive-hierarchy-and-action-modes)
  - [Focused Goals and Opt-in Autonomy](#focused-goals-and-opt-in-autonomy)
  - [Natural-Language Spell Resolution](#natural-language-spell-resolution)
  - [RAM-First Live-State Transport](#ram-first-live-state-transport)
  - [Zero-Token Sensory Reuse](#zero-token-sensory-reuse)
  - [Client HUD and Addon](#client-hud-and-addon)
- [Repository Structure](#repository-structure)
- [Prerequisites](#prerequisites)
- [Installation Guide](#installation-guide)
  - [1. C++ Module Compilation](#1-c-module-compilation)
  - [2. Database Migrations](#2-database-migrations)
  - [3. Registering a Companion](#3-registering-a-companion)
  - [4. Configuration Setup](#4-configuration-setup)
  - [5. Python Bridge Installation](#5-python-bridge-installation)
  - [6. In-Game Addon Setup](#6-in-game-addon-setup)
- [Running the System](#running-the-system)
  - [Standalone Bridge](#standalone-bridge)
  - [Dual-Bridge Orchestrator (with mod-llm-chatter)](#dual-bridge-orchestrator-with-mod-llm-chatter)
- [In-Game Commands Reference](#in-game-commands-reference)
  - [General and Inspection Commands](#general-and-inspection-commands)
  - [Goal Management](#goal-management)
  - [Autonomy and Control](#autonomy-and-control)
  - [Action and Spell Dispatch](#action-and-spell-dispatch)
  - [Bot API v1 Inspection](#bot-api-v1-inspection)
- [Addon Interface Guide (AzerothFriendUI)](#addon-interface-guide-azerothfriendui)
- [Configuration Reference](#configuration-reference)
- [Troubleshooting and Diagnostics](#troubleshooting-and-diagnostics)
- [License and Credits](#license-and-credits)

---

## Overview

mod-azeroth-friend transforms standard non-player characters and playerbot bots into intelligent, goal-driven companions in World of Warcraft 3.3.5a. Rather than relying on rigid scripting or hardcoded state machines, the module connects AzerothCore with advanced reasoning engines (such as OpenAI models, Ollama, LM Studio, vLLM, or OpenRouter).

The core philosophy separates mind from body:
- The Python AI Bridge acts as the companion's mind: perceiving spatial surroundings, deliberating on owner commands, maintaining long-term and short-term goals, and formulating structured plans.
- mod-playerbots acts as the companion's physical body: executing verified movements, managing combat rotations, following formations, navigating terrain, and performing trades.

Every thought formulated by the companion directly couples with native playerbot actions, ensuring deterministic physical execution without phantom or disconnected behavior.

---

## Architectural Topology

The system operates across three decoupled layers to protect the game world from network delays and processing bottlenecks:

```
+-------------------------------------------------------------------------+
|                       World of Warcraft 3.3.5a Client                   |
|  +-------------------------------------------------------------------+  |
|  |           AzerothFriendUI Addon (Lua 5.2 / XML Dashboard)         |  |
|  |  [Context] [Mindset / Debug] [Actions] [Thoughts] [Bot API v1]    |  |
|  +-------------------------------------------------------------------+  |
+------------------------------------+------------------------------------+
                                     | Telemetry & Slash Commands
                                     v
+-------------------------------------------------------------------------+
|                         AzerothCore World Server                        |
|                                                                         |
|  [World Loop Thread: 50-100ms]                                          |
|   - Zero blocking I/O, zero external HTTP calls                         |
|   - Numeric GUID tracking and safe object resolution                    |
|   - Action Dispatcher: verifies control revisions and preconditions     |
|   - Bot Controller: interacts directly with mod-playerbots              |
|                                                                         |
|  [Boost.Asio Loopback Worker Thread]                                    |
|   - Non-blocking TCP socket server on 127.0.0.1:8377                    |
|   - Length-prefixed JSON frames (<256 KiB), authenticated by secret     |
|   - RAM snapshot ring buffer for line-of-sight surroundings             |
+-------------------+---------------------------------+-------------------+
                    | Authenticated TCP               | MySQL Queries
                    | (Live State Socket)             | (Events & Actions)
                    v                                 v
+-------------------------------------------------------------------------+
|                  Decoupled Python Bridge (External Mind)                |
|                                                                         |
|  - LiveState Transport Client: consumes fast surroundings snapshots     |
|  - Sensory Ingestion: reuses observations from mod-llm-chatter          |
|  - Context Builder: token-budgeted planning prompts (<2000 tokens)      |
|  - Compound Multi-Action Planner: resolves intents into playerbot steps  |
|  - Spellbook DBC Resolver: maps natural-language names to spell IDs     |
|  - Async LLM Client: OpenAI / Ollama / OpenRouter reasoning engine      |
+-------------------------------------------------------------------------+
```

---

## Key Features

### Dual-Channel Asynchronous Architecture
The AzerothCore world loop runs at 50 to 100 milliseconds per tick. Any blocking operation freezes the game world for all connected players.
- Real-time spatial perception and environmental deltas stream over an authenticated loopback TCP socket (`127.0.0.1:8377`) managed on an asynchronous Boost.Asio worker thread.
- Priority action queues, owner dispatches, and durable summaries flow asynchronously through MySQL database tables (`azeroth_friend_events`, `azeroth_friend_actions`, `azeroth_friend_summaries`).

### Strict Thought-Action Coupling
The companion cannot perform phantom or ungrounded actions. Every internal cognitive thought generated by the AI model maps directly to a native playerbot command, action, or strategy. Thinking and physical execution are tightly bonded:
- "I will attack this wolf!" binds to Playerbot command `attack <guid>` with the `+grind` strategy.
- "Following the master closely through the forest." binds to Playerbot command `follow` (`+follow,-stay`).
- "Resting to recover health and mana." binds to Playerbot action `eat_drink` (`+stay`).
- "Offering gold in trade." binds to Playerbot trade action `trade_set_gold`.

### 4-Tier Cognitive Hierarchy and Action Modes
Companion behavior is structured into four cooperative tiers:
1. Long-Term Goal: The core purpose or campaign ambition (for example, "Protect the master on the road to level 80").
2. Short-Term Goal: The active immediate objective (for example, "Clear Defias bandits from the vineyard").
3. Action Mode: The active situational posture (`combat`, `travel`, `idle`, `social`).
4. Live Surroundings: Visible hostile units, friendly players, lootable corpses, and resource nodes within line-of-sight.

Switching action modes automatically applies the corresponding native playerbot strategies (`+grind` for Combat, `+travel` for Travel, `+stay` for Idle, `+rpg` for Social) with zero token burn.

### Focused Goals and Opt-in Autonomy
- Focused Single Objective: One goal per companion persists across restarts until verified complete or blocked.
- Opt-In Autonomy: Autonomy is disabled by default and fully controlled by the owner. While autonomy is disabled, direct owner commands, spell requests, and party whispers continue to operate normally.
- Control Revision Gates: Every plan step dispatched by the Python bridge carries a control revision number. If an owner issues a new command while an LLM request is in-flight, the server bumps the revision counter and safely discards obsolete plan steps.

### Natural-Language Spell Resolution
Owners can request spells using everyday language (e.g., `.af cast frostbolt`, `.af cast greater heal on master`). The bridge resolves requests against:
- The bot's learned spellbook.
- AzerothCore DBC spell metadata.
- Automatic highest-rank resolution with support for specific rank requests.
- Server-side range, mana, line-of-sight, and cooldown validation.

### RAM-First Live-State Transport
- Live world state travels over the loopback TCP socket directly from memory rather than querying the SQL database on every tick.
- High-efficiency spatial snapshots provide nearby entities, combat posture, bag fullness, and quest states.
- Compact context budgeting keeps input tokens around 2,000 tokens per request, preventing excessive API costs.

### Zero-Token Sensory Reuse
When paired with `mod-llm-chatter`, the companion bridge monitors shared chatter observations (such as lootable corpses, nearby gathering nodes, or player emotes). It converts those existing perception events into physical bot actions with zero additional LLM token usage.

### Client HUD and Addon
A complete World of Warcraft 3.3.5a addon (`AzerothFriendUI`) provides:
- Live Context and Inventory Inspector (`avg_ilvl`, bag slots, consumable counts, quest tracking, master distance).
- Cognitive Mindset Monitor displaying active state, commitment latency, and token savings telemetry.
- Remote Control Bar for instant stance switching, claiming, syncing, and autonomy toggling.
- Bot API v1 Contract Browser displaying typed capabilities, schemas, and verified execution outcomes.
- Minimap docking button with tooltip status.

---

## Repository Structure

```text
mod-azeroth-friend/
├── Addon/
│   └── AzerothFriendUI/          # WoW 3.3.5a client interface (Lua 5.2 / XML)
│       ├── AzerothFriendGoals.lua
│       ├── AzerothFriendProtocol.lua
│       ├── AzerothFriendUI.lua
│       ├── AzerothFriendUI.toc
│       ├── AzerothFriendUI.xml
│       └── README.md
├── assets/
│   └── azeroth_friend_banner.jpg # Module artwork banner
├── conf/
│   └── mod_azeroth_friend.conf.dist # Production configuration template
├── data/
│   └── sql/
│       └── characters/           # Database migrations
│           ├── base/             # Base table definitions
│           └── updates/          # Idempotent incremental migrations
├── src/                          # C++ module source code (AzerothCore hooks)
│   ├── AzerothFriendActionDispatcher.cpp / .h
│   ├── AzerothFriendActionRegistry.cpp / .h
│   ├── AzerothFriendAiControl.cpp / .h
│   ├── AzerothFriendBotController.cpp / .h
│   ├── AzerothFriendChatHook.cpp / .h
│   ├── AzerothFriendCommand.cpp / .h
│   ├── AzerothFriendConfig.cpp / .h
│   ├── AzerothFriendEnvironment.cpp / .h
│   ├── AzerothFriendLiveState.cpp / .h
│   ├── AzerothFriendPlayerbot.h
│   ├── AzerothFriendPlayerbotActions.cpp / .h
│   ├── AzerothFriendScriptLoader.cpp
│   ├── AzerothFriendShared.cpp / .h
│   └── AzerothFriendSnapshotMemory.cpp / .h
├── tools/                        # Decoupled Python cognitive engine
│   ├── azeroth_friend_bridge.py  # Daemon event loop
│   ├── friend_action_catalog.py  # Action schema and parameter normalization
│   ├── friend_chatter_consumer.py# Sensory reuse and chatter ingestion
│   ├── friend_co_processor.py    # Dual-model coordination
│   ├── friend_command_parser.py  # Natural-language command parser
│   ├── friend_constants.py       # Constants and enums
│   ├── friend_context.py         # Compact context generator
│   ├── friend_db.py              # MySQL connector and state queries
│   ├── friend_goals.py           # Goal lifecycle and progress tracking
│   ├── friend_livestate.py       # LiveState TCP transport client
│   ├── friend_llm.py             # OpenAI-compatible API client
│   ├── friend_memory.py          # Episodic memory and affinity
│   ├── friend_mindset.py         # Cognitive commitment and focus
│   ├── friend_monitor.py         # System telemetry monitor
│   ├── friend_planner.py         # Compound plan generator
│   ├── friend_prompts.py         # System prompts and schema formatting
│   ├── friend_spells.py          # Spellbook resolver
│   ├── friend_summaries.py       # Zero-token verified summaries
│   ├── friend_thinking.py        # Thinking/reasoning model policy
│   ├── requirements.txt          # Python dependencies
│   ├── spell_dbc.py              # AzerothCore DBC spell parser
│   └── data/
│       └── spell_catalog.json    # Cached spell metadata
├── include.sh                    # Shell loader
├── mod-azeroth-friend.cmake      # CMake module definition
├── start_bridge.bat              # Standalone bridge launcher
├── start_all_bridges.bat          # Dual-bridge orchestrator
└── README.md                     # Project documentation
```

---

## Prerequisites

Before setting up mod-azeroth-friend, ensure your environment meets the following requirements:

1. AzerothCore WotLK (branch 3.3.5a) compiled with `mod-playerbots` installed and functioning.
2. MySQL 5.7+ or MariaDB 10.3+ hosting the `acore_characters` database.
3. Python 3.8 or newer installed on the machine running the AI bridge.
4. An LLM Provider supporting OpenAI-compatible chat completions:
   - Cloud providers: OpenAI (gpt-4o, gpt-4o-mini, o3-mini), OpenRouter, Anthropic (via proxy).
   - Local engines: Ollama, vLLM, LM Studio, LocalAI.
5. World of Warcraft 3.3.5a client (Build 12340) for the companion HUD addon.

---

## Installation Guide

### 1. C++ Module Compilation

1. Clone or place this repository into your AzerothCore `modules` directory:
   ```bash
   cd azerothcore/modules
   git clone https://github.com/Ildourol/mod-azeroth-friend.git
   ```

2. Re-run CMake generation from your AzerothCore build directory:
   ```bash
   cd azerothcore/build
   cmake ../ -DCMAKE_INSTALL_PREFIX=/path/to/server
   ```

3. Build the server or module target:
   ```bash
   # Linux
   make -j$(nproc)
   make install

   # Windows (Visual Studio)
   cmake --build . --config Release --target ALL_BUILD -j 4
   ```

### 2. Database Migrations

If `Updates.EnableDatabases` in your `worldserver.conf` includes characters (default `7`), migrations apply automatically on worldserver boot.

To apply migrations manually against `acore_characters`:
```bash
mysql -u acore -p acore_characters < data/sql/characters/base/00000000_azeroth_friend_tables.sql
mysql -u acore -p acore_characters < data/sql/characters/updates/2026_09_19_azeroth_friend_action_executor.sql
mysql -u acore -p acore_characters < data/sql/characters/updates/2026_09_19_azeroth_friend_event_status_and_commands.sql
mysql -u acore -p acore_characters < data/sql/characters/updates/2026_09_20_azeroth_friend_claim_bridge_gate.sql
mysql -u acore -p acore_characters < data/sql/characters/updates/2026_09_20_azeroth_friend_goals.sql
mysql -u acore -p acore_characters < data/sql/characters/updates/2026_09_20_azeroth_friend_long_term_goal.sql
mysql -u acore -p acore_characters < data/sql/characters/updates/2026_09_20_azeroth_friend_ram_first_context.sql
mysql -u acore -p acore_characters < data/sql/characters/updates/2026_09_20_azeroth_friend_telemetry.sql
mysql -u acore -p acore_characters < data/sql/characters/updates/2026_09_21_azeroth_friend_action_modes.sql
mysql -u acore -p acore_characters < data/sql/characters/updates/2026_09_21_azeroth_friend_bot_api_v1.sql
```

All migrations are fully idempotent. Running them multiple times will never overwrite existing companion goals, inventory records, or personality configurations.

### 3. Registering a Companion

Register any existing playerbot character in the `azeroth_friend_bots` table:

```sql
INSERT INTO azeroth_friend_bots (
    bot_guid,
    bot_name,
    mode,
    personality,
    long_term_goal,
    current_goal,
    enabled
) VALUES (
    12345,                                    -- Character GUID of the playerbot
    'Friendbot',                              -- Character Name
    'companion',                              -- Operating mode
    'A steadfast dwarven warrior who values honor and a good pint of ale.',
    'Help my companion conquer the dungeons of Azeroth.',
    'Clear the gnolls threatening the eastern border.',
    1
) ON DUPLICATE KEY UPDATE enabled = 1;
```

### 4. Configuration Setup

Copy the distribution configuration file to your worldserver module configuration directory:

```bash
cp conf/mod_azeroth_friend.conf.dist /path/to/server/etc/modules/mod_azeroth_friend.conf
```

Edit `mod_azeroth_friend.conf` and adjust key settings:

- Database Credentials:
  ```ini
  AzerothFriend.Database.Host = "127.0.0.1"
  AzerothFriend.Database.Port = 3306
  AzerothFriend.Database.User = "acore"
  AzerothFriend.Database.Password = "acore"
  AzerothFriend.Database.CharactersDB = "acore_characters"
  AzerothFriend.Database.WorldDB = "acore_world"
  ```

- Managed Bots Allowlist:
  ```ini
  AzerothFriend.ControlledBots = "Friendbot"
  AzerothFriend.MaxControlledBots = 1
  ```

- Live-State Transport Secret (must match in bridge and C++ module):
  ```ini
  AzerothFriend.LiveState.Enable = 1
  AzerothFriend.LiveState.Host = "127.0.0.1"
  AzerothFriend.LiveState.Port = 8377
  AzerothFriend.LiveState.Secret = "your-secure-random-secret-key"
  ```

- LLM Provider Configuration:
  ```ini
  AzerothFriend.LLM.Provider = "openai"
  AzerothFriend.LLM.BaseUrl = "https://api.openai.com/v1"
  AzerothFriend.LLM.ApiKey = "sk-..."
  AzerothFriend.LLM.Model = "gpt-4o-mini"
  AzerothFriend.LLM.MaxTokens = 800
  AzerothFriend.LLM.Temperature = 0.7
  ```

### 5. Python Bridge Installation

1. Open a terminal in the `tools` directory:
   ```bash
   cd tools
   ```

2. Install Python dependencies:
   ```bash
   pip install --upgrade -r requirements.txt
   ```

3. Run the preflight healthcheck to verify database connectivity, configuration parsing, and LLM endpoint accessibility:
   ```bash
   python azeroth_friend_bridge.py --config ../conf/mod_azeroth_friend.conf --healthcheck
   ```

### 6. In-Game Addon Setup

1. Copy the `Addon/AzerothFriendUI` folder into your World of Warcraft client directory:
   ```text
   <WoW 3.3.5 Directory>/Interface/AddOns/AzerothFriendUI/
   ```

2. Verify the contents of `AzerothFriendUI`:
   - `AzerothFriendUI.toc`
   - `AzerothFriendUI.lua`
   - `AzerothFriendUI.xml`
   - `AzerothFriendGoals.lua`
   - `AzerothFriendProtocol.lua`

3. Start World of Warcraft, log into character select, click the **AddOns** button in the lower-left corner, and ensure **AzerothFriend UI** is checked and set to load out-of-date addons if prompted.

---

## Running the System

### Standalone Bridge

To start the standalone cognitive bridge:
```bash
# On Windows
start_bridge.bat

# On Linux / macOS
cd tools
python azeroth_friend_bridge.py --config /path/to/server/etc/modules/mod_azeroth_friend.conf
```

### Dual-Bridge Orchestrator (with mod-llm-chatter)

When running both `mod-llm-chatter` (for dialogue and banter) and `mod-azeroth-friend` (for planning and agency), launch the dual-bridge runner:
```bash
start_all_bridges.bat
```

This launches both bridges into isolated console processes. The companion bridge automatically listens to sensory observations published by `mod-llm-chatter`, saving thousands of LLM tokens each session.

---

## In-Game Commands Reference

Commands can be invoked in-game or from the server console using `.af`, `/af`, `.friend`, or `/friend`.

### General and Inspection Commands

| Command | Description |
|---|---|
| `.af status` | Displays bridge connection health, RAM vs SQL mode, and token savings telemetry. |
| `.af bots` | Lists all registered companion bots, their online status, mode, and claim state. |
| `.af sync` | Requests an immediate zero-token server context refresh to the client addon. |
| `.af diag [bot]` | Outputs diagnostic information regarding leases, playerbot AI state, and last action results. |
| `.af inspect [bot]` | Broadcasts a live context snapshot packet directly to the owner addon. |
| `.af reset` | Prints instructions to reset addon window frames to default screen coordinates. |
| `.af debug [on\|off]` | Toggles verbose diagnostic logging in the server console and world log. |

### Goal Management

| Command | Description |
|---|---|
| `.af goal [bot] set <text>` | Defines a new short-term goal for the companion (automatically pauses autonomy). |
| `.af goal [bot] show` | Displays the companion's current active goal, status, and progress. |
| `.af goal [bot] pause` | Temporarily suspends the active goal and disables autonomy. |
| `.af goal [bot] resume` | Resumes a paused goal (requires an existing goal). |
| `.af goal [bot] complete` | Marks the current goal as successfully completed and resets autonomy. |
| `.af goal [bot] clear` | Cancels the active goal, removes goal data, and disables autonomy. |

### Autonomy and Control

| Command | Description |
|---|---|
| `.af autonomy [bot] on` | Enables autonomous cognitive operation (requires an active focused goal). |
| `.af autonomy [bot] off` | Disables autonomous operation; companion obeys direct orders only. |
| `.af autonomy [bot] status` | Shows current autonomy state, tick rate, and execution revision. |
| `.af mode [bot] <combat\|travel\|idle\|social>` | Sets the tactical action mode and applies corresponding playerbot strategies. |
| `.af claim [bot]` | Claims exclusive ownership lease on the bot, suspending ambient wander. |
| `.af release [bot]` | Releases exclusive lease back to normal playerbot behaviors. |

### Action and Spell Dispatch

| Command | Description |
|---|---|
| `.af cast <spell> [target]` | Casts a spell using natural language name resolution against the bot's learned spellbook. |
| `.af action <name>` | Immediately dispatches a curated playerbot action (e.g., `follow`, `stay`, `loot`, `eat_drink`). |
| `.af run <bot> <action> [json]` | Manually executes a typed Bot API action with optional JSON arguments (requires GM or owner authority). |

### Bot API v1 Inspection

| Command | Description |
|---|---|
| `.af catalog [filter] [bot]` | Lists indexed Bot API capabilities and exported playerbot actions. |
| `.af catalog describe <capability>` | Displays full parameter schema, preconditions, authority level, and completion policy. |
| `.af events [limit]` | Inspects recently queued inbound companion events and their processing state. |
| `.af actions [limit]` | Displays the outbound action queue and execution status. |

---

## Addon Interface Guide (AzerothFriendUI)

The client addon provides a 5-tab master HUD accessible via `/af` or by clicking the minimap icon.

```
+-------------------------------------------------------------------------+
| [ AzerothFriend UI ]                       [Status: NORMAL] [Tokens: 0] |
+-------------------------------------------------------------------------+
| [Tab 1: Context] [Tab 2: Mindset] [Tab 3: Actions] [Tab 4: Thoughts] ... |
+-------------------------------------------------------------------------+
| Master Distance: 4.2 yds   | Target: Defias Cutpurse                    |
| Gear ilvl: 18.4            | Free Bags: 12 / 16                         |
| Water: 8 | Food: 14        | Potions: 2                                 |
| Zone: Elwynn Forest        | SubZone: Northshire Valley [OUTDOORS]      |
|                                                                         |
| Active Goal: Clear Defias bandits from the vineyard                     |
| Progress: 3 / 8 bandits defeated                                        |
+-------------------------------------------------------------------------+
| [Control Bar]                                                           |
| [Claim / Release] [Combat] [Travel] [Idle] [Social] [Sync] [Autonomy]   |
+-------------------------------------------------------------------------+
```

### Tab Breakdown

1. Context Inspector (`/af context`):
   Real-time view of companion gear average item level, free bag slots, consumable stockpiles (food, water, potions), active quest progression, zone resting status, and distance to master. Uses zero LLM tokens.

2. Cognitive Mindset & Telemetry (`/af debug`):
   Displays active mindset state (`RESTING`, `LOOTING`, `FOLLOWING`, `COMBAT`, `EXPLORING`, `SOCIAL`, `IDLE`), commitment latency countdown (preventing ADHD task-switching), and cumulative LLM token savings.

3. Action Execution Pipeline (`/af actions`):
   Inspects recently executed actions, step revisions, dispatch acknowledgements, and verified world-state outcomes.

4. Thought and Reasoning Stream (`/af thoughts`):
   Live feed of companion internal monologue, reasoning traces, and spoken party responses with anti-repetition filtering.

5. Bot API v1 Contract Browser (`/af api`):
   Interactive catalog of all capabilities supported by the companion, including argument schemas, execution preconditions, authority tiers, and Shift-click manual execution.

### Addon Slash Commands

- `/af` - Toggle Unified Master HUD.
- `/af context` - Jump to Context Inspector.
- `/af debug` - Jump to Mindset & Debug Monitor.
- `/af thoughts` - Open Thought & Speech log.
- `/af api` - Open Bot API v1 Catalog.
- `/af goals` - Open Focused Goal manager.
- `/af minimap` - Toggle minimap docking icon.
- `/af layout` - Switch between unified tabbed HUD and detached floating panels.
- `/af reset` - Reset window coordinates to screen center.

---

## Configuration Reference

Key configuration parameters in `mod_azeroth_friend.conf`:

| Parameter | Type | Default | Description |
|---|---|---|---|
| `AzerothFriend.Enable` | int | `1` | Enables or disables the C++ module. |
| `AzerothFriend.Debug` | int | `0` | Enables verbose server console debugging. |
| `AzerothFriend.ControlledBots` | string | `""` | Comma-separated list of allowed bot names. Empty manages all registered bots. |
| `AzerothFriend.MaxControlledBots` | int | `1` | Maximum number of concurrent bots managed by the module. |
| `AzerothFriend.TickIntervalMs` | int | `500` | Frequency in milliseconds for polling and dispatching pending actions. |
| `AzerothFriend.Environment.ScanRadius` | float | `30.0` | Radius in yards for line-of-sight surroundings scanning. |
| `AzerothFriend.Environment.MaxVisibleEntities` | int | `20` | Maximum visible entities included in environment snapshots. |
| `AzerothFriend.Environment.DeltaDistance` | float | `5.0` | Distance moved before triggering an updated surroundings snapshot. |
| `AzerothFriend.Environment.StorageMode` | int | `1` | `1` = RAM ring buffer (fastest), `2` = SQL table storage. |
| `AzerothFriend.LiveState.Enable` | int | `1` | Enables loopback TCP socket live-state transport. |
| `AzerothFriend.LiveState.Port` | int | `8377` | Loopback TCP socket port. |
| `AzerothFriend.LiveState.Secret` | string | `""` | Authentication secret shared between server and Python bridge. |
| `AzerothFriend.Autonomy.Enable` | int | `1` | Enables the autonomous decision-making loop. |
| `AzerothFriend.Autonomy.TickSeconds` | int | `5` | Cadence in seconds between autonomous planning iterations. |
| `AzerothFriend.MultiActionPlan.Enable` | int | `1` | Enables compound multi-step action planning per LLM call. |
| `AzerothFriend.MultiActionPlan.MaxSteps` | int | `5` | Maximum steps allowed in a single compound plan. |
| `AzerothFriend.SpellSync.Enable` | int | `1` | Enables spellbook and DBC indexing for natural-language spellcasting. |
| `AzerothFriend.Chat.DelegateAmbientToLLMChatter` | int | `1` | Delegates ambient chat generation to `mod-llm-chatter` to conserve tokens. |
| `AzerothFriend.Context.Compact.TargetInputTokens`| int | `2000` | Target input token budget per planning prompt. |
| `AzerothFriend.Context.Compact.MaxInputTokens` | int | `3500` | Hard ceiling for planning prompt token size. |

---

## Troubleshooting and Diagnostics

### Common Symptoms and Solutions

- Bridge Reports Offline in `.af status`:
  - Verify `azeroth_friend_bridge.py` is actively running.
  - Verify MySQL credentials and database names in `mod_azeroth_friend.conf`.
  - Check whether MySQL is accessible from the bridge host on port 3306.

- Actions Queue but the Bot Does Not Move:
  - Run `.af diag <bot>` in-game to inspect lease state and playerbot AI state.
  - Ensure the bot is logged into the world and allowlisted in `AzerothFriend.ControlledBots`.
  - Confirm the companion is not dead, stunned, or incapacitated.

- Spell Is Unknown or Not Learned:
  - The companion can only cast spells present in its learned spellbook.
  - Use `.af catalog` or inspect the bot's spellbook to check learned spell names.
  - Use explicit spell rank if multiple spells share ambiguous naming (e.g., `.af cast Healing Wave (Rank 2)`).

- Addon HUD Shows Transport Offline:
  - Ensure `AzerothFriend.LiveState.Secret` in `mod_azeroth_friend.conf` matches between the server and Python bridge.
  - Check that port 8377 is not blocked by a local firewall.

- Companion Swaps Targets Too Rapidly:
  - The cognitive commitment window (`MindsetManager`) prevents rapid task-switching. Check the Mindset tab in `/af debug` to verify commitment latency.

- Addon Windows Positioned Off-Screen:
  - Type `/af reset` in chat to restore all HUD frames to the center of your screen.

---

## License and Credits

- **Module License**: Licensed under the GNU General Public License v2 (GPLv2), compatible with AzerothCore.
- **AzerothCore**: Open-source MMORPG framework for World of Warcraft 3.3.5a.
- **mod-playerbots**: Playerbot AI execution framework for AzerothCore.
- **mod-llm-chatter**: Optional ambient conversation and dialogue companion module.
