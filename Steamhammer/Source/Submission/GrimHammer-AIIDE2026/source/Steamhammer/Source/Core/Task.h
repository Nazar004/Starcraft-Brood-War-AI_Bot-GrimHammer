#pragma once

#include <string>

namespace UAlbertaBot
{

class Task
{
protected:

    std::string _name;
    int _nextUpdateFrame;

public:

    Task();
    Task(const std::string & name);
    virtual ~Task() = default;

    const std::string & getName() const { return _name; };

    // Called once at startup to decide whether to run the task at all.
    virtual bool enabled() const = 0;

    // Called once if the task is enabled.
    virtual void initialize() { };

    // Update any info that feasible(), good(), execute() may want to look at.
    // Also possibly take actions. Not all skills use execute().
    virtual void update() = 0;

    // Called after each call to update. If true, delete the task.
    virtual bool completed() { return false; };

    // Tasks that want to draw debugging info should override this.
    // It will be called once per frame.
    virtual void draw() const {};

    int nextUpdate() const { return _nextUpdateFrame; };
	void setNextUpdate(int frame) { _nextUpdateFrame = frame; };
};

}