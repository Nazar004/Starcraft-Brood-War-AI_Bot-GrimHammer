
#pragma once

#include "Combat/Squads/Squad.h"

namespace UAlbertaBot
{
class SquadData
{
    std::map<std::string, Squad> _squads;
    SquadUpdateProfile _lastUpdateProfile;
    SquadUpdateProfile _slowestSquadProfile;
    double _slowestSquadMilliseconds = 0.0;

    void    updateAllSquads();
    void    verifySquadUniqueMembership();

public:

    SquadData();

    void            clearSquadData();

    bool            canAssignUnitToSquad(BWAPI::Unit unit, const Squad & squad) const;
    void            assignUnitToSquad(BWAPI::Unit unit, Squad & squad);
    Squad &         createSquad(const std::string & name, size_t priority);
    void            removeSquad(const std::string & squadName);
    void            drawSquadInformation(int x, int y);

    void            update();
    const SquadUpdateProfile& lastUpdateProfile() const { return _lastUpdateProfile; }
    const SquadUpdateProfile& slowestSquadProfile() const { return _slowestSquadProfile; }
    double slowestSquadMilliseconds() const { return _slowestSquadMilliseconds; }
    void            setRegroup();

    bool            squadExists(const std::string & squadName);
    bool            unitIsInSquad(BWAPI::Unit unit) const;
    const Squad *   getUnitSquad(BWAPI::Unit unit) const;
    Squad *         getUnitSquad(BWAPI::Unit unit);

    Squad &         getSquad(const std::string & squadName);
    const std::map<std::string, Squad> & getSquads() const;
};
}
