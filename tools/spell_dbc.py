"""Complete Spell ID -> name index read straight from the client Spell.dbc.

Many AzerothCore repacks ship a partial `spell_dbc` world table (this one carries
4,516 of ~60,000 spells), which leaves most of a character's spellbook without a
name and therefore unmatchable by natural language. The DBC that the server
already loads has every spell, so the bridge reads it once and indexes it.

The name and rank fields are auto-detected by checking known spells rather than
hard-coding an offset, so a patched or customised Spell.dbc still resolves.
"""

import struct
from typing import Dict, Iterable, Optional

_HEADER = struct.Struct("<4sIIII")

# Field index is auto-detected, but the search needs anchors. These three are
# stable in every 3.3.5a Spell.dbc.
_ANCHORS = {133: "Fireball", 116: "Frostbolt", 143: "Fireball"}


class SpellNameIndex:
    """Lazy, read-only view of Spell.dbc: Spell ID -> name and rank text."""

    def __init__(self, path: str):
        self.path = path
        self.count = 0
        self._names: Dict[int, str] = {}
        self._ranks: Dict[int, str] = {}
        self._loaded = False

    def load(self) -> None:
        if self._loaded:
            return
        with open(self.path, "rb") as handle:
            header = handle.read(_HEADER.size)
            if len(header) < _HEADER.size:
                raise ValueError("Spell.dbc is truncated")
            magic, records, fields, record_size, string_size = _HEADER.unpack(header)
            if magic != b"WDBC":
                raise ValueError("Not a WDBC file: %s" % self.path)
            raw = handle.read(records * record_size)
            strings = handle.read(string_size)

        if fields <= 0 or record_size < fields * 4:
            raise ValueError("Unexpected Spell.dbc layout (fields=%d, size=%d)" % (fields, record_size))

        def read_string(record_offset: int, field_index: int) -> str:
            offset = struct.unpack_from("<I", raw, record_offset + field_index * 4)[0]
            if offset <= 0 or offset >= len(strings):
                return ""
            end = strings.find(b"\x00", offset)
            if end == -1:
                end = len(strings)
            return strings[offset:end].decode("utf-8", "replace").strip()

        offsets = {}
        for index in range(records):
            record_offset = index * record_size
            spell_id = struct.unpack_from("<I", raw, record_offset)[0]
            offsets[spell_id] = record_offset

        name_field = self._detect_field(offsets, read_string, fields, _ANCHORS)
        if name_field is None:
            raise ValueError("Could not locate the spell name field in %s" % self.path)
        rank_field = self._detect_field(offsets, read_string, fields, {143: "Rank 2", 133: "Rank 1"})

        for spell_id, record_offset in offsets.items():
            name = read_string(record_offset, name_field)
            if name:
                self._names[spell_id] = name
            if rank_field is not None:
                rank = read_string(record_offset, rank_field)
                if rank:
                    self._ranks[spell_id] = rank

        self.count = len(self._names)
        self._loaded = True

    @staticmethod
    def _detect_field(offsets, read_string, fields, anchors) -> Optional[int]:
        available = {spell_id: expected for spell_id, expected in anchors.items() if spell_id in offsets}
        if not available:
            return None
        for index in range(fields):
            if all(read_string(offsets[spell_id], index) == expected
                   for spell_id, expected in available.items()):
                return index
        return None

    def get(self, spell_id: int) -> Optional[dict]:
        self.load()
        name = self._names.get(int(spell_id))
        if not name:
            return None
        return {"name": name, "rank_text": self._ranks.get(int(spell_id), "")}

    def lookup_many(self, spell_ids: Iterable[int]) -> Dict[int, dict]:
        self.load()
        found = {}
        for spell_id in spell_ids:
            info = self._names.get(int(spell_id))
            if info:
                found[int(spell_id)] = {"name": info, "rank_text": self._ranks.get(int(spell_id), "")}
        return found


def find_spell_dbc(start_paths: Iterable[str]) -> Optional[str]:
    """Return the first existing Spell.dbc among the candidate paths."""
    import os

    for candidate in start_paths:
        if not candidate:
            continue
        path = os.path.abspath(candidate)
        if os.path.isfile(path):
            return path
        probe = os.path.join(path, "Spell.dbc")
        if os.path.isfile(probe):
            return probe
    return None
