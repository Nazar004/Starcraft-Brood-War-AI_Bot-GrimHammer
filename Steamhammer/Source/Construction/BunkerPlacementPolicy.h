#pragma once

#include <algorithm>
#include <cmath>

namespace UAlbertaBot
{
namespace BunkerPlacementPolicy
{
    // Project the whole 3x2 footprint, leaving an 80-pixel traffic corridor.
    // Connectivity remains a separate hard check in BuildingPlacer.
    inline double sideHalfSize(double forwardX, double forwardY)
    {
        return std::abs(forwardY) * 48.0 + std::abs(forwardX) * 32.0;
    }

    inline double shoulderTiles(double widthTiles, double forwardX, double forwardY)
    {
        return std::max(widthTiles * 0.5 + 0.25,
            (sideHalfSize(forwardX, forwardY) + 40.0) / 32.0);
    }

    inline bool nearEntrance(double forwardPixels, double sidePixels,
        double widthTiles, double forwardX, double forwardY)
    {
        const double halfSide = sideHalfSize(forwardX, forwardY);
        const double maxRadius = 32.0 * std::min(5.5, std::max(4.0, widthTiles * 0.5 + 1.5));
        return forwardPixels >= -80.0 && forwardPixels <= 16.0 &&
            std::abs(sidePixels) - halfSide >= 40.0 &&
            std::hypot(forwardPixels, sidePixels) <= maxRadius;
    }
}
}
