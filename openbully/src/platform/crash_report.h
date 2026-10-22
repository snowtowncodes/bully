#pragma once

// Track 49 (diagnostic): installs the unhandled-exception reporter that
// logs a crashing fault's code + module offset through the game logger.

namespace OpenBully {

void InstallCrashReporter();

} // namespace OpenBully
