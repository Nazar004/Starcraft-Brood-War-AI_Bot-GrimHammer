#include "Utilities/Logger.h"
#include "Core/Config.h"
#include <BWAPI.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "Dbghelp.lib")
#endif

using namespace UAlbertaBot;

namespace
{
    char crashContext[512] = "Crash logger has no checkpoint yet";
    char crashGameInfo[1024] = "Game information unavailable";
    bool crashLoggingInitialized = false;

#ifdef _WIN32
    volatile LONG crashFrame = -1;
    volatile LONG crashRecordWritten = 0;
    volatile LONG lastCrashFrame = -2;
    volatile LONG lastCrashCode = 0;
    volatile LONG lastCrashAddress = 0;
    volatile LONG lastCrashTick = 0;
    LPTOP_LEVEL_EXCEPTION_FILTER previousExceptionFilter = nullptr;
    PVOID vectoredExceptionHandler = nullptr;
    std::terminate_handler previousTerminateHandler = nullptr;
    bool symbolEngineInitialized = false;

    bool ensureParentDirectories(const char * path)
    {
        if (!path || !*path)
        {
            return false;
        }

        char buffer[2048];
        std::snprintf(buffer, sizeof(buffer), "%s", path);

        for (char * cursor = buffer; *cursor; ++cursor)
        {
            if (*cursor != '/' && *cursor != '\\')
            {
                continue;
            }

            // Do not try to create the root of an absolute path or the C:
            // drive prefix. Relative paths such as bwapi-data/write are the
            // normal configuration used by GrimHammer.
            if (cursor == buffer ||
                (cursor == buffer + 2 && buffer[1] == ':'))
            {
                continue;
            }

            const char separator = *cursor;
            *cursor = '\0';
            const BOOL created = CreateDirectoryA(buffer, nullptr);
            const DWORD error = created ? ERROR_SUCCESS : GetLastError();
            *cursor = separator;

            if (!created && error != ERROR_ALREADY_EXISTS)
            {
                return false;
            }
        }

        return true;
    }

    const char * memoryStateName(DWORD state)
    {
        switch (state)
        {
        case MEM_COMMIT:  return "MEM_COMMIT";
        case MEM_RESERVE: return "MEM_RESERVE";
        case MEM_FREE:    return "MEM_FREE";
        default:          return "UNKNOWN";
        }
    }

    const char * memoryTypeName(DWORD type)
    {
        switch (type)
        {
        case MEM_IMAGE:   return "MEM_IMAGE";
        case MEM_MAPPED:  return "MEM_MAPPED";
        case MEM_PRIVATE: return "MEM_PRIVATE";
        default:          return "NONE/UNKNOWN";
        }
    }

    bool isReadableProtection(DWORD protect)
    {
        if (protect & PAGE_GUARD)
        {
            return false;
        }

        const DWORD baseProtect = protect & 0xFFu;
        return baseProtect == PAGE_READONLY ||
               baseProtect == PAGE_READWRITE ||
               baseProtect == PAGE_WRITECOPY ||
               baseProtect == PAGE_EXECUTE_READ ||
               baseProtect == PAGE_EXECUTE_READWRITE ||
               baseProtect == PAGE_EXECUTE_WRITECOPY;
    }

    void describeMemoryAddress(const void * address, char * buffer, size_t bufferSize)
    {
        if (!buffer || bufferSize == 0)
        {
            return;
        }

        if (!address)
        {
            std::snprintf(buffer, bufferSize, "address=nullptr");
            return;
        }

        MEMORY_BASIC_INFORMATION mbi = {};
        const SIZE_T queried = VirtualQuery(address, &mbi, sizeof(mbi));
        if (queried == 0)
        {
            std::snprintf(
                buffer,
                bufferSize,
                "address=%p | VirtualQuery failed error=%lu",
                address,
                static_cast<unsigned long>(GetLastError()));
            return;
        }

        std::snprintf(
            buffer,
            bufferSize,
            "address=%p | base=%p | region=%llu | state=%s | protect=0x%08lX | type=%s | readable=%s",
            address,
            mbi.BaseAddress,
            static_cast<unsigned long long>(mbi.RegionSize),
            memoryStateName(mbi.State),
            static_cast<unsigned long>(mbi.Protect),
            memoryTypeName(mbi.Type),
            (mbi.State == MEM_COMMIT && isReadableProtection(mbi.Protect)) ? "yes" : "no");
    }

    void appendFormat(char * buffer, size_t bufferSize, size_t & used, const char * format, ...)
    {
        if (!buffer || bufferSize == 0 || !format || used >= bufferSize - 1)
        {
            return;
        }

        va_list args;
        va_start(args, format);
        const int written = std::vsnprintf(
            buffer + used,
            bufferSize - used,
            format,
            args);
        va_end(args);

        if (written < 0)
        {
            buffer[bufferSize - 1] = '\0';
            return;
        }

        const size_t remaining = bufferSize - used;
        if (static_cast<size_t>(written) >= remaining)
        {
            used = bufferSize - 1;
            buffer[used] = '\0';
            return;
        }

        used += static_cast<size_t>(written);
    }

    const char * baseNameFromPath(const char * path)
    {
        if (!path)
        {
            return "unknown";
        }

        const char * baseName = path;
        for (const char * cursor = path; *cursor; ++cursor)
        {
            if (*cursor == '/' || *cursor == '\\')
            {
                baseName = cursor + 1;
            }
        }
        return *baseName ? baseName : path;
    }

    bool getModuleForAddress(
        DWORD64 address,
        char * modulePath,
        size_t modulePathSize,
        DWORD64 & moduleBase,
        DWORD64 & moduleOffset)
    {
        if (!modulePath || modulePathSize == 0 || address == 0)
        {
            return false;
        }

        modulePath[0] = '\0';
        moduleBase = 0;
        moduleOffset = 0;

        HMODULE module = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(address)),
                &module))
        {
            return false;
        }

        if (GetModuleFileNameA(module, modulePath, static_cast<DWORD>(modulePathSize)) == 0)
        {
            std::snprintf(modulePath, modulePathSize, "unknown-module");
        }

        moduleBase = static_cast<DWORD64>(
            reinterpret_cast<std::uintptr_t>(module));
        moduleOffset = address - moduleBase;
        return true;
    }

    void appendSymbolForAddress(
        DWORD64 address,
        char * buffer,
        size_t bufferSize,
        size_t & used)
    {
        if (!symbolEngineInitialized || address == 0)
        {
            return;
        }

        HANDLE process = GetCurrentProcess();

        char symbolStorage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        SYMBOL_INFO * symbol = reinterpret_cast<SYMBOL_INFO *>(symbolStorage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        DWORD64 symbolDisplacement = 0;
        if (SymFromAddr(process, address, &symbolDisplacement, symbol))
        {
            appendFormat(
                buffer,
                bufferSize,
                used,
                " | %s+0x%llX",
                symbol->Name,
                static_cast<unsigned long long>(symbolDisplacement));
        }

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line) &&
            line.FileName)
        {
            appendFormat(
                buffer,
                bufferSize,
                used,
                " | %s:%lu",
                line.FileName,
                static_cast<unsigned long>(line.LineNumber));
        }
    }

    void buildCallStack(
        EXCEPTION_POINTERS * pointers,
        char * buffer,
        size_t bufferSize)
    {
        if (!buffer || bufferSize == 0)
        {
            return;
        }

        size_t used = 0;
        buffer[0] = '\0';
        appendFormat(buffer, bufferSize, used, "Call stack:\r\n");

        if (!pointers || !pointers->ContextRecord)
        {
            appendFormat(buffer, bufferSize, used, "  unavailable: no CPU context\r\n");
            return;
        }

        __try
        {
            CONTEXT context = *pointers->ContextRecord;
            STACKFRAME64 frame = {};
            DWORD machineType = 0;

#if defined(_M_IX86)
            machineType = IMAGE_FILE_MACHINE_I386;
            frame.AddrPC.Offset = context.Eip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Ebp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Esp;
            frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_X64)
            machineType = IMAGE_FILE_MACHINE_AMD64;
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
#else
            appendFormat(buffer, bufferSize, used, "  unsupported architecture\r\n");
            return;
#endif

            HANDLE process = GetCurrentProcess();
            HANDLE thread = GetCurrentThread();
            DWORD64 previousPc = 0;
            DWORD64 previousSp = 0;

            for (unsigned int index = 0; index < 32; ++index)
            {
                const DWORD64 pc = frame.AddrPC.Offset;
                const DWORD64 sp = frame.AddrStack.Offset;
                if (pc == 0)
                {
                    break;
                }

                char modulePath[MAX_PATH] = "unknown";
                DWORD64 moduleBase = 0;
                DWORD64 moduleOffset = 0;
                const bool hasModule = getModuleForAddress(
                    pc,
                    modulePath,
                    sizeof(modulePath),
                    moduleBase,
                    moduleOffset);

                appendFormat(
                    buffer,
                    bufferSize,
                    used,
                    "  #%02u 0x%08llX",
                    index,
                    static_cast<unsigned long long>(pc));

                if (hasModule)
                {
                    appendFormat(
                        buffer,
                        bufferSize,
                        used,
                        " | %s+0x%llX",
                        baseNameFromPath(modulePath),
                        static_cast<unsigned long long>(moduleOffset));
                }
                else
                {
                    appendFormat(buffer, bufferSize, used, " | module=unknown");
                }

                appendSymbolForAddress(pc, buffer, bufferSize, used);
                appendFormat(
                    buffer,
                    bufferSize,
                    used,
                    " | SP=0x%08llX\r\n",
                    static_cast<unsigned long long>(sp));

                const BOOL walked = StackWalk64(
                    machineType,
                    process,
                    thread,
                    &frame,
                    &context,
                    nullptr,
                    SymFunctionTableAccess64,
                    SymGetModuleBase64,
                    nullptr);

                if (!walked)
                {
                    appendFormat(
                        buffer,
                        bufferSize,
                        used,
                        "  [StackWalk64 stopped: error=%lu]\r\n",
                        static_cast<unsigned long>(GetLastError()));
                    break;
                }

                if (frame.AddrPC.Offset == pc && frame.AddrStack.Offset == sp)
                {
                    appendFormat(buffer, bufferSize, used, "  [StackWalk64 made no progress]\r\n");
                    break;
                }

                if (previousPc == frame.AddrPC.Offset && previousSp == frame.AddrStack.Offset)
                {
                    appendFormat(buffer, bufferSize, used, "  [repeated stack frame]\r\n");
                    break;
                }

                previousPc = pc;
                previousSp = sp;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            appendFormat(
                buffer,
                bufferSize,
                used,
                "  call-stack collection raised SEH 0x%08lX\r\n",
                static_cast<unsigned long>(GetExceptionCode()));
        }
    }

    void buildRawStack(
        EXCEPTION_POINTERS * pointers,
        char * buffer,
        size_t bufferSize)
    {
        if (!buffer || bufferSize == 0)
        {
            return;
        }

        size_t used = 0;
        buffer[0] = '\0';
        appendFormat(buffer, bufferSize, used, "Raw stack (first 32 pointer slots):\r\n");

        if (!pointers || !pointers->ContextRecord)
        {
            appendFormat(buffer, bufferSize, used, "  unavailable: no CPU context\r\n");
            return;
        }

#if defined(_M_IX86)
        const std::uintptr_t stackPointer =
            static_cast<std::uintptr_t>(pointers->ContextRecord->Esp);
#elif defined(_M_X64)
        const std::uintptr_t stackPointer =
            static_cast<std::uintptr_t>(pointers->ContextRecord->Rsp);
#else
        appendFormat(buffer, bufferSize, used, "  unsupported architecture\r\n");
        return;
#endif

        HANDLE process = GetCurrentProcess();
        for (size_t index = 0; index < 32; ++index)
        {
            const std::uintptr_t slotAddress =
                stackPointer + index * sizeof(std::uintptr_t);
            std::uintptr_t value = 0;
            SIZE_T bytesRead = 0;

            if (!ReadProcessMemory(
                    process,
                    reinterpret_cast<const void *>(slotAddress),
                    &value,
                    sizeof(value),
                    &bytesRead) ||
                bytesRead != sizeof(value))
            {
                appendFormat(
                    buffer,
                    bufferSize,
                    used,
                    "  SP+0x%02llX @ 0x%08llX = <unreadable>\r\n",
                    static_cast<unsigned long long>(index * sizeof(std::uintptr_t)),
                    static_cast<unsigned long long>(slotAddress));
                continue;
            }

            appendFormat(
                buffer,
                bufferSize,
                used,
                "  SP+0x%02llX @ 0x%08llX = 0x%08llX",
                static_cast<unsigned long long>(index * sizeof(std::uintptr_t)),
                static_cast<unsigned long long>(slotAddress),
                static_cast<unsigned long long>(value));

            char modulePath[MAX_PATH] = "unknown";
            DWORD64 moduleBase = 0;
            DWORD64 moduleOffset = 0;
            if (getModuleForAddress(
                    static_cast<DWORD64>(value),
                    modulePath,
                    sizeof(modulePath),
                    moduleBase,
                    moduleOffset))
            {
                appendFormat(
                    buffer,
                    bufferSize,
                    used,
                    " | %s+0x%llX",
                    baseNameFromPath(modulePath),
                    static_cast<unsigned long long>(moduleOffset));
                appendSymbolForAddress(
                    static_cast<DWORD64>(value),
                    buffer,
                    bufferSize,
                    used);
            }

            appendFormat(buffer, bufferSize, used, "\r\n");
        }
    }

    const char * exceptionName(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:              return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:   return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION:   return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION:     return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:           return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:            return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:     return "EXCEPTION_INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:        return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:          return "EXCEPTION_STACK_OVERFLOW";
        default:                                return "UNKNOWN_OR_CPP_EXCEPTION";
        }
    }

    bool appendEmergencyText(const char * path, const char * text)
    {
        if (!path || !*path || !text)
        {
            return false;
        }

        ensureParentDirectories(path);

        HANDLE file = CreateFileA(
            path,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD written = 0;
        const DWORD length = static_cast<DWORD>(std::strlen(text));
        const BOOL ok = WriteFile(file, text, length, &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
        return ok && written == length;
    }

    bool isSevereException(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_INVALID_DISPOSITION:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case 0xC0000374: // STATUS_HEAP_CORRUPTION
        case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN / fast fail
        case 0xC0000602: // STATUS_FAIL_FAST_EXCEPTION
            return true;
        default:
            return false;
        }
    }

    void writeWindowsCrashRecord(
        EXCEPTION_POINTERS * pointers,
        const char * handlerName)
    {
        if (!Config::IO::EnableCrashLog ||
            InterlockedCompareExchange(&crashRecordWritten, 1, 0) != 0)
        {
            return;
        }

        const DWORD code = pointers && pointers->ExceptionRecord
            ? pointers->ExceptionRecord->ExceptionCode
            : 0;
        void * address = pointers && pointers->ExceptionRecord
            ? pointers->ExceptionRecord->ExceptionAddress
            : nullptr;

        // The vectored handler and the top-level filter can both observe the
        // same fatal exception. Suppress only an identical record occurring
        // almost immediately in the same game frame.
        const LONG addressValue = static_cast<LONG>(
            reinterpret_cast<std::uintptr_t>(address));
        const LONG nowTick = static_cast<LONG>(GetTickCount());
        if (lastCrashFrame == crashFrame &&
            lastCrashCode == static_cast<LONG>(code) &&
            lastCrashAddress == addressValue &&
            static_cast<DWORD>(nowTick - lastCrashTick) < 1000u)
        {
            InterlockedExchange(&crashRecordWritten, 0);
            return;
        }

        lastCrashFrame = crashFrame;
        lastCrashCode = static_cast<LONG>(code);
        lastCrashAddress = addressValue;
        lastCrashTick = nowTick;

        const char * accessType = "n/a";
        void * referencedAddress = nullptr;
        unsigned long inPageStatus = 0;

        if (pointers && pointers->ExceptionRecord &&
            (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
            pointers->ExceptionRecord->NumberParameters >= 2)
        {
            const ULONG_PTR operation = pointers->ExceptionRecord->ExceptionInformation[0];
            accessType = operation == 0 ? "read"
                : operation == 1 ? "write"
                : operation == 8 ? "execute"
                : "unknown";
            referencedAddress = reinterpret_cast<void *>(
                pointers->ExceptionRecord->ExceptionInformation[1]);

            if (code == EXCEPTION_IN_PAGE_ERROR &&
                pointers->ExceptionRecord->NumberParameters >= 3)
            {
                inPageStatus = static_cast<unsigned long>(
                    pointers->ExceptionRecord->ExceptionInformation[2]);
            }
        }

        SYSTEMTIME time;
        GetLocalTime(&time);

        char modulePath[MAX_PATH] = "unknown";
        unsigned long long moduleOffset = 0;
        HMODULE module = nullptr;

        if (address &&
            GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(address),
                &module))
        {
            GetModuleFileNameA(module, modulePath, MAX_PATH);
            moduleOffset =
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)) -
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(module));
        }

        char referencedMemoryInfo[512] = "not applicable";
        if (referencedAddress)
        {
            describeMemoryAddress(
                referencedAddress,
                referencedMemoryInfo,
                sizeof(referencedMemoryInfo));
        }

        char instructionMemoryInfo[512] = "not available";
        if (address)
        {
            describeMemoryAddress(
                address,
                instructionMemoryInfo,
                sizeof(instructionMemoryInfo));
        }

        char registerInfo[1024] = "Registers: unavailable";
        char faultRelation[1024] = "Fault relation: unavailable";
        char instructionBytes[256] = "Instruction bytes: unavailable";
        if (pointers && pointers->ContextRecord)
        {
#if defined(_M_IX86)
            const CONTEXT * ctx = pointers->ContextRecord;
            std::snprintf(
                registerInfo,
                sizeof(registerInfo),
                "Registers: EIP=%08lX ESP=%08lX EBP=%08lX | EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX",
                static_cast<unsigned long>(ctx->Eip),
                static_cast<unsigned long>(ctx->Esp),
                static_cast<unsigned long>(ctx->Ebp),
                static_cast<unsigned long>(ctx->Eax),
                static_cast<unsigned long>(ctx->Ebx),
                static_cast<unsigned long>(ctx->Ecx),
                static_cast<unsigned long>(ctx->Edx),
                static_cast<unsigned long>(ctx->Esi),
                static_cast<unsigned long>(ctx->Edi));

            if (referencedAddress)
            {
                const std::intptr_t fault = static_cast<std::intptr_t>(
                    reinterpret_cast<std::uintptr_t>(referencedAddress));

                struct RegisterCandidate
                {
                    const char * name;
                    std::uintptr_t value;
                };

                const RegisterCandidate candidates[] =
                {
                    { "EAX", static_cast<std::uintptr_t>(ctx->Eax) },
                    { "EBX", static_cast<std::uintptr_t>(ctx->Ebx) },
                    { "ECX", static_cast<std::uintptr_t>(ctx->Ecx) },
                    { "EDX", static_cast<std::uintptr_t>(ctx->Edx) },
                    { "ESI", static_cast<std::uintptr_t>(ctx->Esi) },
                    { "EDI", static_cast<std::uintptr_t>(ctx->Edi) },
                    { "EBP", static_cast<std::uintptr_t>(ctx->Ebp) },
                    { "ESP", static_cast<std::uintptr_t>(ctx->Esp) }
                };

                size_t used = static_cast<size_t>(std::snprintf(
                    faultRelation,
                    sizeof(faultRelation),
                    "Fault relation:"));
                bool foundRelation = false;

                for (const RegisterCandidate & candidate : candidates)
                {
                    const std::intptr_t delta = fault -
                        static_cast<std::intptr_t>(candidate.value);
                    if (delta >= -0x10000 && delta <= 0x10000)
                    {
                        foundRelation = true;
                        if (used < sizeof(faultRelation))
                        {
                            const int written = std::snprintf(
                                faultRelation + used,
                                sizeof(faultRelation) - used,
                                " %s%+lld",
                                candidate.name,
                                static_cast<long long>(delta));
                            if (written > 0)
                            {
                                used += static_cast<size_t>(written);
                            }
                        }
                    }
                }

                if (!foundRelation)
                {
                    std::snprintf(
                        faultRelation,
                        sizeof(faultRelation),
                        "Fault relation: no register within +/-0x10000 of %p",
                        referencedAddress);
                }
            }
#elif defined(_M_X64)
            const CONTEXT * ctx = pointers->ContextRecord;
            std::snprintf(
                registerInfo,
                sizeof(registerInfo),
                "Registers: RIP=%016llX RSP=%016llX RBP=%016llX | RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX RSI=%016llX RDI=%016llX",
                static_cast<unsigned long long>(ctx->Rip),
                static_cast<unsigned long long>(ctx->Rsp),
                static_cast<unsigned long long>(ctx->Rbp),
                static_cast<unsigned long long>(ctx->Rax),
                static_cast<unsigned long long>(ctx->Rbx),
                static_cast<unsigned long long>(ctx->Rcx),
                static_cast<unsigned long long>(ctx->Rdx),
                static_cast<unsigned long long>(ctx->Rsi),
                static_cast<unsigned long long>(ctx->Rdi));
#endif

            // Read a small window of machine code at the faulting instruction
            // without directly dereferencing the address. This can later be
            // matched against the BWAPI.dll build or inspected in a debugger.
            if (address)
            {
                unsigned char bytes[16] = {};
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(
                        GetCurrentProcess(),
                        address,
                        bytes,
                        sizeof(bytes),
                        &bytesRead) &&
                    bytesRead > 0)
                {
                    size_t used = static_cast<size_t>(std::snprintf(
                        instructionBytes,
                        sizeof(instructionBytes),
                        "Instruction bytes:"));
                    for (SIZE_T i = 0; i < bytesRead && used < sizeof(instructionBytes); ++i)
                    {
                        const int written = std::snprintf(
                            instructionBytes + used,
                            sizeof(instructionBytes) - used,
                            " %02X",
                            static_cast<unsigned int>(bytes[i]));
                        if (written <= 0)
                        {
                            break;
                        }
                        used += static_cast<size_t>(written);
                    }
                }
            }
        }

        // Keep the large diagnostic buffers out of the thread stack. This is
        // important because the logger also handles stack-overflow exceptions.
        static char callStackInfo[16384];
        static char rawStackInfo[16384];
        static char record[65536];
        std::snprintf(callStackInfo, sizeof(callStackInfo), "Call stack: unavailable\r\n");
        std::snprintf(rawStackInfo, sizeof(rawStackInfo), "Raw stack: unavailable\r\n");
        buildCallStack(pointers, callStackInfo, sizeof(callStackInfo));
        buildRawStack(pointers, rawStackInfo, sizeof(rawStackInfo));

        std::snprintf(
            record,
            sizeof(record),
            "\r\n========== GRIMHAMMER CRASH ==========\r\n"
            "Time: %04u-%02u-%02u %02u:%02u:%02u.%03u\r\n"
            "Handler: %s\r\n"
            "Frame: %ld\r\n"
            "Game: %s\r\n"
            "Last checkpoint: %s\r\n"
            "Exception: 0x%08lX (%s)\r\n"
            "Address: %p\r\n"
            "Memory access: %s @ %p\r\n"
            "Referenced memory: %s\r\n"
            "Instruction memory: %s\r\n"
            "In-page status: 0x%08lX\r\n"
            "%s\r\n"
            "%s\r\n"
            "%s\r\n"
            "%s"
            "%s"
            "Module: %s\r\n"
            "Module offset: 0x%llX\r\n"
            "Thread: %lu\r\n"
            "=======================================\r\n",
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond,
            time.wMilliseconds,
            handlerName ? handlerName : "unknown",
            crashFrame,
            crashGameInfo,
            crashContext,
            static_cast<unsigned long>(code),
            exceptionName(code),
            address,
            accessType,
            referencedAddress,
            referencedMemoryInfo,
            instructionMemoryInfo,
            inPageStatus,
            registerInfo,
            faultRelation,
            instructionBytes,
            callStackInfo,
            rawStackInfo,
            modulePath,
            moduleOffset,
            static_cast<unsigned long>(GetCurrentThreadId()));

        if (!appendEmergencyText(Config::IO::CrashLogFilename.c_str(), record))
        {
            // If the configured path is unavailable, leave the record beside
            // StarCraft.exe rather than losing the only crash information.
            appendEmergencyText("GrimHammer_CrashLog.txt", record);
        }

        // Release the writer guard. Duplicate suppression above only filters
        // the same exception/address/frame for one second, so a later genuine
        // crash in the same match is still recorded.
        InterlockedExchange(&crashRecordWritten, 0);
    }

    LONG CALLBACK grimHammerVectoredExceptionFilter(EXCEPTION_POINTERS * pointers)
    {
        const DWORD code = pointers && pointers->ExceptionRecord
            ? pointers->ExceptionRecord->ExceptionCode
            : 0;

        // A vectored handler runs before BWAPI and every top-level exception
        // filter. Record only failures which are normally fatal; ordinary C++
        // exceptions and debugger notifications must remain noise-free.
        if (isSevereException(code))
        {
            writeWindowsCrashRecord(pointers, "vectored exception handler");
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG WINAPI grimHammerUnhandledExceptionFilter(EXCEPTION_POINTERS * pointers)
    {
        writeWindowsCrashRecord(pointers, "top-level exception filter");

        if (previousExceptionFilter &&
            previousExceptionFilter != grimHammerUnhandledExceptionFilter)
        {
            return previousExceptionFilter(pointers);
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    void grimHammerTerminateHandler()
    {
        writeWindowsCrashRecord(nullptr, "std::terminate handler");

        if (previousTerminateHandler &&
            previousTerminateHandler != grimHammerTerminateHandler)
        {
            previousTerminateHandler();
        }

        std::abort();
    }
#else
    int crashFrame = -1;
#endif

    std::string currentDateTime()
    {
        const std::time_t now = std::time(nullptr);
        std::tm localTime = {};

#ifdef _WIN32
        localtime_s(&localTime, &now);
#else
        localtime_r(&now, &localTime);
#endif

        char buffer[64];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
        return buffer;
    }

    void appendCrashText(const std::string & text)
    {
#ifdef _WIN32
        ensureParentDirectories(Config::IO::CrashLogFilename.c_str());
#endif
        std::ofstream stream(Config::IO::CrashLogFilename.c_str(), std::ofstream::app);
        if (stream.good())
        {
            stream << text;
            stream.flush();
            return;
        }

        std::ofstream fallback("GrimHammer_CrashLog.txt", std::ofstream::app);
        fallback << text;
        fallback.flush();
    }
}

void Logger::LogAppendToFile(const std::string & logFile, const std::string & msg)
{
    std::ofstream logStream;
    logStream.open(logFile.c_str(), std::ofstream::app);
    logStream << msg;
    logStream.flush();
    logStream.close();
}

void Logger::LogAppendToFile(const std::string & logFile, const char *fmt, ...)
{
    va_list arg;
        
    va_start(arg, fmt);
    //vfprintf(log_file, fmt, arg);
    char buff[256];
    std::vsnprintf(buff, sizeof(buff), fmt, arg);
    va_end(arg);
        
    std::ofstream logStream;
    logStream.open(logFile.c_str(), std::ofstream::app);
    logStream << buff;
    logStream.flush();
    logStream.close();
}

void Logger::LogOverwriteToFile(const std::string & logFile, const std::string & msg)
{
    std::ofstream logStream(logFile.c_str());
    logStream << msg;
    logStream.flush();
    logStream.close();
}

void Logger::InitializeCrashLogging()
{
    if (!Config::IO::EnableCrashLog || crashLoggingInitialized)
    {
        return;
    }

    crashLoggingInitialized = true;

    if (BWAPI::BroodwarPtr)
    {
        const BWAPI::Player self = BWAPI::BroodwarPtr->self();
        const BWAPI::Player enemy = BWAPI::BroodwarPtr->enemy();

        std::snprintf(
            crashGameInfo,
            sizeof(crashGameInfo),
            "map=%s | strategy=%s | self=%s (%s) | enemy=%s (%s)",
            BWAPI::BroodwarPtr->mapFileName().c_str(),
            Config::Strategy::StrategyName.c_str(),
            self ? self->getName().c_str() : "unknown",
            self ? self->getRace().getName().c_str() : "unknown",
            enemy ? enemy->getName().c_str() : "unknown",
            enemy ? enemy->getRace().getName().c_str() : "unknown");
    }

#ifdef _WIN32
    InterlockedExchange(&crashRecordWritten, 0);
    InterlockedExchange(&lastCrashFrame, -2);
    InterlockedExchange(&lastCrashCode, 0);
    InterlockedExchange(&lastCrashAddress, 0);
    InterlockedExchange(&lastCrashTick, 0);

    // Initialize DbgHelp while the process is healthy. At crash time we use
    // the captured exception CONTEXT with StackWalk64. If a matching PDB is
    // available for GrimHammer.dll, function names and source lines can be
    // resolved; otherwise the log still contains module + offset addresses.
    SymSetOptions(
        SymGetOptions() |
        SYMOPT_DEFERRED_LOADS |
        SYMOPT_UNDNAME |
        SYMOPT_LOAD_LINES |
        SYMOPT_FAIL_CRITICAL_ERRORS);
    symbolEngineInitialized =
        SymInitialize(GetCurrentProcess(), nullptr, TRUE) == TRUE;

    // A vectored handler runs before BWAPI's own exception handling. This is
    // essential when BWAPI catches a fault and merely drops the AI module,
    // because the process-level unhandled filter is then never reached.
    vectoredExceptionHandler =
        AddVectoredExceptionHandler(1, grimHammerVectoredExceptionFilter);

    previousExceptionFilter =
        SetUnhandledExceptionFilter(grimHammerUnhandledExceptionFilter);

    previousTerminateHandler = std::set_terminate(grimHammerTerminateHandler);
#endif

    // Create the file immediately. Previously it appeared only if the final
    // Windows exception filter ran, so a BWAPI-intercepted fault left no file
    // and no evidence that diagnostics had been enabled at all.
    std::ostringstream session;
    session << "\n========== GRIMHAMMER CRASH LOG READY ==========\n";
    session << "Time: " << currentDateTime() << "\n";
    session << "Frame: " << crashFrame << "\n";
    session << "Game: " << crashGameInfo << "\n";
    session << "Configured path: " << Config::IO::CrashLogFilename << "\n";
#ifdef _WIN32
    char absolutePath[2048] = "unavailable";
    GetFullPathNameA(
        Config::IO::CrashLogFilename.c_str(),
        static_cast<DWORD>(sizeof(absolutePath)),
        absolutePath,
        nullptr);
    session << "Absolute path: " << absolutePath << "\n";
    session << "Process: " << GetCurrentProcessId() << "\n";
    session << "DbgHelp symbols: "
            << (symbolEngineInitialized ? "initialized" : "unavailable")
            << "\n";
#endif
    session << "================================================\n";
    appendCrashText(session.str());
}

void Logger::ShutdownCrashLogging()
{
    if (!crashLoggingInitialized)
    {
        return;
    }

#ifdef _WIN32
    if (symbolEngineInitialized)
    {
        SymCleanup(GetCurrentProcess());
        symbolEngineInitialized = false;
    }

    if (vectoredExceptionHandler)
    {
        RemoveVectoredExceptionHandler(vectoredExceptionHandler);
        vectoredExceptionHandler = nullptr;
    }

    SetUnhandledExceptionFilter(previousExceptionFilter);
    previousExceptionFilter = nullptr;

    std::set_terminate(previousTerminateHandler);
    previousTerminateHandler = nullptr;
#endif

    crashLoggingInitialized = false;
}

void Logger::SetCrashContext(const char * context, int frame)
{
    if (!Config::IO::EnableCrashLog)
    {
        return;
    }

    std::snprintf(
        crashContext,
        sizeof(crashContext),
        "%s",
        context ? context : "unknown");

#ifdef _WIN32
    InterlockedExchange(&crashFrame, static_cast<LONG>(frame));
#else
    crashFrame = frame;
#endif
}

void Logger::LogCaughtException(const char * callback, const char * message)
{
    if (!Config::IO::EnableCrashLog)
    {
        return;
    }

    std::ostringstream record;
    record << "\n========== GRIMHAMMER CAUGHT EXCEPTION ==========\n";
    record << "Time: " << currentDateTime() << "\n";
    record << "Frame: " << crashFrame << "\n";
    record << "Game: " << crashGameInfo << "\n";
    record << "Callback: " << (callback ? callback : "unknown") << "\n";
    record << "Last checkpoint: " << crashContext << "\n";
    record << "Message: " << (message ? message : "unknown exception") << "\n";
    record << "=================================================\n";
    appendCrashText(record.str());
}

bool Logger::CheckAddressReadable(
    const void * address,
    size_t bytes,
    const char * context,
    int frame)
{
    SetCrashContext(context ? context : "Logger::CheckAddressReadable", frame);

    if (!address || bytes == 0)
    {
        if (Config::IO::EnableCrashLog)
        {
            LogAppendToFile(
                Config::IO::CrashLogFilename,
                "[POINTER CHECK] frame=%d | %s | address=%p | bytes=%llu | invalid argument\n",
                frame,
                context ? context : "unknown",
                address,
                static_cast<unsigned long long>(bytes));
        }
        return false;
    }

#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0)
    {
        if (Config::IO::EnableCrashLog)
        {
            LogAppendToFile(
                Config::IO::CrashLogFilename,
                "[POINTER CHECK] frame=%d | %s | address=%p | VirtualQuery failed error=%lu\n",
                frame,
                context ? context : "unknown",
                address,
                static_cast<unsigned long>(GetLastError()));
        }
        return false;
    }

    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const std::uintptr_t regionEnd = regionStart + static_cast<std::uintptr_t>(mbi.RegionSize);
    const bool rangeFits = bytes <= static_cast<size_t>(regionEnd - start);
    const bool readable = mbi.State == MEM_COMMIT &&
        isReadableProtection(mbi.Protect) &&
        rangeFits;

    if (!readable && Config::IO::EnableCrashLog)
    {
        char info[512];
        describeMemoryAddress(address, info, sizeof(info));
        LogAppendToFile(
            Config::IO::CrashLogFilename,
            "[POINTER CHECK] frame=%d | %s | %s | requested=%llu bytes\n",
            frame,
            context ? context : "unknown",
            info,
            static_cast<unsigned long long>(bytes));
    }

    return readable;
#else
    (void)bytes;
    return true;
#endif
}

bool Logger::CheckBroodwar(const char * context, int frame)
{
    SetCrashContext(context ? context : "Logger::CheckBroodwar", frame);

    if (!BWAPI::BroodwarPtr)
    {
        if (Config::IO::EnableCrashLog)
        {
            LogAppendToFile(
                Config::IO::CrashLogFilename,
                "[BWAPI CHECK] frame=%d | %s | BroodwarPtr=nullptr\n",
                frame,
                context ? context : "unknown");
        }
        return false;
    }

    return CheckAddressReadable(
        reinterpret_cast<const void *>(BWAPI::BroodwarPtr),
        sizeof(void *),
        context ? context : "Logger::CheckBroodwar raw pointer",
        frame);
}

bool Logger::CheckUnit(BWAPI::Unit unit, const char * context, int frame)
{
    // Set the checkpoint before touching the BWAPI object. If unit->exists()
    // itself faults inside BWAPI.dll, the crash log will still identify the
    // exact call site supplied by ProductionManager.
    SetCrashContext(context ? context : "Logger::CheckUnit", frame);

    if (!unit)
    {
        if (Config::IO::EnableCrashLog)
        {
            LogAppendToFile(
                Config::IO::CrashLogFilename,
                "[UNIT CHECK] frame=%d | %s | unit=nullptr\n",
                frame,
                context ? context : "unknown");
        }
        return false;
    }

    // Before calling any virtual BWAPI method, verify that the raw Unit pointer
    // at least points into committed readable memory. This cannot prove that
    // the object is still logically valid, but it catches freed/unmapped pages
    // without dereferencing the object.
    if (!CheckAddressReadable(
            reinterpret_cast<const void *>(unit),
            sizeof(void *),
            context ? context : "Logger::CheckUnit raw pointer",
            frame))
    {
        return false;
    }

#ifdef _WIN32
    bool exists = false;
    __try
    {
        exists = unit->exists();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (Config::IO::EnableCrashLog)
        {
            LogAppendToFile(
                Config::IO::CrashLogFilename,
                "[UNIT CHECK] frame=%d | %s | BWAPI exception while checking unit->exists()\n",
                frame,
                context ? context : "unknown");
        }
        return false;
    }
#else
    const bool exists = unit->exists();
#endif

    if (!exists)
    {
        if (Config::IO::EnableCrashLog)
        {
            LogAppendToFile(
                Config::IO::CrashLogFilename,
                "[UNIT CHECK] frame=%d | %s | unit no longer exists\n",
                frame,
                context ? context : "unknown");
        }
        return false;
    }

    return true;
}

std::string FileUtils::ReadFile(const std::string & filename)
{
    std::stringstream ss;

    FILE *file = fopen ( filename.c_str(), "r" );
    if ( file != nullptr )
    {
        char line [ 4096 ]; /* or other suitable maximum line size */
        while ( fgets ( line, sizeof line, file ) != nullptr ) /* read a line */
        {
            ss << line;
        }
        fclose ( file );
    }
    else
    {
        BWAPI::Broodwar->printf("Could not open file: %s", filename.c_str());
    }

    return ss.str();
}
