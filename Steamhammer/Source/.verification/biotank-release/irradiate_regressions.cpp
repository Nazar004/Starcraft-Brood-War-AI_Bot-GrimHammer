
#include <cassert>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include "Strategy/BioProductionPolicy.h"
using namespace UAlbertaBot;
namespace BWAPI {
namespace UnitTypes { enum { Terran_Marine, Terran_Medic, Terran_Siege_Tank_Tank_Mode, Terran_Siege_Tank_Siege_Mode, Zerg_Lurker }; }
namespace TechTypes { enum { Stim_Packs, Tank_Siege_Mode }; }
}
struct Counts { std::map<int,int> values; int count(int t) const { auto i=values.find(t); return i==values.end()?0:i->second; } };
struct Player { std::set<int> research; bool hasResearched(int t) const { return research.count(t)>0; } };
struct World { struct { Counts completed; } my; Player player; Player* self() { return &player; } } the;
struct NearbyEnemyThreats { bool ground=false,air=false; bool hasGround() const {return ground;} bool hasAir() const {return air;} } threats;
const NearbyEnemyThreats& visibleEnemyThreatsNearOwnedBases() {return threats;}
struct CombatCommander { bool aggressive=false; void setAggression(bool b){aggressive=b;} static CombatCommander& Instance(){static CombatCommander c;return c;} };
struct StrategyManager {
 bool active=true, _bioTankFirstPushReleased=false;
 std::string _bioTankAttackStatus;
 bool bioTankPlanActive() const {return active;}
 bool bioTankPushReady();
 void updateBioTankAttackState();
};

bool bio=false,muta=false,defiler=false,learned=false; int gol=0,tanks=0,numCC=1,lurkers=0;
struct IrradiateManager { bool bioTankPlanActive() const {return bio;} bool evaluate(); };
int completedGoliathCount(){return gol;} int completedTankCount(){return tanks;} int rememberedEnemyUnitCount(int){return lurkers;}
bool IrradiateManager::evaluate() { bool zergMutaliskIrradiateThreat=muta,zergDefilerIrradiateThreat=defiler,learnedZergSpellcasterPreparation=learned;
const bool bioIrradiateCoreReady = bioTankPlanActive() &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Marine) >= 16 &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Medic) >=
            BioProductionPolicy::medicTarget(16);
    const bool bioLurkerIrradiateThreat = bioTankPlanActive() &&
        rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Lurker) > 0;
    const bool urgentZergIrradiatePlan =
        (bioIrradiateCoreReady &&
            (zergMutaliskIrradiateThreat || bioLurkerIrradiateThreat)) ||
        (zergMutaliskIrradiateThreat && completedGoliathCount() >= 4) ||
        ((zergDefilerIrradiateThreat || learnedZergSpellcasterPreparation) &&
            (completedTankCount() >= 4 || numCC >= 2));
return urgentZergIrradiatePlan; }

int main(){ IrradiateManager m;
 the.my.completed.values={{BWAPI::UnitTypes::Terran_Marine,40},{BWAPI::UnitTypes::Terran_Medic,10}};
 bio=true; muta=true; assert(m.evaluate()); // BioTank vs Muta, zero Goliaths
 bio=false; assert(!m.evaluate()); gol=4; assert(m.evaluate()); // existing mech screen
 gol=0; bio=true; the.my.completed.values[BWAPI::UnitTypes::Terran_Marine]=15; assert(!m.evaluate());
 the.my.completed.values[BWAPI::UnitTypes::Terran_Marine]=16; the.my.completed.values[BWAPI::UnitTypes::Terran_Medic]=3; assert(!m.evaluate());
 the.my.completed.values[BWAPI::UnitTypes::Terran_Medic]=4; muta=false; assert(!m.evaluate()); lurkers=1; assert(m.evaluate());
 bio=false; lurkers=0; defiler=true; numCC=2; assert(m.evaluate());
 std::cout<<"PASS: 8 BioTank/mech Irradiate production scenarios using actual predicate\n";
}
