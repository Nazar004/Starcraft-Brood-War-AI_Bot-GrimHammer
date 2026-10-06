#include "Core/Tasks.h"
#include "Core/The.h"
#include <chrono>

using namespace UAlbertaBot;

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// Usage: post(new WhateverTask(...)).
// Start a new task.
void Tasks::post(Task * task)
{
    if (task->enabled() &&
		!get(task->getName()))		// not already started
    {
        task->initialize();
        tasks[task->getName()] = task;
    }
    else
    {
        delete task;
    }
}

// Delete the given task by name.
void Tasks::erase(const std::string & name)
{
	Task * task = get(name);
	if (task)
	{
		delete task;
	}
    tasks.erase(name);
}

void Tasks::clear()
{
    for (auto & item : tasks)
    {
        delete item.second;
    }
    tasks.clear();
}

void Tasks::update()
{
    _slowestTaskFrame = the.now();
    _slowestTaskName.clear();
    _slowestTaskMilliseconds = 0.0;
    for (auto it = tasks.begin(); it != tasks.end(); )
    {
        Task * task = it->second;
        if (task->nextUpdate() <= the.now())
        {
            const auto started = std::chrono::steady_clock::now();
            task->update();
            const double milliseconds =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
            if (milliseconds > _slowestTaskMilliseconds)
            {
                _slowestTaskMilliseconds = milliseconds;
                _slowestTaskName = task->getName();
            }
            if (task->completed())
            {
                delete task;
                it = tasks.erase(it);
                continue;
            }
        }
        ++it;
    }
}

Task * Tasks::get(const std::string & name) const
{
    for (const std::pair<const std::string, Task *> & item : tasks)
    {
        Task * task = item.second;
        if (task->getName() == name)
        {
            return task;
        }
    }

    return nullptr;
}

// Draw debugging info.
// Draw a table of what tasks exist.
// Then each task decides for itself whether to draw its own info, possibly controlled by different flags.
void Tasks::draw(int x, int y) const
{
    if (Config::Debug::DrawTasks)
    {
        for (const std::pair<const std::string, Task *> & item : tasks)
        {
            const std::string & name = item.first;
            const Task * task = item.second;
            BWAPI::Broodwar->drawTextScreen(x     , y, "%c%d", yellow, task->nextUpdate());
            BWAPI::Broodwar->drawTextScreen(x + 40, y, "%c%s", white , name.c_str());
            y += 10;
        }
    }

    for (const std::pair<const std::string, Task *> & item : tasks)
    {
        item.second->draw();
    }
}

const std::string & Tasks::slowestTaskName(int frame) const
{
    static const std::string empty;
    return _slowestTaskFrame == frame ? _slowestTaskName : empty;
}

double Tasks::slowestTaskMilliseconds(int frame) const
{
    return _slowestTaskFrame == frame ? _slowestTaskMilliseconds : 0.0;
}
