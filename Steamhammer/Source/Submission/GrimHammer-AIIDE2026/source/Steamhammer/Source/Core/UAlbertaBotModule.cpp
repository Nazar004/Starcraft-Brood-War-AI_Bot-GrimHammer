/* 
 *----------------------------------------------------------------------
 * Steamhammer entry point.
 *----------------------------------------------------------------------
 */

#include "Core/UAlbertaBotModule.h"

#include "../../../BOSS/source/BOSS.h"
#include "Core/GameCommander.h"
#include "Utilities/Logger.h"
#include "Intelligence/Learning/OpeningTiming.h"
#include "Utilities/ParseUtils.h"

#include <exception>

using namespace UAlbertaBot;

UAlbertaBotModule::UAlbertaBotModule()
{
}

// BWAPI calls this when the bot starts.
void UAlbertaBotModule::onStart()
{
    _terranGame = BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Terran;
    if (!_terranGame)
    {
        BWAPI::Broodwar->printf("GrimHammer supports Terran only. Select Terran to run the bot.");
        return;
    }

    // Initialize BOSS, the Build Order Search System
    BOSS::init();

    the.initialize();

    // The JSON file is parsed by the.initialize(). Crash logging is installed
    // only after that, so IO.EnableCrashLog=false remains a true no-op for
    // ladder/Basil builds.
    Logger::SetCrashContext(
        "UAlbertaBotModule::onStart - configuration loaded",
        BWAPI::Broodwar->getFrameCount());
    Logger::InitializeCrashLogging();

    if (Config::IO::EnableCrashLog)
    {
        BWAPI::Broodwar->printf(
            "Crash log enabled: %s",
            Config::IO::CrashLogFilename.c_str());
    }

    // Set our BWAPI options according to the configuration. 
    BWAPI::Broodwar->setLocalSpeed(Config::BWAPIOptions::SetLocalSpeed);
    BWAPI::Broodwar->setFrameSkip(Config::BWAPIOptions::SetFrameSkip);
    
    if (Config::BWAPIOptions::EnableCompleteMapInformation)
    {
        BWAPI::Broodwar->enableFlag(BWAPI::Flag::CompleteMapInformation);
    }

    if (Config::BWAPIOptions::EnableUserInput)
    {
        BWAPI::Broodwar->enableFlag(BWAPI::Flag::UserInput);
    }

    StrategyManager::Instance().setOpeningGroup();    // may depend on config and/or opponent model

    if (Config::BotInfo::PrintInfoOnStart)
    {
        BWAPI::Broodwar->printf("%s by %s, based on UAlbertaBot.", Config::BotInfo::BotName.c_str(), Config::BotInfo::Authors.c_str());
        if (Config::Skills::HumanOpponent)
        {
            GameMessage("gl hf");
        }
    }

    // Turn on latency compensation, in case somebody sets it off by default.
    BWAPI::Broodwar->setLatCom(true);
}

void UAlbertaBotModule::onEnd(bool isWinner)
{
    if (!_terranGame) return;

    Logger::SetCrashContext(
        "UAlbertaBotModule::onEnd",
        BWAPI::Broodwar->getFrameCount());

    try
    {
        GameCommander::Instance().onEnd(isWinner);
    }
    catch (const std::exception & exception)
    {
        Logger::LogCaughtException("onEnd", exception.what());
        throw;
    }
    catch (...)
    {
        Logger::LogCaughtException("onEnd", "unknown C++ exception");
        throw;
    }

    Logger::ShutdownCrashLogging();
}

void UAlbertaBotModule::onFrame()
{
    if (!_terranGame) return;

    if (!Config::ConfigFile::ConfigFileFound)
    {
        BWAPI::Broodwar->drawBoxScreen(0,0,450,100, BWAPI::Colors::Black, true);
        BWAPI::Broodwar->setTextSize(BWAPI::Text::Size::Huge);
        BWAPI::Broodwar->drawTextScreen(10, 5, "%c%s Config File Not Found", red, Config::BotInfo::BotName.c_str());
        BWAPI::Broodwar->setTextSize(BWAPI::Text::Size::Default);
        BWAPI::Broodwar->drawTextScreen(10, 30, "%c%s will not run without its configuration file", white, Config::BotInfo::BotName.c_str());
        BWAPI::Broodwar->drawTextScreen(10, 45, "%cCheck that the file below exists. Incomplete paths are relative to Starcraft directory", white);
        BWAPI::Broodwar->drawTextScreen(10, 60, "%cYou can change this file location in Config::ConfigFile::ConfigFileLocation", white);
        BWAPI::Broodwar->drawTextScreen(10, 75, "%cFile Not Found (or is empty): %c %s", white, green, Config::ConfigFile::ConfigFileLocation.c_str());
        return;
    }

    if (!Config::ConfigFile::ConfigFileParsed)
    {
        BWAPI::Broodwar->drawBoxScreen(0,0,450,100, BWAPI::Colors::Black, true);
        BWAPI::Broodwar->setTextSize(BWAPI::Text::Size::Huge);
        BWAPI::Broodwar->drawTextScreen(10, 5, "%c%s Config File Parse Error", red, Config::BotInfo::BotName.c_str());
        BWAPI::Broodwar->setTextSize(BWAPI::Text::Size::Default);
        BWAPI::Broodwar->drawTextScreen(10, 30, "%c%s will not run without a properly formatted configuration file", white, Config::BotInfo::BotName.c_str());
        BWAPI::Broodwar->drawTextScreen(10, 45, "%cThe configuration file was found, but could not be parsed. Check that it is valid JSON", white);
        BWAPI::Broodwar->drawTextScreen(10, 60, "%cFile Not Parsed: %c %s", white, green, Config::ConfigFile::ConfigFileLocation.c_str());
        return;
    }

    Logger::SetCrashContext(
        "UAlbertaBotModule::onFrame - entering GameCommander",
        BWAPI::Broodwar->getFrameCount());

    try
    {
        GameCommander::Instance().update();
    }
    catch (const std::exception & exception)
    {
        Logger::LogCaughtException("onFrame", exception.what());
        throw;
    }
    catch (...)
    {
        Logger::LogCaughtException("onFrame", "unknown C++ exception");
        throw;
    }
}

void UAlbertaBotModule::onUnitDestroy(BWAPI::Unit unit)
{
    if (!_terranGame) return;

    Logger::SetCrashContext(
        "UAlbertaBotModule::onUnitDestroy",
        BWAPI::Broodwar->getFrameCount());
    GameCommander::Instance().onUnitDestroy(unit);
}

void UAlbertaBotModule::onUnitMorph(BWAPI::Unit unit)
{
    if (!_terranGame) return;

    Logger::SetCrashContext(
        "UAlbertaBotModule::onUnitMorph",
        BWAPI::Broodwar->getFrameCount());
    GameCommander::Instance().onUnitMorph(unit);
}

void UAlbertaBotModule::onSendText(std::string text) 
{
    if (!_terranGame) return;
 
    Logger::SetCrashContext(
        "UAlbertaBotModule::onSendText",
        BWAPI::Broodwar->getFrameCount());
    ParseUtils::ParseTextCommand(text);
}

void UAlbertaBotModule::onUnitCreate(BWAPI::Unit unit)
{
    if (!_terranGame) return;
 
    Logger::SetCrashContext(
        "UAlbertaBotModule::onUnitCreate",
        BWAPI::Broodwar->getFrameCount());
    GameCommander::Instance().onUnitCreate(unit);
}

void UAlbertaBotModule::onUnitComplete(BWAPI::Unit unit)
{
    if (!_terranGame) return;

    Logger::SetCrashContext(
        "UAlbertaBotModule::onUnitComplete",
        BWAPI::Broodwar->getFrameCount());
    GameCommander::Instance().onUnitComplete(unit);
}

void UAlbertaBotModule::onUnitShow(BWAPI::Unit unit)
{
    if (!_terranGame) return;
 
    Logger::SetCrashContext(
        "UAlbertaBotModule::onUnitShow",
        BWAPI::Broodwar->getFrameCount());
    GameCommander::Instance().onUnitShow(unit);
}

void UAlbertaBotModule::onUnitHide(BWAPI::Unit unit)
{
    if (!_terranGame) return;
 
    Logger::SetCrashContext(
        "UAlbertaBotModule::onUnitHide",
        BWAPI::Broodwar->getFrameCount());
    GameCommander::Instance().onUnitHide(unit);
}

void UAlbertaBotModule::onUnitRenegade(BWAPI::Unit unit)
{
    if (!_terranGame) return;
 
    Logger::SetCrashContext(
        "UAlbertaBotModule::onUnitRenegade",
        BWAPI::Broodwar->getFrameCount());
    GameCommander::Instance().onUnitRenegade(unit);
}
