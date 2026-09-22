# AzerothFriend UI Addon (WoW 3.3.5 AzerothCore)

**AzerothFriend UI** is an advanced in-game companion HUD and real-time debugging dashboard for the `mod-azeroth-friend` autonomous AI companion module. It provides live character self-awareness, cognitive momentum tracking, token savings telemetry, action execution pipelines, and surroundings radar without consuming extra LLM tokens.

---

## Key Features

### 1. Zero-Token Context & Inventory Inspector (`/af context`)
- **Character Self-Awareness**: Average gear item level (`avg_ilvl`), equipped weapon classifications, free bag space vs total slots (color-coded).
- **Consumables Stock**: Live counts of usable water, food, and health/mana potions.
- **Active Quests**: Real-time log of tracked quests with progress status (`[COMPLETE]` / `[IN PROGRESS]`).
- **World Awareness**: Zone name, SubZone name, Tavern/Inn resting status (`[INN]`), Outdoors flag, Swimming state, and Mounted state.
- **Companion Master Tracking**: Master name, real-time distance in yards, master HP %, and combat status.
- **Cost**: **0 LLM Tokens** (Pulls directly from C++ memory and local MySQL state).

### 2. Cognitive Mindset & Live Debugging Dashboard (`/af debug`)
- **Mindset Monitor (AI ADHD Prevention)**: Displays active cognitive state (`RESTING`, `LOOTING`, `FOLLOWING`, `COMBAT`, `EXPLORING`, `SOCIAL`, `IDLE`).
- **Commitment Latency Window**: Shows remaining goal commitment duration, suppressing distracting ambient interruptions.
- **Bot Agency Lock**: Displays claim state (`CLAIMED` vs `RELEASED`).
- **Token Synergy Dashboard**: Live counter of cumulative LLM tokens saved (~1200 tokens per call), count of compound speech calls, and zero-token sensory reuse actions from `mod-llm-chatter`.
- **Reasoning Policy**: Displays active OpenAI model profile (legacy vs modern thinking models like `gpt-5-nano` with minimal effort).

### 3. Interactive Zero-Token Remote Control Bar
In-game HUD buttons to issue immediate local client-to-server commands with zero LLM API overhead:
- **[Claim / Release]**: Toggle exclusive companion control and suspend random roaming.
- **[Combat / Travel / Idle / Social]**: Apply the server-backed tactical action mode and its native playerbot strategies.
- **[Sync]**: Request immediate fresh context dump from C++ server with zero tokens.
- **[Debug]**: Open live mindset and transport diagnostics; Shift-click toggles verbose server logging.
- **[Autonomy switch]**: Opt the companion in or out of self-directed play. The checkbox
  reflects server telemetry (`FRIEND_GOAL`) rather than guessing, so it only flips once the
  server confirms. Enabling requires a focused goal — set one with the goal panel (`/af goals`).
  While autonomy is off, owner commands (`/af cast`, `/af action`, whispers) still work.

### 4. Playerbot Semantic Bot API v1
- **Server-owned contract view (`/af api`)**: Renders live API version/revision, execution switches, authority counts, and the curated capability index from `FRIEND_API` telemetry.
- **Describe**: Requests `.af catalog describe <capability> <bot>` and displays parameter/result schemas, native binding, preconditions, authority, and completion policy.
- **Verified outcomes**: Enriched `FRIEND_ACTIONS` telemetry distinguishes dispatch acknowledgement from a verified world-state result.
- **Guarded manual call**: `Shift + Run` dispatches `.af run <bot> <capability> <json>`; server ownership, revision, lease, precondition, and playerbot-binding checks remain authoritative.
- **No client schema copy**: The addon never decides what a capability means or whether it is permitted.

### 5. Unified Tabbed Master HUD & Minimap Icon
- **Master Tabbed HUD (`/af`)**: Compact single-window interface with 5 tabs: `[Context]`, `[Mindset/Debug]`, `[Actions]`, `[Thoughts]`, and `[Bot API v1]`.
- **Minimap Quick-Toggle Icon**: Movable icon docked on the minimap ring:
  - **Left-Click**: Toggle Master HUD.
  - **Right-Click**: Toggle between Tabbed Master window and Floating panels (`/af layout`).
  - **Hover Tooltip**: Displays companion name, active mindset, and total tokens saved.
- **Clean Chat Filter**: Intercepts and filters raw `[FRIEND_*]` data packets so standard chat windows remain 100% clean.

---

## Installation

1. Copy the `Addon` folder into your World of Warcraft client directory:
   ```
   World of Warcraft 3.3.5/Interface/AddOns/AzerothFriendUI/
   ```
2. Ensure the following files are present inside `AzerothFriendUI/`:
   - `AzerothFriendUI.toc`
   - `AzerothFriendUI.lua`
   - `AzerothFriendUI.xml`
3. Launch WoW 3.3.5, click **AddOns** at the character selection screen, and verify **AzerothFriend UI** is enabled.

---

## In-Game Slash Commands

You can use `/af`, `/azerothfriend`, or `/friend`:

| Command | Action |
|---|---|
| `/af` | Toggle Unified Master HUD |
| `/af attack` | Order companion to attack your current target |
| `/af follow` | Force companion into formation follow |
| `/af stay` / `/af hold` | Force companion to stay and hold position |
| `/af flee` | Order companion to disengage and retreat to master |
| `/af loot` / `/af loot all` | Order companion to harvest nearby corpses |
| `/af rest` | Order companion to eat and drink to recover |
| `/af rpg <id>` | Order companion to navigate to & complete quest ID |
| `/af rpg ?` | Query companion's active quest status |
| `/af accept` | Order companion to accept offered quest from NPC |
| `/af reward <1-6>` | Choose quest reward index |
| `/af co` | Toggle companion major offensive/defensive cooldowns |
| `/af open` | Open items (clams, lockboxes, quest containers) |
| `/af trainer` | Order companion to learn available spells from class trainer |
| `/af action <cmd>` | Dispatch any indexed catalog action directly |
| `/af claim` / `/af release` | Toggle companion agency lock |
| `/af context` / `/af radar` | Open Merged Context & Surroundings Radar Inspector (Tab 1) |
| `/af debug` | Open Cognitive Mindset & Token Synergy Telemetry (Tab 2) |
| `/af actions` | Open Action Execution Pipeline & Interactive Stances (Tab 3) |
| `/af thoughts` | Open Cognitive Reasoning, Speech & Dialogue Stream (Tab 4) |
| `/af api` / `/af catalog` | Open the Bot API v1 catalogue and contract inspector (Tab 5) |
| `/af sync` | Trigger instant zero-token server context refresh |
| `/af minimap` | Toggle minimap ring icon |
| `/af layout` | Toggle between Unified Tabbed HUD and Floating panels |
| `/af reset` | Reset all HUD window positions |

---

## Zero-Token Telemetry Protocol

The addon accepts telemetry via `CHAT_MSG_SYSTEM` and `CHAT_MSG_ADDON` (Prefix: `AZEROTH_FRIEND`):

Long payloads are framed as chunked `[AF1] <id>:<index>:<count>:<hex>` packets and reassembled
before parsing, so multi-kilobyte context dumps survive chat line limits.

- `[FRIEND_STATE] <Name: ...|Health: ...|Zone: ...>`
- `[FRIEND_CONTEXT] <SelfContext: ...|ZoneName: ...|MasterDist: ...>`
- `[FRIEND_MINDSET] <Claimed: ...|TokensSaved: ...|SensoryReused: ...>`
- `[FRIEND_ACTIONS] <Active: ...|Status: ...|Step: ...|ApiVersion: ...|Authority: ...|BindingKind: ...|Completion: ...|Verified: ...>`
- `[FRIEND_API] <View: overview|catalog|contract|ApiVersion: ...|CapabilityRevision: ...>`
- `[FRIEND_ENV] <HOSTILE: ...|FRIENDLY: ...|LOOT: ...>`
- `[FRIEND_THOUGHT] <Thought: ...|Mindset: ...|Plan: ...>`
- `[FRIEND_MEMORY] <Memory: ...|whisper: ...>`
- `[FRIEND_GOAL] {"bot": ..., "enabled": true|false, "status": ..., "goal": ..., "progress": ..., "result": ...}`
- `[FRIEND_HISTORY]` — last five actions with dispatch/completion status
- `[FRIEND_PLAYERS]` — nearby players with level, health and distance
