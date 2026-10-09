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
constexpr uint16_t kProtocolVersion = 20;
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
    Effects = 22,     // S->C  weather effects the host's game placed (lightning, storms, gas clouds)
    Anim = 23,        // S->C  animations the host's characters start and stop (attacks, actions, stumbles)
    AnimFrame = 24,   // S->C  (unreliable) every animation each nearby character is playing: name, time, weight
    Ground = 25,      // S->C  items dropped on and picked up from the ground in the host's world
    Progress = 26,    // S->C  skill levels of characters, the player faction's money
    Dialog = 27,      // S->C  speech bubbles; conversations of the receiving player's characters
    DialogReply = 28, // C->S  the player picked an answer in a conversation
    Squads = 29,      // S->C  how the player faction's characters are split into squads
    Appearance = 30,  // both  a character's looks and name, made in the game's character editor
    EditCharacter = 31, // S->C  open the character editor on your new character
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
    Kicked = 10,        // removed by the host
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
    uint64_t steamId = 0;    // who the player is, whatever name they use (0: unknown)
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
    Handle previous;       // valid when the same character got a new handle (died, changed squad...)
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
    uint8_t gait = 0;            // movement speed order (Kenshi's MoveSpeed: walk, jog, run...)
    float pace = 0;              // desired speed, units/s (the game's "flat out" is 999): picks walk or run animations
};

struct Snapshot {
    uint32_t tick = 0;     // host tick counter, used for ordering and debugging
    double hostTime = 0;   // host simulation clock (seconds) when sampled
    std::vector<EntityState> entities;
};

enum class CommandKind : uint8_t {
    MoveTo = 1,  // walk/run to `pos`
    Stop = 2,
    PickUp = 3,  // take the item `itemSid` lying at `pos`
    Task = 4,    // a player order (first aid, eat, use a bed, open a door...) given through the game's UI
    SquadMove = 5,   // move the character into the squad of `subject` (none: a new squad) at position `task`
};
// Which PlayerInterface function the client's UI called for a Task (the host calls the same one).
enum class TaskVia : uint8_t {
    AddOrder = 1,      // addOrderSelectedCharacters(building, task, subject, shift, addDontClear, pos)
    NewTask = 2,       // newPlayerTaskSelectedCharacters(task, target, building, pos, addDontClear)
    TaskNearest = 3,   // addTaskNearestSelectedCharacter(building, task, subject, shift, pos, noAnimals)
    AddJob = 4,        // addJobSelectedCharacters(task, subject, shift, add, pos)
    SetOrder = 5,      // setOrderSelectedCharacters(order)
};
struct Command {
    uint32_t seq = 0;
    uint32_t netId = 0;
    CommandKind kind = CommandKind::MoveTo;
    Vec3 pos;
    bool run = false;
    std::string itemSid;   // PickUp
    // Task
    TaskVia via = TaskVia::AddOrder;
    int32_t task = 0;
    bool shift = false, add = false;
    Handle subject;        // the object the order is about (character, item, building), as the host knows it
    Handle building;       // the building the character goes into, if any
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
    float hunger = 0;
    uint8_t flags = 0;
    std::vector<PartVitals> parts;
};
struct VitalsMsg {
    uint32_t tick = 0;
    std::vector<EntityVitals> entities;
};

// Skill levels: the integer part is the level, the fraction the progress to the next one.
constexpr size_t kStatCount = 34;   // every stat with a field of its own (see kenshi.cpp kStatOffsets)
struct CharProgress {
    uint32_t netId = 0;
    std::vector<float> stats;   // kStatCount values
};
struct ProgressMsg {
    bool hasMoney = false;
    int32_t money = 0;          // the player faction's cats
    std::vector<CharProgress> chars;
};

// Conversations happen in the host's world. Lines said aloud (speech bubbles) are shown to
// everyone; the conversation window of a player's character opens on that player's screen.
enum class DialogKind : uint8_t {
    Say = 1,     // netId says `text` (shout: louder bubble)
    Open = 2,    // a conversation window opens: netId = who the player talks with, text = their name
    Text = 3,    // what they say now (text) and the answers the player can pick (replies)
    Close = 4,
};
constexpr size_t kMaxDialogText = 2000;
constexpr size_t kMaxDialogReplies = 16;
struct DialogEvent {
    DialogKind kind = DialogKind::Say;
    uint32_t dialogId = 0;   // which conversation (Open/Text/Close)
    uint32_t netId = 0;
    std::string text;
    bool shout = false;
    std::vector<std::string> replies;
};
struct DialogMsg {
    std::vector<DialogEvent> events;
};
struct DialogReply {
    uint32_t dialogId = 0;
    int32_t index = 0;       // position in the last `replies`
};

// The player faction's squads, in the host's order: name and members (netIds, in squad order).
struct SquadInfo {
    std::string name;
    std::vector<uint32_t> members;
};
struct SquadsMsg {
    std::vector<SquadInfo> squads;
};

// A character's appearance: every value of its appearance GameData (race, gender, head, hair,
// sliders...) and its name.
enum class AppearanceType : uint8_t { Bool = 1, Int = 2, Float = 3, String = 4, Vec3 = 5, Quat = 6, Refs = 7 };
constexpr size_t kMaxAppearanceFields = 1024;
struct AppearanceField {
    AppearanceType type = AppearanceType::Float;
    std::string key;
    bool b = false;
    int32_t i = 0;
    float f[4] = {0, 0, 0, 0};          // Float: f[0]; Vec3: x y z; Quat: w x y z
    std::string s;                      // String
    std::vector<std::string> refs;      // Refs: string ids of the referenced GameData
    bool operator==(const AppearanceField& o) const {
        return type == o.type && key == o.key && b == o.b && i == o.i && f[0] == o.f[0] && f[1] == o.f[1] && f[2] == o.f[2] &&
               f[3] == o.f[3] && s == o.s && refs == o.refs;
    }
};
struct AppearanceMsg {
    uint32_t netId = 0;
    std::string name;
    std::vector<AppearanceField> fields;
};
struct EditCharacter {
    uint32_t netId = 0;
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

// ---- weather effects ----
// A weather effect the host's game placed in the world at random: a lightning bolt, a wandering
// dust storm or gas cloud, a point effect. It belongs to one effect group of one weather region
// (the region's effect, counted among the region's groups showing that same effect); clients
// recreate it there with the host's random values instead of rolling their own.
enum class EffectKind : uint8_t { Point = 1, Wandering = 2 };
struct WeatherEffect {
    uint32_t id = 0;              // host-assigned, increasing
    EffectKind kind = EffectKind::Point;
    std::string regionSid;
    std::string effectSid;
    uint8_t ordinal = 0;
    Vec3 pos;
    float age = 0;                // seconds since it appeared
    float life = 0;               // seconds left (unless endless)
    bool endless = false;
    float strength = 0;
    bool struck = false;          // point: lightning already hit
    float strikeIn = 0;           //        seconds before it hits
    Vec3 dir, turnTo;             // wandering: heading, and the heading it is turning to
};
struct EffectState {              // where a wandering effect is now
    uint32_t id = 0;
    Vec3 pos, dir, turnTo;
};
struct EffectsMsg {
    bool full = false;            // `spawned` is the host's complete live set: anything else goes
    std::vector<WeatherEffect> spawned;
    std::vector<EffectState> moved;
    std::vector<uint32_t> ended;
    bool empty() const { return !full && spawned.empty() && moved.empty() && ended.empty(); }
};
constexpr uint32_t kMaxEffectsPerMsg = 4096;

// ---- animations ----
// What makes a character play a non-locomotion animation, as the host's game did it: an attack or
// block technique, an action (sitting, working, healing...), a stumble, combat or carry mode. The
// client's own game never starts these for the host's characters; it replays these instead.
enum class AnimKind : uint8_t {
    Combat = 1,       // name: technique animation, a: speed
    CombatRun = 2,    //   (continuing technique)
    EndCombat = 3,
    Action = 4,       // name: animation data, a: speed multiplier, b: initial weight, flags 1: stumble
    StopAction = 5,   // name: that action only ("" = any)
    Stumble = 6,      // name: animation data
    EndStumble = 7,
    CombatMode = 8,   // flags 1: on
    Carry = 9,        // flags 1: carried, 2: left hand, 4: right hand
    State = 10,       // periodic: name = current action ("" none), flags 1: combat mode, 2/4/8: carry flags, 16/32: guard legs/upper
    DrawWeapon = 11,  // name: "<item template>\t<section it comes from>"
    Sheathe = 12,
    WeaponState = 13, // periodic: name "<item>\t<section>" in its hands, "" = hands empty
    GuardLegs = 14,   // flags 1: legs in combat idle (guard stance)
    GuardUpper = 15,  // flags 1: upper body in combat idle
    Floater = 16,     // a damage number over it: name "RRGGBBAA|text", flags: size | speed << 4
    FloaterColor = 17,// the last floater's colour changed: name "RRGGBBAA"
};
struct AnimEvent {
    uint32_t netId = 0;
    AnimKind kind = AnimKind::Action;
    std::string name;
    float a = 0, b = 0;
    uint8_t flags = 0;
};
struct AnimMsg {
    std::vector<AnimEvent> events;
};
constexpr uint32_t kMaxAnimEvents = 4096;

// What is on screen: each animation a character plays (whatever logic started it), with its time
// and blend weight. Clients impose it, so every step, guard, swing and turn looks the same.
struct AnimEntry {
    uint8_t layer = 0;
    bool looped = false, fadingOut = false;
    std::string anim;     // Ogre animation name
    std::string data;     // its animation data ("" for combat techniques)
    float time = 0, weight = 0, desired = 0, speed = 0;
};
struct AnimFrame {
    uint32_t netId = 0;
    float masterTime = 0, masterSpeed = 0;   // the clock synchronised animations (walk, run cycles) follow
    std::vector<AnimEntry> anims;
};
struct AnimFrameMsg {
    double hostTime = 0;
    std::vector<AnimFrame> chars;
};
constexpr uint32_t kMaxAnimsPerChar = 32;

// ---- items on the ground ----
enum class GroundKind : uint8_t { Dropped = 1, PickedUp = 2 };
struct GroundEvent {
    GroundKind kind = GroundKind::Dropped;
    Handle item;          // the host's handle of the item
    ItemState state;      // what it is
    Vec3 pos;             // where it lies (PickedUp: where it lay)
};
struct GroundMsg {
    std::vector<GroundEvent> events;
};
constexpr uint32_t kMaxGroundEvents = 1024;
constexpr size_t kMaxAnimNameLen = 200;

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
void Encode(Writer& w, const EffectsMsg& m);
void Encode(Writer& w, const AnimMsg& m);
void Encode(Writer& w, const GroundMsg& m);

// Snapshots are split into packets that each fit `budget` bytes; every packet is self-contained.
std::vector<std::vector<uint8_t>> EncodeSnapshot(const Snapshot& s, size_t budget = kSnapshotBudget);
void Encode(Writer& w, const ProgressMsg& m);
void Encode(Writer& w, const SquadsMsg& m);
void Encode(Writer& w, const AppearanceMsg& m);
bool Decode(Reader& r, AppearanceMsg& m);
void Encode(Writer& w, const EditCharacter& m);
bool Decode(Reader& r, EditCharacter& m);
bool Decode(Reader& r, SquadsMsg& m);
void Encode(Writer& w, const DialogMsg& m);
bool Decode(Reader& r, DialogMsg& m);
void Encode(Writer& w, const DialogReply& m);
bool Decode(Reader& r, DialogReply& m);
bool Decode(Reader& r, ProgressMsg& m);
std::vector<std::vector<uint8_t>> EncodeVitals(const VitalsMsg& v, size_t budget = kSnapshotBudget);
std::vector<std::vector<uint8_t>> EncodeAnimFrames(const AnimFrameMsg& m, size_t budget = kSnapshotBudget);

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
bool Decode(Reader& r, EffectsMsg& m);
bool Decode(Reader& r, AnimMsg& m);
bool Decode(Reader& r, AnimFrameMsg& m);
bool Decode(Reader& r, GroundMsg& m);
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
