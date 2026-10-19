#pragma once

#include "Map/Grids/Grid.h"

// GridRoom reduced to 32x32 build-tile resolution using a robust median of the
// underlying walk-tile passage widths.

namespace UAlbertaBot
{
class GridTileRoom : public Grid
{
public:
    GridTileRoom();

    void initialize();
    
    void draw() const;
};
}
