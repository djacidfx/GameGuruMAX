#include <windows.h>
#include <dbghelp.h>
#include <tchar.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "dbghelp.lib")

#define MAX_PATH 1024

// global we can populate with the current running version to match EXE/PDB pairs
char g_pCrashVersionINIValue[256] = "Very Early";

// stores address to Wicked MAX debug log
static char g_pDebugExtraInfo[10240] = { 0 };

// What time is it
std::string GetTimestamp()
{
    time_t now = time(nullptr);
    struct tm localTime;
    localtime_s(&localTime, &now);

    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
    return std::string(buffer);
}

// Crash handler
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* pExceptionInfo)
{
    // Get path to the EXE folder
    char exeFile[MAX_PATH];
    GetModuleFileNameA(NULL, exeFile, MAX_PATH);

    char exePath[MAX_PATH];
    strcpy_s(exePath, MAX_PATH, exeFile);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    // Build paths
    char logPath[MAX_PATH];
    strcpy_s(logPath, exePath);
    strcat_s(logPath, "Guru-Crash.log");

    // full path message
    char fullPathMsg[MAX_PATH * 2];
    sprintf_s(fullPathMsg, MAX_PATH * 2, "A crash has been detected!\r\nA crash report has been created in file '%s'", logPath);
    MessageBoxA(
        NULL,
        fullPathMsg,
        "GameGuru MAX Crash",
        MB_OK | MB_ICONERROR
    );

    // Initialize symbol handler
    HANDLE process = GetCurrentProcess();

    // start crash log
    std::ostringstream log;
    log << "\r\n==== GAMEGURU MAX CRASH DETECTED ====\r\n";
    log << "EXE Module:      " << exeFile << "\r\n";
    log << "Time:            " << GetTimestamp() << "\r\n";
    log << "Build:           " << g_pCrashVersionINIValue << "\r\n";
    log << "Exception code:  0x" << std::hex << pExceptionInfo->ExceptionRecord->ExceptionCode << "\r\n";

    // two ways to use symbols
    bool bSymbCall = true;
    bool bInvadeProcessMode = true;

    SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);

    if (!SymInitialize(process, NULL, TRUE))
    {
        log << "SymInitialize:   Invade Process FAILED.\r\n";
        bInvadeProcessMode = false;
        bSymbCall = false;

        SymCleanup(process); // ignore result

        if (SymInitialize(process, NULL, FALSE))
        {
            // if TRUE fails, FALSE is likely to succeed :)
            bSymbCall = true;
        }
    }

    bool bSymbolsAvailable = true;
    if (bSymbCall == false)
    {
        DWORD err = GetLastError();
        log << "SymInitialize failed: " << std::dec << err << "\r\n";
        bSymbolsAvailable = false;
    }

    // We'll keep these around for stack translation.
    DWORD64 baseAddress = 0;
    DWORD64 moduleBase = 0;
    DWORD64 crashAddress = 0;
    DWORD64 offset = 0;
    DWORD64 lookupAddress = 0;

    if (bSymbolsAvailable)
    {
        // Load the module (EXE)
        baseAddress = SymLoadModuleEx(
            process,
            NULL,
            exeFile,
            NULL,
            0,  // IMPORTANT: let DbgHelp decide (as you found)
            0,
            NULL,
            0
        );

        if (baseAddress == 0)
        {
            DWORD e = GetLastError();
            log << "SymLoadModuleEx failed/returned 0. GetLastError=" << std::dec << e << "\r\n";
        }
        else
        {
            log << "baseAddress=" << std::dec << baseAddress << "\r\n";
        }

        // the address we need is not the runtime address the exception provides!
        moduleBase = (DWORD64)GetModuleHandle(NULL);
        crashAddress = (DWORD64)pExceptionInfo->ExceptionRecord->ExceptionAddress;
        offset = crashAddress - moduleBase;
        lookupAddress = baseAddress + offset;

        // Get source line info for the crash point
        std::string lineInfo;
        IMAGEHLP_LINE64 lineData = { 0 };
        DWORD displacement = 0;
        lineData.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

        bool bResultOfLineGrab = false;
        if (bInvadeProcessMode == true)
        {
            // Use ExceptionAddress directly.
            bResultOfLineGrab = SymGetLineFromAddr64(process, crashAddress, &displacement, &lineData);
        }
        if (!bResultOfLineGrab)
        {
            // Fall back to translated address (preferred base)
            bResultOfLineGrab = SymGetLineFromAddr64(process, lookupAddress, &displacement, &lineData);
        }
        if (!bResultOfLineGrab)
        {
            DWORD e = GetLastError();
            log << "SymGetLineFromAddr64 FAILED. GetLastError=" << std::dec << e << "\r\n";
        }
        else
        {
            std::ostringstream l;
            l << lineData.FileName << ":" << lineData.LineNumber;
            lineInfo = l.str();
        }

        // Have symbol info to report in the log
        log << "Module address:  0x" << std::hex << moduleBase << "\r\n";
        log << "Crash address:   0x" << std::hex << crashAddress << "\r\n";
        log << "Base address:    0x" << std::hex << baseAddress << "\r\n";
        log << "Offset value:    0x" << std::hex << offset << "\r\n";
        log << "Lookup address:  0x" << std::hex << lookupAddress << "\r\n";
        if (!lineInfo.empty())
        {
            log << "Source Code:     " << lineInfo << "\r\n";
        }

        // ----------------------------
        // 5-layer stack trace (callers)
        // ----------------------------
        // Note: StackWalk64 mutates the CONTEXT you pass, so use a copy.
        CONTEXT ctx = *pExceptionInfo->ContextRecord;

        HANDLE thread = GetCurrentThread();

        STACKFRAME64 frame = {};
        DWORD machineType = 0;

#if defined(_M_X64)
        machineType = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrFrame.Offset = ctx.Rbp;
#elif defined(_M_IX86)
        machineType = IMAGE_FILE_MACHINE_I386;
        frame.AddrPC.Offset = ctx.Eip;
        frame.AddrStack.Offset = ctx.Esp;
        frame.AddrFrame.Offset = ctx.Ebp;
#else
        machineType = 0;
#endif

        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Mode = AddrModeFlat;

        auto TranslateAddrIfNeeded = [&](DWORD64 runtimeAddr) -> DWORD64
        {
            // If invade worked, DbgHelp knows true module bases -> use runtime addr.
            if (bInvadeProcessMode) return runtimeAddr;

            // If invade failed, DbgHelp often thinks your EXE lives at preferred base (0x140...).
            // Translate runtime address (0x7ff6...) into preferred-base address (0x140...).
            if (moduleBase != 0 && baseAddress != 0 && runtimeAddr >= moduleBase)
            {
                DWORD64 off = runtimeAddr - moduleBase;
                return baseAddress + off;
            }
            return runtimeAddr; // best effort
        };

        // For SymFromAddr
        BYTE symBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        SYMBOL_INFO* pSym = (SYMBOL_INFO*)symBuffer;
        pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSym->MaxNameLen = MAX_SYM_NAME;

        log << "Call stack (5 frames):\r\n";

        // Frame 0 should be the crash site; subsequent frames are callers.
        for (int i = 0; i < 5; ++i)
        {
            bool walked = false;

            if (i == 0)
            {
                // Use the PC from the context as the first frame without walking
                walked = true;
            }
            else
            {
                walked = StackWalk64(
                    machineType,
                    process,
                    thread,
                    &frame,
                    &ctx,
                    NULL,
                    SymFunctionTableAccess64,
                    SymGetModuleBase64,
                    NULL
                ) ? true : false;
            }

            if (!walked)
            {
                log << "  #" << std::dec << i << ": <StackWalk64 stopped> err=" << std::dec << GetLastError() << "\r\n";
                break;
            }

            // For i==0, frame.AddrPC.Offset is already set from ctx above.
            DWORD64 runtimePC = (i == 0) ? frame.AddrPC.Offset : frame.AddrPC.Offset;
            if (runtimePC == 0)
            {
                log << "  #" << std::dec << i << ": <null PC>\r\n";
                break;
            }

            DWORD64 pc = TranslateAddrIfNeeded(runtimePC);

            // Function
            std::string fn = "???";
            DWORD64 dispSym = 0;
            if (SymFromAddr(process, pc, &dispSym, pSym))
                fn = pSym->Name;

            // File/line
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(line);
            DWORD dispLine = 0;

            if (SymGetLineFromAddr64(process, pc, &dispLine, &line))
            {
                log << "  #" << std::dec << i
                    << "  0x" << std::hex << runtimePC
                    << "  " << fn
                    << "  (" << line.FileName << ":" << std::dec << line.LineNumber << ")\r\n";
            }
            else
            {
                DWORD e = GetLastError();
                log << "  #" << std::dec << i
                    << "  0x" << std::hex << runtimePC
                    << "  " << fn
                    << "  (no line, err=" << std::dec << e << ")\r\n";
            }
        }

        // NOW cleanup (after stack trace)
        SymCleanup(process);
    }

    if (strlen(g_pDebugExtraInfo) > 0)
    {
        log << g_pDebugExtraInfo << "\r\n";
    }
    log << "=====================================\r\n";

    // Write to log
    HANDLE hFile = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD bytesWritten;
        WriteFile(hFile, log.str().c_str(), (DWORD)log.str().size(), &bytesWritten, NULL);
        FlushFileBuffers(hFile);
        CloseHandle(hFile);
    }

    // Also create dump that we can load in visual studio later to debug.
    strcpy_s(logPath, exePath);
    strcat_s(logPath, "crashdump.dmp");

    hFile = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = pExceptionInfo;
    exceptionInfo.ClientPointers = TRUE;

    BOOL success = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        MiniDumpNormal, // MiniDumpWithFullMemory,
        &exceptionInfo,
        nullptr,
        nullptr
    );
    CloseHandle(hFile);

    Sleep(100);
    return EXCEPTION_EXECUTE_HANDLER;
}

void InitCrashHandler()
{
    SetUnhandledExceptionFilter(CrashHandler);
}

char* GetCrashHandlerDebugLogRef()
{
    if (strlen(g_pDebugExtraInfo) == 0) strcpy_s(g_pDebugExtraInfo, 10240, "Extra Debug Log:");
    return g_pDebugExtraInfo;
}
