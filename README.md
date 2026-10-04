# AzerothFriend (mod-azeroth-friend)

An autonomous AI companion module, Python reasoning bridge, and WoW 3.3.5a client HUD for AzerothCore + mod-playerbots.

![AzerothFriend Companion Banner](assets/azeroth_friend_banner.jpg)

---

## Table of Contents

- [Overview](#overview)
- [Architectural Topology](#architectural-topology)
- [Key Features](#key-features)
  - [Dual-Channel Asynchronous Architecture](#dual-channel-asynchronous-architecture)
  - [Strict Thought-Action Coupling](#strict-thought-action-coupling)
  - [4-Tier Cognitive Hierarchy and Action Modes](#4-tier-cognitive-hierarchy-and-action-modes)
  - [Focused Goals, AI Suggestions, and Opt-in Autonomy](#focused-goals-ai-suggestions-and-opt-in-autonomy)
  - [Natural-Language Spell Resolution](#natural-language-spell-resolution)
  - [RAM-First Live-State Transport](#ram-first-live-state-transport)
  - [Offline World Catalogs and Grounded Planning](#offline-world-catalogs-and-grounded-planning)
  - [JSON-RPC Gateway and CLI Control](#json-rpc-gateway-and-cli-control)
  - [Modular Bridge Plugins](#modular-bridge-plugins)
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
  - [Bridge CLI and JSON-RPC](#bridge-cli-and-json-rpc)
  - [Dual-Bridge Orchestrator with mod-llm-chatter](#dual-bridge-orchestrator-with-mod-llm-chatter)
- [In-Game Commands Reference](#in-game-commands-reference)
  - [General and Inspection Commands](#general-and-inspection-commands)
  - [Goal Management and Suggestions](#goal-management-and-suggestions)
  - [Autonomy and Control](#autonomy-and-control)
  - [Action and Spell Dispatch](#action-and-spell-dispatch)
  - [Bot API v1 Inspection](#bot-api-v1-inspection)
- [Addon Interface Guide (AzerothFriendUI)](#addon-interface-guide-azerothfriendui)
- [Configuration Reference](#configuration-reference)
- [Troubleshooting and Diagnostics](#troubleshooting-and-diagnostics)
- [License and Credits](#license-and-credits)

---

## Overview

AzerothFriend turns a mod-playerbots character into a persistent, goal-driven AI companion. It does not replace PlayerbotAI combat, movement, or game-rule enforcement. Instead, it adds a higher-level cognitive layer that can understand owner intent, maintain goals, inspect live world state, choose grounded capabilities, and dispatch verified actions through the server.

The runtime is intentionally split into three parts:

- **AzerothCore C++ module** — owns world-facing authority, bot claiming, action validation, Bot API v1 capability contracts, LiveState publication, telemetry, and final execution.
- **Python bridge** — owns LLM calls, compact context building, goal planning, suggestions, memories, summaries, offline catalogs, JSON-RPC administration, and optional plugins.
- **AzerothFriendUI addon** — presents context, goals, thoughts, action progress, API contracts, and controls inside the WoW 3.3.5a client.

The supported server stack is the **mod-playerbots AzerothCore fork on the Playerbot branch together with mod-playerbots master**. Standard upstream AzerothCore alone is not the target runtime.

The design follows one rule: the model may reason freely, but the server remains authoritative about what the companion can actually do.

---

## Architectural Topology

The current architecture separates real-time game state from durable coordination and keeps external model latency away from the world loop:

~~~
+-------------------------------------------------------------------------+
|                       World of Warcraft 3.3.5a Client                   |
|                                                                         |
|  AzerothFriendUI                                                        |
|  Context | Mindset | Actions | Thoughts | Bot API v1                   |
+------------------------------------+------------------------------------+
                                     |
                                     | addon/system telemetry + commands
                                     v
+-------------------------------------------------------------------------+
|                         AzerothCore World Server                        |
|                                                                         |
|  mod-azeroth-friend                                                     |
|   - companion claiming / ownership gates                                |
|   - Bot API v1 capability registry                                      |
|   - action validation + dispatch                                        |
|   - playerbot bindings                                                  |
|   - environment / self-context telemetry                                |
|   - LiveState TCP publisher                                             |
|                                                                         |
|  mod-playerbots                                                         |
|   - combat reflexes / rotations                                         |
|   - movement, travel, interaction, trade, questing                      |
|   - actual bot body and game execution                                  |
+----------------------+--------------------------+-----------------------+
                       |                          |
                       | TCP LiveState :8377      | MySQL durable state
                       v                          v
+-------------------------------------------------------------------------+
|                         Python Companion Bridge                         |
|                                                                         |
|  planning + goals + memories + summaries                                |
|  grounded action catalog + spell/world catalogs                         |
|  OpenAI-compatible LLM client + thinking policy                         |
|  mod-llm-chatter sensory reuse                                          |
|  plugin engine                                                          |
|  JSON-RPC 2.0 gateway :8378                                             |
+----------------------+--------------------------------------------------+
                       |
                       | loopback JSON-RPC
                       v
                 tools/af_ctl.py
~~~

Two channels are deliberately used:

1. **LiveState TCP** on <code>127.0.0.1:8377</code> for fresh RAM-first world snapshots and bridge diagnostics.
2. **MySQL** for durable events, goals, plans, action results, summaries, memories, and restart-safe state.

The JSON-RPC gateway on <code>127.0.0.1:8378</code> is a separate operator interface exposed by the Python bridge. It is not the game-state transport.

---

## Key Features

### Dual-Channel Asynchronous Architecture

The worldserver must never wait for an LLM or remote HTTP request.

- The C++ side publishes compact LiveState snapshots from a background transport path.
- The Python bridge performs model calls outside the game server.
- Durable events and action plans use MySQL.
- The server executes only validated actions and reports dispatch/completion state back to the bridge and addon.
- When LiveState is disabled, the bridge can use the SQL compatibility path.

This keeps slow model calls isolated from the AzerothCore world loop.

### Strict Thought-Action Coupling

AzerothFriend does not treat generated prose as execution. Plans are normalized against a grounded action catalog and then validated again by the server.

Examples of grounded behavior include:

- follow / stay / flee through playerbot strategy and movement bindings;
- attack and combat-mode changes through server-side action dispatch;
- spell requests resolved against the companion's learned spellbook;
- quest, loot, trade, vendor, trainer, and interaction operations through registered capabilities;
- manual Bot API v1 calls through <code>.af run</code>.

The addon distinguishes planning, dispatch acknowledgement, and verified completion so a thought is not presented as a successful world action unless the runtime confirms it.

### 4-Tier Cognitive Hierarchy and Action Modes

The companion operates with four layers of context:

1. **Long-term goal** — the broader purpose or campaign direction.
2. **Short-term goal** — the active focused objective.
3. **Action mode** — <code>combat</code>, <code>travel</code>, <code>idle</code>, or <code>social</code>.
4. **Live surroundings and self-context** — nearby units/objects, combat state, master state, bags, gear, consumables, quests, and other current facts.

The LLM chooses high-level intent while mod-playerbots remains responsible for fast combat reflexes and physical execution.

### Focused Goals, AI Suggestions, and Opt-in Autonomy

Autonomy is owner-controlled.

- Autonomy can remain off while direct commands, spell requests, claims, catalog inspection, and manual actions continue to work.
- Enabling autonomy requires a usable short- or long-term goal.
- The bridge can generate a goal pair for a newly registered companion with empty goals when <code>AzerothFriend.Goal.AutoGenerate.Enable</code> is enabled.
- Owners can request fresh AI suggestions for short-term goals, long-term goals, or tactical actions.
- Suggested actions are grounded against the current world and action catalog instead of being free-form fantasy text.
- Owner changes bump control state and interrupt obsolete plans so old model responses do not continue after the player has changed direction.
- Claiming a companion and autonomous bridge control are mutually exclusive by design.

Recent versions also preserve ordered multi-action execution so compound plans stay in sequence instead of racing independent steps.

### Natural-Language Spell Resolution

Owners can ask for spells by name rather than numeric IDs.

The bridge combines:

- the bot's learned spellbook;
- client DBC metadata;
- the cached spell catalog under <code>tools/data/</code>;
- rank-aware matching;
- server-side legality checks such as target, range, cooldown, mana, and line of sight.

The model can suggest a spell, but the runtime decides whether the companion actually knows and can cast it.

### RAM-First Live-State Transport

LiveState is the fast path for companion perception.

- Default listener: <code>127.0.0.1:8377</code>.
- The shared secret is configured with <code>AzerothFriend.LiveState.Secret</code>.
- The listener refuses to start with an empty secret.
- Snapshots are cached in RAM and exposed to the planner without requiring a database query for every perception cycle.
- The bridge reports cache/context diagnostics back to the server for the addon.
- SQL remains available as a compatibility and durable-state layer.

### Offline World Catalogs and Grounded Planning

The bridge now ships static 3.3.5a catalogs:

- <code>tools/data/spell_catalog.json</code>
- <code>tools/data/world_catalog.json</code>

The world catalog resolves maps, zones, subzones, and deterministic spatial calculations without an LLM call or live database lookup. <code>tools/generate_catalogs.py</code> can rebuild the map/area catalog from <code>Map.dbc</code> and <code>AreaTable.dbc</code>.

This supports lower-token, more grounded prompts and operator queries such as zone search through the RPC CLI.

### JSON-RPC Gateway and CLI Control

The Python bridge exposes a loopback JSON-RPC 2.0 endpoint when <code>AzerothFriend.Rpc.Enable = 1</code>.

Default endpoint:

~~~
http://127.0.0.1:8378/rpc
~~~

Built-in methods cover:

- session status and health;
- clean bridge shutdown;
- companion listing and status;
- manual action invocation;
- goal changes;
- mode changes;
- autonomy toggling;
- world-zone search and catalog statistics.

The included <code>tools/af_ctl.py</code> client wraps these methods for shell use.

### Modular Bridge Plugins

The Python bridge can load extensions from <code>tools/plugins/</code> when <code>AzerothFriend.Plugins.Enable = 1</code>.

Each plugin uses a <code>plugin.json</code> manifest and Python entrypoint. Plugins can:

- receive bridge events;
- use shared bridge services passed at load time;
- perform shutdown cleanup;
- publish custom JSON-RPC methods.

A working example lives under <code>tools/plugins/sample_tactics/</code>.

### Zero-Token Sensory Reuse

When paired with <code>mod-llm-chatter</code>, AzerothFriend can reuse already-observed world events such as loot, nearby resources, or dialogue context.

This reduces duplicate model calls and lets one perception event drive both conversation and physical behavior where appropriate. Token-sharing and sensory-reuse counters are surfaced in bridge telemetry and the addon.

### Client HUD and Addon

<code>AzerothFriendUI</code> is the in-game control and debugging surface.

The master window provides five tabs:

1. **Context** — self-context, inventory, quests, master distance, and surroundings.
2. **Mindset / Debug** — cognition state, claim status, transport health, thinking policy, and token telemetry.
3. **Actions** — live plan/action state and direct control.
4. **Thoughts** — reasoning, tactical/action timeline entries, speech, and memory output.
5. **Bot API v1** — capability catalog, contracts, authority, bindings, and verified results.

Recent UI work also adds grounded action suggestions and stronger thinking-lock/debounce behavior so background telemetry does not prematurely clear an active thinking state.

---

## Repository Structure

~~~text
mod-azeroth-friend/
├── Addon/
│   └── AzerothFriendUI/
│       ├── AzerothFriendGoals.lua
│       ├── AzerothFriendProtocol.lua
│       ├── AzerothFriendUI.lua
│       ├── AzerothFriendUI.toc
│       ├── AzerothFriendUI.xml
│       └── README.md
├── assets/
│   └── azeroth_friend_banner.jpg
├── conf/
│   └── mod_azeroth_friend.conf.dist
├── data/
│   └── sql/
│       └── characters/
│           ├── base/
│           └── updates/
├── src/
│   ├── AzerothFriendActionDispatcher.*
│   ├── AzerothFriendActionRegistry.*
│   ├── AzerothFriendAiControl.*
│   ├── AzerothFriendBotController.*
│   ├── AzerothFriendChatHook.*
│   ├── AzerothFriendCommand.*
│   ├── AzerothFriendConfig.*
│   ├── AzerothFriendEnvironment.*
│   ├── AzerothFriendLiveState.*
│   ├── AzerothFriendPlayerbot.h
│   ├── AzerothFriendPlayerbotActions.*
│   ├── AzerothFriendScriptLoader.cpp
│   ├── AzerothFriendShared.*
│   └── AzerothFriendSnapshotMemory.*
├── tools/
│   ├── af_ctl.py
│   ├── azeroth_friend_bridge.py
│   ├── friend_action_catalog.py
│   ├── friend_catalogs.py
│   ├── friend_chatter_consumer.py
│   ├── friend_co_processor.py
│   ├── friend_command_parser.py
│   ├── friend_constants.py
│   ├── friend_context.py
│   ├── friend_db.py
│   ├── friend_goals.py
│   ├── friend_livestate.py
│   ├── friend_llm.py
│   ├── friend_memory.py
│   ├── friend_mindset.py
│   ├── friend_monitor.py
│   ├── friend_planner.py
│   ├── friend_plugins.py
│   ├── friend_prompts.py
│   ├── friend_rpc.py
│   ├── friend_spells.py
│   ├── friend_summaries.py
│   ├── friend_thinking.py
│   ├── generate_catalogs.py
│   ├── requirements.txt
│   ├── spell_dbc.py
│   ├── data/
│   │   ├── spell_catalog.json
│   │   └── world_catalog.json
│   └── plugins/
│       └── sample_tactics/
│           ├── plugin.json
│           └── plugin.py
├── include.sh
├── mod-azeroth-friend.cmake
├── start_bridge.bat
├── start_all_bridges.bat
└── README.md
~~~

---

## Prerequisites

Before installing AzerothFriend, have the following working first:

1. **Core fork:** <code>mod-playerbots/azerothcore-wotlk</code>, branch <code>Playerbot</code>.
2. **Playerbots module:** <code>mod-playerbots/mod-playerbots</code>, branch <code>master</code>, installed under the core <code>modules/</code> tree.
3. MySQL/MariaDB with the normal AzerothCore databases and Playerbots schema available.
4. Python 3 with the packages from <code>tools/requirements.txt</code>.
5. A supported model endpoint. The bridge is designed around OpenAI-compatible chat-completions style providers and also contains Anthropic support.
6. WoW 3.3.5a build 12340 if using the addon.
7. Client DBC data if you want to rebuild offline spell/world catalogs.

Do not install this module against plain upstream AzerothCore and assume Playerbots hooks will be equivalent.

---

## Installation Guide

### 1. C++ Module Compilation

Clone the required Playerbots core and modules:

~~~bash
git clone https://github.com/mod-playerbots/azerothcore-wotlk.git --branch=Playerbot
cd azerothcore-wotlk/modules
git clone https://github.com/mod-playerbots/mod-playerbots.git --branch=master
git clone https://github.com/Ildourol/mod-azeroth-friend.git
~~~

Then compile using your normal Playerbots/AzerothCore workflow. For the current Playerbot fork, the helper workflow is typically:

~~~bash
cd ~/azerothcore-wotlk
./acore.sh install-deps
./acore.sh compiler all
~~~

After installation, verify that <code>authserver</code> and <code>worldserver</code> exist in the configured distribution <code>bin</code> directory.

AzerothFriend is a compiled C++ module. C++ changes require rebuilding worldserver.

### 2. Database Migrations

The repository contains its character-database schema under:

~~~text
data/sql/characters/base/
data/sql/characters/updates/
~~~

Use the AzerothCore/module database updater where available. On an existing installation, make sure the current AzerothFriend schema and updates have been applied before starting the Python bridge; the bridge performs schema checks at startup and will stop when required context tables/columns are missing.

For a manual recovery/import, apply the base schema first and then the update files in chronological order to <code>acore_characters</code>. Do not repeatedly hand-edit the schema to match Python errors; use the repository SQL files as the source of truth.

### 3. Registering a Companion

The bridge can automatically register names from <code>AzerothFriend.ControlledBots</code>. You can also register or inspect companions directly in <code>azeroth_friend_bots</code>.

A minimal manual example:

~~~sql
INSERT INTO azeroth_friend_bots (
    bot_guid,
    bot_name,
    mode,
    personality,
    enabled
) VALUES (
    12345,
    'Friendbot',
    'companion',
    'A steadfast dwarven warrior who values honor and a good pint of ale.',
    1
)
ON DUPLICATE KEY UPDATE enabled = 1;
~~~

Goal fields may be left empty if you want the bridge's one-time goal auto-generation to initialize them.

### 4. Configuration Setup

The distributed template is:

~~~text
conf/mod_azeroth_friend.conf.dist
~~~

When the module is installed, use the generated module config in the server's <code>etc/modules/</code> directory. If it has not been created automatically, copy the distribution file and remove the <code>.dist</code> suffix.

At minimum, review:

~~~ini
AzerothFriend.Enable = 1

AzerothFriend.ControlledBots = "Friendbot"
AzerothFriend.MaxControlledBots = 1

AzerothFriend.Database.Host = "127.0.0.1"
AzerothFriend.Database.Port = 3306
AzerothFriend.Database.User = "acore"
AzerothFriend.Database.Password = "acore"
AzerothFriend.Database.CharactersDB = "acore_characters"
AzerothFriend.Database.WorldDB = "acore_world"

AzerothFriend.LiveState.Enable = 1
AzerothFriend.LiveState.Host = "127.0.0.1"
AzerothFriend.LiveState.Port = 8377
AzerothFriend.LiveState.Secret = "REPLACE_WITH_A_RANDOM_SECRET"

AzerothFriend.Rpc.Enable = 1
AzerothFriend.Rpc.Host = "127.0.0.1"
AzerothFriend.Rpc.Port = 8378

AzerothFriend.Plugins.Enable = 1
~~~

Generate a LiveState secret once, for example:

~~~bash
openssl rand -hex 32
~~~

Keep both LiveState and RPC bound to loopback unless you are intentionally adding a separate authenticated network layer. The built-in RPC server is designed as a local operator interface.

Then configure your model provider:

~~~ini
AzerothFriend.LLM.Provider = "openai"
AzerothFriend.LLM.BaseUrl = "https://api.openai.com/v1"
AzerothFriend.LLM.ApiKey = "YOUR_API_KEY_HERE"
AzerothFriend.LLM.Model = "gpt-4o-mini"
AzerothFriend.LLM.MaxTokens = 800
AzerothFriend.LLM.Temperature = 0.7

AzerothFriend.LLM.Thinking.Mode = "Auto"
AzerothFriend.LLM.Thinking.AutoDetect = 1
AzerothFriend.LLM.Thinking.ModelType = "Auto"
AzerothFriend.LLM.Thinking.Effort = "low"
~~~

### 5. Python Bridge Installation

Install the bridge dependencies:

~~~bash
cd ~/azerothcore-wotlk/modules/mod-azeroth-friend/tools
python3 -m pip install --upgrade -r requirements.txt
~~~

Run a preflight health check before starting the long-running bridge:

~~~bash
python3 azeroth_friend_bridge.py   --config /path/to/server/etc/modules/mod_azeroth_friend.conf   --healthcheck
~~~

Then start the bridge normally:

~~~bash
python3 azeroth_friend_bridge.py   --config /path/to/server/etc/modules/mod_azeroth_friend.conf
~~~

On Windows, <code>start_bridge.bat</code> is provided as a convenience launcher. <code>start_all_bridges.bat</code> starts the combined AzerothFriend + mod-llm-chatter workflow.

### 6. In-Game Addon Setup

Copy:

~~~text
Addon/AzerothFriendUI/
~~~

to:

~~~text
World of Warcraft 3.3.5/Interface/AddOns/AzerothFriendUI/
~~~

Verify the folder contains at least:

- <code>AzerothFriendUI.toc</code>
- <code>AzerothFriendUI.lua</code>
- <code>AzerothFriendUI.xml</code>
- <code>AzerothFriendGoals.lua</code>
- <code>AzerothFriendProtocol.lua</code>

Enable **AzerothFriend UI** from the AddOns button on the character-select screen.

---

## Running the System

Start MySQL, authserver, and worldserver using your normal AzerothCore/Playerbots workflow, then start the Python bridge.

### Standalone Bridge

Linux:

~~~bash
cd ~/azerothcore-wotlk/modules/mod-azeroth-friend/tools
python3 azeroth_friend_bridge.py   --config ~/azerothcore-wotlk/env/dist/etc/modules/mod_azeroth_friend.conf
~~~

Windows:

~~~bat
start_bridge.bat
~~~

A healthy startup should initialize database access, LiveState, the optional JSON-RPC gateway, and the plugin manager.

### Bridge CLI and JSON-RPC

With the bridge running and RPC enabled:

~~~bash
python3 tools/af_ctl.py status
python3 tools/af_ctl.py health
python3 tools/af_ctl.py list
python3 tools/af_ctl.py bot 12345
python3 tools/af_ctl.py mode 12345 travel
python3 tools/af_ctl.py goal 12345 "Travel to the next quest hub"
python3 tools/af_ctl.py autonomy 12345 on
python3 tools/af_ctl.py invoke 12345 follow "{}"
python3 tools/af_ctl.py zone "Elwynn"
python3 tools/af_ctl.py stats
~~~

The CLI defaults to <code>http://127.0.0.1:8378/rpc</code>.

### Dual-Bridge Orchestrator with mod-llm-chatter

AzerothFriend can operate alone, but it has explicit integration with <code>mod-llm-chatter</code> for shared perception, speech ownership, and token reuse.

On Windows:

~~~bat
start_all_bridges.bat
~~~

On Linux, run each bridge as its own service/process and point both at their installed configuration files. AzerothFriend's chatter settings control whether ambient speech is delegated, whether sensory events are reused, and whether token-sharing behavior is enabled.

---

## In-Game Commands Reference

The server-side command prefix is <code>.af</code>. The addon also exposes <code>/af</code> slash commands for UI and convenience controls.

### General and Inspection Commands

Common server commands include:

| Command | Purpose |
|---|---|
| <code>.af status [bot]</code> | Show companion status. |
| <code>.af diag [bot]</code> | Show runtime/lease/playerbot diagnostics. |
| <code>.af context [bot]</code> | Refresh or inspect companion context. |
| <code>.af thinking [low\|normal\|high]</code> | Change the thinking cadence tier. |
| <code>.af claim &lt;bot&gt;</code> | Claim the companion for direct owner control. |
| <code>.af release &lt;bot&gt;</code> | Release the claim and reconnect bridge control with autonomy still off. |

Use <code>.af</code> with no valid subcommand to print the current server-side help for your build.

### Goal Management and Suggestions

Goal and suggestion commands include:

~~~text
.af goal [bot] set <short-term goal>
.af goal [bot] longterm <long-term goal>
.af goal [bot] show
.af goal [bot] pause
.af goal [bot] resume
.af goal [bot] complete
.af goal [bot] clear
.af goal [bot] generate [both|long|short]
.af suggest [bot] [both|long|short|actions]
.af generate [bot] [both|long|short|actions]
~~~

Suggestion requests are handled by the bridge even when autonomy is disabled. The bridge can return grounded tactical action suggestions in addition to goal text.

### Autonomy and Control

~~~text
.af autonomy [bot] on
.af autonomy [bot] off
.af autonomy [bot] status
~~~

Important behavior:

- autonomy requires a focused short- or long-term goal;
- owner commands remain available while autonomy is off;
- claiming the bot disables bridge autonomy;
- releasing a claim reconnects bridge control but does not silently turn autonomy back on;
- owner changes interrupt stale in-flight plans.

### Action and Spell Dispatch

The addon exposes convenient slash actions such as:

~~~text
/af attack
/af follow
/af stay
/af flee
/af loot
/af rest
/af rpg <quest id>
/af accept
/af reward <1-6>
/af open
/af trainer
/af action <catalog action>
~~~

Natural-language spell requests are supported through the companion command path and learned-spell resolver.

For low-level manual capability dispatch, prefer Bot API v1 rather than inventing raw playerbot commands.

### Bot API v1 Inspection

Bot API v1 is server-owned. The addon and bridge consume the contract; they do not redefine it.

Useful commands:

~~~text
.af catalog [filter] [bot]
.af catalog describe <capability> [bot]
.af run <bot> <action> [json params]
~~~

The contract view exposes capability metadata such as:

- category and authority;
- parameter/result schemas;
- native binding kind;
- preconditions;
- completion policy;
- dispatch and verification status.

The bridge caches the live capability catalog by revision and refreshes it when the server changes the contract.

---

## Addon Interface Guide (AzerothFriendUI)

Open the master HUD with:

~~~text
/af
~~~

You can also use <code>/azerothfriend</code> or <code>/friend</code>.

### Tab Breakdown

| Tab | What it shows |
|---|---|
| **Context** | Self-context, bags, gear, consumables, quests, master distance, zone/subzone, and surroundings. |
| **Mindset / Debug** | Active mindset, commitment window, claim/autonomy state, thinking policy, transport status, and token counters. |
| **Actions** | Current action/plan state, execution results, action-mode controls, and direct commands. |
| **Thoughts** | Thought stream, tactical/action badges, speech, memories, and goal reasoning. |
| **Bot API v1** | Live capability catalog, contract details, authority, bindings, and verified outcomes. |

The minimap button toggles the HUD and can switch between unified and floating layouts.

### Addon Slash Commands

Common addon commands:

~~~text
/af
/af context
/af debug
/af actions
/af thoughts
/af api
/af catalog
/af sync
/af minimap
/af layout
/af reset
/af claim
/af release
~~~

Long telemetry payloads are chunked and reassembled by the addon protocol so large context and API responses are not limited to a single chat line.

---

## Configuration Reference

The full authoritative list is <code>conf/mod_azeroth_friend.conf.dist</code>. These are the highest-impact settings:

| Parameter | Default | Purpose |
|---|---:|---|
| <code>AzerothFriend.Enable</code> | <code>1</code> | Enables the module. |
| <code>AzerothFriend.ControlledBots</code> | <code>Friendbot,Ollamatest</code> | Comma-separated bot allowlist. |
| <code>AzerothFriend.MaxControlledBots</code> | <code>1</code> | Maximum concurrently managed companions. |
| <code>AzerothFriend.TickIntervalMs</code> | <code>500</code> | C++ pending-action polling interval. |
| <code>AzerothFriend.Environment.ScanRadius</code> | <code>30.0</code> | Nearby environment scan radius in yards. |
| <code>AzerothFriend.Environment.MaxVisibleEntities</code> | <code>20</code> | Cap on visible entities in environment context. |
| <code>AzerothFriend.LiveState.Enable</code> | <code>1</code> | Enables RAM-first TCP state transport. |
| <code>AzerothFriend.LiveState.Port</code> | <code>8377</code> | LiveState TCP port. |
| <code>AzerothFriend.LiveState.Secret</code> | empty | Shared LiveState authentication secret; required for the listener to start. |
| <code>AzerothFriend.Rpc.Enable</code> | <code>1</code> | Enables local JSON-RPC administration. |
| <code>AzerothFriend.Rpc.Port</code> | <code>8378</code> | JSON-RPC HTTP port. |
| <code>AzerothFriend.Plugins.Enable</code> | <code>1</code> | Loads bridge plugins from <code>tools/plugins/</code>. |
| <code>AzerothFriend.MultiActionPlan.MaxSteps</code> | <code>5</code> | Maximum steps in a generated plan. |
| <code>AzerothFriend.Autonomy.Enable</code> | <code>1</code> | Enables autonomous event/tick generation globally. |
| <code>AzerothFriend.Autonomy.TickSeconds</code> | <code>5</code> | Autonomous idle tick cadence. |
| <code>AzerothFriend.Autonomy.ThinkingCadence</code> | <code>low</code> | Idle thought cadence tier. |
| <code>AzerothFriend.Autonomy.HeartbeatSeconds</code> | <code>60</code> | Re-plan heartbeat for unchanged situations. |
| <code>AzerothFriend.Autonomy.MinimumPlanSeconds</code> | <code>10</code> | Minimum gap between autonomous plans. |
| <code>AzerothFriend.Goal.AutoGenerate.Enable</code> | <code>1</code> | Initializes empty goals for newly registered companions. |
| <code>AzerothFriend.LLM.Thinking.Mode</code> | <code>Auto</code> | Controls use of reasoning-capable model settings. |
| <code>AzerothFriend.LLM.Thinking.Effort</code> | <code>low</code> | Requested reasoning effort. |
| <code>AzerothFriend.Context.MaxInputTokens</code> | <code>3500</code> | Planner context ceiling used by the bridge. |
| <code>AzerothFriend.Grind.SearchRadiusYards</code> | <code>60.0</code> | Default ambient grind/roam radius. |

Settings are split between C++ worldserver behavior and Python-bridge behavior. Restart the relevant process after changing a setting that is only read at startup.

---

## Troubleshooting and Diagnostics

### Common Symptoms and Solutions

**Bridge fails schema verification**

- Confirm the AzerothFriend character-database base schema and updates are installed.
- Read the exact startup error before changing tables manually.
- The bridge explicitly checks the RAM-first context migration at startup.

**LiveState is offline**

- Confirm <code>AzerothFriend.LiveState.Enable = 1</code>.
- Set a non-empty <code>AzerothFriend.LiveState.Secret</code>.
- Keep host/port aligned between the C++ module and bridge.
- Check that loopback port <code>8377</code> is not already in use.

**RPC CLI cannot connect**

- Confirm <code>AzerothFriend.Rpc.Enable = 1</code>.
- Run <code>python3 tools/af_ctl.py health</code>.
- Verify the bridge is listening on <code>127.0.0.1:8378</code>.
- RPC is intentionally loopback-only by default.

**Companion is registered but does not act autonomously**

- Check <code>.af autonomy &lt;bot&gt; status</code>.
- Make sure the companion has a short- or long-term goal.
- Ensure the bot is not currently claimed.
- Check <code>AzerothFriend.Autonomy.Enable</code> and allowed modes.
- Run <code>.af diag &lt;bot&gt;</code> to inspect control and playerbot state.

**Suggested action looks correct but does not execute**

- Suggestions are advisory until dispatched.
- Inspect Bot API v1 for the exact capability and required parameters.
- Check the Actions tab for server dispatch/verification results.
- The server may reject an action because of authority, revision, range, target, cooldown, ownership, or another precondition.

**Spell cannot be resolved**

- The bot must actually know the spell.
- Confirm Spell DBC/catalog data is available.
- Try a more specific spell/rank name if the request is ambiguous.

**Addon appears stuck in thinking state or clears too early**

- Update the addon and module together; recent versions changed thinking-lock/debounce behavior and thought timeline handling.
- Use <code>/af debug</code> to compare client state with bridge/server telemetry.

**Need a quick bridge health snapshot**

~~~bash
python3 tools/af_ctl.py status
python3 tools/af_ctl.py health
~~~

---

## License and Credits

- **AzerothCore** — open-source WoW 3.3.5a server framework.
- **mod-playerbots** — required Playerbot fork/module runtime and physical bot execution layer.
- **mod-llm-chatter** — optional dialogue/perception integration used for speech delegation and sensory reuse.
- **agent-wow** — credited in source comments for architectural ideas adapted by the JSON-RPC, plugin, and offline-catalog work.
- **AzerothFriend contributors** — companion module, Python bridge, addon UI, Bot API integration, planning, goals, memory, and telemetry.

Review the repository and upstream dependency licenses before redistribution.
