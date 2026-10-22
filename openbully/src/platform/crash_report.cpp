// Track 49 (diagnostic): unhandled-exception reporter.
// The boss-fight verification hit recurring int3 crashes (0x80000003) whose
// Windows event-log entries only gave a bare offset. This handler prints the
// faulting code as module+offset through the game logger, so a crash in any
// log run is directly actionable. Kept permanently: it is 20 lines, inactive
// until a crash happens, and makes future verifications cheaper.
#include <windows.h>

#include "logger.h"

namespace OpenBully {

static LONG WINAPI CrashReporter(EXCEPTION_POINTERS* ep) {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&CrashReporter), &mod);
    const uintptr_t fault =
        reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    const uintptr_t base = reinterpret_cast<uintptr_t>(mod);
    Logger::Error("CRASH: code 0x%08lX at module+0x%llX (addr %p)",
                  static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                  static_cast<unsigned long long>(mod ? fault - base : 0),
                  ep->ExceptionRecord->ExceptionAddress);
    // Let WER take the crash after logging (EXECUTE_HANDLER would swallow it
    // and leave the process in an unknown state).
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashReporter() {
    SetUnhandledExceptionFilter(CrashReporter);
}

} // namespace OpenBully
