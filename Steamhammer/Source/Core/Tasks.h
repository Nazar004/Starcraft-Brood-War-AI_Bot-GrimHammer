#pragma once

#include "Core/Task.h"

#include <map>

namespace UAlbertaBot
{
class GameRecord;

class Tasks
{
private:
    std::map<std::string, Task *> tasks;
    int _slowestTaskFrame = -1;
    std::string _slowestTaskName;
    double _slowestTaskMilliseconds = 0.0;

public:

    ~Tasks() { clear(); }

    void post(Task * Task);
    void erase(const std::string & name);
    void clear();

    void update();
    const std::string & slowestTaskName(int frame) const;
    double slowestTaskMilliseconds(int frame) const;

    size_t size() const { return tasks.size(); };
    Task * get(const std::string & name) const;

    void draw(int x, int y) const;
};

}
