#pragma once
#include <string>

namespace cotwvr {

// Log lives next to the game's other user data, not in the game folder, so a
// Steam file-integrity check never sees it.
std::wstring LogDirectory();

void LogInit();
void LogShutdown();
void LogRaw(const char* text);
void Logf(const char* fmt, ...);

// *** THE RUNNING COMMENTARY CAN BE TURNED OFF. THE OPENING CANNOT. ***
//
// Called once, right after cotwvr.ini is read - which is deliberately AFTER the
// lines that say what was detected, what the game's build is and whether the
// hooks took. Those are the lines a bug report is useless without, and a log
// that can be switched off before it has said anything is a support channel
// that fails exactly when it is needed. So "off" means "stop the per-second
// commentary", not "write nothing at all".
void LogSetEnabled(bool on);

// Whether the log is still open and writing - so a diagnostic that costs
// something to GATHER can skip the work rather than formatting a line that is
// then thrown away.
bool LogEnabled();

}  // namespace cotwvr

#define COTW_LOG(...) ::cotwvr::Logf(__VA_ARGS__)
