#pragma once

#include <set>
#include <vector>
#include "Map/Grids/Grid.h"

// High-level map areas and chokepoints.
//
// GridZone keeps the old tile->zone API used throughout Steamhammer, but now
// also exposes explicit ChokePoint objects and paths through the area graph.
// This is intentionally similar to the useful high-level part of BWEM while
// preserving the existing GrimHammer interfaces.

namespace UAlbertaBot
{
enum class ZoneState { Choke, Normal, Invalid };

class Zone
{
    friend class GridZone;

private:
    int _id;
    ZoneState _state;
    int _groundHeight;      // -1 if not the same throughout
    std::vector<BWAPI::TilePosition> _tiles;
    std::set<Zone *> _neighbors;
    BWAPI::Position _center;

    Zone(int zoneID);

    void mergeInto(Zone * target);
    void invalidate();

public:
    int id() const { return _id; };
    bool isValid() const { return _state != ZoneState::Invalid; };
    bool isChoke() const { return _state == ZoneState::Choke; };
    bool isArea() const { return _state == ZoneState::Normal; };
    int groundHeight() const { return _groundHeight; };
    const std::vector<BWAPI::TilePosition> & tiles() const { return _tiles; };
    const std::set<Zone *> & neighbors() const { return _neighbors; };
    const BWAPI::Position & center() const { return _center; };
};

class ChokePoint
{
    friend class GridZone;

private:
    int _id;
    Zone * _zone;
    Zone * _areaA;
    Zone * _areaB;
    BWAPI::Position _center;
    BWAPI::Position _endA;
    BWAPI::Position _endB;
    int _widthWalkTiles;

public:
    ChokePoint();

    int id() const { return _id; };
    Zone * zone() const { return _zone; };
    Zone * areaA() const { return _areaA; };
    Zone * areaB() const { return _areaB; };
    const BWAPI::Position & center() const { return _center; };
    const BWAPI::Position & endA() const { return _endA; };
    const BWAPI::Position & endB() const { return _endB; };
    int widthWalkTiles() const { return _widthWalkTiles; };
    int widthPixels() const { return 8 * _widthWalkTiles; };

    bool connects(const Zone * a, const Zone * b) const;
    Zone * otherArea(const Zone * area) const;
};

class GridZone : public Grid
{
private:
    std::vector<Zone *> zones;
    std::vector<ChokePoint> chokePoints;
    // Voronoi-style assignment of every usable build tile to its nearest
    // spacious area. This is separate from the legacy tile->zone grid and is
    // used for the explicit Area/Choke graph.
    std::vector< std::vector<short> > areaGrid;

    // Several spacious legacy seeds may actually be one open high-level Area.
    // areaCanonical maps every seed Zone id to the canonical Area id after
    // broad/open frontiers have been merged. This is analogous to BWEM's rule
    // that Areas are separated by real chokepoints, not arbitrary Voronoi lines.
    std::vector<short> areaCanonical;
    std::vector<BWAPI::Position> areaCenters;

    // Hot combat code often asks the same Area A -> Area B question for every
    // unit in a squad. Cache only the first choke index for each area pair.
    // -2 = not computed, -1 = no route, >=0 = chokePoints index.
    mutable std::vector< std::vector<short> > firstChokeCache;

    void clearZones();
    void newZoneID(const Zone * zone, int id);
    void computeZoneCenters();
    void buildChokePoints();
    BWAPI::Position findWalkableNear(const BWAPI::Position & pos, const Zone * preferredZone) const;
    BWAPI::Position traceChokeEdge(const BWAPI::Position & center, double sideX, double sideY, int sign) const;
    const ChokePoint * chokeForZone(const Zone * zone) const;
    int canonicalAreaID(int id) const;
    const Zone * areaFromID(int id) const;
    BWAPI::Position areaCenter(const Zone * area) const;

    void sanityCheck();

public:
    GridZone();
    ~GridZone();

    void initialize();

    // Return nullptr for zone 0, a pointer to the zone otherwise.
    Zone * ptr(int id);
    Zone * ptr(int x, int y);
    Zone * ptr(const BWAPI::TilePosition & tile);
    const Zone * ptr(int id) const;
    const Zone * ptr(int x, int y) const;
    const Zone * ptr(const BWAPI::TilePosition & tile) const;

    const std::vector<ChokePoint> & chokes() const { return chokePoints; };

    // Return the high-level spacious area associated with a position. Tiles in
    // a narrow legacy choke zone are assigned to the nearest adjacent area.
    const Zone * areaAt(const BWAPI::TilePosition & tile) const;
    const Zone * areaAt(const BWAPI::WalkPosition & walk) const;
    const Zone * areaAt(const BWAPI::Position & pos) const;
    const Zone * areaAt(BWAPI::Unit unit) const;

    // Stable high-level topology API. Game logic should use these instead of
    // legacy Grid::at()/ptr(), because narrow choke tiles intentionally have a
    // different legacy zone id while still belonging to one practical Area.
    int areaIDAt(const BWAPI::TilePosition & tile) const;
    int areaIDAt(const BWAPI::WalkPosition & walk) const;
    int areaIDAt(const BWAPI::Position & pos) const;
    int areaIDAt(BWAPI::Unit unit) const;
    bool sameArea(const BWAPI::TilePosition & a, const BWAPI::TilePosition & b) const;
    bool sameArea(const BWAPI::Position & a, const BWAPI::Position & b) const;
    bool sameArea(BWAPI::Unit a, const BWAPI::Position & b) const;

    // High-level graph routes. The returned zone path includes start and goal.
    std::vector<const Zone *> findZonePath(const BWAPI::Position & from, const BWAPI::Position & to) const;
    std::vector<const ChokePoint *> findChokePath(const BWAPI::Position & from, const BWAPI::Position & to) const;
    const ChokePoint * findFirstChoke(const BWAPI::Position & from, const BWAPI::Position & to) const;

    // Nearest explicit choke to a position, optionally preferring chokes in the
    // direction of 'toward'. Useful as a fallback on open/odd maps.
    const ChokePoint * findNearestChoke(const BWAPI::Position & from, const BWAPI::Position & toward) const;

    void draw();
};

}
