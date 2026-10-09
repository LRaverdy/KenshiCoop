// Kenshi 1.0.68 (Steam, x64) memory layout and functions used by KenshiCoop.
//
// Every address here was located by static analysis of kenshi_x64.exe (SHA-256 below):
// RTTI vtables, string cross-references, and KenshiLib's 1.0.65 layout translated through 693
// vtable anchors. Each hooked/called function also carries its first prologue bytes, which are
// re-checked at startup; on any mismatch the plugin stays disabled instead of risking a crash.
#pragma once
#include <cstdint>
#include <vector>

#include "kc/protocol.h"

namespace kenshi {

// SHA-256 of the supported kenshi_x64.exe ("Kenshi 1.0.68 - x64 (Newland)", Steam).
inline constexpr char kSupportedExeSha256[] = "A596AB4E407C67B58599C54FFB32DC1BF2B64510CDEBD3FA9359EF05A576AEB1";

// ---------------------------------------------------------------- RVAs
namespace rva {
inline constexpr uintptr_t GameWorldInstance = 0x2134110;   // global `GameWorld ou` object
inline constexpr uintptr_t HandleTable = 0x2133f90;         // first arg of the hand resolver
inline constexpr uintptr_t GameClockOwner = 0x21303d0;      // pointer; +0xA0 = in-game hours (double)
inline constexpr uintptr_t SaveUsesUserPath = 0x2133573;    // bool: SaveManager uses userSavePath, else localSavePath
inline constexpr uintptr_t TradeGui = 0x21337B0;            // the GUI object showTradeWindow() is called on

inline constexpr uintptr_t VtCharacter = 0x16f9eb8;
inline constexpr uintptr_t VtCharacterHuman = 0x16f2848;
inline constexpr uintptr_t VtCharacterAnimal = 0x16f2208;
inline constexpr uintptr_t VtGameWorld = 0x1722608;
inline constexpr uintptr_t VtPlayerInterface = 0x171a2b8;
inline constexpr uintptr_t VtCharMovement = 0x16fcc88;
inline constexpr uintptr_t VtAI = 0x16fa3e8;
inline constexpr uintptr_t VtCombatClass = 0x16f6698;
inline constexpr uintptr_t VtCombatClassAI = 0x16f67b8;
inline constexpr uintptr_t VtHand = 0x16852d0;
} // namespace rva

struct FunctionSig {
    const char* name;
    uintptr_t rva;
    uint8_t prologue[12];
};

// Functions we hook or call. Index into this table with the Fn enum.
enum Fn : int {
    FnMainLoop,                 // void GameWorld::mainLoop_GPUSensitiveStuff(float)
    FnPlayerMove,               // void PlayerInterface::playerMove(const Vector3&, Building*)
    FnAddOrderSelected,         // void PlayerInterface::addOrderSelectedCharacters(Building*, TaskType, RootObject*, bool, bool, const Vector3&)
    FnNewPlayerTaskSelected,    // void PlayerInterface::newPlayerTaskSelectedCharacters(TaskType, const hand&, Building*, const Vector3&, bool)
    FnSetOrderSelected,         // void PlayerInterface::setOrderSelectedCharacters(StandingOrder)
    FnStopCharactersMovement,   // void PlayerInterface::stopCharactersMovement()
    FnPlayerMoveOrderDefault,   // void Character::playerMoveOrderDefault(Building*, RootObject*, const Vector3&)
    FnAIUpdate4Frame,           // void AI::update4Frame(float)
    FnAIPeriodicUpdate,         // void AI::periodicUpdate(float)
    FnSetFrameSpeedMultiplier,  // void GameWorld::setFrameSpeedMultiplier(float)
    FnUserPause,                // void GameWorld::userPause(bool)
    FnHandleResolve,            // RootObject* resolve(HandleTable*, const hand*, bool)
    FnTogglePause,              // void GameWorld::togglePause(bool)
    FnMedApplyDamage,           // void MedicalSystem::applyDamage(HealthPartStatus*, const Damages&, bool loadingSavestate, bool canSever, const Vector3& force)
    FnMedKnockout,              // void MedicalSystem::knockout(float skill01)
    FnDeclareDead,              // void Character::declareDead()
    FnSaveManagerGet,           // static SaveManager* SaveManager::getSingleton()
    FnSaveManagerSave,          // void SaveManager::save(const std::string& name, bool autosave)   (deferred)
    FnSaveManagerLoad,          // void SaveManager::load(const std::string& name)                  (deferred)
    FnCreateRandomCharacter,    // RootObject* RootObjectFactory::createRandomCharacter(Faction*, Vector3, RootObjectContainer*, GameData*, Building*, float age)
    FnWorldDestroy,             // bool GameWorld::destroy(RootObject*, bool justUnloaded, const char* debugInfo)
    FnEndCombatMode,            // void Character::endCombatMode()
    FnRagdollMode,              // void Character::ragdollMode(bool on, RagdollPart::Enum part)
    FnRegionUpdateBT,           // void WeatherRegion::updateBT()          (background thread: advances weather)
    FnSeasonGetNewWeather,      // void Season::getNewWeather()            (random pick of the next weather)
    FnInstanceSetupWeather,     // void WeatherInstance::setupWeather(Weather*)
    FnCreateItem,               // Item* RootObjectFactory::createItem(GameData*, const hand&, GameData* company, GameData* material, int level, Faction*)
    FnShowTradeWindow,          // void ForgottenGUI::showTradeWindow(const hand& a, const hand& b, TradeWindowType)   (deferred)
    FnIsRagdoll,                // bool Character::isRagdoll() const   (lying in ragdoll, or carried)
    FnRecruit,                  // bool PlayerInterface::recruit(Character*, bool editor)
    FnAddTaskNearest,           // void PlayerInterface::addTaskNearestSelectedCharacter(Building*, TaskType, RootObject*, bool shift, const Vector3&, bool noAnimals)
    FnAddJobSelected,           // void PlayerInterface::addJobSelectedCharacters(TaskType, RootObject*, bool shift, bool add, const Vector3&)
    FnCount
};
extern const FunctionSig kFunctions[FnCount];

// ---------------------------------------------------------------- field offsets
namespace off {
// GameWorld
inline constexpr uintptr_t GW_frameSpeedMult = 0x700;  // float
inline constexpr uintptr_t GW_player = 0x580;          // PlayerInterface*
inline constexpr uintptr_t GW_paused = 0x8B9;          // bool
inline constexpr uintptr_t GW_charUpdateList = 0x750;  // boost::unordered_set<Character*>: every active character
inline constexpr uintptr_t GW_deathParade = 0x708;     // boost::unordered_map<hand, Character*>: dead bodies
inline constexpr uintptr_t HandMapNode_mapped = 0x30;  // boost unordered_map<hand, T*> node: mapped pointer
inline constexpr uintptr_t GW_gamedataBySid = 0xF0;    // GameDataManager(+0x20)::gamedataSID: boost::unordered_map<std::string, GameData*>
inline constexpr uintptr_t GW_factory = 0x4A0;         // RootObjectFactory*
inline constexpr uintptr_t GW_factionMgr = 0x4A8;      // FactionManager*: +0 lektor<Faction*> participants
inline constexpr uintptr_t MapNode_key = 0x10;         // boost unordered_map<std::string, T*> node: key string
inline constexpr uintptr_t MapNode_mapped = 0x38;      //                                          mapped pointer
inline constexpr uintptr_t GD_name = 0x28;             // GameData: std::string
inline constexpr uintptr_t GD_type = 0x50;             // GameData: itemType
inline constexpr uintptr_t GD_stringID = 0x58;         // GameData: std::string
inline constexpr uintptr_t FAC_data = 0x240;           // Faction: GameData*
inline constexpr uintptr_t Clock_hours = 0xA0;         // double, in the object at rva::GameClockOwner

// PlayerInterface
inline constexpr uintptr_t PI_selectedCharacters = 0x208;  // boost::unordered_set<hand>
inline constexpr uintptr_t PI_playerCharacters = 0x2B0;    // lektor<Character*>

// boost::unordered_set (relative to the set)
inline constexpr uintptr_t US_bucketCount = 0x18;
inline constexpr uintptr_t US_size = 0x20;
inline constexpr uintptr_t US_buckets = 0x38;
inline constexpr uintptr_t USNode_next = 0x0;
inline constexpr uintptr_t USNode_value = 0x10;

// lektor<T> (Kenshi's vector)
inline constexpr uintptr_t LK_count = 0x8;   // uint32
inline constexpr uintptr_t LK_data = 0x10;   // T*

// hand (0x20 bytes)
inline constexpr uintptr_t H_type = 0x8;
inline constexpr uintptr_t H_container = 0xC;
inline constexpr uintptr_t H_containerSerial = 0x10;
inline constexpr uintptr_t H_index = 0x14;
inline constexpr uintptr_t H_serial = 0x18;
inline constexpr size_t HandSize = 0x20;

// RootObjectBase / RootObject / Character
inline constexpr uintptr_t RO_owner = 0x10;       // Faction*
inline constexpr uintptr_t RO_name = 0x18;        // std::string (display name)
inline constexpr uintptr_t RO_data = 0x40;        // GameData* (template)
inline constexpr uintptr_t RO_handle = 0x58;      // hand
inline constexpr uintptr_t RO_rot = 0xB0;         // Ogre::Quaternion (w,x,y,z)
inline constexpr uintptr_t CH_movement = 0x640;   // CharMovement*
inline constexpr uintptr_t CH_ai = 0x650;         // AI*
inline constexpr uintptr_t CH_medical = 0x458;    // MedicalSystem (inline)
inline constexpr uintptr_t CH_body = 0x648;       // CharBody*: +0x8 CombatClass*
inline constexpr uintptr_t BODY_combat = 0x8;
// CombatClass
inline constexpr uintptr_t CC_active = 0x130;       // bool combatModeActive
inline constexpr uintptr_t CC_target = 0x298;       // hand: attack target

// MedicalSystem
inline constexpr uintptr_t MS_blood = 0x70;          // float
inline constexpr uintptr_t MS_koTimer = 0xA0;        // float
inline constexpr uintptr_t MS_me = 0xE0;             // Character*
inline constexpr uintptr_t MS_unconscious = 0x161;  // bool
inline constexpr uintptr_t MS_dead = 0x164;          // bool
inline constexpr uintptr_t MS_anatomy = 0x190;       // lektor<HealthPartStatus*>
// SaveManager
inline constexpr uintptr_t SM_localSavePath = 0x50;  // std::string (VS2010 layout)
inline constexpr uintptr_t SM_userSavePath = 0x78;   // std::string
inline constexpr uintptr_t SM_signal = 0xA0;         // int: pending save/load operation, 0 = idle
inline constexpr uintptr_t SM_location = 0xD8;       // std::string: folder of the last save/load request
// HealthPartStatus
inline constexpr uintptr_t HP_flesh = 0x40;
inline constexpr uintptr_t HP_stun = 0x44;
inline constexpr uintptr_t HP_bandage = 0x48;

// CharMovement
inline constexpr uintptr_t CM_currentlyMoving = 0x24;  // bool
inline constexpr uintptr_t CM_currentSpeed = 0xB8;     // float
inline constexpr uintptr_t CM_destination = 0xDC;      // Ogre::Vector3

// AI
inline constexpr uintptr_t AI_me = 0x2F8;  // Character*
} // namespace off

// vtable slots (byte offsets)
namespace slot {
inline constexpr uintptr_t RO_getPosition = 0x40;                 // Vector3 getPosition()  (ret via hidden ptr)
inline constexpr uintptr_t RO_isUnconcious = 0x30;                // bool isUnconcious() const
inline constexpr uintptr_t RO_setName = 0x10;                     // void setName(const std::string&)
inline constexpr uintptr_t CH_getAge = 0x390;                     // float getAge() const
inline constexpr uintptr_t CM_setDestination = 0x90;              // void setDestination(const Vector3&, UpdatePriority, bool)
inline constexpr uintptr_t CM_halt = 0x98;                        // void halt()
inline constexpr uintptr_t CM_setPositionDirectionAndTeleport = 0xC0;  // (const Vector3&, const Quaternion&) - halts first
inline constexpr uintptr_t CH_setProneState = 0x370;   // void setProneState(ProneState): 0 = normal (standing)
inline constexpr uintptr_t CC_initCombatMode = 0x10;   // bool initCombatMode(const hand& subject, int end, bool focused): end 0 = engage
inline constexpr uintptr_t CM_setPositionSimple = 0xC8;  // (const Vector3&): moves body + physics capsule, keeps walking
} // namespace slot

inline constexpr uint32_t kItemTypeCharacter = 1;
inline constexpr uint32_t kItemTypeAnimalCharacter = 0x5B;
enum UpdatePriority : int { LOW_PRIORITY = 0, MED_PRIORITY = 1, HIGH_PRIORITY = 2 };

// ---------------------------------------------------------------- runtime access
// All accessors are exception-safe (SEH) and validate object types by vtable before use.
// They must only be called from the game thread (inside the main loop hook).

bool Init(std::string* err);   // resolves module base, verifies prologues
uintptr_t Base();
uintptr_t Addr(uintptr_t rva);
void* FnAddr(Fn f);

struct Character;  // opaque game objects
struct PlayerInterface;
struct GameWorld;
struct AI;

GameWorld* World();
PlayerInterface* Player();             // null when no game is loaded
bool IsCharacter(const void* obj);

void PlayerCharacters(std::vector<Character*>& out);
void ActiveCharacters(std::vector<Character*>& out);
void DeadBodies(std::vector<Character*>& out);   // corpses: they leave the active list when they die   // every character the game is updating
Character* Resolve(const kc::Handle& h);               // game handle -> live character (or null)
bool HandleFromHand(const void* hand, kc::Handle& out); // reads a game `hand` object
// Opens the game's loot window between two characters (what reaching a body with a loot order does).
bool OpenLootWindow(Character* looter, Character* target);
// tests: the order a right-click on `subject` gives to the nearest selected character
bool CallAddTaskNearest(int task, Character* subject);
// A new member of the player's squad, of the same kind as `model`, named `name` (15 chars max).
Character* CreateRecruit(Character* model, const std::string& name, const kc::Vec3& pos, std::string* err);
bool CharacterName(const Character* c, std::string& out);
std::string ClassRvas(const Character* c);   // tests: "<character vtable>/<movement vtable>" as RVAs
void SelectedHandles(std::vector<kc::Handle>& out);
bool GetHandle(const Character* c, kc::Handle& out);
bool GetPosition(Character* c, kc::Vec3& out);
bool GetRotation(const Character* c, kc::Quat& out);
bool GetMovement(const Character* c, kc::Vec3& dest, bool& moving, float& speed);
bool IsDown(Character* c);
Character* AICharacter(const AI* ai);
Character* MedicalCharacter(const void* medical);
bool ReadVitals(Character* c, kc::EntityVitals& out);
bool IsDead(Character* c);
bool IsUnconscious(Character* c);
bool IsRagdoll(Character* c);   // the body is physically on the ground (or carried)

bool GetGameHours(double& out);
// Save management (deferred operations, executed by the game a frame later).
bool SaveManagerBusy();                                     // a save/load is pending or running
bool RequestSave(const std::string& name, std::string* folderOut);   // folder = where it will be written
bool RequestLoad(const std::string& name);
bool SaveFolder(std::string& out);                          // where this machine's saves live

// Character creation / removal (game thread, live world only).
bool ReadSpawnSource(Character* c, kc::SpawnInfo& out);    // template + faction string ids, name, age
void ResetLookupCaches();                                  // call when a new world loads
Character* CreateCharacter(const kc::SpawnInfo& info, const kc::Vec3& pos, std::string* err);
bool DestroyObject(void* obj);

// Inventories (read side).
bool ReadInventory(Character* c, std::vector<kc::ItemState>& out);   // canonical order
// Replace the whole content with these items, laid out like a save load does.
bool RebuildInventory(Character* c, const std::vector<kc::ItemState>& items, std::string* err);
// Host: replay a client's item movement (from -> to, or a drop to the ground).
bool MoveInventoryItem(Character* from, Character* to, const kc::InvOp& op, std::string* err);

// Melee combat.
bool ReadCombat(Character* c, kc::Handle& target);          // true when in combat mode with a target
bool StartCombat(Character* c, const kc::Handle& target);   // engage (local handle of the target)
bool EndCombat(Character* c);

// Getting knocked down / getting up (the AI normally drives the getting up).
bool SetRagdoll(Character* c, bool on);

// Weather (WeatherRegion objects are reached through the hooked WeatherRegion::updateBT).
bool ReadRegionWeather(void* region, kc::RegionWeather& out);
bool WriteRegionWeather(void* region, const kc::RegionWeather& w);   // returns false if ids are unknown here
bool ExpireRegionWeather(void* region);   // the game rolls a new weather on its next update (tests)
bool StandUp(Character* c);   // clears the unconscious flag, leaves ragdoll, normal posture
bool SetGameHours(double hours);

float GetFrameSpeed();
bool GetPaused();

// Game calls (game thread only). Return false if the call faulted or the object was invalid.
bool Teleport(Character* c, const kc::Vec3& pos, const kc::Quat& rot);
bool SetDestination(Character* c, const kc::Vec3& dest);
bool SetPositionSimple(Character* c, const kc::Vec3& pos);
bool IsMoving(const Character* c);
bool Halt(Character* c);
bool CallSetFrameSpeed(float speed);
bool CallUserPause(bool paused);
bool CallTogglePause(bool paused);
// Writes host health into the local copy (does not trigger death/knockout by itself).
bool WriteVitals(Character* c, const kc::EntityVitals& v);
// The irreversible transitions; callers must hold a HostCallScope on clients.
bool CallDeclareDead(Character* c);
bool CallKnockout(Character* c);

} // namespace kenshi
