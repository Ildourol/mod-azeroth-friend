"""Offline Client Data Catalogs & Deterministic Spatial Math.

Provides zero-database, zero-token resolution for WoW 3.3.5a maps, zones,
subzones, and deterministic 2D/3D spatial geometry calculations.
Adapted from agent-wow architectural innovations (MAF-060).
"""

import json
import math
import os
from typing import Any, Dict, List, Optional, Tuple, Union

Point3D = Union[Tuple[float, float, float], List[float], Dict[str, float]]
Point2D = Union[Tuple[float, float], List[float], Dict[str, float]]


# =============================================================================
# SPATIAL VECTOR MATHEMATICS
# =============================================================================

def _extract_xyz(point: Point3D) -> Tuple[float, float, float]:
    if isinstance(point, (tuple, list)):
        x = float(point[0])
        y = float(point[1])
        z = float(point[2]) if len(point) > 2 else 0.0
        return x, y, z
    if isinstance(point, dict):
        x = float(point.get("x") or point.get("pos_x") or 0.0)
        y = float(point.get("y") or point.get("pos_y") or 0.0)
        z = float(point.get("z") or point.get("pos_z") or 0.0)
        return x, y, z
    return 0.0, 0.0, 0.0


def calculate_distance_3d(p1: Point3D, p2: Point3D) -> float:
    """Calculate exact Euclidean distance between two 3D points."""
    x1, y1, z1 = _extract_xyz(p1)
    x2, y2, z2 = _extract_xyz(p2)
    return math.sqrt((x2 - x1) ** 2 + (y2 - y1) ** 2 + (z2 - z1) ** 2)


def calculate_distance_2d(p1: Point2D, p2: Point2D) -> float:
    """Calculate 2D horizontal Euclidean distance between two points."""
    x1, y1, _ = _extract_xyz(p1)
    x2, y2, _ = _extract_xyz(p2)
    return math.sqrt((x2 - x1) ** 2 + (y2 - y1) ** 2)


def is_within_range_3d(p1: Point3D, p2: Point3D, max_dist: float) -> bool:
    """Fast squared distance comparison avoiding square root."""
    x1, y1, z1 = _extract_xyz(p1)
    x2, y2, z2 = _extract_xyz(p2)
    sq_dist = (x2 - x1) ** 2 + (y2 - y1) ** 2 + (z2 - z1) ** 2
    return sq_dist <= (max_dist ** 2)


def calculate_bearing(from_pt: Point2D, to_pt: Point2D) -> float:
    """Calculate heading/bearing in radians (-pi to +pi) from point 1 to point 2."""
    x1, y1, _ = _extract_xyz(from_pt)
    x2, y2, _ = _extract_xyz(to_pt)
    dx = x2 - x1
    dy = y2 - y1
    return math.atan2(dy, dx)


def format_coordinates(x: float, y: float, z: float = 0.0) -> str:
    """Format coordinates as a readable string."""
    return f"(X: {x:.1f}, Y: {y:.1f}, Z: {z:.1f})"


# =============================================================================
# OFFLINE WORLD & ZONE CATALOG
# =============================================================================

class WorldCatalog:
    """Offline cache of 3.3.5a Maps and AreaTable entries."""
    _instance: Optional["WorldCatalog"] = None

    def __init__(self, catalog_path: Optional[str] = None):
        if catalog_path is None:
            catalog_path = os.path.join(os.path.dirname(__file__), "data", "world_catalog.json")
        self.catalog_path = catalog_path
        self._maps: Dict[int, Dict[str, Any]] = {}
        self._areas: Dict[int, Dict[str, Any]] = {}
        self._loaded = False
        self.load()

    @classmethod
    def get_instance(cls) -> "WorldCatalog":
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    def load(self) -> None:
        if self._loaded or not os.path.isfile(self.catalog_path):
            return
        try:
            with open(self.catalog_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            raw_maps = data.get("maps", {})
            for k, v in raw_maps.items():
                self._maps[int(k)] = v
            raw_areas = data.get("areas", {})
            for k, v in raw_areas.items():
                self._areas[int(k)] = v
            self._loaded = True
        except Exception:
            self._loaded = False

    def get_map_name(self, map_id: int) -> str:
        """Resolve Map ID to human-readable map name."""
        info = self._maps.get(int(map_id))
        if info:
            return info.get("name") or f"Map {map_id}"
        return f"Map {map_id}"

    def get_area_name(self, area_id: int) -> str:
        """Resolve Area ID to area/zone name."""
        info = self._areas.get(int(area_id))
        if info:
            return info.get("name") or f"Area {area_id}"
        return f"Area {area_id}"

    def get_area_info(self, area_id: int) -> Optional[Dict[str, Any]]:
        return self._areas.get(int(area_id))

    def format_location(self, map_id: int, area_id: int) -> str:
        """Format hierarchical location: 'Zone (Subzone)' or 'Map Name'."""
        area_info = self._areas.get(int(area_id))
        if not area_info:
            return self.get_map_name(map_id)

        area_name = area_info.get("name", "")
        parent_id = area_info.get("parent_id", 0)

        if parent_id > 0 and parent_id != area_id:
            parent_info = self._areas.get(parent_id)
            if parent_info and parent_info.get("name"):
                parent_name = parent_info.get("name")
                if parent_name != area_name:
                    return f"{parent_name} ({area_name})"

        return area_name or self.get_map_name(map_id)

    def search_zones(self, query: str, limit: int = 10) -> List[Dict[str, Any]]:
        """Search areas by partial name match."""
        q = query.strip().lower()
        if not q:
            return []
        results = []
        for area_id, info in self._areas.items():
            name = info.get("name", "")
            if q in name.lower():
                results.append({
                    "id": area_id,
                    "name": name,
                    "map_id": info.get("map_id", 0),
                    "parent_id": info.get("parent_id", 0),
                    "formatted": self.format_location(info.get("map_id", 0), area_id)
                })
                if len(results) >= limit:
                    break
        return results

    def get_stats(self) -> Dict[str, int]:
        return {
            "maps": len(self._maps),
            "areas": len(self._areas)
        }
