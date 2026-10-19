#include "Production/MacroAct.h"

#include "Construction/BuildingPlacer.h"
#include "Production/ProductionManager.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"

#include <regex>

using namespace UAlbertaBot;

// Map unit type names to unit types.
static std::map<std::string, BWAPI::UnitType> _unitTypesByName;

MacroLocation MacroAct::getMacroLocationFromString(const std::string & s) const
{
    if (s == "main")
    {
        return MacroLocation::Main;
    }
    if (s == "natural")
    {
        return MacroLocation::Natural;
    }
    if (s == "front")
    {
        return MacroLocation::Front;
    }
    if (s == "macro")
    {
        return MacroLocation::Macro;
    }
    if (s == "expo")
    {
        return MacroLocation::Expo;
    }
    if (s == "min only")
    {
        return MacroLocation::MinOnly;
    }
    if (s == "gas only")
    {
        return MacroLocation::GasOnly;
    }
    if (s == "hidden")
    {
        return MacroLocation::Hidden;
    }
    if (s == "center")
    {
        return MacroLocation::Center;
    }
    if (s == "proxy")
    {
        return MacroLocation::Proxy;
    }
    if (s == "enemy main")
    {
        return MacroLocation::EnemyMain;
    }
    if (s == "enemy natural")
    {
        return MacroLocation::EnemyNatural;
    }
    if (s == "gas steal")
    {
        return MacroLocation::GasSteal;
    }

    UAB_ASSERT(false, "config file - bad location '@ %s'", s.c_str());

    return MacroLocation::Anywhere;
}

void MacroAct::initializeUnitTypesByName()
{
    if (_unitTypesByName.size() == 0)       // if not already initialized
    {
        for (BWAPI::UnitType unitType : BWAPI::UnitTypes::allUnitTypes())
        {
            if (unitType.getRace() != BWAPI::Races::Terran) continue;
            std::string typeName = TrimRaceName(unitType.getName());
            std::replace(typeName.begin(), typeName.end(), '_', ' ');
            std::transform(typeName.begin(), typeName.end(), typeName.begin(), ::tolower);
            _unitTypesByName[typeName] = unitType;
        }
    }
}

BWAPI::UnitType MacroAct::getUnitTypeFromString(const std::string & s) const
{
    auto it = _unitTypesByName.find(s);
    if (it != _unitTypesByName.end())
    {
        return (*it).second;
    }
    return BWAPI::UnitTypes::Unknown;
}

MacroAct::MacroAct() 
    : _type(MacroActs::Default)
    , _macroLocation(MacroLocation::Anywhere)
    , _tileLocation(BWAPI::TilePositions::None)
    , _parent(nullptr)
{
}

// Create a Terran MacroAct from its name, like "marine" or "command center @ min only".
// String comparison here is case-insensitive.
MacroAct::MacroAct(const std::string & name)
    : _type(MacroActs::Default)
    , _macroLocation(MacroLocation::Anywhere)
    , _tileLocation(BWAPI::TilePositions::None)
    , _parent(nullptr)
{
    initializeUnitTypesByName();

    // Normalize the input string.
    std::string inputName(name);
    std::replace(inputName.begin(), inputName.end(), '_', ' ');
    std::transform(inputName.begin(), inputName.end(), inputName.begin(), ::tolower);
    if (inputName.substr(0, 7) == "terran ")
    {
        inputName = inputName.substr(7, std::string::npos);
    }
    
    

    // You can specify a location, like "command center @ expo" or "go post worker @ enemy natural".
    MacroLocation specifiedMacroLocation(MacroLocation::Anywhere);    // the default
    std::regex macroLocationRegex("([a-zA-Z_ ]+[a-zA-Z])\\s+\\@\\s+([a-zA-Z][a-zA-Z ]+)");
    std::smatch m;
    if (std::regex_match(inputName, m, macroLocationRegex))
    {
        std::string nameCopy = m[1].str();
        specifiedMacroLocation = getMacroLocationFromString(m[2].str());
        inputName = nameCopy;
    }

    // Commands like "go gas until 100". 100 is the amount.
    if (inputName.substr(0, 3) == std::string("go "))
    {
        for (MacroCommandType t : MacroCommand::allCommandTypes())
        {
            std::string commandName = MacroCommand::getName(t);
            if (MacroCommand::hasNumericArgument(t))
            {
                // There's an argument. Match the command name and parse out the argument.
                std::regex commandWithArgRegex(commandName + " (\\d+)");
                std::smatch m;
                if (std::regex_match(inputName, m, commandWithArgRegex))
                {
                    int amount = GetIntFromString(m[1].str());
                    if (amount >= 0)
                    {
                        *this = MacroAct(t, amount);
                        _macroLocation = specifiedMacroLocation;
                        return;
                    }
                }
            }
            else if (MacroCommand::hasUnitArgument(t))
            {
                // There's an argument. Match the command name and parse out the argument.
                std::regex commandWithArgRegex(commandName + " ([A-Za-z_ ]+)");
                std::smatch m;
                if (std::regex_match(inputName, m, commandWithArgRegex))
                {
                    BWAPI::UnitType unitType = getUnitTypeFromString(m[1].str());
                    if (unitType != BWAPI::UnitTypes::Unknown && unitType != BWAPI::UnitTypes::None)
                    {
                        *this = MacroAct(t, unitType);
                        _macroLocation = specifiedMacroLocation;
                        return;
                    }
                }
            }
            else
            {
                // No argument. Just compare for equality.
                if (commandName == inputName)
                {
                    *this = MacroAct(t);
                    _macroLocation = specifiedMacroLocation;
                    return;
                }
            }
        }
    }

    BWAPI::UnitType unitType = getUnitTypeFromString(inputName);
    if (unitType != BWAPI::UnitTypes::Unknown && unitType != BWAPI::UnitTypes::None)
    {
        *this = MacroAct(unitType);
        _macroLocation = specifiedMacroLocation;
        return;
    }

    for (BWAPI::TechType techType : BWAPI::TechTypes::allTechTypes())
    {
        if (techType.getRace() != BWAPI::Races::Terran) continue;
        std::string typeName = techType.getName();
        std::replace(typeName.begin(), typeName.end(), '_', ' ');
        std::transform(typeName.begin(), typeName.end(), typeName.begin(), ::tolower);
        if (typeName == inputName)
        {
            *this = MacroAct(techType);
            _macroLocation = specifiedMacroLocation;
            return;
        }
    }

    for (BWAPI::UpgradeType upgradeType : BWAPI::UpgradeTypes::allUpgradeTypes())
    {
        if (upgradeType.getRace() != BWAPI::Races::Terran) continue;
        std::string typeName = TrimRaceName(upgradeType.getName());
        std::replace(typeName.begin(), typeName.end(), '_', ' ');
        std::transform(typeName.begin(), typeName.end(), typeName.begin(), ::tolower);
        if (typeName == inputName)
        {
            *this = MacroAct(upgradeType);
            _macroLocation = specifiedMacroLocation;
            return;
        }
    }

    UAB_ASSERT_WARNING(false, "No MacroAct with name: %s", name.c_str());
}

MacroAct::MacroAct (BWAPI::UnitType t) 
    : _unitType(t)
    , _type(MacroActs::Unit) 
    , _macroLocation(MacroLocation::Anywhere)
    , _tileLocation(BWAPI::TilePositions::None)
    , _parent(nullptr)
{
}

MacroAct::MacroAct(BWAPI::UnitType t, MacroLocation loc)
    : _unitType(t)
    , _type(MacroActs::Unit)
    , _macroLocation(loc)
    , _tileLocation(BWAPI::TilePositions::None)
    , _parent(nullptr)
{
}

// For placing a building precisely on a pre-chosen tile.
MacroAct::MacroAct(BWAPI::UnitType t, const BWAPI::TilePosition & tile)
    : _unitType(t)
    , _type(MacroActs::Unit)
    , _macroLocation(MacroLocation::Tile)
    , _tileLocation(tile)
    , _parent(nullptr)
{
}

MacroAct::MacroAct(BWAPI::UnitType t, BWAPI::Unit parent) 
    : _unitType(t)
    , _type(MacroActs::Unit)
    , _macroLocation(MacroLocation::Anywhere)
    , _tileLocation(BWAPI::TilePositions::None)
    , _parent(parent)
{
}

MacroAct::MacroAct(BWAPI::TechType t)
    : _techType(t)
    , _type(MacroActs::Tech) 
    , _macroLocation(MacroLocation::Anywhere)
    , _tileLocation(BWAPI::TilePositions::None)
    , _parent(nullptr)
{
}

MacroAct::MacroAct(BWAPI::UpgradeType t) 
    : _upgradeType(t)
    , _type(MacroActs::Upgrade) 
    , _macroLocation(MacroLocation::Anywhere)
    , _tileLocation(BWAPI::TilePositions::None)
    , _parent(nullptr)
{
}

MacroAct::MacroAct(MacroCommandType t)
    : _macroCommandType(t)
    , _type(MacroActs::Command)
    , _macroLocation(MacroLocation::Anywhere)
    , _tileLocation(BWAPI::TilePositions::None)
    , _parent(nullptr)
{
}

MacroAct::MacroAct(MacroCommandType t, int amount)
    : _macroCommandType(t, amount)
    , _type(MacroActs::Command)
    , _macroLocation(MacroLocation::Anywhere)
    , _tileLocation(BWAPI::TilePositions::None)
    , _parent(nullptr)
{
}

MacroAct::MacroAct(MacroCommandType t, BWAPI::UnitType type)
    : _macroCommandType(t, type)
    , _type(MacroActs::Command)
    , _macroLocation(MacroLocation::Anywhere)
    , _tileLocation(BWAPI::TilePositions::None)
    , _parent(nullptr)
{
}

size_t MacroAct::type() const
{
    return _type;
}

bool MacroAct::isUnit() const 
{
    return _type == MacroActs::Unit; 
}

bool MacroAct::isWorker() const
{
    return _type == MacroActs::Unit && _unitType.isWorker();
}

// The unit type is a mobile combat unit for purposes of counting resources spent toward combat.
// Different from UnitUtil::IsCombatSimUnit() and UnitUtil::IsCombatUnit().
bool MacroAct::isCombatUnit() const
{
    if (!isUnit() || _unitType.isBuilding() || _unitType.isWorker() ||
        _unitType.isDetector() || _unitType.spaceProvided() > 0) return false;
    return _unitType.canAttack() || _unitType == BWAPI::UnitTypes::Terran_Medic ||
        _unitType == BWAPI::UnitTypes::Terran_Science_Vessel;
}

bool MacroAct::isTech() const
{ 
    return _type == MacroActs::Tech; 
}

bool MacroAct::isUpgrade() const 
{ 
    return _type == MacroActs::Upgrade; 
}

bool MacroAct::isCommand() const 
{ 
    return _type == MacroActs::Command; 
}

bool MacroAct::isBuilding()	const 
{ 
    return _type == MacroActs::Unit && _unitType.isBuilding(); 
}

bool MacroAct::isAddon() const
{
    return _type == MacroActs::Unit && _unitType.isAddon();
}



bool MacroAct::isRefinery()	const
{ 
    return _type == MacroActs::Unit && _unitType.isRefinery();
}

// The standard supply unit, ignoring the hatchery (which provides 1 supply) and nexus/CC.
bool MacroAct::isSupply() const
{
    return isUnit() && _unitType == BWAPI::UnitTypes::Terran_Supply_Depot;
}

bool MacroAct::isGasSteal() const
{
    return getMacroLocation() == MacroLocation::GasSteal;
}

BWAPI::UnitType MacroAct::getUnitType() const
{
    UAB_ASSERT(_type == MacroActs::Unit, "getUnitType of non-unit");
    return _unitType;
}

BWAPI::TechType MacroAct::getTechType() const
{
    UAB_ASSERT(_type == MacroActs::Tech, "getTechType of non-tech");
    return _techType;
}

BWAPI::UpgradeType MacroAct::getUpgradeType() const
{
    UAB_ASSERT(_type == MacroActs::Upgrade, "getUpgradeType of non-upgrade");
    return _upgradeType;
}

MacroCommand MacroAct::getCommandType() const
{
    UAB_ASSERT(_type == MacroActs::Command, "getCommandType of non-command");
    return _macroCommandType;
}

MacroLocation MacroAct::getMacroLocation() const
{
    return _macroLocation;
}

BWAPI::TilePosition MacroAct::getTileLocation() const
{
    return _tileLocation;
}

// Supply required if this is produced.
// It is NOT THE SAME as the supply required to have one of the units; it is the extra supply needed
// to make one of them.
int MacroAct::supplyRequired() const
{
    return isUnit() ? _unitType.supplyRequired() : 0;
}

// NOTE Because upgrades vary in price with level, this is context dependent.
int MacroAct::mineralPrice() const
{
    if (isCommand()) {
        
        return 0;
    }
    if (isUnit())
    {
        // Special case for sunks and spores, which are built from drones by Building Manager:
        // Return the price of the creep colony, not the total price (125) and not the final morph price (50).
        

        return _unitType.mineralPrice();
    }
    if (isTech())
    {
        return _techType.mineralPrice();
    }
    if (isUpgrade())
    {
        if (_upgradeType.maxRepeats() > 1 && BWAPI::Broodwar->self()->getUpgradeLevel(_upgradeType) > 0)
        {
            return _upgradeType.mineralPrice(1 + BWAPI::Broodwar->self()->getUpgradeLevel(_upgradeType));
        }
        return _upgradeType.mineralPrice();
    }

    UAB_ASSERT(false, "bad MacroAct");
    return 0;
}

// NOTE Because upgrades vary in price with level, this is context dependent.
int MacroAct::gasPrice() const
{
    if (isCommand()) {
        return 0;
    }
    if (isUnit())
    {
        return _unitType.gasPrice();
    }
    if (isTech())
    {
        return _techType.gasPrice();
    }
    if (isUpgrade())
    {
        if (_upgradeType.maxRepeats() > 1 && BWAPI::Broodwar->self()->getUpgradeLevel(_upgradeType) > 0)
        {
            return _upgradeType.gasPrice(1 + BWAPI::Broodwar->self()->getUpgradeLevel(_upgradeType));
        }
        return _upgradeType.gasPrice();
    }

    UAB_ASSERT(false, "bad MacroAct");
    return 0;
}

BWAPI::UnitType MacroAct::whatBuilds() const
{
    if (isUnit())
    {
        // Special case for sunks and spores, which are built from drones by Building Manager.
        

        return _unitType.whatBuilds().first;
    }
    if (isTech())
    {
        return _techType.whatResearches();
    }
    if (isUpgrade())
    {
        return _upgradeType.whatUpgrades();
    }
    if (isCommand())
    {
        return BWAPI::UnitTypes::None;
    }

    UAB_ASSERT(false, "bad MacroAct");
    return BWAPI::UnitTypes::Unknown;
}

std::string MacroAct::getName() const
{
    if (isUnit())
    {
        return _unitType.getName();
    }
    if (isTech())
    {
        return _techType.getName();
    }
    if (isUpgrade())
    {
        return _upgradeType.getName();
    }
    if (isCommand())
    {
        return _macroCommandType.getName();
    }

    UAB_ASSERT(false, "bad MacroAct");
    return "error";
}

// The given unit can produce the macro act.
bool MacroAct::isProducer(BWAPI::Unit unit) const
{
    BWAPI::UnitType producerType = whatBuilds();

    // Require the exact Terran producer type.
    if (!(
        producerType == unit->getType()
        ))
    {
        return false;
    }

    if (!unit->isCompleted())  { return false; }
    if (unit->isTraining())    { return false; }
    if (unit->isLifted())      { return false; }
    if (!unit->isPowered())    { return false; }
    if (unit->isUpgrading())   { return false; }
    if (unit->isResearching()) { return false; }

    if (isAddon())
    {
        // Already has an addon, or is otherwise unable to make one.
        if (!unit->canBuildAddon())
        {
            return false;
        }

        // There is latency between ordering an addon and the addon starting.
        // Do not retry immediately, but also do not trust the last command
        // forever: BWAPI keeps Build_Addon as the last command even when
        // StarCraft rejected it because the footprint was temporarily
        // occupied. The old unconditional check permanently locked goals such
        // as Comsat Station after one failed attempt.
        if (unit->getLastCommand().getType() == BWAPI::UnitCommandTypes::Build_Addon)
        {
            const int commandAge =
                BWAPI::Broodwar->getFrameCount() - unit->getLastCommandFrame();
            const int acknowledgementWindow = std::max(
                24,
                2 * BWAPI::Broodwar->getLatencyFrames() + 8);

            if (commandAge >= 0 && commandAge < acknowledgementWindow)
            {
                return false;
            }
        }
    }

    if (isUnit())
    {
        // Make sure the candidate hasn't already been given a morph order.
        // NOTE This is part of a workaround for a bug in BWAPI 4.4.0 latency compensation.
        //      Even without the bug, it can't hurt.
        BWAPI::Order order = the.micro.getMicroState(unit).getOrder();
        

        // Check for required tech buildings.
        typedef std::pair<BWAPI::UnitType, int> ReqPair;
        for (const ReqPair& pair : getUnitType().requiredUnits())
        {
            BWAPI::UnitType requiredType = pair.first;

            if (getUnitType() == BWAPI::UnitTypes::Terran_Ghost &&
                requiredType == BWAPI::UnitTypes::Terran_Covert_Ops)
            {
                continue;
            }

            // Only an add-on built by this producer must be attached to it.
            // Battlecruisers also require Physics Lab, which is attached to
            // Science Facility, not to their Starport. Global prerequisites
            // are checked by BWAPI::canMake() after producer selection.
            if (requiredType.isAddon() &&
                requiredType.whatBuilds().first == producerType)
            {
                if (!unit->getAddon() || unit->getAddon()->getType() != requiredType)
                {
                    return false;
                }
            }
        }
    }

    return true;
}

// Record the units which are currently able to carry out this macro act.
// For example, the idle barracks which can produce a marine.
// It gives a warning if you call it for a command, which has no producer.
void MacroAct::getCandidateProducers(std::vector<BWAPI::Unit> & candidates) const
{
    if (isCommand())
    {
        UAB_ASSERT(false, "no producer of a command");
        return;
    }

    if (_parent && _parent->exists() && isProducer(_parent))
    {
        candidates.push_back(_parent);
        return;
    }

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (isProducer(unit))
        {
            candidates.push_back(unit);
        }
    }
}

// The item can eventually be produced; a producer exists and may be free someday.
bool MacroAct::hasEventualProducer() const
{
    BWAPI::UnitType producerType = whatBuilds();

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        // A producer is good if it is the right type and doesn't suffer from
        // any condition that makes it unable to produce ever.
        if (unit->getType() == producerType &&
            unit->isPowered() &&     // replacing a pylon is a separate queue item
            !unit->isLifted() &&     // lifting/landing a building is a separate queue item
            (!isAddon() || unit->getAddon() == nullptr))
        {
            return true;
        }

        // NOTE An addon may be required on the producer. This doesn't check.
    }

    // We didn't find a producer. We can't make it.
    return false;
}

// The item can potentially be produced soon-ish; a producer is on hand and not too busy.
bool MacroAct::hasPotentialProducer() const
{
    BWAPI::UnitType producerType = whatBuilds();

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        // A producer is good if it is the right type and doesn't suffer from
        // any condition that makes it unable to produce for a long time.
        // Producing something else only makes it busy for a short time,
        // except that research takes a long time.
        if (unit->getType() == producerType &&
            unit->isPowered() &&     // replacing a pylon is a separate queue item
            !unit->isLifted() &&     // lifting/landing a building will be a separate queue item when implemented
            !unit->isUpgrading() &&
            !unit->isResearching() &&
            (!isAddon() || unit->getAddon() == nullptr))
        {
            return true;
        }

        // NOTE An addon may be required on the producer. This doesn't check.
    }

    // BWAPI::Broodwar->printf("missing producer for %s", getName().c_str());

    // We didn't find a producer. We can't make it.
    return false;
}

// Check the units needed for producing a unit type, beyond its producer.
bool MacroAct::hasTech() const
{
    // If it's not a unit, let's assume we're good.
    if (!isUnit())
    {
        return true;
    }

    // What we have.
    std::set<BWAPI::UnitType> ourUnitTypes;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        ourUnitTypes.insert(unit->getType());
    }

    // What we need. We only pay attention to the unit type, not the count,
    // which is needed only for merging archons and dark archons (which is not done via MacroAct).
    for (const std::pair<BWAPI::UnitType, int> & typeAndCount : getUnitType().requiredUnits())
    {
        BWAPI::UnitType requiredType = typeAndCount.first;
        if (ourUnitTypes.find(requiredType) == ourUnitTypes.end() &&
            (ProductionManager::Instance().isOutOfBook() || !requiredType.isBuilding() || !BuildingManager::Instance().isBeingBuilt(requiredType)))
        {
            // BWAPI::Broodwar->printf("missing tech: %s requires %s", getName().c_str(), requiredType.getName().c_str());
            // We don't have a type we need. We don't have the tech.
            return false;
        }
    }

    // We have the technology.
    return true;
}

// Can we produce the target now?
bool MacroAct::canProduce(BWAPI::Unit producer) const
{
    if (isCommand())
    {
        // NOTE Not always correct for an extractor trick (it may execute but do nothing).
        return true;
    }

    UAB_ASSERT(producer != nullptr, "producer was null");

    if (ProductionManager::Instance().meetsReservedResources(*this))
    {
        if (isUnit())
        {
            // Special case for sunks and spores, which are built from drones by Building Manager.
            

            return BWAPI::Broodwar->canMake(getUnitType(), producer);
        }
        if (isTech())
        {
            return BWAPI::Broodwar->canResearch(getTechType(), producer);
        }
        if (isUpgrade())
        {
            return BWAPI::Broodwar->canUpgrade(getUpgradeType(), producer);
        }

        UAB_ASSERT(false, "bad MacroAct");
    }

    return false;
}

// Create a unit or start research. Return whether the order was accepted.
bool MacroAct::produce(BWAPI::Unit producer) const
{
    UAB_ASSERT(producer != nullptr, "producer was null");

    // A terran add-on.
    if (isAddon())
    {
        return the.micro.Make(producer, getUnitType());
    }
    // A building that the building manager is responsible for.
    // The building manager handles sunkens and spores.
    else if (isBuilding() && UnitUtil::NeedsWorkerBuildingType(getUnitType()))
    {
        BWAPI::TilePosition desiredPosition = the.placer.getMacroLocationTile(getMacroLocation());
        BuildingManager::Instance().addBuildingTask(*this, desiredPosition, producer, isGasSteal());
        return true;
    }
    // A non-building unit, or a morphed zerg building.
    else if (isUnit())
    {
        return the.micro.Make(producer, getUnitType());
    }
    else if (isTech())
    {
        return producer->research(getTechType());
    }
    else if (isUpgrade())
    {
        return producer->upgrade(getUpgradeType());
    }
    else
    {
        UAB_ASSERT(false, "bad MacroAct");
        return false;
    }
}
