#pragma once

#include <string>
#include <vector>
#include "BWAPI.h"

namespace UAlbertaBot
{

enum class OpeningPlan
    { None			// used as a null value
	, Unknown		// enemy plan not known yet or not recognized as one of the below
    , Contain       // early-game contain with bunker/cannons/sunkens
    , Proxy			// proxy building
    , WorkerRush	// early like Stone, late like one Tscmoo version
    , FastRush		// a cheese rush faster than 9 pool/8 rax/9 gate
    , HeavyRush		// 2 hatcheries pool only, 2 barracks no gas, 2 gates no gas
    , Factory		// terran fast factory
    , Wraith        // terran fast wraiths (typically 2 starport)
    , SafeExpand	// defended fast expansion, with bunker or cannons
    , NakedExpand	// undefended fast expansion (usual for zerg, bold for others)
    , Turtle		// cannons/bunker/sunkens thought to be on 1 base
    , VultureRush   // early Terran Vultures committed toward our bases
    , Size
    };

// An opening name is deliberately coarse: One opponent can expand and then
// add four gateways, or open factory and transition to air. These independent
// flags keep updating even after the main OpeningPlan has been fixed.
enum class OpeningThreat : unsigned int
    { None              = 0
    , GatewayPressure   = 1u << 0
    , ZealotPressure    = 1u << 1
    , DragoonPressure   = 1u << 2
    , FactoryPressure   = 1u << 3
    , MassBarracks      = 1u << 4
    , FastAirTech       = 1u << 5
    , CloakTech         = 1u << 6
    , MassProduction    = 1u << 7
    , LingPressure      = 1u << 8
    , HydraliskPressure = 1u << 9

    // Specific scouted opening signatures. These do not replace the coarse
    // OpeningPlan value: More than one can remain true after a transition.
    , ProtossOneGateCore       = 1u << 10
    , ProtossTwoGateZealot     = 1u << 11
    , ProtossFourGateDragoon   = 1u << 12
    , ProtossFastRobotics      = 1u << 13
    , ProtossFastDarkTemplar   = 1u << 14
    , ProtossFastStargate      = 1u << 15
    , TerranTwoRax             = 1u << 16
    , TerranTwoFactory         = 1u << 17
    , TerranTwoStarport        = 1u << 18
    , TerranFactoryExpand      = 1u << 19
    , ZergPoolFirst            = 1u << 20
    , ZergTwoHatchLing         = 1u << 21
    , ZergTwoHatchMuta         = 1u << 22
    , ZergTwoHatchLurker       = 1u << 23
    , ZergMutaliskTech         = 1u << 24
    , TerranVultureRush        = 1u << 25
    };

const std::vector< std::pair<OpeningPlan, std::string> > PlanNames =
{
    std::pair<OpeningPlan, std::string>(OpeningPlan::Unknown, "Unknown"),
    std::pair<OpeningPlan, std::string>(OpeningPlan::Contain, "Contain"),
    std::pair<OpeningPlan, std::string>(OpeningPlan::Proxy, "Proxy"),
    std::pair<OpeningPlan, std::string>(OpeningPlan::WorkerRush, "Worker rush"),
    std::pair<OpeningPlan, std::string>(OpeningPlan::FastRush, "Fast rush"),
    std::pair<OpeningPlan, std::string>(OpeningPlan::HeavyRush, "Heavy rush"),
    std::pair<OpeningPlan, std::string>(OpeningPlan::Factory, "Factory"),
    std::pair<OpeningPlan, std::string>(OpeningPlan::Wraith, "Wraith"),
    std::pair<OpeningPlan, std::string>(OpeningPlan::SafeExpand, "Safe expand"),
    std::pair<OpeningPlan, std::string>(OpeningPlan::NakedExpand, "Naked expand"),
    std::pair<OpeningPlan, std::string>(OpeningPlan::Turtle, "Turtle")
    , std::pair<OpeningPlan, std::string>(OpeningPlan::VultureRush, "Vulture rush")
};

// Turn an opening plan into a string.
static std::string OpeningPlanString(OpeningPlan plan)
{
    for (auto it = PlanNames.begin(); it != PlanNames.end(); ++it)
    {
        if ((*it).first == plan)
        {
            return (*it).second;
        }
    }

    return "Error";
}

// Turn a string into an opening plan.
static OpeningPlan OpeningPlanFromString(const std::string & planString)
{
    for (auto it = PlanNames.begin(); it != PlanNames.end(); ++it)
    {
        if ((*it).second == planString)
        {
            return (*it).first;
        }
    }

    return OpeningPlan::Unknown;
}

// Forward declarations.
class Base;
struct UnitInfo;

class OpponentPlan
{
private:

    OpeningPlan _openingPlan;		// estimated enemy plan
    bool _planIsFixed;				// estimate will no longer change
    unsigned int _threatFlags;	// independent early production/tech evidence

    // Keep the strongest Protoss gateway commitment we have actually scouted.
    // The threat bit flags are intentionally sticky, but a bare 1 Gate Core
    // flag used to remain the UI label until FOUR gateways were found.  These
    // counters let the recognizer progress naturally through 1/2/3/4 Gate.
    int _maxProtossGatewaysSeen;
    bool _protossRangedTechSeen;

    // Utility functions. Time in frames.
    int travelTime(BWAPI::UnitType unitType, const BWAPI::Position & pos, const Base * base) const;
    Base * closestEnemyBase(const BWAPI::Position & pos) const;
    bool rushBuilding(const UnitInfo & ui) const;

    OpeningPlan recognizeProxy() const;
    OpeningPlan recognizeRush() const;
    bool recognizeVultureRush() const;
    OpeningPlan recognizeTerranTech() const;
    void recognizeThreats();

    void recognize();

public:
    OpponentPlan();

    void update();

    OpeningPlan getPlan() const { return _openingPlan; };
    std::string getThreatName() const;
    bool hasThreat(OpeningThreat threat) const
    {
        return (_threatFlags & static_cast<unsigned int>(threat)) != 0;
    };
};

}
