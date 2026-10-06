#include "Map/Grids/Grid.h"

#include "Utilities/UABAssert.h"

using namespace UAlbertaBot;

// Create an empty, unitialized, unusable grid.
// Necessary if a Grid subclass is created before BWAPI is initialized.
Grid::Grid()
{
}

// Create an initialized grid, given the size.
Grid::Grid(int w, int h, int value)
    : width(w)
    , height(h)
    , grid(w, std::vector<short>(h, value))
{
}

// Initialize a grid by copying another.
Grid::Grid(const Grid & g)
	: width(g.width)
	, height(g.height)
	, grid(g.grid)
{
}

int Grid::get(int x, int y) const
{
    const bool valid =
        width > 0 && height > 0 &&
        grid.size() == size_t(width) &&
        x >= 0 && y >= 0 && x < width && y < height &&
        size_t(x) < grid.size() && grid[x].size() == size_t(height);

    if (!valid)
    {
        // Do not turn a diagnostic into an access violation. A bad Grid query
        // must be visible in-game, but printing the same failure hundreds of
        // times per second would itself create the micro-stutter we are trying
        // to diagnose. Report at most once per second and return "unreachable".
        static int lastGridErrorReportFrame = -100000;
        const int frame = BWAPI::BroodwarPtr ? BWAPI::Broodwar->getFrameCount() : -1;
        if (frame < lastGridErrorReportFrame ||
            frame - lastGridErrorReportFrame >= 24)
        {
            lastGridErrorReportFrame = frame;
            const size_t storedHeight = grid.empty() ? 0 : grid.front().size();
            UAB_MESSAGE(
                "GRID ERROR: bad at(%d,%d), logical=%dx%d stored=%dx%d",
                x, y, width, height, int(grid.size()), int(storedHeight));
        }
        return -1;
    }

    return grid[x][y];
}

int Grid::at(int x, int y) const
{
    return get(x, y);
}

int Grid::at(const BWAPI::TilePosition & pos) const
{
    return at(pos.x, pos.y);
}

int Grid::at(const BWAPI::WalkPosition & pos) const
{
    return at(BWAPI::TilePosition(pos));
}

int Grid::at(const BWAPI::Position & pos) const
{
    return at(BWAPI::TilePosition(pos));
}

int Grid::at(BWAPI::Unit unit) const
{
    return at(unit->getTilePosition());
}

// Check the correct shape of the data structure.
void Grid::selfTest(const std::string & message) const
{
    UAB_ASSERT(width > 0 && width < 256 && height > 0 && height < 256 && grid.size() == width,
        "GRID ERROR: %s bad size %dx%d (size %d)", message.c_str(), width, height, grid.size());
    for (size_t x = 0; x < grid.size(); ++x)
    {
        UAB_ASSERT(grid[x].size() == height,
            "GRID ERROR: %s bad grid[%d] height %d in %dx%d",
            message.c_str(), x, grid[x].size(), width, height);
    }
}

// Draw a number in each tile.
// This default method is overridden in some subclasses.
void Grid::draw() const
{
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            int n = grid[x][y];
            if (n)
            {
                char color = n < 0 ? purple : gray;
                BWAPI::Broodwar->drawTextMap(
                    BWAPI::Position(x * 32 + 8, y * 32 + 8),
                    "%c%d", color, n);
            }
        }
    }
}