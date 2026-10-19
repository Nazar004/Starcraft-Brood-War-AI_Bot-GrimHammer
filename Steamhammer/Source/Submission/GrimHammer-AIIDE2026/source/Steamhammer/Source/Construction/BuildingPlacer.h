#pragma once

#include "Construction/BuildingData.h"
#include "Map/Grids/GridBuildable.h"

namespace UAlbertaBot
{
class The;
class Base;

class BuildingPlacer
{
    enum ReserveBits : unsigned char
    {
        ReserveBuilding = 1 << 0,
        ReserveResource = 1 << 1,
        ReserveLane     = 1 << 2,
        // A verified Barracks + 1 or 2 Depot wall. It stays reserved after the
        // buildings finish and after the Barracks opens the wall by lifting.
        ReserveWall     = 1 << 3,
        ReserveWallBunker = 1 << 4
    };

    std::vector< std::vector<unsigned char> > _reserveMap;
    GridBuildable _buildable;
    // Short-lived rejected production footprints survive task replacement.
    std::map<std::pair<int, int>, int> _failedProductionLocations;

    bool _terranMainWallValid;
    // Initial tile positions are only defined for static neutral units.
    mutable int _terranMainWallBarracksID = -1;
    bool _terranMainWallOpened;
    bool _terranMainWallReleased;
    bool _terranMainWallDisabledForOpening = false;
    bool _terranMainWallCentralGate;
    bool _terranMainWallCoversNatural;
    bool _terranMainWallUsedRouteFallback;
    bool _terranMainWallUsedDestructibleRoute;
    std::string _terranMainWallStatus;
    int _lastTerranRushWallAttemptFrame = -1;
    BWAPI::TilePosition _terranMainWallBarracks;
    BWAPI::TilePosition _terranMainWallDepot1;
    BWAPI::TilePosition _terranMainWallDepot2;
    BWAPI::TilePosition _terranMainWallBunker;
    BWAPI::Position _terranMainWallInside;
    BWAPI::Position _terranMainWallOutside;
    BWAPI::Position _terranMainWallAnalysisCenter;
    BWAPI::Position _terranMainWallChokeEndA;
    BWAPI::Position _terranMainWallChokeEndB;

    void	reserveSpaceNearResources();
    void    reserveBaseMovementLanes();
    void    reserveOpenMainExitLane();
    void    clearReserveBits(unsigned char bits);
    void    findTerranMainWall(bool forceRouteFallback = false,
                              int sectionAttempt = 0, bool naturalFallback = false);
    std::string terranMainWallReadyStatus() const;
    void reserveTerranMainWallBunker();
    BWAPI::TilePosition findTerranMainWallBunkerLocation(bool terrainOnly) const;

    void	setReserve(const BWAPI::TilePosition & position, int width, int height,
                       unsigned char bits, bool flag);

    BWAPI::TilePosition connectedWalkableTileNear(const BWAPI::TilePosition & start) const;

    bool    enemyMacroLocation(MacroLocation loc) const;
    bool	boxOverlapsBase(int x1, int y1, int x2, int y2) const;
    bool	tileBlocksAddon(const BWAPI::TilePosition & position) const;

    bool    buildableTerrain(int x1, int y1, int width, int height) const;

    bool	isFreeTile(int x, int y) const;
    bool	freeOnTop(const BWAPI::TilePosition & tile, BWAPI::UnitType buildingType) const;
    bool	freeOnRight(const BWAPI::TilePosition & tile, BWAPI::UnitType buildingType) const;
    bool	freeOnLeft(const BWAPI::TilePosition & tile, BWAPI::UnitType buildingType) const;
    bool	freeOnBottom(const BWAPI::TilePosition & tile, BWAPI::UnitType buildingType) const;
    bool	freeOnAllSides(BWAPI::Unit building) const;

    bool	canBuildHere(const BWAPI::TilePosition & position, const Building & b) const;
    bool    keepsMovementLaneClear(int x1, int y1, int x2, int y2, BWAPI::UnitType type) const;
    bool	canBuildWithSpace(const BWAPI::TilePosition & position, const Building & b, int extraSpace) const;

    bool	groupTogether(BWAPI::UnitType type) const;

    bool    terranMainWallSlotAvailable(
                const BWAPI::TilePosition & tile,
                BWAPI::UnitType type) const;
    BWAPI::TilePosition getTerranMainWallLocation(const Building & b) const;

    BWAPI::TilePosition findEdgeLocation(const Building & b) const;
    BWAPI::TilePosition findGroupedLocation(const Building & b) const;
    BWAPI::TilePosition findSpecialLocation(const Building & b) const;
    BWAPI::TilePosition findAnyLocation(const Building & b, int extraSpace) const;

    int     countInRange(const BWAPI::Unitset & units, BWAPI::Position xy, int range) const;

public:

    BuildingPlacer();
    void initialize();
    // Recompute terrain/buildability acceleration data without touching current
    // building reservations after a neutral blocker changes the map.
    void refreshMapAnalysis(bool retryWall = false);
    // Rebuild only the base exit lanes when our chosen main/front changes.
    void refreshMovementLanes();
    // Reconsider a Terran wall once the opponent model has been read or a
    // current-game rush has been recognized, while the opening is still free.
    void tryTerranRushWall();
    // Simulate a building footprint against the active ground exits.
    bool preservesGroundExit(BWAPI::UnitType type, BWAPI::TilePosition tile) const;

    // Return a build location near a building's desired location, with the given margin of space.
    BWAPI::TilePosition	getBuildLocationNear(const Building & b, int extraSpace) const;
    void avoidFailedProductionLocation(BWAPI::UnitType type,
        const BWAPI::TilePosition& tile);

    void				reserveTiles(const BWAPI::TilePosition & position, int width, int height);
    void                reserveTiles(const Building & b);
    void				freeTiles(const BWAPI::TilePosition & position, int width, int height);
    void                freeTiles(const Building & b);
    bool				isReserved(int x, int y) const;
    bool                isReserved(const BWAPI::TilePosition & tile) const { return isReserved(tile.x, tile.y); };
    void				drawReservedTiles() const;

    bool                hasTerranMainWall() const { return _terranMainWallValid; };
    void                disableTerranMainWallForEarlyExpand();
    bool                isTerranMainWallReleased() const
                        { return _terranMainWallReleased; };
    bool                terranMainWallCoversNatural() const
                        { return _terranMainWallValid && _terranMainWallCoversNatural; };
    const std::string & getTerranMainWallStatus() const { return _terranMainWallStatus; };
    bool                isTerranMainWallBarracks(BWAPI::Unit unit) const;
    bool                isTerranMainWallBuilding(BWAPI::Unit unit) const;
    bool                isTerranMainWallComplete(bool allowLiftedGate = false) const;
    BWAPI::Position     getTerranMainWallCenter() const;
    BWAPI::Position     getTerranMainWallGateCenter() const
    {
        if (!_terranMainWallValid || !_terranMainWallBarracks.isValid())
        {
            return BWAPI::Positions::Invalid;
        }

        const BWAPI::UnitType type = BWAPI::UnitTypes::Terran_Barracks;
        return BWAPI::Position(
            _terranMainWallBarracks.x * 32 + type.tileWidth() * 16,
            _terranMainWallBarracks.y * 32 + type.tileHeight() * 16);
    }
    BWAPI::TilePosition getTerranMainWallBarracksTile() const { return _terranMainWallBarracks; };
    BWAPI::TilePosition getTerranMainWallDepot2Tile() const { return _terranMainWallDepot2; };
    BWAPI::TilePosition getTerranMainWallBunkerTile() const { return _terranMainWallBunker; };
    BWAPI::TilePosition getTerranMainWallBunkerLocation() const;
    bool                isNearTerranMainWall(const BWAPI::Position & position, int radius) const;
    bool                isInsideTerranMainWall(
                            const BWAPI::Position & position,
                            int marginPixels = 0) const;
    bool                crossesTerranMainWall(
                            const BWAPI::Position & from,
                            const BWAPI::Position & to,
                            int deadZonePixels = 24) const;
    // A ground waypoint just beyond the Barracks opening when a trip crosses
    // the completed main wall. Invalid means ordinary routing is appropriate.
    BWAPI::Position     terranMainWallTransitWaypoint(
                            const BWAPI::Position & from,
                            const BWAPI::Position & to,
                            int moverID = -1) const;
    void                markTerranMainWallOpened();
    void                markTerranMainWallClosed();
    void                markTerranMainWallReleasedForVision();

    bool                buildingOK(const Building & b) const;
    bool                buildingOK(const Building & b, const BWAPI::TilePosition & pos) const;

    BWAPI::TilePosition getExpoLocationTile(MacroLocation loc) const;
    BWAPI::TilePosition getMacroLocationTile(MacroLocation loc) const;
    BWAPI::Position     getMacroLocationPos(MacroLocation loc) const;
    BWAPI::TilePosition	getRefineryPosition() const;

    BWAPI::TilePosition getTerrainProxyPosition(const Base * base) const;
    BWAPI::TilePosition getInBaseProxyPosition(const Base * base) const;
    BWAPI::TilePosition getProxyPosition(const Base * base) const;

    BWAPI::TilePosition getAntiBunkerSunkenPosition(const Base * base, BWAPI::Unit bunker) const;
    BWAPI::TilePosition getAntiCannonSunkenPosition(const Base * base, BWAPI::Unit cannon) const;
};
}
