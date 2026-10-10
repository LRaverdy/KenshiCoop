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
constexpr uint16_t kProtocolVersion = 33;
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
    EditState = 32,   // C->S  the character editor is open / closed here (the host waits meanwhile)
    ClientLog = 33,   // C->S  the client's log lines: the host's log shows what happens on every machine
    ClientReport = 34, // C->S  how well the client's game follows the host's (every few seconds)
    Resync = 35,      // S->C  reload the host's world now (the host's cure for any desync)
    ContainerOpen = 36,   // C->S  my character wants to look into this container (chest, shelf...)
    ContainerOpened = 37, // S->C  it is there: here is the container's netId (its items follow as an Inventory)
    ContainerClose = 38,  // both  the window is closed (client) / must close (host: caught stealing, too far)
    TradeOpen = 39,       // S->C  trade with a merchant: its shop counters (their items follow), its cats
    // ---- lot B: factions
    Factions = 43,        // S->C  the player faction's relations with every faction, both ways
    Bounties = 44,        // S->C  bounties, crimes and prison sentences of the squad's characters
    // ---- lot A: doors
    Doors = 40,           // S->C  doors and locks near the players: open/closed, locked, lock level, broken
    DoorRequest = 41,     // C->S  the player clicked a door's open / lock button
    // ---- lot D: prisons (49-51)
    Captives = 49,        // S->C  characters in a cage, in shackles, enslaved or serving a sentence (and freed ones)
    // ---- lot C: ranged combat
    Shots = 46,           // S->C  projectiles the host's characters and turrets fired (clients fire the same, visual only)
    Ranged = 47,          // S->C  aim of characters in ranged combat and of turrets near the players
    // ---- lot E: buildings
    BuildPlace = 52,      // both  a building placed in build mode (client: asks the host; host: everyone builds it)
    BuildState = 53,      // S->C  construction progress of player buildings (complete, paused, dismantling)
    BuildRemove = 54,     // S->C  a player building is gone (dismantled, destroyed)
    BuildAction = 55,     // both  buy / dismantle a building (client: asks the host; host: replay of a purchase)
    // ---- fix G6
    Stall = 70,           // S->C  your game is about to freeze a while (a far teleport loads a zone): keep the link up
    Floors = 71,          // S->C  the floor characters are on inside buildings (it drives the floor shown)
    // ---- fix G5
    JobList = 68,         // S->C  the job list (Tâches panel) of the players' characters, as the host has it
    // ---- diplomacy
    Diplomacy = 85,       // S->C  the world beyond the player faction: relations between factions, faction leaders, towns
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
struct StallMsg {   // fix G6
    uint16_t seconds = 0;   // how long the connection may stay silent
};
struct FloorEntry {
    uint32_t netId = 0;
    uint8_t group = 0;   // the game's floor group of its movement (9: ground floor / outdoors)
};
struct FloorsMsg {   // fix G6
    std::vector<FloorEntry> entries;
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
    uint32_t carrying = 0;       // netId of the character it carries on its shoulder, 0 = none
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
    // the Tâches panel (fix G5): run on that character itself, no selection
    RemovePermajob = 6,   // Character::removePermajob: task = the job's TaskType, pos.x = its slot on the client
    MovePermajob = 7,     // Character::movePermajob: task = the job's TaskType, pos.x = from slot, pos.y = to slot
    RemoveJob = 8,        // Character::removeJob(task): every job of that kind
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
    Vec3 subjectPos;       // where the subject is (objects of towns have other handles on every machine:
                           // the host finds the same kind of object there, see itemSid)
    std::string buildingSid;   // the destination building's kind and place, found the same way
    Vec3 buildingPos;
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
// Standing orders a character keeps (the squad bar's toggles): bit per order.
enum ModeBits : uint16_t {
    kModeStealth = 1 << 0, kModeDefensive = 1 << 1, kModeRanged = 1 << 2, kModeTaunt = 1 << 3, kModeHold = 1 << 4,
    kModePassive = 1 << 5, kModeChase = 1 << 6,
};
struct CharProgress {
    uint32_t netId = 0;
    std::vector<float> stats;   // kStatCount values
    uint16_t modes = 0;         // ModeBits
    uint8_t style = 0;          // fight style: 0 attack, 1 defend, 2 evade (AGG/DEF/EVADE orders)
    std::string tool;           // the tool its current job puts in its hands (a pickaxe...), empty: none
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
struct EditState {
    bool editing = false;
};
constexpr size_t kMaxLogLines = 64, kMaxLogLine = 400;
struct ClientLog {
    std::vector<std::string> lines;
};
struct ClientReport {
    uint16_t entities = 0;       // host characters known here
    uint16_t missingNpcs = 0;    // ... not in the local world yet
    uint16_t missingSquad = 0;
    uint16_t farOff = 0;         // characters more than 5 units off the host's position (standing ones)
    float maxErr = 0;            // largest correction applied since the last report (units)
    uint16_t fps = 0;
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
    uint32_t traderNetId = 0;      // a purchase or sale in that merchant's trade window (0: none)
    int32_t price = 0;             // cats the player's game took for it (negative: paid to the player)
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
void Encode(Writer& w, const StallMsg& m);
void Encode(Writer& w, const FloorsMsg& m);
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
void Encode(Writer& w, const EditState& m);
const char* TaskLabel(int task);   // a player order's name, for logs ("?" when unknown)
const char* StandingOrderLabel(int order);   // a squad bar toggle's name, for logs
// Containers (chests, shelves, safes...) are furniture: other handles on every machine, so they
// are named by kind and place. The host gives an open one a netId; its items then travel like a
// character's (Inventory messages, InvOp moves).
struct ContainerOpen {
    uint32_t looterNetId = 0;
    std::string sid;
    Vec3 pos;
};
struct ContainerOpened {
    uint32_t netId = 0;
    uint32_t looterNetId = 0;
    std::string sid;
    Vec3 pos;
};
struct ContainerClose {
    uint32_t netId = 0;
    std::string reason;   // host: why it closes (empty: none)
};
// Trading with a merchant. The host's game asks for the trade window (a merchant's "let's trade"
// in a conversation); it opens on the player's own screen instead, with the shop's counters (the
// containers its trade window sells from) as the host has them. Every purchase or sale is then an
// InvOp with its price. Sent again (no counters, same merchant) when the merchant's cats change.
struct TradeCounter {
    uint32_t netId = 0;
    std::string sid;
    Vec3 pos;
};
struct TradeOpen {
    uint32_t traderNetId = 0;
    uint32_t looterNetId = 0;      // the player's character trading
    int32_t traderMoney = 0;       // the merchant's cats on the host
    std::vector<TradeCounter> counters;
    std::string note;              // no counters: why the trade cannot open (shown to the player)
};
constexpr uint32_t kMaxTradeCounters = 32;

// ---- lot A: doors and locks
// A door (DoorStuff) or a piece of furniture with a lock (chest, cage...). Objects of the world have
// another handle on every machine: they are known by kind and place (as containers are).
enum class DoorKind : uint8_t { Door = 1, Lock = 2 };
enum DoorFlags : uint8_t {
    kDoorWantsLock = 1,   // locks itself once closed
    kDoorBroken = 2,      // bashed open
    kDoorHasLock = 4,
    kDoorLocked = 8,
    kDoorLockBroken = 16,
};
struct DoorState {
    std::string sid;
    Vec3 pos;
    DoorKind kind = DoorKind::Door;
    uint8_t state = 0;      // DoorState: 0 closed, 1 open, 2 opening, 3 closing (doors)
    uint8_t flags = 0;      // DoorFlags
    int32_t lockLevel = 0;
    float openAmount = 0;   // 0 closed .. 1 open (doors)
    bool sameState(const DoorState& o) const {   // what matters (the opening amount moves on its own)
        return kind == o.kind && state == o.state && flags == o.flags && lockLevel == o.lockLevel;
    }
};
struct DoorsMsg {
    bool full = false;      // every door near the players (others the client knows are unchanged)
    std::vector<DoorState> doors;
};
constexpr uint32_t kMaxDoorsPerMsg = 256;
enum class DoorAction : uint8_t { OpenButton = 1, LockButton = 2 };
struct DoorRequest {
    std::string sid;
    Vec3 pos;
    DoorAction action = DoorAction::OpenButton;
};
void Encode(Writer& w, const DoorsMsg& m);
bool Decode(Reader& r, DoorsMsg& m);
void Encode(Writer& w, const DoorRequest& m);
bool Decode(Reader& r, DoorRequest& m);
// ---- end lot A
// ---- lot E: buildings. Buildings and furniture get other handles on every machine: they are
// named by kind (template) and place, plus a netId the host gives to the ones it follows.
// A placement carries exactly what the game's factory (RootObjectFactory::createBuilding) gets in
// build mode, so every machine builds the same thing at the same place.
struct BuildPlace {
    uint32_t netId = 0;          // 0: a client's request; else the host's building, built by everyone
    std::string sid;             // building template
    Vec3 pos;                    // as the factory takes it (relative to the parent building / terrain)
    Quat rot;
    int32_t floor = 0;
    uint8_t flags = 0;           // kBuildOutside | kBuildFloorLayout
    std::string parentSid;       // the building it is furniture of (empty: none)
    Vec3 parentPos;
    std::string indoorsSid;      // the building it stands inside (empty: none)
    Vec3 indoorsPos;
    std::string snapSid;         // the building it snaps to (walls, gates; empty: none)
    Vec3 snapPos;
    Handle town;                 // the town it belongs to (invalid: none)
    Vec3 worldPos;               // host: where the new building stands (to find it by kind and place)
};
constexpr uint8_t kBuildOutside = 1, kBuildFloorLayout = 2;
struct BuildStateEntry {
    uint32_t netId = 0;
    std::string sid;
    Vec3 pos;                    // world position (kind-and-place lookup)
    float progress = 0;          // construction progress (the game's units)
    uint8_t flags = 0;           // kSiteComplete | kSitePaused | kSiteDismantling
};
constexpr uint8_t kSiteComplete = 1, kSitePaused = 2, kSiteDismantling = 4;
struct BuildStateMsg {
    std::vector<BuildStateEntry> entries;
};
constexpr uint32_t kMaxBuildStates = 256;
// fix G5: a character's job list, by kind (TaskType), in the panel's order.
struct JobListEntry {
    uint32_t netId = 0;
    std::vector<int32_t> jobs;
};
struct JobListMsg {
    std::vector<JobListEntry> entries;
};
constexpr uint32_t kMaxJobLists = 64, kMaxJobsPerCharacter = 64;
void Encode(Writer& w, const JobListMsg& m);
bool Decode(Reader& r, JobListMsg& m);
struct BuildRemove {
    uint32_t netId = 0;
    std::string sid;
    Vec3 pos;
};
enum class BuildActionKind : uint8_t { Buy = 1, Dismantle = 2 };
struct BuildAction {
    BuildActionKind kind = BuildActionKind::Buy;
    int32_t arg = 0;             // the confirmation dialog's answer
    std::string sid;
    Vec3 pos;
};
void Encode(Writer& w, const BuildPlace& m);
bool Decode(Reader& r, BuildPlace& m);
void Encode(Writer& w, const BuildStateMsg& m);
bool Decode(Reader& r, BuildStateMsg& m);
void Encode(Writer& w, const BuildRemove& m);
bool Decode(Reader& r, BuildRemove& m);
void Encode(Writer& w, const BuildAction& m);
bool Decode(Reader& r, BuildAction& m);

void Encode(Writer& w, const TradeOpen& m);
bool Decode(Reader& r, TradeOpen& m);

// ---- lot D: prisons
// What holds a character captive, as the host's game has it: a cage or prison furniture (found on
// clients by kind and place), shackles, slavery (owner, faction), escape / kidnapping flags and the
// prison sentence. A character set free is sent once with everything cleared.
struct CaptiveState {
    uint32_t netId = 0;
    bool caged = false;            // Character::inSomething == IN_PRISON
    std::string cageSid;           // the cage's kind and place
    Vec3 cagePos;
    bool chained = false;          // Character::isChained (shackles on)
    Handle slaveOwner;             // Character::slaveOwner (the host's handle; invalid: none)
    uint8_t slaveState = 0;        // SlaveStateEnum: 0 not, 1 slave, 2 escaping, 3 ex-slave
    std::string slaveOf;           // the faction it is a slave of (game data id, may be empty)
    bool escaped = false;          // an escaped prisoner (wanted)
    bool kidnapped = false;
    uint64_t sentenceBegan = 0;    // BountyManager prison sentence: when it began (raw TimeOfDay)
    float sentence = 0;            // hours to serve
    bool free() const {
        return !caged && !chained && slaveState == 0 && slaveOf.empty() && !escaped && !kidnapped && sentence <= 0.0f;
    }
    bool operator==(const CaptiveState& o) const {
        return netId == o.netId && caged == o.caged && cageSid == o.cageSid && cagePos.x == o.cagePos.x && cagePos.y == o.cagePos.y &&
               cagePos.z == o.cagePos.z && chained == o.chained && slaveOwner == o.slaveOwner && slaveState == o.slaveState &&
               slaveOf == o.slaveOf && escaped == o.escaped && kidnapped == o.kidnapped && sentenceBegan == o.sentenceBegan &&
               sentence == o.sentence;
    }
    bool operator!=(const CaptiveState& o) const { return !(*this == o); }
};
struct CaptivesMsg {
    std::vector<CaptiveState> chars;
};
constexpr uint32_t kMaxCaptivesPerMsg = 256;
void Encode(Writer& w, const CaptivesMsg& m);
bool Decode(Reader& r, CaptivesMsg& m);
void Encode(Writer& w, const ContainerOpen& m);
bool Decode(Reader& r, ContainerOpen& m);
void Encode(Writer& w, const ContainerOpened& m);
bool Decode(Reader& r, ContainerOpened& m);
void Encode(Writer& w, const ContainerClose& m);
bool Decode(Reader& r, ContainerClose& m);
void EncodeResync(Writer& w);
void Encode(Writer& w, const ClientLog& m);
bool Decode(Reader& r, ClientLog& m);
void Encode(Writer& w, const ClientReport& m);
bool Decode(Reader& r, ClientReport& m);
bool Decode(Reader& r, EditState& m);
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
bool Decode(Reader& r, StallMsg& m);
bool Decode(Reader& r, FloorsMsg& m);
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

// ---- lot C: ranged combat (bows, crossbows, harpoons, turrets) ----
// A shot as the host's game fired it. Clients fire the same projectile from the same weapon along the
// same path; on their side it is only seen (damage and its effects are the host's, through vitals).
struct ShotEvent {
    uint32_t shooterNetId = 0;   // who fired (a turret's shot: its operator)
    uint32_t targetNetId = 0;    // what it was fired at (0: nothing the client knows)
    uint8_t stat = 0;            // the skill the shot used (StatsEnumerated)
    Vec3 aimPos;                 // where the shooter aimed
    Quat dir;                    // the projectile's orientation as it left the weapon: its flight path
    std::string turretSid;       // a turret's shot: that turret (kind and place); empty: a personal weapon
    Vec3 turretPos;
};
struct ShotsMsg {
    std::vector<ShotEvent> shots;
};
constexpr uint32_t kMaxShotsPerMsg = 64;
// Where characters in ranged combat aim, and turrets near the players (sent when it changes).
struct RangedAim {
    uint32_t netId = 0;
    uint8_t state = 0;           // RangedCombatClass::RangedState (0 shooting, 1 moving, 2 reloading, 3 waiting, 4 thinking)
    Vec3 aimPos;
    uint32_t targetNetId = 0;
};
struct TurretAim {
    std::string sid;             // the turret, by kind and place (handles differ between machines)
    Vec3 pos;
    Vec3 target;                 // the point it turns toward
};
struct RangedMsg {
    std::vector<RangedAim> aims;
    std::vector<TurretAim> turrets;
    std::vector<uint32_t> stopped;   // characters out of ranged combat since the last message
};
constexpr uint32_t kMaxRangedAims = 256;
constexpr uint32_t kMaxTurretAims = 64;
void Encode(Writer& w, const ShotsMsg& m);
bool Decode(Reader& r, ShotsMsg& m);
void Encode(Writer& w, const RangedMsg& m);
bool Decode(Reader& r, RangedMsg& m);

// FNV-1a 64, used for mod list / world fingerprints.
uint64_t Fnv1a64(const void* data, size_t n, uint64_t seed = 0xcbf29ce484222325ull);

// ---- lot B: factions
// Relations and bounties are the host's: clients never change them (their hooks refuse it) and
// impose the host's values, so faction screens, "wanted" states and NPC attitudes shown match.
struct RelationState {   // what one faction feels about another (FactionRelations::RelationData)
    bool alliance = false, peace = false, war = false, coexists = false;
    float relation = 0, trustPositives = 0, trustNegatives = 0, strength = 0;
    bool operator==(const RelationState&) const = default;
};
struct FactionRelationEntry {
    std::string factionSid;
    bool hasOurs = false, hasTheirs = false;
    RelationState ours;     // the player faction toward it
    RelationState theirs;   // it toward the player faction
    bool operator==(const FactionRelationEntry&) const = default;
};
struct FactionsMsg {
    int32_t playerRank = 0;
    float reputationTrust = 0, reputationBadassery = 0;
    std::vector<FactionRelationEntry> factions;
    bool operator==(const FactionsMsg&) const = default;
};
// Relations are floats the game nudges on its own every frame (trust and strength drift, decay toward
// neutral): differences below these are noise, neither sent again nor imposed on a client.
inline bool CloseEnough(float a, float b, float tol) { return (a > b ? a - b : b - a) < tol; }
inline bool SameRelation(const RelationState& a, const RelationState& b) {
    return a.alliance == b.alliance && a.peace == b.peace && a.war == b.war && a.coexists == b.coexists && CloseEnough(a.relation, b.relation, 0.5f) &&
           CloseEnough(a.trustPositives, b.trustPositives, 0.5f) && CloseEnough(a.trustNegatives, b.trustNegatives, 0.5f) &&
           CloseEnough(a.strength, b.strength, 1.0f + 0.01f * (a.strength > 0 ? a.strength : -a.strength));
}
inline bool SameFactions(const FactionsMsg& a, const FactionsMsg& b) {
    if (a.playerRank != b.playerRank || !CloseEnough(a.reputationTrust, b.reputationTrust, 0.5f) ||
        !CloseEnough(a.reputationBadassery, b.reputationBadassery, 0.5f) || a.factions.size() != b.factions.size())
        return false;
    for (size_t i = 0; i < a.factions.size(); ++i) {
        const auto &x = a.factions[i], &y = b.factions[i];
        if (x.factionSid != y.factionSid || x.hasOurs != y.hasOurs || x.hasTheirs != y.hasTheirs) return false;
        if ((x.hasOurs && !SameRelation(x.ours, y.ours)) || (x.hasTheirs && !SameRelation(x.theirs, y.theirs))) return false;
    }
    return true;
}
struct BountyEntry {
    std::string factionSid;     // the faction that wants the character
    int32_t amount = 0;
    uint32_t crimes = 0;        // bit per CrimeEnum
    bool claimed = false;
    uint64_t since = 0;         // TimeOfDay the bounty was set (raw)
    bool operator==(const BountyEntry&) const = default;
};
struct CharBounties {
    uint32_t netId = 0;
    std::vector<BountyEntry> bounties;
    int32_t crime = 0;              // the crime being committed now (CrimeEnum, 0: none)
    std::string crimeFactionSid;    // against whom
    float crimeExpiry = 0;
    float prisonSentence = 0;       // hours of prison still to serve
    uint64_t prisonBegan = 0;       // TimeOfDay (raw)
    std::string accessPassSid;      // faction that gave an access pass ("" none)
    uint64_t accessPassUntil = 0;   // TimeOfDay (raw)
    bool operator==(const CharBounties&) const = default;
};
struct BountiesMsg {
    std::vector<CharBounties> chars;
    bool operator==(const BountiesMsg&) const = default;
};
constexpr uint32_t kMaxFactions = 2048, kMaxBountiesPerChar = 128, kMaxBountyChars = 256;
void Encode(Writer& w, const FactionsMsg& m);
bool Decode(Reader& r, FactionsMsg& m);
void Encode(Writer& w, const BountiesMsg& m);
bool Decode(Reader& r, BountiesMsg& m);

// ---- diplomacy: what the world thinks beyond the player faction, the host's everywhere.
// - relations between two factions that are not the player's (wars, alliances: the game changes them
//   after a leader's death, a dialogue, a campaign). The host sends only the pairs that changed since
//   it started hosting: every client loaded the host's save, which holds the rest;
// - unique characters (faction leaders, named NPCs): dead, alive, imprisoned, and whether the player
//   did it (UniqueNPCManager). World states (and the town overrides, dialogues and campaigns that
//   test them) are computed from these and from the player faction's relations;
// - towns: owner faction and the override the world states put on them (taken over, destroyed...).
struct FactionPairRelation {
    std::string from, to;   // what `from` feels about `to` (faction game data ids)
    RelationState rel;
    bool operator==(const FactionPairRelation&) const = default;
};
enum : uint8_t { kUniqueDead = 0, kUniqueAlive = 1, kUniqueImprisoned = 2 };   // UniqueNPCManager states
struct UniqueState {
    std::string sid;        // the character's game data id
    uint8_t state = kUniqueAlive;
    bool byPlayer = false;  // the player killed / imprisoned it (or it is the player's)
    bool operator==(const UniqueState&) const = default;
};
struct TownState {
    std::string sid;            // the town's game data id
    std::string ownerSid;       // its faction ("" none)
    std::string overrideSid;    // the override applied ("" none: the town as the game data makes it)
    bool operator==(const TownState&) const = default;
};
struct DiplomacyState {         // everything, as one side's game has it
    std::vector<FactionPairRelation> pairs;
    std::vector<UniqueState> uniques;
    std::vector<TownState> towns;
    bool operator==(const DiplomacyState&) const = default;
};
enum class DiploPart : uint8_t { Pairs = 1, Uniques = 2, Towns = 3 };
struct DiplomacyMsg {           // one part at a time (each fits in a packet)
    DiploPart part = DiploPart::Pairs;
    std::vector<FactionPairRelation> pairs;
    std::vector<UniqueState> uniques;
    std::vector<TownState> towns;
    bool operator==(const DiplomacyMsg&) const = default;
};
constexpr uint32_t kMaxDiploPairs = 4096, kMaxUniques = 8192, kMaxTowns = 2048;
constexpr size_t kDiplomacyBudget = 60 * 1024;   // bytes per Diplomacy message (kMaxPacketSize leaves room)
// Two factions' relation as the game uses it between NPC factions: the flags, and the value to the point.
inline bool SamePairRelation(const RelationState& a, const RelationState& b) {
    return a.alliance == b.alliance && a.peace == b.peace && a.war == b.war && a.coexists == b.coexists && CloseEnough(a.relation, b.relation, 1.0f);
}
// What a faction thinks of another, as the game decides it (FactionRelations::isAlly 0x6B2630:
// alliance or relation >= 50; isEnemy 0x6B26D0: relation <= -30).
enum class Standing : uint8_t { Neutral = 0, Ally = 1, Enemy = 2 };
inline Standing StandingOf(const RelationState& r) {
    if (r.alliance || r.relation >= 50.0f) return Standing::Ally;
    if (r.relation <= -30.0f) return Standing::Enemy;
    return Standing::Neutral;
}
void Encode(Writer& w, const DiplomacyMsg& m);
bool Decode(Reader& r, DiplomacyMsg& m);

} // namespace kc
