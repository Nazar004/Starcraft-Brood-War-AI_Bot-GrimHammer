#pragma once

#include <string>
#include <cstddef>
#include <BWAPI.h>

namespace UAlbertaBot
{
namespace Logger 
{
    void LogAppendToFile(const std::string & logFile, const std::string & msg);
    void LogAppendToFile(const std::string & logFile, const char *fmt, ...);
    void LogOverwriteToFile(const std::string & logFile, const std::string & msg);

    // Optional local crash diagnostics. They are completely inactive unless
    // IO.EnableCrashLog is true in the JSON configuration.
    void InitializeCrashLogging();
    void ShutdownCrashLogging();
    void SetCrashContext(const char * context, int frame);
    void LogCaughtException(const char * callback, const char * message);

    // Lightweight pointer/BWAPI guards. CheckAddressReadable never
    // dereferences the supplied address on Windows; it checks the page with
    // VirtualQuery first.
    bool CheckAddressReadable(
        const void * address,
        size_t bytes,
        const char * context,
        int frame);
    bool CheckBroodwar(const char * context, int frame);

    // Sets a precise crash checkpoint and validates a BWAPI unit before use.
    // Returns false for nullptr, non-existing units, or if BWAPI itself throws
    // an access violation while checking unit->exists().
    bool CheckUnit(BWAPI::Unit unit, const char * context, int frame);
};

namespace FileUtils
{
    std::string ReadFile(const std::string & filename);
}
}
