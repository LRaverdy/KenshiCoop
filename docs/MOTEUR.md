# Notes moteur : Kenshi 1.0.68 (Steam, x64)

Tout ce que le mod sait du jeu : adresses de fonctions, structures, énumérations et comportements
observés.

**Conventions**
- Les adresses sont des **RVA** : un décalage depuis la base de `kenshi_x64.exe`.
- Les offsets sont en hexadécimal.
- Une « vt 0x… » est un décalage d'emplacement dans la table virtuelle (vtable) de l'objet.

**Exécutable supporté** : « Kenshi 1.0.68 - x64 (Newland) », Steam, de SHA-256
`A596AB4E407C67B58599C54FFB32DC1BF2B64510CDEBD3FA9359EF05A576AEB1`. Le mod refuse toute autre
version.

**Sources**
- Analyse statique de l'exécutable : vtables RTTI, chaînes de caractères, désassemblage.
- Les noms et offsets de KenshiLib (version 1.0.65), recalés sur 1.0.68 grâce à 693 vtables
  communes.
- Expériences en jeu.

KenshiLib est sous licence GPLv3 : **on n'en reprend que des faits** (noms, adresses, offsets),
jamais de code.

**Code** : le mod les définit dans `plugin/kenshi.h` (`rva::`, `off::`, `slot::`) et
`plugin/kenshi.cpp` (table `kFunctions`, constantes locales).

---

## 1. Vérifications au démarrage

1. Le SHA-256 de `kenshi_x64.exe` doit être exactement celui ci-dessus.
2. Pour chaque fonction de `kFunctions`, les 12 premiers octets à l'adresse donnée doivent être
   exactement ceux de la table (prologue).
Si l'un ou l'autre échoue, le mod se désactive, sans risquer de plantage, et l'écrit dans le
journal.

## 2. Fonctions appelées ou détournées (table `kFunctions`)

Colonne « Hook » : la fonction est détournée par MinHook. Sinon, le mod se contente de l'appeler.
Les signatures sont celles du commentaire du code.

| # | Enum | Fonction (table) | RVA | Hook | Signature / rôle (commentaire du code) |
|---|---|---|---|---|---|
| 0 | `FnMainLoop` | `GameWorld::mainLoop_GPUSensitiveStuff` | `0x788A00` | oui | void GameWorld::mainLoop_GPUSensitiveStuff(float) |
| 1 | `FnPlayerMove` | `PlayerInterface::playerMove` | `0x7FA850` | oui | void PlayerInterface::playerMove(const Vector3&, Building*) |
| 2 | `FnAddOrderSelected` | `PlayerInterface::addOrderSelectedCharacters` | `0x7F9E20` | oui | void PlayerInterface::addOrderSelectedCharacters(Building*, TaskType, RootObject*, bool, bool, const Vector3&) |
| 3 | `FnNewPlayerTaskSelected` | `PlayerInterface::newPlayerTaskSelectedCharacters` | `0x7FA650` | oui | void PlayerInterface::newPlayerTaskSelectedCharacters(TaskType, const hand&, Building*, const Vector3&, bool) |
| 4 | `FnSetOrderSelected` | `PlayerInterface::setOrderSelectedCharacters` | `0x7F3880` | oui | void PlayerInterface::setOrderSelectedCharacters(StandingOrder) |
| 5 | `FnStopCharactersMovement` | `PlayerInterface::stopCharactersMovement` | `0x7F6470` | oui | void PlayerInterface::stopCharactersMovement() |
| 6 | `FnPlayerMoveOrderDefault` | `Character::playerMoveOrderDefault` | `0x5D22B0` | oui | void Character::playerMoveOrderDefault(Building*, RootObject*, const Vector3&) |
| 7 | `FnAIUpdate4Frame` | `AI::update4Frame` | `0x5965B0` | oui | void AI::update4Frame(float) |
| 8 | `FnAIPeriodicUpdate` | `AI::periodicUpdate` | `0x5112B0` | oui | void AI::periodicUpdate(float) |
| 9 | `FnSetFrameSpeedMultiplier` | `GameWorld::setFrameSpeedMultiplier` | `0x787CB0` | — | void GameWorld::setFrameSpeedMultiplier(float) |
| 10 | `FnUserPause` | `GameWorld::userPause` | `0x787FB0` | — | void GameWorld::userPause(bool) |
| 11 | `FnHandleResolve` | `HandleTable::resolve` | `0x2676E0` | — | RootObject* resolve(HandleTable*, const hand*, bool) |
| 12 | `FnTogglePause` | `GameWorld::togglePause` | `0x787D40` | — | void GameWorld::togglePause(bool) |
| 13 | `FnMedApplyDamage` | `MedicalSystem::applyDamage` | `0x64F300` | oui | void MedicalSystem::applyDamage(HealthPartStatus*, const Damages&, bool loadingSavestate, bool canSever, const Vector3& force) |
| 14 | `FnMedKnockout` | `MedicalSystem::knockout` | `0x644980` | oui | void MedicalSystem::knockout(float skill01) |
| 15 | `FnDeclareDead` | `Character::declareDead` | `0x7A6200` | oui | void Character::declareDead() |
| 16 | `FnSaveManagerGet` | `SaveManager::getSingleton` | `0x37DD00` | — | static SaveManager* SaveManager::getSingleton() |
| 17 | `FnSaveManagerSave` | `SaveManager::save` | `0x47B920` | — | void SaveManager::save(const std::string& name, bool autosave) (différé) |
| 18 | `FnSaveManagerLoad` | `SaveManager::load` | `0x47B480` | — | void SaveManager::load(const std::string& name) (différé) |
| 19 | `FnCreateRandomCharacter` | `RootObjectFactory::createRandomCharacter` | `0x5836E0` | oui | RootObject* RootObjectFactory::createRandomCharacter(Faction*, Vector3, RootObjectContainer*, GameData*, Building*, float age) |
| 20 | `FnWorldDestroy` | `GameWorld::destroy(RootObject*)` | `0x799AF0` | oui (lot E) | bool GameWorld::destroy(RootObject*, bool justUnloaded, const char* debugInfo) ; détourné chez l'hôte pour voir disparaître les bâtiments suivis |
| 21 | `FnEndCombatMode` | `Character::endCombatMode` | `0x5C91C0` | — | void Character::endCombatMode() |
| 22 | `FnRagdollMode` | `Character::ragdollMode` | `0x5CBD60` | oui | void Character::ragdollMode(bool on, RagdollPart::Enum part) |
| 23 | `FnRegionUpdateBT` | `WeatherRegion::updateBT` | `0x9DDE50` | oui | void WeatherRegion::updateBT() (thread d'arrière-plan : fait avancer la météo) |
| 24 | `FnSeasonGetNewWeather` | `Season::getNewWeather` | `0x9DD980` | oui | void Season::getNewWeather() (tirage de la météo suivante) |
| 25 | `FnInstanceSetupWeather` | `WeatherInstance::setupWeather` | `0x9DCF60` | — | void WeatherInstance::setupWeather(Weather*) |
| 26 | `FnCreateItem` | `RootObjectFactory::createItem` | `0x580750` | — | Item* RootObjectFactory::createItem(GameData*, const hand&, GameData* company, GameData* material, int level, Faction*) |
| 27 | `FnShowTradeWindow` | `ForgottenGUI::showTradeWindow` | `0x791830` | — | void ForgottenGUI::showTradeWindow(const hand& a, const hand& b, TradeWindowType) (différé) |
| 28 | `FnIsRagdoll` | `Character::isRagdoll` | `0x7D1440` | — | bool Character::isRagdoll() const (au sol en ragdoll, ou porté) |
| 29 | `FnRecruit` | `PlayerInterface::recruit` | `0x692820` | — | bool PlayerInterface::recruit(Character*, bool editor) |
| 30 | `FnAddTaskNearest` | `PlayerInterface::addTaskNearestSelectedCharacter` | `0x7FAE70` | oui | void PlayerInterface::addTaskNearestSelectedCharacter(Building*, TaskType, RootObject*, bool shift, const Vector3&, bool noAnimals) |
| 31 | `FnAddJobSelected` | `PlayerInterface::addJobSelectedCharacters` | `0x7F5A90` | oui | void PlayerInterface::addJobSelectedCharacters(TaskType, RootObject*, bool shift, bool add, const Vector3&) |
| 32 | `FnEffectHandlerCtor` | `EffectHandler::EffectHandler` | `0x1034F0` | oui | EffectHandler::EffectHandler(GameData* effect, AreaBiomeGroup*, const Vector3& pos) |
| 33 | `FnEffectAffectObjects` | `EffectHandler::affectObjects` | `0x1020E0` | oui | void EffectHandler::affectObjects() (un effet météo blesse les personnages qu'il atteint) |
| 34 | `FnEffectStop` | `EffectHandler::stop` | `0x100B00` | oui | void EffectHandler::stop() (fondu, puis retrait) |
| 35 | `FnRegionUpdateEffects` | `WeatherRegion::updateWeatherEffects` | `0x9DCAF0` | oui | void WeatherRegion::updateWeatherEffects() (remplace les groupes d'effets après un changement de météo) |
| 36 | `FnAnimStartCombat` | `AnimationClass::startCombatAnimation` | `0x5B7800` | oui | void AnimationClass::startCombatAnimation(CombatTechniqueData*, float speed, std::string extra) |
| 37 | `FnAnimRunCombat` | `AnimationClass::runCombatAnimation` | `0x5B7600` | oui | void AnimationClass::runCombatAnimation(CombatTechniqueData*, float speed, std::string extra) |
| 38 | `FnAnimEndCombat` | `AnimationClass::endCombatAnimation` | `0x5B3C60` | oui | void AnimationClass::endCombatAnimation() |
| 39 | `FnAnimPlayAction` | `AnimationClass::playAction(AnimationData*)` | `0x51EAB0` | oui | void AnimationClass::playAction(AnimationData*, float speedMult, float initialWeight, bool isStumble) |
| 40 | `FnAnimStopAction` | `AnimationClass::stopAction()` | `0x51DFA0` | oui | bool AnimationClass::stopAction() |
| 41 | `FnAnimStopActionNamed` | `AnimationClass::stopAction(name)` | `0x51E0E0` | oui | bool AnimationClass::stopAction(const std::string& name) |
| 42 | `FnAnimStartStumble` | `AnimationClass::startStumble` | `0x520490` | oui | void AnimationClass::startStumble(AnimationData*) |
| 43 | `FnAnimEndStumble` | `AnimationClass::endStumble` | `0x51E840` | oui | void AnimationClass::endStumble() |
| 44 | `FnAnimSetCombatMode` | `AnimationClass::setCombatMode` | `0x51CC30` | oui | void AnimationClass::setCombatMode(bool) |
| 45 | `FnAnimSetCarryMode` | `AnimationClass::setCarryMode` | `0x51C8D0` | oui | void AnimationClass::setCarryMode(bool carried, bool left, bool right) |
| 46 | `FnDrawWeapon` | `CharacterHuman::drawWeapon` | `0x5DBF80` | oui | bool CharacterHuman::drawWeapon(Item*, std::string lastSection) |
| 47 | `FnSheatheWeapon` | `CharacterHuman::sheatheWeapon` | `0x5CC820` | oui | void CharacterHuman::sheatheWeapon() |
| 48 | `FnAnimGuardLegs` | `AnimationClass::setCombatModeLegsIdle` | `0x51C920` | oui | void AnimationClass::setCombatModeLegsIdle(bool) |
| 49 | `FnAnimGuardUpper` | `AnimationClass::setCombatModeUpperIdle` | `0x51C940` | oui | void AnimationClass::setCombatModeUpperIdle(bool) |
| 50 | `FnSingleAnimUpdate` | `SingleAnimation::update` | `0x5B1700` | oui | void AnimationClassBase::SingleAnimation::update(float masterTime, float frameTime, bool sounds) |
| 51 | `FnRunAnimationLayer` | `AnimationClass::runAnimation(AnimationData*, layer)` | `0x5B7AC0` | — | void AnimationClass::runAnimation(AnimationData*, float speed, AnimationLayerEnum, float blend) |
| 52 | `FnSectionCanItemGoHere` | `InventorySection::canItemGoHere` | `0x74BE40` | — | bool InventorySection::canItemGoHere(Item*, int x, int y) |
| 53 | `FnSectionValidPosition` | `InventorySection::existsItemInFootprint` | `0x7466F0` | — | ⚠ voir la note ci-dessous |
| 54 | `FnSectionFootprintTaken` | `InventorySection::getValidInventoryPosition` | `0x74BEC0` | — | ⚠ voir la note ci-dessous |
| 55 | `FnPickupItem` | `PlayerInterface::pickupItem` | `0x7FB3A0` | oui | void PlayerInterface::pickupItem(Item*) (l'ordre « ramasser » du joueur) |
| 56 | `FnGiveItem` | `Character::giveItem` | `0x5CB400` | oui | bool Character::giveItem(Item*, bool dropOnFail, bool destroyOnFail) (les ramassages passent par là) |
| 57 | `FnDropItemHuman` | `CharacterHuman::dropItem` | `0x5CA740` | oui | void CharacterHuman::dropItem(RootObject*) |
| 58 | `FnCreateScreenLabel` | `ForgottenGUI::createScreenLabel` | `0x73FAF0` | oui | ScreenLabel* ForgottenGUI::createScreenLabel(const std::string&, const Colour&, LabelSize, RisingSpeed) |
| 59 | `FnLabelSetTracking` | `ScreenLabel::setTracking` | `0x6E25F0` | oui | void ScreenLabel::setTracking(const hand&, const Vector3& offset) |
| 60 | `FnLabelSetColor` | `ScreenLabel::setColor` | `0x6E2670` | oui | void ScreenLabel::setColor(const Colour&) |
| 61 | `FnReassessCollapse` | `MedicalSystem::reassessCollapseMode` | `0x649320` | oui | void MedicalSystem::reassessCollapseMode(bool medic, bool agony) (décide d'un effondrement) |
| 62 | `FnAnimationSelection` | `AnimationClass::animationSelection` | `0x520500` | oui | void AnimationClass::animationSelection(float time) (choisit quoi jouer à chaque image) |
| 63 | `FnTrackAnimationMovement` | `CharMovement::trackAnimationMovement` | `0x65E240` | oui | void CharMovement::trackAnimationMovement(bool) (les animations déplacent le personnage) |
| 64 | `FnCombatMovementUpdate` | `CharMovement::combatMovementUpdate` | `0x2AF1E0` | oui | void CharMovement::combatMovementUpdate(float, const Vector3& pos, const Vector3& dir, bool moving, Vector3& repulsion, Vector3& facingOut, bool defensive, swordStateEnum, float raceSpeedMult) |
| 65 | `FnIncreaseStat` | `increaseStat` | `0x8C5DF0` | oui | void increaseStat(float& stat, float amount, float upperLimit) (tout gain d'expérience finit ici) |
| 66 | `FnObjectSelected` | `PlayerInterface::objectSelected` | `0x7F7F20` | — | void PlayerInterface::objectSelected(RootObject*, bool select) |
| 67 | `FnUnselectAll` | `PlayerInterface::unselectAll` | `0x7F8DA0` | — | void PlayerInterface::unselectAll() |
| 68 | `FnDialogueSay` | `Dialogue::say` | `0x67FD80` | oui | void Dialogue::say(const std::string& text, DialogLineData* line) (chaque bulle) |
| 69 | `FnDialogueSetInDialog` | `Dialogue::setInDialog` | `0x6746A0` | oui | void Dialogue::setInDialog(bool on) (montre / cache la fenêtre de conversation) |
| 70 | `FnDialogueSetResponses` | `Dialogue::setResponesGUI` | `0x674070` | oui | void Dialogue::setResponesGUI() (les réponses dans la fenêtre) |
| 71 | `FnDialogueSetReplyText` | `Dialogue::setConversationReplyGUI` | `0x674170` | oui | void Dialogue::setConversationReplyGUI() (ce que dit l'interlocuteur dans la fenêtre) |
| 72 | `FnDialogueReplyClicked` | `Dialogue::replyClicked` | `0x683DF0` | — | void Dialogue::replyClicked(int) |
| 73 | `FnDialogueSendEvent` | `Dialogue::sendEvent` | `0x684990` | oui | bool Dialogue::sendEvent(Character*, EventTriggerEnum) |
| 74 | `FnDialogueSendEventOverride` | `Dialogue::sendEventOverride` | `0x685660` | oui | bool Dialogue::sendEventOverride(Character*, EventTriggerEnum, bool) |
| 75 | `FnDialogueStartConversation` | `Dialogue::startConversation` | `0x683F90` | oui | bool Dialogue::startConversation(Character*, DialogLineData*, EventTriggerEnum, bool) |
| 76 | `FnDialogueStartPlayerConversation` | `Dialogue::startPlayerConversation` | `0x684320` | oui | bool Dialogue::startPlayerConversation(Character*, DialogLineData*) |
| 77 | `FnDialogueDoActions` | `Dialogue::_doActions` | `0x680560` | oui | void Dialogue::_doActions(DialogLineData*) (recrutement, primes, relations… d'une réplique) |
| 78 | `FnTaskSystemUpdate` | `AITaskSytem::update` | `0x50D920` | — | void AITaskSytem::update(Vector3 position, float time) (fait tourner la tâche en cours) |
| 79 | `FnSensoryDialogAssessment` | `SensoryData::dialogAssessmentUpdate` | `0x85A5F0` | oui | void SensoryData::dialogAssessmentUpdate(float, bool) (remarque les crimes, décide de parler) |
| 80 | `FnSensoryAssessCrimes` | `SensoryData::assessCrimes` | `0x854D10` | oui | void SensoryData::assessCrimes(Character*) |
| 81 | `FnBlackboardUpdate` | `Blackboard::update` | `0x26A320` | oui | void Blackboard::update(float) (IA d'escouade) |
| 82 | `FnBlackboardPeriodic` | `Blackboard::periodicUpdate` | `0x2732B0` | oui | void Blackboard::periodicUpdate(float) |
| 83 | `FnFactionWarPeriodic` | `FactionWarMgr::periodicUpdate` | `0x9CB310` | oui | void FactionWarMgr::periodicUpdate() (raids, campagnes) |
| 84 | `FnUniqueSquadPeriodic` | `FactionUniqueSquadManager::periodicUpdate` | `0x2DD680` | oui | void FactionUniqueSquadManager::periodicUpdate(float) |
| 85 | `FnAffectRelationsAmount` | `FactionRelations::affectRelations(amount)` | `0x6B2EA0` | oui | void FactionRelations::affectRelations(Faction*, float amount, float mult) |
| 86 | `FnAffectRelationsEvent` | `FactionRelations::affectRelations(event)` | `0x6B2D20` | oui | void FactionRelations::affectRelations(Faction*, FactionEvent, float mult) |
| 87 | `FnSetRelation` | `FactionRelations::setRelation` | `0x6B4D80` | oui | void FactionRelations::setRelation(Faction*, float) |
| 88 | `FnSetCrime` | `BountyManager::setCrime` | `0x852C80` | oui | bool BountyManager::setCrime(CrimeEnum, Faction*, const hand&) |
| 89 | `FnAssignBounty` | `BountyManager::assignBountyForCrimes` | `0x853EC0` | oui | void BountyManager::assignBountyForCrimes(Faction*) |
| 90 | `FnFocusCamera` | `PlayerInterface::focusCameraSelectedCharacter` | `0x7F37F0` | — | void PlayerInterface::focusCameraSelectedCharacter() |
| 91 | `FnSquadAddCharacterAt` | `ActivePlatoon::addCharacterAt` | `0x796620` | oui | void ActivePlatoon::addCharacterAt(RootObject*, int index) (met un personnage dans une escouade) |
| 92 | `FnCreateSquad` | `PlayerInterface::createSquad` | `0x7F4910` | — | ActivePlatoon* PlayerInterface::createSquad() |
| 93 | `FnCloseCharacterEditor` | `ForgottenGUI::closeCharacterEditor` | `0x6E22B0` | oui | void ForgottenGUI::closeCharacterEditor() (seul le bouton de validation de l'éditeur l'appelle) |
| 94 | `FnShowCharacterEditor` | `ForgottenGUI::showCharacterEditor` | `0x6E32F0` | — | void ForgottenGUI::showCharacterEditor(lektor<Character*>, CharacterEditMode, const vector<GameDataReference>* races) |
| 95 | `FnSetAppearanceData` | `Character::setAppearanceData` | `0x5B9E90` | — | void Character::setAppearanceData(GameDataCopyStandalone*) |
| 96 | `FnMapBool` | `GameData bool map []` | `0x6C7C0` | — | tables de valeurs d'une GameData : operator[](const std::string&) → pair* |
| 97 | `FnMapString` | `GameData string map []` | `0x6D190` | — | idem (chaînes) |
| 98 | `FnMapInt` | `GameData int map []` | `0x6CF30` | — | idem (entiers) |
| 99 | `FnMapFloat` | `GameData float map []` | `0xB0AB0` | — | idem (flottants) |
| 100 | `FnMapVec3` | `GameData vec3 map []` | `0xB0D40` | — | idem (vecteurs) |
| 101 | `FnMapQuat` | `GameData quat map []` | `0x2E6110` | — | idem (quaternions) |
| 102 | `FnStringAssign` | `std::string::assign` | `0x69BC0` | — | std::string& std::string::assign(const std::string&, size_t pos, size_t n) |
| 103 | `FnCharAddOrder` | `Character::addOrder` | `0x5D20D0` | — | void Character::addOrder(Building* dest, TaskType, RootObject* subject, bool shift, bool clear, const Vector3&) |
| 104 | `FnCharAddJob` | `Character::addJob` | `0x5C8DA0` | — | void Character::addJob(TaskType, RootObject* subject, bool shift, bool add, const Vector3&) |
| 105 | `FnSetStandingOrder` | `Character::setStandingOrder` | `0x5CAA50` | oui | void Character::setStandingOrder(StandingOrder, bool on) |
| 106 | `FnPickupCharacter` | `Character::pickupObject` | `0x5CFF90` | oui | void Character::pickupObject(Character* who) (met un corps sur l'épaule) |
| 107 | `FnDropCarried` | `Character::dropCarriedObject` | `0x5CE1E0` | — | void Character::dropCarriedObject(bool ragdollHim, bool removeOnly) |
| 108 | `FnSetCurrentPlatoon` | `PlayerInterface::setCurrentPlatoon` | `0x7F2800` | — | bool PlayerInterface::setCurrentPlatoon(Platoon*) (l'escouade montrée par la barre d'escouade) |
| 109 | `FnShowLoadWindow` | `SaveManager::showLoad` | `0x4824C0` | — | void SaveManager::showLoad() (la fenêtre « Charger » du jeu) |
| 110 | `FnReThinkAIAction` | `Character::reThinkCurrentAIAction` | `0x5C8330` | — | void Character::reThinkCurrentAIAction() (abandonne ce qu'il fait, à la manière du jeu) |

| 111 | `FnGetOwnerships` | `Character::getOwnerships` | `0x7956A0` | — | Ownerships* Character::getOwnerships() : celles de la faction du joueur pour nos personnages, celles de l'escouade (Platoon) pour un PNJ |
| 112 | `FnInteriorShopFurniture` | meubles de boutique d'un intérieur | `0x54ACB0` | — | void BuildingInterior::<?>(lektor<Building*>&) : les meubles d'où la fenêtre d'un marchand vend (ce que le constructeur de `ShopTrader` appelle) |
| 113 | `FnGetNpcTrader` | `InventoryGUI::getNPCTrader` | `0x70E2D0` | — | static Character* getNPCTrader() : le marchand de la fenêtre de commerce ouverte (null si moins de 2 fenêtres) |
| 114 | `FnCharTakeMoney` | `Character::takeMoney` | `0x7965F0` | — | bool Character::takeMoney(int) : `getOwnerships()->takeMoney` ; négatif = donner ; false si pas assez |
| 115 | `FnRClickAutoTrade` | `InventoryGUI::RClickAutoTrade` | `0x713D20` | — | TradeResult* RClickAutoTrade(TradeResult* out, const std::string& section, int x, int y, InventoryGUI* vers, bool vol, bool premier) : clic droit, une unité passe de l'autre côté (achat / vente du jeu) ; tests seulement |
| 116 | `FnGetRelationData` | `FactionRelations::getRelationData` | `0x6B4C60` | — | RelationData* getRelationData(Faction*) : trouve ou crée la relation (lot B) |
| 117 | `FnBountyMapIndex` | `BountyManager bounties operator[]` | `0x5E7EE0` | — | paire `<Faction*, Bounty>` de la table des primes, créée si absente (lot B) |
| 116 | `FnDoorOpen` | `DoorStuff::openDoor` | `0x297040` | oui (lot A) | bool openDoor() : fermée → en ouverture ; refusé chez un client hors `HostCallScope` |
| 117 | `FnDoorClose` | `DoorStuff::closeDoor` | `0x298C50` | oui (lot A) | bool closeDoor() : ouverte → en fermeture (pas si cassée) |
| 118 | `FnDoorLock` | `DoorStuff::lockDoor` | `0x2969F0` | oui (lot A) | void lockDoor() |
| 119 | `FnDoorUnlock` | `DoorStuff::unlockDoor` | `0x56A3D0` | oui (lot A) | void unlockDoor() |
| 120 | `FnDoorOpenButton` | `DoorStuff::openButton` | `0x546FB0` | oui (lot A) | void openButton(DataPanelLine*) : chez un client, le clic part à l'hôte |
| 121 | `FnDoorLockButton` | `DoorStuff::lockButton` | `0x547060` | oui (lot A) | void lockButton(DataPanelLine*) : idem |
| 116 | `FnSetPrisonMode` | `Character::setPrisonMode` | `0x330600` | oui | lot D : met dans une cage / en sort (voir section 10) |
| 117 | `FnSetChainedMode` | `Character::setChainedMode` | `0x32E590` | oui | lot D : menottes (crée l'objet si besoin) |
| 118 | `FnSetSlaveState` | `StateBroadcastData::setSlaveState` | `0x5A4940` | oui | lot D : état d'esclave |
| 116 | `FnGunShoot` | `GunClass::shoot` | `0x43A730` | oui | (lot C) void shoot(Character* me, RootObject* cible, StatsEnumerated stat, const Vector3& visée) : tire un projectile ; hôte : noté pour les clients ; client : seuls les tirs rejoués par le mod passent |
| 117 | `FnProjectileGet` | réserve des projectiles `get` | `0x43A2F0` | oui | (lot C) Projectile* get(mesh, matériau), appelé par `shoot` seulement : attrape le projectile du tir |
| 116 | `FnCreateFromPreviews` | <PreviewGroup>::createBuildings | `0x4D72A0` | oui | lot E : le mode construction bâtit les aperçus posés du groupe |
| 117 | `FnCreateBuilding` | `RootObjectFactory::createBuilding` | `0x57CC70` | oui | lot E : la fabrique de bâtiments (16 arguments, voir section 10) |
| 118 | `FnBuyMeCallback` | `Building::buyMeCallback` | `0x7AD6C0` | oui | lot E : achat confirmé (réponse 2) |
| 119 | `FnConfirmDismantle` | `Building::confirmDismantle` | `0x54FEA0` | oui | lot E : démontage confirmé (réponse 2) |
| 120 | `FnAddConstructionProgress` | `Building::addConstructionProgress` | `0x5595A0` | oui | lot E : un ouvrier construit (refusé chez les clients) |
| 121 | `FnAddDismantleProgress` | `Building::addDismantleProgress` | `0x2A2860` | oui | lot E : un ouvrier démonte (refusé chez les clients) |
| 122 | `FnClearUsageNodes` | `Building::clearUsageNodes` | `0x54C4D0` | — | lot E : ce que fait le mode construction à un bâtiment neuf |
| 123 | `FnCalculateSaleValue` | `Building::calculateSaleValue` | `0x7AD300` | — | lot E : prix d'un bâtiment à vendre |

| fix G5 | `FnCharRemovePermajob` | `Character::removePermajob(int slot)` | `0x5C9000` | oui | croix du panneau Tâches (`OrderCellView::onRemove` `0x7268B0`, `OrdersPanel::removeJob` `0x7265A0`) ; client : demandé à l'hôte |
| fix G5 | `FnCharMovePermajob` | `Character::movePermajob(int, int)` | `0x5C8FC0` | oui | glisser une tâche dans le panneau (`OrdersPanel::moveJob`, `notifyEndDropOrder`) |
| fix G5 | `FnCharRemoveJob` | `Character::removeJob(TaskType)` | `0x5C8EB0` | oui | `removeJobSelectedCharacters` (`0x7F5C80`) |
| fix G5 | `FnCharGetPermajob` | `Character::getPermajob(int) const` | `0x5C8EF0` | — | type de la tâche d'un emplacement |
| fix G5 | `FnCharPermajobCount` | `Character::getPermajobCount() const` | `0x5C8F30` | — | nombre de tâches |

| validité | `FnTerrainHeight` | `UtilityT::getTerrainHeight(float x, float z)` | `0x9B3710` | — | le sol sous l'eau éventuelle ; -99 où aucun terrain n'est connu (saut vers `getTerrainHeightFast(x, z, nullptr)` `0x9B32F0`) |
| validité | `FnTerrainWithWaterHeight` | `UtilityT::getTerrainWithWaterHeight(float x, float z)` | `0x9B3720` | — | max(sol, 100) : la hauteur que `createBuildings` retire à la position (appel vérifié à `0x4D7512`) |
| validité | `FnIsIndoors` | `UtilityT::isIndoors(const Vector3&)` | `0x9B2BA0` | — | le bâtiment dont l'intérieur contient ce point (rayons vers le haut et le bas, groupe 0x2000) ; appelée par `isIndoorsOK` et le clic du mode construction |
| validité | `FnGetNearestTown` | `TownList::getNearestTown(pos, owner, except, mine, TownType)` | `0x927F10` | — | appelée par `placementVerification` (`0x4DBAF6`, type 10) |
| validité | `FnWithinBordersRange` | `TownBase::withinBordersRange(pos, mult) const` | `0x926D50` | — | (rayon vt 0x2A0 × mult)² > distance² au sol ; faux pour le type 8 |
| validité | `FnGetNearestWithinItsRadius` | `TownList::getNearestWithinItsRadius(pos, skipPlayerTowns) const` | `0x928890` | — | appelée par `placementVerification` (`0x4DBBBE`, avec `true`) |

Les six adresses : traduites de KenshiLib 1.0.65 (décalage +0x780 / +0x17B0 dans ces zones) puis
confirmées dans le 1.0.68 comme cibles des appels lus dans `placementVerification`, `createBuildings`
et le clic du mode construction ; prologues relevés dans `kenshi_x64.exe` et vérifiés au démarrage.

Les cinq passent par `Character::ai` (+0x650) puis `AI::orders` (+0x20, `OrdersReceiver`, tâches
à +0x90 nombre et +0x98 tableau de `Tasker*`). `Character::clearPermajobs` (`0x5C8FE0`) n'a aucun
appelant : le panneau n'a pas de « tout effacer » propre.

L'ordre de la table doit suivre celui de l'énumération `Fn` : la vérification des prologues ne voit
pas deux lignes inversées (chaque adresse correspond bien à ses octets). Deux lignes l'étaient
(`getValidInventoryPosition` / `existsItemInFootprint`, sans effet car jamais appelées) ; c'est
corrigé, et `python tools/check_functions.py` vérifie désormais que chaque entrée correspond au
commentaire de son énumérateur.

### Autres adresses de fonctions utilisées directement

| RVA | Fonction | Usage |
|---|---|---|
| `0x4BE480` | `ActivePlatoon::setName(const std::string&)` | renommer une escouade |
| `0x9FF210` | `ZoneSpacialGrid::getObjects(point, radius, filter, lektor&, max)` | objets autour d'un point |
| `0xED6504` / `0xED64FE` | `operator new` / `operator delete` du jeu | allouer ce que le jeu libérera (lektor) |
| `0x6508D0`–`0x651FF1` | corps de `MedicalSystem::addWound` | reconnaître les chiffres de dégâts (adresse de retour) |
| `0x70CEF0` | `Inventory::getCallbackCharacter` | propriétaire d'un inventaire (victime d'un vol) |

## 3. Objets globaux et vtables

| RVA | Quoi |
|---|---|
| `0x2134110` | `GameWorld` global (`ou`) |
| `0x2133F90` | table des handles (1ᵉʳ argument de `HandleTable::resolve`) |
| `0x21303D0` | pointeur ; `+0xA0` = heure du jeu (double, en heures) |
| `0x2133573` | bool : `SaveManager` utilise le chemin utilisateur, sinon le chemin local |
| `0x21337B0` | `ForgottenGUI` (objet de l'interface ; fenêtres de commerce, de fouille, éditeur) |
| `0x2128190` | pointeur ; `+0` = `WeatherRegion*` où se trouve la caméra |
| `0x21349C0` | pointeur vers le gestionnaire de zones (grilles spatiales) |
| `0x2011F78` | `lektor<CombatTechniqueData*>` : toutes les techniques de combat |
| `0x1E3A5F8` | le `hand` vide que le jeu donne à un objet posé au sol |
| `0x2247DA0` | pointeur vers `Quaternion::IDENTITY` |

Vtables (pour reconnaître un objet) :

| RVA | Classe |
|---|---|
| `0x16F9EB8` | `Character` |
| `0x16F2848` | `CharacterHuman` |
| `0x16F2208` | `CharacterAnimal` |
| `0x1722608` | `GameWorld` |
| `0x171A2B8` | `PlayerInterface` |
| `0x16FCC88` | `CharMovement` |
| `0x16FA3E8` | `AI` |
| `0x16F6698` / `0x16F67B8` | `CombatClass` / `CombatClassAI` |
| `0x16852D0` | `hand` |
| `0x168BE10` / `0x168BE58` | groupes d'effets ponctuels / errants |
| `0x168C0A8` / `0x168C128` | effets ponctuels / errants (`EffectHandler`) |
| `0x168BAF0` | `lektor<RootObject*>` |
| `0x16EFE88` | `lektor<Character*>` |

## 4. Structures

### Types de base
- **`std::string`** (compilateur VS2010) : 0x28 octets.
  - +0x00 : texte en ligne (moins de 16 caractères) ou pointeur ;
  - +0x10 : taille ;
  - +0x18 : capacité.
  `GameStringView` fabrique une vue en lecture seule de ce format.
- **`lektor<T>`** (le vecteur de Kenshi) : vtable, +0x8 `count` (u32), +0xC capacité, +0x10
  `data`.
- **`boost::unordered_set/map`** : +0x18 nombre de compartiments, +0x20 taille, +0x38
  compartiments.
  - Nœud : +0x0 suivant, +0x10 valeur.
  - Table indexée par chaîne : clé à +0x10, valeur à +0x38.
  - Table indexée par `hand` : valeur à +0x30.
- **`hand`** (0x20 octets) : vtable, +0x8 `type`, +0xC `container`, +0x10 `containerSerial`, +0x14
  `index`, +0x18 `serial`.
  - Types connus : 0 bâtiment ou meuble ; 1 personnage ; 0x5B personnage animal ; 0xB « aucun ».

### GameWorld
| Offset | Champ |
|---|---|
| +0xF0 | gestionnaire de GameData (+0x20 : table chaîne → `GameData*`) |
| +0x4A0 | `RootObjectFactory*` |
| +0x4A8 | `FactionManager*` (+0 : `lektor<Faction*>`) |
| +0x580 | `PlayerInterface*` |
| +0x700 | `frameSpeedMult` (float) |
| +0x708 | « death parade » : table `hand` → `Character*` des cadavres |
| +0x750 | ensemble des personnages actifs (`Character*`) |
| +0x8B9 | `paused` (bool) |

### PlayerInterface
| Offset | Champ |
|---|---|
| +0xF0 | `hand` du personnage sélectionné (panneau de détails) |
| +0x208 | ensemble des `hand` sélectionnés |
| +0x220 / +0x228 / +0x240 | file des sélectionnés parcourue par les fonctions « *SelectedCharacters » (début, nombre, blocs) |
| +0x2A0 | `Faction*` du joueur |
| +0x2A8 | escouade montrée par la barre d'escouade (`setCurrentPlatoon`) |
| +0x2B0 | `lektor<Character*>` des personnages du joueur |

- `PlayerInterface::playerMove(const Vector3& pos, Building*)` (détourné) reçoit le point du sol où
  le joueur a cliqué droit pour un déplacement, hauteur du sol comprise : l'administration le garde
  comme « point marqué » pour y téléporter des persos (pas de fonction de hauteur du terrain
  connue). Un clic sur la carte du monde passe-t-il aussi par là ? Non vérifié.

### RootObject / Character
| Offset | Champ |
|---|---|
| +0x10 | `Faction*` propriétaire |
| +0x18 | nom affiché (`std::string`) |
| +0x40 | `GameData*` modèle (nom à +0x28, type à +0x50, identifiant `sid` à +0x58) |
| +0x58 | `hand` |
| +0xB0 | rotation (quaternion w, x, y, z) |
| +0xD4 | furtif (bool, ordres STEALTH_ON / OFF) |
| +0xF0 | `BountyManager` du personnage (premier argument de `setCrime`) |
| +0x148 / +0x150 / +0x160 / +0x180 | crime actif : type, faction, `hand` de la victime, expiration (au moins 3 s), d'après la recherche |
| +0x280 | `Dialogue*` |
| +0x2E8 | `Inventory*` |
| +0x2F8 | `inSomething` : 0 rien, 1 au lit, 2 en prison / cage ; `inWhat` (`hand`) à +0x300 |
| +0x348 / +0x380 | porte quelqu'un (bool) / `hand` du corps porté |
| +0x418 | rang dans l'escouade (int) |
| +0x448 | `AnimationClass*` |
| +0x450 | `CharStats*` (son `me` à +0x10) |
| +0x458 | `MedicalSystem` (en ligne) |
| +0x640 | `CharMovement*` |
| +0x648 | `CharBody*` (+0x8 `CombatClass*`) |
| +0x650 | `AI*` (son `me` à +0x2F8) |
| +0x658 | `ActivePlatoon*` (escouade ; +0x78 → `Platoon`, nommé) |

`CharacterHuman` : +0x6D8 arme en main (`Item*`), +0x6E0 section d'où elle vient (`std::string`).

Emplacements de vtable :

| vt | Fonction |
|---|---|
| 0x10 | `setName` |
| 0x30 | `isUnconcious` |
| 0x40 | `getPosition` (retour par pointeur caché) |
| 0x58 | `getFaction` |
| 0x160 | `getInventory` (bâtiments, meubles) |
| 0x1A8 | `dropItem` |
| 0x290 | `isItOkForMeToLoot` |
| 0x298 | `ImStealingDoYouNotice` |
| 0x370 | `setProneState` |
| 0x390 | `getAge` |

### CharStats (ordres permanents et compétences)
- Ordres permanents :
  - +0x128 bloquer (défensif), +0x129 distance, +0x12A narguer, +0x12B tenir la position, +0x12C
    passif ;
  - « poursuivre » se lit dans `AI` (+0x20) puis +0x35 ;
  - le style de combat se lit dans `AI` +0x2B8 : 0 attaque, 1 défense, 2 esquive.
- Compétences (`kStatOffsets`, 34 flottants, dans l'ordre) : force, attaque, travail physique,
  science, ingénierie, robotique, forge d'armes, forge d'armures, médecine, vol, tourelles,
  agriculture, cuisine, discrétion, athlétisme, dextérité, défense, robustesse, assassinat, nage,
  perception, katanas, sabres, hackers, armes lourdes, armes contondantes, arts martiaux, esquive,
  armes d'hast, arbalètes, tir ami, crochetage, fabrication d'arcs, combat de masse.
  - Offsets : 0x80, 0x120, 0xE4, 0xE0, 0xCC, 0xDC, 0xD0, 0xD4, 0x98, 0xAC, 0x114, 0xE8, 0xEC,
    0xA4, 0x94, 0x88, 0x124, 0x90, 0xB8, 0xA8, 0x8C, 0xF8, 0xFC, 0x100, 0x108, 0x104, 0x10C,
    0xF0, 0x118, 0x110, 0xF4, 0xB0, 0xD8, 0x9C.
- `increaseStat(float& stat, float amount, float upperLimit)` (`0x8C5DF0`, désassemblé le 10 oct.
  pour l'administration) : `stat += amount * ((upperLimit - stat) / upperLimit)²`. Rien ne change
  si `amount <= 0`, si `amount > 20` (constante `0x1683188` = 20.0) ou si le facteur dépasse 20. Un
  résultat NaN est annulé (l'ancienne valeur revient ; si elle-même est NaN, la compétence passe à
  20). Le jeu l'appelle avec `upperLimit` = 100. Conséquences : un « gain d'expérience » se donne
  par paquets de 20 au plus, avec des rendements décroissants (à 50, 20 points donnent +5) ; près
  de 100, les gains deviennent infimes (à 99, +0,002 par appel de 20). L'administration (niveaux)
  calcule chaque appel pour tomber juste et écrit le dernier bout elle-même après 600 appels.

### MedicalSystem (Character + 0x458)
| Offset | Champ |
|---|---|
| +0x60 | faim |
| +0x70 | sang |
| +0xA0 | minuteur de K.-O. |
| +0xE0 | `me` (`Character*`) |
| +0x161 | inconscient (bool) |
| +0x164 | mort (bool) |
| +0x190 | `lektor<HealthPartStatus*>` (membres : chair +0x40, étourdissement +0x44, bandage +0x48) |
| +0x1A8 | pointeur utilisé par `knockout` pour calculer la durée |

### CharMovement
- +0x20 consigne d'allure (`MoveSpeed`, 4 valeurs) ; +0x24 en mouvement (bool) ; +0xB8 vitesse
  actuelle ; +0xBC vitesse voulue ; +0xD0 direction regardée ; +0xDC destination.
- vt 0x30 `faceDirection` ; vt 0x90 `setDestination(pos, priorité, bool)` ; vt 0x98 `halt` ; vt
  0xB8 `_setPositionAndTeleport` ; vt 0xC0 `setPositionDirectionAndTeleport` (s'arrête d'abord) ;
  vt 0xC8 `_setPositionSimple` (déplace le corps et la capsule physique, sans arrêter la marche).

### CombatClass
- +0x130 mode combat actif ; +0x298 `hand` de la cible.
- vt 0x10 `initCombatMode(const hand&, int end, bool focused)` : `end` = 0 pour engager.

### AnimationClass (Character + 0x448)
| Offset | Champ |
|---|---|
| +0xC8 / +0xCC | horloge maîtresse : temps (phase 0..1) / vitesse |
| +0xD0 | `lektor<AnimationLayer*>` (chaque couche : liste « ajout » +0x0, liste « retrait » +0x18 de `SingleAnimation*`) |
| +0xE8 | `AppearanceBase*` (+0x148 : GameData d'apparence) |
| +0x208 | technique demandée |
| +0x210 | action demandée |
| +0x21E / +0x21F / +0x220 | porter à gauche / à droite / être porté |
| +0x228 | trébuché demandé |
| +0x244 | mode combat |
| +0x24C / +0x24D | garde des jambes / du haut du corps |
| +0x2C0 | `AnimList*` (+0xB8 : table nom → `AnimationData*`) |
| +0x2D8 | `me` (`Character*`) |

- `SingleAnimation` : +0x0 nom Ogre, +0x30 `AnimationData*`, +0x38 classe propriétaire, +0x40
  vitesse, +0x44 poids, +0x48 poids voulu, +0x50 temps, +0x54 temps 0..1, +0x5D en boucle, +0x67
  « encore voulue ».
- `AnimationData` : nom à +0x8. `CombatTechniqueData` : animation à +0x0.

### Dialogue (Character + 0x280)
- +0x149 crie (bool) ; +0x150 `me` ; +0x158 `hand` de l'interlocuteur ; +0x258 réponses
  (`vector<std::string>`) ; +0x278 réplique de l'interlocuteur (`std::string`).
- Événement 1 = `EV_PLAYER_TALK_TO_ME`.

### Inventaires
- `Inventory` : +0x10 tous les objets (`lektor<Item*>`) ; +0x28 sections (table nom →
  `InventorySection*`) ; +0x68 sections dans l'ordre de recherche ; +0x80 objet de rappel
  (`callbackObject`) ; +0x88 propriétaire.
- vtable d'`Inventory` : 0x10 `addItem`, 0x18 `tryAddItem`, 0x20 `hasRoomForItem`, 0x28
  `removeItemDontDestroy`, 0x30 `removeAutoDestroy`, 0x38 `dropItem`.
- `InventorySection` : +0x30 largeur, +0x34 hauteur, +0x40 objets (`vector` de
  `{Item*, u16 x, y, w, h}`, 0x10 octets chacun), +0xD0 activée. vt 0x10 `addItem`, vt 0x18
  `_addItem(item, x, y)`.
- `Item` : +0xC0 fabricant, +0xC8 matériau, +0xD8 dans un inventaire, +0xDC position, +0xE8
  section, +0x118 charges, +0x11C qualité, +0x129 équipé, +0x12C quantité, +0x130 / +0x134
  largeur / hauteur, +0x188 groupe d'objets.
  - vt 0x228 `activate`, vt 0x238 `deactivate`, vt 0x2B8 `getLevel`, vt 0x358
    `setInventoryWeAreIn`.
- Les armes se créent à partir de leur fabricant :
  `createItem(factory, fabricant, hand, type d'arme, matériau, niveau)`. Le fabricant vient en
  premier, comme dans le code du jeu.
- L'équipement porté n'existe que dans les sections d'équipement, pas dans la liste
  `_allItems`.

### Escouades
- `Character` +0x658 → `ActivePlatoon` ; +0x78 → `Platoon` (nom à +0x18).
- Créer une escouade : `PlayerInterface::createSquad`.
- Déplacer un personnage : `ActivePlatoon::addCharacterAt`, ce que fait le dépôt d'un portrait.

### Apparence et éditeur
- GameData d'apparence : `Character` +0x448 → +0xE8 → +0x148.
- Tables de valeurs dans la GameData :
  - +0xF8 booléens, +0x138 chaînes, +0x178 entiers, +0x1B8 flottants, +0x238 vec3, +0x278
    quaternions, +0x2B8 références ;
  - une référence occupe 0x40 octets : identifiant à +0x10, `GameData*` à +0x38.
- On écrit par l'`operator[]` du jeu (fonctions 96 à 101), puis
  `setAppearanceData(c, même pointeur)` : le jeu reconstruit le corps.
- Éditeur :
  - `ForgottenGUI` +0x1C0 ; ses personnages à +0x260 (données) et +0x268 (nombre) ;
  - ouverture : `showCharacterEditor(gui, lektor par valeur, mode, races)`. Mode 2 =
    `EDIT_DEBUG` : race et sexe modifiables, rien de tiré au hasard ; `races = nullptr`.

### SaveManager
- +0x50 chemin local, +0x78 chemin utilisateur, +0xA0 opération en attente (0 = rien), +0xD8
  dossier de la dernière demande.
- Les sauvegardes sont dans `%LOCALAPPDATA%\kenshi\save\` sur cette machine.

### Gestionnaire de zones (`*(0x21349C0)`)
- Une grille spatiale par genre d'objet : +0x10 personnages, **+0x48 bâtiments et meubles**,
  +0x80 objets.
- Requête : `ZoneSpacialGrid::getObjects`, avec une `lektor` allouée par le `new` du jeu.

### Météo
- `WeatherRegion` :
  - +0x0 groupe de biomes (GameData à +0x10) ; +0x8 / +0x10 saisons ; +0x30 `WeatherInstance` ;
    +0x38 saison ; +0x40 rang de la saison ; +0x44 fin de saison ;
  - +0x69 effets à refaire ; +0x70 / +0x78 groupes d'effets ; +0xB1 nouvelle météo.
- `WeatherInstance` : +0x8 météo, +0x10 force des effets, +0x14 force, +0x18 vent, +0x1C direction
  du vent, +0x28 à +0x40 montée du vent, +0x44 début, +0x48 fin, +0x4C mise à jour du vent, +0x50
  temps.
- Groupe d'effets : +0x8 données, +0x20 / +0x28 effets vivants, +0x40 minuteur d'apparition.
  vt 0x20 `spawn()`.
- Effet (`EffectHandler`) : +0x24 position, +0x48 force, +0x54 âge, +0x58 durée restante, +0x5C
  sans fin. vt 0x20 `stop()`.
  - Ponctuel (éclair) : +0x69 a frappé, +0x6C frappe dans.
  - Errant : +0x68 direction, +0x74 cap visé, +0x80 délai avant de tourner.

### Argent
- Faction du joueur : `PlayerInterface` +0x2A0 → `Faction` +0x80 `Ownerships` → argent à +0x88.

## 5. Énumérations

### Ordres permanents (`StandingOrder`)
| Valeur | Ordre |
|---|---|
| 0 / 1 / 2 | courir / trottiner / marcher |
| 3 / 4 | furtif / fin du furtif |
| 5 / 6 / 7 | combat : attaque / défense / esquive |
| 8 / 9 | loin / près |
| 11 | bloquer (défensif) |
| 12 | tenir la position |
| 13 | passif |
| 14 | narguer |
| 15 | poursuivre |
| 16 | vitesse de groupe |
| 17 | distance |

Dans l'interface, les ordres 11 et plus sont des bascules : `setOrderSelectedCharacters` décide
d'après les boutons du joueur **local**. Pour l'ordre d'un client, l'hôte calcule donc l'état voulu
d'après le personnage lui-même (`on = !état actuel`) et appelle `Character::setStandingOrder`
(ordre, on) directement.

### Tâches (`TaskType`, celles que le mod nomme dans le journal)
| Valeur | Tâche |
|---|---|
| 2 | construire |
| 3 | ramasser |
| 4, 5 | attaquer |
| 6 / 7 | dégainer / rengainer |
| 12 | parler |
| 25 | premiers soins |
| 26 | fouiller (`LOOT_TARGET`) |
| 27 / 28 | s'accroupir / se relever |
| 29 | aller à |
| 30 | tenir la position |
| 44 | suivre |
| 54 | se reposer |
| 55 | recruter (centre d'emploi) |
| 57 | réparer un robot |
| 58 | médecin (métier) |
| 60, 61 | premiers soins (robot) |
| 68, 225 | porter quelqu'un |
| 69 | déposer |
| 70 | déposer dans un lit |
| 72 / 73 | ouvrir / fermer une porte |
| 76 | crocheter |
| 77 / 78 | verrouiller / déverrouiller |
| 81, 226 | enfoncer une porte |
| 87 | utiliser une machine (`OPERATE_MACHINERY`) |
| 95 | réparer |
| 96 | démonter |
| 97 | s'entraîner |
| 98, 258 | dormir |
| 99 | mettre quelqu'un au lit |
| 100 | utiliser un lit (`Task_UseBed`) |
| 107 / 108 | entrer dans une cage / mettre en cage |
| 110 | libérer un prisonnier |
| 116, 208, 257 | sortir du lit |
| 118, 119 | commerce (`SHOPPING`, `BUY_SHIT`) |
| 124 | `OPERATE_STORAGE` (métier de PNJ) |
| 126 | parler (au plus proche) |
| 146, 149, 234 | servir une tourelle |
| 152 | machine automatique |
| 221 | faire semblant d'utiliser une machine |
| 228 / 229 | assommer / tuer en furtif |
| 231 | manger des cultures |
| 235, 262, 263 | tirer |
| 244 | prendre de la nourriture |
| 246 | kidnapper |
| 249, 250 | poser une attelle |
| 255 | s'asseoir sur le trône |
| 259 | manger |
| 269 | soigner les jambes |
| 275 | s'asseoir (`SIT_AROUND`) |
| 284 | `LOOT_CONTAINER` (métier de PNJ) |
| 285 | couper une serrure |
| 286 | forcer une serrure |
| 290 | enfoncer un portail |

Tâches par défaut d'un clic droit sur un meuble, selon la recherche :
- `StorageBuilding` : toujours 26 ;
- `UseableStuff` :
  - stockage ou boutique → 26 ;
  - lit → 258 ;
  - mannequin d'entraînement → 97 ;
  - table → 29 ;
  - cage → 108, ou 76 si verrouillée ;
  - le reste → 87, ou 152 sans place d'opérateur.

### Crimes (`CrimeEnum`)
| Valeur | Crime |
|---|---|
| 1 | asservissement |
| 2 | cambriolage |
| 3 | vol (`THEFT`) |
| 4 | meurtre |
| 5 | agression |
| 6 | trahison |
| 7, 9 | terrorisme |
| 8 | contrebande |
| 10 | pillage (`LOOTING`) |
| 11 | intrusion |
| 12 | évasion |
| 13 | recel |
| 14 | vol de cultures |
| 15 | enlèvement |
| 16 | vol d'uniforme |

### Fenêtre de commerce (`TradeWindowType`)
1 commerce avec argent (`TW_MONEY_TRADING`) ; 2 fouille (`TW_LOOTING`) ; 3 automatique (`TW_AUTO`).

### Résultat d'un échange (`TradeResult`)
| Valeur | Sens |
|---|---|
| 0 | OK |
| 1 | hors de portée |
| 2 | pas de place |
| 3 | pas assez d'argent |
| 4 | le marchand n'a pas assez d'argent |
| 5 | ne peut pas le porter |
| 6 | incompatible |
| 7 | verrouillé |
| 8 | voleur repéré |
| 9 | recel repéré |
| 10 | position de l'objet |
| 11 | invalide |
| 12 | « c'est à moi » |
| 13 | cible consciente |
| 14 | contrebande seulement |
| 15 | marchandise illégale |
| 16 | uniformes |
| 17 | contenant non vide |

### Fonction d'un bâtiment (`BuildingFunction`, valeurs connues)
- 2 stockage de ressources (`StorageBuilding`) ;
- **9 boutique (`BF_SHOP`)** ;
- 13 stockage général ;
- 17 chaise ;
- 24 trône.
- Lits, entraînement, tables, lits squelette et équarrissage deviennent aussi des `UseableStuff`.

### Types d'objets (`itemType`, valeurs utilisées)
0 bâtiment ; 1 personnage ; 2 arme ; 7 race ; 0x5B personnage animal.

### Factions, relations et primes (lot B, vérifié par désassemblage)
- `Faction` : +0x78 `FactionRelations*`, +0x240 `GameData*` (identifiant de chaîne de la faction),
  +0x1D0 `notARealFaction`, +0x250 `PlayerInterface*` (non nul : la faction du joueur, aussi à
  `PlayerInterface+0x2A0`). Liste des factions : `GameWorld+0x4A8` → lektor `Faction*`.
- `FactionRelations` : +0x10 `playerRank` (int), +0x14 `globalReputationTrust`, +0x18
  `globalReputationForBadassery` (float), +0x20 `boost::unordered_map<Faction*, RelationData>`
  (nombre de seaux +0x18, taille +0x20, seaux +0x38 de la table ; nœud : suivant +0, empreinte +8,
  clé +0x10, valeur +0x18), +0x60 relation par défaut.
- `RelationData` : `alliance`, `peaceTreaty`, `war`, `coexists` (octets 0 à 3), `relation` +4,
  confiance positive +8, négative +0xC, force perçue +0x10.
- `FactionRelations::getRelationData(Faction*)` `0x6B4C60` (vt 0x50) : trouve **ou crée** l'entrée
  (valeur de départ : la relation par défaut +0x60) et renvoie la `RelationData*`.
  `setRelation` `0x6B4D80`, `affectRelations` (montant `0x6B2EA0`, événement `0x6B2D20`) : déjà
  bloqués chez les clients.
- `BountyManager` = `Character+0xF0` :
  - +0 `boost::unordered_map<Faction*, Bounty>` ; nœud de 0x30 octets : clé +0x10, `amount` +0x18,
    `crimes` +0x1C (un bit par crime), `bountyHasBeenClaimedOnce` +0x20, heure du début +0x28
    (`TimeOfDay`, 8 octets) ;
  - +0x48 faction du laissez-passer, +0x50 son expiration ; +0x58 crime en cours (`CrimeEnum`),
    +0x60 contre quelle faction, +0x70 `hand` de la victime, +0x90 expiration du crime ;
    +0x98 début de la peine de prison (8 octets), +0xA0 heures de prison à faire (float) —
    `notifyStartPrisonSentence` (`0x854950`) écrit ces deux derniers.
- `operator[]` de la table des primes `0x5E7EE0` `(table*, Faction* const&)` : trouve ou crée le nœud
  (`Bounty` construit à 0) et renvoie la paire (clé +0, montant +8, crimes +0xC, réclamée +0x10,
  heure +0x18). C'est ce que `unfairAddToBounty` (`0x853E20`) appelle avant d'ajouter le montant.
- Appelants de `0x5E7EE0` (par le saut `0xA966`) : `unfairAddToBounty`, `assignBountyForCrimes`
  (`0x853F34`) et 5 autres dans `0x854000`-`0x854330`, plus `0x5D118F` et le chargement `0x6260D9` ;
  tous passent `rcx` = le `BountyManager` lui-même (la table est bien à +0). La clé vient de
  `0x852010` : la faction **racine** (on remonte `Faction+0x38` tant qu'il est non nul), d'où une
  prime « Cités Unies » pour un crime contre les Chasseurs de Tech. Après le montant, le jeu écrit
  l'heure (+0x18 de la paire) depuis une table globale, et prévient l'interface (`[bm+0x40]+0x58`,
  le `hand` du perso).
- **Plantage du 10/10** : l'insertion du mod était correcte (même chemin que le jeu). Mais le client
  avait aussi planté au **chargement** de la sauvegarde de l'hôte qui contenait la prime, sans
  aucune écriture du mod : c'est la présence de la prime (et du crime en cours) dans le jeu du client
  qui le fait planter, quand sa police locale agit sur un perso piloté par l'hôte (7 à 17 s d'écart
  entre clients). D'où : primes et crimes jamais écrits chez un client, et vidés s'il en a.
- `getTotalBounty` `0x853740` renvoie en fait la **plus grosse** prime (pas la somme).
- `clearBounty` `0x8539F0` efface la prime de la faction **et** celle de la faction qui tient la
  prison où est le personnage (ou de la ville où il se trouve) : le mod ne l'appelle pas, il met le
  montant à 0.
- Adresses de KenshiLib (1.0.65) pour `BountyManager` : décalage de +0x1590 dans cette zone
  (`setCrime` 0x8516F0 → `0x852C80`).

## 6. Comportements observés

- **Mort d'un perso de l'escouade du joueur** : le jeu le déplace dans une escouade `__DEAD_SQUAD__`.
  Son `hand` change de conteneur (`container`, `containerSerial`) mais garde `type`, `index` et
  `serial` (hôte : `1:1:2712646400:6:3978271488` → `1:4:940063488:6:3978271488`). Chaque jeu le
  fait de son côté : chez un client, la copie change de handle quand il rejoue la mort. Un
  déplacement fait par le mod (`MoveToSquad`) donne en revanche un nouvel `index` et un nouveau
  `serial`.

- `PlayerInterface::addOrderSelectedCharacters` (`0x7F9E20`) parcourt la sélection et appelle
  `Character::addOrder` (`0x5D20D0`) sur chacun, immédiatement ; pour une attaque (tâches 5, 16, 61)
  sur un personnage, il appelle aussi `Character::rememberCharacter(cible, 4)` (`0x6744A0`) sur
  l'attaquant seul. Rien ne passe aux autres membres de l'escouade.
- `OrdersPanel::passiveButtonCallback` (`0x721640`) bascule le bouton **avant** d'appeler
  `setOrderSelectedCharacters(13)` : si cet appel est refusé, le bouton s'affiche activé sans que
  le mode soit posé.
- `setOrderSelectedCharacters` (`0x7F3880`) : `on = ordre >= 10 ? !getStandingOrder(perso principal) : true`,
  le perso principal étant lu dans un `hand` global (`0x2133A70`), puis `setStandingOrder(ordre, on)`
  sur chaque perso de la sélection. Avec une sélection mixte, le sens vient donc du dernier cliqué.
- `addJobSelectedCharacters(task, subject, shift, add, pos)` → `Character::addJob` →
  `OrdersReceiver::addJob` (`0x5086D0`) : `shift` vrai = tâche permanente (liste +0x88/+0x90, panneau
  Tâches), faux = ordre passager (liste +0x70). `add` faux vide d'abord les tâches.
- `PlayerInterface::objectSelected` (`0x7F7F20`) écrit aussi un `hand` global (`0x21345D0`) et
  `PlayerInterface`+0xF0 ; `unselectAll` les remet à zéro.
- **La sélection n'est jamais vide par le jeu** (désassemblage 1.0.68) : `unselectAll` (`0x7F8DA0`)
  vide la sélection puis **resélectionne** un perso : l'objet du `hand` global `0x21345D0` si le
  `hand` du panneau de détails (+0xF0) est dans la sélection, sinon le premier de la sélection (rien
  si elle était vide ; et rien du tout si `PlayerInterface`+0x2A0 est nul : il sort sans rien faire).
  `objectSelected(obj, false)` refuse de retirer le dernier sélectionné (`taille <= 1`).
  `objectSelected(obj, true)` n'ajoute un perso que s'il est de la faction du joueur
  (`owner->+0x250` non nul) et sans le drapeau perso +0x5BC. « unselectAll puis sélectionner X »
  laisse donc aussi le perso principal sélectionné : un ordre « des sélectionnés » part aux deux
  (`addOrderSelectedCharacters`, `newPlayerTaskSelectedCharacters`, qui renvoie vers `addOrder` ou
  `addJob`), `addTaskNearestSelectedCharacter` au plus proche des deux. Pour une sélection exacte :
  unselectAll, sélectionner les acteurs, puis retirer le reste (possible tant qu'un acteur est
  sélectionné). Pour la vider : `hand` global mis à nul (type 0xB, reste 0), le `hand` +0xF0 sur un
  sélectionné, puis unselectAll.

- `UseableStuff` : ensemble des occupants (`std::set<hand>`) à +0x3D0, taille à +0x3E0 (lit libre :
  0). `BuildingFunction` (ordre de l'énumération) : 1 mine, 6 lit, 8 cage, 9 boutique, 12 tourelle,
  18 décor, 27 gisement naturel.
- Un handle de meuble de ville envoyé par un client peut désigner **un autre objet** chez l'hôte :
  toujours vérifier type et position.

**Unités et identifiants**
- 1 unité du monde vaut environ 10 cm. On marche à environ 14 unités/s ; le bassin d'un ragdoll
  est 1 à 2 unités au-dessus du sol.
- Le `hand` d'un personnage change quand il meurt ou change d'escouade (sa partie « container »).
  Il faut suivre l'identité du personnage, pas le handle.
- Meubles, machines et maisons d'une ville ont **un handle différent sur chaque machine**, même
  avec la même sauvegarde. Il faut les retrouver par type et endroit. Les meubles de ville n'ont
  pas de lien vers leur bâtiment (`isFurnitureOf`, +0x238, est vide).

**Mouvement**
- Les PNJ lointains ne sont déplacés que quelques fois par seconde le long de leur chemin. Une
  position écrite sur eux ne tient pas ; les personnages du joueur l'acceptent à chaque image.
- **Pendant la pause, le jeu n'enregistre aucune position écrite** sur un personnage.

**Ragdolls**
- `pickupObject` (0x5CFF90) ne fait rien si le porteur est en ragdoll, porte déjà, ou si le corps a
  son drapeau ragdoll (+0x3d4) ; son étape d'attache (0x5CED90) remet ce drapeau à 1 sur le corps porté
  (un corps porté se lit donc « ragdoll »).
- Téléporter juste avant le début d'un ragdoll projette le corps : la vitesse vient des écarts
  entre les dernières poses.
- Une téléportation ne déplace pas un ragdoll actif.
- Reconstruire un ragdoll le lance en l'air.

**Heure**
- L'heure du jeu (`0x21303D0`, +0xA0) est recalculée à chaque image depuis un compteur interne.
  L'écrire n'a aucun effet durable.

**Santé**
- `MedicalSystem` écrit lui-même ses drapeaux « mort » et « inconscient » avant d'appeler
  `declareDead`.
- `MedicalSystem::knockout` (`0x644980`) ne fait que calculer un minuteur de K.-O. (+0xA0) : il ne
  rend pas inconscient tout de suite.
- La faim, le sommeil et la perte de sang mettent K.-O. sans minuteur.
- `reassessCollapseMode` décide qu'un personnage s'effondre (douleur, membres estropiés).

**IA**
- Les tâches qu'un personnage avait dans la sauvegarde (s'asseoir, patrouiller, errer) tournent
  dans `AITaskSytem` (`AI` +0x20, file d'actions à +0x300, taille à +0x28).
- `Character::reThinkCurrentAIAction` les abandonne proprement.
- Bloquer tout `AITaskSytem::update` empêche aussi la marche vers une destination.

**Animations**
- L'horloge maîtresse est une phase entre 0 et 1, qui reboucle. Les cycles de marche et de course
  la suivent.
- Si on ne choisit pas les animations d'un personnage (`animationSelection` sautée), il faut faire
  avancer cette horloge soi-même.

**Conversations**
- Le jeu prépare les conversations aussi sur des threads de travail. La première réplique peut
  arriver avant l'ouverture de la fenêtre.

**Sauvegarde**
- `SaveManager` efface sa demande avant que tous les fichiers soient écrits : il recopie son
  dossier de travail dans l'emplacement ensuite.
- Il faut attendre que `quick.save` existe et que la taille du dossier ne bouge plus.

**Interface**
- Un glisser-déposer de souris simulé ne marche pas dans l'interface de Kenshi ; les clics et
  clics droits, si (avec un léger mouvement relatif).
- Le jeu lit clavier et souris par DirectInput, via OIS.

## 7. Recherche : commerce, fouille et contenants

Recherche faite le 9 octobre 2026 pour le commerce. Fiabilité de chaque fait :
- **[D]** désassemblé, comportement confirmé ;
- **[V]** emplacement de vtable confirmé par la vtable RTTI ;
- **[P]** début de fonction réel et offset KenshiLib cohérent, comportement non lu ;
- **[U]** déduit de l'usage.

### ForgottenGUI (`0x21337B0`) [D]
| Offset | Champ |
|---|---|
| +0x58 | type de fenêtre de commerce en attente (0 aucune, 1, 2, 3) |
| +0x5C | demande de fermeture |
| +0x60 | `hand` A (son type à +0x68) |
| +0x80 | `hand` B |
| +0xA0 | fenêtres d'inventaire ouvertes : table `hand` → `InventoryGUI*` (fenêtre à +0x20 de la paire renvoyée) |
| +0x1C0 | éditeur de personnage |

### Ouvrir et fermer
- **`showTradeWindow`** `0x791830` `(gui, const hand& a, const hand& b, int type)` [D]
  - Ne fait que **ranger la demande** (+0x58, +0x60, +0x80).
  - Deux appelants seulement :
    - `Task_Loot_Order::startAction`, avec le type 3 ;
    - l'assistant `0x9524F0`.
- **Assistant `0x9524F0`** `(?, Character* x, Character* y, int type)` [D]
  - Appelle `showTradeWindow(&y->hand, &x->hand, type)`.
  - Appelé par `Dialogue::_doActions` (`0x680560`) pour l'action de dialogue `DA_TRADE` (1) :
    d'abord `endDialogue`, puis l'assistant avec `x = Dialogue+0x150` (le propriétaire du
    dialogue) et `y` = l'autre, type 1. Donc `a` = l'autre, `b` = le propriétaire : si le dialogue
    est celui du perso du joueur, le joueur arrive **en second** (fix G5 : le mod regarde les deux
    côtés).
  - L'ordre d'un client et sa réponse au dialogue sont exécutés chez l'hôte dans un appel du mod :
    le détournement de `showTradeWindow` ne doit pas dépendre de « qui appelle ».
- **`closeTradeWindow`** `0x791890` : met +0x5C à 1. Appelé par `Task_Loot_Order::endAction`
  (`0x3466A0`). [D]
- **`ForgottenGUI::update`** `0x6EA070` [D]
  - Si +0x58 est posé : résout les deux handles (`hand::getRootObject` `0x79D410`), appelle
    `_showTradeWindow`, puis remet +0x58 et +0x5C à 0.
  - Si +0x5C est posé : `closeInventory(hand)` (`0x6E53D0`) de chaque côté.
- **`_showTradeWindow`** `0x7918A0` `(gui, RootObject* a, RootObject* b, int type)` [D]
  1. Ne fait rien si la faction de `a` n'est pas celle du joueur. On la reconnaît à
     `Faction`+0x250 : un `PlayerInterface*` non nul.
  2. `closeAllInventories` (`0x6E5740`).
  3. `setTradingTown` avec la ville la plus proche de `a` (`TownList::getNearestTown`
     `0x927F10`).
  4. Fenêtre de A : `showInventory` (`0x6E6820`).
  5. Fenêtre de B :
     - type 2 : `showInventory(b)`, sans paiement ;
     - type 1 : `showTraderInventory(b)` (`0x6E6AF0`), avec paiement ;
     - type 3 : paiement et fenêtre de marchand seulement si `b` est un personnage, ni en ragdoll,
       ni au lit ou en cage (+0x2F8 = 0), ni en train de se relever (+0x278 = 0), ni estropié
       (vt 0x360). Sinon (corps, contenant, bâtiment), chemin de fouille sans paiement.
  6. Enregistre les deux fenêtres comme partenaires d'échange (`addTradePartner`). Un personnage
     mort du joueur n'y compte pas comme joueur.

### Autres fonctions de `ForgottenGUI`
| Fonction | RVA | Fiabilité |
|---|---|---|
| `showInventory` | `0x6E6820` | [U] |
| `showTraderInventory` (obtient ou crée le `ShopTrader` via le gestionnaire `[0x212FA08]` et `0x953F90`) | `0x6E6AF0` | [D] |
| `showInventoryBuilding` (ouvert par `Building::select`) | `0x6E6640` | [U] |
| `closeInventory(hand)` | `0x6E53D0` | [U] |
| `closeAllInventories` (aussi appelé quand un voleur est repéré) | `0x6E5740` | [U] |
| `closeAllWindows` | `0x6E64F0` | [P] |
| `createInventoryWindow(hand, Inventory*, disposition, marchand)` | `0x6E5260` | [U] |
| `getNumOpenInventoryWindows` (`Task_Loot_Order::runAction` finit la tâche s'il vaut 0 pendant 5 ticks) | `0x6E2DF0` | [U] |

### Fenêtres
- **`InventoryGUI`** (vtable `0x1707568`, constructeur `0x716A40`) [D]
  - +0x08 widget ; +0x10 `hand` du propriétaire ; +0x30 disposition ; +0x50 objet de rappel ;
    +0x58 à rafraîchir ; +0x59 visible ; +0x60 sections.
  - vt 0x78 `getCallbackCharacter` = `0x70CFE0`, qui appelle `Inventory::getCallbackCharacter`
    (`0x70CEF0`). Celle-ci renvoie le personnage, le propriétaire d'un contenant, ou, pour un
    bâtiment, le chef de l'escouade résidente (`Building::getResidentSquadLeader` `0x547BA0`).
  - vt 0x80 `getCallbackObject` = `0x70D010`.
- **`InventoryTraderGUI`** (vtable `0x1707618`) [D] : +0x50 `ShopTrader*`. Son inventaire est
  `ShopTrader`+0xC8 (un `ShopTraderInventory`), son personnage de rappel `ShopTrader`+0xC0 (le
  marchand).
- **Table statique des partenaires d'échange** `0x2132BE0` (`std::map<InventoryGUI*, données>` ;
  tête `0x2132BE8`, taille `0x2132BF0`) [D]
  - Données : fenêtre +0x0 ; paiement +0x8 ; peut poser +0x9 ; joueur +0xA ; `hand` du
    propriétaire +0x10.
  - Recherche ou insertion : `0x6F57A0`.

### État de l'échange
| Fonction | RVA | Rôle |
|---|---|---|
| `addTradePartner` | `0x70F2A0` | enregistre une fenêtre [D] |
| `removeTradePartner` | `0x70E540` | [D] |
| `clearTradePartners` | `0x70DC20` | [D] |
| `setTradingTown` / `getTradingTown` | `0x70D130` / `0x70D170` | `hand` statique en `0x1F396A0` [D] |
| `getNPCTrader` | `0x70E2D0` | premier personnage de rappel non joueur parmi les partenaires [D] |
| `isTradingForMoney` | `0x70FF10` | vrai si un côté paie, que les deux côtés ne sont pas joueurs, que les inventaires diffèrent et qu'il y a un objet de rappel [D] |
| `isStealing` | `0x711740` | faux si les deux côtés sont joueurs, si la faction du propriétaire est factice (`Faction`+0x1D0), si source = destination, ou pour un bâtiment de catégorie « CAMPING » ; sinon vrai quand ce n'est pas un échange payant [D] |
| `isWithinRangeToTrade` | `0x70D290` | [P] |
| `canDropMouseItemWithoutPaying` | `0x70F340` | [D] |

### Déplacer un objet dans ces fenêtres
- Clic : `sectionMouseButtonPressed` (`0x716800`) prend l'objet à la souris
  (`pickupItemToMouse` `0x7139A0`, sans aucun mouvement d'argent) ou le pose. [D]
- Relâchement : `sectionMouseButtonReleased` (`0x7169A0`). Bouton gauche → `placeItemFromMouse` ;
  bouton droit → `rightClickAutoEquipping` (`0x714A20`). [D]
- Objet tenu par la souris (`MouseInventory`) : singleton `[0x2132B58]` (lecture `0x6EF0E0`).
  +0x30 `Item*`, +0x40 section, +0x68 fenêtre d'origine. [D]
- **`placeItemFromMouse`** `0x715560` : c'est l'**achat, la vente, le vol ou le transfert**. [D]
  1. Vérifications : portée, contenant non vide, position valide, places limitées, race,
     verrou.
  2. Échange payant :
     - confirmation de recel ;
     - le marchand détecte une marchandise volée (vt 0x190) → recel repéré (9) ;
     - contrôle de contrebande (vt 0x2A8) → marchandise illégale (15).
  3. Vol (`isStealing`) :
     - `setCrime(voleur+0xF0, crime, faction de la victime, hand de la victime)` ;
     - puis `ImStealingDoYouNotice` (vt 0x298) ; si vrai : voleur repéré (8), fermeture de
       toutes les fenêtres, l'objet revient.
  4. Propriétaire de l'objet volé : `notifyTheftFrom` (vt 0x348).
  5. **Prix** :
     - quantité plafonnée par ce que l'acheteur peut payer (vt 0x298 de l'objet) ;
     - `prix = getValueAll(vendeurEstJoueur ≠ (rachat ≠ 0))` (vt 0x290) ;
     - si l'acheteur a assez d'argent (vt 0x1B8) : l'inventaire acheteur paie
       (`takeMoney(prix)`), l'autre côté reçoit (`takeMoney(-prix)`) ;
     - sinon : pas assez d'argent (3, ou 4 pour le marchand).
  6. La monnaie échangée entre joueur et non-joueur est consommée (`MoneyItem::consume`
     `0x75F700`).
  7. Fusion de pile, puis `InventorySection::_addItem`.
- **`RClickAutoTrade`** `0x713D20` : clic droit, une unité par appel, mêmes vérifications. Avec Maj
  enfoncée (`0x2133449`), `RClickAutoTradeAll` (`0x714930`) le répète. Le jeu de base n'a pas de
  bouton « tout prendre ». [D]
- `setItemToPlayerPortrait` `0x712490` : déposer un objet sur un portrait. Même séquence de vol,
  crime 3. [D]

### Argent [D]
- `Inventory::takeMoney` `0x745E30` → `callbackObject->takeMoney` (vt 0x1B0) ; `getMoney`
  `0x745E60` (vt 0x1B8).
- `Character::takeMoney` `0x7965F0` → `getOwnerships()->takeMoney`. `Character::getMoney`
  `0x791660`.
- `Character::getOwnerships` `0x7956A0` :
  - faction du joueur : `[0x2134690]` (`PlayerInterface`) +0x2A0 → `Faction` +0x80 ;
  - sinon : escouade (+0x658) +0x78 → `Platoon`, vt 0x98.
- `ShopTrader::takeMoney` `0x9526C0` et `getMoney` `0x9526E0` passent au marchand (+0xC0).
- `UseableStuff::takeMoney` `0x54E560` et `getMoney` `0x54E5D0` passent au `hand` +0x380, ou à la
  faction.
- `Ownerships` (argent à +0x88) :
  - `takeMoney` (vt 0) `0x7CBAA0` : faux si pas assez d'argent pour un montant positif, sinon
    retire (un montant négatif ajoute) ;
  - `takeMoneyByForce` `0x7CBA90` ; `addMoney` `0x32DF80` ; `setMoney` `0x37DC50` ; `getMoney`
    `0x37DC40`.
- Il n'existe pas de `Character::addMoney` : on utilise `takeMoney(-n)`.

### Valeur et prix [D]
- `getValueSingle(bool joueur)` (vt 0x288, `0x7A8F90`), dans l'ordre :
  1. valeur de base d'après la GameData et le niveau ;
  2. × multiplicateur local de la ville (`Town::getLocalTradePriceMult` `0x92D590`) si une ville
     d'échange est posée ;
  3. si `joueur` et (objet non marchand ou volé) : × `[0x2133EBC]` ;
  4. × charges / charges pleines s'il n'y en a qu'un ;
  5. × `merchantPriceMod`.
- `getValueAll` (vt 0x290, `0x7915B0`) : valeur unitaire × quantité. `getMaxAffordableNum`
  (vt 0x298, `0x75D5C0`).
- `merchantPriceMod` `0x79E6D0` :
  - part du multiplicateur du marchand (`Platoon`+0xE8) quand un échange payant est ouvert ;
  - × une constante si l'objet a été volé à un joueur ;
  - × multiplicateur culturel de la ville du marchand (`0x92BEC0`).
- **Conséquence** : le prix dépend de l'état de l'interface (ville d'échange, table des
  partenaires). L'hôte ne peut pas recalculer exactement le prix vu par un client sans ouvrir
  lui-même ces fenêtres. Le client calcule donc le prix et l'envoie.

### Stock d'un marchand [D, partiel]
- `ShopTrader` :
  - construit par `0x953850` à partir des meubles du bâtiment du marchand (comptoirs
    `UseableStuff` de fonction `BF_SHOP`) et de son « backpack_content » ;
  - contient un `ShopTraderInventory` (vtable `0x1737768`, constructeur `0x953130`) ;
  - `0x953F90` l'obtient ou le crée : il gère un `hand` de type 1 ou 0x5B et alloue 0xD0 octets.
- La fenêtre du marchand est donc **une copie fusionnée** :
  - retirer un objet passe par `_removeItemFromInventories` (`0x952A70`), qui le prend dans les
    vraies sections ;
  - ajouter le copie dans les vrais inventaires (`0x9529E0`).
- Achat par un PNJ : `Inventory::buyItem` `0x74A630` (utilisé par `Task_Shopping`).
- `getSpecialFunction` (vt 0x2F0, `0xF6B30`) lit la fonction du bâtiment à +0x158.
- **D'où vend un marchand** (constructeur `0x953850`, lu en détail) :
  - `Character::getOwnerships(marchand)` (`0x7956A0`) ; à +0x38, le `hand` de son **bâtiment
    domicile** (type à +0x40 ; 0xB = aucun) ;
  - s'il en a un : `Building+0x1F0` (`myInterior`) → `0x54ACB0(intérieur, lektor&)` remplit la liste
    des meubles de boutique (un ensemble de `hand` de l'intérieur, sinon ses meubles) ; pour chacun,
    l'inventaire (vt 0x160) donne une section ;
  - sinon (marchand ambulant : `hand` de type 0xB, ou qui ne se résout pas) : les sacs à dos portés
    par les membres de son escouade, lus ainsi [D] :
    - `0x791B10(marchand)` : `Character+0x658` (`ActivePlatoon*`) ; ses membres sont la
      `lektor<Character*>` en +0x50 (nombre +0x58, données +0x60) ;
    - pour chacun, son inventaire (vt 0x160), puis `Inventory::getSection(type)` `0x745EF0` avec le
      type 12 : parcourt `Inventory+0x68` (`lektor<InventorySection*>`, nombre +0x70, données +0x78)
      et compare `InventorySection+0xB8` au type ;
    - `0x746880` (section vide : `+0x40 == +0x48`) ; sinon le premier `SectionItem` (+0x40 → `Item*`) ;
    - l'inventaire de cet objet (vt 0x160), et **sa première section** (`+0x78[0]`) s'ajoute à la
      fenêtre ;
  - le mod refait la même chose (`kenshi::ShopCounters`, et `kenshi::WornBackpack` /
    `TravellingWearers` pour les sacs) pour savoir quels contenants envoyer.
- **Sacs à dos** [D] : un sac est un `ContainerItem` (vtables `0x170F4F8`, et `0x170F4D8` pour sa
  seconde base). `RootObject::getInventory` est l'emplacement vt 0x160 de tout objet : `Item` le
  laisse à `0xD2280` (renvoie null), `ContainerItem` le remplace par `0x76BE60` (renvoie
  `this+0x290`). vt 0x2C8 vaut `0x76BE50` (renvoie 0x2E) pour un `ContainerItem`. `HandleTable::resolve`
  (`0x2676E0`) résout les `hand` d'objets (types 2 à 0x5A, sauf quelques-uns) par la liste générique
  en +0x50 : le `hand` d'un sac porté se résout comme celui d'un objet au sol.
- `0x953F90` (appelé par `showTraderInventory`) **détruit le `ShopTrader` précédent et en crée un
  neuf** à chaque ouverture : rouvrir la fenêtre suffit à montrer un stock changé.
- `lektor<T*>` du jeu : vtable, `uint32` nombre (+8), `uint32` capacité (+0xC), données (+0x10) ;
  l'agrandissement (`0xD9410`) passe par `operator new` / `delete` du jeu (`0xED6504` / `0xED64FE`) :
  une liste qu'on donne au jeu doit avoir sa mémoire allouée par ces fonctions.
- Table des fenêtres ouvertes (`std::map<InventoryGUI*, InventoryTradeData>` à `0x2132BE0`) :
  tête `0x2132BE8`, taille `0x2132BF0` ; nœud : gauche +0, parent +8, droite +0x10, clé (fenêtre)
  +0x18, données +0x20 (`isPlayer` à +0x2A), `isNil` +0x51. Le mod y retrouve la fenêtre du
  marchand et celle du joueur.
- Objet tenu à la souris : `*(0x2132B58)` +0x30 (`Item*`, null si rien).
- `Character::takeMoney` passe par `Ownerships::takeMoney` (vt 0, `0x7CBAA0`) : refuse si l'argent
  manque et que la somme est positive, sinon `argent -= somme` ; argent à `Ownerships+0x88`.

### Tâches derrière ces fenêtres [D]
- 26 `LOOT_TARGET` → `Task_Loot_Order` (vtable `0x16BE490`) :
  - `startAction` `0x354DC0` appelle `showTradeWindow(moi, sujet, 3)` ;
  - `runAction` `0x354EF0` ; `endAction` `0x3466A0`.
- 119 `BUY_SHIT` → `Task_Shopping`.
- 124 → `Task_FillMachine` ; 284 → `Task_EmptyMachine` : métiers de PNJ, **sans fenêtre**.
- Ouvrir un coffre, c'est **26 avec le meuble pour sujet** : la tâche passe par
  `addTaskNearestSelectedCharacter(maison du coffre, 26, coffre, …)`.

### Bâtiments et meubles [D]
- `RootObjectFactory::createBuilding` (`0x57CC70`) choisit la classe d'après la fonction :
  - stockage de ressources → `StorageBuilding` (vtable `0x16B07A8`) ;
  - lits, entraînement, boutique, stockage général, tables, chaises, trônes… → `UseableStuff`
    (vtable `0x16B21C8`) ;
  - sans fonction → `Building` (vtable `0x16EA4A8`).
- `Building` : +0x158 fonction spéciale ; +0x198 type de classe ; +0x238 meuble de
  (`isFurnitureOf`) ; +0xD0 `hand` de l'escouade résidente ; +0x100 identifiant d'instance ;
  +0x130 identifiant de disposition. On ne sait pas si ces deux identifiants sont les mêmes d'une
  machine à l'autre.
  - vt 0x300 `getUseableStuff`, vt 0x418 tâche par défaut, vt 0x470 / 0x478 / 0x480 marqueurs de
    position et de direction.
- `UseableStuff` : +0x360 propriétaire de boutique ; +0x380 propriétaire de rappel ; +0x3AC nombre
  d'opérateurs maximum ; +0x3D0 ensemble des opérateurs ; +0x400 données de fonction ; +0x408
  animation ; +0x430 `Inventory*` ; +0x438 `DoorLock*`.
  - vt 0x160 `getInventory` : `0xF6CF0`, renvoie +0x430 ; renvoie null pour un simple `Building`.
- Retrouver un objet par `hand` : `HandleManager::getBuilding_UseableStuff` `0x9F9200`, ou
  `ZoneMapHandleContainerList::getObject` `0x9F8F20`.
- Requêtes spatiales : `GameWorld::getObjectsWithinSphere` `0x7861A0` (type 1 → grille +0x10,
  type 0 → grille +0x48, sinon +0x80).

## Portes et serrures (lot A) [D]

Vérifié par désassemblage du 1.0.68 (les décalages de KenshiLib 1.0.65 sont identiques pour
`DoorStuff`) :

| Champ | Décalage | Sens |
|---|---|---|
| `DoorStuff::doorLock` | +0x370 | `DoorLock*` (null : pas de serrure) |
| `DoorStuff::doorOpenAmount` | +0x37C | float, 0 fermée → 1 ouverte |
| `DoorStuff::state` | +0x380 | `DoorState` : 0 fermée, 1 ouverte, 2 en ouverture, 3 en fermeture |
| `DoorStuff::wantsToLock` | +0x384 | bool : se verrouille une fois fermée |
| `DoorStuff::_isBroken` | +0x3EC | bool : défoncée |
| `UseableStuff` serrure | +0x438 | `DoorLock*` des meubles (coffres, cages…) |
| `UseableStuff` cassé | +0x3B6 | bool |
| `DoorLock` niveau | +0x0 | int (0 : pas de vraie serrure) |
| `DoorLock` propriétaire | +0x8 | l'objet qui porte la serrure |
| `DoorLock` verrouillée | +0x20 | bool |
| `DoorLock` serrure cassée | +0x21 | bool |

Slots virtuels de `Building` (valables pour tout bâtiment ou meuble) : 0x308 `isBroken`, 0x310
`setBroken(bool)`, 0x3C8 `doorStuff()` (lui-même pour une porte, null sinon), 0x3E0 `getDoor()`,
0x400 `getDoorLock()`, 0x408 `hasDoorLock()`, 0x418 `getDefaultTask()` (porte : 72 « ouvrir » si
fermée, 73 « fermer » si ouverte).

| Fonction | RVA | Comportement |
|---|---|---|
| `DoorStuff::openDoor` | `0x297040` | si fermée : passe en ouverture (2), son « porte » |
| `DoorStuff::closeDoor` | `0x298C50` | si ouverte et pas cassée : passe en fermeture (3), son |
| `DoorStuff::lockDoor` | `0x2969F0` | verrouille tout de suite si fermée, sinon `wantsToLock` |
| `DoorStuff::unlockDoor` | `0x56A3D0` | `locked = 0`, puis prévient |
| `DoorStuff::openButton` | `0x546FB0` | bouton du panneau : ferme si ouverte, sinon déverrouille si besoin et ouvre |
| `DoorStuff::lockButton` | `0x547060` | bouton du panneau : bascule `wantsToLock` et le verrou |
| `DoorStuff::isLocked` | `0x2EA220` | verrouillée, niveau > 0 et serrure non cassée |
| `DoorStuff::setDoorState` | `0x299000` | écrit l'état et pose aussitôt l'ouverture (chargement) |
| `DoorLock::isBroken` | `0x297D60` | propriétaire cassé, ou +0x21 |
| `CharStats::xpLockpicking` | `0x8C62E0` | expérience de crochetage (traduit de KenshiLib, non lu) |

## 8. Recherche : vol et crimes [D]

- **`BountyManager::setCrime`** `0x852C80` `(voleur+0xF0, crime, faction, hand)` : aucun crime si
  le voleur est garde, est en prison, si la faction est la sienne ou si elle est factice.
- `notifyCrimeWitnessed` `0x852E10`.
- **`Character::ImStealingDoYouNotice`** (vt 0x298, `0x794810`) :
  - chance de réussite = `getStealingSuccessChance` (`0x7944D0`) ;
  - repéré si un tirage aléatoire la dépasse. La victime se lève (ou se réveille si elle dort),
    réagit, le crime est témoigné, et elle se souvient du voleur ;
  - expérience de vol accordée.
- Autres contrôles :
  - `isItOkForMeToLoot` (vt 0x290, `0x793940`) ;
  - `stolenGoodsDetectionCheck` (vt 0x190, `0x793770`) ;
  - `smugglingTradeCheck` (vt 0x2A8, `0x794E80`) ;
  - `sellingUniformDetectionCheck` (vt 0x2A0, `0x7936C0`) ;
  - `Item::notifyTheftFrom` (vt 0x348, `0x792780`) : pose le propriétaire de l'objet, s'il était
    vide ; pour un meuble, c'est l'escouade résidente du bâtiment.
- **Pour que l'hôte rejoue le vol d'un joueur distant** (ce que fait `kenshi::StealCheck`) :
  1. `setCrime(voleur+0xF0, 3, faction de la victime, hand de la victime)` ;
  2. si `ImStealingDoYouNotice(contenant, objet)` est vrai : repéré, l'objet ne bouge pas ;
  3. sinon `notifyTheftFrom(propriétaire de l'inventaire)`, puis on déplace l'objet.
  Les PNJ réagissent ensuite tout seuls, d'après le crime actif (`SensoryData::assessCrimes`).

## 9. Recherche : sièges, lits, cages [D]

- `inSomething` (`Character`+0x2F8) : 0 rien, 1 au lit, 2 en prison ou en cage.
  - Tant qu'il vaut autre chose que 0, `getPosition` renvoie la position du meuble.
- **Chaise ou trône** : `Task_OperateMachine` (vtable `0x16BE898`).
  - Chaque tick d'IA, `runAction` (`0x35BC90`) remet le personnage sur la chaise, le tourne vers
    sa cible et joue l'animation du meuble.
  - `isSitting` (`StateBroadcastData` +0xB8) passe à 1, et rien ne le remet à 0 : valeur peu
    fiable.
- **Lit** : `Task_UseBed` (vtable `0x16C0998`) ; `Character::setBedMode` `0x32E2F0`
  `(c, bool on, UseableStuff* lit)` met `inSomething` à 1 et l'opérateur du lit.
  - Pour se relever : `Task_GetOutOfBed`.
- **Cage** : `setPrisonMode` `0x330600` met `inSomething` à 2 [P].
  - Avec `inSomething` = 2, `animationSelection` joue l'animation de K.-O. du meuble.
- **Occupation d'un meuble** : `tryOperate(hand)` (vt 0x4F8, `0xF8030`) et `stopOperating(hand)`
  (`0x2ACA90`).

## 10. Prisons, cages, chaînes, esclavage (lot D)

Adresses 1.0.68, traduites depuis KenshiLib 1.0.65 (`translate.py`, encadrées par des ancres) puis
lues au désassembleur.

| RVA | Fonction | Ce qu'elle fait |
|---|---|---|
| `0x330600` | `Character::setPrisonMode(bool on, UseableStuff* cage)` [D] | `on` : `inSomething` = 2, `inWhat` = hand de la cage, `tryOperate` (vt 0x4F8) de la cage, place le personnage à la position de la cage (mouvement vt 0xB8), remet `isEscapedPrisoner` (`StateBroadcastData`+0xE8) à 0, choisit un objectif d'IA (4 ou 0x10 selon un drapeau de la cage). `off` (cage nulle acceptée) : `inSomething` = 0, `inWhat` = hand vide (type 0xB), objectifs d'IA d'origine, ordres permanents « tenir » et « passif » réappliqués. |
| `0x32E590` | `Character::setChainedMode(bool on, const hand& propriétaire)` [D] | écrit `isChained` (+0x320), passe l'état d'esclave à 1 (`setSlaveState`), copie le propriétaire dans `slaveOwner` (+0x328) s'il n'est pas vide (type 1 ou 0x22 : aussi la faction de l'esclave à `StateBroadcastData`+0xE0), puis **cherche des menottes dans l'inventaire (fonction d'objet 9) et en crée si besoin** : à ne pas appeler chez un client (l'inventaire vient de l'hôte). |
| `0x32DF60` | `Character::isChainedMode` | lit l'octet +0x320 |
| `0x5C8A10` | `Character::getChainedModeShackles` | l'objet de fonction 9 (sinon 5) de l'inventaire |
| `0x5C8AA0` | `Character::isSlave` | `getStateBroadcast()` (vt 0x70) → premier entier |
| `0x32E430` | `Character::changeSlaveOwner(const hand&)` | — |
| `0x5A4940` | `StateBroadcastData::setSlaveState(SlaveStateEnum)` [D] | passage à « esclave » d'un perso du joueur : message ; état 2 : minuteur ; prévient le personnage (`0x34C07`) ; écrit l'état à +0 |

Champs (`Character`) :
- +0x2F8 `inSomething` (0 rien, 1 lit, 2 prison / cage), +0x300 `inWhat` (hand du meuble) ;
- +0x320 `isChained` (octet), +0x328 `slaveOwner` (hand ; type 0xB = personne) ;
- +0x1A0 `StateBroadcastData*` : +0 `_slaveState` (0 non, 1 esclave, 2 en fuite, 3 ancien
  esclave), +0x8 date du changement, +0xE0 `isSlaveOf` (`Faction*`), +0xE8 `isEscapedPrisoner`,
  +0xE9 `isKidnapped` ;
- +0xF0 `BountyManager` (intégré) : +0x98 début de la peine de prison (`TimeOfDay`, 8 octets),
  +0xA0 heures à purger [U : déduit des champs voisins vérifiés +0x58, +0x70, +0x90].
- Fonction de bâtiment `BF_CAGE` = 8 (`getSpecialFunction`, vt 0x2F0).

## 10. Combat à distance (lot C) [D]

Adresses de la version 1.0.68 vérifiées par désassemblage. KenshiLib 1.0.65 décale cette zone de
+0x780.

**Fonctions**

| RVA | Fonction | Usage |
|---|---|---|
| `0x43A730` | `GunClass::shoot(Character* me, RootObject* cible, StatsEnumerated stat, const Vector3& visée)` | tire un projectile ; un seul appelant (`0x43AFAC`, dans la mise à jour de `RangedCombatClass`) ; hooké |
| `0x43A2F0` | réserve des projectiles (`*(0x212F1E0)`) : `get(mesh, matériau)` | renvoie un projectile de 0x90 octets ; appelé seulement par `shoot` ; hooké pour attraper le projectile d'un tir |
| `0x43AFE0` | mise à jour de la réserve (depuis `mainLoop_GPUSensitiveStuff`) | fait voler chaque projectile (`0x4380E0`) |
| `0x440260` | `TurretBuilding::aimAt(const Vector3&)` | écrit le point visé à `TurretBuilding+0x4A8` |
| `0x435100` | `GunClassTurret::aimAt` (vt 0x28) | écrit `tourelle+0x4A8` et `canon+0x118` ; `GunClass::aimAt` des armes personnelles (`0x4350F0`) ne fait rien |
| `0x4366B0` | `Character::isInRangedCombatMode` | lit `Character+0x2F0` → `+0x36` |

**Ce que fait `shoot`**
1. Il prend un projectile dans la réserve avec le mesh de munition de l'arme (`GunClass+0xB8`).
2. Il copie dans le projectile le `hand` de la cible (+0x8…) et celui du tireur (+0x28…).
3. Il place le nœud Ogre du projectile (+0x58) au bout de l'arme (vt 0x48 `getBarrelPos`), puis
   l'oriente avec `setDirection` vers `getAimDir(visée)` (vt 0x60), dévié au hasard
   (`Vector3::randomDeviant`, angle tiré de la compétence et de `GunClass+0x10`).
4. Il remplit le projectile : +0x40 portée, +0x44 vitesse, +0x48, +0x4C dégâts, +0x70 état = 1
   (en vol). Il décrémente `GunClass+0x20` (coups chargés) et joue le son.

**Projectile** (sans RTTI ; KenshiLib l'appelle `Harpoon`) :
- +0x58 nœud Ogre : le projectile avance le long de **son orientation** (`Node::translate` local) ;
- +0x68 objet de trace : la trace entre l'ancienne et la nouvelle position détecte ce qui est
  touché ;
- +0x70 état : 0 fini, 1 en vol, 2 planté dans un personnage, 3 planté ailleurs.

Changer l'orientation du nœud juste après `shoot` change donc toute la trajectoire. Le mod lit et
écrit cette orientation avec les exports d'`OgreMain_x64.dll` :
- `?getOrientation@Node@Ogre@@QEBA?AVQuaternion@2@XZ` (retour par pointeur caché) ;
- `?setOrientation@Node@Ogre@@QEAAXMMMM@Z` (w, x, y, z).

**Structures**
- `Character+0x2F0` : `RangedCombatClass*`. Champs : +0x0 état (`RangedState`), +0x28
  `GunClass*`, +0x30 compétence, +0x35 « tirer maintenant », +0x36 mode combat à distance, +0x38
  point visé, +0x48 `hand` de la cible, +0x68 le personnage, +0x70 minuterie de visée.
- `GunClass` : +0x8 portée, +0x10 déviation de base, +0x18 vitesse du tir, +0x1C/+0x20 coups
  max/chargés, +0x58 type de munition, +0x68 `GameData` de l'arme, +0xB8 mesh de munition.
- `GunClassTurret+0x128` : son `TurretBuilding`.
- `TurretBuilding` : +0x4A8 point visé. Le pointeur vers son `GunClassTurret` est dans
  +0x400…+0x520 (+0x440 dans KenshiLib) : le mod le cherche une fois, par la vtable et le
  pointeur retour.
- Vtables : `TurretBuilding` `0x16D3F78`, `GunClassTurret` `0x16D5258`, `GunClassPersonal`
  `0x16D5138`.
- Tâches liées : `Task_RangedAttack`, `Task_UseTurret` (RTTI) ; `MAN_A_TURRET`, `USE_TURRET`,
  `SHOOT_AT_TARGET`, `RANGED_ATTACK…` dans `TaskType`.
## 10. Recherche : bâtiments, construction, achat (lot E) [D]
- **Mode construction** : les aperçus (`PreviewBuilding`, vtable `0x16E0C98`, 0x130 octets, Ogre
  allocator ; sous-classes `_Resource` `0x16E0E18`, `_Ceiling` `0x16E1B68`, `_Snapping`
  `0x16E1CB8`, `_Wall` `0x16D2EB8`) se rangent dans un groupe statique (`*(0x212DE48)`, 0xB0
  octets, constructeur `0x4D4190`, lektor des aperçus à +8/+0x10).
  - aperçu : +0x8 nœud Ogre, +0x50 bâtiment auquel il s'accroche, +0x90 ville, +0x98 étage,
    +0x9C « dehors », +0xB8 bâtiment construit, +0xD0 parent (meuble de), +0xD8 bâtiment où il se
    trouve, +0xF8 GameData, +0x10C position, +0x118 rotation (Ogre : w, x, y, z) ;
  - `placeFinalPreviewBuilding` (vt 0xC0, `0x4D5130`) range l'aperçu dans le groupe ;
    `placePreview` (vt 0xF0, `0x4D2770`) écrit position, rotation, étage ;
  - `0x4D72A0(groupe)` construit tous les aperçus du groupe : position finale = nœud Ogre + décalage
    du monde, hauteur moins `getTerrainWithWaterHeight` (ou relative au parent pour un meuble),
    puis `createBuilding`, `setupMiningResourceLevel` (vt 0x2D8), `clearUsageNodes` (`0x54C4D0`),
    enregistrement auprès de la ville et sélection du dernier bâtiment ; à la fin
    `0x4D3750(groupe, aperçu)` retire chaque aperçu, `0x4D33A0(groupe)` vide le groupe.
  - « is node » (GameData) : passe par `RootObjectFactory::create` (`0x583400`), pas par
    `createBuilding`.
  - échec de la fabrique (null) : message « Error: Failed to build … » via `0x6E1E00`, qui est un
    simple `ret` dans cette version.
- **`RootObjectFactory::createBuilding`** `0x57CC70` : (fabrique, GameData*, Vector3* position,
  TownBase*, Faction*, Quaternion* rotation, rappel, Layout* meubleDe, Building* porteDe, état de
  sauvegarde, Building* àLIntérieurDe, invisible, terminé, feuillage, étage, meubleExtérieur) ; en
  mode construction le rappel est le `PlayerInterface` (ou, accroché, un objet de 0x18 octets
  {vtable `0x16DFB00`, PlayerInterface, bâtiment accroché} alloué par `0xED650A`, libéré par
  `0xED64F8`).
- **Plans** : `Building+0x1F0` intérieur, intérieur+0x30 son `Layout` ; `Layout+0x90` le bâtiment ;
  `Building+0x238` le plan dont il est un meuble.
- **Chantier** : `ConstructionState` en ligne à `Building+0x160` (`getBuildState`, vt 0x228, rend
  `this+0x160`) : +0 terminé, +1 en pause, +2 démonté, +4 avancement, +0x28 total.
  `setConstructionProgress` (vt 0x238, `0x559AD0`) termine le bâtiment (vt 0x240
  `notifyConstructionComplete`) quand l'avancement atteint le total. `addConstructionProgress`
  `0x5595A0`, `addDismantleProgress` `0x2A2860`.
- **Achat** : `buyMeAsk` (vt 0x280) ouvre la confirmation ; `buyMeCallback(int)` `0x7AD6C0`
  achète si la réponse vaut 2 (prix `calculateSaleValue` `0x7AD300`, pris aux cats de la faction du
  joueur) ; `isForSale` vt 0x2C0. Démontage : `confirmDismantle(int)` `0x54FEA0` (2 = oui).
- `getFaction` est le slot 0x58 de tout `RootObjectBase` ; un bâtiment a un handle de type 0.

### Validité d'une pose (mode construction) [D]
Lu dans le 1.0.68 (désassemblage), pour que l'hôte refuse ce que le mode construction refuserait.
- **Qui décide** : le clic gauche du mode construction (`0x4E27E0`, appelé chaque image avec
  l'aperçu courant) appelle `buildingPlacementUpdate` (vt 0x50, `0x4D5420`), qui remet `slopeOK`
  (+0x8C) et `floorOk` (+0x8A) à 1, met `slopeOK` à 0 si la normale du sol touché par la souris a
  y < 0,2 (sur un objet : angle > 5), puis `placementVerification` (vt 0xC8, `0x4DD240`, un saut
  vers `placementVerification_recurse` `0x4DBA40`). Le clic vérifie ensuite, dans cet ordre, et
  affiche le message du jeu au premier échec : ville (+0x90 non nul) « Can't build too close to
  another town. » ; `checkProspectingIsNotZero` (vt 0x10 : culture, mine) ; `indoorsOK` (+0x8B)
  « Must be placed outside or on a roof. » / « Must be placed inside a building. » / « Cannot
  build inside incomplete buildings. » ; `snappingOk` (vt 0x28) ; `isCollisionOK` (vt 0x60, +0x88)
  « Too close to another building. » ; `charactersOK` (+0x89) ; `isBlockingBuildingsNodes` (vt
  0x80) ; `isOnValidGround` (vt 0x90, +0x8F) « The building has one or more parts on invalid
  ground. » ; `isFloorOk` (vt 0x68) ; `isGoodAboveAndBelow` (vt 0x88) et `slopeOK` « Ground needs
  to be more level. ». Tout passe : `placeFinalPreviewBuilding` (vt 0xC0), et seuls ces aperçus
  arrivent à `createBuildings`. **Un client n'envoie donc que des poses que son jeu a acceptées.**
- **Eau et acide** : la surface est à 100 partout (`getTerrainWithWaterHeight` = max(sol, 100) ;
  `UtilityT::getPositionInWater(x, z)` `0x9B3770` = sol ≤ 100). Une empreinte de type LAND
  (`BuildingPlacementGroundType` 1, le défaut ; entier « ground type » de la GameData de la pièce,
  constructeur `Footprint` `0x4D9530`) est invalide sous 98 (`Footprint::isGroundValid` `0x4D13D0`,
  constante `0x16DFAF0`). Quand la souris touche l'eau (groupe 2), la hauteur prise est celle du
  sol dessous (`getTerrainHeightFromRenderer` `0x9B1F80`), d'où le refus.
- **Ville** (`placementVerification`, `0x4DBAEF`–`0x4DBD86`) : `getNearestTown(pos, 0, 0, 0, 10)` ;
  « près » si `withinBordersRange(pos, 2,5)` (1,0 pour les types 4 et 9) ou à moins de 2000 du
  centre (+0x48 / +0x50) ; une ville dont le rayon (vt 0x2A0 × 0,9, ou × 0,6) contient la pose
  (`getNearestWithinItsRadius(pos, true)`, `isTown` vt 0x268) la remplace ; ignorée si sa faction
  (vt 0x58) est celle du joueur (`Faction+0x250` `isPlayer`) ; sauf pour un bâtiment « creates
  player town », il faut en plus `withinBordersRange(pos, 1,0)`. `TownList*` à `*(0x2134100)`.
- **Position** : `createBuildings` donne à la fabrique x, z du monde et y moins
  `getTerrainWithWaterHeight(x, z)` (`0x4D7512`).
- **Ce que l'hôte refait** (`KenshiWorld::CheckPlacement`, `plugin/buildings.cpp`) sans aperçu,
  pour un bâtiment (pas un meuble) : ville (même calcul), intérieur (`isIndoors` au point de la
  pose, sauf posé dans un bâtiment), un autre bâtiment dont l'origine est à moins de 10 (1 m ; le
  vrai test de collision des empreintes demande l'aperçu), sol LAND sous 98 au point de la pose,
  pente (normale du terrain à ±2 sous 0,2). Pas refait : collision des empreintes, personnages
  dans le passage, nœuds d'usage, étage, prospection, aimantation. Terrain inconnu chez l'hôte
  (-99) : refusé (« réessaie »).
