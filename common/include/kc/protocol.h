// KenshiCoop wire protocol.
//
// Authority model: the host runs the only authoritative simulation. Clients send commands
// (orders for the characters they own) and render the state the host replicates. Nothing a
// client simulates locally is ever trusted or sent back, so client state cannot drift away
// from the host: every replicated field is overwritten by the next snapshot.
//
// Transport: ENet, one message per packet.
//   channel 0  reliable, ordered      : session, binding, commands, events
//   channel 1  unreliable, sequenced  : snapshots (newest wins, stale ones are dropped by ENet)
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kc/wire.h"

namespace kc {

constexpr uint32_t kMagic = 0x4B434F50; // "KCOP"
constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 27960;
constexpr uint8_t kMaxPlayers = 8;
constexpr size_t kMaxNameLen = 24;
constexpr size_t kMaxChatLen = 200;
constexpr size_t kMaxPacketSize = 64 * 1024;   // ENet refuses anything larger on receive
constexpr size_t kSnapshotBudget = 1100;       // bytes per snapshot packet, stays under one MTU
constexpr uint32_t kMaxEntitiesPerMsg = 512;

enum Channel : uint8_t { kChanReliable = 0, kChanSnapshot = 1, kChannelCount = 2 };

enum class Msg : uint8_t {
    Hello = 1,        // C->S
    Welcome = 2,      // S->C
    Reject = 3,       // S->C
    PlayerJoined = 4, // S->C
    PlayerLeft = 5,   // S->C
    Chat = 6,         // both
    Bind = 7,         // S->C  entity netId <-> game handle, plus owner
    Unbind = 8,       // S->C
    Snapshot = 9,     // S->C  (unreliable)
    Command = 10,     // C->S
    TimeState = 11,   // S->C
    Ping = 12,        // both
    Pong = 13,        // both
};

enum class RejectReason : uint8_t {
    BadProtocol = 1,
    GameMismatch = 2,   // different kenshi_x64.exe build
    ModsMismatch = 3,   // different active mod list
    WorldMismatch = 4,  // different save loaded
    Full = 5,
    BadName = 6,
    NotReady = 7,       // host has not loaded a world yet
};
const char* ToString(RejectReason r);

// Kenshi's persistent object handle (class `hand`). Identical on every machine that loaded the
// same save, so it is how pre-existing world objects are matched between host and clients.
struct Handle {
    uint32_t type = 0, container = 0, containerSerial = 0, index = 0, serial = 0;
    bool operator==(const Handle& o) const {
        return type == o.type && container == o.container && containerSerial == o.containerSerial &&
               index == o.index && serial == o.serial;
    }
    bool operator!=(const Handle& o) const { return !(*this == o); }
    bool valid() const { return serial != 0 || index != 0; }
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
};

// ---- session ----
struct Hello {
    uint16_t protocol = kProtocolVersion;
    uint64_t gameBuild = 0;  // first 8 bytes of SHA-256(kenshi_x64.exe)
    uint64_t modsHash = 0;   // hash of the ordered active mod list
    uint64_t worldHash = 0;  // fingerprint of the loaded save
    std::string name;
};
struct PlayerInfo {
    uint8_t id = 0;
    std::string name;
};
struct Welcome {
    uint8_t yourId = 0;
    uint64_t worldHash = 0;  // host world fingerprint; client verifies after it loads
    double hostTime = 0;
    std::vector<PlayerInfo> players;
};
struct Reject {
    RejectReason reason = RejectReason::BadProtocol;
};
struct PlayerLeft {
    uint8_t id = 0;
};
struct Chat {
    uint8_t from = 0;  // set by the host, ignored when sent by a client
    std::string text;
};

// ---- replication ----
enum class EntityKind : uint8_t { Character = 1 };

struct Bind {
    uint32_t netId = 0;
    EntityKind kind = EntityKind::Character;
    Handle handle;
    uint8_t owner = 0;  // player id that may command it, 0 = host/world
};
struct Unbind {
    uint32_t netId = 0;
};

enum EntityFlags : uint8_t {
    kFlagMoving = 1 << 0,
    kFlagRunning = 1 << 1,
    kFlagDown = 1 << 2,   // unconscious / ragdoll
    kFlagDead = 1 << 3,
    kFlagIndoors = 1 << 4,
};

struct EntityState {
    uint32_t netId = 0;
    Vec3 pos;
    Quat rot;
    Vec3 dest;       // movement destination (lets the client animate locomotion naturally)
    uint8_t flags = 0;
};

struct Snapshot {
    uint32_t tick = 0;     // host tick counter, used for ordering and debugging
    double hostTime = 0;   // host simulation clock (seconds) when sampled
    std::vector<EntityState> entities;
};

enum class CommandKind : uint8_t {
    MoveTo = 1,  // walk/run to `pos`
    Stop = 2,
};
struct Command {
    uint32_t seq = 0;
    uint32_t netId = 0;
    CommandKind kind = CommandKind::MoveTo;
    Vec3 pos;
    bool run = false;
};

struct TimeState {
    float speed = 1.0f;   // GameWorld::frameSpeedMult
    bool paused = false;
};

struct Ping {
    double t = 0;  // sender clock, echoed back in Pong
};

// ---- encoding ----
// Encoders append the message id byte first. Decoders expect the id byte to have been consumed
// (see PeekType) and fail on trailing garbage.
void Encode(Writer& w, const Hello& m);
void Encode(Writer& w, const Welcome& m);
void Encode(Writer& w, const Reject& m);
void Encode(Writer& w, const PlayerInfo& m);  // PlayerJoined
void Encode(Writer& w, const PlayerLeft& m);
void Encode(Writer& w, const Chat& m);
void Encode(Writer& w, const Bind& m);
void Encode(Writer& w, const Unbind& m);
void Encode(Writer& w, const Command& m);
void Encode(Writer& w, const TimeState& m);
void EncodePing(Writer& w, const Ping& m, bool pong);

// Snapshots are split into packets that each fit `budget` bytes; every packet is self-contained.
std::vector<std::vector<uint8_t>> EncodeSnapshot(const Snapshot& s, size_t budget = kSnapshotBudget);

std::optional<Msg> PeekType(Reader& r);
bool Decode(Reader& r, Hello& m);
bool Decode(Reader& r, Welcome& m);
bool Decode(Reader& r, Reject& m);
bool Decode(Reader& r, PlayerInfo& m);
bool Decode(Reader& r, PlayerLeft& m);
bool Decode(Reader& r, Chat& m);
bool Decode(Reader& r, Bind& m);
bool Decode(Reader& r, Unbind& m);
bool Decode(Reader& r, Snapshot& m);
bool Decode(Reader& r, Command& m);
bool Decode(Reader& r, TimeState& m);
bool Decode(Reader& r, Ping& m);

// Name rules: 1..kMaxNameLen printable ASCII, no leading/trailing spaces.
bool ValidName(const std::string& s);
// Chat: strips control characters, truncates to kMaxChatLen.
std::string SanitizeChat(const std::string& s);

// Smallest-three quaternion packing into 32 bits (~0.001 precision per component).
uint32_t PackQuat(const Quat& q);
Quat UnpackQuat(uint32_t v);

// FNV-1a 64, used for mod list / world fingerprints.
uint64_t Fnv1a64(const void* data, size_t n, uint64_t seed = 0xcbf29ce484222325ull);

} // namespace kc
