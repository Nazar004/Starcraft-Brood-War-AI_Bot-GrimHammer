"""Offline map-data checks, without compiling or executing the bot/game."""
import re
import struct
import sys
from pathlib import Path

from inspect_map import explode_binary, read_chk, terrain_walkability


def audit(map_path, archive_path, unit_types_path):
    assert explode_binary(bytes.fromhex("00 04 82 24 25 8f 80 7f")) == b"AIAIAIAIAIAIA"
    sections = read_chk(map_path)
    width, height = struct.unpack("<HH", sections[b"DIM "])
    terrain = terrain_walkability(sections, archive_path)
    ground = [row[:] for row in terrain]
    source = Path(unit_types_path).read_text()
    dimensions = [tuple(map(int, item.split(","))) for item in re.findall(
        r"\{([^{}]+)\}", re.search(
            r"unitDimensions\[.*?\]\s*=\s*\{(.*?)\};", source, re.S).group(1))]
    flags = re.search(r"static const unsigned int unitFlags\[.*?\]\s*=\s*\{(.*?)\};",
                      source, re.S).group(1).split(",")
    sprites = [struct.unpack_from("<HHHBBH", sections[b"THG2"], i)
               for i in range(0, len(sections[b"THG2"]), 10)]
    units = [struct.unpack_from("<HHH", sections[b"UNIT"], i + 4)
             for i in range(0, len(sections[b"UNIT"]), 36)]
    # THG2 bit 0x1000 distinguishes a decorative sprite from a unit sprite.
    units += [(x, y, kind) for kind, x, y, owner, unused, bits in sprites if not bits & 0x1000]
    doors = [(x, y, kind) for x, y, kind in units if kind in (205, 206, 207, 208)]
    for x, y, kind in units:
        if kind >= 214 or "AutoAttackAndMove" in flags[kind] or "Flyer" in flags[kind]:
            continue
        _, _, left, up, right, down = dimensions[kind]
        for yy in range(max(0, (y - up) // 8), min(height * 4, (y + down) // 8 + 1)):
            for xx in range(max(0, (x - left) // 8), min(width * 4, (x + right) // 8 + 1)):
                ground[yy][xx] = False

    def fits(point, kind, grid=ground):
        x, y = point
        left, up, right, down = dimensions[kind][2:] if kind is not None else (8, 8, 7, 7)
        if x - left < 0 or y - up < 0 or x + right >= width * 32 or y + down >= height * 32:
            return False
        return all(grid[yy][xx] for yy in range((y - up) // 8, (y + down) // 8 + 1)
                   for xx in range((x - left) // 8, (x + right) // 8 + 1))

    def waypoint(tx, ty, kind):
        for dx, dy in [(0, 0), (-8, 0), (8, 0), (0, -8), (0, 8),
                       (-8, -8), (8, -8), (-8, 8), (8, 8)]:
            point = (tx * 32 + 16 + dx, ty * 32 + 16 + dy)
            if fits(point, kind):
                return point
        return None

    bad_centers = 0
    relocated = 0
    native_fallback = 0
    for ty in range(height):
        for tx in range(width):
            old_passable = any(all(ground[ty * 4 + y + j][tx * 4 + x + i]
                                  for j in range(2) for i in range(2))
                               for y in range(3) for x in range(3))
            if not old_passable:
                continue
            assert waypoint(tx, ty, None) is not None, (tx, ty)
            center = (tx * 32 + 16, ty * 32 + 16)
            if fits(center, 5):
                continue
            bad_centers += 1
            point = waypoint(tx, ty, 5)
            if point is None:
                native_fallback += 1
            else:
                assert fits(point, 5)
                relocated += 1

    assert len(doors) == 8
    for x, y, kind in doors:
        # Old gather checks considered the ground beneath the door usable.
        assert terrain[y // 8][x // 8], (x, y)
        assert not fits((x, y), 5), (x, y)
        # The projection neighborhood has legal tank-sized ground on this map.
        assert any(waypoint(x // 32 + dx, y // 32 + dy, 5) is not None
                   for dx in range(-3, 4) for dy in range(-3, 4)), (x, y)
    # A 16px crack is usable by the coarse minimum, but not by a 32px Tank.
    crack = [[16 <= x < 18 for x in range(40)] for _ in range(40)]
    assert fits((17 * 8, 20 * 8), None, crack)
    assert not fits((17 * 8, 20 * 8), 5, crack)
    # Bounds and neutral geometry are checked for small and large ground units.
    assert not fits((0, 0), 0)
    assert not fits((width * 32 - 1, height * 32 - 1), 5)
    for kind in (0, 3, 5, 7, 34):
        for x, y, door_kind in doors:
            assert not fits((x, y), kind)

    print("Fortress: 128x128, 8 neutral invincible door units in THG2.")
    print("Old single-cell gather check accepts all 8 door centers; body check rejects all 8.")
    print(f"Coarse passable tiles with an invalid Tank center: {bad_centers}.")
    print(f"Safe local waypoint available: {relocated}; native final-goal fallback: {native_fallback}.")
    print("All returned Tank waypoints fit; all coarse 16px passages retain a valid waypoint.")
    print("Bounds, narrow crack, Marine/Goliath/Tank/SCV/Medic checks: PASS.")


if __name__ == "__main__":
    audit(*sys.argv[1:])
