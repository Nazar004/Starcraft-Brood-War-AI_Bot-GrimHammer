#include "Map/Grids/GridTasks.h"

#include "Map/Bases.h"
#include "Core/The.h"

#include <algorithm>

using namespace UAlbertaBot;

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

void GroundHitsFixedTask::initialize()
{
    _name = "ground hits fixed";

    // Static defense can appear very early (proxy cannon/bunker/sunken).
    _nextUpdateFrame = std::max(1, the.now() + 1);
}

void GroundHitsFixedTask::update()
{
    the.groundHitsFixed.update();
    _nextUpdateFrame += 19;
}

void GroundHitsFixedTask::draw() const
{
    //the.groundHitsFixed.draw();
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

void AirHitsFixedTask::initialize()
{
    _name = "air hits fixed";

    // Air-safe routing should have a valid fixed-threat grid as soon as the
    // air tasks are enabled.
    _nextUpdateFrame = std::max(1, the.now() + 1);
}

void AirHitsFixedTask::update()
{
    the.airHitsFixed.update();
    _nextUpdateFrame += 19;
}

void AirHitsFixedTask::draw() const
{
    //the.airHitsFixed.draw();
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

void GroundHitsMobileTask::initialize()
{
    _name = "ground hits mobile";
    _nextUpdateFrame = std::max(1, the.now() + 1);
}

void GroundHitsMobileTask::update()
{
    the.groundHitsMobile.update();
    _nextUpdateFrame += 3;
}

void GroundHitsMobileTask::draw() const
{
    //the.groundHitsMobile.draw();
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

void AirHitsMobileTask::initialize()
{
    _name = "air hits mobile";
    _nextUpdateFrame = std::max(1, the.now() + 1);
}

void AirHitsMobileTask::update()
{
    the.airHitsMobile.update();
    _nextUpdateFrame += 3;
}

void AirHitsMobileTask::draw() const
{
    //the.airHitsMobile.draw();
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

GroundSafePathTask::GroundSafePathTask()
	: _influence(nullptr)
	, _baseIndex(0)
{
	_name = "ground safe path";
	_nextUpdateFrame = UpdateStartFrame;
}

GroundSafePathTask::~GroundSafePathTask()
{
    delete _influence;
    _influence = nullptr;
}

void GroundSafePathTask::update()
{
    const auto & bases = the.bases.getAll();
    if (bases.empty())
    {
        _nextUpdateFrame = the.now() + 2;
        _baseIndex = 0;
        return;
    }
    if (_baseIndex < 0 || size_t(_baseIndex) >= bases.size())
    {
        _baseIndex = 0;
    }

	Base * base = bases.at(_baseIndex);

	// Update the paths.
	updateBase(base);

	// The next update frame.
	_nextUpdateFrame = (the.now() & ~1) + 2;		// update on even frames

	size_t nBases = bases.size();
	_baseIndex = (_baseIndex + 1) % nBases;
}

// Update the paths for a given base without worrying about other details.
// This is called from outside to do updates without going through the task system.
void GroundSafePathTask::updateBase(Base * base)
{
	if (!_influence)
	{
		_influence = new GroundInfluence();
	}
	_influence->update();
	base->getGroundSafePaths().update(_influence);
}

void GroundSafePathTask::draw() const
{
    if (the.now() > UpdateStartFrame)
    {
        //the.groundSafePath.draw();
    }
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

AirSafePathTask::AirSafePathTask()
	: _influence(nullptr)
	, _baseIndex(0)
{
	_name = "air safe path";
	_nextUpdateFrame = UpdateStartFrame;
}

AirSafePathTask::~AirSafePathTask()
{
    delete _influence;
    _influence = nullptr;
}

// The first updateBase() for some bases is called before the task is started!
// It's because the task is started on demand, and the demand may come at any time.
// There needs to be an initial update before the task gets around to it.
void AirSafePathTask::update()
{
    const auto & bases = the.bases.getAll();
    if (bases.empty())
    {
        _nextUpdateFrame = the.now() + 2;
        _baseIndex = 0;
        return;
    }
    if (_baseIndex < 0 || size_t(_baseIndex) >= bases.size())
    {
        _baseIndex = 0;
    }

	Base * base = bases.at(_baseIndex);

	// Update the paths.
	updateBase(base);

	// The next update frame.
	_nextUpdateFrame = (the.now() | 1) + 2;		// update on odd frames

	// Next base to update.
	size_t nBases = bases.size();
	_baseIndex = (_baseIndex + 1) % nBases;
}

// Update the paths for a given base without worrying about other details.
// This is called from outside to do updates without going through the task system.
// It's also called straight from update().
void AirSafePathTask::updateBase(Base * base)
{
	if (!_influence)
	{
		_influence = new AirInfluence();
	}
	_influence->update();
	base->getAirSafePaths().update(_influence);
}

void AirSafePathTask::draw() const
{
    if (the.now() > UpdateStartFrame)
    {
        //the.airSafePath.draw();
    }
}
