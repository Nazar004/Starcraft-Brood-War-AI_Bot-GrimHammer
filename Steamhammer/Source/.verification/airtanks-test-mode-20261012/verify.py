"""Offline source/configuration checks. Does not compile or launch the bot."""
from pathlib import Path
import difflib
import json
import re

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SDK = Path(r'D:\DU\BP\BWAPI\BWAPI\include\BWAPI')
report = []

def check(name, valid):
    report.append({'check': name, 'passed': bool(valid)})

candidate = json.loads((HERE / 'GrimHammer.candidate.json').read_text(encoding='utf-8-sig'))
original = json.loads((HERE / 'before/GrimHammer.json').read_text(encoding='utf-8-sig'))
strategy = candidate['Strategy']
definitions = strategy['Strategies']
air = {k: v for k, v in definitions.items() if k.startswith('airtanks')}
check('eight separately named air openings', len(air) == 8)
check('forced BioTank only TvZ, DeepSix only TvP', strategy['ForcedStrategy'] ==
      {'TvZ': 'BioTank', 'TvP': 'TvP_DeepSix'})
check('TvT pools and fallback unchanged', all(strategy.get(k) == original['Strategy'].get(k)
      for k in ['TvT', 'TvU', 'Terran', 'StrategyCombos', 'CounterStrategies', 'EnemySpecificStrategy']))
check('all existing definitions preserved', all(definitions[k] == v
      for k, v in original['Strategy']['Strategies'].items()))
check('all non-strategy configuration preserved', all(candidate[k] == v
      for k, v in original.items() if k != 'Strategy'))
check('BioTank has no TvT eligibility', definitions['BioTank']['Matchups'] == ['TvZ'])
for matchup in ['TvZ', 'TvP', 'TvT', 'TvU']:
    available = {name for name, data in definitions.items()
                 if data.get('Enabled', True) and
                 (not 'Matchups' in data or matchup in data['Matchups'])}
    check(f'{matchup}: draft air excluded from all learning/random choices', not (set(air) & available))
    forced = strategy['ForcedStrategy'].get(matchup)
    check(f'{matchup}: forced choice registered or absent', not forced or forced in available)
    if matchup == 'TvT':
        check('TvT: no selectable bio opening', all(definitions[k].get('OpeningGroup') != 'bio'
              for k in available))
for name, data in air.items():
    check(f'{name}: old/new selector exclusion', data['Enabled'] is False and data['Matchups'] == [])
    check(f'{name}: separate host and plan', data['OpeningGroup'] == 'airtanks' and data['TerranPlan'] == 'AirTanks')
    check(f'{name}: sources and intended matchup recorded', bool(data['Sources']) and bool(data['SupportedMatchups']))

# Check the book grammar against names exposed by the installed SDK and
# prerequisite order, including required add-ons. This is not a timing simulator.
aliases = set()
for filename, prefix in [('UnitType.h', 'Terran_'), ('TechType.h', ''), ('UpgradeType.h', '')]:
    text = (SDK / filename).read_text(encoding='utf-8-sig')
    for token in re.findall(r'constexpr\s+\w+\s+(\w+)\s*\{', text):
        if prefix and not token.startswith(prefix): continue
        aliases.add(token.removeprefix('Terran_').replace('_', ' ').lower())
prerequisites = {
    'barracks': ['command center'], 'factory': ['barracks'], 'starport': ['factory'],
    'science facility': ['starport'], 'physics lab': ['science facility'],
    'control tower': ['starport'], 'machine shop': ['factory'], 'armory': ['factory'],
    'academy': ['barracks'], 'bunker': ['barracks'], 'marine': ['barracks'],
    'vulture': ['factory'], 'goliath': ['factory', 'armory'],
    'siege tank tank mode': ['factory', 'machine shop'], 'wraith': ['starport'],
    'valkyrie': ['starport', 'control tower', 'armory'],
    'science vessel': ['starport', 'control tower', 'science facility'],
    'battlecruiser': ['starport', 'control tower', 'science facility', 'physics lab'],
    'medic': ['academy', 'barracks'], 'stim packs': ['academy'], 'irradiate': ['science facility'],
    'spider mines': ['machine shop'], 'tank siege mode': ['machine shop'], 'ion thrusters': ['machine shop'],
}
for name, data in air.items():
    seen = {'command center': 1, 'scv': 4}
    for entry in data['OpeningBuildOrder']:
        token = entry.lower()
        count = 1
        match = re.fullmatch(r'(\d+)\s+x\s+(.+)', token)
        if match: count, token = int(match[1]), match[2]
        token = token.split(' @ ')[0]
        if token.startswith('go '):
            check(f'{name}: command {entry}', token in ['go defensive', 'go scout', 'go aggressive'])
            continue
        check(f'{name}: SDK alias {token}', token in aliases)
        check(f'{name}: prerequisites for {token}', all(seen.get(x, 0) for x in prerequisites.get(token, [])))
        seen[token] = seen.get(token, 0) + count
        if token == 'machine shop': check(f'{name}: enough Factories', seen[token] <= seen.get('factory', 0))
        if token == 'control tower': check(f'{name}: enough Starports', seen[token] <= seen.get('starport', 0))

existing = [p.relative_to(HERE / 'before').as_posix() for p in (HERE / 'before').rglob('*')
            if p.suffix in ['.h', '.cpp']]
new = [p.relative_to(ROOT).as_posix() for p in (ROOT / 'Strategy/Terran').glob('AirTanks*.h')]
new += ['Combat/Micro/Terran/AirTanksMicro.h']
files = existing + new
sdk_text = {k: (SDK / f).read_text(encoding='utf-8-sig')
            for k, f in [('UnitTypes', 'UnitType.h'), ('TechTypes', 'TechType.h'),
                         ('UpgradeTypes', 'UpgradeType.h'), ('Orders', 'Order.h')]}
strip_cpp = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
patches = []
for file in files:
    content = (ROOT / file).read_text(encoding='utf-8-sig')
    clean = strip_cpp.sub('', content)
    stack = []
    balanced = True
    for ch in clean:
        if ch in '{([': stack.append(ch)
        elif ch in '})]':
            if not stack or stack.pop() != {'}': '{', ')': '(', ']': '['}[ch]:
                balanced = False
                break
    check(f'{file}: balanced delimiters', balanced and not stack)
    for include in re.findall(r'^#include "([^"]+)"', content, re.M):
        check(f'{file}: include {include}', (ROOT / include).exists() or
              (ROOT / file).parent.joinpath(include).exists() or SDK.parent.joinpath(include).exists())
    for namespace, token in set(re.findall(r'BWAPI::(UnitTypes|TechTypes|UpgradeTypes|Orders)::(\w+)', clean)):
        check(f'{file}: SDK {namespace}::{token}', bool(re.search(r'\b' + token + r'\b', sdk_text[namespace])))
    before = HERE / 'before' / file
    old = before.read_text(encoding='utf-8-sig') if before.exists() else ''
    patches.extend(difflib.unified_diff(old.splitlines(True), content.splitlines(True),
                   fromfile='before/' + file, tofile='after/' + file))

parser = (ROOT / 'Utilities/ParseUtils.cpp').read_text(encoding='utf-8-sig')
manager = (ROOT / 'Strategy/StrategyManager.cpp').read_text(encoding='utf-8-sig')
check('parser disables definitions before registration', parser.index('if (!enabled) continue;') < parser.index('addStrategy('))
check('forced selection precedes map/learning application', parser.index('ForcedStrategy.clear()') < parser.index('// 0.5 SPECIAL CASE'))
check('Crazyhammer honors forced selection', 'if (!openingStrategyDecided && Config::Strategy::Crazyhammer' in parser)
check('BioTank test prevents initial macro UCB substitution', 'defaultPlan == TerranPlanId::BioTank && !Config::Strategy::ForcedStrategy.empty()' in manager)
check('AirTanks not an automatic UCB macro candidate', 'defaultPlan != TerranPlanId::AirTanks' in manager)
check('AirTanks composition returns before bio/mech goals', manager.index('return AirTanks::productionGoals(') < manager.index('else if (_openingGroup == "bio")', manager.index('const MetaPairVector StrategyManager::getTerranBuildOrderGoal')))
check('AirTanks not subject to PureAir plan replacement', '!deepSixPlanActive() && !airTanksPlanActive()' in manager)
check('AirTanks not fed by generic bank filler', 'StrategyManager::Instance().airTanksPlanActive()) return false;' in manager)
check('air micro gated to AirTanks', '!StrategyManager::Instance().airTanksPlanActive()' in (ROOT / new[-1]).read_text())
check('per-game independent latches reset', '_airTanksFirstPushReleased = false;' in manager and '_airTanksAirReleased = false;' in manager)

(HERE / 'task-only.patch').write_text(''.join(patches), encoding='utf-8')
(HERE / 'source-checks.json').write_text(json.dumps({'compiled': False, 'game_run': False,
     'checks': report}, indent=2, ensure_ascii=False), encoding='utf-8')
failed = [item['check'] for item in report if not item['passed']]
print(f'{len(report)} offline source/configuration checks; {len(failed)} failed')
for name in failed: print('FAILED:', name)
raise SystemExit(bool(failed))
