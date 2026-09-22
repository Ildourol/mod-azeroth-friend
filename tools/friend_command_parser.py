"""Deterministic natural-language owner commands, resolved straight to spell IDs.

This runs before any model call, so "cast fireball on the wolf" becomes a concrete
Spell ID instantly, with no tokens spent and no chance of the LLM inventing a spell
the companion never learned.

Only the companion's own spellbook is authoritative: every lookup goes through
SpellDatabaseResolver.resolve(..., bot_guid=...) so an unknown or unlearned spell
fails closed instead of producing a cast the server would reject.
"""

import re
from typing import Any, Dict, List, Optional, Tuple

__all__ = ["parse_cast_command", "is_cast_intent", "CAST_VERBS", "parse_slash_intent", "parse_trade_intent", "match_bag_item_request"]

# Leading verbs that mark an explicit cast request.
CAST_VERBS = (
    "cast", "use", "throw", "fire", "invoke", "channel", "activate", "pop", "drop",
    "release", "unleash", "open", "buff",
)

# Markers that introduce the target of the cast.
TARGET_MARKERS = ("onto", "upon", "towards", "toward", "against", "on", "at", "to")

# Target phrases normalised to the keywords the C++ dispatcher understands.
TARGET_ALIASES = {
    "me": "master",
    "for me": "master",
    "myself": "master",
    "master": "master",
    "my master": "master",
    "the master": "master",
    "owner": "master",
    "my owner": "master",
    "self": "self",
    "yourself": "self",
    "you": "self",
    "target": "target",
    "my target": "target",
    "the target": "target",
    "your target": "target",
    "current target": "target",
    "master target": "target",
}

_POLITE_PREFIXES = (
    "can you ", "could you ", "would you ", "will you ", "please ", "pls ", "plz ",
    "i want you to ", "i need you to ", "i'd like you to ", "go ahead and ",
    "try to ", "now ", "quickly ", "quick ", "just ", "then ",
)

_TRAILING_FILLERS = (
    " right now", " right away", " asap", " quickly", " quick", " please", " pls",
    " plz", " now", " then",
)

# Destination spells name their target in the spell itself ("Portal: Stormwind"),
# so "portal to stormwind" must never be split into a spell plus a unit target.
_DESTINATION_RE = re.compile(
    r"^(?:(?:open|make|create|cast|use)\s+)?(?:(?:a|an|the)\s+)?(portal|teleport)\b\s*:?\s*(?:to\s+)?(.*)$",
    re.I)
_INVERTED_DESTINATION_RE = re.compile(
    r"^(?:(?:open|make|create|cast|use)\s+)?(?:(?:a|an|the)\s+)?(.*?)\s+(portal|teleport)$",
    re.I)
_DESTINATION_FAMILIES = ("portal", "teleport")

_RANK_RE = re.compile(r"\(?\s*\b(?:rank|rk|rn|r)\s*(\d+)\s*\)?", re.I)
_ARTICLE_RE = re.compile(r"^(?:the|a|an)\s+", re.I)
_EMPTY_PARENS_RE = re.compile(r"\(\s*\)")


def is_cast_intent(text: str) -> bool:
    """True when the text reads as an explicit request to cast something."""
    lowered = _normalise(text)
    words = lowered.split()
    if not words:
        return False
    if words[0] in CAST_VERBS:
        return True
    if _DESTINATION_RE.match(lowered) or _INVERTED_DESTINATION_RE.match(lowered):
        return True
    return False


def _normalise(text: str) -> str:
    cleaned = (text or "").strip().strip('"\u201c\u201d\'')
    cleaned = cleaned.rstrip("!?.,;: ")
    lowered = cleaned.lower()
    changed = True
    while changed:
        changed = False
        for prefix in _POLITE_PREFIXES:
            if lowered.startswith(prefix):
                lowered = lowered[len(prefix):]
                changed = True
        for suffix in _TRAILING_FILLERS:
            if lowered.endswith(suffix):
                lowered = lowered[: -len(suffix)]
                changed = True
    return lowered.strip()


def _strip_article(name: str) -> str:
    return _ARTICLE_RE.sub("", name).strip()


def _resolve(resolver: Any, name: str, bot_guid: Optional[int], rank: Optional[int]):
    if not name:
        return None
    for candidate in (name, _strip_article(name)):
        if not candidate:
            continue
        try:
            info = resolver.resolve(candidate, bot_guid=bot_guid, rank=rank)
        except TypeError:
            info = resolver.resolve(candidate, bot_guid=bot_guid)
        except Exception:
            info = None
        if info:
            return info
    return None


def _fuzzy_resolve(resolver: Any, query: str, bot_guid: Optional[int], rank: Optional[int]):
    """Match what the player said to a spell they actually know.

    "heal me" should find Lesser Heal, "shield yourself" should find Power Word:
    Shield. Matching stays strictly whole-word (fire never matches Fireball) and
    prefers the shortest matching name, so "heal" picks Heal over Lesser Heal when
    the companion knows both.
    """
    if not query or len(query) < 4:
        return None
    try:
        known = resolver.get_bot_learned_spells(bot_guid) if bot_guid else []
    except Exception:
        known = []

    needle = query.casefold().strip()
    candidates = []
    for spell in known or []:
        name = str(spell.get("name") or "").strip()
        if not name:
            continue
        lowered = name.casefold()
        if re.search(r"\b" + re.escape(needle) + r"\b", lowered) or \
                re.search(r"\b" + re.escape(lowered) + r"\b", needle):
            candidates.append(spell)
    if not candidates:
        return None

    if rank is not None:
        ranked = [s for s in candidates if int(s.get("rank_num") or 0) == rank]
        if ranked:
            candidates = ranked

    best = sorted(
        candidates,
        key=lambda s: (len(str(s.get("name") or "")), -int(s.get("rank_num") or 0)),
    )[0]
    return {
        "spell_id": int(best.get("spell_id") or 0),
        "name": best.get("name"),
        "rank_num": best.get("rank_num"),
    }


def _canonical_target(phrase: str) -> Optional[str]:
    phrase = _strip_article((phrase or "").strip().rstrip("!?.,;:"))
    if not phrase:
        return None
    if phrase in TARGET_ALIASES:
        return TARGET_ALIASES[phrase]
    # A short proper name is passed through; the server resolves it against
    # players and nearby creatures and rejects anything ambiguous.
    if len(phrase.split()) <= 3 and re.fullmatch(r"[A-Za-z'\-\s]+", phrase):
        return _strip_article(phrase)
    return None


def _split_target(remainder: str):
    """Split trailing target phrases, preferring the split that resolves as a spell.

    Returns (spell_part, target_part) where target_part may be None.
    """
    best = (remainder, None)
    for match in reversed(list(re.finditer(r"\b(" + "|".join(TARGET_MARKERS) + r")\b", remainder))):
        left = remainder[:match.start()].strip()
        right = remainder[match.end():].strip()
        target = _canonical_target(right)
        if left and target:
            return left, target
    # "heal me" / "shield yourself" - a bare trailing target word is still a target.
    words = remainder.split()
    for size in (2, 1):
        if len(words) > size:
            tail = " ".join(words[-size:])
            target = _canonical_target(tail) if tail in TARGET_ALIASES else None
            if target:
                return " ".join(words[:-size]).strip(), target
    return best


def _cast_step(info: dict, reference: str, target: Optional[str] = None) -> Dict[str, Any]:
    params: Dict[str, Any] = {
        "spellid": int(info.get("spell_id") or 0),
        "spell": info.get("name") or reference,
    }
    action = "cast"
    if target:
        params["target"] = target
        action = "cast_on"
    return {"action": action, "params": params}


def _resolve_destination(family: str, city: str, rank: Optional[int],
                         resolver: Any, bot_guid: Optional[int]):
    """Resolve Portal:/Teleport: style spells.

    These name their destination inside the spell itself, so the destination must
    never be treated as a unit target. When the owner gives no city and several are
    known, the request is refused with the option list instead of guessing - porting
    the group to the wrong city is worse than asking.
    """
    family = family.capitalize()
    prefix = family + ":"
    city = _strip_article((city or "").strip().rstrip("!?.,;:"))

    try:
        known = resolver.get_bot_learned_spells(bot_guid) if bot_guid else []
    except Exception:
        known = []

    if city:
        for candidate in (f"{prefix} {city.title()}", f"{prefix} {city.capitalize()}",
                          f"{prefix} {city}"):
            info = _resolve(resolver, candidate, bot_guid, rank)
            if info:
                return _cast_step(info, candidate), None
        # Fall back to a substring match on the bot's own portal list.
        entries = [s for s in known
                   if str(s.get("name") or "").startswith(prefix)
                   and city.casefold() in str(s.get("name") or "").casefold()]
        if entries:
            best = sorted(entries, key=lambda s: int(s.get("rank_num") or 0))[-1]
            return _cast_step(best, str(best.get("name"))), None
        return None, "unknown_spell:%s %s" % (family.lower(), city)

    entries = sorted({str(s.get("name")) for s in known
                      if str(s.get("name") or "").startswith(prefix)})
    if len(entries) == 1:
        info = _resolve(resolver, entries[0], bot_guid, rank)
        if info:
            return _cast_step(info, entries[0]), None
    if len(entries) > 1:
        return None, "ambiguous_spell:%s:%s" % (family.lower(), ", ".join(entries))
    return None, None


def parse_cast_command(
    text: str,
    resolver: Any,
    bot_guid: Optional[int] = None,
) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    """Translate natural language into a resolved cast step.

    Returns (step, error):
      step  - {"action": "cast"|"cast_on", "params": {"spellid": int, "spell": str, ...}}
      error - None, or "unknown_spell:<name>" when an explicit cast could not be resolved.
    """
    if not text or not isinstance(text, str):
        return None, None

    lowered = _normalise(text)
    if not lowered:
        return None, None

    words = lowered.split()
    explicit_verb = words[0] in CAST_VERBS
    if explicit_verb:
        lowered = " ".join(words[1:]).strip()
    if not lowered:
        return None, None

    rank = None
    rank_match = _RANK_RE.search(lowered)
    if rank_match:
        rank = int(rank_match.group(1))
        lowered = (lowered[:rank_match.start()] + " " + lowered[rank_match.end():]).strip()
        lowered = _EMPTY_PARENS_RE.sub(" ", lowered).strip()

    destination = _DESTINATION_RE.match(lowered)
    if destination:
        step, error = _resolve_destination(destination.group(1), destination.group(2),
                                           rank, resolver, bot_guid)
        if step or error:
            return step, error
        # "portal" alone with nothing known: fall through to the generic path so the
        # usual unknown-spell handling reports it.

    inverted = _INVERTED_DESTINATION_RE.match(lowered)
    if inverted:
        step, error = _resolve_destination(inverted.group(2), inverted.group(1),
                                           rank, resolver, bot_guid)
        if step or error:
            return step, error

    # Check for "<target> with <spell>" e.g. "me with focus magic"
    with_match = re.match(r"^(.+?)\s+with\s+(.+)$", lowered)
    if with_match:
        potential_target = _canonical_target(with_match.group(1))
        if potential_target:
            cand_spell = with_match.group(2).strip()
            info = _resolve(resolver, cand_spell, bot_guid, rank) or _fuzzy_resolve(resolver, cand_spell, bot_guid, rank)
            if info:
                return _cast_step(info, cand_spell, potential_target), None

    spell_part, target_part = _split_target(lowered)

    info = _resolve(resolver, spell_part, bot_guid, rank)
    if not info and target_part:
        # The marker may have been inside the spell name ("hand of protection on me").
        info = _resolve(resolver, lowered, bot_guid, rank)
        if info:
            spell_part, target_part = lowered, target_part
    if not info:
        info = _fuzzy_resolve(resolver, spell_part, bot_guid, rank)
        if not info:
            info = _fuzzy_resolve(resolver, lowered, bot_guid, rank)
            if info:
                spell_part, target_part = lowered, target_part
    if not info:
        if explicit_verb and spell_part:
            return None, "unknown_spell:" + spell_part
        return None, None

    target = _canonical_target(target_part) if target_part else None
    params: Dict[str, Any] = {
        "spellid": int(info.get("spell_id") or 0),
        "spell": info.get("name") or spell_part,
    }
    if not params["spellid"]:
        return None, "unknown_spell:" + spell_part
    action = "cast"
    if target:
        params["target"] = target
        action = "cast_on"
    return {"action": action, "params": params}, None


# Regex to detect and extract WoW 3.3.5 item hyperlinks:
# e.g. |cff0070dd|Hitem:1234:0:0:0:0:0:0:0:0|h[Super Healing Potion]|h|r
_ITEM_HYPERLINK_RE = re.compile(r'(\|c[0-9a-fA-F]{8}\|Hitem:(\d+):[0-9:\-]+\|h\[([^\]]+)\]\|h\|r)')


def parse_trade_intent(raw: str, speaker: Optional[str] = None, is_trade_active: bool = False) -> Optional[Dict[str, Any]]:
    """Deterministically parses trade-related requests (0 LLM tokens):
    - Right-click / place item into trade: "right click [Item]", "put [Item] in trade", "give me 5 Linen Cloth", "trade [Item]"
    - Clear item from trade: "remove item 1", "remove [Item]", "clear trade"
    - Set gold in trade: "adjust gold to 5g 20s", "set gold 10g", "put 5g in trade", "5g" (when in trade)
    - Link items in chat: "link items", "link potions", "link food", "link gear", "show inventory"
    - Accept / Cancel: "accept trade", "cancel trade", "deal"
    """
    if not raw:
        return None
    cleaned = str(raw).strip()
    has_hyperlink = _ITEM_HYPERLINK_RE.search(cleaned)

    lowered = cleaned.lower()
    lowered = re.sub(r'^[/.!]+', '', lowered).strip()

    for prefix in _POLITE_PREFIXES:
        if lowered.startswith(prefix):
            lowered = lowered[len(prefix):].strip()

    for filler in _TRAILING_FILLERS:
        if lowered.endswith(filler.strip()):
            lowered = lowered[:-len(filler.strip())].strip()

    # 1. Trade Accept / Cancel
    if lowered in ("trade accept", "accept trade", "accept the trade", "deal", "confirm trade"):
        return {"action": "trade_accept", "params": {}}
    if is_trade_active and lowered in ("accept", "ready"):
        return {"action": "trade_accept", "params": {}}

    if lowered in ("trade cancel", "cancel trade", "close trade", "stop trade"):
        return {"action": "trade_cancel", "params": {}}
    if is_trade_active and lowered in ("cancel", "close", "stop"):
        return {"action": "trade_cancel", "params": {}}

    # 2. Link items in chat
    link_match = re.match(
        r'^(?:link|show|list)\s+(?:me\s+)?(?:your\s+)?(items|inventory|bags|potions?|elixirs?|food|drinks?|consumables?|gear|equipment|armor|weapons?|trade\s*goods|mats|materials?|reagents?|all)$',
        lowered
    )
    if link_match:
        cat_word = link_match.group(1).replace(" ", "_")
        if cat_word in ("potion", "potions", "elixir", "elixirs"):
            cat = "potion"
        elif cat_word in ("food", "drink", "drinks", "consumable", "consumables"):
            cat = "consumable"
        elif cat_word in ("gear", "equipment", "armor", "weapon", "weapons"):
            cat = "equipment"
        elif cat_word in ("trade_goods", "mats", "materials", "material", "reagents", "reagent"):
            cat = "trade_goods"
        else:
            cat = "all"
        return {"action": "trade_link_items", "params": {"category": cat}}

    if lowered in ("what do you have", "what do you have?", "what have you got", "what's in your bags", "whats in your bags"):
        return {"action": "trade_link_items", "params": {"category": "all"}}

    # 3. Gold adjustment in trade
    gold_set_match = re.match(
        r'^(?:(?:adjust|set|put|offer|change|trade|give)(?:\s+(?:the|me))?\s+)?(?:trade\s+)?(?:gold|money)\s+(?:to\s+)?(\d+.*)$',
        lowered
    )
    if gold_set_match:
        val = gold_set_match.group(1).strip()
        val = re.sub(r'(\d+)\s*gold\b', r'\1g', val)
        val = re.sub(r'(\d+)\s*silver\b', r'\1s', val)
        val = re.sub(r'(\d+)\s*copper\b', r'\1c', val)
        return {"action": "trade_set_gold", "params": {"gold": val}}

    # Pattern 3B: "trade me 5 gold", "give me 5 gold", "put 5 gold", "trade 5 gold", "offer 5g", "5 gold"
    money_words_match = re.match(
        r'^(?:(?:adjust|set|put|offer|change|trade|give)(?:\s+(?:the|me))?\s+)?(\d+)\s*(?:gold|g)(?:\s*(\d+)\s*(?:silver|s))?(?:\s*(\d+)\s*(?:copper|c))?(?:\s+in\s+trade)?$',
        lowered
    )
    if money_words_match and (is_trade_active or lowered.startswith(("adjust", "set", "put", "offer", "trade", "give")) or "in trade" in lowered or "gold" in lowered):
        g = money_words_match.group(1)
        s = money_words_match.group(2)
        c = money_words_match.group(3)
        formatted = f"{g}g"
        if s:
            formatted += f" {s}s"
        if c:
            formatted += f" {c}c"
        return {"action": "trade_set_gold", "params": {"gold": formatted}}

    # Standalone money expression: e.g. "5g", "5g 20s", "10g 50s 25c", "put 5g", "put 5g in trade", "give me 5g"
    pure_money_match = re.match(
        r'^(?:(?:put|offer|trade|give)(?:\s+me)?\s+)?(\d+\s*g(?:\s*\d+\s*s)?(?:\s*\d+\s*c)?)(?:\s+in\s+trade)?$',
        lowered
    )
    if pure_money_match:
        val = pure_money_match.group(1).strip()
        if is_trade_active or lowered.startswith(("put ", "offer ", "trade ", "give ")) or "in trade" in lowered:
            return {"action": "trade_set_gold", "params": {"gold": val}}

    # 4. Clear item from trade
    clear_match = re.match(r'^(?:remove|clear|take\s+back|take\s+off)\s+(?:item\s+)?(?:slot\s+)?(\d+)$', lowered)
    if clear_match:
        return {"action": "trade_clear_item", "params": {"slot": int(clear_match.group(1))}}

    clear_named_match = re.match(r'^(?:remove|take\s+back|take\s+off)\s+(?:the\s+)?(.+)$', cleaned, re.IGNORECASE)
    if clear_named_match:
        target_item = clear_named_match.group(1).strip()
        if target_item.lower() not in ("it", "trade", "all"):
            return {"action": "trade_clear_item", "params": {"item": target_item}}

    if lowered in ("clear trade", "clear items", "clear trade items"):
        return {"action": "trade_clear_item", "params": {}}

    # 5. Right-click / place item into trade
    # Pattern A: "right click [Item]" or "right click on the potion"
    rc_match = re.match(r'^(?:right\s*click|rc)\s+(?:on\s+)?(?:the\s+)?(.+)$', cleaned, re.IGNORECASE)
    if rc_match:
        item_text = rc_match.group(1).strip()
        count_match = re.match(r'^(\d+)\s+(?:x\s+)?(.+)$', item_text)
        if count_match:
            count = int(count_match.group(1))
            item_name = count_match.group(2).strip()
            return {"action": "trade_set_item", "params": {"item": item_name, "count": count}}
        return {"action": "trade_set_item", "params": {"item": item_text}}

    # Pattern B: "put [Item] in trade", "put 5 Linen Cloth", "trade 5 Linen Cloth", "give me 5 [Linen Cloth]", "add [Item]", "trade me a shiny apple"
    put_match = re.match(r'^(?:put|add|offer|trade|give)(?:\s+me)?\s+(?:in\s+trade\s+)?(?:the\s+|a\s+|an\s+)?(.+)$', cleaned, re.IGNORECASE)
    if put_match:
        rest = put_match.group(1).strip()
        rest_lower = rest.lower()
        if rest_lower in ("leader", "lead", "party leader", "raid leader"):
            return {"action": "give_leader", "params": {}}
        if rest_lower.startswith(("gold", "money")):
            return None
        if rest_lower in ("with me", "master", "me") and lowered.startswith("trade"):
            return {"action": "trade", "params": {"target": speaker or "master"}}

        # Check if the rest is actually money e.g. "5 gold", "5g" or "5g in trade"
        money_cand = re.sub(r'\s+in\s+trade$', '', rest_lower).strip()
        if re.search(r'^\d+\s*(?:gold|silver|copper|g|s|c)', money_cand):
            norm = re.sub(r'(\d+)\s*gold\b', r'\1g', money_cand)
            norm = re.sub(r'(\d+)\s*silver\b', r'\1s', norm)
            norm = re.sub(r'(\d+)\s*copper\b', r'\1c', norm)
            return {"action": "trade_set_gold", "params": {"gold": norm}}

        count_match = re.match(r'^(\d+)\s+(?:x\s+)?(.+)$', rest)
        if count_match:
            count = int(count_match.group(1))
            item_name = count_match.group(2).strip()
            if not re.match(r'^[gsc]\b', item_name, re.IGNORECASE):
                item_name = re.sub(r'\s+in\s+trade$', '', item_name, flags=re.IGNORECASE).strip()
                item_name = re.sub(r'^(?:the\s+|a\s+|an\s+)', '', item_name, flags=re.IGNORECASE).strip()
                return {"action": "trade_set_item", "params": {"item": item_name, "count": count}}
        else:
            item_name = re.sub(r'\s+in\s+trade$', '', rest, flags=re.IGNORECASE).strip()
            item_name = re.sub(r'^(?:the\s+|a\s+|an\s+)', '', item_name, flags=re.IGNORECASE).strip()
            if has_hyperlink or item_name.startswith("[") or lowered.startswith(("put ", "add ", "offer ", "trade ", "give ")) or is_trade_active:
                return {"action": "trade_set_item", "params": {"item": item_name}}

    # If in active trade and the player whispers an item link directly: e.g. "|cff...[Item]|r" or "[Item Name]"
    if is_trade_active and (has_hyperlink or (cleaned.startswith("[") and cleaned.endswith("]"))):
        return {"action": "trade_set_item", "params": {"item": cleaned}}

    return None


def parse_slash_intent(raw: str, speaker: Optional[str] = None, is_trade_active: bool = False) -> Optional[Dict[str, Any]]:
    """Deterministically parses common slash command requests (trade, duel, inspect, follow, etc.).
    Returns an action dict if recognized, or None.
    """
    if not raw:
        return None

    # Check trade intents first (covers item placement, gold adjustment, linking, accept/cancel)
    trade_step = parse_trade_intent(raw, speaker=speaker, is_trade_active=is_trade_active)
    if trade_step:
        return trade_step

    cleaned = str(raw).strip().lower()
    cleaned = re.sub(r'^[/.!]+', '', cleaned).strip()

    for prefix in _POLITE_PREFIXES:
        if cleaned.startswith(prefix):
            cleaned = cleaned[len(prefix):].strip()

    for filler in _TRAILING_FILLERS:
        if cleaned.endswith(filler.strip()):
            cleaned = cleaned[:-len(filler.strip())].strip()

    # Action Mode intents
    mode_match = re.match(r'^(?:set\s+)?(?:action\s+)?mode\s+(combat|travel|idle|social|grind|follow|stay|rest|rpg)$', cleaned)
    if not mode_match:
        mode_match = re.match(r'^(combat|travel|idle|social|grind|stay|rest|follow|rpg)\s+mode$', cleaned)
    if mode_match:
        m = mode_match.group(1).lower()
        if m == "grind": m = "combat"
        elif m == "follow": m = "travel"
        elif m in ("stay", "rest"): m = "idle"
        elif m == "rpg": m = "social"
        return {"action": "set_action_mode", "params": {"mode": m}}

    # Trade intent
    if cleaned in ("trade", "trade with me", "open trade", "let's trade", "lets trade", "tr"):
        return {"action": "trade", "params": {"target": speaker or "master"}}
    trade_match = re.match(r'^(?:trade|open trade|trade with)\s+([a-zA-Z0-9_\-]+)$', cleaned)
    if trade_match:
        target = trade_match.group(1).strip()
        if target in ("me", "myself", "master"):
            target = speaker or "master"
        return {"action": "trade", "params": {"target": target}}

    # Duel intent
    if cleaned in ("duel", "duel me", "challenge me", "let's duel", "lets duel"):
        return {"action": "duel_start", "params": {"target": speaker or "master"}}
    duel_match = re.match(r'^(?:duel|challenge)\s+([a-zA-Z0-9_\-]+)$', cleaned)
    if duel_match:
        target = duel_match.group(1).strip()
        if target in ("me", "myself", "master"):
            target = speaker or "master"
        return {"action": "duel_start", "params": {"target": target}}

    # Inspect intent
    if cleaned in ("inspect", "inspect me", "inspect gear", "ins"):
        return {"action": "inspect", "params": {"target": speaker or "master"}}
    inspect_match = re.match(r'^(?:inspect|ins)\s+([a-zA-Z0-9_\-]+)$', cleaned)
    if inspect_match:
        target = inspect_match.group(1).strip()
        if target in ("me", "myself", "master", "gear"):
            target = speaker or "master"
        return {"action": "inspect", "params": {"target": target}}

    # Target intent
    tar_match = re.match(r'^(?:target|tar)\s+(.+)$', cleaned)
    if tar_match:
        target = tar_match.group(1).strip()
        if target in ("me", "myself", "master"):
            target = speaker or "master"
        return {"action": "target", "params": {"target": target}}

    # Follow intent
    if cleaned in ("follow", "follow me", "fol", "come with me"):
        return {"action": "follow", "params": {}}

    # Assist intent
    if cleaned in ("assist", "assist me"):
        return {"action": "assist", "params": {}}

    # Hold / Stop intent
    if cleaned in ("stay", "hold", "stop", "halt", "wait"):
        return {"action": "stay", "params": {}}

    # Dismount intent
    if cleaned in ("dismount", "get off mount", "dismount now"):
        return {"action": "dismount", "params": {}}

    # Mount intent
    if cleaned in ("mount", "mount up"):
        return {"action": "mount", "params": {}}

    # PvP toggle intent
    if cleaned in ("pvp", "toggle pvp", "pvp on", "enable pvp"):
        return {"action": "pvp", "params": {}}

    return None


def match_bag_item_request(
    text: str, inventory: List[Dict[str, Any]]
) -> Optional[Tuple[Dict[str, Any], int]]:
    """Match a natural language request (item name, typo, category) against the bot's carried bag items.

    Returns (matched_item_dict, count) or None.
    """
    if not text or not inventory:
        return None

    cleaned = str(text).strip()
    cleaned = re.sub(r'^[/.!]+', '', cleaned).strip()
    for prefix in _POLITE_PREFIXES:
        if cleaned.lower().startswith(prefix):
            cleaned = cleaned[len(prefix):].strip()
    for filler in _TRAILING_FILLERS:
        if cleaned.lower().endswith(filler.strip()):
            cleaned = cleaned[:-len(filler.strip())].strip()

    lowered = cleaned.lower()
    for trigger in (
        "trade me a ", "trade me an ", "trade me some ", "trade me ",
        "give me a ", "give me an ", "give me some ", "give me ",
        "hand me a ", "hand me an ", "hand me some ", "hand me ",
        "put in trade a ", "put in trade an ", "put in trade ",
        "put ", "offer ", "add ", "trade ", "give ",
    ):
        if lowered.startswith(trigger):
            cleaned = cleaned[len(trigger):].strip()
            lowered = cleaned.lower()
            break

    if lowered.endswith(" in trade"):
        cleaned = cleaned[:-9].strip()
        lowered = cleaned.lower()

    count = 1
    count_m = re.match(r"^(\d+)\s+(?:x\s+)?(.+)$", cleaned)
    if count_m:
        count = max(1, int(count_m.group(1)))
        cleaned = count_m.group(2).strip()
        lowered = cleaned.lower()

    if lowered.startswith(("a ", "an ", "the ", "some ")):
        cleaned = cleaned.split(" ", 1)[1].strip()
        lowered = cleaned.lower()

    if cleaned.startswith("[") and cleaned.endswith("]"):
        cleaned = cleaned[1:-1].strip()
        lowered = cleaned.lower()

    if not lowered:
        return None

    if lowered in ("gold", "money", "follow", "stay", "stop", "attack", "flee", "loot", "accept", "cancel", "deal", "yes", "no"):
        return None
    if re.search(r'^\d+\s*(?:gold|silver|copper|g|s|c)', lowered):
        return None

    for it in inventory:
        if it.get("name", "").lower() == lowered:
            return it, min(count, int(it.get("count", 1)))

    for it in inventory:
        if lowered in it.get("name", "").lower():
            return it, min(count, int(it.get("count", 1)))

    query_words = set(lowered.split())
    if len(query_words) > 1:
        for it in inventory:
            name_words = set(it.get("name", "").lower().split())
            if query_words.issubset(name_words):
                return it, min(count, int(it.get("count", 1)))

    singular = lowered[:-1] if (lowered.endswith("s") and not lowered.endswith("ss")) else lowered
    if singular in ("meat", "food", "eat", "bread", "fish", "fruit"):
        for it in inventory:
            nm = it.get("name", "").lower()
            if any(k in nm for k in ("meat", "bread", "fish", "egg", "apple", "cheese", "roast", "steak", "mutton", "liver")):
                return it, min(count, int(it.get("count", 1)))
            if it.get("item_class") == 0 and it.get("item_subclass") == 5:
                return it, min(count, int(it.get("count", 1)))

    if singular in ("water", "drink", "milk", "juice", "tea"):
        for it in inventory:
            nm = it.get("name", "").lower()
            if any(k in nm for k in ("water", "milk", "juice", "tea", "drink", "ale")):
                return it, min(count, int(it.get("count", 1)))

    if singular in ("potion", "pot", "heal", "mana"):
        for it in inventory:
            nm = it.get("name", "").lower()
            if "potion" in nm:
                return it, min(count, int(it.get("count", 1)))

    if singular in ("cloth",):
        for it in inventory:
            if "cloth" in it.get("name", "").lower():
                return it, min(count, int(it.get("count", 1)))

    if singular in ("herb", "flower"):
        for it in inventory:
            if it.get("item_class") == 7 and it.get("item_subclass") == 9:
                return it, min(count, int(it.get("count", 1)))

    import difflib
    names = [it.get("name", "") for it in inventory if it.get("name")]
    close = difflib.get_close_matches(lowered, [n.lower() for n in names], n=1, cutoff=0.60)
    if not close and singular != lowered:
        close = difflib.get_close_matches(singular, [n.lower() for n in names], n=1, cutoff=0.60)
    if close:
        for it in inventory:
            if it.get("name", "").lower() == close[0]:
                return it, min(count, int(it.get("count", 1)))

    return None

