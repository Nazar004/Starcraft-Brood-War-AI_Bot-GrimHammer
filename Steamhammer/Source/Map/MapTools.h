#pragma once

#include <map>
#include <string>
#include <vector>
#include "Map/Grids/GridDistances.h"

// Keep track of map information, like what tiles are walkable or buildable.

namespace UAlbertaBot
{
class Base;
class GridDistances;

class MapTools
{
    const size_t allMapsSize = 96;			// keep a larger cheap runtime-distance cache to avoid periodic clear/rebuild bursts

    std::map<BWAPI::TilePosition, GridDistances>
                        _allMaps;			// a cache of already computed distance maps
    std::map<BWAPI::TilePosition, unsigned long long>
                        _allMapsLastUse;
    unsigned long long  _allMapsUseClock = 0;
    std::map<BWAPI::TilePosition, GridDistances>
                        _runtimeMaps;        // same, but including current building occupancy
    std::vector< std::vector<bool> >
                        _terrainWalkable;	// walkable considering terrain only
    std::vector< std::vector<bool> >
                        _walkable;			// walkable considering terrain and neutral units
    std::vector< std::vector<bool> >
                        _buildable;
    std::vector< std::vector<bool> >
                        _depotBuildable;
    std::vector< std::vector<unsigned char> >
                        _runtimeBlocked;     // occupied by a grounded building remembered right now
    std::vector< std::vector<unsigned char> >
                        _groundTransitions; // cached 8-direction static transition mask

    int                 _lastRuntimeUpdateFrame;
    unsigned int        _runtimeRevision;
    mutable int         _lastExpansionSafetyFrame;
    mutable unsigned int _expansionSafetyTopologyRevision;
    mutable std::map<int, double> _expansionSafetyScores;
    mutable std::map<int, std::string> _expansionSafetyDetails;
    mutable std::map<BWAPI::TilePosition, int> _deferredExpansions;
    mutable int _lastDeferredExpansionFrame = -1;
    // Static terrain routes are reused when the dynamic safety scores refresh.
    mutable std::map<std::pair<int, int>, std::vector<BWAPI::TilePosition>>
                        _expansionSafetyRoutes;
    int _pathWorkFrame;
    int _distanceFieldsBuilt;
    int _groundWaypoints;
    int _groundAStarCalls;
    int _tileDistanceFields = 0;
    int _waypointFields = 0;
    int _placementFields = 0;
    int _distanceMapEvictions = 0;
    void notePathWork(int fields, int waypoints, int astar);
    void touchDistanceMap(BWAPI::TilePosition tile);
    void evictDistanceMapIfFull();

    void				setBWAPIMapData();					// reads in the map data from bwapi and stores it in our map format
    BWAPI::TilePosition nearestWalkableTile(BWAPI::TilePosition tile, bool runtimeBlocks, int maxRadius = 8) const;
    bool pathTransitionWalkable(BWAPI::TilePosition from, BWAPI::TilePosition to, bool runtimeBlocks) const;
    void refreshExpansionSafetyScores() const;
    double expansionSafetyScore(const Base * base) const;
    const std::string & expansionSafetyDetail(const Base * base) const;

public:

    struct PathWork
    {
        int distanceFields;
        int waypoints;
        int astar;
        int tileDistanceFields;
        int waypointFields;
        int placementFields;
        int evictions;
    };
    PathWork pathWorkAtFrame(int frame) const;

    MapTools();
    void initialize();
    // Rebuild dynamic walk/build data and invalidate cached distances after
    // static neutral topology changes.
    void refreshTopology();
    // Refresh the lightweight occupancy layer made from our grounded buildings
    // plus remembered enemy grounded buildings. Terrain/Area topology is NOT
    // rebuilt when a Factory lands or a bunker appears.
    void updateRuntimeWalkability(bool force = false);

    int		getGroundTileDistance(BWAPI::TilePosition from, BWAPI::TilePosition to);
    int		getGroundTileDistance(BWAPI::Position from, BWAPI::Position to);
    int		getGroundDistance(BWAPI::Position from, BWAPI::Position to);

    // True only when a unit can cross the shared edge between adjacent build
    // tiles in the current 8x8 topology. Two tiles being individually
    // walkable is not enough on cliff maps.
    bool    isGroundTransitionWalkable(BWAPI::TilePosition from, BWAPI::TilePosition to) const;
    bool    isGroundTransitionRuntimeWalkable(BWAPI::TilePosition from, BWAPI::TilePosition to) const;

    // Extract one deterministic shortest ground route from the cached distance
    // field. This is used for strategic movement/entrance placement instead of
    // trusting high-level Area boundaries which may be ambiguous on open maps.
    bool    getGroundPath(BWAPI::TilePosition from, BWAPI::TilePosition to,
                          std::vector<BWAPI::TilePosition> & path, int maxSteps = 256,
                          bool runtimeBlocks = false);
    BWAPI::Position getGroundPathWaypoint(BWAPI::Position from, BWAPI::Position to,
                                          int lookAheadTiles = 6, bool runtimeBlocks = false);

    // Pass only valid tiles to these routines!
    bool	isTerrainWalkable(BWAPI::TilePosition tile) const { return _terrainWalkable[tile.x][tile.y]; };
    bool	isWalkable(BWAPI::TilePosition tile) const { return _walkable[tile.x][tile.y]; };
    bool    isRuntimeWalkable(BWAPI::TilePosition tile) const
    {
        return tile.isValid() && _walkable[tile.x][tile.y] && !_runtimeBlocked[tile.x][tile.y];
    };
    bool	isBuildable(BWAPI::TilePosition tile) const { return _buildable[tile.x][tile.y]; };
    bool	isDepotBuildable(BWAPI::TilePosition tile) const { return _depotBuildable[tile.x][tile.y]; };
    unsigned int runtimeRevision() const { return _runtimeRevision; };

    // TODO deprecated method, used only in Bases
    bool	isBuildable(BWAPI::TilePosition tile, BWAPI::UnitType type) const;

    const std::vector<BWAPI::TilePosition> & getClosestTilesTo(BWAPI::TilePosition pos);
    const std::vector<BWAPI::TilePosition> & getClosestTilesTo(BWAPI::Position pos);

    void	drawHomeDistances();
    void    drawExpoScores();

    Base *				nextExpansion(bool hidden, bool wantMinerals, bool wantGas) const;
    BWAPI::TilePosition	getNextExpansion(bool hidden, bool wantMinerals, bool wantGas) const;
    void deferExpansion(BWAPI::TilePosition tile, int frames);
    BWAPI::TilePosition	reserveNextExpansion(bool hidden, bool wantMinerals, bool wantGas);
};

}
