#pragma once

#include "Core/Config.h"
#include "Core/Common.h"
#include "../../../BOSS/source/Timer.hpp"

namespace UAlbertaBot
{

struct Timer
{
    std::string name;
    BOSS::Timer timer;
    double highWaterMark;    // maximum milliseconds seen

    Timer();
    Timer(std::string n);

    void start()             { timer.start(); };
    void stop()              { timer.stop(); };
    double getMilliseconds() { return timer.getElapsedTimeInMilliSec(); };
};

class TimerManager
{
public:

    enum Type { Total, Assignments, Information, The, OpponentModel, Search, Worker, Production, Building, Combat, Micro, Tasks, NumTypes };

private:

    std::map<Type, Timer> _timers;

    int _count;
    double _maxMilliseconds;
    double _totalMilliseconds;
    int _lastSpikeFrame;
    double _lastSpikeMilliseconds;
    Type _lastSpikeModule;
    double _lastSpikeModuleMilliseconds;

    const int _barWidth;

public:

    TimerManager();

    void startTimer(TimerManager::Type t);

    void stopTimer(TimerManager::Type t);

    double getMilliseconds();      // total elapsed time
    double getMaxMilliseconds();   // over all frames
    double getMeanMilliseconds();  // over all frames
    int getLastSpikeFrame() const { return _lastSpikeFrame; }
    double getLastSpikeMilliseconds() const { return _lastSpikeMilliseconds; }
    const char * getLastSpikeModuleName() const;
    double getLastSpikeModuleMilliseconds() const { return _lastSpikeModuleMilliseconds; }

    void drawModuleTimers(int x, int y);
};

}
