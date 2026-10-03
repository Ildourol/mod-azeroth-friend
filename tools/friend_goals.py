"""Deterministic gates around the goal planner. No model calls here."""
import json
import re
import time


def select_bots(bots, names, limit=1):
    """An empty configuration selects nobody, including on event refresh."""
    ordered = list(dict.fromkeys(n.strip().casefold() for n in names if n.strip()))
    if limit > 0:
        ordered = ordered[:limit]
    by_name = {b['bot_name'].casefold(): b for b in bots}
    return {by_name[n]['bot_guid']: by_name[n] for n in ordered if n in by_name}


class GoalScheduler:
    def __init__(self, heartbeat=60, minimum=10):
        self.heartbeat = max(30, heartbeat)
        self.minimum = max(3, minimum)
        self.last = {}

    def should_plan(self, bot, state, event, history, now=None):
        if event.get('event_type') == 'player_command':
            return True
        if event.get('event_type') == 'dialogue_heard':
            return False
        if not bot.get('autonomy_enabled') or bot.get('goal_status') != 'active':
            return False
        if any(a['status'] in ('pending', 'in_progress') for a in history):
            return False
        if state.get('in_combat'):
            return False  # Tactical decisions belong to Playerbots.
        now = time.monotonic() if now is None else now
        environment = state.get('environment_json') or {}
        if isinstance(environment, str):
            environment = json.loads(environment)
        signature = json.dumps([
            bot.get('control_revision'), bot.get('current_goal'), state.get('level'),
            int(state.get('health_pct', 100)) // 20, state.get('map_id'),
            round(float(state.get('pos_x', 0)) / 20), round(float(state.get('pos_y', 0)) / 20),
            [(e.get('guid'), e.get('type')) for e in environment.get('nearby', [])],
            [(a['id'], a['status']) for a in history],
        ], sort_keys=True)
        previous, timestamp = self.last.get(bot['bot_guid'], (None, -float('inf')))
        if now - timestamp < self.minimum or (signature == previous and now - timestamp < self.heartbeat):
            return False
        self.last[bot['bot_guid']] = (signature, now)
        return True


def validate_goal_update(update, state, goal=''):
    """Completion needs a verifiable observation; prose alone is not evidence."""
    if not isinstance(update, dict):
        return None
    status = update.get('status', 'active')
    if status not in ('active', 'completed', 'blocked'):
        return None
    result = str(update.get('result', ''))[:1000]
    progress = str(update.get('progress', ''))[:1000]
    evidence = update.get('evidence') or {}
    if status == 'completed':
        kind = evidence.get('kind') if isinstance(evidence, dict) else None
        verified = False
        try:
            if kind == 'level':
                target = re.fullmatch(r'\s*(?:reach|get to)\s+level\s+(\d+)\s*[.!]?\s*', goal, re.I)
                verified = bool(target and int(evidence['value']) == int(target[1]) and
                                int(state.get('level', 0)) >= int(target[1]) > 0)
            elif kind == 'position':
                # Free-form destinations require owner confirmation until a trusted
                # destination contract can be captured when setting the goal.
                verified = False
        except (KeyError, TypeError, ValueError):
            pass
        if not verified:
            return {'status': 'active', 'progress': progress,
                    'result': 'Completion awaiting verifiable game state or owner confirmation.'}
    if status == 'blocked' and not result.strip():
        return None
    return {'status': status, 'progress': progress, 'result': result}


import logging
import random
from typing import Optional, Tuple, Dict, Any, List, Set

logger = logging.getLogger("azeroth_friend.goals")

ABSTRACT_ARCHETYPES: Dict[str, Dict[str, Any]] = {
    "xp": {
        "theme": "Focus on combat progression, slaying creatures, and advancing character levels through adventuring.",
        "fallbacks": [
            ("Advance character combat power and gain a full experience level.", "Reach the maximum combat level and master class abilities."),
            ("Grind nearby hostile beasts and complete local quests for XP.", "Become a renowned veteran champion of our faction."),
        ]
    },
    "money": {
        "theme": "Focus on earning coin, harvesting trade goods, looting monsters, and accumulating wealth.",
        "fallbacks": [
            ("Loot valuable trade items and earn gold from vendor sales.", "Amass a personal fortune in gold to afford mounts and training."),
            ("Gather natural resources and sell junk to local merchants.", "Become an affluent trader with deep coin reserves."),
        ]
    },
    "loot": {
        "theme": "Focus on finding better gear, acquiring weapons and armor upgrades, and obtaining dungeon treasures.",
        "fallbacks": [
            ("Hunt for weapon and armor upgrades to improve combat gear.", "Equip a complete set of superior quality armor and enchanted weapons."),
            ("Defeat challenging enemies and bosses for rare equipment drops.", "Acquire the finest equipment available for our class specialization."),
        ]
    },
    "exploration": {
        "theme": "Focus on discovering new territories, clearing regional quest hubs, and mastering uncharted lands.",
        "fallbacks": [
            ("Explore surrounding zones, scouting map locations and quest outposts.", "Chart every corner of Azeroth and uncover forgotten territories."),
            ("Travel through adjacent regions and assist local settlements.", "Master all flight paths and regional territories across the continent."),
        ]
    }
}


def generate_starter_goals(
    bot_info: Dict[str, Any],
    categories_config: str = "mixed",
    llm_client: Optional[Any] = None
) -> Tuple[str, str]:
    """Generates an authentic World of Warcraft player goal pair (short_term, long_term) once.
    Uses abstract archetypes and queries the LLM if available; otherwise uses archetype fallbacks.
    """
    valid_categories = list(ABSTRACT_ARCHETYPES.keys())
    configured = [c.strip().lower() for c in (categories_config or "mixed").split(",") if c.strip()]
    if "mixed" in configured or "all" in configured or not configured:
        chosen_cat = random.choice(valid_categories)
    else:
        candidates = [c for c in configured if c in ABSTRACT_ARCHETYPES]
        chosen_cat = random.choice(candidates) if candidates else random.choice(valid_categories)

    archetype = ABSTRACT_ARCHETYPES[chosen_cat]
    theme = archetype["theme"]
    bot_name = bot_info.get("bot_name", "Companion")
    level = bot_info.get("level", 1)
    cls_name = bot_info.get("class_name", "Adventurer")

    # Try LLM generation if available
    if llm_client:
        prompt = (
            f"Generate an authentic World of Warcraft player goal pair for a bot companion.\n"
            f"Character: {bot_name}, Class: {cls_name}, Level: {level}.\n"
            f"Theme: {theme}\n\n"
            "Return JSON only with exact keys:\n"
            "{\n"
            '  "short_term_goal": "<concise 5-10 word actionable task>",\n'
            '  "long_term_goal": "<broad 8-15 word overarching purpose>"\n'
            "}"
        )
        try:
            res = llm_client.generate_json_plan(
                [{"role": "system", "content": "You create authentic WoW player goals."},
                 {"role": "user", "content": prompt}],
                event_type="goal_init"
            )
            data = res if isinstance(res, dict) else (json.loads(res) if isinstance(res, str) else None)
            if data and data.get("short_term_goal") and data.get("long_term_goal"):
                st = str(data["short_term_goal"]).strip()
                lt = str(data["long_term_goal"]).strip()
                if st and lt:
                    logger.info("[GOAL] AI generated goals for %s (Category: %s): Short='%s', Long='%s'",
                                bot_name, chosen_cat, st, lt)
                    return st, lt
        except Exception as e:
            logger.debug("[GOAL] AI goal generation exception, falling back to archetype: %s", e)

    pair = random.choice(archetype["fallbacks"])
    logger.info("[GOAL] Archetype goals assigned to %s (Category: %s): Short='%s', Long='%s'",
                bot_name, chosen_cat, pair[0], pair[1])
    return pair[0], pair[1]


PROGRESSION_BRACKETS: Dict[int, Dict[str, Any]] = {
    0: {  # Levels 1-9: Starter Fundamentals
        "theme": "Starter valley fundamentals, trials, and initial supplies",
        "long_term_by_class": {
            "Warrior": "Master frontline combat stances and forge unbreakable warrior prowess.",
            "Paladin": "Uphold the virtues of the Holy Light and defend our allies from darkness.",
            "Hunter": "Master marksmanship and the wilderness survival arts of our ancestors.",
            "Rogue": "Hone stealth techniques and lethal blade precision from the shadows.",
            "Priest": "Deepen spiritual devotion to heal and shield our companions with Holy Light.",
            "Death Knight": "Master the dark runic arts and unholy power granted in service.",
            "Shaman": "Harmonize with ancestral spirits and wield elemental fury.",
            "Mage": "Attune to the arcane flows and master destructive spellcraft.",
            "Warlock": "Bound demonic entities to our will and harvest soul power.",
            "Druid": "Attune with nature's balance and awaken ancient shapeshifting forms.",
            "default": "Master the fundamental combat arts and survive our starter trials."
        },
        "short_term_by_class": {
            "Warrior": "Slay local hostile beasts and collect starter armor upgrades",
            "Paladin": "Judge encroaching enemies with holy seals and protect travelers",
            "Hunter": "Hunt valley wildlife for meat and pelts, and practice ranged kiting",
            "Rogue": "Scout hostile encampments from stealth and practice precision strikes",
            "Priest": "Mend wounded companions and smite hostile aggressors",
            "Death Knight": "Slay adversaries with icy touch and practice blood strike combinations",
            "Shaman": "Call upon primal lightning and complete ancestral elemental trials",
            "Mage": "Conjure provisions, practice fireball rotations, and eliminate enemy scouts",
            "Warlock": "Summon demon minion and harvest soul shards from hostile beasts",
            "Druid": "Gather herbal reagents, cast wrath on corrupters, and awaken nature forms",
            "default": "Complete starter valley trials and gather basic supplies"
        }
    },
    1: {  # Levels 10-19: Specialization & First Dungeons
        "theme": "Talents, class specialization quests, and introductory dungeons",
        "long_term_by_class": {
            "Warrior": "Unlock warrior talent specializations and stand as an impenetrable frontline bulwark.",
            "Paladin": "Attain holy or retributive specialization and purify contested dungeons.",
            "Hunter": "Bond with a loyal wilderness beast companion and master ranged marksmanship.",
            "Rogue": "Master lockpicking, dual-wielding daggers, and clandestine ambush tactics.",
            "Priest": "Master holy triage or shadow word incantations in regional dungeon delves.",
            "Death Knight": "Dominate runic combinations and conquer contested battlegrounds.",
            "Shaman": "Master totem attunements and channel elemental shocks in early dungeon expeditions.",
            "Mage": "Master frost and fire specializations and research advanced arcane conduits.",
            "Warlock": "Complete demonic pact quests and siphon the souls of our faction's adversaries.",
            "Druid": "Master feral and balance forms to harmonize raw beast fury with healing rejuvenation.",
            "default": "Unlock talent specializations, complete class rites, and conquer introductory dungeons."
        },
        "short_term_by_faction": {
            "Alliance": "Travel to Westfall, explore the Deadmines, and complete class talent quests.",
            "Horde": "Travel to the Barrens, delve into Wailing Caverns, and complete quest trials.",
            "default": "Explore introductory dungeons, complete class quests, and unlock our specialization talent."
        }
    },
    2: {  # Levels 20-29: Weapon Mastery & Regional Quests
        "theme": "Weapon upgrades, Shadowfang Keep / Blackfathom Deeps, and contested outposts",
        "long_term": "Master advanced weapon proficiencies and secure our reputation across contested regions.",
        "short_term": "Upgrade weapon armaments, explore Shadowfang Keep or Blackfathom Deeps, and level trade skills."
    },
    3: {  # Levels 30-39: Contested Renown & Riding
        "theme": "Apprentice riding, Scarlet Monastery, and Stranglethorn Vale",
        "long_term": "Earn coin for apprentice riding, conquer Scarlet Monastery, and master mid-tier abilities.",
        "short_term": "Assault Scarlet Monastery wings, hunt dangerous game in Stranglethorn Vale, and train apprentice riding."
    },
    4: {  # Levels 40-49: Ancient Ruins & Journeyman Riding
        "theme": "Tanaris, Zul'Farrak, Maraudon, and journeyman riding",
        "long_term": "Attain journeyman riding speed, delve into ancient desert ruins, and master high-tier abilities.",
        "short_term": "Explore the sands of Tanaris, conquer Zul'Farrak and Maraudon, and acquire journeyman riding."
    },
    5: {  # Levels 50-59: Plaguelands & Champion Threshold
        "theme": "Plaguelands Scourge, Blackrock Depths, and level 60 threshold",
        "long_term": "Cleanse the Scourge in the Plaguelands, conquer Blackrock Mountain, and achieve champion status at level 60.",
        "short_term": "Assault Blackrock Depths, cleanse Stratholme and Scholomance, and complete level 60 preparation."
    },
    6: {  # Levels 60-69: Beyond the Dark Portal
        "theme": "Outland expedition, Hellfire Peninsula, Zangarmarsh, and aerial flying",
        "long_term": "March beyond the Dark Portal, conquer the Outland frontier, and master aerial riding.",
        "short_term": "Establish base camps in Hellfire Peninsula and Zangarmarsh, and clear Outland citadel dungeons."
    },
    7: {  # Levels 70-79: The Northrend Expedition
        "theme": "Northrend campaign, Borean Tundra, Dragonblight, and cold weather flying",
        "long_term": "Champion the Northrend expedition, attain cold weather flying, and reach the level 80 threshold.",
        "short_term": "Advance through Borean Tundra and Dragonblight, attune to Dalaran, and level toward 80."
    },
    8: {  # Level 80: Endgame Raids & Heroics
        "theme": "Emblem farming, Icecrown Citadel, Ulduar, and heroic dungeon mastery",
        "long_term": "Stand among the greatest champions of Azeroth in the halls of Icecrown Citadel and Ulduar.",
        "short_term": "Farm Emblem of Triumph in Heroic dungeons, optimize gems and enchants, and prepare for raid trials."
    }
}


def get_progression_bracket(level: int) -> int:
    if level >= 80:
        return 8
    elif level >= 70:
        return 7
    elif level >= 60:
        return 6
    elif level >= 50:
        return 5
    elif level >= 40:
        return 4
    elif level >= 30:
        return 3
    elif level >= 20:
        return 2
    elif level >= 10:
        return 1
    return 0


# Extensive alternative goal and action pools to guarantee diversity when LLM is unavailable or for fallbacks
DIVERSE_POOLS: Dict[str, Dict[str, List[str]]] = {
    "Warrior": {
        "long_term": [
            "Master frontline combat stances and forge unbreakable warrior prowess.",
            "Become a legendary vanguard colossus capable of withstanding the fiercest dragons.",
            "Wield two-handed armaments with lethal precision and lead our warband into glorious battle.",
            "Master defensive shield techniques and protect all companion allies under our banner.",
            "Forge a reputation as an unstoppable gladiator feared across enemy territory.",
            "Unleash berserker fury across every contested battlefield in Azeroth.",
        ],
        "short_term": [
            "Slay local hostile beasts and collect starter armor upgrades",
            "Crush nearby hostile brutes with thunderclap and practice battle stance rotations",
            "Hunt aggressive predators, gather meat, and sharpen weapon blades",
            "Clear out nearby hostile bandit camps and fortify our outpost perimeter",
            "Execute flanking sweeps against encroaching enemies and upgrade our armor plate",
            "Lead assault on hostile beast dens and secure local travel routes",
        ],
        "actions": [
            "attack nearest hostile threat, loot all",
            "grind nearby enemies, follow master",
            "attack, use defensive cooldowns, loot",
            "follow, guard master flank",
        ]
    },
    "Paladin": {
        "long_term": [
            "Uphold the virtues of the Holy Light and defend our allies from darkness.",
            "Cleanse every manifestation of evil and undead plague from sacred ground.",
            "Become a consecrated beacon of divine justice, shielding the innocent from harm.",
            "Master holy retribution vows and smite demons and heretics wherever they lurk.",
            "Attain crusader status, purging dark dungeons in the name of the Silver Hand.",
            "Channel boundless healing grace and divine judgment across all frontline campaigns.",
        ],
        "short_term": [
            "Judge encroaching enemies with holy seals and protect travelers",
            "Consecrate contested ground, purge nearby undead, and bless party members",
            "Deliver righteous retribution upon local brigands and recover stolen supplies",
            "Cast holy light on wounded allies, smite hostile scouts, and maintain active seals",
            "Hunt predatory fiends threatening the sanctuary and distribute protective auras",
            "Purify corrupted wildlife and restore peaceful order to the nearby region",
        ],
        "actions": [
            "cast seal, attack target, loot all",
            "judge target, assist master, loot",
            "cast blessing, follow, guard master",
            "attack hostile, cast holy light if injured",
        ]
    },
    "Hunter": {
        "long_term": [
            "Master marksmanship and the wilderness survival arts of our ancestors.",
            "Bond with Azeroth's rarest and most ferocious beasts across untamed continents.",
            "Become an unmatched sharpshooter capable of striking targets unseen from the tree line.",
            "Establish domain over the wildest frontiers and master lethal animal aspect affinities.",
            "Chart unexplored territory and harvest legendary trophies from apex predators.",
            "Hunt down elite wilderness quarry and master tactical traps in every terrain.",
        ],
        "short_term": [
            "Hunt valley wildlife for meat and pelts, and practice ranged kiting",
            "Track down stealthy predators in the brush and practice concussive shot kiting",
            "Scout regional animal dens, lay serpent traps, and harvest prime hides",
            "Coordinate attacks with our beast pet and eliminate enemy outrunners from range",
            "Set snare traps near hostile patrol routes and harvest provisions for camp",
            "Track encroaching humanoids and neutralize dangerous scouts before they alert camps",
        ],
        "actions": [
            "pet_attack, attack with ranged shots, loot",
            "grind local wildlife, skin and loot all",
            "kite hostile target, concussive shot, loot",
            "follow master, pet_attack master target",
        ]
    },
    "Rogue": {
        "long_term": [
            "Hone stealth techniques and lethal blade precision from the shadows.",
            "Infiltrate enemy strongholds undetected and strike down high-value targets.",
            "Master the deadly synergy of crippling poisons, lockpicking, and vanishing acts.",
            "Build an untraceable network of shadow contacts and accumulate contraband riches.",
            "Strike with surgical assassinations and vanish before reinforcements arrive.",
            "Dominate underworld contracts and extract rare treasures from guarded vaults.",
        ],
        "short_term": [
            "Scout hostile encampments from stealth and practice precision strikes",
            "Pickpocket enemy scouts, apply blade poisons, and eliminate sentries quietly",
            "Infiltrate nearby den from shadows, unlock supply footlockers, and ambush leaders",
            "Practice stealth ambush rotations and loot high-value trinkets from sentinels",
            "Disrupt hostile supply chains from stealth and harvest rare lockbox goods",
            "Ambush isolated patrols, harvest trophies, and vanish back into the shadows",
        ],
        "actions": [
            "stealth, ambush target, eviscerate, loot",
            "attack target from rear flank, loot all",
            "open lockboxes, pickpocket nearby hostiles",
            "follow from stealth, assist on master pull",
        ]
    },
    "Priest": {
        "long_term": [
            "Deepen spiritual devotion to heal and shield our companions with Holy Light.",
            "Master the dual duality of divine mending and shadowy mind flay discipline.",
            "Walk the path of saintly grace, preserving companions through harrowing dungeon depths.",
            "Channel the dark whispers of the void to strip the sanity of our adversaries.",
            "Attain spiritual transcendence, shielding entire raiding cohorts from annihilation.",
            "Harmonize holy protection with devastating shadow incantations across all frontiers.",
        ],
        "short_term": [
            "Mend wounded companions and smite hostile aggressors",
            "Apply Power Word: Shield to allies and smite encroaching dark acolytes",
            "Channel holy prayers, cleanse afflictions from travelers, and banish restless spirits",
            "Disperse shadow word pain onto enemy patrols and preserve our companion's health",
            "Collect holy reagents, smite creeping beasts, and safeguard our campsite",
            "Cast mind blasts on hostile commanders and maintain renewal shields on our tank",
        ],
        "actions": [
            "cast Power Word: Shield, smite target, loot",
            "assist master, heal if damaged, loot all",
            "cast Shadow Word: Pain, wand attack, loot",
            "follow, maintain defensive ward on master",
        ]
    },
    "Death Knight": {
        "long_term": [
            "Master the dark runic arts and unholy power granted in service.",
            "Command legions of undead minions and spread virulent frost plague across enemy ranks.",
            "Harness blood runic supremacy to siphon life essence from the mightiest commanders.",
            "Emerge as a relentless harbinger of the frozen North, unyielding in any attrition war.",
            "Master runic power transformations and subjugate Scourge relics across the world.",
            "Become an indomitable dread champion marching across contested continents.",
        ],
        "short_term": [
            "Slay adversaries with icy touch and practice blood strike combinations",
            "Infect hostile packs with blood boil and frost fever, harvesting their vitality",
            "Spread virulent plague upon enemy scouts and summon ghoulish servants to fight",
            "Execute death grip strikes on escaping enemies and freeze hostile commanders",
            "Assault encroaching enemy outposts and empower runic weapons with dark runes",
            "Cleanse the zone of rival undead and practice obliterate weapon strikes",
        ],
        "actions": [
            "cast Icy Touch, Plague Strike, Blood Strike, loot",
            "death grip target, attack, loot all",
            "cast Blood Boil, assist master, loot",
            "follow master, strike master target with runes",
        ]
    },
    "Shaman": {
        "long_term": [
            "Harmonize with ancestral spirits and wield elemental fury.",
            "Master the primal elements of earth, fire, water, and air in harmonious balance.",
            "Commune with ancestral totems to unleash devastating storm strikes and tidal mending.",
            "Become a venerated spiritual elder guiding our faction with primal wisdom.",
            "Channel cataclysmic lightning storms to shatter the fortresses of our foes.",
            "Attune to ancient elemental spirits and protect the sacred ley lines of Azeroth.",
        ],
        "short_term": [
            "Call upon primal lightning and complete ancestral elemental trials",
            "Plant searing and stoneskin totems to repel surrounding aggressive wildlife",
            "Channel lightning bolts into hostile packs and mend allies with healing waves",
            "Gather elemental motes, commune with local elemental nodes, and shock attackers",
            "Empower weapon blades with elemental flame and shock hostile spellcasters",
            "Commune with spirit guides, purge enemy magical buffs, and secure the camp",
        ],
        "actions": [
            "cast Lightning Bolt, Earth Shock, loot all",
            "drop totems, attack target, loot",
            "assist master, cast Healing Wave if low, loot",
            "follow master, shock enemy spellcasters",
        ]
    },
    "Mage": {
        "long_term": [
            "Attune to the arcane flows and master destructive spellcraft.",
            "Command absolute mastery over piercing frost novas and devastating firestorms.",
            "Unravel deep ley line conduits, discovering forgotten ancient arcane secrets.",
            "Become an archmage of Dalaran revered for peerless conjuration and teleportation.",
            "Master the destructive cadence of combustion, freezing time and space at will.",
            "Channel limitless mana reserves to annihilate encroaching dungeon hordes.",
        ],
        "short_term": [
            "Conjure provisions, practice fireball rotations, and eliminate enemy scouts",
            "Freeze encroaching enemies in ice and shatter them with frostbolt volleys",
            "Conjure arcane food and water, scout from afar, and scorch hostile beast dens",
            "Research local ley energy anomalies, practice blink evasions, and incinerate foes",
            "Channel arcane missiles into elite hostile commanders and maintain mana shields",
            "Disrupt enemy spellcasters with counterspells and clear out hostile monster nests",
        ],
        "actions": [
            "cast Frostbolt, Fireball, loot all",
            "conjure food, conjure water, rest",
            "frost nova encroaching enemies, blink away, loot",
            "assist master with ranged spell rotation, loot",
        ]
    },
    "Warlock": {
        "long_term": [
            "Bound demonic entities to our will and harvest soul power.",
            "Master the dark arts of affliction, draining the very lifeforce of our enemies.",
            "Summon devastating demonic horrors and subjugate the Burning Legion's secrets.",
            "Accumulate limitless soul shards, trading dark curses for forbidden power.",
            "Become a dreaded warlock master whose dark incantations make emperors tremble.",
            "Unleash cataclysmic shadow and hellfire upon every citadel that defies us.",
        ],
        "short_term": [
            "Summon demon minion and harvest soul shards from hostile beasts",
            "Spread curses of agony and corruption across enemy encampments",
            "Drain life essence from wounded predators and create healthstones for party",
            "Command voidwalker pet to taunt hostile packs while raining shadow bolts",
            "Harvest fresh soul shards, practice life tap conservation, and banish demons",
            "Curse hostile spellcasters, summon imp support, and siphon vital energy",
        ],
        "actions": [
            "cast Corruption, Curse of Agony, drain soul, loot",
            "pet_attack, cast Shadow Bolt, loot all",
            "create healthstone, cast Demon Armor, follow",
            "assist master with dark curses, siphon life",
        ]
    },
    "Druid": {
        "long_term": [
            "Attune with nature's balance and awaken ancient shapeshifting forms.",
            "Master the wild ferocity of the predator form and the serene resilience of the bear.",
            "Channel celestial solar and lunar powers to cleanse corrupting blights.",
            "Become an archdruid protector of the Emerald Dream and Azeroth's wild spirits.",
            "Harmonize savage melee combat forms with boundless restorative rejuvenation.",
            "Guard the great world trees and restore pristine wild balance to contested zones.",
        ],
        "short_term": [
            "Gather herbal reagents, cast wrath on corrupters, and awaken nature forms",
            "Shift into beast form to stalk and rake hostile beasts threatening the glade",
            "Cast rejuvenation on party allies, moonfire on scouts, and harvest herbs",
            "Shapeshift to bear form, taunt aggressive brutes, and defend our campsite",
            "Cleanse poisoned flora, strike down encroaching poachers, and heal companions",
            "Cast roots on aggressive monsters and harvest natural resources for our journey",
        ],
        "actions": [
            "cast Wrath, Moonfire, loot all",
            "cast Rejuvenation, assist master, loot",
            "shapeshift form, attack target, loot",
            "gather nearby herbs, follow master",
        ]
    }
}


def get_known_hardcoded_goals() -> set:
    """Returns the set of legacy static strings that C++ GenerateContextualGoals previously produced."""
    res = set()
    for b in PROGRESSION_BRACKETS.values():
        if "long_term" in b:
            res.add(b["long_term"])
        if "short_term" in b:
            res.add(b["short_term"])
        for k, v in b.get("long_term_by_class", {}).items():
            res.add(v)
        for k, v in b.get("short_term_by_class", {}).items():
            res.add(v)
        for k, v in b.get("short_term_by_faction", {}).items():
            res.add(v)
    for cat in ABSTRACT_ARCHETYPES.values():
        for pair in cat.get("fallbacks", []):
            res.add(pair[0])
            res.add(pair[1])
    return res


# Suggested actions are echoed into the client HUD and dispatched through
# `.af action`, which splits the string on commas and maps each clause onto a
# native playerbot command. Prose clauses ("use defensive cooldowns",
# "attack nearest hostile threat") are rejected there, so a recommendation the
# body cannot execute is noise. Ground every suggestion onto that verb set
# before it reaches the owner.
# Every verb here either has an explicit branch in AzerothFriendCommand.cpp's
# HandleAction (follow, stay/stop/hold, eat_drink/rest, attack, loot/loot_all,
# flee, grind/grind_nearby/roam_nearby/roam/wander) or resolves to a real
# mod-playerbots chat trigger (attack, follow, stay, grind, flee, cast).
# Bare "assist" is deliberately absent: this playerbot fork has no "assist"
# chat trigger, so it is rewritten to grind_nearby instead.
_EXECUTABLE_ACTION_VERBS = {
    "attack", "follow", "stay", "stop", "hold", "loot", "loot_all",
    "flee", "grind", "grind_nearby", "roam_nearby", "roam", "wander",
    "eat_drink", "rest", "cast",
}

# Prose targets the LLM invents ("nearest hostile threat"). They are not entity
# names, so the clause is rewritten to a command the dispatcher can actually run.
_PLACEHOLDER_TARGETS = {
    "target", "targets", "nearest", "nearby", "hostile", "hostiles", "enemy",
    "enemies", "threat", "threats", "foe", "foes", "mob", "mobs", "creature",
    "creatures", "master", "master's", "masters", "current", "the", "a", "an",
    "any", "local", "some", "attacker", "attackers", "adds",
}

# Last-resort pool: only commands HandleAction maps onto curated playerbot
# behaviour, so a suggestion always survives as something the body can execute.
_EXECUTABLE_ACTION_POOLS: Dict[str, List[str]] = {
    "default": ["attack, loot all", "grind_nearby, loot all", "follow, loot all"],
    "Warrior": ["attack, loot all", "grind_nearby, loot all"],
    "Paladin": ["attack, loot all", "follow, loot all"],
    "Hunter": ["attack, loot all", "roam_nearby, loot all"],
    "Rogue": ["attack, loot all", "follow, loot all"],
    "Priest": ["follow, loot all", "roam_nearby, loot all"],
    "Death Knight": ["attack, loot all", "grind_nearby, loot all"],
    "Shaman": ["attack, loot all", "follow, loot all"],
    "Mage": ["roam_nearby, loot all", "attack, loot all"],
    "Warlock": ["attack, loot all", "follow, loot all"],
    "Druid": ["attack, loot all", "roam_nearby, loot all"],
}

_TRAILING_CONDITION = re.compile(
    r"\s+(?:if|when|while|until|unless|then|after|before|once|should)\b.*$", re.I)

# Tokens that mark a phrase as prose rather than a resolvable entity name.
_TARGET_PROSE_WORDS = _PLACEHOLDER_TARGETS | {
    "with", "from", "using", "at", "on", "in", "into", "by", "for", "to", "and",
    "or", "then", "your", "my", "its", "their", "up", "out", "down", "around",
    "near", "toward", "towards", "before", "after", "if", "when",
}


def _ground_clauses(raw: str) -> List[str]:
    """Split a candidate into the curated command clauses it legitimately contains."""
    clauses: List[str] = []
    seen: Set[str] = set()
    for chunk in re.split(r"[,;]|\band\b", raw or ""):
        text = _TRAILING_CONDITION.sub("", " ".join((chunk or "").split())).strip(" .!?;:'\"")
        if not text:
            continue
        tokens = text.split()
        verb = tokens[0].lower().strip(".!?;:,")
        rest = tokens[1:]

        if verb in ("attack", "assist"):
            # Keep a target only when every token reads like an entity name. Prose
            # targets ("nearest hostile threat", "with ranged shots") are dropped.
            prose = any(t.lower().strip(".!?;:,['\"]") in _TARGET_PROSE_WORDS for t in rest)
            target = "" if prose else " ".join(rest).strip(" .!?;:,'\"")
            if verb == "assist" or (verb == "attack" and not target):
                # The module's grind_nearby is the executable equivalent: it scans for
                # the nearest attackable creature in radius and attacks it.
                clause = "grind_nearby"
            else:
                clause = verb if not target else f"{verb} {target}"
        elif verb == "cast":
            if not rest:
                continue
            clause = "cast " + " ".join(rest)
        elif verb in _EXECUTABLE_ACTION_VERBS:
            clause = verb
        else:
            continue

        key = clause.lower()
        if key not in seen:
            seen.add(key)
            clauses.append(clause)
        if len(clauses) >= 3:
            break
    return clauses


def ground_suggested_actions(raw: str, cls_name: str = "Warrior") -> str:
    """Reduce a suggested action string to clauses the dispatcher can execute.

    Returns a comma-separated sequence of curated playerbot commands (for example
    ``grind_nearby, loot``). Falls back to a class-appropriate executable pool when
    nothing in the candidate survives, so the owner is never handed a recommendation
    the companion body cannot run.
    """
    clauses = _ground_clauses(raw)
    if not clauses:
        pool = _EXECUTABLE_ACTION_POOLS.get(cls_name) or _EXECUTABLE_ACTION_POOLS["default"]
        clauses = _ground_clauses(pool[0]) or ["grind_nearby"]
    return ", ".join(clauses)


def generate_contextual_suggestions(
    bot_info: Dict[str, Any],
    surroundings: Optional[Dict[str, Any]] = None,
    llm_client: Optional[Any] = None,
    scope: str = "both",
    previous_goals: Optional[Dict[str, str]] = None,
    state: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """Generates authentic, fresh World of Warcraft companion objectives and tactical actions.
    Every invocation generates brand new suggestions without repeating or reusing the previous ones.
    Scope can be 'both', 'long', 'short', or 'actions'.
    """
    bot_name = str(bot_info.get("bot_name", "Companion") or "Companion").strip()
    try:
        level = int(bot_info.get("level", 1) or 1)
    except (ValueError, TypeError):
        level = 1

    cls_name = str(bot_info.get("class_name", "Adventurer") or "Adventurer").strip()
    if not cls_name or cls_name.lower() in ("unknown", "none"):
        cid = bot_info.get("class_id") or bot_info.get("class")
        class_map = {1: "Warrior", 2: "Paladin", 3: "Hunter", 4: "Rogue", 5: "Priest",
                     6: "Death Knight", 7: "Shaman", 8: "Mage", 9: "Warlock", 11: "Druid"}
        cls_name = class_map.get(cid, "Warrior")

    race_name = str(bot_info.get("race_name", "") or "").strip()
    team_name = str(bot_info.get("team_name", "") or "").strip()
    if not team_name:
        team_id = bot_info.get("team_id")
        if team_id == 0:
            team_name = "Alliance"
        elif team_id == 1:
            team_name = "Horde"

    personality = str(bot_info.get("personality", "") or "").strip()
    zone_name = ""
    nearby_entities = []
    if surroundings and isinstance(surroundings, dict):
        zone_name = str(surroundings.get("zone_name", "") or "").strip()
        nearby_raw = surroundings.get("nearby", [])
        if isinstance(nearby_raw, list):
            for e in nearby_raw[:5]:
                if isinstance(e, dict) and e.get("name"):
                    nearby_entities.append(str(e["name"]))
    if not zone_name and state and isinstance(state, dict):
        env = state.get("environment_json")
        if isinstance(env, str):
            try:
                env = json.loads(env)
            except Exception:
                env = {}
        if isinstance(env, dict):
            zone_name = str(env.get("zone_name", "") or "").strip()

    bracket = get_progression_bracket(level)
    bracket_data = PROGRESSION_BRACKETS[bracket]

    # Extract previous suggestions to strictly prevent repetition
    prev_dict = previous_goals or {}
    prev_short = str(prev_dict.get("short") or bot_info.get("current_goal") or "").strip()
    prev_long = str(prev_dict.get("long") or bot_info.get("long_term_goal") or "").strip()
    prev_actions = str(prev_dict.get("actions") or bot_info.get("last_suggested_actions") or "").strip()

    themes = [
        "heroic progression and glory", "wilderness beast hunting and survival",
        "dungeon mastery and relic recovery", "faction vanguard and outpost defense",
        "treasure hunting and economic prosperity", "skill mastery and ancient lore",
        "regional pacification and cleansing", "scouting and tactical reconnaissance"
    ]
    chosen_theme = random.choice(themes)
    nonce = f"{time.time():.2f}_{random.randint(1000, 999999)}"

    # 1. Try LLM generation if available
    if llm_client:
        prompt = (
            f"You are the intelligent cognitive companion agent for World of Warcraft (3.3.5a).\n"
            f"Generate authentic, immersive, and strictly NOVEL companion objectives and action recommendations.\n\n"
            f"Character: {bot_name}, Class: {cls_name}, Level: {level}, Race: {race_name or 'Adventurer'}, Faction: {team_name or 'Neutral'}.\n"
            f"Personality: {personality or 'Eager and loyal adventurer'}.\n"
            f"Current Zone: {zone_name or 'Azeroth'}.\n"
            f"Nearby Entities: {', '.join(nearby_entities) if nearby_entities else 'Open surroundings'}.\n"
            f"Level Bracket Theme: {bracket_data['theme']}.\n"
            f"Narrative Focus: {chosen_theme}.\n"
            f"Scope requested: {scope}.\n\n"
            f"CRITICAL REQUIREMENT - ZERO REPETITION:\n"
            f"The player pressed 'Suggest' to get a BRAND NEW suggestion. You MUST NOT reuse or copy the previous ones below:\n"
            f"  Previous Long-term: '{prev_long}'\n"
            f"  Previous Short-term: '{prev_short}'\n"
            f"  Previous Actions: '{prev_actions}'\n"
            f"Provide completely distinct, creative, and situational goals fitting a Level {level} {cls_name}.\n"
            f"Generation Nonce: {nonce}\n\n"
            "Return JSON ONLY with this exact structure:\n"
            "{\n"
            '  "short_term_goal": "<concise 6-12 word actionable task matching current zone and level>",\n'
            '  "long_term_goal": "<broad 8-16 word overarching ambition and class role>",\n'
            '  "suggested_actions": "<1-3 concrete catalog action commands, e.g. attack Kobold Vermin, loot all, accept_quest, trainer learn>",\n'
            '  "action_summary": "<brief 6-10 word tactical explanation>"\n'
            "}"
        )
        try:
            res = llm_client.generate_json_plan(
                [{"role": "system", "content": "You create immersive, creative, and non-repeating World of Warcraft companion objectives. Return JSON only."},
                 {"role": "user", "content": prompt}],
                event_type="goal_generate"
            )
            # Normalize response if wrapped or string
            data = None
            if isinstance(res, dict):
                data = res
            elif isinstance(res, str):
                cleaned = re.sub(r"^```(?:json)?\s*", "", res.strip(), flags=re.I)
                cleaned = re.sub(r"\s*```$", "", cleaned.strip())
                try:
                    data = json.loads(cleaned)
                except Exception:
                    pass

            if data and isinstance(data, dict):
                st = str(data.get("short_term_goal", "")).strip()
                lt = str(data.get("long_term_goal", "")).strip()
                act = str(data.get("suggested_actions", "")).strip()
                act_sum = str(data.get("action_summary", "")).strip()
                # Only recommend commands the companion body can actually run.
                if act:
                    act = ground_suggested_actions(act, cls_name)

                # Ensure non-trivial results
                if st or lt or act:
                    # Validate that the generated suggestion differs from previous according to requested scope
                    if scope == "actions":
                        valid = bool(act and act.lower() != prev_actions.lower())
                    elif scope == "short":
                        valid = bool(st and st.lower() != prev_short.lower())
                    elif scope == "long":
                        valid = bool(lt and lt.lower() != prev_long.lower())
                    else:  # "both" or default
                        valid = bool((st and st.lower() != prev_short.lower()) or (lt and lt.lower() != prev_long.lower()) or (act and act.lower() != prev_actions.lower()))

                    if valid:
                        logger.info("[GOAL] AI generated fresh novel suggestions for %s [scope=%s]: Short='%s', Long='%s', Actions='%s'",
                                    bot_name, scope, st, lt, act)
                        return {
                            "short_term_goal": st or prev_short,
                            "long_term_goal": lt or prev_long,
                            "suggested_actions": act or (f"grind in {zone_name}" if zone_name else "attack, loot"),
                            "action_summary": act_sum or f"Advance objectives in {zone_name or 'the field'}."
                        }
        except Exception as e:
            logger.debug("[GOAL] AI goal generation exception, falling back to diverse pool: %s", e)

    # 2. Diverse Non-Repeating Pool Fallback
    pool = DIVERSE_POOLS.get(cls_name, DIVERSE_POOLS.get("Warrior", {}))
    lt_candidates = [g for g in pool.get("long_term", []) if g.lower() != prev_long.lower()]
    if not lt_candidates:
        lt_candidates = pool.get("long_term", ["Master our combat capabilities."])

    loc_suffix = f" in {zone_name}." if zone_name else "."
    st_candidates = [g + loc_suffix for g in pool.get("short_term", []) if (g + loc_suffix).lower() != prev_short.lower()]
    if not st_candidates:
        st_candidates = [g + loc_suffix for g in pool.get("short_term", ["Complete local adventure objectives."])]

    act_candidates = [a for a in pool.get("actions", []) if a.lower() != prev_actions.lower()]
    if not act_candidates:
        act_candidates = pool.get("actions", ["attack nearest target, loot all"])

    # If first time called and no previous goals, use bracket canonical default for unit tests
    if not prev_long and not prev_short:
        lt_map = bracket_data.get("long_term_by_class", {})
        long_goal = lt_map.get(cls_name, lt_map.get("default", lt_candidates[0])) if lt_map else bracket_data.get("long_term", lt_candidates[0])
        st_map = bracket_data.get("short_term_by_class", {})
        if st_map:
            short_goal = st_map.get(cls_name, st_map.get("default", "Complete local quest objectives")) + loc_suffix
        elif "short_term_by_faction" in bracket_data:
            f_map = bracket_data["short_term_by_faction"]
            short_goal = f_map.get(team_name, f_map.get("default", "Explore introductory dungeons and complete class quests."))
        else:
            short_goal = bracket_data.get("short_term", st_candidates[0])
        chosen_action = act_candidates[0]
    else:
        # User pressed suggest repeatedly: select fresh novel entries from diverse pool
        long_goal = prev_long if (scope == "actions" and prev_long) else random.choice(lt_candidates)
        short_goal = prev_short if (scope == "actions" and prev_short) else random.choice(st_candidates)
        chosen_action = random.choice(act_candidates)

    action_summary = f"Engage tactical {cls_name.lower()} routine{loc_suffix[:-1]}."
    # The fallback pool is written as prose for flavour; reduce it to the curated
    # command vocabulary before it is offered as a runnable recommendation.
    chosen_action = ground_suggested_actions(chosen_action, cls_name)
    logger.info("[GOAL] Diverse suggestions selected for %s (Level %d %s): Short='%s', Long='%s', Actions='%s'",
                bot_name, level, cls_name, short_goal, long_goal, chosen_action)

    return {
        "short_term_goal": short_goal,
        "long_term_goal": long_goal,
        "suggested_actions": chosen_action,
        "action_summary": action_summary
    }


def generate_contextual_goals(
    bot_info: Dict[str, Any],
    surroundings: Optional[Dict[str, Any]] = None,
    llm_client: Optional[Any] = None,
    scope: str = "both",
    previous_goals: Optional[Dict[str, str]] = None,
    state: Optional[Dict[str, Any]] = None,
) -> Tuple[str, str]:
    """Generates authentic World of Warcraft goals (short_term, long_term) contextualized
    to the bot's level progression bracket, class specialization, race, faction, and active zone.
    Scope can be 'both', 'long', or 'short'.
    """
    res = generate_contextual_suggestions(
        bot_info=bot_info,
        surroundings=surroundings,
        llm_client=llm_client,
        scope=scope,
        previous_goals=previous_goals,
        state=state,
    )
    return str(res.get("short_term_goal", "")), str(res.get("long_term_goal", ""))
