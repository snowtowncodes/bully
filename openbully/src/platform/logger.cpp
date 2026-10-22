#include "logger.h"

namespace OpenBully {

void Logger::Init() {
    // Platform-specific initialization (if needed)
    // For now, just use stdout/stderr. Unbuffered so redirected/tee'd runs
    // keep their tail when the process is killed (verification scripts do).
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

} // namespace OpenBully
