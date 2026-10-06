#pragma once

#include "Core/Common.h"
#include "Intelligence/Learning/GameRecordNow.h"
#include "Intelligence/OpponentPlan.h"

namespace UAlbertaBot
{
    class OpponentModel
    {
    public:

        // Data structure used locally in deciding on the opening.
        struct OpeningInfoType
        {
            int sameWins;		// on the same map as this game, or following the same plan as this game
            int sameGames;
            int otherWins;		// across all other maps/plans
            int otherGames;
            int classWins;      // games with the opponent's likely response class
            int classGames;
            double weightedWins;
            double weightedGames;

            OpeningInfoType()
                : sameWins(0)
                , sameGames(0)
                , otherWins(0)
                , otherGames(0)
                , classWins(0)
                , classGames(0)
                , weightedWins(0.0)
                , weightedGames(0.0)
            {
            }
        };

        struct OpponentSummary
        {
            int totalWins;
            int totalGames;
            std::map<std::string, OpeningInfoType> openingInfo;		// opening name -> opening info
            OpeningInfoType planInfo;								// summary of the recorded enemy plans
            StrategyClass likelyStrategyClass;
            int likelyStrategyClassGames;
            double likelyStrategyClassConfidence;
            // Each threat is learned independently. The mask contains only
            // threats which passed the conservative 3-game/70% threshold.
            EnemyThreatMask likelyEnemyThreats;
            std::map<EnemyThreat, int> enemyThreatGames;
            std::map<EnemyThreat, double> enemyThreatConfidence;
            int earlyGroundRushGames;
            double earlyGroundRushConfidence;

            OpponentSummary()
                : totalWins(0)
                , totalGames(0)
                , likelyStrategyClass(StrategyClass::Unknown)
                , likelyStrategyClassGames(0)
                , likelyStrategyClassConfidence(0.0)
                , likelyEnemyThreats(0)
                , earlyGroundRushGames(0)
                , earlyGroundRushConfidence(0.0)
            {
            }
        };

    private:

        OpponentPlan _planRecognizer;

        std::string _filename;
        GameRecordNow _gameRecord;                      // the current game
        std::vector<GameRecord *> _pastGameRecords;     // from learning files
        bool _writeCompleted;                           // onEnd must persist at most once

        GameRecord * _bestMatch;				// not currently usable

        // Advice for the rest of the bot.
        OpponentSummary _summary;
        bool _singleStrategy;					// enemy seems to always do the same thing, false until proven true
        OpeningPlan _initialExpectedEnemyPlan;  // first predicted enemy plan, before play starts
        OpeningPlan _expectedEnemyPlan;		    // in-game predicted enemy plan
        int _expectedEnemyPlanGames;            // matching recognized games behind the prediction
        double _expectedEnemyPlanConfidence;    // recency-weighted share of recorded matchups
        // NOTE There is also an actual recognized enemy plan. It is kept in _planRecognizer.getPlan().
        std::string _recommendedOpening;
        std::string _recommendedTerranPlan;
        std::string _terranPlanLearningText;

        // Per-model cache. Function-static state leaked predictions across games.
        mutable OpeningPlan _lastAirTimingEnemyPlan;
        mutable int _lastAirTimingReturnValue;

        OpeningPlan predictEnemyPlan(int * matchingGames, double * confidence) const;

        void considerSingleStrategy();
        void summarizeGames();
        bool readRecords(const std::string & path);
        void clearPastGameRecords();
        void refreshFilename();
        void reconsiderEnemyPlan();
        void setBestMatch();

        std::string getOpeningForEnemyPlan(OpeningPlan enemyPlan);

    public:
        OpponentModel();
        ~OpponentModel();

        void setOpening() { _gameRecord.setOpening(Config::Strategy::StrategyName); };
        void setInitialTerranPlan(const std::string & plan) { _gameRecord.setInitialTerranPlan(plan); };
        void setWin(bool isWinner) { _gameRecord.setWin(isWinner); };

        void read();
        void write();
        void selectOpening(const std::vector<std::string> & availableOpenings);
        std::string recommendTerranPlan(
            const std::string & defaultPlan,
            const std::vector<std::string> & allowedPlans);

        void update();

        void predictEnemy(int lookaheadFrames, PlayerSnapshot & snap) const;

        const std::vector<GameRecord *> & getRecords() const { return _pastGameRecords; };
        const OpponentSummary & getSummary() const { return _summary; };
        StrategyClass getLikelyStrategyClass() const { return _summary.likelyStrategyClass; };
        int getLikelyStrategyClassGames() const { return _summary.likelyStrategyClassGames; };
        double getLikelyStrategyClassConfidence() const { return _summary.likelyStrategyClassConfidence; };
        bool stronglyPredictsStrategyClass(StrategyClass strategyClass) const;
        EnemyThreatMask getLikelyEnemyThreats() const { return _summary.likelyEnemyThreats; };
        int getEnemyThreatGames(EnemyThreat threat) const;
        double getEnemyThreatConfidence(EnemyThreat threat) const;
        bool stronglyPredictsEnemyThreat(EnemyThreat threat) const;
        bool stronglyPredictsLurkers() const;
        bool        sameMatchup(const GameRecord & record) const;

        bool		isEnemySingleStrategy() const { return _singleStrategy; };
        OpeningPlan getEnemyPlan() const;
        std::string getEnemyPlanString() const;
        OpeningPlan getInitialExpectedEnemyPlan() const { return _initialExpectedEnemyPlan; };
        OpeningPlan getExpectedEnemyPlan() const { return _expectedEnemyPlan; };
        std::string getExpectedEnemyPlanString() const;
        int getExpectedEnemyPlanGames() const { return _expectedEnemyPlanGames; };
        double getExpectedEnemyPlanConfidence() const { return _expectedEnemyPlanConfidence; };
        bool stronglyPredictsEnemyPlan(OpeningPlan plan) const;
        bool stronglyPredictsEarlyGroundRush() const;
        bool hasEarlyGroundRushRisk() const;
        // Current-game recognition always wins. A historical plan becomes a
        // preparation signal only after passing the 3-game/70% confidence gate.
        OpeningPlan getPreparationEnemyPlan() const;
        OpeningPlan getBestGuessEnemyPlan() const;
        OpeningPlan getDarnLikelyEnemyPlan() const;
        bool hasEnemyOpeningThreat(OpeningThreat threat) const
        {
            return _planRecognizer.hasThreat(threat);
        };

		int predictEnemyAirUnitTiming() const;

        const std::string & getRecommendedOpening() const { return _recommendedOpening; };
        const std::string & getRecommendedTerranPlan() const { return _recommendedTerranPlan; };
        const std::string & getTerranPlanLearningText() const { return _terranPlanLearningText; };

        static OpponentModel & Instance();
    };

}
