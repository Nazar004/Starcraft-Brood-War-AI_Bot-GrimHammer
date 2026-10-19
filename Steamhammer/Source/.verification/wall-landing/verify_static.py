from pathlib import Path
import math
import re

root = Path(__file__).resolve().parents[2]
pm = (root / 'Production/ProductionManager.cpp').read_text(encoding='utf-8-sig')
mi = (root / 'Combat/Micro/Micro.cpp').read_text(encoding='utf-8-sig')
bm = (root / 'Construction/BuildingManager.cpp').read_text(encoding='utf-8-sig')
wm = (root / 'Economy/WorkerManager.cpp').read_text(encoding='utf-8-sig')

timeout = eval(re.search(r'the.now\(\) - entry.first->second >= ([0-9 *]+)\)', pm).group(1))
assert timeout == 96

# Model observed footprint occupancy at the manager's six-frame cadence.
# Motion inside the footprint does not count as finishing a crossing.
def clearing_frame(samples):
    entered = {}
    for frame, ids in samples:
        for ident in ids:
            entered.setdefault(ident, frame)
        entered = {ident: start for ident, start in entered.items() if ident in ids}
        if any(frame - start >= timeout for start in entered.values()):
            return frame
    return None

assert clearing_frame((f, {1}) for f in range(0, 181, 6)) == 96
assert clearing_frame((f, {1} if f < 72 else set()) for f in range(0, 181, 6)) is None
assert clearing_frame((f, {1} if f < 60 or f >= 66 else set()) for f in range(0, 181, 6)) == 162
assert clearing_frame((f, {f // 72}) for f in range(0, 288, 6)) is None
assert clearing_frame([(0, {1}), (90, {1}), (96, {1})]) == 96

# Evaluate the actual C++ collision expression and compare it with an
# independent rectangle-separation calculation, including edge-only blockers.
body = re.search(r'const auto overlaps = .*?return (.*?);\s*\};', mi, re.S).group(1)
expr = body.replace('&&', ' and ')
for name, replacement in [('point.x', 'x'), ('point.y', 'y'),
    ('type.dimensionRight()', 'dr'), ('type.dimensionLeft()', 'dl'),
    ('type.dimensionDown()', 'dd'), ('type.dimensionUp()', 'du')]:
    expr = expr.replace(name, replacement)
expr = ' '.join(expr.split())
left, top, right, bottom = 320, 640, 447, 735
geometry = 0
for dl, dr, du, dd in [(11, 11, 11, 11), (7, 8, 9, 10), (31, 32, 31, 32)]:
    for margin in [0, 12, 32]:
        for x in range(270, 501, 5):
            for y in range(590, 791, 5):
                actual = eval(expr)
                expected = not (x + dr < left - margin or x - dl > right + margin or
                    y + dd < top - margin or y - du > bottom + margin)
                assert actual == expected
                # A 20-pixel arrival tolerance at the selected escape point
                # still clears the actual footprint after a 32-pixel margin.
                if margin == 32 and not actual:
                    for ox, oy in [(20, 0), (-20, 0), (0, 20), (0, -20)]:
                        assert (x + ox + dr < left or x + ox - dl > right or
                            y + oy + dd < top or y + oy - du > bottom)
                geometry += 1

# Check integration: construction cannot bypass the temporary movement owner.
for name, parameter in [('RightClick', 'unit'), ('MineMinerals', 'unit'),
    ('Repair', 'unit'), ('ReturnCargo', 'worker'), ('Build', 'builder'), ('MoveToBase', 'unit')]:
    function = re.search(r'(?:void|bool) Micro::' + name + r'\([^\n]*\)\s*\{(.*?)\n\}', mi, re.S).group(1)
    assert 'landingClearanceActive(' + parameter + ')' in function[:120]
assert 'busy.contains(worker) || the.micro.landingClearanceActive(worker)' in wm
assert 'if (the.micro.landingClearanceActive(b.builderUnit)) continue;' in bm
assert 'b.builderUnit && !the.micro.landingClearanceActive(b.builderUnit)' in bm
assert '_terranWallGateOccupiedSince.clear();' in pm[pm.index('void ProductionManager::initialize()'):]
clear = mi[mi.index('void Micro::clearLandingArea('):mi.index('bool Micro::yieldBlockingUnit(')]
assert 'existing->second.untilFrame = the.now() + 3 * 24;' in clear
assert 'orders[unit].setOrder(unit, BWAPI::Orders::Move, candidate.position)' in clear
assert 'terranMainWallTransitWaypoint' not in clear
assert 'lease.second.position.getApproxDistance(point) < 48' in clear
assert '!it->second.landingBuilding->isLifted()' in mi
print(f'PASS: 5 occupancy scenarios, {geometry} collision cases, arrival clearance, stable targets, command ownership and reset/release guards')
