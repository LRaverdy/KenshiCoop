// Bug reports (0.3.5): "/bug <note>" from any player becomes a file on the host,
// KenshiCoop_bugs\bug-<date>_<time>-<name>.txt next to KenshiCoop.log. The rules (parsing, throttle,
// file name, the log kept in memory) are in common/include/kc/bug_report.h; kc::Session intercepts the
// chat line and hands the requests out (TakeBugRequests). This writes them.
#pragma once

#include <string>

#include "kc/session.h"
#include "world.h"

namespace kcp {

// Host: writes the reports that are due and tells everyone in the chat. Every tick; `hostName`: ours,
// `version`: the mod's.
void BugReportUpkeep(kc::Session& s, KenshiWorld& w, const std::string& hostName, const char* version);
// The last report written by this game (full path, UTF-8), empty: none yet (tests: "buglast").
std::string LastBugReport();

} // namespace kcp
