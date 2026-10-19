#pragma once

#include "Core/Task.h"

// Tasks for updating various grids.

namespace UAlbertaBot
{
// Forward declarations.
class Base;
class GridAttacks;

class GridTask : public Task
{
public:
    virtual void initialize() = 0;
    bool enabled() const { return true; };
};

class GroundHitsFixedTask : public GridTask
{
public:
    void initialize();
    void update();
    void draw() const;
};

class GroundHitsMobileTask : public GridTask
{
public:
    void initialize();
    void update();
    void draw() const;
};

class AirHitsFixedTask : public GridTask
{
public:
    void initialize();
    void update();
    void draw() const;
};

class AirHitsMobileTask : public GridTask
{
public:
    void initialize();
    void update();
    void draw() const;
};

// Update on even frames.
class GroundSafePathTask : public GridTask
{
private:
	const int UpdateStartFrame = 3;		// start once bases and threat grids are initialized
	GridAttacks * _influence;
	int _baseIndex;
public:
	GroundSafePathTask();
    ~GroundSafePathTask() override;
	void initialize() {};
    void update();
	void updateBase(Base * base);
    void draw() const;
};

// Update on odd frames.
class AirSafePathTask : public GridTask
{
	const int UpdateStartFrame = 3;		// relative scheduling takes over after the first update
	GridAttacks * _influence;
	int _baseIndex;
public:
	AirSafePathTask();
    ~AirSafePathTask() override;
	void initialize() {};
    void update();
	void updateBase(Base * base);
	void draw() const;
};

}