from pathlib import Path
import json,re,itertools
root=Path.cwd(); manager=(root/'Strategy/StrategyManager.cpp').read_text(encoding='utf-8-sig')
def unique(items):
 d={}
 for k,v in items:
  assert k not in d,('duplicate JSON key',k)
  d[k]=v
 return d
live=Path(r'D:\DU\BP\Starcraft\Starcraft_1161\bwapi-data\AI\GrimHammer.json')
j=json.loads(live.read_text(encoding='utf-8-sig'),object_pairs_hook=unique)
old=json.loads((root/'.verification/tvz-bio-plans/GrimHammer.before-three-plans.json').read_text(encoding='utf-8-sig'))
cfg=j['Strategy'];books=cfg['Strategies'];names=['TvZ_Sparks','TvZ_PreHive','TvZ_SK']
for key,value in old.items():
 if key!='Strategy':assert j[key]==value,key
for key,value in old['Strategy']['Strategies'].items():assert books[key]==value,('old book changed',key)
for key in ['TvP','TvT','TvU','CounterStrategies']:
 assert cfg[key]==old['Strategy'][key],('old selector changed',key)
for key in ['TvZ_RushDefense','TvZ_GroundDefense']:
 assert cfg['StrategyCombos'][key]==old['Strategy']['StrategyCombos'][key]
assert cfg['TvZ']['Terran'][:10]==old['Strategy']['TvZ']['Terran']
assert len(cfg['TvZ']['Terran'])==13
for name in names:
 assert books[name]['Matchups']==['TvZ'] and books[name]['TerranPlan']==name
 assert manager.count('case TerranPlanId::'+name)>=2,name
# BWAPI's actual public name tables, not guessed spellings.
api=root.parent.parent/'BWAPILIB/Source'
available=set()
for f in ['UnitType.cpp','TechType.cpp','UpgradeType.cpp']:
 for token in re.findall(r'"([A-Za-z0-9_]+)"',(api/f).read_text(encoding='utf-8-sig')):
  available.add(re.sub(r'^(Terran|Zerg|Protoss)_','',token).replace('_',' ').lower())
for matchup in ['TvZ','TvT','TvP','TvU']:
 for entry in cfg[matchup]['Terran']:
  assert entry['Weight']>0 and entry['Strategy'] in books
  permitted=books[entry['Strategy']].get('Matchups');assert not permitted or matchup in permitted
requirements={'barracks':{'supply depot'},'academy':{'barracks'},'refinery':{'command center'},'marine':{'barracks'},'medic':{'academy','barracks'},'factory':{'barracks','refinery'},'machine shop':{'factory'},'siege tank tank mode':{'factory','machine shop'},'stim packs':{'academy'},'tank siege mode':{'machine shop'}}
for name in names:
 counts={'SCV':4};built={'command center'};used=4;cap=10
 for i,step in enumerate(books[name]['OpeningBuildOrder']):
  if step.startswith('go '):assert step in ['go defensive','go aggressive','go scout'];continue
  match=re.fullmatch(r'(\d+) x (.*)',step);n=int(match[1]) if match else 1
  act=(match[2] if match else step).split(' @ ')[0].lower()
  assert act in available,(name,i,act)
  assert requirements.get(act,set())<=built,(name,i,act,requirements.get(act,set())-built)
  supply={'scv':1,'marine':1,'medic':1,'siege tank tank mode':2}.get(act,0)*n
  assert used+supply<=cap,(name,i,step,used,cap)
  used+=supply
  cap+=n*{'supply depot':8,'command center':10}.get(act,0)
  built.add(act)
 if name in ['TvZ_Sparks','TvZ_SK']:
  assert not built&{'siege tank tank mode','machine shop','tank siege mode'}
 print(name,'opening dependencies and supply OK; final supply',used,'/',cap)
# Evaluate the actual new-plan readiness predicate over boundary states.
expr=manager.split('const auto& push = policy->push;',1)[1].split('_bioTankFirstPushReleased =',1)[1].split(';',1)[0]
expr=re.sub(r'the\.my\.completed\.count\(BWAPI::UnitTypes::(\w+)\)',lambda m:'units["'+m[1]+'"]',expr)
expr=re.sub(r'the\.self\(\)->hasResearched\(BWAPI::TechTypes::(\w+)\)',lambda m:'tech["'+m[1]+'"]',expr)
expr=expr.replace('the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::U_238_Shells)','ranged')
expr=expr.replace('BioProductionPolicy::medicTarget(push.marines)','((push.marines + 4) // 5)')
expr=expr.replace('&&',' and ').replace('||',' or ');expr=re.sub(r'!(?!=)',' not ',expr)
expr=' '.join(expr.split())
from types import SimpleNamespace
cases=0
for name in names:
 text=(root/'Strategy/Terran'/({'TvZ_Sparks':'TvZSparks.h','TvZ_PreHive':'TvZPreHive.h','TvZ_SK':'TvZSK.h'}[name])).read_text()
 match=re.search(r'"'+name+r'",\s*\{(\d+), (\d+), (\d+), (true|false), (true|false), (true|false)\}',text);assert match
 m,t,v=map(int,match.group(1,2,3));siege,range_gate,irrad=(x=='true' for x in match.group(4,5,6))
 push=SimpleNamespace(marines=m,tanks=t,vessels=v,siege=siege,range=range_gate,irradiate=irrad)
 for marine,medic,tanks,vessels,stim,sg,ran,irr in itertools.product([m-1,m,m+1],[0,(m+4)//5-1,(m+4)//5],[0,2,3,4],[0,1,2],[False,True],[False,True],[0,1],[False,True]):
  units={'Terran_Marine':marine,'Terran_Medic':medic,'Terran_Siege_Tank_Tank_Mode':0,'Terran_Siege_Tank_Siege_Mode':tanks,'Terran_Science_Vessel':vessels}
  tech={'Stim_Packs':stim,'Tank_Siege_Mode':sg,'Irradiate':irr};ranged=ran
  got=bool(eval(expr,{'__builtins__':{}},locals()))
  expected=marine>=m and medic>=(m+4)//5 and tanks>=t and vessels>=v and stim and (sg or not siege) and (ran>0 or not range_gate) and (irr or not irrad)
  assert got==expected,(name,units,tech,ran)
  cases+=1
 print(name,'readiness boundaries OK, including sieged Tanks and optional tech')
# Extract actual transition expressions from separate policy headers.
def evaluate_condition(expr,state):
 expr=expr.replace('TvZBioPlan::Minute','1440').replace('&&',' and ').replace('||',' or ')
 expr=re.sub(r'!(?!=)',' not ',expr)
 return eval(' '.join(expr.split()),{'__builtins__':{}},{'s':SimpleNamespace(**state)})
def transition(name,state):
 source=(root/'Strategy/Terran'/({'TvZ_Sparks':'TvZSparks.h','TvZ_PreHive':'TvZPreHive.h','TvZ_SK':'TvZSK.h'}[name])).read_text()
 body=source.split('inline TvZBioPlan::Next transition',1)[1].split('inline const',1)[0]
 if name=='TvZ_Sparks':
  match=re.search(r'return\s+(.*?)\?\s*TvZBioPlan::Next::(\w+)\s*:\s*TvZBioPlan::Next::(\w+)',body,re.S);assert match
  return match[2] if evaluate_condition(match[1],state) else match[3]
 for condition,result in re.findall(r'if\s*\((.*?)\)\s*return TvZBioPlan::Next::(\w+);',body,re.S):
  if evaluate_condition(condition,state):return result
 return re.findall(r'return TvZBioPlan::Next::(\w+);',body)[-1]
base={'frame':5*1440,'firstAttackFrame':-1,'vessels':1,'lairArmy':False,'hiveTech':False,'irradiate':True}
fixtures=[('TvZ_Sparks',{},'Keep'),('TvZ_Sparks',{'frame':7*1440-1},'Keep'),('TvZ_Sparks',{'frame':7*1440},'BioTank'),('TvZ_Sparks',{'lairArmy':True},'BioTank'),('TvZ_Sparks',{'firstAttackFrame':4*1440},'BioTank'),('TvZ_PreHive',{},'Keep'),('TvZ_PreHive',{'hiveTech':True},'SK'),('TvZ_PreHive',{'firstAttackFrame':5*1440-24*90},'SK'),('TvZ_PreHive',{'frame':11*1440,'vessels':0},'BioTank'),('TvZ_PreHive',{'frame':11*1440,'irradiate':False},'BioTank'),('TvZ_PreHive',{'frame':11*1440},'Keep'),('TvZ_SK',{'vessels':0,'hiveTech':True},'Keep')]
for name,updates,expected in fixtures:
 state={**base,**updates};assert transition(name,state)==expected,(name,state,expected)
print('PASS:',cases,'readiness states;',len(fixtures),'transition boundaries; original books/selectors preserved')