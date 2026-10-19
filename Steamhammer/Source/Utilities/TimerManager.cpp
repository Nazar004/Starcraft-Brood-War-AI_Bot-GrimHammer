#include "Utilities/TimerManager.h"
#include <iomanip>
#include <ostream>

using namespace UAlbertaBot;

Timer::Timer()
    : name("")
    , highWaterMark(0)
    , accumulatedMilliseconds(0)
    , samples(0)
{
}

Timer::Timer(std::string n)
    : name(n)
    , highWaterMark(0)
    , accumulatedMilliseconds(0)
    , samples(0)
{
}

TimerManager::TimerManager() 
    : _count(0)
    , _maxMilliseconds(0.0)
    , _totalMilliseconds(0.0)
    , _framesOver55(0)
    , _framesOver100(0)
    , _lastSpikeFrame(-1)
    , _lastSpikeMilliseconds(0.0)
    , _lastSpikeModule(Total)
    , _lastSpikeModuleMilliseconds(0.0)
    , _barWidth(50)
{
    _timers[TimerManager::Type::Total]         = Timer("Total");
    _timers[TimerManager::Type::Assignments]   = Timer("Assign");

    _timers[TimerManager::Type::Information]   = Timer("Info");
    _timers[TimerManager::Type::The]           = Timer("The");
    _timers[TimerManager::Type::OpponentModel] = Timer("Opponent");

    _timers[TimerManager::Type::Search]        = Timer("Search");
    _timers[TimerManager::Type::Worker]        = Timer("Worker");
    _timers[TimerManager::Type::Production]    = Timer("Produce");
    _timers[TimerManager::Type::Building]      = Timer("Build");
    _timers[TimerManager::Type::Combat]        = Timer("Combat");
    _timers[TimerManager::Type::Micro]         = Timer("Micro");
    _timers[TimerManager::Type::Tasks]         = Timer("Tasks");
    _timers[TimerManager::Type::Debug]         = Timer("Debug");
}

void TimerManager::startTimer(TimerManager::Type t)
{
    _timers[t].start();
}

void TimerManager::stopTimer(TimerManager::Type t)
{
    Timer& measured = _timers[t];
    measured.timer.stop();
    const double elapsed = measured.getMilliseconds();
    measured.highWaterMark = std::max(measured.highWaterMark, elapsed);
    measured.accumulatedMilliseconds += elapsed;
    ++measured.samples;
    if (t == Total)
    {
        ++_count;
        double ms = getMilliseconds();
        _maxMilliseconds = std::max(_maxMilliseconds, ms);
        _totalMilliseconds += ms;
        if (ms > 55.0) ++_framesOver55;
        if (ms > 100.0) ++_framesOver100;
        if (ms >= 40.0 && ms > _lastSpikeMilliseconds)
        {
            _lastSpikeFrame = BWAPI::Broodwar->getFrameCount();
            _lastSpikeMilliseconds = ms;
            _lastSpikeModule = Total;
            _lastSpikeModuleMilliseconds = 0.0;
            double measuredModules = 0.0;
            std::vector<std::pair<double, std::string>> modules;
            for (auto & entry : _timers)
            {
                if (entry.first == Total) continue;
                const double elapsed = entry.second.timer.getElapsedTimeInMilliSec();
                measuredModules += elapsed;
                modules.emplace_back(elapsed, entry.second.name);
                if (elapsed > _lastSpikeModuleMilliseconds)
                {
                    _lastSpikeModule = entry.first;
                    _lastSpikeModuleMilliseconds = elapsed;
                }
            }
            const double other = std::max(0.0, ms - measuredModules);
            modules.emplace_back(other, "Other");
            std::sort(modules.begin(), modules.end(),
                [](const std::pair<double, std::string>& a,
                   const std::pair<double, std::string>& b) { return a.first > b.first; });
            std::ostringstream summary;
            summary << std::fixed << std::setprecision(1);
            for (size_t i = 0; i < std::min<size_t>(4, modules.size()); ++i)
            {
                if (i != 0) summary << ' ';
                summary << modules[i].second << ' ' << modules[i].first;
            }
            _lastSpikeModuleBreakdown = summary.str();
            if (other > _lastSpikeModuleMilliseconds)
            {
                _lastSpikeModule = NumTypes;
                _lastSpikeModuleMilliseconds = other;
            }
        }
    }
}

double TimerManager::getMilliseconds()
{
    return _timers[Total].getMilliseconds();
}

double TimerManager::getMaxMilliseconds()
{
    return _maxMilliseconds;
}

double TimerManager::getMeanMilliseconds()
{
    if (_count == 0)
    {
        return 0.0;
    }
    return _totalMilliseconds / _count;
}

void TimerManager::writeGameSummary(std::ostream& output) const
{
    output << std::fixed << std::setprecision(2);
    output << "frames=" << _count << " mean_ms="
        << (_count > 0 ? _totalMilliseconds / _count : 0.0)
        << " max_ms=" << _maxMilliseconds
        << " over_55ms=" << _framesOver55
        << " over_100ms=" << _framesOver100 << '\n';
    output << "module mean_ms max_ms samples\n";
    for (const auto& entry : _timers)
    {
        const Timer& timer = entry.second;
        output << timer.name << ' '
            << (timer.samples > 0
                ? timer.accumulatedMilliseconds / timer.samples : 0.0)
            << ' ' << timer.highWaterMark << ' ' << timer.samples << '\n';
    }
}

std::string TimerManager::currentBreakdown()
{
    const double total = getMilliseconds();
    double measured = 0.0;
    std::vector<std::pair<double, std::string>> modules;
    for (auto& entry : _timers)
    {
        if (entry.first == Total) continue;
        const double elapsed = entry.second.getMilliseconds();
        measured += elapsed;
        modules.emplace_back(elapsed, entry.second.name);
    }
    modules.emplace_back(std::max(0.0, total - measured), "Other");
    std::sort(modules.begin(), modules.end(),
        [](const std::pair<double, std::string>& a,
           const std::pair<double, std::string>& b) { return a.first > b.first; });
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(1);
    for (size_t i = 0; i < std::min<size_t>(5, modules.size()); ++i)
    {
        if (i != 0) summary << ' ';
        summary << modules[i].second << ' ' << modules[i].first;
    }
    return summary.str();
}

void TimerManager::resetStatistics()
{
    _count = 0;
    _maxMilliseconds = 0.0;
    _totalMilliseconds = 0.0;
    _framesOver55 = 0;
    _framesOver100 = 0;
    _lastSpikeFrame = -1;
    _lastSpikeMilliseconds = 0.0;
    _lastSpikeModule = Total;
    _lastSpikeModuleMilliseconds = 0.0;
    _lastSpikeModuleBreakdown.clear();
    for (auto& entry : _timers)
    {
        entry.second.highWaterMark = 0.0;
        entry.second.accumulatedMilliseconds = 0.0;
        entry.second.samples = 0;
    }
}

const char * TimerManager::getLastSpikeModuleName() const
{
    if (_lastSpikeModule == NumTypes) return "Other";
    const auto found = _timers.find(_lastSpikeModule);
    return found == _timers.end() ? "Unknown" : found->second.name.c_str();
}

void TimerManager::drawModuleTimers(int x, int y)
{
    if (!Config::Debug::DrawModuleTimers)
    {
        return;
    }

    BWAPI::Broodwar->drawBoxScreen(x-3, y-3, x+102+_barWidth, y+3+(10*_timers.size()), BWAPI::Colors::Black, true);

    int yskip = 0;
    double total = _timers[Total].getMilliseconds();
    for (std::pair<const TimerManager::Type, Timer> & kv : _timers)
    {
        Timer & t = kv.second;
        double elapsed = t.getMilliseconds();
        if (elapsed > 55)
        {
            BWAPI::Broodwar->printf("Timer Debug: %s %lf", t.name.c_str(), elapsed);
        }

        int width = (total == 0) ? 0 : int(_barWidth * (elapsed / total));

        BWAPI::Broodwar->drawTextScreen(x-4, y+yskip-2, "\x04 %s", t.name.c_str());
        BWAPI::Broodwar->drawBoxScreen(x+52, y+yskip+1, x+52+width+1, y+yskip+9, BWAPI::Colors::White);
        BWAPI::Broodwar->drawTextScreen(x+59+_barWidth, y+yskip-2, "%.1lf", elapsed);
        BWAPI::Broodwar->drawTextScreen(x+80+_barWidth, y+yskip-2, "%4.1lf", t.highWaterMark);
        yskip += 10;
    }
}
