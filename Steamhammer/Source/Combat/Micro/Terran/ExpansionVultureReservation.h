#pragma once

#include <BWAPI.h>

namespace UAlbertaBot
{
namespace ExpansionVultureReservation
{
    bool canReserve(BWAPI::Unit unit);
    bool reserve(BWAPI::Unit unit);
    void release(int unitID);
}
}
