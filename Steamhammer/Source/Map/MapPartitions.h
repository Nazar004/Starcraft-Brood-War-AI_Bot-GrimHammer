#pragma once

#include <map>
#include <set>
#include <vector>
#include "BWAPI.h"

// Partition the map into connected walkable areas.
// The grain size is the walk tile, 8x8 pixels, the finest granularity the map provides.
// Unwalkable walk tiles are partition ID 0.
// Walkable partitions get partition IDs 1 and up.
//
// In GrimHammer this layer is dynamic: when a static neutral blocker disappears,
// the obstacle counts and connected components are rebuilt. Higher map layers can
// then rebuild their clearance/choke information without restarting the game.

namespace UAlbertaBot
{
    class MapPartitions
    {
        int width;      // in walk tiles
        int height;     // in walk tiles
        int numPartitions;
        unsigned int revision;

        std::vector< std::vector<unsigned short> > unwalkability; // 0 if walkable, otherwise count of blockages
        std::vector< std::vector<unsigned short> > partition;     // 0 if unwalkable, otherwise partition ID
        std::set<int> removedNeutralIDs;
        std::map<int, std::vector<BWAPI::WalkPosition> > neutralFootprints;

        void findUnwalkability();
        void rebuildPartitions();
        void markOnePartition(const BWAPI::WalkPosition & start);

    public:
        MapPartitions();
        void initialize();

        // Remove exactly one static neutral obstacle from the walkability map.
        // Returns true if topology was changed and the partitions were rebuilt.
        bool removeNeutral(BWAPI::Unit unit);

        bool walkable(int walkX, int walkY) const;
        bool walkable(const BWAPI::WalkPosition & pos) const;
        
        // Return the partition ID of a given spot.
        int id(int walkX, int walkY) const;
        int id(const BWAPI::WalkPosition & pos) const;
        int id(const BWAPI::TilePosition & pos) const;
        int id(const BWAPI::Position & pos) const;

        // Are the two points connected by ground?
        bool connected(const BWAPI::TilePosition & a, const BWAPI::TilePosition & b) const;
        bool connected(const BWAPI::Position & a, const BWAPI::Position & b) const;

        int getNumPartitions() const { return numPartitions; };
        unsigned int getRevision() const { return revision; };

        void drawWalkable() const;
        void drawPartition(int i, BWAPI::Color color) const;
    };
}
