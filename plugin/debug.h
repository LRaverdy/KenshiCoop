// Test channel: lets an external script drive a Kenshi instance (load a save, host, join, give
// orders) and dump everything KenshiCoop sees, so host and client states can be compared.
// Only active when KenshiCoop.ini has [debug] commands=1.
//
// Protocol: the script writes kcp_cmd_<pid>.txt in the game folder, one command per line:
//     <id> <command> [args...]
// The plugin consumes the file and appends "<id> ok [details]" or "<id> err <reason>" lines to
// kcp_out_<pid>.txt.
#pragma once

#include "kc/session.h"
#include "util.h"
#include "world.h"

namespace kcp {

void DebugPoll(kc::Session& session, KenshiWorld& world, bool live);

} // namespace kcp
