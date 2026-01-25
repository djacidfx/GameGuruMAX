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
    sprintf_s(fullPathMsg, MAX_PATH * 2, "A crash report has been created in file '%s'", logPath);
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
    if(pExceptionInfo)  log << "Exception code:  0x" << std::hex << pExceptionInfo->ExceptionRecord->ExceptionCode << "\r\n";

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

		// Refresh module list to be sure
        SymRefreshModuleList(process);

        // handle baseAddress return value
        if (baseAddress == 0)
        {
            DWORD e = GetLastError();
            log << "SymLoadModuleEx failed/returned 0. GetLastError=" << std::dec << e << "\r\n";
        }
        else
        {
            log << "SymLoadModuleEx: " << std::dec << baseAddress << "\r\n";
        }

        // the address we need is not the runtime address the exception provides!
        moduleBase = (DWORD64)GetModuleHandle(NULL);
        if (pExceptionInfo)
        {
            crashAddress = (DWORD64)pExceptionInfo->ExceptionRecord->ExceptionAddress;
            offset = crashAddress - moduleBase;
        }
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
            log << "Source code:     " << lineInfo << "\r\n";
        }

        // ----------------------------
        // 5-layer stack trace (callers)
        // ----------------------------
        // Note: StackWalk64 mutates the CONTEXT you pass, so use a copy.
        if (pExceptionInfo)
        {
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


            // excellent stack tracer (walk more, print best 5)
            log << "Call stack:   Up to 10 frames:\r\n";

            const int kMaxWalk = 32;
            const int kWantPrinted = 10;

            DWORD64 lastRuntimePC = 0;
            int printed = 0;

            for (int walk = 0; walk < kMaxWalk && printed < kWantPrinted; ++walk)
            {
                BOOL ok = StackWalk64(
                    machineType,
                    process,
                    thread,
                    &frame,
                    &ctx,
                    NULL,
                    SymFunctionTableAccess64,
                    SymGetModuleBase64,
                    NULL
                );

                if (!ok || frame.AddrPC.Offset == 0)
                {
                    log << "  <StackWalk64 stopped> err=" << std::dec << GetLastError() << "\r\n";
                    break;
                }

                DWORD64 runtimePC = frame.AddrPC.Offset;

                // sanity: 0x1 / 0x42 etc
                if (runtimePC < 0x10000)
                {
                    log << "  <invalid PC 0x" << std::hex << runtimePC << ">\r\n";
                    break;
                }

                // skip duplicates (can happen depending on unwind/prolog)
                if (runtimePC == lastRuntimePC)
                    continue;
                lastRuntimePC = runtimePC;

                // We will try to resolve symbols/lines using runtime PC first.
                // If that fails (common when InvadeProcess fails), fall back to translated PC.
                DWORD64 pcRuntime = runtimePC;
                DWORD64 pcXlated = TranslateAddrIfNeeded(runtimePC);

                auto ResolveFnAndLine = [&](DWORD64 addr,
                    std::string& outFn,
                    DWORD64& outDispSym,
                    std::string& outFile,
                    DWORD& outLine,
                    DWORD& outDispLine,
                    DWORD& outLineErr) -> bool
                {
                    outFn = "???";
                    outDispSym = 0;

                    if (SymFromAddr(process, addr, &outDispSym, pSym))
                        outFn = pSym->Name;

                    IMAGEHLP_LINE64 line = {};
                    line.SizeOfStruct = sizeof(line);
                    DWORD dispLine = 0;

                    if (SymGetLineFromAddr64(process, addr, &dispLine, &line))
                    {
                        outFile = line.FileName ? line.FileName : "";
                        outLine = line.LineNumber;
                        outDispLine = dispLine;
                        outLineErr = 0;
                        return true;
                    }

                    outLineErr = GetLastError();
                    outFile.clear();
                    outLine = 0;
                    outDispLine = 0;
                    return false;
                };

                std::string fn;
                DWORD64 dispSym = 0;
                std::string file;
                DWORD lineNo = 0;
                DWORD dispLine = 0;
                DWORD lineErr = 0;

                bool haveLine = ResolveFnAndLine(pcRuntime, fn, dispSym, file, lineNo, dispLine, lineErr);

                // If runtime address failed, try translated address:
                if (!haveLine && pcXlated != pcRuntime)
                {
                    std::string fn2;
                    DWORD64 dispSym2 = 0;
                    std::string file2;
                    DWORD lineNo2 = 0;
                    DWORD dispLine2 = 0;
                    DWORD lineErr2 = 0;

                    bool haveLine2 = ResolveFnAndLine(pcXlated, fn2, dispSym2, file2, lineNo2, dispLine2, lineErr2);

                    // Prefer the translated result if it gives us line info, or even a better symbol name:
                    if (haveLine2 || fn == "???")
                    {
                        fn = fn2;
                        dispSym = dispSym2;
                        file = file2;
                        lineNo = lineNo2;
                        dispLine = dispLine2;
                        lineErr = lineErr2;
                        haveLine = haveLine2;
                    }
                }

                // Module name (handy for filtering / sanity)
                std::string modName = "?";
                IMAGEHLP_MODULE64 mod = {};
                mod.SizeOfStruct = sizeof(mod);
                if (SymGetModuleInfo64(process, pcXlated, &mod))
                {
                    if (mod.ModuleName) modName = mod.ModuleName;
                }

                // Print
                log << "  #" << std::dec << printed
                    << "  0x" << std::hex << runtimePC
                    << "  [" << modName << "] "
                    << fn;

                if (dispSym)
                    log << "+0x" << std::hex << dispSym;

                if (haveLine && !file.empty())
                {
                    log << "  (" << file << ":" << std::dec << lineNo << ")";
                    if (dispLine)
                        log << " +0x" << std::hex << dispLine;
                    log << "\r\n";
                }
                else
                {
                    log << "  (no line, err=" << std::dec << lineErr << ")\r\n";
                }

                ++printed;
            }
        }

        // NOW cleanup (after stack trace)
        SymCleanup(process);
    }
	// used to dump parameter values using GG_CRASH_CONTEXT macro when we want to investigate a function call 
    extern int g_iDisableCrashLogSystem;
    if (g_iDisableCrashLogSystem == 0)
    {
        extern thread_local char g_CrashContext[1024];
        if (strlen(g_CrashContext) > 0)
        {
            log << "Crash Ring Buffer: " << g_CrashContext << "\r\n";
        }
        // used to dump crash extra debug info from actual Wicked Engine calls
        if (strlen(g_pDebugExtraInfo) > 0)
        {
            log << g_pDebugExtraInfo << "\r\n";
        }
    }
    else
    {
        log << "Crash Ring Buffer and Debug Logs disabled with 'disablecrashlogsystem = 1'\r\n";
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

    if (pExceptionInfo)
    {
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
    }

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
    if (strlen(g_pDebugExtraInfo) == 0) strcpy_s(g_pDebugExtraInfo, 10240, "GFX Debug Log:");
    return g_pDebugExtraInfo;
}
