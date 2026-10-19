# TvZ bio plans

Each strategy has its own header containing production targets, first-push
requirements, and transition conditions. These are inline headers, so they do
not need new .cpp entries in the Visual Studio project.

| JSON strategy / plan | Policy file | First push |
| --- | --- | --- |
| TvZ_Sparks | TvZSparks.h | 20 Marines, 4 Medics, Stim |
| TvZ_PreHive | TvZPreHive.h | 24 Marines, 5 Medics, 3 Tanks, 1 Vessel, Stim, Siege, Range, Irradiate |
| TvZ_SK | TvZSK.h | 24 Marines, 5 Medics, 1 Vessel, Stim, Range, Irradiate |

Counts refer to completed units. Both Tank modes count. After a first release,
reinforcements do not have to rebuild the original army to keep attacking.
Owned-base threats still take priority. SK holds its attack after losing all
Vessels against known Lurkers, while its production lane rebuilds them.

Sparks uses three Barracks on one base, without Factory/Siege/Vessel investment.
It commits to BioTank at observed Lair combat tech, at seven minutes, or sixty
seconds after its first attack permission. Economy expansion is then released.

Pre-Hive uses three/five Barracks and one Tank Factory. It transitions to SK at
Hive/Defiler evidence or ninety seconds after first attack permission. If the
first attack has not started by eleven minutes and Vessel/Irradiate is missing,
it commits to BioTank so the failed support timing cannot hold the army forever.

SK builds one Factory for Starport prerequisites, zero Machine Shops and zero
new Tanks. Existing Tanks from Pre-Hive remain usable. At two bases and 24
Marines it uses two Starports; Vessel targets grow with infantry up to ten.

TvZBioPlan.h contains the common value contract. TvZBioSupport.h contains the
staged Factory -> Starport -> Tower/Science Facility -> Vessel/Irradiate adapter,
used only by Pre-Hive and SK. StrategyManager registers these plans and adapts
their policy to the existing bio macro rules. Existing BioTank, Deep Six, mech,
combat simulation, and unit micro keep their existing strategy policies.

The live books are in D:/DU/BP/Starcraft/Starcraft_1161/bwapi-data/AI/GrimHammer.json.
All three are TvZ-only and participate in ordinary opening selection. Rush and
contain counter pools keep their previous books. The pre-change live JSON is
backed up under Source/.verification/tvz-bio-plans/.

Initial tuning needs in-game review: first attack times, Vessel survival,
Irradiate usage, and engagements with Lurker lines. Shared combat micro remains
in use; these changes do not implement professional SK split-army control.
No build or game was run for this change.
