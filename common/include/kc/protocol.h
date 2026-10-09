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
//   channel 2  unreliable, sequenced  : vitals (health), on its own channel so it never races snapshots
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kc/wire.h"

namespace kc {

constexpr uint32_t kMagic = 0x4B434F50; // "KCOP"
constexpr uint16_t kProtocolVersion = 7;
constexpr uint16_t kDefaultPort = 27960;
constexpr uint8_t kMaxPlayers = 8;
constexpr size_t kMaxNameLen = 24;
constexpr size_t kMaxChatLen = 200;
constexpr size_t kMaxPacketSize = 64 * 1024;   // ENet refuses anything larger on receive
constexpr size_t kSnapshotBudget = 1100;       // bytes per snapshot packet, stays under one MTU
constexpr uint32_t kMaxEntitiesPerMsg = 512;

enum Channel : uint8_t { kChanReliable = 0, kChanSnapshot = 1, kChanVitals = 2, kChannelCount = 3 };

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
    Vitals = 14,      // S->C  (unreliable)
    WorldBegin = 15,  // S->C  the host's world (a fresh save) is about to be streamed
    WorldChunk = 16,  // S->C  a piece of one save file
    WorldEnd = 17,    // S->C  all files sent
    Ready = 18,       // C->S  the client loaded the host's world
    Weather = 19,     // S->C  weather of every region
    Inventory = 20,   // S->C  full inventory of one character
    InvOp = 21,       // C->S  the client moved items (loot, equip, rearrange, drop)
};

// World transfer limits (a Kenshi save is a few MB).
constexpr uint64_t kMaxWorldBytes = 256ull << 20;
constexpr uint32_t kMaxWorldFiles = 20000;
constexpr size_t kWorldChunkSize = 16 * 1024;
constexpr size_t kMaxWorldPathLen = 240;

enum class RejectReason : uint8_t {
    BadProtocol = 1,
    GameMismatch = 2,   // different kenshi_x64.exe build
    ModsMismatch = 3,   // different active mod list
    WorldMismatch = 4,  // different save loaded
    Full = 5,
    BadName = 6,
    NotReady = 7,       // host has not loaded a world yet
    HostSaveFailed = 8, // host could not save its world for the joiner
    Timeout = 9,        // joining took too long
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

// How to recreate a character the client's world does not have (spawned on the host after the
// save was made: wandering squads, reinforcements...).
constexpr size_t kMaxSidLen = 160;
struct SpawnInfo {
    std::string templateSid;   // GameData string id of the character template
    std::string factionSid;    // GameData string id of its faction
    std::string name;
    float age = 0;
    bool operator==(const SpawnInfo& o) const {
        return templateSid == o.templateSid && factionSid == o.factionSid && name == o.name && age == o.age;
    }
};

struct Bind {
    uint32_t netId = 0;
    EntityKind kind = EntityKind::Character;
    Handle handle;
    uint8_t owner = 0;     // player id that may command it, 0 = nobody (world NPC)
    bool squad = false;    // member of the shared player squad (must exist on every machine)
    bool hasSpawn = false;
    SpawnInfo spawn;
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
    uint32_t combatTarget = 0;   // netId of the character it fights, 0 = not in melee combat
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
    float speed = 1.0f;     // GameWorld::frameSpeedMult
    bool paused = false;    // GameWorld::paused
    double gameHours = 0;   // in-game clock (total hours since the world began)
};

// Health of one character, authoritative on the host.
constexpr uint32_t kMaxBodyParts = 16;
enum VitalFlags : uint8_t { kVitUnconscious = 1 << 0, kVitDead = 1 << 1 };
struct PartVitals {
    float flesh = 0, stun = 0, bandage = 0;
    bool operator==(const PartVitals& o) const { return flesh == o.flesh && stun == o.stun && bandage == o.bandage; }
};
struct EntityVitals {
    uint32_t netId = 0;
    float blood = 0;
    float koTimer = 0;
    uint8_t flags = 0;
    std::vector<PartVitals> parts;
};
struct VitalsMsg {
    uint32_t tick = 0;
    std::vector<EntityVitals> entities;
};

struct Ping {
    double t = 0;  // sender clock, echoed back in Pong
};

// ---- inventories ----
struct ItemState {
    std::string templateSid;
    std::string materialSid;      // may be empty
    std::string manufacturerSid;  // may be empty
    std::string section;          // inventory section name ("main", equipment slots...)
    int32_t quantity = 1;
    int16_t x = 0, y = 0;
    bool equipped = false;
    int32_t level = 0;
    float quality = 0;
    float charges = 0;
    bool sameKind(const ItemState& o) const {   // same sort of item, wherever it is
        return templateSid == o.templateSid && materialSid == o.materialSid && manufacturerSid == o.manufacturerSid && level == o.level;
    }
    bool operator==(const ItemState& o) const {
        return sameKind(o) && section == o.section && quantity == o.quantity && x == o.x && y == o.y && equipped == o.equipped;
    }
};
struct InventoryMsg {
    uint32_t netId = 0;
    std::vector<ItemState> items;
};
// One item movement made by a client player in its inventory UI, replayed by the host.
enum class InvOpKind : uint8_t { Move = 1, Drop = 2 };
struct InvOp {
    InvOpKind kind = InvOpKind::Move;
    uint32_t fromNetId = 0;
    uint32_t toNetId = 0;          // Move only (may equal fromNetId: rearranging / equipping)
    ItemState item;                // identity + where it was (section/x/y) + quantity moved
    std::string toSection;
    int16_t toX = 0, toY = 0;
};
constexpr uint32_t kMaxItemsPerInventory = 2000;

// ---- weather ----
// One weather region (Kenshi's WeatherRegion, one per biome group), identified by game data ids.
// The instance block mirrors Kenshi's WeatherInstance so the client can reproduce it exactly.
struct RegionWeather {
    std::string regionSid;    // biome group
    std::string seasonSid;
    int32_t seasonEnd = 0;    // in-game day the season ends
    std::string weatherSid;
    float effectStrength = 0, strength = 0, windSpeed = 0;
    Vec3 windDir;
    bool windBuildUpEnded = false;
    int32_t windBuildUpStart = 0, windBuildUpEnd = 0;
    float windBuildUpSpeedStart = 0, windBuildUpSpeedEnd = 0, windBuildUpAngleStart = 0, windBuildUpAngleEnd = 0;
    int32_t startMinutes = 0, endMinutes = 0, updateWindMinutes = 0;
    float time = 0;
    bool sameKind(const RegionWeather& o) const {
        return regionSid == o.regionSid && seasonSid == o.seasonSid && weatherSid == o.weatherSid && seasonEnd == o.seasonEnd &&
               startMinutes == o.startMinutes && endMinutes == o.endMinutes;
    }
};
struct WeatherMsg {
    std::vector<RegionWeather> regions;
};
constexpr uint32_t kMaxWeatherRegions = 256;

// ---- world transfer ----
struct WorldFile {
    std::string path;            // relative, '/'-separated, validated by ValidWorldPath
    std::vector<uint8_t> data;
};
struct WorldBegin {
    uint64_t totalBytes = 0;
    uint32_t fileCount = 0;
};
struct WorldChunk {
    uint32_t file = 0;           // index in [0, fileCount)
    std::string path;            // only in the first chunk of a file (offset == 0)
    uint64_t fileSize = 0;       // only in the first chunk of a file
    uint64_t offset = 0;
    std::vector<uint8_t> data;
};
struct WorldEnd {
    uint64_t worldHash = 0;      // fingerprint the client must see once it has loaded the world
};
struct ReadyMsg {
    uint64_t worldHash = 0;
};

// A save-relative path coming from the network: no traversal, no absolute paths, no drive or
// stream syntax, printable characters only. Anything else is refused.
bool ValidWorldPath(const std::string& p);

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
void Encode(Writer& w, const WorldBegin& m);
void Encode(Writer& w, const WorldChunk& m);
void Encode(Writer& w, const WorldEnd& m);
void Encode(Writer& w, const ReadyMsg& m);
void Encode(Writer& w, const WeatherMsg& m);
void Encode(Writer& w, const InventoryMsg& m);
void Encode(Writer& w, const InvOp& m);

// Snapshots are split into packets that each fit `budget` bytes; every packet is self-contained.
std::vector<std::vector<uint8_t>> EncodeSnapshot(const Snapshot& s, size_t budget = kSnapshotBudget);
std::vector<std::vector<uint8_t>> EncodeVitals(const VitalsMsg& v, size_t budget = kSnapshotBudget);

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
bool Decode(Reader& r, VitalsMsg& m);
bool Decode(Reader& r, WorldBegin& m);
bool Decode(Reader& r, WorldChunk& m);
bool Decode(Reader& r, WorldEnd& m);
bool Decode(Reader& r, ReadyMsg& m);
bool Decode(Reader& r, WeatherMsg& m);
bool Decode(Reader& r, InventoryMsg& m);
bool Decode(Reader& r, InvOp& m);
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
