#include <fstream>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <climits>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "Intelligence/Learning/OpponentModel.h"

#include "Map/Bases.h"
#include "Utilities/Random.h"
#include "Strategy/StrategyManager.h"
#include "Core/The.h"

using namespace UAlbertaBot;

namespace
{
    // A full v3 record is small, but old skill data and legacy snapshots can be larger.
    // Keeping the newest 200 games is enough for adaptation and places a hard bound on
    // both the learning file and the amount of memory used while playing.
    const size_t DefaultGameRecordLimit = 200;
    const size_t HardGameRecordLimit = 200;

    const EnemyThreat TrackedEnemyThreats[] =
    {
        EnemyThreat::GroundAir,
        EnemyThreat::AntiAirOnly,
        EnemyThreat::Spellcaster,
        EnemyThreat::Cloak,
        EnemyThreat::CapitalAir,
        EnemyThreat::Splash
    };

    size_t GameRecordLimit()
    {
        if (Config::IO::MaxGameRecords <= 0)
        {
            return DefaultGameRecordLimit;
        }

        return std::min(static_cast<size_t>(Config::IO::MaxGameRecords), HardGameRecordLimit);
    }

    bool InstallLearningFile(const std::string & temporaryPath, const std::string & finalPath)
    {
#ifdef _WIN32
        if (MoveFileExA(
            temporaryPath.c_str(),
            finalPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return true;
        }
#else
        if (std::rename(temporaryPath.c_str(), finalPath.c_str()) == 0)
        {
            return true;
        }
#endif

        std::remove(temporaryPath.c_str());
        return false;
    }
}

OpeningPlan OpponentModel::predictEnemyPlan(
    int * matchingGames,
    double * confidence) const
{
    if (matchingGames)
    {
        *matchingGames = 0;
    }
    if (confidence)
    {
        *confidence = 0.0;
    }

    // Don't bother to predict on island maps.
    // Data from other maps will only be misleading.
    if (Bases::Instance().isIslandStart())
    {
        return OpeningPlan::Unknown;
    }

    struct PlanInfoType
    {
        int wins;
        int games;
        double weight;
    };
    PlanInfoType planInfo[int(OpeningPlan::Size)];

    // 1. Initialize.
    for (int plan = int(OpeningPlan::None); plan < int(OpeningPlan::Size); ++plan)
    {
        planInfo[plan].wins = 0;
        planInfo[plan].games = 0;
        planInfo[plan].weight = 0.0;
    }

    // 2. Gather info.
    double weight = 1.0;
    double totalPlanWeight = 0.0;
    for (const GameRecord * record : _pastGameRecords)
    {
        if (sameMatchup(*record))
        {
            const OpeningPlan recordedPlan = record->getEnemyPlan();
            PlanInfoType & info = planInfo[int(recordedPlan)];
            info.games += 1;
            info.weight += weight;
            if (recordedPlan != OpeningPlan::None)
            {
                // Unknown is deliberately part of the denominator. Three
                // Wraith records among many unrecognized games are not a 100%
                // prediction and must not authorize blind preparation.
                totalPlanWeight += weight;
            }
            weight *= 1.25;        // more recent game records are more heavily weighted
        }
    }

    // 3. Select the most heavily weighted recognized plan and measure its
    // share of every recorded matchup, including unrecognized games.
    OpeningPlan bestPlan = OpeningPlan::Unknown;
    double bestWeight = 0.0;
    for (int plan = int(OpeningPlan::Unknown) + 1; plan < int(OpeningPlan::Size); ++plan)
    {
        if (planInfo[plan].weight > bestWeight)
        {
            bestPlan = OpeningPlan(plan);
            bestWeight = planInfo[plan].weight;
        }
    }

    if (bestPlan != OpeningPlan::Unknown)
    {
        if (matchingGames)
        {
            *matchingGames = planInfo[int(bestPlan)].games;
        }
        if (confidence && totalPlanWeight > 0.0)
        {
            *confidence = bestWeight / totalPlanWeight;
        }
    }

    return bestPlan;
}

// Does the opponent seem to play the same strategy every game?
// If we're pretty sure, set _singleStrategy to true.
// So far, we only check the plan. We have plenty of other data that could be helpful.
void OpponentModel::considerSingleStrategy()
{
    // Humans, we assume, are unpredictable.
    if (Config::Skills::HumanOpponent)
    {
        _singleStrategy = false;
        return;
    }

    // Gather info.
    int knownPlan = 0;
    int unknownPlan = 0;
    std::set<OpeningPlan> plansSeen;

    for (const GameRecord * record : _pastGameRecords)
    {
        if (sameMatchup(*record))
        {
            OpeningPlan plan = record->getEnemyPlan();
            if (plan == OpeningPlan::Unknown)
            {
                unknownPlan += 1;
            }
            else
            {
                knownPlan += 1;
                plansSeen.insert(plan);
            }
        }
    }

    // Decide.
    // If we don't recognize the majority of plans, we're not sure.
    if (knownPlan >= 2 && plansSeen.size() == 1 && unknownPlan <= knownPlan)
    {
        _singleStrategy = true;
    }
}

// Build the compact in-memory summary used by strategy selection and the debug UI.
// The game records themselves remain bounded by GameRecordLimit().
void OpponentModel::summarizeGames()
{
    _summary = OpponentSummary();

    // Predict the enemy's observed army dimensions. Only three enum buckets
    // are retained; no per-frame history is allocated.
    std::map<StrategyClass, double> classWeights;
    std::map<StrategyClass, int> classGames;
    double totalClassWeight = 0.0;
    std::map<EnemyThreat, double> threatWeights;
    std::map<EnemyThreat, int> threatGames;
    double totalThreatWeight = 0.0;
    double totalPlanWeight = 0.0;
    double earlyGroundRushWeight = 0.0;
    double recencyWeight = 1.0;
    for (const GameRecord * record : _pastGameRecords)
    {
        if (!sameMatchup(*record))
        {
            continue;
        }

        const OpeningPlan recordedPlan = record->getEnemyPlan();
        if (recordedPlan != OpeningPlan::None)
        {
            totalPlanWeight += recencyWeight;
        }
        if (recordedPlan == OpeningPlan::FastRush ||
            recordedPlan == OpeningPlan::HeavyRush)
        {
            ++_summary.earlyGroundRushGames;
            earlyGroundRushWeight += recencyWeight;
        }


        const StrategyClass strategyClass = record->getStrategyClass();
        if (strategyClass != StrategyClass::Unknown)
        {
            classWeights[strategyClass] += recencyWeight;
            classGames[strategyClass] += 1;
            totalClassWeight += recencyWeight;
        }

        // Old 3.3-and-earlier records did not contain this line. Do not treat
        // their missing mask as evidence that the opponent had no threats.
        if (record->hasEnemyThreatProfile())
        {
            totalThreatWeight += recencyWeight;
            const EnemyThreatMask mask = record->getEnemyThreats();
            for (EnemyThreat threat : TrackedEnemyThreats)
            {
                if (HasEnemyThreat(mask, threat))
                {
                    threatWeights[threat] += recencyWeight;
                    threatGames[threat] += 1;
                }
            }
        }
        recencyWeight *= 1.25;
    }

    _summary.earlyGroundRushConfidence = totalPlanWeight > 0.0
        ? earlyGroundRushWeight / totalPlanWeight
        : 0.0;

    StrategyClass likelyStrategyClass = StrategyClass::Unknown;
    double bestClassWeight = 0.0;
    for (const auto & item : classWeights)
    {
        if (item.second > bestClassWeight)
        {
            likelyStrategyClass = item.first;
            bestClassWeight = item.second;
        }
    }

    _summary.likelyStrategyClass = likelyStrategyClass;
    if (likelyStrategyClass != StrategyClass::Unknown)
    {
        _summary.likelyStrategyClassGames = classGames[likelyStrategyClass];
        _summary.likelyStrategyClassConfidence =
            totalClassWeight > 0.0 ? bestClassWeight / totalClassWeight : 0.0;
    }

    // Threats are not mutually exclusive: Carrier contributes GroundAir and
    // CapitalAir, while Arbiter contributes GroundAir/Cloak/Spellcaster. Each
    // flag therefore receives its own confidence instead of competing for one
    // winning bucket like StrategyClass.
    for (EnemyThreat threat : TrackedEnemyThreats)
    {
        const int games = threatGames[threat];
        const double confidence = totalThreatWeight > 0.0
            ? threatWeights[threat] / totalThreatWeight
            : 0.0;

        _summary.enemyThreatGames[threat] = games;
        _summary.enemyThreatConfidence[threat] = confidence;

        if (games >= 3 && confidence >= 0.70)
        {
            _summary.likelyEnemyThreats |= EnemyThreatBit(threat);
        }
    }

    for (const GameRecord * record : _pastGameRecords)
    {
        if (sameMatchup(*record))
        {
            ++_summary.totalGames;
            if (record->getWin())
            {
                ++_summary.totalWins;
            }
            OpeningInfoType & info = _summary.openingInfo[record->getOpeningName()];
            if (record->getMapName() == BWAPI::Broodwar->mapFileName())
            {
                info.sameGames += 1;
                if (record->getWin())
                {
                    info.sameWins += 1;
                }
            }
            else
            {
                info.otherGames += 1;
                if (record->getWin())
                {
                    info.otherWins += 1;
                }
            }
            if (likelyStrategyClass != StrategyClass::Unknown &&
                record->getStrategyClass() == likelyStrategyClass)
            {
                info.classGames += 1;
                if (record->getWin())
                {
                    info.classWins += 1;
                }
            }
            if (record->getExpectedEnemyPlan() == record->getEnemyPlan())
            {
                // The plan was recorded as correctly predicted in that game.
                _summary.planInfo.sameGames += 1;
                if (record->getWin())
                {
                    _summary.planInfo.sameWins += 1;
                }
            }
            else
            {
                // The plan was not correctly predicted.
                _summary.planInfo.otherGames += 1;
                if (record->getWin())
                {
                    _summary.planInfo.otherWins += 1;
                }
            }
        }
    }

    UAB_ASSERT(_summary.totalWins == _summary.planInfo.sameWins + _summary.planInfo.otherWins, "bad total");
    UAB_ASSERT(_summary.totalGames == _summary.planInfo.sameGames + _summary.planInfo.otherGames, "bad total");

    // A result on the current map counts twice. A result from the likely adaptive
    // class gets one extra vote, enough to help without fragmenting sparse data.
    // Laplace smoothing is applied later, when current JSON openings are known.
    for (auto & item : _summary.openingInfo)
    {
        OpeningInfoType & info = item.second;
        info.weightedWins =
            2.0 * info.sameWins + info.otherWins + info.classWins;
        info.weightedGames =
            2.0 * info.sameGames + info.otherGames + info.classGames;
    }
}

// Choose only among strategies which were successfully parsed from the current JSON.
// Most games exploit the best observed strategy. A small amount of exploration is
// retained so the bot can discover a better counter after either bot changes.
void OpponentModel::selectOpening(const std::vector<std::string> & availableOpenings)
{
    _recommendedOpening.clear();

    if (!Config::IO::ReadOpponentModel || availableOpenings.empty())
    {
        return;
    }

    const std::set<std::string> available(availableOpenings.begin(), availableOpenings.end());
    std::map<std::string, int> lastUsed;
    int totalObservedGames = 0;
    for (size_t i = 0; i < _pastGameRecords.size(); ++i)
    {
        const GameRecord * record = _pastGameRecords[i];
        if (sameMatchup(*record) && available.find(record->getOpeningName()) != available.end())
        {
            lastUsed[record->getOpeningName()] = static_cast<int>(i);
            ++totalObservedGames;
        }
    }

    // Do not overfit game one. Until we have a few opponent-specific samples,
    // let the configured matchup/counter mix generate the initial exploration.
    // Afterwards return to that safe, race-specific mix every fifth game. UCB
    // below can only score openings which have already been played, so without
    // this scheduled exploration an old Tanks record permanently prevents a
    // newly added JSON opening from ever receiving its first sample.
    // Selecting a dedicated "Counter <plan>" JSON is an expensive, early
    // commitment. Use it only when the same prediction which drives Terran
    // preparation has passed the 3-game/70% confidence gate.
    const bool allowPredictedCounter =
        the.selfRace() != BWAPI::Races::Terran ||
        stronglyPredictsEnemyPlan(_expectedEnemyPlan);
    const OpeningPlan counterPlan = allowPredictedCounter
        ? _expectedEnemyPlan
        : OpeningPlan::Unknown;
    const std::string counterOpening = getOpeningForEnemyPlan(counterPlan);
    const bool scheduledExploration =
        totalObservedGames >= 3 &&
        (totalObservedGames + 1) % 5 == 0;

    if (totalObservedGames < 3 || scheduledExploration)
    {
        _recommendedOpening = counterOpening.empty() ? "matchup" : counterOpening;
        return;
    }

    std::string bestOpening;
    double bestScore = -1.0;
    int bestRecent = INT_MAX;

    // UCB1-style opening selection. We intentionally consider only openings
    // that have actually occurred in this matchup; the JSON matchup mix remains
    // responsible for safely introducing brand-new openings.
    const double exploration = 0.60;
    for (const auto & item : _summary.openingInfo)
    {
        if (available.find(item.first) == available.end())
        {
            continue;
        }

        const OpeningInfoType & info = item.second;
        const int games = info.sameGames + info.otherGames;
        if (games <= 0)
        {
            continue;
        }

        const double mean =
            (info.weightedWins + 1.0) / (info.weightedGames + 2.0);
        const double bonus = exploration * std::sqrt(
            std::log(double(totalObservedGames) + 1.0) /
            std::max(0.5, info.weightedGames));
        const double score = mean + bonus;
        const int recent = lastUsed[item.first];

        if (score > bestScore + 0.000001 ||
            (std::fabs(score - bestScore) <= 0.000001 && recent < bestRecent))
        {
            bestOpening = item.first;
            bestScore = score;
            bestRecent = recent;
        }
    }

    _recommendedOpening = bestOpening.empty()
        ? (counterOpening.empty() ? "matchup" : counterOpening)
        : bestOpening;
}

// Choose a long-lived Terran Plan with UCB1. The default Plan is always
// eligible. A specialized Plan becomes eligible only after this opponent has
// repeatedly forced us into it (final-plan evidence) or after it has already
// been explicitly tried as an initial Plan. This avoids suicidal blind
// exploration such as opening AntiCarrier against an opponent never seen air.
std::string OpponentModel::recommendTerranPlan(
    const std::string & defaultPlan,
    const std::vector<std::string> & allowedPlans)
{
    _recommendedTerranPlan = defaultPlan;
    _terranPlanLearningText.clear();

    if (!Config::IO::ReadOpponentModel || allowedPlans.empty())
    {
        return _recommendedTerranPlan;
    }

    struct PlanStats
    {
        int wins = 0;
        int games = 0;
        int mapWins = 0;
        int mapGames = 0;
        int finalGames = 0;
        int lastUsed = -1;
    };

    const std::set<std::string> allowed(allowedPlans.begin(), allowedPlans.end());
    std::map<std::string, PlanStats> stats;
    int planAwareGames = 0;

    for (size_t i = 0; i < _pastGameRecords.size(); ++i)
    {
        const GameRecord * record = _pastGameRecords[i];
        if (!sameMatchup(*record))
        {
            continue;
        }

        const std::string & initialPlan = record->getInitialTerranPlan();
        const std::string & finalPlan = record->getFinalTerranPlan();
        if ((initialPlan != "None" && !initialPlan.empty()) ||
            (finalPlan != "None" && !finalPlan.empty()))
        {
            ++planAwareGames;
        }

        if (allowed.find(finalPlan) != allowed.end())
        {
            ++stats[finalPlan].finalGames;
        }

        if (allowed.find(initialPlan) == allowed.end() || initialPlan == "None")
        {
            continue;
        }

        PlanStats & p = stats[initialPlan];
        ++p.games;
        p.lastUsed = static_cast<int>(i);
        if (record->getWin())
        {
            ++p.wins;
        }
        if (record->getMapName() == BWAPI::Broodwar->mapFileName())
        {
            ++p.mapGames;
            if (record->getWin())
            {
                ++p.mapWins;
            }
        }
    }

    if (planAwareGames < 2)
    {
        _terranPlanLearningText = "UCB: collecting plan samples";
        return _recommendedTerranPlan;
    }

    std::vector<std::string> eligible;
    for (const std::string & plan : allowedPlans)
    {
        const PlanStats & p = stats[plan];
        const bool defaultArm = plan == defaultPlan;
        const bool triedArm = p.games > 0;
        const bool repeatedlyForced =
            p.finalGames >= 2 && 5 * p.finalGames >= 2 * planAwareGames; // >=40%

        // Carrier openings are expensive to discover late and the selected
        // TvP_AntiCarrier plan still contains a live-game ground-danger escape.
        // One AntiCarrier ending in the first two plan-aware games is therefore
        // enough for one deliberate pre-emptive trial. Other specialized plans
        // retain the stricter two-game evidence requirement.
        const bool earlyAntiCarrierEvidence =
            plan == "TvP_AntiCarrier" &&
            p.finalGames >= 1 &&
            2 * p.finalGames >= planAwareGames; // >=50%

        if (defaultArm || triedArm || repeatedlyForced || earlyAntiCarrierEvidence)
        {
            eligible.push_back(plan);
        }
    }

    if (eligible.empty())
    {
        eligible.push_back(defaultPlan);
    }

    int totalTrials = 0;
    for (const std::string & plan : eligible)
    {
        totalTrials += stats[plan].games;
    }
    totalTrials = std::max(1, totalTrials);

    const double exploration = 0.65;
    double bestScore = -1.0;
    std::string bestPlan = defaultPlan;
    int bestLastUsed = INT_MAX;

    for (const std::string & plan : eligible)
    {
        const PlanStats & p = stats[plan];
        double score;

        if (p.games == 0)
        {
            // A Plan became eligible because the enemy repeatedly forced that
            // transition. Give it one deliberate pre-emptive trial.
            score = 1000.0 + 0.001 * p.finalGames;
        }
        else
        {
            // Same-map games receive a half extra vote in the posterior mean,
            // while the UCB exploration denominator remains real game count.
            const double weightedWins = p.wins + 0.5 * p.mapWins;
            const double weightedGames = p.games + 0.5 * p.mapGames;
            const double mean = (weightedWins + 1.0) / (weightedGames + 2.0);
            const double bonus = exploration * std::sqrt(
                std::log(double(totalTrials) + 1.0) / double(p.games));
            const double forcedEvidence = planAwareGames > 0
                ? 0.05 * double(p.finalGames) / double(planAwareGames)
                : 0.0;
            score = mean + bonus + forcedEvidence;
        }

        const int last = p.lastUsed < 0 ? -1 : p.lastUsed;
        if (score > bestScore + 0.000001 ||
            (std::fabs(score - bestScore) <= 0.000001 && last < bestLastUsed))
        {
            bestScore = score;
            bestPlan = plan;
            bestLastUsed = last;
        }
    }

    _recommendedTerranPlan = bestPlan;
    const PlanStats & chosen = stats[bestPlan];
    std::ostringstream out;
    out << "UCB start " << bestPlan << ' ' << chosen.wins << '-' <<
        (chosen.games - chosen.wins) << " final " << chosen.finalGames << '/' << planAwareGames;
    _terranPlanLearningText = out.str();
    return _recommendedTerranPlan;
}

// Possibly update the expected enemy plan.
// This runs later in the game, when we may have more information.
void OpponentModel::reconsiderEnemyPlan()
{
    if (_planRecognizer.getPlan() != OpeningPlan::Unknown)
    {
        // We already know the actual plan. No need to form an expectation.
        return;
    }

    if (!_gameRecord.getEnemyIsRandom() || the.enemy()->getRace() == BWAPI::Races::Unknown)
    {
        // For now, we only update the expected plan if the enemy went random
        // and we have now learned its race. 
        // The new information should narrow down the possibilities.
        return;
    }

    // Don't reconsider too often.
    if (BWAPI::Broodwar->getFrameCount() % 12 != 8)
    {
        return;
    }

    // We set the new expected plan even if it is Unknown. Better to know that we don't know.
    _expectedEnemyPlan = predictEnemyPlan(
        &_expectedEnemyPlanGames,
        &_expectedEnemyPlanConfidence);
}

// Find the past game record which best matches the current game and remember it.
void OpponentModel::setBestMatch()
{
    int bestScore = -1;
    GameRecord * bestRecord = nullptr;

    for (GameRecord * record : _pastGameRecords)
    {
        int score = _gameRecord.distance(*record);
        if (score != -1 && (!bestRecord || score < bestScore))
        {
            bestScore = score;
            bestRecord = record;
        }
    }

    _bestMatch = bestRecord;
}

// We expect the enemy to follow the given opening plan.
// Recommend an opening to counter that plan.
// The counters are configured; all we have to do is name the strategy mix.
// The empty opening "" means play the regular openings, no plan recognized.
std::string OpponentModel::getOpeningForEnemyPlan(OpeningPlan enemyPlan)
{
    if (enemyPlan == OpeningPlan::Unknown)
    {
        return "";
    }
    return "Counter " + OpeningPlanString(enemyPlan);
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

OpponentModel::OpponentModel()
    : _writeCompleted(false)
    , _bestMatch(nullptr)
    , _singleStrategy(false)
    , _initialExpectedEnemyPlan(OpeningPlan::Unknown)
    , _expectedEnemyPlan(OpeningPlan::Unknown)
    , _expectedEnemyPlanGames(0)
    , _expectedEnemyPlanConfidence(0.0)
    , _lastAirTimingEnemyPlan(OpeningPlan::None)
    , _lastAirTimingReturnValue(0)
{
    refreshFilename();
}

void OpponentModel::refreshFilename()
{
    std::string name = the.enemy()->getName();

    // Keep the legacy space replacement, and also block path separators and
    // characters which are illegal in Windows filenames.
    const std::string invalidFilenameCharacters = "\\/:*?\"<>|";
    for (char & ch : name)
    {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (std::isspace(value) || std::iscntrl(value) ||
            invalidFilenameCharacters.find(ch) != std::string::npos)
        {
            ch = '_';
        }
    }
    if (name.empty())
    {
        name = "unknown";
    }
    if (name.length() > 80)
    {
        name.resize(80);
    }

    _filename = "GrimHammer_" + name + "_inf.txt";
}

OpponentModel::~OpponentModel()
{
    clearPastGameRecords();
}

void OpponentModel::clearPastGameRecords()
{
    for (GameRecord * record : _pastGameRecords)
    {
        delete record;
    }
    _pastGameRecords.clear();
}

// Read one candidate learning file. Only the newest bounded set is retained,
// even if an old installation left behind a much larger file.
bool OpponentModel::readRecords(const std::string & path)
{
    std::ifstream inFile(path);
    if (!inFile.is_open())
    {
        return false;
    }

    std::deque<GameRecord *> recentRecords;
    const size_t recordLimit = GameRecordLimit();
    while (inFile.peek() != std::char_traits<char>::eof())
    {
        GameRecord * record = new GameRecord(inFile);
        if (record->isValid())
        {
            recentRecords.push_back(record);
            if (recentRecords.size() > recordLimit)
            {
                delete recentRecords.front();
                recentRecords.pop_front();
            }
        }
        else
        {
            delete record;
        }
    }

    if (recentRecords.empty())
    {
        return false;
    }

    clearPastGameRecords();
    _pastGameRecords.assign(recentRecords.begin(), recentRecords.end());
    return true;
}

// Read past game records from the opponent model file, and do initial analysis.
void OpponentModel::read()
{
    // Batch 7: every ladder game gets a genuinely fresh live model even if the
    // DLL stays loaded. Without this, game two could keep _writeCompleted=true,
    // the first opponent's filename and the first GameRecordNow.
    clearPastGameRecords();
    _gameRecord.resetForNewGame();
    _planRecognizer = OpponentPlan();
    _writeCompleted = false;
    _bestMatch = nullptr;
    _summary = OpponentSummary();
    _singleStrategy = false;
    _initialExpectedEnemyPlan = OpeningPlan::Unknown;
    _expectedEnemyPlan = OpeningPlan::Unknown;
    _expectedEnemyPlanGames = 0;
    _expectedEnemyPlanConfidence = 0.0;
    _recommendedOpening.clear();
    _recommendedTerranPlan.clear();
    _terranPlanLearningText.clear();
    _lastAirTimingEnemyPlan = OpeningPlan::None;
    _lastAirTimingReturnValue = 0;
    refreshFilename();

    // This creates the skills, which is necessary for reading the data files.
    the.skillkit.initialize();

    if (Config::IO::ReadOpponentModel)
    {
        // Local games leave the latest model in write/. Tournament runners may
        // copy it to read/. Read ONLY our current
        // GrimHammer_<opponent>_inf.txt format and use exactly one source to
        // avoid counting games twice. Old Steamhammer om_<opponent>.txt files
        // deliberately remain ignored, even when no GrimHammer file exists.
        if (!readRecords(Config::IO::WriteDir + _filename) &&
            !readRecords(Config::IO::ReadDir + _filename))
        {
            readRecords(Config::IO::PreparedDataDir + _filename);
        }
    }

    // Make immediate decisions that may take into account the game records.
    // The initial expected enemy plan is set only here. That's the idea.
    // The current expected enemy plan may be reset later.
    _expectedEnemyPlan = _initialExpectedEnemyPlan = predictEnemyPlan(
        &_expectedEnemyPlanGames,
        &_expectedEnemyPlanConfidence);
    considerSingleStrategy();
    summarizeGames();
}

// Write once, at the end of the game. The temporary file prevents a partial
// model from replacing the last valid one if writing is interrupted.
void OpponentModel::write()
{
    if (!Config::IO::WriteOpponentModel || _writeCompleted)
    {
        return;
    }

    const std::string finalPath = Config::IO::WriteDir + _filename;
    const std::string temporaryPath = finalPath + ".tmp";
    std::ofstream outFile(temporaryPath, std::ios::trunc);
    if (!outFile.is_open())
    {
        return;
    }

    const size_t recordLimit = GameRecordLimit();
    const size_t oldRecordsToKeep = recordLimit - 1;
    const size_t firstRecord = _pastGameRecords.size() > oldRecordsToKeep
        ? _pastGameRecords.size() - oldRecordsToKeep
        : 0;

    for (size_t i = firstRecord; i < _pastGameRecords.size(); ++i)
    {
        _pastGameRecords[i]->write(outFile);
    }
    _gameRecord.setFinalTerranPlan(
        BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Terran
            ? StrategyManager::Instance().getTerranPlanName()
            : "None");
    _gameRecord.write(outFile);

    outFile.flush();
    const bool writeSucceeded = outFile.good();
    outFile.close();
    if (!writeSucceeded)
    {
        std::remove(temporaryPath.c_str());
        return;
    }

    if (InstallLearningFile(temporaryPath, finalPath))
    {
        _writeCompleted = true;
    }
}

void OpponentModel::update()
{
    _planRecognizer.update();
    reconsiderEnemyPlan();

    if (Config::IO::ReadOpponentModel || Config::IO::WriteOpponentModel)
    {
        _gameRecord.update();

        // TODO the rest is turned off for now, not currently useful
        return;

        if (BWAPI::Broodwar->getFrameCount() % 32 == 31)
        {
            setBestMatch();
        }

        if (_bestMatch)
        {
            //_bestMatch->debugLog();
            //BWAPI::Broodwar->drawTextScreen(200, 10, "%cmatch %s %s", white, _bestMatch->mapName, _bestMatch->openingName);
            BWAPI::Broodwar->drawTextScreen(220, 6, "%cmatch", white);
        }
        else
        {
            BWAPI::Broodwar->drawTextScreen(220, 6, "%cno best match", white);
        }
    }
}

// Fill in the snapshot with a prediction of what the opponent may have at a given time.
// Not currently usable.
void OpponentModel::predictEnemy(int lookaheadFrames, PlayerSnapshot & snap) const
{
    const int t = BWAPI::Broodwar->getFrameCount() + lookaheadFrames;

    // Use the best-match past game record if possible.
    // Otherwise, take a current snapshot and call it the prediction.
    if (_bestMatch && _bestMatch->findClosestSnapshot(t, snap))
    {
        // All done.
    }
    else
    {
        snap.takeEnemy();
    }
}

// Is this game record for the same matchup as the current game?
// We usually ignore records which aren't.
bool OpponentModel::stronglyPredictsStrategyClass(StrategyClass strategyClass) const
{
    // A saved macro class is a prior, not an emergency detector. Require
    // several matching games and a clear recency-weighted majority before
    // other managers are allowed to prepare for it proactively.
    return
        Config::IO::ReadOpponentModel &&
        strategyClass != StrategyClass::Unknown &&
        _summary.likelyStrategyClass == strategyClass &&
        _summary.likelyStrategyClassGames >= 3 &&
        _summary.likelyStrategyClassConfidence >= 0.70;
}

int OpponentModel::getEnemyThreatGames(EnemyThreat threat) const
{
    const auto it = _summary.enemyThreatGames.find(threat);
    return it == _summary.enemyThreatGames.end() ? 0 : it->second;
}

double OpponentModel::getEnemyThreatConfidence(EnemyThreat threat) const
{
    const auto it = _summary.enemyThreatConfidence.find(threat);
    return it == _summary.enemyThreatConfidence.end() ? 0.0 : it->second;
}

bool OpponentModel::stronglyPredictsEnemyThreat(EnemyThreat threat) const
{
    return
        Config::IO::ReadOpponentModel &&
        HasEnemyThreat(_summary.likelyEnemyThreats, threat) &&
        getEnemyThreatGames(threat) >= 3 &&
        getEnemyThreatConfidence(threat) >= 0.70;
}

bool OpponentModel::stronglyPredictsLurkers() const
{
    // EnemyThreatForUnitType marks only Lurkers as Cloak for Zerg. The
    // cumulative threat mask is already stored in existing 3.4 game records,
    // whereas snapshots are deliberately omitted when records are written.
    return the.enemyRace() == BWAPI::Races::Zerg &&
        stronglyPredictsEnemyThreat(EnemyThreat::Cloak);
}

bool OpponentModel::sameMatchup(const GameRecord & record) const
{
    return _gameRecord.sameMatchup(record);
}

// The recognized enemy opening plan.
OpeningPlan OpponentModel::getEnemyPlan() const
{
    return _planRecognizer.getPlan();
}

// String for displaying the recognized enemy opening plan in the UI.
std::string OpponentModel::getEnemyPlanString() const
{
    const std::string threatName = _planRecognizer.getThreatName();
    if (!threatName.empty())
    {
        const OpeningPlan plan = _planRecognizer.getPlan();
        if (plan == OpeningPlan::Contain ||
            plan == OpeningPlan::Proxy ||
            plan == OpeningPlan::WorkerRush ||
            plan == OpeningPlan::FastRush ||
            plan == OpeningPlan::HeavyRush)
        {
            return OpeningPlanString(plan) + " / " + threatName;
        }

        return threatName;
    }

    return OpeningPlanString(_planRecognizer.getPlan());
}

// String for displaying the expected enemy opening plan in the UI.
std::string OpponentModel::getExpectedEnemyPlanString() const
{
    return OpeningPlanString(_expectedEnemyPlan);
}

bool OpponentModel::stronglyPredictsEnemyPlan(OpeningPlan plan) const
{
    // Historical openings are a preparation hint, never current-game proof.
    // Match the confidence policy already used for learned army classes and
    // threat flags so one unusual game cannot trigger an expensive counter.
    return
        Config::IO::ReadOpponentModel &&
        plan != OpeningPlan::Unknown &&
        plan != OpeningPlan::None &&
        _expectedEnemyPlan == plan &&
        _expectedEnemyPlanGames >= 3 &&
        _expectedEnemyPlanConfidence >= 0.70;
}

bool OpponentModel::stronglyPredictsEarlyGroundRush() const
{
    // Fast and Heavy Rush share the same early bunker answer. Count them
    // together so a 20-game split between the two still leads to preparation.
    return Config::IO::ReadOpponentModel &&
        _summary.totalGames >= 10 &&
        _summary.earlyGroundRushGames >= 7 &&
        _summary.earlyGroundRushConfidence >= 0.70;
}

bool OpponentModel::hasEarlyGroundRushRisk() const
{
    // One inexpensive Bunker is worthwhile with a modest historical risk.
    // Keep the stronger 70% signal separate from this insurance threshold.
    return Config::IO::ReadOpponentModel &&
        _summary.totalGames >= 3 &&
        _summary.earlyGroundRushGames >= 1 &&
        _summary.earlyGroundRushConfidence >= 0.10;
}

OpeningPlan OpponentModel::getPreparationEnemyPlan() const
{
    if (_planRecognizer.getPlan() != OpeningPlan::Unknown)
    {
        return _planRecognizer.getPlan();
    }

    return stronglyPredictsEnemyPlan(_expectedEnemyPlan)
        ? _expectedEnemyPlan
        : OpeningPlan::Unknown;
}

// The recognized enemy plan, or the current expected enemy plan if none.
OpeningPlan OpponentModel::getBestGuessEnemyPlan() const
{
    if (_planRecognizer.getPlan() != OpeningPlan::Unknown)
    {
        return _planRecognizer.getPlan();
    }
    return _expectedEnemyPlan;
}

// One of:
// 1 the recognized plan
// 2 the expected plan if the enemy is single-strategy
// 3 the expected plan if the enemy went random and we have learned their race
OpeningPlan OpponentModel::getDarnLikelyEnemyPlan() const
{
    if (_planRecognizer.getPlan() != OpeningPlan::Unknown)
    {
        return _planRecognizer.getPlan();
    }

    if (_singleStrategy)
    {
        return _expectedEnemyPlan;
    }

    if (_gameRecord.getEnemyIsRandom() &&
        the.enemy()->getRace() != BWAPI::Races::Unknown)
    {
        return _expectedEnemyPlan;
    }

    return OpeningPlan::Unknown;
}

// Based on past games, when may the enemy have air units?
// 0 means no prediction, otherwise it's a frame number (past or future doesn't matter).
// The answer may vary over time depending on what we think the enemy opening is.
// The caller will call repeatedly as long as it is interested in the answer.
// NOTE This is really "does the enemy have air tech?" Even a robo facility counts.
int OpponentModel::predictEnemyAirUnitTiming() const
{
	OpeningPlan enemyPlanNow = getBestGuessEnemyPlan();		// may be Unknown, can never be None

	// Cache the prediction until and unless the enemy plan changes.  Keep the
	// cache in this model, never in process-wide function-static variables.
	if (enemyPlanNow == _lastAirTimingEnemyPlan)
	{
		return _lastAirTimingReturnValue;
	}
	_lastAirTimingEnemyPlan = enemyPlanNow;

	// Timings when the enemy follows any plan, or the same plan as this game.
	// 0 means never.
	int anyPlan = 0;			// earliest timing ever seen, for when there are few games
	std::vector<int> samePlan;	// all timings seen; we'll take one that's early-ish but not the earliest
	size_t countZeros = 0;		// how often the enemy never got air tech, when following the same plan

	samePlan.reserve(_pastGameRecords.size());

	for (const GameRecord * record : _pastGameRecords)
	{
		int timing = record->getFrameEnemyGetsAirUnits();
		if (timing)
		{
			anyPlan = (anyPlan == 0) ? timing : std::min(anyPlan, timing);
		}

		if (enemyPlanNow == record->getEnemyPlan())
		{
			if (timing)
			{
				samePlan.push_back(timing);
			}
			else
			{
				countZeros += 1;
			}
		}
	}

	// Figure out the return value.
	// Don't return it yet; we have to save it for the cache.
	if (samePlan.size() >= 4)
	{
		if (countZeros >= samePlan.size())
		{
			// Usually no air tech for the enemy. Predict that.
			_lastAirTimingReturnValue = 0;
		}
		else
		{
			std::sort(samePlan.begin(), samePlan.end());
			_lastAirTimingReturnValue = samePlan.at(samePlan.size() / 3 - 1);		// -1 because it is zero-based
		}
	}
	else
	{
		_lastAirTimingReturnValue = anyPlan;
	}

	return _lastAirTimingReturnValue;
}

OpponentModel & OpponentModel::Instance()
{
    static OpponentModel instance;
    return instance;
}
