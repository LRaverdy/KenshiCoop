// Kenshi 1.0.68 (Steam, x64) memory layout and functions used by KenshiCoop.
//
// Every address here was located by static analysis of kenshi_x64.exe (SHA-256 below):
// RTTI vtables, string cross-references, and KenshiLib's 1.0.65 layout translated through 693
// vtable anchors. Each hooked/called function also carries its first prologue bytes, which are
// re-checked at startup; on any mismatch the plugin stays disabled instead of risking a crash.
#pragma once
#include <cstdint>
#include <functional>
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
inline constexpr uintptr_t WeatherSystem = 0x2128190;       // pointer; +0 = WeatherRegion* where the camera is
inline constexpr uintptr_t TradePartnersHead = 0x2132BE8;   // std::map<InventoryGUI*, InventoryTradeData> of the open trade windows: head node
inline constexpr uintptr_t TradePartnersSize = 0x2132BF0;   //                                                                   size
inline constexpr uintptr_t MouseInventory = 0x2132B58;      // pointer to the item being dragged (+0x30 Item*)
inline constexpr uintptr_t GameNew = 0xED6504;              // the game's operator new (its CRT's)
inline constexpr uintptr_t GameDelete = 0xED64FE;           // the game's operator delete
inline constexpr uintptr_t VtLektor = 0x168BAF0;            // lektor<RootObject*> (the game's small vector)

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
inline constexpr uintptr_t VtEffectGroupPoint = 0x168be10;        // point effects: lightning, static points
inline constexpr uintptr_t VtEffectGroupWandering = 0x168be58;    // wandering storms and gas clouds
inline constexpr uintptr_t VtEffectHandlerPoint = 0x168c0a8;
inline constexpr uintptr_t VtEffectHandlerWandering = 0x168c128;
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
    FnEffectHandlerCtor,        // EffectHandler::EffectHandler(GameData* effect, AreaBiomeGroup*, const Vector3& pos)
    FnEffectAffectObjects,      // void EffectHandler::affectObjects()   (a weather effect hurts the characters it reaches)
    FnEffectStop,               // void EffectHandler::stop()            (fades out; removed once faded)
    FnRegionUpdateEffects,      // void WeatherRegion::updateWeatherEffects()   (replaces the effect groups after a weather change)
    FnAnimStartCombat,          // void AnimationClass::startCombatAnimation(CombatTechniqueData*, float speed, std::string extra)
    FnAnimRunCombat,            // void AnimationClass::runCombatAnimation(CombatTechniqueData*, float speed, std::string extra)
    FnAnimEndCombat,            // void AnimationClass::endCombatAnimation()
    FnAnimPlayAction,           // void AnimationClass::playAction(AnimationData*, float speedMult, float initialWeight, bool isStumble)
    FnAnimStopAction,           // bool AnimationClass::stopAction()
    FnAnimStopActionNamed,      // bool AnimationClass::stopAction(const std::string& name)
    FnAnimStartStumble,         // void AnimationClass::startStumble(AnimationData*)
    FnAnimEndStumble,           // void AnimationClass::endStumble()
    FnAnimSetCombatMode,        // void AnimationClass::setCombatMode(bool)
    FnAnimSetCarryMode,         // void AnimationClass::setCarryMode(bool carried, bool left, bool right)
    FnDrawWeapon,               // bool CharacterHuman::drawWeapon(Item*, std::string lastSection)
    FnSheatheWeapon,            // void CharacterHuman::sheatheWeapon()
    FnAnimGuardLegs,            // void AnimationClass::setCombatModeLegsIdle(bool)
    FnAnimGuardUpper,           // void AnimationClass::setCombatModeUpperIdle(bool)
    FnSingleAnimUpdate,         // void AnimationClassBase::SingleAnimation::update(float masterTime, float frameTime, bool sounds)
    FnRunAnimationLayer,        // void AnimationClass::runAnimation(AnimationData*, float speed, AnimationLayerEnum, float blend)
    FnSectionCanItemGoHere,     // bool InventorySection::canItemGoHere(Item*, int x, int y)
    FnSectionValidPosition,     // bool InventorySection::getValidInventoryPosition(Item*, int& x, int& y)
    FnSectionFootprintTaken,    // bool InventorySection::existsItemInFootprint(Item*, int x, int y)
    FnPickupItem,               // void PlayerInterface::pickupItem(Item*)   (the player's "pick up" order)
    FnGiveItem,                 // bool Character::giveItem(Item*, bool dropOnFail, bool destroyOnFail)   (pickups go through it)
    FnDropItemHuman,            // void CharacterHuman::dropItem(RootObject*)
    FnCreateScreenLabel,        // ScreenLabel* ForgottenGUI::createScreenLabel(const std::string&, const Colour&, LabelSize, RisingSpeed)
    FnLabelSetTracking,         // void ScreenLabel::setTracking(const hand&, const Vector3& offset)
    FnLabelSetColor,            // void ScreenLabel::setColor(const Colour&)
    FnReassessCollapse,         // void MedicalSystem::reassessCollapseMode(bool medic, bool agony)   (decides a collapse)
    FnAnimationSelection,       // void AnimationClass::animationSelection(float time)   (picks what to play each frame)
    FnTrackAnimationMovement,   // void CharMovement::trackAnimationMovement(bool)   (animations move the character)
    FnCombatMovementUpdate,     // void CharMovement::combatMovementUpdate(float, const Vector3& pos, const Vector3& dir, bool moving, Vector3& repulsion, Vector3& facingOut, bool defensive, swordStateEnum, float raceSpeedMult)
    FnIncreaseStat,             // void increaseStat(float& stat, float amount, float upperLimit)   (every experience gain ends here)
    FnObjectSelected,           // void PlayerInterface::objectSelected(RootObject*, bool select)
    FnUnselectAll,              // void PlayerInterface::unselectAll()
    FnDialogueSay,              // void Dialogue::say(const std::string& text, DialogLineData* line)   (every speech bubble)
    FnDialogueSetInDialog,      // void Dialogue::setInDialog(bool on)   (shows / hides the conversation window)
    FnDialogueSetResponses,     // void Dialogue::setResponesGUI()   (the answers into the window)
    FnDialogueSetReplyText,     // void Dialogue::setConversationReplyGUI()   (what the other one says into the window)
    FnDialogueReplyClicked,     // void Dialogue::replyClicked(int)
    FnDialogueSendEvent,        // bool Dialogue::sendEvent(Character*, EventTriggerEnum)
    FnDialogueSendEventOverride,// bool Dialogue::sendEventOverride(Character*, EventTriggerEnum, bool)
    FnDialogueStartConversation,// bool Dialogue::startConversation(Character*, DialogLineData*, EventTriggerEnum, bool)
    FnDialogueStartPlayerConversation,// bool Dialogue::startPlayerConversation(Character*, DialogLineData*)
    FnDialogueDoActions,        // void Dialogue::_doActions(DialogLineData*)   (recruit, bounties, relations... of a line)
    FnTaskSystemUpdate,         // void AITaskSytem::update(Vector3 position, float time)   (runs the current task)
    FnSensoryDialogAssessment,  // void SensoryData::dialogAssessmentUpdate(float, bool)   (notices crimes, decides to talk)
    FnSensoryAssessCrimes,      // void SensoryData::assessCrimes(Character*)
    FnBlackboardUpdate,         // void Blackboard::update(float)   (squad AI)
    FnBlackboardPeriodic,       // void Blackboard::periodicUpdate(float)
    FnFactionWarPeriodic,       // void FactionWarMgr::periodicUpdate()   (raids, campaigns)
    FnUniqueSquadPeriodic,      // void FactionUniqueSquadManager::periodicUpdate(float)
    FnAffectRelationsAmount,    // void FactionRelations::affectRelations(Faction*, float amount, float mult)
    FnAffectRelationsEvent,     // void FactionRelations::affectRelations(Faction*, FactionEvent, float mult)
    FnSetRelation,              // void FactionRelations::setRelation(Faction*, float)
    FnSetCrime,                 // bool BountyManager::setCrime(CrimeEnum, Faction*, const hand&)
    FnAssignBounty,             // void BountyManager::assignBountyForCrimes(Faction*)
    FnFocusCamera,              // void PlayerInterface::focusCameraSelectedCharacter()
    FnSquadAddCharacterAt,      // void ActivePlatoon::addCharacterAt(RootObject*, int index)   (moves a character into a squad)
    FnCreateSquad,              // ActivePlatoon* PlayerInterface::createSquad()
    FnCloseCharacterEditor,     // void ForgottenGUI::closeCharacterEditor()   (only the editor's confirm button calls it)
    FnShowCharacterEditor,      // void ForgottenGUI::showCharacterEditor(lektor<Character*>, CharacterEditMode, const vector<GameDataReference>* races)
    FnSetAppearanceData,        // void Character::setAppearanceData(GameDataCopyStandalone*)
    FnMapBool, FnMapString, FnMapInt, FnMapFloat, FnMapVec3, FnMapQuat,   // GameData value maps: operator[](const std::string&) -> pair*
    FnStringAssign,             // std::string& std::string::assign(const std::string&, size_t pos, size_t n)
    FnCharAddOrder,             // void Character::addOrder(Building* dest, TaskType, RootObject* subject, bool shift, bool clear, const Vector3&)
    FnCharAddJob,               // void Character::addJob(TaskType, RootObject* subject, bool shift, bool add, const Vector3&)
    FnSetStandingOrder,         // void Character::setStandingOrder(StandingOrder, bool on)
    FnPickupCharacter,          // void Character::pickupObject(Character* who)   (puts a body on the shoulder)
    FnDropCarried,              // void Character::dropCarriedObject(bool ragdollHim, bool removeOnly)
    FnSetCurrentPlatoon,        // bool PlayerInterface::setCurrentPlatoon(Platoon*)   (the squad the squad bar shows)
    FnShowLoadWindow,           // void SaveManager::showLoad()   (the game's "Load" window)
    FnReThinkAIAction,          // void Character::reThinkCurrentAIAction()   (drops what it is doing, the game's own way)
    FnGetOwnerships,            // Ownerships* Character::getOwnerships()   (the player faction's for ours, the squad's for NPCs)
    FnInteriorShopFurniture,    // void BuildingInterior::<shop furniture>(lektor<Building*>&)   (what a merchant's trade window sells from)
    FnGetNpcTrader,             // static Character* InventoryGUI::getNPCTrader()   (the merchant of the open trade window)
    FnCharTakeMoney,            // bool Character::takeMoney(int)   (negative: gives)
    FnRClickAutoTrade,          // TradeResult* InventoryGUI::RClickAutoTrade(TradeResult*, const std::string& section, int x, int y, InventoryGUI* to, bool thievery, bool first)
    // ---- lot B: factions
    FnGetRelationData,          // RelationData* FactionRelations::getRelationData(Faction*)   (creates the entry if missing)
    FnBountyMapIndex,           // pair<Faction* const, Bounty>* BountyManager's unordered_map<Faction*, Bounty>::operator[](Faction* const&)   (creates)
    // ---- lot A: doors
    FnDoorOpen,                 // bool DoorStuff::openDoor()   (closed -> opening, plays the sound)
    FnDoorClose,                // bool DoorStuff::closeDoor()   (open -> closing; refused when broken)
    FnDoorLock,                 // void DoorStuff::lockDoor()   (locks it now if closed, else once closed)
    FnDoorUnlock,               // void DoorStuff::unlockDoor()
    FnDoorOpenButton,           // void DoorStuff::openButton(DataPanelLine*)   (the door panel's open/close button)
    FnDoorLockButton,           // void DoorStuff::lockButton(DataPanelLine*)   (the door panel's lock toggle)
    // ---- end lot A
    // ---- lot D: prisons
    FnSetPrisonMode,            // void Character::setPrisonMode(bool on, UseableStuff* cage)   (in / out of a cage or prison)
    FnSetChainedMode,           // void Character::setChainedMode(bool on, const hand& owner)   (shackles on / off; may create the shackles)
    FnSetSlaveState,            // void StateBroadcastData::setSlaveState(SlaveStateEnum)   (enslaved, escaping, ex-slave)
    // ---- lot C: ranged
    FnGunShoot,                 // void GunClass::shoot(Character* me, RootObject* target, StatsEnumerated stat, const Vector3& aimpos)   (fires one projectile)
    FnProjectileGet,            // Projectile* <projectile pool>::get(const std::string& mesh, const std::string& material)   (shoot's projectile)
    // ---- lot E: buildings
    FnCreateFromPreviews,       // void <PreviewGroup>::createBuildings()   (build mode: builds every placed preview of the group)
    FnCreateBuilding,           // Building* RootObjectFactory::createBuilding(GameData*, Vector3, TownBase*, Faction*, Quaternion, FactoryCallbackInterface*, Layout* furnitureOf, Building* doorOf, GameSaveState*, Building* indoorsOf, bool invisible, bool completed, bool isFoliage, int floor, bool outsideFurniture)
    FnBuyMeCallback,            // void Building::buyMeCallback(int answer)   (2: the player confirmed the purchase)
    FnConfirmDismantle,         // void Building::confirmDismantle(int answer)   (2: the player confirmed)
    FnAddConstructionProgress,  // void Building::addConstructionProgress(float amount)   (a worker builds)
    FnAddDismantleProgress,     // bool Building::addDismantleProgress(float amount)   (a worker dismantles)
    FnClearUsageNodes,          // void Building::clearUsageNodes()
    FnCalculateSaleValue,       // int Building::calculateSaleValue()   (price of a building for sale)
    // ---- admin console
    FnHealCompletely,           // void Character::healCompletely()   (every wound, blood, KO)
    FnShowInventoryBuilding,    // InventoryGUI* ForgottenGUI::showInventoryBuilding(const hand& owner)   (a building's own inventory panel)
    // ---- fix G5: a character's job list (the Tâches panel)
    FnCharRemovePermajob,       // void Character::removePermajob(int slot)   (the panel's cross on a job)
    FnCharMovePermajob,         // void Character::movePermajob(int from, int to)   (a job dragged in the panel)
    FnCharRemoveJob,            // void Character::removeJob(TaskType)   (every job of that kind)
    FnCharGetPermajob,          // TaskType Character::getPermajob(int slot) const
    FnCharPermajobCount,        // int Character::getPermajobCount() const
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
inline constexpr uintptr_t MS_hunger = 0x60;         // float (MedicalSystem::isReallyHungry reads it)
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
bool ObjectHandle(const void* rootObject, kc::Handle& out);   // any RootObject (character, item, building)
void* ResolveObject(const kc::Handle& h);                    // any RootObject, or null
void MakeHand(const kc::Handle& h, void* out);               // writes a game `hand` (off::HandSize bytes)
// Opens the game's loot window between two characters (what reaching a body with a loot order does).
bool OpenLootWindow(Character* looter, void* target);   // a character or a container
bool CloseInventoryWindows();                           // every inventory window of the game
int OpenInventoryWindows();                             // how many are open
// Theft from a container by `thief`: 0 not theft (ours, nobody's, a faction that is not a real one),
// 1 theft unseen (the crime is set, the item now belongs to us), 2 caught (the owners react; the move
// must not happen).
int StealCheck(Character* thief, void* container, void* item);
void* FindItemIn(void* container, const kc::ItemState& s);   // the stack in its inventory
// Trade with merchants. A merchant with a home sells from its shop's counters (furniture of its home
// building); its trade window shows them merged.
bool ShopCounters(Character* trader, std::vector<void*>& out);
bool MoneyOf(Character* c, int32_t& out);           // the player faction's cats for ours, a merchant's own
bool SetMoneyOf(Character* c, int32_t money);
bool TakeMoney(Character* c, int32_t amount);       // Character::takeMoney (negative gives; false: cannot pay)
bool OpenTradeWindow(Character* looter, Character* trader);   // the game's trade window (for cats)
Character* NpcTrader();                              // the merchant of the trade window open here, or null
bool MouseHoldsItem();                               // an item is being dragged in an inventory window
int BuildingFunctionOf(void* building);              // BuildingFunction (9 BF_SHOP...), -1 unknown
// An open inventory window (trade, loot) is about `obj` (its character or object): it must close
// before `obj` goes away (the window keeps a raw pointer to it).
bool InventoryWindowShows(const void* obj);
bool ReadOperatorCount(void* useable, uint64_t& n);  // characters using a bed, a chair, a machine (UseableStuff operator set size)
// ---- lot A: doors and locks (plugin/doors.cpp). A door (DoorStuff) or any building with a lock
// (DoorLock: chests, cages...). Read: false when it is neither. Apply: the host's state, the game's
// way for opening and closing (callers hold a HostCallScope on clients).
bool ReadDoor(void* obj, kc::DoorState& out);        // sid and pos not filled
bool ApplyDoorState(void* obj, const kc::DoorState& d);
bool DoorLocked(void* obj);                          // a lock that holds (locked, level > 0, not broken)
bool PressDoorButton(void* door, kc::DoorAction action);   // the door panel's button, through our hooks
// ---- end lot A
// tests: the items of the open trade window's merchant side (or the player side), and a right click
// on one of them (one unit goes to the other side, bought or sold the game's own way)
struct WindowItem { std::string section; int x = 0, y = 0; kc::ItemState state; };
bool TradeWindowItems(bool merchantSide, std::vector<WindowItem>& out);
int TradeRightClick(bool merchantSide, const WindowItem& item);   // TradeResult (0 ok), -1 failed
// tests: the order a right-click on `subject` gives to the nearest selected character
bool CallAddTaskNearest(int task, Character* subject);
bool CallAddTaskNearestObject(int task, void* subject, const kc::Vec3& at);   // tests: the same on any object
bool CallNewPlayerTaskOn(int task, void* subject, const kc::Vec3& at, void* building = nullptr);   // tests: newPlayerTaskSelectedCharacters on that object
bool ReadInSomething(Character* c, int& v);   // 0 nothing, 1 in bed, 2 in a cage
void* FurnitureParent(void* furniture);       // the building a piece of furniture belongs to (or null)
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
// Skill levels (kc::kStatCount floats, in kStatOffsets order) and the player faction's money.
bool ReadStats(Character* c, std::vector<float>& out);
bool WriteStats(Character* c, const std::vector<float>& stats);
bool ReadPlayerMoney(int32_t& out);
bool WritePlayerMoney(int32_t money);
bool IsStatOfCharacter(const void* statField);
bool GainExperience(Character* c, size_t statIndex, float amount);
bool HealCompletely(Character* c);                 // every wound healed, blood back, awake
// The tool its current job (Task_OperateMachine: mining, farming...) put in its hands, or null.
void* JobTool(Character* c);
// Put a new item of that template in its hands the way that job does ("hands" attachment), or take
// it out and destroy it (sid empty). Returns the item now held (or null).
void* SetHandTool(Character* c, void* current, const std::string& sid);
// God mode (host): these characters take no damage and are never knocked out.
void SetGodMode(Character* c, bool on);
void ForgetGodModes();   // a new world: the characters it named are gone
// ---- fix G6: the floor a character is on (CharMovement::floorGroup; 9 = ground floor)
bool ReadFloorGroup(const Character* c, int32_t& group);
bool WriteFloorGroup(Character* c, int32_t group);
bool GodMode(const void* c);   // tests: increaseStat on that stat
bool CallSay(Character* c, const std::string& text);
bool FocusCamera(Character* c);   // tests: the camera goes to that character
// Carrying a body on the shoulder.
bool ReadCarried(Character* c, kc::Handle& carried);   // false: carries nothing
bool CarryCharacter(Character* carrier, Character* who);
bool DropCarried(Character* carrier);
// Standing orders (the squad bar's toggles) and fight style of one character.
uint16_t ReadModes(Character* c, uint8_t& style);
void SetStandingOrder(Character* c, int order, bool on);   // through the game's own function
bool GetStandingOrder(Character* c, int order);
// Objects near a point (any RootObject of the zone grid, characters excluded); what an object is.
void ObjectsNear(const kc::Vec3& pos, float radius, std::vector<void*>& out);
bool ObjectPosition(void* obj, kc::Vec3& out);
bool ObjectTemplate(const void* obj, std::string& sid);
bool TemplateDisplayName(const std::string& sid, std::string& out);   // the name players see
// Squads (ActivePlatoon) of the player faction.
void* SquadOf(Character* c);                                   // Character::platoon
int SquadMemberIndex(Character* c);                            // its place in its squad
bool SquadName(void* squad, std::string& out);
void SetSquadName(void* squad, const std::string& name);
void SquadMembers(void* squad, std::vector<Character*>& out);   // the player characters in it
bool MoveToSquad(void* squad, Character* c, int index);          // what dropping a portrait on a squad does
void* NewSquad();                                              // what the "new squad" button does
void* ShownSquad();                                            // the squad the squad bar shows
void ShowSquad(void* squad);                                   // show that one
// Appearance: the character's appearance GameData, read whole / written back (then the game
// rebuilds the body), plus its name. And the game's character editor, opened on one character.
bool ReadAppearance(Character* c, kc::AppearanceMsg& out);
bool WriteAppearance(Character* c, const kc::AppearanceMsg& m);
bool OpenCharacterEditor(Character* c);
void EditorCharacters(std::vector<Character*>& out);           // who the open editor works on
bool CharacterEditorOpen();
bool CallStartPlayerConversation(Character* npc, Character* pc);   // tests: npc talks to pc (its default conversation)                // tests: Dialogue::say through the hooks   // the float lies inside a live character's CharStats
// Conversations (Dialogue, Character+0x280).
void* CharacterDialogue(Character* c);
Character* DialogueOwner(const void* dialogue);          // Dialogue::me, when it points back
Character* DialogueTarget(const void* dialogue);         // who it talks with (conversationTarget)
bool DialogueShouting(const void* dialogue);
void SetDialogueShouting(void* dialogue, bool shout);
// What the other one says now and the answers the player can pick (what the window would show).
bool ReadDialogueWindowText(const void* dialogue, std::string& text, std::vector<std::string>& replies);
// A std::string the game can read (const&) for as long as `s` lives; 0x28 bytes.
void GameStringView(const std::string& s, void* out);
// Orders: make `only` the whole selection, run `fn`, then restore the player's selection.
void WithSelection(Character* only, const std::function<void()>& fn);
// Run `fn`, then put the player's selection, squad bar and details panel back as they were (fn may
// select characters: a squad created or joined).
void KeepSelection(const std::function<void()>& fn);
void UnselectObject(void* obj);   // PlayerInterface::objectSelected(obj, false)
// The job list of a character (the Tâches panel: follow, operate a machine...), by kind, in order.
int PermajobCount(Character* c);
int PermajobType(Character* c, int slot);              // TaskType, -1: none
bool RemovePermajob(Character* c, int slot);
bool MovePermajob(Character* c, int from, int to);
bool RemoveJobKind(Character* c, int task);            // every job of that kind
bool IsDead(Character* c);
bool IsUnconscious(Character* c);
void SetUnconscious(Character* c, bool on);   // the medical state only (no fall, no timer)
bool IsRagdoll(Character* c);   // the body is physically on the ground (or carried)

bool GetGameHours(double& out);
// Save management (deferred operations, executed by the game a frame later).
bool SaveManagerBusy();                                     // a save/load is pending or running
bool ShowLoadWindow();                                      // opens the game's "Load" window
size_t LocalTaskCount(Character* c);                        // tasks waiting in its own task system
bool DropLocalTasks(Character* c);                          // it drops them (Character::reThinkCurrentAIAction)
bool RequestSave(const std::string& name, std::string* folderOut);   // folder = where it will be written
bool RequestLoad(const std::string& name);
bool SaveFolder(std::string& out);                          // where this machine's saves live

// Character creation / removal (game thread, live world only).
bool ReadSpawnSource(Character* c, kc::SpawnInfo& out);    // template + faction string ids, name, age
void ResetLookupCaches();                                  // call when a new world loads
Character* CreateCharacter(const kc::SpawnInfo& info, const kc::Vec3& pos, std::string* err);
bool DestroyObject(void* obj);

// Inventories (read side).
bool ReadInventory(const void* obj, std::vector<kc::ItemState>& out);   // canonical order (character or container)
// Replace the whole content with these items, laid out like a save load does.
bool RebuildInventory(void* obj, const std::vector<kc::ItemState>& items, std::string* err);
// Host: replay a client's item movement (from -> to, or a drop to the ground).
// Two whole stacks trading places between `from` and `to` (a: from -> to, onto b's slot; b: to ->
// from). Nothing moves on failure.
bool SwapInventoryItems(void* from, void* to, const kc::InvOp& a, const kc::InvOp& b, std::string* err = nullptr);
bool MoveInventoryItem(void* from, void* to, const kc::InvOp& op, std::string* err);

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

// Weather effects. A weather region owns effect groups (one per effect of its current weather);
// a point or wandering group spawns effect handlers at random places and times, each one a
// lightning bolt, a dust storm, a gas cloud... All of this runs on the weather thread.
struct EffectGroupInfo {
    void* group = nullptr;
    kc::EffectKind kind = kc::EffectKind::Point;
    std::string effectSid;
    uint8_t ordinal = 0;              // among the region's groups of that kind showing that effect
    std::vector<void*> handlers;      // live effects, oldest first
};
bool ReadEffectGroups(void* region, std::vector<EffectGroupInfo>& out);   // point and wandering groups only
bool ReadEffect(void* handler, kc::EffectKind kind, kc::WeatherEffect& e);  // fills pos and the state fields
void BlockEffectSpawns(void* group);   // the group places nothing on its next update
// Has the group spawn one effect now, at e.pos, with e's random values. Returns the new handler.
void* SpawnEffect(void* group, const kc::WeatherEffect& e);
bool WriteEffectState(void* handler, const kc::EffectState& s);   // wandering effects
bool StopEffect(void* handler);        // fades out, then the game removes it
// Set only while SpawnEffect runs: the EffectHandler constructor hook builds the effect there.
const float* EffectSpawnPosition();
bool EffectShown(void* handler);
bool RegionRebuildingEffects(void* region);   // its weather just changed: its effect groups are replaced on this update         // the game currently renders it (near enough to the camera)
void HurryEffectSpawn(void* group);      // tests: the group places its next effect on its next update
// tests: "<weatherSid> '<name>' [<effectSid> '<name>' kind]..." for every weather of the region's season
std::string DescribeRegionWeathers(void* region);
void* CameraWeatherRegion();             // the weather region the camera is in
bool StandUp(Character* c);   // clears the unconscious flag, leaves ragdoll, normal posture

float GetFrameSpeed();
bool GetPaused();

// Game calls (game thread only). Return false if the call faulted or the object was invalid.
bool Teleport(Character* c, const kc::Vec3& pos, const kc::Quat& rot);
bool SetDestination(Character* c, const kc::Vec3& dest);
bool GetFacing(const Character* c, kc::Vec3& dir);   // CharMovement facing direction
bool FaceDirection(Character* c, const kc::Vec3& dir);   // the game turns the character that way
void* MovementOf(const Character* c);                    // CharMovement*
kc::Vec3 ForwardOf(const kc::Quat& q);                   // where a character with this rotation faces
bool ReadPace(const Character* c, uint8_t& gait, float& pace);         // speed order and desired speed

// Animations (AnimationClass, reached from the character; its owner is at a fixed offset).
Character* AnimOwner(const void* animationClass);
bool ReadStdString(const void* gameString, std::string& out);   // a std::string as Kenshi's compiler lays it out
void* AnimationOf(const Character* c);
std::string TechniqueName(const void* technique);       // CombatTechniqueData: its animation
std::string AnimDataName(const void* animData);         // AnimationData: its data name
void* FindTechnique(const std::string& name);           // in the game's list of every technique
void* FindAnimData(const Character* c, const std::string& name);   // among the character's animations
struct AnimModes {
    std::string action;     // current action ("" none)
    bool combat = false, carried = false, carryLeft = false, carryRight = false;
    bool guardLegs = false, guardUpper = false;
};
bool ReadAnimModes(const Character* c, AnimModes& out);
std::string CurrentTechniqueName(const Character* c);   // the attack/block it is playing ("-" none)
// Every animation the character is playing right now, layer by layer (what is actually on screen).
struct PlayingAnim {
    uint8_t layer = 0;
    std::string anim;       // Ogre animation name
    std::string data;       // its AnimationData name ("" for combat techniques)
    float time = 0, time01 = 0, weight = 0, desired = 0, speed = 0;
    bool looped = false, fadingOut = false, synched = false;
    void* single = nullptr;
};
bool ReadPlayingAnims(const Character* c, std::vector<PlayingAnim>& out);
bool ReadAnimMaster(const Character* c, float& time, float& speed);   // AnimationClassBase master clock
bool WriteAnimMaster(void* animationClass, float time, float speed);
void* SingleAnimOwner(const void* single);           // its AnimationClass
bool SingleAnimName(const void* single, std::string& out);
void WriteSingleAnim(void* single, float time, float speed, float weight, float desired);
bool ReadSingleAnimTime(const void* single, float& time);
// The time to give an animation playing at `mine` so it matches `want` (looped ones by phase).
float SyncedAnimTime(const void* single, float mine, float want, bool looped, float gameSpeed = 1.0f);
bool ReadAnimMasterOf(const void* ac, float& time, float& speed);
bool CallRunAnimation(Character* c, void* animData, float speed, int layer, float blend);
std::string CurrentStumbleName(const Character* c);     // the stumble it is playing ("-" none)
bool WeaponInHands(const Character* c, std::string& itemSid, std::string& fromSection);   // false: hands empty
bool CallStartCombatAnim(void* fn, Character* c, void* technique, float speed);
bool CallAnimVoid(void* fn, Character* c);
bool CallPlayAction(Character* c, void* animData, float speedMult, float weight, bool stumble);
bool CallStopActionNamed(Character* c, const std::string& name);
bool CallStartStumble(Character* c, void* animData);
bool CallSetCombatMode(Character* c, bool on);
bool CallSetGuard(Character* c, bool legs, bool on);
// Damage numbers ("floaters"): the game shows them from MedicalSystem::addWound.
inline constexpr uintptr_t kAddWoundBegin = 0x6508D0, kAddWoundEnd = 0x651FF1;
Character* CharacterOfHand(const void* hand);
// Items on the ground
bool ItemOnGround(void* item);                             // in the world, not in an inventory
bool DescribeInventoryItem(void* item, kc::ItemState& s);   // what it is and where it sits in its inventory
bool DescribeGroundItem(void* item, kc::Handle& h, kc::ItemState& s, kc::Vec3& pos);
void* ResolveItem(const kc::Handle& h);                    // the item with that handle here, if any
void* CreateGroundItem(const kc::ItemState& s, const kc::Vec3& pos, kc::Handle& localHandle, std::string* why);
bool DestroyItem(void* item);
void GroundItemsNear(const kc::Vec3& pos, float radius, std::vector<void*>& out);
// Any item lying in the world, out of every inventory, even one the game keeps in an item group or
// as a non-physical prop (shop goods, town clutter): what a player can still pick up or steal.
bool ItemLoose(void* item);
void LooseItemsNear(const kc::Vec3& pos, float radius, std::vector<void*>& out);
// The player's own "pick up" order (PlayerInterface::pickupItem) given to that character alone: it
// walks there and the game takes the item, as a theft when it belongs to someone.
bool OrderPickupItem(Character* c, void* item);
void AllObjectsNear(const kc::Vec3& pos, float radius, std::vector<void*>& out);   // items lying within radius
void* FirstLooseItem(Character* c);                        // tests: an unequipped item it carries
bool CallDropItem(Character* c, void* item);               // tests: the character drops it (the game's own drop)
bool CallGiveItem(Character* c, void* item);               // tests: the character takes it
void* ShowFloater(Character* c, const std::string& text, const float colour[4], int size, int speed);   // returns the label
bool SetLabelColor(void* label, const float colour[4]);
bool CallSetCarryMode(Character* c, bool carried, bool left, bool right);
std::string ItemTemplate(const void* item);                                        // its game data id
bool CallDrawWeapon(Character* c, const std::string& itemSid, const std::string& section);   // the matching item it carries
bool CallSheathe(Character* c);
std::string DrawnFrom(const Character* c);   // section the weapon in its hands came from ("-": hands empty)
bool WritePace(Character* c, uint8_t gait, float pace);
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

// ---- lot D: prisons
// What holds a character captive. Character: +0x2F8 inSomething (2 = IN_PRISON), +0x300 inWhat
// (hand of the cage), +0x320 isChained, +0x328 slaveOwner (hand); StateBroadcastData (*+0x1A0):
// +0 slave state, +0xE0 isSlaveOf (Faction*), +0xE8 isEscapedPrisoner, +0xE9 isKidnapped;
// BountyManager (+0xF0): +0x98 prison sentence began (TimeOfDay, 8 bytes), +0xA0 hours to serve.
struct Captivity {
    int inSomething = 0;        // 0 nothing, 1 bed, 2 cage / prison
    void* cage = nullptr;       // the furniture it is in, when any
    bool chained = false;
    kc::Handle slaveOwner;      // invalid: none
    int slaveState = 0;         // SlaveStateEnum
    void* slaveOf = nullptr;    // Faction*
    bool escaped = false, kidnapped = false;
    uint64_t sentenceBegan = 0;
    float sentence = 0;
};
bool ReadCaptivity(Character* c, Captivity& out);
// Clients: the flags, owner, faction and sentence (never the cage itself: SetPrisonMode does that).
bool WriteCaptivity(Character* c, const Captivity& s);
bool SetPrisonMode(Character* c, bool on, void* cage);   // Character::setPrisonMode (puts it in the cage / takes it out)
bool CallSetChainedMode(Character* c, bool on);          // the game's own shackling (tests, host)
bool CallSetSlaveState(Character* c, int state);         // StateBroadcastData::setSlaveState (tests, host)
std::string FactionSidOf(void* faction);                 // a faction's game data id ("" none)
void* FactionBySid(const std::string& sid);
void* NearestCage(const kc::Vec3& at, float radius);     // furniture with function BF_CAGE (8) around a point
// ---- lot C: ranged combat (plugin/ranged.cpp)
namespace rva {
inline constexpr uintptr_t VtTurretBuilding = 0x16d3f78;
inline constexpr uintptr_t VtGunClassTurret = 0x16d5258;
inline constexpr uintptr_t VtGunClassPersonal = 0x16d5138;
} // namespace rva
// A character's ranged combat (Character+0x2F0 RangedCombatClass): its weapon (null: none set up),
// whether it is in ranged combat, its state, the point it aims at and its target.
void* CharacterGun(Character* c);
struct RangedView {
    bool combat = false;
    uint8_t state = 0;
    kc::Vec3 aimPos;
    kc::Handle target;
};
bool ReadRanged(Character* c, RangedView& out);
bool WriteRanged(Character* c, const RangedView& v, bool writeTarget);   // clients: the host's aim
// Turrets: what a gun belongs to (null: a personal weapon), a turret's gun, its aim point.
bool IsTurret(const void* building);
void* GunTurret(void* gun);
void* TurretGun(void* turret);
bool TurretAimPoint(void* turret, kc::Vec3& out);
bool AimTurret(void* turret, const kc::Vec3& target);   // through its gun (GunClassTurret::aimAt)
// A projectile's orientation (its flight path), through Ogre's Node.
bool ProjectileOrientation(void* projectile, kc::Quat& out);
bool SetProjectileOrientation(void* projectile, const kc::Quat& q);
// Fires the gun as the game does (GunClass::shoot through its hook); the projectile it made, if any.
bool FireGun(void* gun, Character* me, Character* target, int stat, const kc::Vec3& aimPos, void*& projectile);
// ---- lot E: buildings (plugin/buildings.cpp uses these)
bool ReadRaw(const void* obj, uintptr_t offset, void* out, size_t n);   // SEH-guarded
bool WriteRaw(void* obj, uintptr_t offset, const void* in, size_t n);
void* GameDataBySid(const std::string& sid);
bool GameDataSidOf(const void* gd, std::string& out);
// building templates (itemType BUILDING) whose players' name contains `part` ("sid name" each)
void BuildingTemplates(const std::string& part, std::vector<std::pair<std::string, std::string>>& out, size_t max);
bool DestroyAnyObject(void* obj);   // GameWorld::destroy for good (any RootObject)

} // namespace kenshi
