#include "Core/Task.h"

using namespace UAlbertaBot;

// For when the task is initialized later.
Task::Task()
    : _name("")
    , _nextUpdateFrame(0)
{
}

Task::Task(const std::string & name)
    : _name(name)
    , _nextUpdateFrame(1)
{
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
