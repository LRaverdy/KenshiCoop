// How many "KenshiCoop is calling the game" scopes are open on a thread, and their repair after an
// exception skipped their destructors.
#pragma once
#include <algorithm>

namespace kc {

// The plugin counts its open HostCallScope / AnimReplayScope objects per thread; while the count is
// above zero, the order hooks take every call for one of KenshiCoop's own and let the game run it.
// An access violation caught by an __except handler does not run the destructors of the C++
// objects between the fault and the handler (MSVC /EHsc: the frames are marked synchronous-only),
// so a scope opened in between stays counted forever. A client then ran every later order itself
// instead of asking the host (crash at kenshi_x64+0x883B78, 10 oct.). Scopes are strictly nested:
// after a call returns, or after its exception was caught, the count must be what it was before.
struct ScopeCounts {
    int host = 0;   // HostCallScope (AnimReplayScope counts here too)
    int anim = 0;   // AnimReplayScope
};

// Puts `live` back to `mark` (taken before the call). Returns how many scopes were left open (0: the
// call was clean). A count below the mark (a scope closed twice) is put back too, returned as 0.
inline int RepairScopeCounts(ScopeCounts& live, const ScopeCounts& mark) {
    const int leaked = std::max(std::max(live.host - mark.host, live.anim - mark.anim), 0);
    live = mark;
    return leaked;
}

} // namespace kc
