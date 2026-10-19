#pragma once

#include "Core/Common.h"

#include "Production/MacroAct.h"

namespace UAlbertaBot
{
struct BuildOrderItem
{
    MacroAct macroAct;	   // the thing we want to produce
    bool     isGasSteal;
    int      openingBookIndex; // >= 0 only for an item from the original JSON opening

    BuildOrderItem()
        : macroAct(BWAPI::UnitTypes::None)
        , isGasSteal(false)
        , openingBookIndex(-1)
    {
    }

    BuildOrderItem(MacroAct m, bool gasSteal = false, int bookIndex = -1)
        : macroAct(m)
        , isGasSteal(gasSteal)
        , openingBookIndex(bookIndex)
    {
    }
};

class BuildOrderQueue
{
    std::deque< BuildOrderItem > queue;		// highest priority item is in the back
    bool modified;							// so ProductionManager can detect changes made behind its back

public:

    BuildOrderQueue();

    bool isModified() const { return modified; };
    void resetModified() { modified = false; };

    void clearAll();											// clear the entire build order queue
    void dropStaticDefenses();									// delete any static defense buildings

    void queueAsHighestPriority(MacroAct m, bool gasSteal = false, int openingBookIndex = -1); // queue at the highest priority
    void queueAfterSpore(MacroAct m);                           // queue at highest priority except after spore
    void queueAsLowestPriority(MacroAct m, int openingBookIndex = -1); // queue at the lowest priority
    void removeHighestPriorityItem();							// remove the highest priority item
    void doneWithHighestPriorityItem();							// remove highest priority item without setting `modified`
    void removeAt(size_t i);									// remove an item at an arbitrary queue index
    void pullToTop(size_t i);									// move item at index i to the highest priority position

    size_t size() const;										// number of items in the queue
    bool isEmpty() const;
    
    const BuildOrderItem & getHighestPriorityItem() const;		// return the highest priority item
    BWAPI::UnitType getNextUnit() const;						// skip commands and return item if it's a unit
    int getNextGasCost(int n) const;							// look n ahead, return next nonzero gas cost
    
    bool anyInQueue(BWAPI::UpgradeType type) const;
    bool anyInQueue(BWAPI::UnitType type) const;
    bool anyInNextN(BWAPI::UnitType type, int n) const;
    bool anyInNextN(BWAPI::UpgradeType type, int n) const;
    bool anyInNextN(BWAPI::TechType type, int n) const;
    size_t numInQueue(BWAPI::UnitType type) const;
    size_t numInNextN(BWAPI::UnitType type, int n) const;
    void totalCosts(int & minerals, int & gas) const;
    bool isGasStealInQueue() const;

    void drawQueueInformation(int x, int y, bool outOfBook);

    // overload the bracket operator for ease of use
    // queue[queue.size()-1] is the next item
    // queue[0] is the last item
    BuildOrderItem operator [] (int i);
};
}
