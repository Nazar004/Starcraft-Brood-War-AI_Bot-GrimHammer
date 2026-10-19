#include "Production/ProductionGoal.h"

#include "Construction/BuildingManager.h"
#include "Production/ProductionManager.h"
#include "Core/The.h"
#include <map>

using namespace UAlbertaBot;

namespace
{
    std::map<std::pair<int, int>, int> blockedAddonProducerUntil;
    int blockedAddonLastObservedFrame = -1;

    std::pair<int, int> addonProducerKey(
        const MacroAct & act,
        BWAPI::Unit producer)
    {
        return std::make_pair(
            act.isUnit() ? act.getUnitType().getID() : -1,
            producer ? producer->getID() : -1);
    }

    bool addonProducerTemporarilyBlocked(
        const MacroAct & act,
        BWAPI::Unit producer,
        int now)
    {
        if (!act.isAddon() || !producer)
        {
            return false;
        }

        const auto found = blockedAddonProducerUntil.find(
            addonProducerKey(act, producer));

        return
            found != blockedAddonProducerUntil.end() &&
            now < found->second;
    }

    void temporarilyBlockAddonProducer(
        const MacroAct & act,
        BWAPI::Unit producer,
        int untilFrame)
    {
        if (act.isAddon() && producer)
        {
            blockedAddonProducerUntil[addonProducerKey(act, producer)] =
                untilFrame;
        }
    }

    BWAPI::Position clampToMap(const BWAPI::Position& position)
    {
        return BWAPI::Position(
            std::max(0, std::min(BWAPI::Broodwar->mapWidth() * 32 - 1, position.x)),
            std::max(0, std::min(BWAPI::Broodwar->mapHeight() * 32 - 1, position.y)));
    }

    void clearMobileUnitsFromAddonSpace(BWAPI::Unit producer)
    {
        if (!producer || !producer->exists() || !producer->getTilePosition().isValid())
        {
            return;
        }

        const BWAPI::TilePosition producerTile = producer->getTilePosition();
        const BWAPI::UnitType producerType = producer->getType();

        // Every Terran add-on occupies 2x2 tiles immediately to the right of
        // the producer, aligned with its bottom edge. One tile of padding also
        // removes units whose collision box still overlaps the construction
        // footprint although their center is just outside it.
        const int left = (producerTile.x + producerType.tileWidth()) * 32 - 16;
        const int top =
            (producerTile.y + producerType.tileHeight() - 2) * 32 - 16;
        const int right = left + 2 * 32 + 32;
        const int bottom = top + 2 * 32 + 32;

        // Spread blockers to several points on the side opposite the add-on.
        // Sending a large army to one exact fallback point can create another
        // collision jam and leave the add-on footprint occupied indefinitely.
        static const int offsets[][2] =
        {
            {-6,  0}, {-6, -3}, {-6,  3}, {-4, -5},
            {-4,  5}, {-8, -2}, {-8,  2}, {-7,  0}
        };

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists() || unit->getType().isBuilding() ||
                unit->isLoaded() || !unit->getPosition().isValid())
            {
                continue;
            }

            const bool overlaps =
                unit->getRight() >= left &&
                unit->getLeft() <= right &&
                unit->getBottom() >= top &&
                unit->getTop() <= bottom;

            if (overlaps)
            {
                const int slot =
                    unit->getID() % (sizeof(offsets) / sizeof(offsets[0]));

                const BWAPI::Position fallback = clampToMap(
                    producer->getPosition() +
                    BWAPI::Position(
                        offsets[slot][0] * 32,
                        offsets[slot][1] * 32));

                the.micro.MoveSafely(unit, fallback);
            }
        }
    }

    bool canEventuallyOwnGoalProducer(
        const MacroAct& act,
        BWAPI::Unit producer)
    {
        if (!producer || !producer->exists() ||
            producer->getType() != act.whatBuilds() ||
            !producer->isCompleted() || producer->isLifted() ||
            !producer->isPowered() || producer->isUpgrading() ||
            producer->isResearching())
        {
            return false;
        }

        if (act.isAddon() && producer->getAddon())
        {
            return false;
        }

        return true;
    }
}

// An upgrade or research goal may already be done.
bool ProductionGoal::alreadyAchieved() const
{
    if (act.isTech())
    {
        return
            BWAPI::Broodwar->self()->hasResearched(act.getTechType()) ||
            BWAPI::Broodwar->self()->isResearching(act.getTechType());
    }

    if (act.isUpgrade())
    {
        int level = BWAPI::Broodwar->self()->getUpgradeLevel(act.getUpgradeType());
        int max = BWAPI::Broodwar->self()->getMaxUpgradeLevel(act.getUpgradeType());
        return
            level >= max ||
            level == max - 1 && BWAPI::Broodwar->self()->isUpgrading(act.getUpgradeType());
    }

    return false;
}

bool ProductionGoal::failure() const
{
    // The goal fails if no possible unit could become its parent,
    // including buildings not yet started by BuildingManager.

    return
        alreadyAchieved() ||
        !act.hasEventualProducer() && !BuildingManager::Instance().isBeingBuilt(act.whatBuilds());
}

ProductionGoal::ProductionGoal(const MacroAct & macroAct)
    : parent(nullptr)
    , attempted(false)
    , lastAttemptFrame(-100000)
    , lastAddonClearFrame(-100000)
    , addonBlockedSinceFrame(-1)
    , addonRetryAfterFrame(-1)
    , nextParentSearchFrame(-1)
    , avoidedAddonProducerId(-1)
    , act(macroAct)
{
    UAB_ASSERT(act.isAddon() || act.isUpgrade() || act.isTech(), "unsupported goal");
    //BWAPI::Broodwar->printf("create goal %s", act.getName().c_str());
}

// Meant to be called once per frame to try to carry out the goal.
bool ProductionGoal::update()
{
    const int now = BWAPI::Broodwar->getFrameCount();
    const int addonAcknowledgementFrames = std::max(
        24,
        2 * BWAPI::Broodwar->getLatencyFrames() + 8);

    // The DLL may stay loaded while a new match resets the frame counter.
    if (blockedAddonLastObservedFrame > now)
    {
        blockedAddonProducerUntil.clear();
    }
    blockedAddonLastObservedFrame = now;

    // Clear any former parent that is lost.
    if (parent && !parent->exists())
    {
        parent = nullptr;
        attempted = false;
        lastAttemptFrame = -100000;
        addonBlockedSinceFrame = -1;
    }

    if (parent && !canEventuallyOwnGoalProducer(act, parent))
    {
        parent = nullptr;
        attempted = false;
        lastAttemptFrame = -100000;
        addonBlockedSinceFrame = -1;
    }

    // A successfully submitted add-on order needs a short latency window
    // before parent->getAddon() becomes visible. Do not treat that normal
    // delay as a blocked footprint and do not issue the command every frame.
    const bool waitingForAddonAcknowledgement =
        parent &&
        act.isAddon() &&
        attempted &&
        now >= lastAttemptFrame &&
        now < lastAttemptFrame + addonAcknowledgementFrames;

    if (waitingForAddonAcknowledgement)
    {
        return false;
    }

    // The acknowledgement window expired without an add-on appearing. The
    // command was not carried out, so a later add-on made by another goal must
    // not be mistaken for completion of this one.
    if (parent && act.isAddon() && attempted && !parent->getAddon())
    {
        attempted = false;
    }

    const bool parentReadyNow =
        parent &&
        act.isProducer(parent) &&
        act.canProduce(parent);

    // Add or replace a parent if necessary. MacroAct::getCandidateProducers()
    // intentionally returns only producers which are free right now. That is
    // not enough for an add-on goal: a Command Center training an SCV was
    // invisible here, so the Comsat goal could not reserve it or clear the
    // add-on footprint. Prefer a ready producer, but remember a valid busy or
    // footprint-blocked producer as the eventual parent.
    if ((!parent || !parentReadyNow) &&
        now >= addonRetryAfterFrame &&
        now >= nextParentSearchFrame)
    {
        // Looking through every building is cheap once per second, but it was
        // needlessly repeated on every frame while all candidate footprints
        // were blocked.
        nextParentSearchFrame = now + 24;

        std::vector<BWAPI::Unit> producers;
        act.getCandidateProducers(producers);

        BWAPI::Unit readyParent = nullptr;
        BWAPI::Unit avoidedReadyParent = nullptr;

        for (BWAPI::Unit candidate : producers)
        {
            if (!candidate || !candidate->exists())
            {
                continue;
            }

            if (addonProducerTemporarilyBlocked(act, candidate, now))
            {
                continue;
            }

            if (act.canProduce(candidate))
            {
                if (candidate->getID() != avoidedAddonProducerId)
                {
                    readyParent = candidate;
                    break;
                }

                avoidedReadyParent = candidate;
            }
        }

        if (!readyParent)
        {
            readyParent = avoidedReadyParent;
        }

        if (readyParent)
        {
            parent = readyParent;
            addonBlockedSinceFrame = -1;

            if (parent->getID() != avoidedAddonProducerId)
            {
                avoidedAddonProducerId = -1;
            }
        }
        else if (!parent)
        {
            BWAPI::Unit avoidedEventualParent = nullptr;

            for (BWAPI::Unit candidate : BWAPI::Broodwar->self()->getUnits())
            {
                if (canEventuallyOwnGoalProducer(act, candidate) &&
                    !addonProducerTemporarilyBlocked(act, candidate, now))
                {
                    if (candidate->getID() != avoidedAddonProducerId)
                    {
                        parent = candidate;
                        break;
                    }

                    avoidedEventualParent = candidate;
                }
            }

            if (!parent)
            {
                parent = avoidedEventualParent;
            }

            if (parent)
            {
                addonBlockedSinceFrame = -1;

                if (parent->getID() != avoidedAddonProducerId)
                {
                    avoidedAddonProducerId = -1;
                }
            }
        }
    }

    // Achieve the goal if possible.
    const bool selectedParentReadyNow =
        parent &&
        act.isProducer(parent) &&
        act.canProduce(parent);

    if (selectedParentReadyNow)
    {
        //BWAPI::Broodwar->printf("attempt goal %s", act.getName().c_str());
        if (act.produce(parent))
        {
            attempted = true;
            lastAttemptFrame = now;
            addonBlockedSinceFrame = -1;
            return true;
        }

        // canProduce() is only a preliminary check. buildAddon() can still be
        // rejected by StarCraft when a unit overlaps the footprint or the
        // terrain is invalid. Fall through to the add-on recovery below.
        attempted = false;
    }

    if (parent &&
        act.isAddon() &&
        (act.getUnitType() == BWAPI::UnitTypes::Terran_Comsat_Station ||
         act.getUnitType() == BWAPI::UnitTypes::Terran_Machine_Shop ||
         act.getUnitType() == BWAPI::UnitTypes::Terran_Control_Tower) &&
        BWAPI::Broodwar->self()->minerals() >= act.mineralPrice() &&
        BWAPI::Broodwar->self()->gas() >= act.gasPrice())
    {
        // A busy producer is a normal temporary wait. Once it is idle,
        // failure to add the add-on means that the footprint or terrain is
        // blocking it. Clear mobile blockers and keep retrying, but do not
        // keep this producer forever: an impossible footprint must not stop
        // this goal from trying another eligible building.
        if (parent->isTraining() || parent->isUpgrading() ||
            parent->isResearching() || parent->isLifted())
        {
            addonBlockedSinceFrame = -1;
        }
        else
        {
            if (addonBlockedSinceFrame < 0)
            {
                addonBlockedSinceFrame = now;
            }

            if (now >= lastAddonClearFrame + 48)
            {
                clearMobileUnitsFromAddonSpace(parent);
                lastAddonClearFrame = now;
            }

            // Eight seconds is long enough for moved units and command latency
            // to clear. Quarantine this one footprint for 30 seconds, release
            // its producer immediately, and try another eligible building on
            // the next second. This prevents one impossible Comsat, Machine
            // Shop or Control Tower position from owning the goal forever.
            if (now >= addonBlockedSinceFrame + 8 * 24)
            {
                avoidedAddonProducerId = parent->getID();
                temporarilyBlockAddonProducer(
                    act,
                    parent,
                    now + 30 * 24);
                parent = nullptr;
                attempted = false;
                lastAttemptFrame = -100000;
                addonBlockedSinceFrame = -1;
                addonRetryAfterFrame = now + 24;
                nextParentSearchFrame = now + 24;
            }
        }
    }
    else
    {
        addonBlockedSinceFrame = -1;
    }
    return false;
}

bool ProductionGoal::reservesProducer(BWAPI::Unit producer) const
{
    return
        parent && producer && parent == producer &&
        canEventuallyOwnGoalProducer(act, parent) &&
        act.isAddon() &&
        act.getUnitType() == BWAPI::UnitTypes::Terran_Comsat_Station &&
        ProductionManager::Instance().meetsReservedResources(act);
}

// Meant to be called once per frame to see if the goal is completed and can be dropped.
bool ProductionGoal::done()
{
    bool done = false;

    if (act.isAddon())
    {
        BWAPI::Unit addon = parent && parent->exists() ? parent->getAddon() : nullptr;
        done =
            addon &&
            addon->exists() &&
            addon->getType() == act.getUnitType();
    }
    else
    {
        // TODO upgrade/research completion remains approximate.
        done = parent && (parent->isUpgrading() || parent->isResearching());
    }

    if (done)
    {
        if (attempted)
        {
            //BWAPI::Broodwar->printf("completed goal %s", act.getName().c_str());
        }
        else
        {
            // The goal was "completed" but not attempted. That means it hit a race condition:
            // It took the same parent as another goal which was attempted and succeeded.
            // The goal went bad and has to be retried with a new parent.
            //BWAPI::Broodwar->printf("retry goal %s", act.getName().c_str());
            parent = nullptr;
            done = false;
        }
    }
    // No need to check every frame whether the goal has failed.
    else if (BWAPI::Broodwar->getFrameCount() % 32 == 22 && failure())
    {
        //BWAPI::Broodwar->printf("failed goal %s", act.getName().c_str());
        done = true;
    }

    return done;
}
