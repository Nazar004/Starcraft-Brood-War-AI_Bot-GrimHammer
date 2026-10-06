#pragma once

#include "Map/Grids/GridWalk.h"

// Orientation-independent accessible passage width, measured in 8x8 walk tiles,
// around each walk tile. Low values identify narrow terrain/chokes.

namespace UAlbertaBot
{
class GridRoom : public GridWalk
{
public:
    GridRoom();

    void initialize();
    
    void draw() const;
};
}
