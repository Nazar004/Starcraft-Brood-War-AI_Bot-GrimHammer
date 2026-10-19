
#include <cassert>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include "Strategy/BioProductionPolicy.h"
using namespace UAlbertaBot;
namespace BWAPI {
namespace UnitTypes { enum { Terran_Marine, Terran_Medic, Terran_Siege_Tank_Tank_Mode, Terran_Siege_Tank_Siege_Mode }; }
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
bool StrategyManager::bioTankPushReady()
{
    if (!bioTankPlanActive())
    {
        return true;
    }

    if (_bioTankFirstPushReleased)
    {
        return true;
    }

    _bioTankFirstPushReleased =
        the.my.completed.count(BWAPI::UnitTypes::Terran_Marine) >= 16 &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Medic) >=
            BioProductionPolicy::medicTarget(16) &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) >= 2 &&
        the.self()->hasResearched(BWAPI::TechTypes::Stim_Packs) &&
        the.self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode);
    return _bioTankFirstPushReleased;
}
void StrategyManager::updateBioTankAttackState()
{
    if (!bioTankPlanActive())
    {
        _bioTankAttackStatus.clear();
        return;
    }

    bool attack = false;
    if (!bioTankPushReady())
    {
        _bioTankAttackStatus = "WAIT opening army/Stim/Siege";
    }
    else
    {
        const NearbyEnemyThreats& nearby = visibleEnemyThreatsNearOwnedBases();
        if (nearby.hasGround())
        {
            _bioTankAttackStatus = "DEFEND ground threat at base";
        }
        else if (nearby.hasAir() &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Marine) < 14)
        {
            _bioTankAttackStatus = "DEFEND air threat: need Marines";
        }
        else
        {
            // Opening labels and their predictions are sticky. Once the core
            // is ready, a historical rush/contain alone must not park it until
            // 11/13 minutes. Local combat and base responders still run.
            attack = true;
            _bioTankAttackStatus = "ATTACK core ready";
        }
    }
    CombatCommander::Instance().setAggression(attack);
}

void setup(StrategyManager& s,int marines=40,int medics=10,int tanks=7,bool stim=true,bool siege=true) {
 s=StrategyManager{}; the=World{}; threats=NearbyEnemyThreats{};
 the.my.completed.values={{BWAPI::UnitTypes::Terran_Marine,marines},{BWAPI::UnitTypes::Terran_Medic,medics},{BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode,tanks}};
 if(stim) the.player.research.insert(BWAPI::TechTypes::Stim_Packs);
 if(siege) the.player.research.insert(BWAPI::TechTypes::Tank_Siege_Mode);
 CombatCommander::Instance().aggressive=false;
}
int main() {
 StrategyManager s;
 setup(s); s.updateBioTankAttackState(); assert(CombatCommander::Instance().aggressive); // screenshot army, no Vessel requirement
 setup(s,16,4,2); s.updateBioTankAttackState(); assert(CombatCommander::Instance().aggressive);
 setup(s,15); s.updateBioTankAttackState(); assert(!CombatCommander::Instance().aggressive);
 setup(s,40,3); s.updateBioTankAttackState(); assert(!CombatCommander::Instance().aggressive);
 setup(s,40,10,1); s.updateBioTankAttackState(); assert(!CombatCommander::Instance().aggressive);
 setup(s,40,10,7,false); s.updateBioTankAttackState(); assert(!CombatCommander::Instance().aggressive);
 setup(s,40,10,7,true,false); s.updateBioTankAttackState(); assert(!CombatCommander::Instance().aggressive);
 setup(s,16,4,0); the.my.completed.values[BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode]=2; s.updateBioTankAttackState(); assert(CombatCommander::Instance().aggressive);
 setup(s); threats.ground=true; s.updateBioTankAttackState(); assert(!CombatCommander::Instance().aggressive); assert(s._bioTankAttackStatus.find("ground threat")!=std::string::npos);
 threats.ground=false; s.updateBioTankAttackState(); assert(CombatCommander::Instance().aggressive); // threat clears, production state cannot latch defense
 threats.air=true; the.my.completed.values[BWAPI::UnitTypes::Terran_Marine]=13; s.updateBioTankAttackState(); assert(!CombatCommander::Instance().aggressive);
 the.my.completed.values[BWAPI::UnitTypes::Terran_Marine]=14; s.updateBioTankAttackState(); assert(CombatCommander::Instance().aggressive);
 setup(s); s.active=false; s.updateBioTankAttackState(); assert(!CombatCommander::Instance().aggressive);
 CombatCommander::Instance().aggressive=true; s.updateBioTankAttackState(); assert(CombatCommander::Instance().aggressive); // other plans retain their decision
 std::cout<<"PASS: 14 BioTank release and defense scenarios using actual source methods\n";
}
