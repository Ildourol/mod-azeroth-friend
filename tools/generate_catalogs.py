#!/usr/bin/env python3
"""Generate tools/data/world_catalog.json from Map.dbc and AreaTable.dbc.

Compiles 3.3.5a client map and zone/area definitions into a static, zero-DB,
zero-token JSON catalog for fast spatial resolution.
"""

import json
import os
import struct
import sys

_HEADER = struct.Struct("<4sIIII")


def parse_dbc(path: str):
    with open(path, "rb") as f:
        header = f.read(_HEADER.size)
        if len(header) < _HEADER.size:
            raise ValueError(f"DBC truncated: {path}")
        magic, records, fields, record_size, string_size = _HEADER.unpack(header)
        if magic != b"WDBC":
            raise ValueError(f"Not a WDBC file: {path}")
        raw = f.read(records * record_size)
        strings = f.read(string_size)

    def read_str(offset: int) -> str:
        if offset <= 0 or offset >= len(strings):
            return ""
        end = strings.find(b"\x00", offset)
        if end == -1:
            end = len(strings)
        return strings[offset:end].decode("utf-8", "replace").strip()

    return records, fields, record_size, raw, read_str


def generate_world_catalog(dbc_dir: str, output_path: str):
    map_dbc = os.path.join(dbc_dir, "Map.dbc")
    area_dbc = os.path.join(dbc_dir, "AreaTable.dbc")

    if not os.path.isfile(map_dbc) or not os.path.isfile(area_dbc):
        print(f"Error: Map.dbc or AreaTable.dbc not found in {dbc_dir}", file=sys.stderr)
        return False

    # 1. Parse Map.dbc
    m_records, m_fields, m_rec_size, m_raw, m_read_str = parse_dbc(map_dbc)
    maps = {}
    for idx in range(m_records):
        off = idx * m_rec_size
        map_id = struct.unpack_from("<I", m_raw, off)[0]
        dir_off = struct.unpack_from("<I", m_raw, off + 4)[0]
        name_off = struct.unpack_from("<I", m_raw, off + 20)[0]
        dir_name = m_read_str(dir_off)
        name = m_read_str(name_off)
        if not name:
            name = dir_name or f"Map {map_id}"
        maps[str(map_id)] = {
            "id": map_id,
            "name": name,
            "directory": dir_name
        }

    # 2. Parse AreaTable.dbc
    a_records, a_fields, a_rec_size, a_raw, a_read_str = parse_dbc(area_dbc)
    areas = {}
    for idx in range(a_records):
        off = idx * a_rec_size
        area_id = struct.unpack_from("<I", a_raw, off)[0]
        map_id = struct.unpack_from("<I", a_raw, off + 4)[0]
        parent_id = struct.unpack_from("<I", a_raw, off + 8)[0]
        name_off = struct.unpack_from("<I", a_raw, off + 44)[0]
        name = a_read_str(name_off)
        if not name:
            name = f"Area {area_id}"
        areas[str(area_id)] = {
            "id": area_id,
            "map_id": map_id,
            "parent_id": parent_id,
            "name": name
        }

    catalog = {
        "version": "3.3.5a-12340",
        "counts": {
            "maps": len(maps),
            "areas": len(areas)
        },
        "maps": maps,
        "areas": areas
    }

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(catalog, f, indent=2, ensure_ascii=False)

    print(f"Generated {output_path}: {len(maps)} maps, {len(areas)} areas.")
    return True


if __name__ == "__main__":
    default_dbc = r"C:\Users\Admin\AntigravityProfiles\Projects Azerothcore\Azerothcore server\Server\data\dbc"
    default_out = os.path.join(os.path.dirname(__file__), "data", "world_catalog.json")
    dbc_dir = sys.argv[1] if len(sys.argv) > 1 else default_dbc
    out_path = sys.argv[2] if len(sys.argv) > 2 else default_out
    generate_world_catalog(dbc_dir, out_path)
