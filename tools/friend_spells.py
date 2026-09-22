"""AzerothFriend Spell Database Resolver.

Provides high-speed, multi-tier resolution between spell names, ranks, and numeric Spell IDs:
- Tier 1: In-memory L1 cache.
- Tier 2: Bot-specific learned spells (character_spell).
- Tier 3: Live AzerothCore MySQL database (azeroth_friend_spells / spell_dbc).
- Tier 4: Bundled offline catalog (tools/data/spell_catalog.json).
"""

import json
import logging
import os
import re
from typing import Any, Dict, List, Optional, Tuple

logger = logging.getLogger("azeroth_friend.spells")

_GLOBAL_RESOLVER: Optional["SpellDatabaseResolver"] = None


class SpellDatabaseResolver:
    def __init__(self, db_mgr: Optional[Any] = None, catalog_path: Optional[str] = None,
                 enabled: bool = True):
        self.db_mgr = db_mgr
        self.enabled = bool(enabled)
        self._l1_name_cache: Dict[Tuple[str, Optional[int]], Dict[str, Any]] = {}
        self._l1_id_cache: Dict[int, Dict[str, Any]] = {}
        self._bot_spell_cache: Dict[int, List[Dict[str, Any]]] = {}

        if catalog_path is None:
            catalog_path = os.path.join(os.path.dirname(__file__), "data", "spell_catalog.json")
        self.catalog_path = catalog_path
        self._catalog: Dict[str, List[Dict[str, Any]]] = {}
        if self.enabled:
            self._load_bundled_catalog()

    @classmethod
    def get_instance(cls, db_mgr: Optional[Any] = None,
                     enabled: Optional[bool] = None) -> "SpellDatabaseResolver":
        global _GLOBAL_RESOLVER
        if _GLOBAL_RESOLVER is None:
            _GLOBAL_RESOLVER = cls(db_mgr=db_mgr, enabled=True if enabled is None else enabled)
        else:
            if db_mgr is not None and _GLOBAL_RESOLVER.db_mgr is None:
                _GLOBAL_RESOLVER.db_mgr = db_mgr
            if enabled is not None:
                _GLOBAL_RESOLVER.set_enabled(enabled)
        return _GLOBAL_RESOLVER

    def set_enabled(self, enabled: bool) -> None:
        """Toggle AzerothFriend.SpellSync.Enable at runtime.

        Disabling drops the supplementary lookup layers (bundled offline catalog and
        AzerothCore spell tables). Spells learned from the bot's live spellbook context
        keep resolving, so casting never depends on the sync layers being warm.
        """
        was_enabled = self.enabled
        self.enabled = bool(enabled)
        if self.enabled and not was_enabled and not self._catalog:
            self._load_bundled_catalog()

    def _load_bundled_catalog(self) -> None:
        if not self.enabled:
            return
        if os.path.exists(self.catalog_path):
            try:
                with open(self.catalog_path, "r", encoding="utf-8") as f:
                    self._catalog = json.load(f)
                logger.debug("Loaded bundled spell catalog with %d entries", len(self._catalog))
            except Exception as e:
                logger.warning("Failed to load bundled spell catalog: %s", e)
                self._catalog = {}

    def _clean_name_and_rank(self, reference: str) -> Tuple[str, Optional[int]]:
        """Separate spell name and optional rank, e.g. 'Fireball (Rank 2)' -> ('fireball', 2)."""
        clean = reference.strip()
        rank_match = re.search(r"\(?rank\s*(\d+)\)?", clean, re.IGNORECASE)
        rank: Optional[int] = None
        if rank_match:
            try:
                rank = int(rank_match.group(1))
            except ValueError:
                rank = None
            clean = re.sub(r"\(?rank\s*\d+\)?", "", clean, flags=re.IGNORECASE).strip()
        # Remove trailing punctuation or whitespace
        clean = re.sub(r"[\(\):]+$", "", clean).strip().lower()
        return clean, rank

    def update_bot_spells_from_context(self, bot_guid: int, combat_spells: List[Dict[str, Any]]) -> None:
        """Cache bot spells emitted from C++ self-context."""
        if not bot_guid or not isinstance(combat_spells, list):
            return
        spells = []
        for s in combat_spells:
            if not isinstance(s, dict):
                continue
            spell_id = int(s.get("id") or s.get("spell_id") or 0)
            name = str(s.get("name", "")).strip()
            rank_text = str(s.get("rank", "")).strip()
            rank_num = int(s.get("rank_num") or 0)
            if not rank_num and rank_text:
                m = re.search(r"\d+", rank_text)
                if m:
                    rank_num = int(m.group(0))
            if spell_id > 0:
                spells.append({
                    "spell_id": spell_id,
                    "name": name,
                    "rank_text": rank_text,
                    "rank_num": rank_num,
                    "mana_cost": int(s.get("mana") or s.get("mana_cost") or 0),
                    "cast_time_ms": int(s.get("cast_time") or s.get("cast_time_ms") or 0),
                    "cooldown_ms": int(s.get("cd") or s.get("cooldown_ms") or 0),
                    "learned": True,
                })
        self._bot_spell_cache[bot_guid] = spells

    def get_bot_learned_spells(self, bot_guid: int) -> List[Dict[str, Any]]:
        """Retrieve learned spells for a bot character, checking cache then database."""
        if not bot_guid:
            return []
        if bot_guid in self._bot_spell_cache and self._bot_spell_cache[bot_guid]:
            return self._bot_spell_cache[bot_guid]

        if self.enabled and self.db_mgr:
            try:
                db_spells = self.db_mgr.fetch_bot_learned_spells(bot_guid)
                if db_spells:
                    normalized = []
                    for s in db_spells:
                        spell_id = int(s.get("spell") or s.get("spell_id") or 0)
                        name = str(s.get("name", "")).strip()
                        if not name and spell_id > 0:
                            # Try looking up name from id cache/catalog
                            info = self.resolve_by_id(spell_id)
                            if info:
                                name = info.get("name", "")
                        rank_text = str(s.get("rank_text", "")).strip()
                        rank_match = re.search(r'\d+', rank_text)
                        normalized.append({
                            "spell_id": spell_id,
                            "name": name,
                            "rank_text": str(s.get("rank_text", "")).strip(),
                            "rank_num": int(s.get("rank_num") or (rank_match.group() if rank_match else 0)),
                            "mana_cost": int(s.get("mana_cost") or 0),
                            "cooldown_ms": int(s.get("cooldown_ms") or 0),
                            "cast_time_ms": int(s.get("cast_time_ms") or 0),
                            "learned": True,
                        })
                    self._bot_spell_cache[bot_guid] = normalized
                    return normalized
            except Exception as e:
                logger.debug("Database fetch_bot_learned_spells failed for bot %d: %s", bot_guid, e)

        return self._bot_spell_cache.get(bot_guid, [])

    def resolve_by_id(self, spell_id: int) -> Optional[Dict[str, Any]]:
        """Look up spell info by exact Spell ID."""
        if not spell_id or spell_id <= 0:
            return None
        if spell_id in self._l1_id_cache:
            return self._l1_id_cache[spell_id]

        # 1. Check bundled catalog
        if self.enabled:
            for _, rank_list in self._catalog.items():
                for sp in rank_list:
                    if int(sp.get("spell_id", 0)) == spell_id:
                        self._l1_id_cache[spell_id] = sp
                        return sp

        # 2. Check Database
        if self.enabled and self.db_mgr:
            try:
                db_res = self.db_mgr.query_spell_by_id(spell_id)
                if db_res:
                    sp = {
                        "spell_id": int(db_res.get("spell_id") or spell_id),
                        "name": str(db_res.get("name", "")).strip(),
                        "rank_text": str(db_res.get("rank_text", "")).strip(),
                        "rank_num": int(db_res.get("rank_num") or 0),
                        "mana_cost": int(db_res.get("mana_cost") or 0),
                        "cooldown_ms": int(db_res.get("cooldown_ms") or 0),
                        "cast_time_ms": int(db_res.get("cast_time_ms") or 0),
                    }
                    self._l1_id_cache[spell_id] = sp
                    return sp
            except Exception as e:
                logger.debug("Database query_spell_by_id failed for %d: %s", spell_id, e)

        # 3. Client Spell.dbc index (complete, unlike a partial spell_dbc table)
        index = getattr(self.db_mgr, "spell_index", None) if self.db_mgr else None
        if index is not None:
            try:
                info = index.get(spell_id)
            except Exception as e:
                logger.debug("Spell.dbc lookup failed for %d: %s", spell_id, e)
                info = None
            if info:
                sp = {
                    "spell_id": spell_id,
                    "name": info.get("name", ""),
                    "rank_text": info.get("rank_text", ""),
                    "rank_num": 0,
                    "mana_cost": 0,
                    "cooldown_ms": 0,
                    "cast_time_ms": 0,
                }
                rank_match = re.search(r"\d+", sp["rank_text"])
                if rank_match:
                    sp["rank_num"] = int(rank_match.group())
                self._l1_id_cache[spell_id] = sp
                return sp

        return None

    def resolve(
        self,
        spell_reference: Any,
        bot_guid: Optional[int] = None,
        rank: Optional[int] = None,
    ) -> Optional[Dict[str, Any]]:
        """Resolve a spell reference (numeric ID or string name) to full SpellInfo.

        Prioritizes:
        1. If numeric: ID lookup.
        2. If bot_guid is given: checks bot's learned spells (highest rank or requested rank).
        3. Checks in-memory L1 cache.
        4. Queries MySQL (azeroth_friend_spells / spell_dbc).
        5. Falls back to bundled offline catalog.
        """
        if spell_reference is None:
            return None

        if bot_guid:
            learned = self.get_bot_learned_spells(bot_guid)
            reference = str(spell_reference).strip()
            if reference.isdigit():
                return next((dict(s) for s in learned if s['spell_id'] == int(reference)), None)
            name, parsed = self._clean_name_and_rank(reference)
            requested = rank if rank is not None else parsed
            matches = [s for s in learned if s.get('name', '').casefold() == name.casefold()]
            if requested is not None:
                matches = [s for s in matches if int(s.get('rank_num', 0)) == requested]
            return dict(max(matches, key=lambda s: int(s.get('rank_num', 0)))) if matches else None

        # Handle numeric ID
        if isinstance(spell_reference, int):
            return self.resolve_by_id(spell_reference)
        ref_str = str(spell_reference).strip()
        if not ref_str:
            return None
        if ref_str.isdigit():
            return self.resolve_by_id(int(ref_str))

        clean_name, parsed_rank = self._clean_name_and_rank(ref_str)
        target_rank = rank if rank is not None else parsed_rank

        # Check Bot's Learned Spells first
        if bot_guid:
            learned = self.get_bot_learned_spells(bot_guid)
            matches = [s for s in learned if s.get("name", "").lower() == clean_name]
            if matches:
                if target_rank:
                    for sp in matches:
                        if sp.get("rank_num") == target_rank:
                            return dict(sp)
                # Default to highest rank learned
                sorted_matches = sorted(matches, key=lambda s: int(s.get("rank_num", 0)), reverse=True)
                return dict(sorted_matches[0])

        # Check L1 Cache
        cache_key = (clean_name, target_rank)
        if cache_key in self._l1_name_cache:
            return dict(self._l1_name_cache[cache_key])

        # Check Database
        if self.enabled and self.db_mgr:
            try:
                db_res = self.db_mgr.query_spell_by_name(clean_name, rank=target_rank)
                if db_res:
                    sp = {
                        "spell_id": int(db_res.get("spell_id") or 0),
                        "name": str(db_res.get("name", "")).strip(),
                        "rank_text": str(db_res.get("rank_text", "")).strip(),
                        "rank_num": int(db_res.get("rank_num") or 0),
                        "mana_cost": int(db_res.get("mana_cost") or 0),
                        "cooldown_ms": int(db_res.get("cooldown_ms") or 0),
                        "cast_time_ms": int(db_res.get("cast_time_ms") or 0),
                        "learned": False,
                    }
                    if sp["spell_id"] > 0:
                        self._l1_name_cache[cache_key] = sp
                        self._l1_id_cache[sp["spell_id"]] = sp
                        return dict(sp)
            except Exception as e:
                logger.debug("Database query_spell_by_name failed for '%s': %s", clean_name, e)

        # Check Bundled Catalog
        if self.enabled and clean_name in self._catalog:
            rank_list = self._catalog[clean_name]
            if rank_list:
                if target_rank:
                    for sp in rank_list:
                        if int(sp.get("rank_num", 0)) == target_rank:
                            self._l1_name_cache[cache_key] = sp
                            return dict(sp)
                # Default to highest rank
                sorted_ranks = sorted(rank_list, key=lambda s: int(s.get("rank_num", 0)), reverse=True)
                highest = sorted_ranks[0]
                self._l1_name_cache[cache_key] = highest
                return dict(highest)

        return None

    def format_spellbook_for_prompt(
        self, bot_guid: int, learned_spells: Optional[List[Dict[str, Any]]] = None
    ) -> str:
        """Format learned spell list into an actionable markdown list for the LLM prompt."""
        spells = learned_spells if learned_spells is not None else self.get_bot_learned_spells(bot_guid)
        if not spells:
            return ""

        lines = ["### BOT LEARNED SPELLS (Can be cast by ID or Name):"]
        seen_names = set()
        # Sort spells: prioritize named spells, highest rank first
        for sp in sorted(spells, key=lambda s: (s.get("name", ""), -int(s.get("rank_num", 0)))):
            name = sp.get("name", "").strip()
            if not name:
                continue
            spell_id = sp.get("spell_id", 0)
            rank = sp.get("rank_text", "").strip() or (f"Rank {sp.get('rank_num')}" if sp.get("rank_num") else "")
            mana = sp.get("mana_cost", 0)
            cost_str = f"{mana} mana" if mana > 0 else ""
            cast_ms = sp.get("cast_time_ms", 0)
            cast_str = f"{cast_ms/1000.0:.1f}s cast" if cast_ms > 0 else "instant"

            details = [d for d in (rank, cost_str, cast_str) if d]
            detail_str = f" ({', '.join(details)})" if details else ""
            lines.append(f"- [{spell_id}] {name}{detail_str}")
            seen_names.add(name.lower())

        return "\n".join(lines)
