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
| 20 | `FnWorldDestroy` | `GameWorld::destroy(RootObject*)` | `0x799AF0` | oui (lot E) | bool GameWorld::destroy(RootObject*, bool justUnloaded, const char* debugInfo) ; détourné chez l'hôte pour voir disparaître les bâtiments suivis ; et (0.3.1), des deux côtés, pour chaque perso détruit ou déchargé avec sa zone (`justUnloaded`) : son adresse est retirée de tout l'état du mod (`KenshiWorld::PurgeDestroyed`), et le client sait pourquoi une doublure a disparu |
| 21 | `FnEndCombatMode` | `Character::endCombatMode` | `0x5C91C0` | — | void Character::endCombatMode() |
| 22 | `FnRagdollMode` | `Character::ragdollMode` | `0x5CBD60` | oui | void Character::ragdollMode(bool on, RagdollPart::Enum part) |
| 23 | `FnRegionUpdateBT` | `WeatherRegion::updateBT` | `0x9DDE50` | oui | void WeatherRegion::updateBT() (thread d'arrière-plan : fait avancer la météo) |
| 24 | `FnSeasonGetNewWeather` | `Season::getNewWeather` | `0x9DD980` | oui | void Season::getNewWeather() (tirage de la météo suivante) |
| 25 | `FnInstanceSetupWeather` | `WeatherInstance::setupWeather` | `0x9DCF60` | — | void WeatherInstance::setupWeather(Weather*) |
| 26 | `FnCreateItem` | `RootObjectFactory::createItem` | `0x580750` | — | Item* RootObjectFactory::createItem(GameData*, const hand&, GameData* company, GameData* material, int level, Faction*) |
| 27 | `FnShowTradeWindow` | `ForgottenGUI::showTradeWindow` | `0x791830` | — | void ForgottenGUI::showTradeWindow(const hand& a, const hand& b, TradeWindowType) (différé) |
| 28 | `FnIsRagdoll` | `Character::isRagdoll` | `0x7D1440` | — | bool Character::isRagdoll() const (au sol en ragdoll, ou porté) |
| 29 | `FnRecruit` | `PlayerInterface::recruit` | `0x692820` | oui | bool PlayerInterface::recruit(Character*, bool editor) (éditeur : la ligne de dialogue « rejoindre avec édition », action 3 de `_doActions` `0x683775`) |
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
| 94 | `FnShowCharacterEditor` | `ForgottenGUI::showCharacterEditor` | `0x6E32F0` | hôte (0.3.4) | void ForgottenGUI::showCharacterEditor(lektor<Character*>, CharacterEditMode, const vector<GameDataReference>* races). Détourné (`hk_showEditor`) : rien n'est ouvert dans un `NoEditorScope` (vente d'animaux à un autre joueur, voir « Boutique d'animaux ») ; la liste reçue par valeur est alors libérée comme l'éditeur le fait (`delete` du jeu sur son tableau) |
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
| — | `FnSetInBed` | `Character::setInBed(bool, UseableStuff* lit)` (notre nom) | `0x32E2F0` | — | [D, faraway] inSomething (+0x2F8) = 1, inWhat (+0x300) = le `hand` du lit (+0x58), le lit le prend (vt 0x4F8) ; déjà dans ce lit : rien. Le client met la copie d'un dormeur dans le même lit (`ApplyCaptive`) |
| — | `FnItemIsStackable` | `Item::isStackable(InventorySection*)` | `0x75E090` | — | [D, v0.3.2] maximum d'une case pour cet objet dans cette section (1 : ne s'empile pas) ; règle de fusion des piles des déplacements et achats rejoués par l'hôte |
| — | `FnSectionRemoveItem` | `InventorySection::removeItem(Item*)` | `0x749A20` | — | [D, v0.3.2] retire l'objet de la section puis de la liste de son inventaire (+0xC8) ; réparation des sacs abîmés (`CheckInventories` en mode réparation) |
| — | `FnBuildModeInstance` | `PlayerBuildMode` : l'instance (notre nom) | `0x296D10` | — | [D, 0.3.2] créée au premier appel, gardée en `0x212DE48` ; la touche B (`0x7884D9` : `isActive` ? `deactivate` : `activate(false)`) et le bouton de la barre (`0x720F80`) s'en servent |
| — | `FnBuildModeActivate` | `PlayerBuildMode::activate(bool éditeur de niveau)` (notre nom) | `0x4E65E0` | oui (hôte) | [D, 0.3.2] appelle d'abord `GameWorld::togglePause(true)` (`0x4E6608`), puis crée la `BuildModeWindow` (+0x28). Détour `hk_buildActivate` : chez l'hôte avec d'autres joueurs, `togglePause(false)` juste après (A_FAIRE 3n) |
| — | `FnBuildModeDeactivate` | `PlayerBuildMode::deactivate()` (notre nom) | `0x4D1870` | — | [D] détruit la fenêtre +0x28 puis `togglePause(false)` (`0x4D18CE`) |
| — | `FnBuildModeIsActive` | `PlayerBuildMode::isActive()` (notre nom) | `0x4D1860` | — | [D] +0x28 non nul |
| — | `FnStopTrackCharacter` | `PlayerInterface::stopTrackCharacter` | `0x7F2780` | — | [D, 0.3.2] +0x270 = aucun, +0x290 = -1, `Camera::stopFollowing` ; appelé par `CameraClass::move` (`0x6AE5D1`) et `CameraClass::teleport` (`0x6AF934`) : déplacer la caméra lâche le perso suivi |
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
| diplomatie | `FnUniqueMapIndex` | `operator[]` de la table de `UniqueNPCManager` | `0x349950` | — | paire `<GameData* const, UniqueCharacterState>` créée si absente (comme au chargement `0x9AA270`) |
| diplomatie | `FnTownSetOverride` | `TownBase::setOverride(GameData*)` | `0x9FE7A0` | — | applique une variante de ville (voir « Diplomatie ») |
| diplomatie | `FnTownSetFaction` | `TownBase::setFaction(Faction*, bool)` | `0x9287D0` | — | vt 0xA0 de `Town` / `TownBase` : change la faction d'une ville |
| carte | `FnMapWorldToCoords` | `MapScreen::worldToMapCoords(const Vector3&)` | `0x48C3E0` | — | projection de la carte (voir § Carte) ; appelée seulement par la commande de test `mapproj` |
| carte | `FnMapMarkerColor` | `MapScreen::getMarkerColor(RootObjectBase*)` (statique) | `0x48F320` | — | couleur du point du jeu : allié / neutre / ennemi / joueur ; l'hôte s'en sert pour « hostile » |
| carte | `FnWarCurrentCampaign` | `FactionWarMgr::getCurrentCampaign(Platoon*)` | `0x283500` | — | la campagne (raid, vague d'attaque, visite) d'une escouade, ou nul ; recherche dans `forces` (+0x28), sans insertion |
| carte | `FnPortraitCellUpdate` | `PortraitMainCellView::update(const IBDrawItemInfo&, PortraitData*)` | `0x415150` | oui | un portrait de la barre d'escouade est (re)dessiné : le mod retient la cellule |
| carte | `FnPortraitCellDtor` | `PortraitMainCellView::~PortraitMainCellView()` | `0x426450` | oui | destructeur complet (le destructeur virtuel `0x4264E0` l'appelle) : le mod oublie la cellule |
| noms (0.3.4) | `FnNameTagUpdate` | `CharacterNameTag::<update>()` (notre nom, vt 1) | `0x6E8100` | oui | [D] chaque image, pour chaque nom au-dessus d'une tête : le place, et quand +0x18 est mis donne sa couleur et son texte à `NameText` ; le mod remet ensuite la couleur du joueur (voir « Noms au-dessus des têtes ») |
| TP (0.3.4) | `FnAnimSetPosition` | `AnimationClass::<set position and direction>(const Vector3&, const Vector3&)` (notre nom) | `0x5B18F0` | non | écrit l'ancre du nœud de scène du corps (+0x258), sur laquelle la mise à jour de l'animation remet le nœud à chaque image ; `kenshi::PlaceBodyNode` l'appelle avec une direction nulle (voir « Ragdolls ») |
| tâches (0.3.4) | `FnGuiRefreshJobs` | `ForgottenGUI::<refresh jobs panel>()` (notre nom) | `0x6E2050` | non | [D] sur l'objet GUI `0x21337B0` : `0x729B70(*(+0x10) MainBarGUI, &(+0x2C0))`, le panneau Tâches reconstruit pour le perso affiché (hand `0x2133A70`) avec les boutons de la barre ; ce que la fin de `newPlayerTaskSelectedCharacters` appelle (voir « Panneau Tâches ») |
| barre | `FnMainBarUpdateCurrent` | `MainBarGUI::updateCurrentPlatoon()` | `0x415880` | non | remplit à nouveau l'onglet de l'escouade affichée, après qu'un onglet fautif a été vidé (`CheckSquadBar`) |
| sol | `FnInventoryDropItem` | `Inventory::dropItem(Item*)` | `0x745D90` | oui | ce que fait une fenêtre d'inventaire d'un objet lâché sur le monde ; vt 0x38 de `Inventory` et `ShopTraderInventory` |
| sol | `FnDropItemAnimal` | `CharacterAnimal::dropItem(RootObject*)` | `0x5CA4A0` | oui | même code que `CharacterHuman::dropItem`, pour les bêtes de somme |
| dialogue (162) | `FnDialogueEndDialogue` | `Dialogue::endDialogue(bool definitelyTheEnd)` | `0x674830` | — | termine la conversation (voir « Dialogue ») ; le mod l'appelle quand le joueur part, quitte la partie, ou que la conversation est finie sans fermeture (combat, TP, K.-O.) |
| escouade | `FnSquadSwapCharacters` | `ActivePlatoon::swapCharacters(int a, int b)` | `0x792E60` | oui | [D] échange deux cases du tableau des membres (+0x60, nombre +0x58), renumérote les cases entre les deux (`Character::_setPlatoon`), la case 0 devient chef (`setSquadLeader`) ; appelée seulement par `SquadManagementScreen::notifyEndDropPortrait` (`0x49B050`) |
| escouade | `FnChangePlatoonIndex` | `Faction::changePlatoonIndex(Platoon*, int index)` | `0x7F3440` | — | [D] retire le `Platoon` de `Faction::activePlatoons` (+0x208 : nombre +0x210, tableau +0x218), le réinsère à `index` (index < 0 : à la fin), renumérote `Platoon`+0x1F2 ; appelée par `notifyEndDropSquad` (`0x493CB0`) : l'ordre des escouades de la fenêtre |
| escouade | `FnDestroyPlatoon` | `Faction::destroyPlatoon(Platoon*)` | `0x6BA9D0` | — | [D, partiel] retire l'escouade de la faction ; `SquadManagementScreen::removeSquad` (`0x491D20`) ne l'appelle que pour une escouade vide et pas la dernière (sinon une boîte de message) |
| escouade | `FnSquadSetName` | `ActivePlatoon::setName(const std::string&)` | `0x4BE480` | — | [D] affecte `Platoon` (+0x78) +0x18 ; sans appelant direct dans 1.0.68 : la case de nom de la fenêtre (`onNameChanged` `0x48DF00`) écrit le même champ à chaque frappe |
| tâches | `FnCharGetPermajobData` | `Character::getPermajobData(int slot) const` | `0x5C8F10` | — | [D] `Character::ai` (+0x650) → `orders` (+0x20) → `OrdersReceiver::getPermajobData` : le `Tasker*` de la tâche (sujet `hand` +0x10, lieu +0x58) |
| dégâts | `FnMedAddWound` | `MedicalSystem::addWound(this, bool, int, ptr, 4 arguments sur la pile)` (signature lue au désassemblage : `dl`, `r8d`, `r9`, `[rsp+0x28..0x40]`, retour `rax`) | `0x6508D0` | hôte | [D, 0.3.1] crée le chiffre de dégâts (`createScreenLabel` en `0x651E46`) seulement si l'option des chiffres (`[0x2133AE0]`), l'interface (`0x7F5710`) et `Character+0x1A9` (vu par la caméra) le permettent. Détour `hk_addWound` : pour un perso près d'un perso d'un joueur client, +0x1A9 mis à 1 le temps de l'appel puis remis à 0 ; rien d'autre de la fonction ne le lit |
| vue | `FnCharUpdateView` | `Character::updateView()` (notre nom ; rend l'octet +0xE4) | `0x5C9F60` | hôte et client | [D] recalcule à chaque image +0x1A9 « dans la vue » (voir `Character`) ; appelé pour chaque perso par la boucle `0x787AF9` / `0x787B44`. Détour `hk_charUpdateView` : après le calcul du jeu, met +0x1A9 à 1 pour les PNJ près des persos d'un client (hôte) et pour les persos pilotés par l'hôte (client) : ils prennent la mise à jour de déplacement complète, comme les persos des joueurs |
| heure | `FnClockSetHourOfDay` | `GameClock::setHourOfDay(float)` (notre nom) | `0x66CF30` | — | [D] l'objet de `0x21303D0` : écrit l'heure du jour (0..24) dans l'horloge du ciel (+0x20 → +0x1C), appelle sa vt 0x8 (mise à jour, 0 s), recalcule les heures (+0xA0 = jour × 24 + heure du jour) et heure / minute (+0x0 / +0x4). Une nouvelle partie l'appelle avec 0 puis écrit le jour 1 (+0x8, `0x36CFB4`) ; le mod fait de même pour recaler l'horloge d'un client (`kenshi::SetGameHours`) |

| validité | `FnTerrainHeight` | `UtilityT::getTerrainHeight(float x, float z)` | `0x9B3710` | — | le sol sous l'eau éventuelle ; -99 où aucun terrain n'est connu (saut vers `getTerrainHeightFast(x, z, nullptr)` `0x9B32F0`) |
| validité | `FnTerrainWithWaterHeight` | `UtilityT::getTerrainWithWaterHeight(float x, float z)` | `0x9B3720` | — | max(sol, 100) : la hauteur que `createBuildings` retire à la position (appel vérifié à `0x4D7512` : `y -= hauteur(x, z)` en `0x4D751C`). La hauteur d'une pose (`BuildPlace::pos.y`, journal « build mode placement ») est donc **relative au sol** : -13,6 chez rob = 268,1 dans le monde chez l'hôte (A_FAIRE 3o, pas un bug) |
| validité | `FnIsIndoors` | `UtilityT::isIndoors(const Vector3&)` | `0x9B2BA0` | — | le bâtiment dont l'intérieur contient ce point (rayons vers le haut et le bas, groupe 0x2000) ; appelée par `isIndoorsOK` et le clic du mode construction |
| validité | `FnGetNearestTown` | `TownList::getNearestTown(pos, owner, except, mine, TownType)` | `0x927F10` | — | appelée par `placementVerification` (`0x4DBAF6`, type 10) |
| validité | `FnWithinBordersRange` | `TownBase::withinBordersRange(pos, mult) const` | `0x926D50` | — | (rayon vt 0x2A0 × mult)² > distance² au sol ; faux pour le type 8 |
| validité | `FnGetNearestWithinItsRadius` | `TownList::getNearestWithinItsRadius(pos, skipPlayerTowns) const` | `0x928890` | — | appelée par `placementVerification` (`0x4DBBBE`, avec `true`) |

Les six adresses : traduites de KenshiLib 1.0.65 (décalage +0x780 / +0x17B0 dans ces zones) puis
confirmées dans le 1.0.68 comme cibles des appels lus dans `placementVerification`, `createBuildings`
et le clic du mode construction ; prologues relevés dans `kenshi_x64.exe` et vérifiés au démarrage.

### Atelier : recherche, établis, machines, énergie (`plugin/workshop.cpp`)

| Groupe | Enum | Fonction | RVA | Hook | Rôle |
|---|---|---|---|---|---|
| recherche | `FnResearchStart` | `Research::startResearch(GameData*)` | `0x8348A0` | oui | niveau du banc (`GameData` « level » contre `Research+0x170`), `payCosts`, retrait de la liste « disponible », `isInQueue`, ajout en fin de file ; seul appelant : le bouton « ajouter » de la fenêtre Recherche (`0x4976C0`), qui ignore le résultat |
| recherche | `FnResearchStop` | `Research::stopResearch(GameData*)` | `0x830DE0` | oui | retire de la file, remet dans la liste disponible, rien n'est rendu ; appelée par les boutons « retirer » (`0x49A930`, `0x49AA50`) avec `*(0x2134690)+0x38`, et par `refreshResearchList` (retour `0x49A5B0`) pour chaque technologie en file dont `checkRequirements_andQueue` échoue : chez un client ce nettoyage est ignoré (la file est celle de l'hôte) |
| recherche | `FnResearchComplete` | `Research::completeResearch(GameData*)` | `0x834550` | — | `upgradeShit`, **dépile l'avant de la file** si une entrée correspond (quelle que soit sa place), ajoute à l'ensemble `finished` (+0xF0), `_updateTheAllList`, rafraîchit la fenêtre, message « Research complete: {1} » ; ne consomme rien |
| recherche | `FnResearchPayCosts` | `Research::payCosts(GameData*)` | `0x8343C0` | oui | `canPayCosts` (`0x832E50`), `deleteAllArtifactsFor` (`0x832F20`), message « Used artifacts for '{1}' research. » ; seul appelant : `startResearch` |
| recherche | `FnResearchProgress` | `Research::progressResearch(float)` | `0x836B70` | oui | ajoute au `progress` (+8) du premier `ResearchItem`, `completeResearch` quand il suffit ; appelée par `0x7B65B0` (le travail des chercheurs, rangé par `ResearchBuilding::operate` dans une liste globale `0x21346D8`) |
| recherche | `FnResearchIsInQueue` | `Research::isInQueue(GameData*)` | `0x82EAA0` | — | parcourt la file (+0x38) |
| recherche | `FnResearchCheckRequirements` | `Research::checkRequirements(GameData*, bool twoLevels, bool checkCost)` | `0x832FA0` | — | tests seulement (`researchpick`) |
| recherche | `FnLearnResearch` | `Item::learnResearch()` | `0x2B65D0` | oui | un plan lu (« Failed to learn from blueprint… », « Research already known ») : `checkRequirements` puis `completeResearch` de sa technologie ; l'appelant (`0x714A20`, clic droit dans l'inventaire sur un objet de type 0x15) retire ensuite le plan par `Inventory` vt 0x30 `removeItemAutoDestroy(item, 1)` |
| établi | `FnCraftAdd` | `CraftingBuilding::_addCraft(GameData* base, GameData* matériau, float progress, YesNoMaybe crit)` | `0x2B5CA0` | oui | ajoute un `CraftingItem` (crée l'objet) ; appelée par le chargement, `addFinishedCraftItem` (répéter) et le bouton « ajouter » de la fenêtre Fabrication |
| établi | `FnCraftRemove` | `CraftingBuilding::_removeCraft(int)` | `0x2B6A20` | oui | retire l'ordre n (l'ordre 0 libère aussi `+0x448`) ; chaîne « _removeCraft » |
| établi | `FnCraftQueueAdd` | `CraftingQueue::addCraftButton(Widget*)` (nom du mod) | `0x2B9770` | oui | lit les chaînes « id » / « id2 » du bouton, établi = `hand` à `CraftingQueue+0x60`, appelle `_addCraft(base, mat, 0, 2)` |
| établi | `FnCraftQueueRemove` | `CraftingQueue::removeCraftButton(Widget*)` (nom du mod) | `0x2B9940` | oui | lit « index », appelle `_removeCraft` |
| établi | `FnCraftQueueRemoved` | `CraftingQueue::craftRemoved(int, const CraftItemViewData&)` (nom du mod) | `0x2B9A50` | oui | ordre glissé hors de la liste : `_removeCraft` |
| établi | `FnCraftQueueRepeat` | `CraftingQueue::repeatButton(Widget*)` (nom du mod) | `0x2B5000` | oui | inverse `CraftingBuilding+0x4CC` |
| machine | `FnStopOperating` | `UseableStuff::stopOperating(const hand&)` | `0x2ACA90` | — | `erase` dans l'ensemble des opérateurs (+0x3D0) |
| machine | `FnOperatorSetInsert` | `std::set<hand>::insert(pair* ret, const hand&, bool)` | `0xF7D90` | — | l'insertion qu'appelle `UseableStuff::tryOperate` (`0xF8030`, vt 0x4F8) |
| énergie | `FnUpdatePowerGrid` | `Town::updatePowerGrid()` | `0x92CD60` | oui | vt 0x2B8 de `Town` : remet à zéro puis répartit l'énergie des générateurs et batteries de la ville (totaux à `Town+0x470`…`+0x490`) |
| énergie | `FnTogglePowerButton` | `UseableStuff::togglePowerButton(DataPanelLine*)` | `0x297A60` | oui | bouton marche / arrêt du panneau : `switchPowerOn(!powerOn)` (vt 0x318, `0x2973F0`) ; la ligne n'est pas lue |
| énergie | `FnToggleBattButton` | `UseableStuff::toggleBattButton(DataPanelLine*)` | `0x297AE0` | oui | inverse `+0x3B4` (reçoit l'énergie des batteries) |
| recherche | `FnRefreshResearchList` | `ManagementScreen::refreshResearchList()` | `0x49A4C0` | — | redessine la fenêtre Recherche après un changement imposé |

Trouvées par leurs chaînes (« Research complete: {1} » → `0x834550`, « Used artifacts… » →
`0x8343C0`, « Failed to learn from blueprint… » → `0x2B65D0`, « _removeCraft » → `0x2B6A20`) puis
par leurs appelants (`startResearch` : seul appel de `0x4976C0`, la fonction de « Cannot research
'{1}' » ; `stopResearch` : les boutons « retirer » de la fenêtre) ; les fonctions de `Research` et de
`CraftingBuilding` sont décalées de +0x1590, +0x1240 et +0x4B0 par rapport à KenshiLib 1.0.65 dans
ces zones. Prologues relevés dans `kenshi_x64.exe` et vérifiés au démarrage.

Les cinq passent par `Character::ai` (+0x650) puis `AI::orders` (+0x20, `OrdersReceiver`, tâches
à +0x90 nombre et +0x98 tableau de `Tasker*`). `Character::clearPermajobs` (`0x5C8FE0`) n'a aucun
appelant : le panneau n'a pas de « tout effacer » propre.

L'ordre de la table doit suivre celui de l'énumération `Fn` : la vérification des prologues ne voit
pas deux lignes inversées (chaque adresse correspond bien à ses octets). Deux lignes l'étaient
(`getValidInventoryPosition` / `existsItemInFootprint`, sans effet car jamais appelées) ; c'est
corrigé, et `python tools/check_functions.py` vérifie désormais que chaque entrée correspond au
commentaire de son énumérateur.

Fermes (`plugin/workshop.cpp`, 0.3.2) :

| Groupe | Enum | Fonction | RVA | Hook | Rôle |
|---|---|---|---|---|---|
| ferme | `FnFarmDestroyAPlant` | `FarmBuilding::destroyAPlant()` | `0xE4800` | — | le plant n° `+0x528` récolté : âge -10, sous-parties non statiques à l'échelle 0, `+0x528` + 1, `updateMaterial` ; ce qu'appelle la récolte de `FarmBuilding::operate` (`0xE59E0`) |
| ferme | `FnFarmReset` | `FarmBuilding::resetFarm()` | `0xE48E0` | — | replante : chaque plant âge `rand × -0,2`, `+0x524` le plus jeune, `+0x528` = 0, `+0x51C` / `+0x520` = 0, `+0x518` = `+0x524`, production `+0x448` à 0 |

### Fermes (`FarmBuilding`, vtable `0x1689948`) [D]
Trouvées depuis KenshiLib (décalage +0x40 dans la zone `0xD0000`-`0xF8000` : `tryOperate` KenshiLib
`0xF7FF0` = nôtre `0xF8030`), vérifiées par la vtable (vt 0x4E8 → `0xF6E00`, vt 0x4B0 → `0xE59E0`).
- `UseableStuff::tryOperate(hand)` (vt 0x4F8, `0xF8030`) : refuse si `dontNeedWorkRightNow()` (vt
  0x4E8) est vrai, ou si les opérateurs (`+0x3E0`) atteignent `+0x3AC` ; sinon insère dans `+0x3D0`.
  La tâche 87 (`Task_OperateMachine`, début `0x35B430`) l'appelle dès le départ et s'arrête si c'est
  refusé : pas d'ouvrier compté, pas de ligne « workers ».
- `FarmBuilding::dontNeedWorkRightNow` (`0xF6E00`) : vrai si `+0x468 != 0` **et** `+0x518 < 1` **et**
  `+0x54C > +0x51C` : les cultures poussent, pas de travail. `ResearchBuilding` : vrai sans recherche
  en file (vu en jeu, banc de Geoffrey).
- Champs : `+0x468` état de production (2 récolte due ou cultures mortes à nettoyer, 1 sortie pleine,
  0 / 3 pousse, 3 quand les entrées suivent) ; `+0x4E8` `lektor<Plant>` (nombre `+0x4F0`, données
  `+0x4F8`, 0x28 octets par plant : âge `+0`, position `+4`, sous-parties `+0x10`) ; `+0x518` pousse
  (0..1 : âge du plus jeune plant en croissance, recalculé à chaque image ; 1..2 : fenêtre de récolte,
  `dt / +0x540`) ; `+0x51C` pourriture (au-delà de 2) ; `+0x520` nettoyé ; `+0x524` début ;
  `+0x528` plants récoltés ; `+0x52C` objets par plant ; `+0x53C` durée de pousse ; `+0x54C` seuil de
  mort ; `+0x558` hydroponie.
- `FarmBuilding::update` (`0xE5580`) fait pousser chez tout le monde (eau : `_updateInputs`, sinon
  `+0x550` sécheresse) et appelle `resetFarm` quand tous les plants sont récoltés ou que `+0x51C` ≥ 1 ;
  `FarmBuilding::operate` (`0xE59E0`, le fermier) remplit la production `+0x448`, met les objets dans
  l'inventaire et appelle `destroyAPlant` : seulement chez l'hôte.
### Boutique d'animaux et prospection (`plugin/shops.cpp`, A_FAIRE 3i et 3p)

| Groupe | Enum | Fonction | RVA | Hook | Rôle |
|---|---|---|---|---|---|
| animaux | `FnAnimalTradeCtor` | constructeur du gestionnaire d'achat d'animaux (notre nom) `(this, Character* vendeur, Character* acheteur, lektor<Character*>* animaux, int prixDeBase)` | `0x6A9440` | hôte | [D] appelé par l'action de dialogue `CHOOSE_ANIMALS_BUYING` (0x30, `0x6817F4`) ; vtable `0x16FFAD8` ; prix de chaque animal = (prix de base, 14000 par défaut si ≤ 0) × (âge vt 0x398 + constante). L'hôte note celui fait pour le perso d'un autre joueur |
| animaux | `FnAnimalTradeConfirm` | sa `confirm()` (vt 0, notre nom) | `0x6AAC20` | client | [D] rassemble les entrées cochées (`0x7261C0`, vivantes), vérifie la taille d'escouade (message sinon), puis pour chacune : `removePermajob`, `PlayerInterface::recruit(animal, false)` ; puis `takeMoney(total)` sur la faction du joueur (`[0x2134690]`+0x2A0 → +0x80, vt 0) ; **enfin `ForgottenGUI::showCharacterEditor(animaux achetés, mode 1, nul)`** (appel en `0x6AB5A3`, 0.3.4) : l'éditeur pour nommer les animaux. **Ne vérifie pas l'argent** (c'est la fenêtre qui grise le bouton) : l'hôte le vérifie avant |
| animaux | `FnCharTradeHandlerDtor` | `CharacterTradingHandler::~` (non virtuel) | `0x6A69D0` | — | la fenêtre l'appelle puis libère par l'`operator delete` importé `[0x2247198]` (alloué par `[0x22471A0]`, 0x88 octets) |
| animaux | `FnCharTradeWindowCtor` | `CharacterTradingWindow::CharacterTradingWindow(handler)` | `0x72A880` | — | [D] 0x110 octets ; détruit l'`Instance` précédente (`0x2132C30`) puis prend sa place ; ne met pas le jeu en pause |
| animaux | `FnCharTradeWindowShow` | `CharacterTradingWindow::show(bool)` (vt 0x10) | `0x7223A0` | hôte | [D] true : widget (+0x38) vt 0x18 `setVisible(true)` ; false : `close()` (`0x726830` : cache, `GameWorld::setPaused(false)` `0x787D40`, libère le gestionnaire +0xD0 et la fenêtre). L'hôte garde cachée la fenêtre d'un gestionnaire noté |
| animaux | `FnCharTradeConfirmButton` | `CharacterTradingWindow::confirmButton(Widget*)` | `0x7267D0` | — | `handler->confirm()` ; vrai : libère le gestionnaire et ferme. Tests (`animalbuy`) |
| prospection | `FnProspectButton` | `OrdersPanel::prospectingButton(Widget*)` | `0x721800` | oui | [D] fenêtre ouverte : `hide` ; sinon perso = `0x7F4BA0(PI, 0x785F90(GameWorld))`, `checkPlayerOrderForProblems(150)` (`0x5D1940`), puis **`Character::addOrder(null, 150, null, false, true, 0)`** directement (aucune fonction d'ordre de `PlayerInterface` : aucun hook d'ordre ne le voyait), son de clic. N'utilise pas son `this` |
| prospection | `FnProspectSingleton` | `ProspectingWindow::getSingleton()` | `0x338400` | — | objet en `0x212EA60` (garde `0x212EA6C`, 0x140 octets) ; `getVisible` `0x48BF50` lit le widget à +8 |
| prospection | `FnProspectShow` | `ProspectingWindow::showT(const Vector3& pos, float skill, const std::string& name)` | `0x48E9E0` | — | [D] ce qu'appelle la tâche 150 (`0x348610`, son action) une fois son temps écoulé : position du perso (vt 0x38), compétence **science** (`CharStats`+0xE0), nom ; la fenêtre ne lit ensuite que les cartes de ressources du monde (données du jeu). Garde +0x108 position, +0x114 compétence, +0x118 nom |
| prospection | `FnCameraTarget` | `GameWorld::<cible de la caméra>(Vector3* out)` (notre nom) | `0x785F90` | — | `PlayerInterface`+0x30 (caméra) +0x58 nœud → position |
| prospection | `FnNearestSelected` | `PlayerInterface::<sélectionné le plus proche>(const Vector3&)` (notre nom) | `0x7F4BA0` | — | parcourt la sélection (+0x208) et rend le perso (type 1 ou 0x5B) le plus proche du point |
| TP admin (0.3.4) | `FnAiTasksClear` | `AITaskSystem::<vider les ordres>()` (notre nom) | `0x5074A0` | — | [D] vide le conteneur +0x38 (`0xE750`), détruit chaque ordre du tableau +0x80 (nombre +0x78, destructeur virtuel avec 1), remet le nombre à 0, puis `0x7789(this, 1)` si +0x1C0 n'est pas nul. C'est ce que fait le bouton « stop » du jeu (`PlayerInterface::stopCharactersMovement` `0x7F6470`) pour chaque perso sélectionné : `CharMovement::halt` (`Character`+0x640, vt 0x98), puis cette fonction sur `Character`+0x650 (`AI`) → +0x20 (`AITaskSystem`), puis `CharBody` (+0x648) vt 0x60. Les travaux permanents (`getPermajob`) ne sont pas dans ce tableau : ils restent. `kenshi::StopCharacter` = halt + cette fonction, appelé par la TP admin |

Mise en page lue au désassemblage (KenshiLib n'a pas `CharacterTradingHandler`) : gestionnaire +0x08
vendeur, +0x10 acheteur (pointeurs bruts), +0x18 booléen, +0x20 tableau d'entrées (nombre +0x28,
capacité +0x30), +0x38 titre, +0x60 texte ; entrée de 0x68 octets : `hand` de l'animal (+0), prix
(+0x20, int), cochée (+0x24). Fenêtre : +0x38 widget, +0xD0 gestionnaire, +0xD8 cases. Les quatre
fenêtres de ce type (vente et achat d'esclaves 0x29 / 0x2A, caution 0x2B, animaux 0x30) partagent la
classe ; seule celle des animaux est détournée pour l'instant. Décalages par rapport à KenshiLib
1.0.65 : +0x11D0 (zone 0x72xxxx : `OrdersPanel`), +0x780 (`ProspectingWindow` 0x48xxxx), +0x4B0
(`getSingleton`), +0x1260 (`ForgottenGUI`).

**Animal acheté et liste de l'escouade (0.3.4, [D])** : la liste des persos du joueur
(`PlayerInterface`+0x2B0, ce que lit le mod pour l'escouade) n'est pas tenue à jour au fil des
recrutements : `PlayerInterface::<update>` (`0x801630`, appelée par la boucle de `GameWorld` en
`0x788D9B`, pause ou pas) la **reconstruit à chaque image** (`0x801E26` : faction du joueur vt 8
`getCharacters(liste, 1, false)` → pour chaque peloton +0x210/+0x218, `ActivePlatoon` (+0x1D8) vt 0x28,
membres +0x58/+0x60). Mais cette fonction sort tout de suite (`0x801669` → `0x801F3D`) pendant un
écran de chargement (`0x801676`) ou quand une des fenêtres bloquantes de `ForgottenGUI` est affichée
(`0x80168A` → `0x6E2F00`, liste globale de fenêtres, drapeau +0xC0 ; que l'éditeur de persos en
soit une est déduit de ce qu'on a vu en jeu, pas lu au désassemblage). Le `confirm()` des animaux finit
par `showCharacterEditor(animaux, 1)` : exécuté par l'hôte pour l'achat d'un client, il ouvrait cet
éditeur chez l'hôte (invisible pour le test), le jeu de l'hôte se mettait en pause à cet instant
(horloge du relevé `h_animalshop2` du 13/10 arrêtée au moment de la vente) et l'animal, bien dans le
peloton du joueur, n'entrait jamais dans la liste. L'hôte n'ouvre plus cet éditeur pendant une vente
pour un autre joueur (`NoEditorScope` autour du `confirm`, `hk_showEditor`) ; l'animal garde son nom.
L'achat de l'hôte pour lui-même ouvre toujours l'éditeur, comme en solo.

PROSPECT (tâche 150) : `checkPlayerOrderForProblems` (`0x5D1940`) refuse si le perso n'est pas dehors
(vt 0x1D8, handle du lieu, type ≠ 0xB : « I need to go outside to do this. ») ou pas sur le sol ferme
(`CharMovement`+0x340 ≠ 0xB : « I need to be stood on solid ground. ») ; la tâche (`0x348610`) ouvre
la fenêtre après 1 s de mise à jour du perso.

### Clic droit sur un personnage et conversations (0.3.4, A_FAIRE 12/10 points 1, 7, 8)

| Groupe | Enum | Fonction | RVA | Hook | Rôle |
|---|---|---|---|---|---|
| parler | `FnCharacterCursor` | `PlayerInterface::<personnage sous le curseur>(Character*)` (notre nom) | `0x7FB410` | client | [D] appelée chaque image par `PlayerInterface::update` (`0x800250`, appel `0x80098F`) et par `0x7FC6E0` (appel `0x7FCA90`), deux arguments, ne rend rien. Choisit le curseur, et à l'image d'un clic droit donne l'ordre (voir plus bas). Prologue `40 55 53 41 54 48 8D 6C 24 B9 48 81` |
| parler | `FnCharacterCanTalk` | `Character::<peut qu'on lui parle>()` (notre nom) | `0x67AC00` | — | [D] `mov rcx,[rcx+0x280]` puis saut à `0x67AA50` (son `Dialogue`) : le contrôle du jeu avant l'ordre « parler » |

**Ce que fait un clic droit sur un personnage** (`0x7FB410`, `rcx` = `PlayerInterface`, `rdx` = la cible) [D] :
- `PlayerInterface`+0x2F1 (clic gauche de l'image, mis par `update` en `0x8016B6`) : saut à `0x7F9000`
  (la sélection). +0x2F3 = **clic droit de l'image** (`0x8016D5`) : sans lui, seul le curseur change
  (survol). +0x268 : un autre mode de l'interface ; quand il est mis, le clic ne donne aucun ordre.
- `rdi` = le perso sélectionné le plus proche de la cible (`0x7F4BA0`, sinon `0x7F2F70`) ; aucun :
  rien.
- `0x79CBE0(PI, cible)` (« hostile ») : ennemi → si la cible n'est pas en combat (`0x436610`, combat
  au corps à corps ou à distance) et que `0x67AC00` dit oui, **parler** (curseur 7 ou 8, tâche 12 par
  `addTaskNearestSelectedCharacter`, saut commun `0x7FC3CB`) ; sinon attaquer (curseur 5, tâches 235,
  ou 5 puis 16).
- Non hostile (`0x7FBC0C`) : un perso de la faction du joueur (`0x7916D0`, `Faction`+0x250) a ses
  propres ordres ; quand `0x5C9750(rdi)` est vrai (`rdi` hors combat et `rdi`+0xD4, sens non établi),
  un perso dans une cage ou un lit (`Character`+0x2F8 = 2 ou 1) aussi (tâches 76, 201, 228, et
  `Character::addOrder` `0x5D20D0` directement pour la tâche 26 : **aucun hook d'ordre ne le voit**).
  Sinon : `0x67AC00` oui → parler (tâche 12), **non → rien du tout** (ni ordre, ni message).
- `Dialogue::<a une réplique « parle-moi »>` (`0x67AA50`) : vrai si une conversation est en cours
  (+0xC8) ; sinon il faut des répliques pour l'événement 1 (`EV_PLAYER_TALK_TO_ME`) dans la table
  +0x1A0, un PNJ qui n'est ni hors d'état (vt 0x30) ni de la faction du joueur (`Faction`+0x250), et
  une réplique dont les conditions passent pour le perso sélectionné le plus proche
  (`0x48CED`). Chez un client tout cela est lu sur **sa copie** du PNJ : une copie dont le dialogue ou
  l'état diffère de l'hôte ne propose pas « parler », et le clic ne produit rien. Le mod le détecte
  (aucun de ses hooks d'ordre n'a été appelé pendant le clic), le dit dans le journal du client et
  envoie l'ordre « parler » quand même : l'hôte décide (voir FONCTIONNALITES « Parler »).

**Minuteries d'une conversation** [D] (`Dialogue::update` `0x685090`, `Dialogue::say` `0x67FD80`) :
- `Dialogue`+0x228 : l'étiquette de la bulle en cours (`0x672320` la place au-dessus de la tête) ;
  +0x230 : minuterie de la réplique dite, mise par `say` à **2 s + 0,12 s par caractère** ; +0x234 :
  pause d'une action de dialogue (action 12 de `_doActions`, `0x6823EE`) ; +0x190 réplique en cours,
  +0x198 suivante, +0x2A0 le `hand` de l'autre.
- `update` ne décompte ces minuteries que si +0x228 existe **et** que le jeu n'est pas en pause
  (`GameWorld`+0x8B9, lu en `0x685284` à l'adresse `0x21349C9`). Quand les deux sont sous zéro : sans
  réplique suivante, ou avec une réplique en cours sans suite (+0x1E1 de la réplique à 0), il appelle
  `endDialogue` ; si la suite est dite par l'autre (+0x31C de la réplique), il termine celle-ci
  (`endDialogue(false)`) et la fait continuer dans le `Dialogue` de l'autre (`0x49A85`) ; sinon il
  dit la suite lui-même.
- En solo, la fenêtre de conversation met le jeu en pause : rien n'avance tant qu'on n'a pas
  répondu. Chez l'hôte, la conversation d'un client ne met pas le jeu en pause : une conversation
  commencée par une bulle (le « Vérifications de contrebande » d'un garde, événement 3) se fermait
  ou continuait toute seule quand la minuterie de la bulle tombait à zéro (12/10 : 0,15 s et 1 s
  après l'ouverture chez rob1 et nass4 ; chez rob1 elle a continué dans un autre `Dialogue`, sans nom,
  avec « Non. » comme si une réponse avait été choisie). Les conversations ouvertes par « parler »
  n'ont pas de bulle : elles attendaient déjà. Le mod garde +0x230 à 3 s au moins tant que le joueur
  a des réponses à choisir (`kenshi::HoldDialogueLine`, au plus 5 min sans réponse).
- La fenêtre de dialogue du jeu (`0x727B20`) termine elle-même une conversation dont la réplique n'a
  aucune réponse (+0x168 nombre de réponses à 0 → `endDialogue(true)`) : le mod ne garde donc une
  réplique que si elle a des réponses.

### Autres adresses de fonctions utilisées directement

| RVA | Fonction | Usage |
|---|---|---|
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
| `0x212EB00` | pointeur vers `UniqueNPCManager` (créé par son accesseur `0x354560`) |
| `0x2134100` | pointeur vers `TownList` (créé en `0x15D5B77`) |
| `0x2133DD0` | `GlobalConstants con` (objet, pas un pointeur ; KenshiLib `0x2130CE0` + `0x30F0`, comme `ou`), rempli depuis les données du jeu par `GlobalConstants::setup` (`0x86B2B0`, appelé en `0x871E6C` avec `lea rcx, con`). Champs vérifiés par désassemblage : +0x6C « heal rate mult » (float, lu par la guérison des membres `0x64F815`, `0x64F971`, `0x64FAA8`), +0x70 « resting heal rate mult » (float, `0x64FA13`, `0x652676`), +0x138 « exp gain multiplier » (double = valeur des données × constante ; multiplie les gains d'XP de `CharStats` `0x8C5E80`…`0x8C69B7` avant `increaseStat` `0x8C5DF0`, qui ne le lit pas). Multiplicateurs de l'hôte (0.3.2) |

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
| +0x1E4 | étage affiché, 0 à 3 (`setViewFloor` `0x7F3530`, boutons d'étage et PgPréc/PgSuiv) [D] |
| +0x270 | `hand` du perso **suivi** par la vue des étages (type 0xB : aucun) [D] |
| +0x290 | étage de ce perso vu à la dernière image [D] |

- **Vue des étages** [D] : la mise à jour de `PlayerInterface` (`0x801630`, sautée si +0x2F0) résout
  +0x270 (rien si son type vaut 0xB), lit l'étage du perso (vt 0x60 = `0xD1F80`, `RootObject+0xA4`,
  écrit chaque image par vt 0xD8 `0x5C7C30` depuis `CharMovement::getCurrentFloor` `0x6610E0`) et,
  s'il diffère de +0x290 (`0x801CD1`), écrit `clamp(étage, 0, 3)` dans +0x1E4 (`0x801D09`). La vue
  suit donc ce perso, pas la sélection. Seul `0x7F5400` (`RootObject*` ou null : le premier perso
  du joueur) le change : il copie `obj+0x60..+0x70` dans +0x278..+0x288, pose +0x290 et +0x1E4, puis
  `Camera::setTarget` (`0x6AE520`, qui garde sa propre copie du `hand` à `Camera+0x28`). Appelé par
  un double appui sur une touche d'escouade (`0x7F8F60`), la fenêtre d'un perso (`0x727820`), la fin
  de l'éditeur de personnage (`0x5F5D10`) et `ActivePlatoon::addCharacterAt` quand le perso déplacé
  était suivi. `objectSelected` et `focusCameraSelectedCharacter` (`0x7F37F0`) n'y touchent pas.
  Chez un client, +0x270 restait le perso de la partie chargée (celui de l'hôte) : le mod
  (`kenshi::SetFloorFocus`, `KenshiWorld::UpdateFloorFocus`) écrit les champs du `hand` d'un perso
  du joueur et +0x290 = -1, sans toucher à la caméra.
- **+0x270 est le perso que la caméra « suit »** [D, 0.3.2] : `0x7F5400` est
  `PlayerInterface::startTrackCharacter`, `0x7F2780` `stopTrackCharacter` (+0x270 = aucun,
  +0x290 = -1, puis `Camera::stopFollowing` sur `PlayerInterface+0x30`), `0x7F27E0`
  `isTrackingCharacter`. `CameraClass::move` (`0x6AE5D1`, touches et bords d'écran) et
  `CameraClass::teleport` (`0x6AF934`, clic sur la carte) appellent `stopTrackCharacter` : dès que le
  joueur bouge la caméra, le jeu remet +0x270 à « aucun ». Le mod 0.3.1 le reprenait toutes les 0,5 s
  et se battait avec la caméra du joueur (A_FAIRE 3l) ; depuis 0.3.2 il ne le pose qu'une fois par
  monde chargé (ou s'il revient sur le perso de la sauvegarde de l'hôte, 3 fois au plus), et les toits
  au-dessus du perso sélectionné sont masqués par `UpdateRoof` sans toucher à ce focus.
- **Le mode construction met le jeu en pause** [D, 0.3.2] : `PlayerBuildMode::activate`
  (`0x4E65E0`) appelle `GameWorld::togglePause(true)` (`0x787D40`), qui écrit `GameWorld+0x8B9`
  (= demande || vitesse +0x700 nulle) sans changer la vitesse ; `deactivate` (`0x4D1870`) appelle
  `togglePause(false)`. `togglePause(false)` laisse donc la pause si le joueur l'avait mise lui-même
  (`userPause(true)` met la vitesse à 0). `0x4D1920` (`togglePause(true)` seul) n'a pas d'appelant.

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
| +0x2E0 | `RaceData*` (vt 0xD0 le rend, `0x5E1F00`) ; son `GameData*` de race (type 7) à +0x40 : « pathfind footprint radius », « hull size X/Y/Z », lus par `CharMovement` (`0x661F90`, rayon de pathfinding à `CharMovement+0x364`, multiplié par vt 0x390) [D, 0.3.1] |
| +0x2E8 | `Inventory*` |
| +0x2F8 | `inSomething` : 0 rien, 1 au lit, 2 en prison / cage ; `inWhat` (`hand`) à +0x300 |
| +0x348 / +0x380 | porte quelqu'un (bool) / `hand` du corps porté |
| +0x418 | rang dans l'escouade (int) |
| +0x1A9 | **vu par la caméra** (bool, `kenshi::CH_inView`) : recalculé à chaque image par `0x5C9F60` (distance² à la caméra, `0x7F2A20` = `PlayerInterface+0x30` caméra → position, comparée à un seuil, puis tests de cadrage `0x6AE7B0` / `0x6AE710`) ; lu par l'animation (`0x51E59C`, `0x5B562A`), le rendu, et `addWound` pour le chiffre de dégâts (`0x651DF5`) [D, 0.3.1]. **Choisit aussi la mise à jour de déplacement** : `CharMovement` (`+0x3A8` → perso) teste +0x1A9 en `0x660A3D` : à 1, la mise à jour complète `0x147FD0` (physique, sols et murs, chaque image) ; à 0, la version réduite `0x148AC0` (le PNJ « lointain » déplacé quelques fois par seconde, une autre hauteur : 33 à 68 unités d'écart sur les remparts d'Admag et la forteresse d'Okran) ; lu aussi en `0x65F425`, `0x65F489`, `0x660089`. Au début de `0x5C9F60`, `0x5C79C0` laisse à 1 tout perso dont la faction a +0x250 non nul (la faction du joueur) : les persos des joueurs sont toujours « dans la vue », où qu'ils soient [D, faraway] |
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
| 0x228 | crée un `CharStats` neuf (`0x621190` ; `CharacterAnimal` : `0x623660`, `CharStatsAnimal`) et l'écrit à +0x450, **`MedicalSystem` nul** jusqu'à son `init` ; sur un `Building`, le même emplacement est un simple accesseur (`0xF6B10` : `return this+0x160`) |
| 0x278 / 0x280 | mises à jour du perso par le thread de travail (`0x787940`, listes +0x1C8 / +0x1E0 de son objet) ; 0x280 = `0x5D00B0`, qui appelle `stats->update` (`0x5D066A`) si `ActivePlatoon` (+0x658) est non nul ; 0x278 (`0x5C7D40`) l'appelle aussi (`0x5C7DCF`) |
| 0x290 | `isItOkForMeToLoot` |
| 0x298 | `ImStealingDoYouNotice` |
| 0x370 | `setProneState` |
| 0x378 | mise en place du perso depuis son `GameData` (`0x62B210` : écrit `ActivePlatoon` +0x658 en premier, initialise les stats plus loin) |
| 0x390 | **échelle** : 1,0 pour `Character` / `CharacterHuman` (`0x5E5980`) ; `CharacterAnimal` (`0x5E1FA0`) : +0x6F0 + (+0x6F4 − +0x6F0) × +0x700, soit lerp(échelle min, échelle max, âge). Appelée « getAge » à tort jusqu'en 0.3.1 [D] |
| 0x398 | **âge** : 1,0 pour un humain (`0x5E5990`) ; `CharacterAnimal` +0x700 (`0x5E2010`), écrit avec +0x6FC par son constructeur (`0x5C8150`) depuis l'argument `age` de la fabrique [D] |

### CharStats (ordres permanents et compétences)
- +0x8 `MedicalSystem*` (celui du perso, `Character+0x458`), +0x10 `me`. Le constructeur
  (`0x886E20`) met +0x8 à nul ; c'est vt 0x0 `init(GameData*, MedicalSystem*, Character*)` qui le
  remplit (appelé par la mise en place du perso, `0x62BB73`). vt 0x8 `update(dt)` (`0x883B70`, aussi
  `CharStatsAnimal`) : `if (!medical->inconscient(+0x161) && !medical->mort(+0x164)) this->vt0x20(dt)`,
  sans tester le pointeur : des stats créées mais pas initialisées plantent là (`+0x883B78`, lecture
  à 0x161). Voir section 14.
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
- +0x334 `floorGroup` (9 = rez-de-chaussée, 10 = premier étage ; dehors : groupe de la surface, 2
  pour le sol des persos relevés). Recalculé à chaque mise à jour depuis la surface trouvée sous le
  perso (`0x65F564`, mot du résultat de la requête). Un perso ne tient que sur les surfaces de son
  groupe : une doublure créée à l'étage avec le groupe 9 retombe au rez-de-chaussée à chaque
  position écrite, téléportation comprise (vt 0xC0 appelle `_setPositionAndTeleport(pos, -1)` :
  groupe inchangé) [observé, farfps 13/10]. `_setPositionAndTeleport(pos, étage)` (`0x65E940`) :
  `halt` (vt 0x98), position valide ?, `_setPositionSimple` (vt 0xC8), +0x3B0 (vt 0x48) à la même
  position, puis `floorGroup = étage + 9` si étage >= 0 [D].

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
- +0x80 messages en attente des threads de travail (`vector<DT_MSG>` : 1 fin, 2 ouvrir la fenêtre,
  3 la fermer, 4 vider les réponses, 5 réponses, 6 réplique) ; +0x148 terminée (`_hasEnded`, bool) ;
  +0x149 crie (bool) ; +0x150 `me` ; +0x158 `hand` de l'interlocuteur ; +0x258 réponses
  (`vector<std::string>`) ; +0x278 réplique de l'interlocuteur (`std::string`) ; +0x190 / +0x198
  réplique en cours / suivante, +0x228 étiquette de la bulle, +0x230 / +0x234 minuteries (0.3.4 :
  voir « Clic droit sur un personnage et conversations »).
- Événements (`EventTriggerEnum`) : 1 = `EV_PLAYER_TALK_TO_ME`, 3 = `EV_I_SEE_NEUTRAL_SQUAD` (le
  contrôle des gardes, « halte »), 8 voleur pris, 44 prime repérée, 45 prisonnier évadé repéré...
- **`endDialogue(bool)`** `0x674830` [D] (KenshiLib 1.0.65 `0x6740B0` + `0x780`, le même décalage que
  `setInDialog` et `replyClicked` ; prologue `40 53 56 41 54 48 83 EC 30 48 8B F1`). Hors du thread
  principal (test `0x25C6D0`) : range seulement `DT_END_DIALOG` (1) dans +0x80, traité plus tard par
  `Dialogue::update`. Sur le thread principal : met +0x148 à 1, remet les états de la conversation,
  puis appelle `setInDialog(false)` (`0x674B46`) : le crochet du mod voit donc la fermeture.
- **`setInDialog(bool)`** `0x6746A0` [D] : hors du thread principal, range `DT_OPENWINDOW` (2) ou
  `DT_CLOSEWINDOW` (3) ; `Dialogue::update` le rappelle ensuite sur le thread principal (appels
  `0x685106` / `0x685112`), donc par le crochet. Sur le thread principal : ouvrir saute à `0x727820`
  (ouverture de la fenêtre de conversation), fermer appelle `0x721F20`. Seul `setInDialog` mène à
  `0x727820` (références relevées).
- **La conversation met le jeu en pause** [D] : `0x727820` appelle `GameWorld::userPause(true)`
  (`0x727875`, `dl = 1`) avant d'ouvrir la fenêtre, et `0x721F20` `userPause(false)` en la fermant.
  En solo, parler à quelqu'un fige donc le monde. En coop, la fenêtre d'une conversation d'un client
  n'est jamais ouverte chez l'hôte (pas de pause) ; celle de l'hôte lui-même l'est, et le mod lève la
  pause aussitôt quand d'autres joueurs sont là (voir FONCTIONNALITES « Dialogues »).

### Inventaires
- `Inventory` : +0x10 tous les objets (`lektor<Item*>`) ; +0x28 sections (table nom →
  `InventorySection*`) ; +0x68 sections dans l'ordre de recherche ; +0x80 objet de rappel
  (`callbackObject`) ; +0x88 propriétaire.
- vtable d'`Inventory` : 0x10 `addItem`, 0x18 `tryAddItem`, 0x20 `hasRoomForItem`, 0x28
  `removeItemDontDestroy`, 0x30 `removeAutoDestroy`, 0x38 `dropItem`.
- `InventorySection` : +0x30 largeur, +0x34 hauteur, +0x40 objets (`vector` de
  `{Item*, u16 x, y, w, h}`, 0x10 octets chacun), +0xD0 activée. vt 0x10 `addItem`, vt 0x18
  `_addItem(item, x, y)`.
  - `canItemGoHere` (`0x74BE40`) répond non tout de suite pour une section désactivée (+0xD0 = 0,
    son premier test), puis vérifie le type d'objet et la taille. Chez un client, des sections
    d'équipement de doublures sont désactivées (bottes, chemise) alors que le perso de l'hôte les
    porte : le mod active la section le temps de vérifier et de poser (`PlaceItem`, mode
    `hostLayout`).
- `Item` : +0xC0 fabricant, +0xC8 matériau, +0xD8 dans un inventaire, +0xDC position, +0xE8
  section, +0x118 charges, +0x11C qualité, +0x129 équipé, +0x12C quantité, +0x130 / +0x134
  largeur / hauteur, +0x188 groupe d'objets.
  - vt 0x228 `activate`, vt 0x238 `deactivate`, vt 0x2B8 `getLevel`, vt 0x358
    `setInventoryWeAreIn`.
  - +0x190 actif dans le monde (mis à 1 par `activate` via vt 0x230, à 0 par `deactivate`) ;
    +0x1C8 / +0x1D8 : ce que lit `isPhysical` (vt 0xF8, `0xD2210`). Le corps physique est créé par
    vt 0x220 (`0x75E5D0`) seulement si l'objet a déjà son visuel (vt 0xC8 non nul) et que sa zone
    est prête (`0x3B0E3`) : juste après un `activate`, un objet peut être au sol sans être
    « physique ». [D]
  - `Item::activate` (`0x75D9B0`) est partagé par les 14 classes d'objets (`Item`, `Weapon`,
    `Sword`, `Crossbow`, `Armour`, `LockedArmour`, `MoneyItem`, `ContainerItem`, `MapItem`,
    `NestItem`, `BlueprintItem`, `Gear`, `SeveredLimbItem`, `RobotLimbItem` ; vtables listées dans
    `kItemVtables`, `plugin/kenshi.cpp`). Il est aussi appelé quand une zone charge les objets de la
    sauvegarde : il ne distingue pas un objet lâché. [D]
- **Comment un objet arrive au sol** [D] :
  - glisser-déposer d'une fenêtre d'inventaire vers le monde : `MouseInventory` (`0x7136A0`, son
    « Character_Drop_Ground ») appelle `Inventory::dropItem` (vt 0x38) de l'inventaire de la
    fenêtre. Celui-ci appelle `callbackObject->dropItem` (vt 0x1A8 ; `Inventory` +0x80) puis
    `removeItemDontDestroy(item, -1, true)` ;
  - `dropItem` (vt 0x1A8) selon le propriétaire : `CharacterHuman` `0x5CA740`, `CharacterAnimal`
    `0x5CA4A0` (même code), bâtiments à inventaire (coffres, production, fermes…) `0x54E630` (fait
    lâcher le personnage dont le `hand` est en +0x380 du bâtiment), sac à dos `ContainerItem`
    `0x75D810` (fait lâcher son porteur, +0x230) ; `RootObject` et les autres : rien (`0xD2040`) ;
  - `CharacterHuman::dropItem` : position = devant le perso (`CharMovement`+0xD0),
    `setInventoryWeAreIn(hand vide 0x1E3A5F8)`, vt 0x1C8 (propriétaire), `activate(true, pos,
    IDENTITY, false, &2, false)`, puis retrait de **son propre** inventaire. Pour un objet d'un
    coffre, le drapeau « dans un inventaire » (+0xD8) n'est remis à 0 qu'au retour, par
    `Inventory::dropItem` ;
  - une fenêtre qui n'a pas de place pour un objet (`0x70EAA0`) le lâche par vt 0x38 ou par
    `callbackObject->dropItem` ;
  - `Character::giveItem(dropOnFail)` : un inventaire plein lâche l'objet.
- Les armes se créent à partir de leur fabricant :
  `createItem(factory, fabricant, hand, type d'arme, matériau, niveau)`. Le fabricant vient en
  premier, comme dans le code du jeu.
- L'équipement porté n'existe que dans les sections d'équipement, pas dans la liste
  `_allItems`.
- **Quel inventaire possède un objet** [D, 12/10, cause des plantages de l'hôte du 11/10] :
  - `InventorySection` +0xB4 `isAnEquippedItemSection` (ses objets ne sont pas dans `_allItems`), +0xC0
    objet de rappel (équipement : vt 0x198 à l'ajout, vt 0x1A0 au retrait), **+0xC8 l'inventaire qui la
    possède** (`InventorySection::getInventory` `0x745BC0` ne fait que le lire) ;
  - `InventorySection::_addItem` (vt 0x18, `0x749720`) : position et nom de section dans l'objet,
    `setInventoryWeAreIn`, +0xD8 = 1, ajout au vecteur et à la grille, puis (section non d'équipement)
    `parent->_sectionAddItemCallback` (vt 0x40 → `_addToList`, vt 0x60) : **l'objet entre dans la liste de
    l'inventaire de la section**, quel que soit l'inventaire par lequel on est passé ;
  - `InventorySection::removeItem` (`0x749A20`) : retrait du vecteur et de la grille, puis
    `parent->_sectionRemoveItemCallback` (vt 0x50 → `_removeFromList(item, false)`, vt 0x68 : retrait de
    la liste par simple comparaison de pointeurs, `0x7462C0`), rappel vt 0x1A0, `setInventoryWeAreIn(vide)`,
    +0xD8 = 0, nom de section vidé ;
  - `Inventory::removeItemDontDestroy_returnsItem` (vt 0x28, **`0x74A950`**) : si +0xD8 vaut 0, rend l'objet
    sans rien retirer ; sinon cherche la section **par son nom** (`getSection(nom)` `0x747BB0`, la table
    +0x28 de **cet** inventaire). **Pas de section de ce nom : +0xD8 = 0, nom vidé, l'objet est rendu,
    mais il reste dans la liste et la section de l'inventaire qui le possède.** Moins que la pile : une
    copie (`returnCopy`) ou rien ; sinon `section->removeItem`. `removeItemAutoDestroy` (vt 0x30, `0x745DE0`)
    = ce retrait puis `GameWorld::destroy` (`0x799AF0`) de ce qui est rendu ;
  - `Inventory::dropItem` (vt 0x38, `0x745D90`) = `callbackObject->dropItem(item)` puis
    `removeItemDontDestroy(item, -1, true)`. Pour un sac, `ContainerItem::dropItem` (`0x75D810`) fait lâcher
    son porteur (+0x230), et `CharacterHuman::dropItem` (`0x5CA740`) retire l'objet par l'inventaire **du
    perso** (+0x2E8), qui n'a pas de section « backpack_content » : l'objet est au sol, marqué hors
    inventaire, et **reste dans la liste et la section du sac**. Puis le retrait par le sac le voit à
    +0xD8 = 0 et ne fait rien. C'est ce que faisait l'hôte en rejouant un dépôt au sol depuis un sac
    (`MoveInventoryItem` → vt 0x38 de l'inventaire du sac) : objet lâché deux fois, puis, une fois l'objet
    détruit (ramassé et fusionné dans une pile), pointeur pendant lu par `Inventory::getResourceItems`
    (`0x749490`, l'IA de transport par `findResourceDestinationFor` et `hasABackpackOn`) ou par
    `InventorySection::recalculateTotalWeight` (`0x749160`, depuis `removeItem`) : les deux plantages.
    Dans le jeu de base, la souris sort d'abord l'objet de sa section (+0xD8 = 0) avant ce `dropItem`.
  - Le mod passe donc toujours par l'inventaire qui possède l'objet (`HomeOf` : le parent de la section qui
    le tient, `plugin/kenshi.cpp`), vérifie que cet inventaire trouve la section par son nom avant tout
    retrait (`TakeFromHome`), sort l'objet d'un sac avant de le faire lâcher par le porteur (`DropOut`), et
    ne détruit un objet qu'une fois absent de toute liste (`DisposeTaken`).
- **Piles** [D] : `Item::isStackable(InventorySection*)` (`0x75E090`) rend le maximum d'une case : 1 pour les
  types 2 (arme), 3 (armure), 0x2E (sac), 0x66, 0x6B, 0x6F ; sinon l'entier « stackable » des données de
  l'objet (`GameData`+0x178), passé par `applyStackingBonuses` de la section (bonus des sacs de marchand).
- **Bonus de pile d'une section** [D, 12 oct.] : `InventorySection::applyStackingBonuses` (`0x7466B0`) rend
  `max(stackable, +0x88) * +0x8C` (int « stackable bonus minimum », float « stackable bonus mult » des données
  du sac) ; si +0x98 est non nul, le « stackable » de l'objet est pris pour 1. Traders Backpack Medium
  (`1498-gamedata.base`) et Large (`1499`) : 2 et ×3 (Matériaux de Construction en piles de 6) ; les autres
  sacs (Shopkeepers, Old Traders, Small, Large…) : 1 et ×1. Commande de debug `bagbonus N`.
  `Item::addQuantity` (`0x75E220`) fusionne en tenant compte des charges. `canStackWith` `0x75D470`.

### Escouades
- `Character` +0x658 → `ActivePlatoon` ; +0x78 → `Platoon` (nom à +0x18). Le `hand` de
  `RootObjectBase` (+0x58) ne donne **pas** d'identifiant valide pour les escouades du joueur (essai
  en jeu du 10 oct. : aucune escouade envoyée) ; le mod nomme une escouade par l'adresse de son
  `Platoon` (`kenshi::SquadHandle`, type 0x5153), propre à chaque machine.
- `Platoon` +0x1D8 → `ActivePlatoon` ; `ActivePlatoon` +0x58 nombre de membres, +0x60 tableau,
  +0xA0 chef. `Character` +0x418 sa case dans l'escouade.
- **Le chef est la case 0** [D] : `swapCharacters` et `addCharacterAt` appellent
  `ActivePlatoon::setSquadLeader` (`0x7917A0`, écrit +0xA0 et le `hand` du chef dans `Platoon`
  +0x128) pour la case 0, `Character::setSquadMemberType(0)` pour les autres.
- Créer une escouade : `PlayerInterface::createSquad` (bouton `onAddSquad`, `0x491B50`).
- Déplacer un personnage : `ActivePlatoon::addCharacterAt`, ce que fait le dépôt d'un portrait sur
  une autre escouade ; sur un portrait de la même escouade : `swapCharacters`.
- Ordre des escouades (fenêtre Escouade) : celui de `Faction::activePlatoons` (faction du joueur :
  `PlayerInterface` +0x2A0), changé par `changePlatoonIndex`. L'escouade des morts
  (`PlayerInterface` +0x2C8, `hand`) en fait partie et n'est pas montrée.
- Retirer une escouade : `Faction::destroyPlatoon`, seulement vide. `ActivePlatoon::emptySquadCheck`
  (`0x79AB10`) : vide → `Platoon::setPersistentSquad(false)` puis `Platoon::declareDead`.
- Renommer : la case de nom écrit `Platoon`+0x18 directement à chaque frappe (pas d'appel à
  hooker) ; le mod compare les noms deux fois par seconde.
- Gestionnaires de la fenêtre (`SquadManagementScreen`, traduits de KenshiLib, début de fonction
  confirmé par `.pdata`) : `onNameChanged` `0x48DF00`, `onRemove` `0x4A1460`, `removeSquad`
  `0x491D20`, `onAddSquad` `0x491B50`, `notifyEndDropSquad` `0x493CB0`, `notifyEndDropPortrait`
  `0x49B050`, `dismissCharacter` `0x48FA30` (non étudiée).

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
- +0xB0 `std::string` : **nom** de la dernière demande (chargement `0x47B5BF`, sauvegarde `0x47B978`),
  +0xA4 sauvegarde automatique (`save`, `0x47B993`). Lu par le mod quand un monde devient prêt : le
  nom de la sauvegarde chargée, auquel les statistiques des joueurs sont rattachées (0.3.5).
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
| 15 | poursuivre : le bouton **JOBS** de la barre (`OrdersChaseButton`) : fait ou non les travaux de la liste (`OrdersReceiver` +0x35) ; ne crée pas de travail (c'est Maj + clic droit, voir « Touche Maj ») |
| 16 | vitesse de groupe |
| 17 | distance |

Dans l'interface, les ordres 11 et plus sont des bascules : `setOrderSelectedCharacters` décide
d'après les boutons du joueur **local**. Le client envoie donc la valeur voulue (`Command.shift` =
valeur absolue dans `add`, calculée sur le premier perso choisi) et l'hôte appelle
`Character::setStandingOrder(ordre, valeur)` directement ; un ancien client (sans `shift`) : l'hôte
calcule `on = !état actuel`.

`setStandingOrder` (`0x5CAA50`, table de sauts de 18 cas) [D] : 0/1/2/16 écrivent `CharMovement`
(+0x640) +0x20 (2 courir, 1 trottiner, 0 marcher, 3 vitesse du groupe ; c'est le `gait` des
snapshots) ; 3/4 `Character`+0xD4 ; 5/6/7 `AI` (+0x650) +0x2B8 ; 8/9 une fonction d'`AI` ; 11/12/13/14/17
`CharStats` +0x128/+0x12B/+0x12C/+0x12A/+0x129 ; 15 `OrdersReceiver` (`0x508210`).

Boutons de la barre (`OrdersPanel`, `0x72B540`) : BLOCK, HOLD, PASSIVE, JOBS, RANGED, TAUNT, SNEAK et
les flèches d'allure appellent `setOrderSelectedCharacters` ; MEDIC (`0x7FAC20`) et RESCUE
(`0x721790`) appellent `newPlayerTaskSelectedCharacters` avec les tâches 58 (`JOB_MEDIC`) et 148
(`FIND_AND_RESCUE_IF_THERES_BEDS`) ; PROSPECT (`0x721800`) donne la tâche 150 au perso sélectionné le
plus proche de la caméra par `Character::addOrder`, et la tâche ouvre la fenêtre de prospection à sa
fin (voir « Boutique d'animaux et prospection »). Le curseur de
comportement (AGRESSIVE / CONFIDENT / SUBMISSIVE) existe dans la mise en page mais est caché.

### Tâches (`TaskType`, celles que le mod nomme dans le journal)
La numérotation complète est celle du jeu lui-même : `fcs_enums.def` (dossier de Kenshi), `enum
taskType`, numérotée depuis 0 (289 valeurs, `NULL_TASK` à `PROTECT_ALLIES_STAY_IN_TOWN`) ; elle
concorde avec toutes les valeurs relevées au désassemblage. Les tâches qu'un client peut envoyer
et leur cible : `kc::TaskTargetAllowed` (protocol.cpp), revue le 11/10 contre les constantes passées
aux quatre fonctions d'ordres (`addOrderSelectedCharacters` 0x7F9E20, `newPlayerTaskSelectedCharacters`
0x7FA650, `addTaskNearestSelectedCharacter` 0x7FAE70, `addJobSelectedCharacters` 0x7F5A90, appelées
par des sauts `0x37DAD`, `0x272FA`, `0x311C4`, `0x53323`) et la tâche par défaut de chaque classe de
bâtiment (vtable +0x418 : `Building` 29 ; `Production`, `Crafting`, `Farm`, `Generator`,
`WindGenerator`, `RainCollector`, `Torture` 87, ou 152 sans place d'opérateur (+0x3AC) ;
`ResearchBuilding` 87 ; `StorageBuilding` et `FurnaceBuilding` 26 ; `TurretBuilding` 146 ;
`UseableStuff` et `LightBuilding` voir plus bas). Le clic droit sur un ennemi (`0x7FBB5C`) : 235 au
mode tir, sinon 5 puis **16** (`ATTACK_ENEMIES`) ajouté derrière, même cible.

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
| 9, 10, 11, 13, 15, 16 | se battre contre les ennemis alentour, défendre (16 : après l'attaque du clic droit) |
| 31 / 45 | rester près / garde du corps |
| 60 | premiers soins (robot) |
| 61 | attaque sans provocation (`UNPROVOKED_FOCUSED_MELEE_ATTACK`) |
| 71 | apporter les matériaux à un chantier |
| 88 à 94, 113, 151, 247 | transporter, tout faire tourner, débloquer, récolter la sortie, remplir, vider une machine |
| 125 | bâtisseur (métier) |
| 150 | prospecter |
| 166 | enchaîner |
| 185, 186, 201, 219 | couper, forcer, crocheter des chaînes ; ôter les siennes |
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

**Panneau d'inventaire d'un bâtiment** : `Building::select` (vtable +0x130, `0x5504A0`, commun à
toutes les classes de bâtiment) joue le son du bâtiment puis appelle `showInventoryBuilding` **seulement
si** son chantier est fini (`getBuildState()` +0x228, octet 0) **et** sa faction (+0x58) est celle du
joueur (`Faction`+0x250 non nul). Un filon ou une mine (fonctions 27 et 1) n'a pas de propriétaire
tant que personne ne le travaille : `Task_OperateMachine` (vtable +0x8, `0x35BA3E`) appelle
`Building::setFaction(faction de l'ouvrier, nullptr)` (vtable +0xA0, `0x557950` ; pour les fonctions
1 et 27 il inscrit aussi le bâtiment dans la zone quand il passe de « personne » à une faction, puis
propage aux meubles et à l'intérieur). Chez un client, où la tâche ne tourne pas, le mod impose le
propriétaire de l'hôte (`kMachOurs`, `KenshiWorld::SetBuildingOurs`).

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
0 bâtiment ; 1 personnage ; 2 arme ; 3 armure ; 7 race ; 0x5B personnage animal.

Ce sont aussi les types de `hand` d'un perso : 1 un `CharacterHuman`, 0x5B un `CharacterAnimal`. Le
**modèle** d'un animal a le type 76 (`ANIMAL_CHARACTER` : « scale min », « scale max », références
« race » et « death items ») ; celui d'un humanoïde, le type 1.

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

### Diplomatie : factions entre elles, personnages uniques, villes [D]
Trouvé par désassemblage le 10/10 (rien de vérifié en jeu).
- **`UniqueNPCManager`** (`*0x212EB00`) : l'objet est une `boost::unordered_map<GameData*,
  UniqueCharacterState>` (nombre de seaux +0x18, taille +0x20, seaux +0x38). Nœud : suivant +0,
  empreinte +8, clé `GameData*` +0x10, valeur +0x18 = `{ GameData* data ; hand escouade (0x20 octets,
  type 0xB) ; int état ; bool joueur }`, soit `data` +0x18, `hand` +0x20, **état +0x40**, **joueur
  +0x44** (dans la paire de `operator[]` `0x349950` : +8, +0x10, +0x30, +0x34).
  - États : **0 mort, 1 vivant, 2 emprisonné**. `getState` (`0x5E87F0`) renvoie 1 pour une entrée
    absente ; `isPlayerInvolved` (`0x9B0D00`) renvoie l'octet « joueur » (faux si absente).
  - Écrivains : `Character::declareDead` (`0x7A6200`, à `0x7A6405`) met 0 et « joueur » si la faction
    du perso est celle du joueur (`Faction`+0x250) ; `0x5CF470` (mise à jour d'un prisonnier :
    `Character`+0x3D4, son geôlier) met 2 (« joueur » si le geôlier est la faction du joueur) puis 1 à
    la libération ; `0x7CD570` met 0 ; `0x5E87A0` remet 1 ; `0x34AE20` met 2 ou 1.
  - Sauvegarde `0x9A8660` / chargement `0x9AA270` : clés `usedUniques`, `usedUniquesState`,
    `usedUniquesPlayer` (le chargement crée l'entrée par `operator[]` puis écrit `data` = la clé).
- **États du monde** : `WorldEventStateQuery::evaluate` (`0x9A7E00`) ne lit que : l'état de
  personnages uniques (`getState`, et `isPlayerInvolved` si la requête +0x140 le demande), et
  `FactionRelations::isAlly` / `isEnemy` de factions envers la faction du joueur. Avec les mêmes
  uniques et les mêmes relations du joueur, un client évalue les mêmes états du monde.
- **`FactionRelations::isAlly(Faction*)`** `0x6B2630` : soi-même, ou `alliance`, ou relation ≥ 50
  (relation par défaut +0x60 si pas d'entrée). **`isEnemy`** `0x6B26D0` : relation ≤ −30.
- **`FactionRelations::declareWar(Faction*)`** `0x6B3000` (pas d'appel direct : vtable ou pointeur) :
  `war` = 1, relation ramenée à −35 si elle était au-dessus, message « War breaks out between {1}
  and {2} » ; `FactionRelations`+8 = sa faction. Non détournée : ses appelants n'ont pas été
  retrouvés ; s'ils sont les actions de dialogue ou les campagnes, ils sont déjà refusés chez le client.
- **`TownList`** (`*0x2134100`, hérite de `RootObjectContainer`) : +0x50 `lektor<TownBase*>`
  (nombre +0x58, tableau +0x60). `TownList::getNearestTown` `0x927F10` la parcourt.
- **`TownBase`** (`Town` : vtable `0x1735BA8`, `TownBase` : `0x1734F48`, hérite de `RootObject`) :
  +0x10 faction propriétaire, +0x18 nom, +0x40 `GameData*`, +0x48 / +0x50 position x / z, +0xD8 type
  (`getNearestTown` traite 8 à part), +0x110 drapeau remis à 0 par `setOverride`, **+0x338 variante appliquée
  (`GameData*`)**, +0x380 « is public », +0x384 « no-foliage range ».
  - `setOverride` `0x9FE7A0` : écrit +0x338, recopie de la variante `no-foliage range`, `type`,
    `is public`, cherche sa référence `faction` et, si elle diffère, appelle `setFaction` (vt 0xA0).
  - `setFaction` `0x9287D0` : retire la ville de la liste de son ancienne faction (`Faction`+0x88 → +0x10),
    l'ajoute à la nouvelle (sauf faction factice +0x1D0), écrit +0x10. Une faction nulle prend celle
    par défaut.
- **Choix de la variante d'une ville** : au chargement d'une zone (`0x9FFAD0`, objets de type 0xD),
  `0x9FF5A0(gestionnaire de zones *0x21349C0, ville)` : rien pour une ville du joueur ; sinon, si la
  ville n'est pas déjà dans la table du gestionnaire (+0x168148), parcourt les références
  `override town` de la ville, garde celles dont tous les `world state` sont vrais et différentes de
  la variante actuelle, prend **la plus lourde** (`0x5778D0` garde le maximum : déterministe), appelle
  `setOverride` et note la ville dans la table. Puis la zone applique les listes `0-buildinglist` /
  `1-itemlist` de la variante. La table n'est pas sauvegardée : la décision est reprise à chaque
  chargement de partie.

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
- **Touche Maj et travaux permanents** (désassemblage 1.0.68, v0.3.4) : l'octet global `0x2133449`
  vaut 1 quand Maj est tenue (gestionnaire de touches `0x82B111` : codes 0x2A et 0x36 ; Ctrl `0x2133448`,
  Alt `0x213344A` ; remis à zéro avec Alt par le mot écrit en `0x8130D2`).
  `newPlayerTaskSelectedCharacters` (`0x7FA650`) le lit **lui-même** : Maj tenue (et la `TaskData`
  de la tâche +4 non nulle) → `addJobSelectedCharacters(tâche, cible, shift = 1, add, Vector3 zéro)`,
  puis la tâche suivante de la `TaskData` (+0x10) s'il y en a une, et le bâtiment visé sans
  propriétaire devient celui de la faction du joueur (vt +0xA0) ; sinon → `addOrderSelectedCharacters`
  (ordre ponctuel). Cas particulier : tâche 26 (fouiller) avec Maj sur un bâtiment (`hand` +8 = 0)
  devient 124 (`OPERATE_STORAGE`, ranger) si le bâtiment a un stockage (vt +0x2F8), sinon rien.
  `addTaskNearestSelectedCharacter` (`0x7FAE70`) lit aussi l'octet (tâche 26), `Character::addOrder`
  (`0x5D20D0`) reçoit `shift` en paramètre (vrai : `Character::addJob` permanent, si la faction est
  celle du joueur et la tâche le permet). Le bouton MEDIC (`0x7FAC20`) passe l'octet en paramètre à
  `addJobSelectedCharacters`. Texte du jeu : « To add jobs, hold shift key when giving an order ».
  Le mod vérifie l'instruction qui le lit (`0x7FA6A3` : `cmp byte ptr [rip+disp], 0`) avant d'y
  toucher (`kenshi::ShiftHeld`, `kenshi::ScopedShift`).
- **Bouton JOBS** (ordre 15, `OrdersReceiver` +0x35) : ne crée aucun travail ; lu par le choix du
  travail (`0x50E801`, `0x510FBB`) : éteint, le perso ne fait aucun de ceux de sa liste (« If this is
  disabled then the character won't do any of the jobs listed »).
- `OrdersReceiver::addJob` (`0x5086D0`) en permanent : ne regarde **pas** les places libres de la
  machine (un travail sur une machine pleine est mis dans la liste, le perso attend) ; ne l'ajoute pas
  si un travail du même type (`Tasker` +0x70 → +0x44) existe déjà sans cible (+8 nul) ou sur la même
  cible.
- **Transport 88 sans cible** (`addJobSelectedCharacters(88, nul, shift = 1, …)`) : le jeu n'ajoute
  **aucun** travail (vu le 13/10 chez l'hôte et chez le client : liste 1 -> 1, et dans les parties
  `workstations`) ; il lui faut une cible. Le travail « suivre » 31 sur un autre perso, lui, va bien
  dans le panneau Tâches.
- **Reprise d'un travail permanent** (vu le 13/10, `clientjobs`) : quand une place se libère sur la
  machine pleine, le choix du travail du jeu y envoie le perso de lui-même (1904 -> 125 unités, sans
  ordre). Un combat passe avant : arrivé au filon de la ville au milieu d'une bagarre, le perso se bat
  (défense, robustesse, esquive, combat de masse qui montent) et ne s'assoit pas ; en mode dieu le
  combat ne finit jamais. Un ordre du joueur, lui, le met au travail tout de suite.
- **Panneau Tâches : quand le jeu le reconstruit** (désassemblage 1.0.68, v0.3.4). La liste de travaux
  sous la barre d'escouade est `MainBarGUI` (`*0x21337C0`, soit l'objet GUI `0x21337B0` +0x10)
  +0x190. Le jeu l'écrit à `0x72B5F6` dans le global `0x2132C28`.
  - *Sa reconstruction* (`0x7263B0`) vide le `MyGUI::ItemBox` (+0x20 → +0xF0) en gardant le
    défilement. Elle ajoute une ligne par travail du perso, tiré de son propre `hand` (+0x8…, recopié
    de `Character`+0x60 par `0x726930`).
  - *Ses lignes* : `lektor` à +0x40 (nombre +0x48, tableau +0x50, 0x40 octets par ligne). Dans une
    ligne : +0x0 le `Tasker*` de `Character::getPermajobData(i)`, +0x8 l'emplacement, +0x38 « actif ».
    « Actif » vaut 1 si JOBS est allumé (`OrdersReceiver`+0x35) ; sinon, c'est le test du jeu
    `0x50C1C0` sur le travail.
  - *`0x726930(panneau, perso)`* met aussi l'état des boutons de la barre (ordres 12 à 15, etc.) puis
    appelle la reconstruction. `0x729B70(barre, hand*)` résout le `hand` (types 1, 0x5B, 0x22) et
    appelle `0x726930`, qui ne fait rien sans perso.
  - *`0x6E2050(GUI)`* = `0x729B70(GUI+0x10, GUI+0x2C0)`. Le `hand` GUI+0x2C0 (`0x2133A70`) est le
    perso affiché. C'est celui que lit `setOrderSelectedCharacters` (`0x7F3880`).
  - *Appelants* : seuls les chemins du jeu reconstruisent le panneau.
    - la croix d'une ligne (`0x7268B0`) et `OrdersPanel::removeJob` (`0x7265A0`) : `removePermajob`,
      puis la reconstruction ;
    - le glisser (`0x7265F0`) : `movePermajob`, puis la reconstruction ;
    - la fin de `newPlayerTaskSelectedCharacters` (`0x7FA824`) et le bouton MEDIC (`0x7FAE11`) :
      `0x6E2050` ;
    - `OrdersReceiver` `0x508210` (bascule JOBS), si le perso est celui de `0x2133A70` : il poste
      l'événement GUI 6 (liste `0x21346D8`, seul poste de ce type relevé). Le traitement des
      événements (`0x7B65B0`, cas 6 en `0x7B66C1`) appelle `0x6E2050` si le `hand` est celui de
      `0x2133A70` (ou nul).
  - *Ce qui ne reconstruit rien* : `Character::addJob` (`0x5C8DA0`) et `addJobSelectedCharacters`
    (`0x7F5A90`), ainsi que `removePermajob`, `movePermajob` et `removeJob` appelés directement.
  Le mod appelle ces fonctions dans trois cas : un client qui montre d'avance un travail (son
  `newPlayerTaskSelectedCharacters` est bloqué), `ApplyJobList`, et l'hôte qui exécute l'ordre d'un
  client. D'où le panneau en retard (rapport du 13/10).
  - *Correctif* : `KenshiWorld::UpdateJobsPanel` (fin de chaque image, en session) compare les lignes
    au perso de `0x2133A70` (`kenshi::ReadJobsPanel`) et appelle `0x6E2050` s'ils diffèrent
    (`kenshi::RefreshJobsPanel`, entrée `FnGuiRefreshJobs`).
  - *Vérifications* : la disposition est contrôlée sur les instructions `0x7263EB` (`mov [rsi+0x48]`),
    `0x7264D1` (`mov rax,[rsi+0x50]`) et `0x7B66E0` (`lea rcx, GUI`). Le pointeur de la barre et du
    panneau doit être non nul : `0x729B70` / `0x726930` lisent le panneau sans le tester.
- **Un poste sans rien à faire n'a pas d'ouvrier** (vu le 13/10, `workstations`) : cinq persos de
  l'hôte et celui du client avec un travail permanent 87 sur une ferme, une cuisinière, un établi, un
  banc de recherche et un puits posés en plein désert : 0 ouvrier partout pendant 2 min à x3. Aucune
  recherche en file, cuisinière vide, établi sans matière ni commande, ferme de chanvre qui ne pousse
  pas (désert). Avec une recherche en file et des livres, un perso s'assoit au banc (`research`). Le
  minerai d'un filon, lui, est toujours à prendre (mine 3/3).
- **Nom ou identifiant** : les noms des modèles sont traduits (« Cuisinière » pour
  `1944-gamedata.base`, la paille de blé `1913-gamedata.base` ne s'appelle pas « Ble ») ; les commandes
  de test qui cherchent par nom acceptent aussi l'identifiant. Une machine peut avoir plusieurs
  sections d'inventaire ; un objet refusé par l'une (« no room ») peut aller dans une autre.
- `OrdersReceiver::movePermajob(from, to)` (`0x5078A0`, via `Character::movePermajob` `0x5C8FC0`) :
  retire le travail `from` puis l'insère à `to` (indice après retrait) ; `to = -1` : à la fin. Ne
  vérifie pas `from` (lecture hors du tableau si trop grand : le mod le vérifie).
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
  pas de lien vers leur bâtiment (`isFurnitureOf`, +0x238, est vide) : on le retrouve parmi les
  objets autour dont l'intérieur (+0x1F0) a un plan (+0x30) qui pointe vers eux (+0x90)
  (`GiveShopHome`).

**Mouvement**
- Les PNJ lointains ne sont déplacés que quelques fois par seconde le long de leur chemin. Une
  position écrite sur eux ne tient pas ; les personnages du joueur l'acceptent à chaque image.
  « Lointain » = hors de la vue de la caméra (`Character+0x1A9` à 0) : c'est la mise à jour réduite de
  `CharMovement` (`0x148AC0`) ; les persos de la faction du joueur sont toujours dans la vue. Le mod
  met dans la vue les PNJ qu'un client voit (`hk_charUpdateView`) [D, faraway].
- **Pendant la pause, le jeu n'enregistre aucune position écrite** sur un personnage.

**Ragdolls**
- `pickupObject` (0x5CFF90) ne fait rien si le porteur est en ragdoll, porte déjà, ou si le corps a
  son drapeau ragdoll (+0x3d4) ; son étape d'attache (0x5CED90) remet ce drapeau à 1 sur le corps porté
  (un corps porté se lit donc « ragdoll »).
- Téléporter juste avant le début d'un ragdoll projette le corps : la vitesse vient des écarts
  entre les dernières poses.
- Une téléportation ne déplace pas un ragdoll actif.
- `Character::ragdollMode` (`0x5CBD60`) ne fait que mettre la demande en file (+0x3E0) ; appliquée,
  elle passe par `AnimationClass::setRagdoll` (`0x5B9A10`) : à l'arrêt, le ragdoll (`AnimationClass`
  +0x2E8) est **détruit** ; au démarrage, un ragdoll neuf est créé **à la position du nœud de scène**
  (`AnimationClass`+0xB0, `Node::getPosition`), remis sur le perso seulement si ce nœud est à
  `Vector3::ZERO`. La position du perso suit ensuite la racine du ragdoll (`0x7D16F0`, appelée par la
  mise à jour `0x5CF130`). `CharMovement::setPositionDirectionAndTeleport` (vt 0xC0, `0x65E9E0`) ne
  bouge pas ce nœud ; `Character::setPosition` (`0x5CA680`) et la remise d'un corps posé (`0x5CCBA0`,
  depuis `dropCarriedObject`) le font, par `AnimationClass` `0x5B1B00` (`Node::setPosition` si +0x88).
  Un corps K.-O. tenu debout puis téléporté gardait son nœud là où il gisait et s'y recouchait
  (0.3.4 : `kenshi::PlaceBodyNode`).
- Le nœud a une **ancre**, `AnimationClass`+0x258 (Vector3 ; +0x25C, la hauteur, est lissée). La
  mise à jour de l'animation (`0x5B5590`, table virtuelle d'`AnimationClass`, slot à `0x16F12A0`) et
  `0x5B5980` remettent le nœud dessus à chaque image. Seule `AnimationClass::<set position and
  direction>` (`0x5B18F0`, notre nom) écrit l'ancre : +0x258 = position abaissée de +0x2C8 − +0x2CC,
  +0x25C = hauteur si le propriétaire le veut, orientation (+0x78) seulement si la direction n'est
  pas nulle en x/z. Elle est appelée par la mise à jour de `CharMovement` (`0x65FFA0`, avec sa
  position +0xC4 et sa direction +0xD0 ; sautée tant que son +0x320 est nul), par `0x7D3130`
  (position du perso) et par `0x62B210`. Un `Node::setPosition` seul est donc défait à l'image
  suivante si rien n'appelle cette fonction. C'était le cas pour un corps K.-O. relevé et téléporté
  (`suite` 13/10 16:19 : nœud « 1066 units away again » 0,3 s après, recouché à 1147 de l'hôte).
  Depuis 0.3.4, `kenshi::PlaceBodyNode` déplace le nœud et son ancre.
- Reconstruire un ragdoll le lance en l'air.
- Un corps qui s'effondre ne reste pas où étaient ses pieds : sa position (le bassin) finit à une
  distance à peu près fixe, d'un essai à l'autre, de l'endroit où il était debout (7 à 12 unités
  pour un humain, 47 à 79 pour une chauve-souris terrestre ou une araignée ; stress4).
  Posé d'un portage chez l'hôte, le corps tombe de l'épaule à ~2,5 unités du porteur ; chez le
  client, lâché debout au point de l'hôte puis mis en ragdoll, il finissait à 12 unités (`carry`,
  13/10). D'où le second essai, une fois, décalé de ce que la première chute a mesuré (0.3.4).
  Second essai du 13/10 : la chute n'avait rien décalé (-0,1 ; -0,2), le corps est retombé au même
  point au millième près. Le corps posé de l'hôte se lit à la hauteur de l'épaule (17,5 unités
  au-dessus du sol du porteur) ; un perso téléporté (`setPositionDirectionAndTeleport`) à un point en
  l'air dans une maison à étage est posé par le jeu sur le navmesh le plus proche en 3D (ici
  l'escalier, 12 unités plus loin, 10 plus haut). Notre copie est donc mise debout à la hauteur du
  sol de son porteur (`kc::motion::FallSpotHeight`).
  Troisième essai (15:05) : ce n'était pas la hauteur. Posé sans ragdoll (`dropCarriedObject`), le
  corps K.-O. est remis par le jeu **là où il gisait avant d'être ramassé** (lu dans l'image même du
  dépôt : 24660,0 / 10545,1, son point de `spawnnpc 12 4`), et `setPositionDirectionAndTeleport` ne le
  déplace plus (position inchangée après 0,6 s de téléportations). Son ragdoll part du nœud de scène :
  le mod met ce nœud sur le point de chute (`kenshi::PlaceBodyNode`) avant de le faire tomber.

**Heure**
- L'heure du jeu (`0x21303D0`, +0xA0) est recalculée à chaque image (`0x66FF60`) : jour (+0x8,
  entier) × 24 + heure du jour de l'horloge du ciel (+0x20 → +0x1C, float). Écrire +0xA0 n'a
  aucun effet durable ; écrire le jour puis appeler `0x66CF30` (heure du jour) en a un (c'est ce
  que fait une nouvelle partie). Au chargement (`0x66DB50`), jour, heure et minute viennent de la
  sauvegarde.

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
- Ouvrir la fenêtre de conversation met le jeu en pause (`userPause(true)` dans `0x727820`).
- Hors pause, une conversation commencée par une bulle se ferme ou avance toute seule quand la
  minuterie de la bulle (2 s + 0,12 s par caractère) est écoulée (`Dialogue::update`) : chez l'hôte,
  la conversation d'un client n'a pas cette pause (0.3.4 : le mod garde la minuterie).
- Une téléportation ou un combat d'un des deux persos ne termine pas la conversation dans le jeu de
  l'hôte (essai du 13/10 : toujours ouverte 14 s après une TP de 2100 unités, 19 s après le début
  d'un combat ; la réplique du PNJ, plus de 20 s, n'était pas écoulée). C'est le balayage du mod
  (`SweepDialogs`) qui la termine (`endDialogue`).
- « Parler » n'est proposé par le clic droit que si le jeu trouve une réplique « parle-moi » sur la
  copie locale du PNJ (`0x67AC00`) ; sinon le clic ne fait rien du tout.
- `startConversation` / `startPlayerConversation` sont appelés sur le `Dialogue` de l'un ou l'autre
  des deux personnages : le mod regarde les deux côtés (propriétaire du dialogue et cible).

**Fabrique de persos et modèles uniques (0.3.2) [D]**
- `RootObjectFactory::createRandomCharacter` (`0x5836E0`) lit le booléen `unique` du modèle
  (table des booléens du `GameData`, +0xF8). S'il est vrai, il demande au registre des uniques
  (`0x1C84B` puis `0x50E5C`) si ce modèle existe déjà dans le monde : si oui, il prend un modèle de la
  liste « unique replacement spawn » du modèle, et **rend null** si elle est vide ; si non, il inscrit
  le modèle au registre. Puis il appelle `RootObjectFactory::create` (`0x583400`) et `vt 0x308` du
  perso.
- Les recrues de Dialogue.mod (`41370-Dialogue.mod` « hire engineer wall man », `41299-Dialogue.mod`
  « Sword for hire ex-slave ») et les persos nommés (Cat, Ruka, Kang) sont uniques. Un client qui a
  chargé la sauvegarde de l'hôte les a déjà au registre : il ne pouvait pas en faire la copie
  (« factory did not return a character (template type 1 …) »). `kenshi::CreateCharacter` met
  `unique` à faux le temps de l'appel puis le remet.
- L'embauche par dialogue ne crée pas de perso : elle recrute le PNJ de la conversation
  (`PlayerInterface::recruit`).
- Renvoyer un perso de la faction (fenêtre d'escouade) : `SquadManagementScreen::dismissCharacter`
  (`0x48FA30`, KenshiLib `0x48F2B0` + 0x780) : `setFaction` (vt 0xA0) vers une faction trouvée par
  nom, puis une nouvelle escouade et son IA. Le mod ne l'appelle pas (jamais de perso d'escouade
  supprimé ou renvoyé par le mod).

**Sauvegarde**
- `SaveManager` efface sa demande avant que tous les fichiers soient écrits : il recopie son
  dossier de travail dans l'emplacement ensuite.
- Il faut attendre que `quick.save` existe et que la taille du dossier ne bouge plus.

**Interface**
- Un glisser-déposer de souris simulé ne marche pas dans l'interface de Kenshi ; les clics et
  clics droits, si (avec un léger mouvement relatif).
- Le jeu lit clavier et souris par DirectInput, via OIS.

### Déplacement loin de la caméra, murs et navmesh (3f, lu au désassemblage) [D]
- `CharMovement::update` (`0x65FFA0`) ne prend la mise à jour complète `HavokCharacter::update` (`0x147FD0` : chemin, arêtes du navmesh comme murs) en `0x660A3D` que si le perso est dans la vue (`Character`+0x1A9) **et** si le pas fait moins de 0,5 s de temps de jeu (`0x660A47`, flottant en `0x167E8D8`) ; sinon `HavokCharacter::moveAlongPath` (`0x148AC0`) d'un coup. Un perso hors de la liste « chaque image » (+0xE4) rattrape son temps en un seul pas (souvent plus de 0,5 s), et un long gel de l'hôte à x2/x3 fait de même pour tous.
- `CharMovement` : +0x320 `HavokCharacter*`, +0x3A8 le perso. `HavokCharacter` : +0x14 clé de face du navmesh (-1 : hors navmesh, `isOnNavmesh` `0x6646B0`), +0x88 état (`0x664690`), +0x90 état du chemin (`0x6646A0`). `NavMesh::getPositionValid(const Vector3&)` : `0x3A2930` (KenshiLib `0x3A2410`, +0x520 ici).
- **Aucune règle de caméra trouvée pour le navmesh ni la physique des bâtiments.** Le navmesh d'une zone est créé (`NavMesh::create(ZoneMap*)`, `0x3AC040`) à l'activation de la zone (`0xA0E740`, `0xA0EC1B`, `0xA0F052`, `0xA0F3AD`), sous la seule condition `0x25068` sur `[0x2134128]`, que la zone soit activée par la caméra ou par un perso du joueur (`ZoneManager::playerActivate` `0xA0F640`, type 1). `RootObject::loadUnloadCheck` (`0x594350`) crée ou détruit la physique d'un bâtiment (vt +0x200 / +0x208) selon que sa zone est chargée, pas selon la caméra. Le générateur (`validateBuildingStates` `0x3C7560`) attend seulement que les bâtiments soient créés (vt +0x488 `isCreated`). Le seul décalage lié à la caméra : `NavMesh::update` (`0x3AE490`) recentre le monde Havok (`shiftWorld`) sur la zone de la caméra, sans rien décharger.
- `CharMovement::combatMovementUpdate` (`0x2AF1E0`) écrit l'octet +0xE4 **de `CharMovement`** (pas celui du perso) : ce n'est pas la liste « chaque image ».

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
  - détail [D] : `getOwnerships` (`0x7956A0`, par le thunk `0x4BCB3`) ; `cmp [own+0x40], 0xB` (aucun
    domicile) ; `hand::isValid` (thunk `0x3F954`) ; le type 0 (bâtiment) se résout par `0x9F8F20` ;
    puis `Building+0x1F0` → `0x54ACB0`. Celle-ci prend d'abord l'ensemble de `hand` de l'intérieur
    (+0x100/+0x108/+0x120, chaque `hand` de type 0 résolu), et seulement s'il ne donne rien, tous
    les meubles de l'intérieur (+0x88 nombre, +0x90 données) dont vt 0x20 renvoie 0 ;
  - pour un PNJ, `getOwnerships` est celle de son escouade : une **doublure** créée par le mod
    (`createRandomCharacter`) n'a pas de domicile, et sa fenêtre ne montre rien (les sacs de son
    escouade, vides). C'était la fenêtre vide des clients (0.3.0) : `kenshi::GiveShopHome` écrit
    dans `own+0x38` les champs du `hand` du bâtiment des comptoirs (`FurnitureParent`, celui dont
    `0x54ACB0` rend le plus de ces comptoirs) avant d'ouvrir la fenêtre.
- **Sacs à dos** [D] : un sac est un `ContainerItem` (vtables `0x170F4F8`, et `0x170F4D8` pour sa
  seconde base). `RootObject::getInventory` est l'emplacement vt 0x160 de tout objet : `Item` le
  laisse à `0xD2280` (renvoie null), `ContainerItem` le remplace par `0x76BE60` (renvoie
  `this+0x290`). vt 0x2C8 vaut `0x76BE50` (renvoie 0x2E) pour un `ContainerItem`. `HandleTable::resolve`
  (`0x2676E0`) résout les `hand` d'objets (types 2 à 0x5A, sauf quelques-uns) par la liste générique
  en +0x50 : le `hand` d'un sac porté se résout comme celui d'un objet au sol.
- `0x953F90` (appelé par `showTraderInventory`) **détruit le `ShopTrader` précédent et en crée un
  neuf** à chaque ouverture : rouvrir la fenêtre suffit à montrer un stock changé.
- **Ce que la fenêtre garde du marchand** (lu le 13 octobre 2026 pour faire passer la fenêtre d'une
  doublure au vrai PNJ, `kenshi::RetargetTradeWindow`) [D] :
  - `ShopTrader` (vtable `0x1736D48`, RTTI `ShopTrader`, 0xD0 octets) est un `RootObjectBase` à lui
    (son propre `hand` à +0x58, type 0x5B ; son nom copié en +0x18 ; sa faction, celle du marchand,
    objet global) ; **+0xC0 le marchand** (`Character*`, écrit une fois par le constructeur
    `0x953850`) ; +0xC8 son `ShopTraderInventory`, dont le propriétaire est le `ShopTrader` (et non
    le marchand) et les sections celles des comptoirs (meubles) ou des sacs de l'escouade ;
  - le `ShopTrader` vit dans l'emplacement `[0x212FA08]` (le gestionnaire que `showTraderInventory`
    passe à `0x953F90` avec le `hand` du marchand) ;
  - la fenêtre du marchand (`InventoryTraderGUI`, vtable `0x1707618`) : +0x50 le `ShopTrader` ; vt 0x70
    `getInventory` (`0x70D520`), vt 0x78 `getCallbackCharacter` (`0x70D540` : `ShopTrader`+0xC0), vt
    0x80 `getCallbackObject` (`0x70D560` : le `ShopTrader`) ;
  - `showTraderInventory` range la fenêtre dans la table des fenêtres ouvertes (`gui`+0xA0) et dans
    `gui`+0x120 sous le `hand` **du `ShopTrader`**, pas celui du marchand ;
  - `_showTradeWindow` (`0x7918A0`) passe à `addTradePartner` (`0x70F2A0`, arguments : fenêtre,
    paiement, peut poser, joueur, `hand*`) le `hand` du marchand (`b`+0x58), copié par valeur dans les
    données (+0x10 : type +0x18, conteneur +0x1C, série du conteneur +0x20, index +0x24, série +0x28
    dans les données) ;
  - `getNPCTrader` (`0x70E2D0`) et `isWithinRangeToTrade` (`0x70D290`) relisent à chaque appel vt 0x78
    / vt 0x80 de chaque fenêtre ; le rafraîchissement du `ShopTrader` (`0x954800`, `r8d == 0xF`) lit
    +0xC0 puis `getMoney` (vt 0x1B8) : le plantage du 12/10 (`kenshi_x64+0x95484B`) sur un marchand
    libéré ;
  - donc, pour changer de marchand sous une fenêtre ouverte : écrire `ShopTrader`+0xC0 et les champs
    du `hand` du partenaire, rien d'autre ; à condition que les sections ne soient pas des sacs de
    l'escouade de l'ancien marchand (marchand ambulant : la fenêtre se ferme). Argent d'un PNJ :
    `Character::getMoney` (`0x791660`) et `getOwnerships` (`0x7956A0`) passent par l'escouade
    (+0x658 → +0x78 → vt 0x98 → +0x88) ; sans escouade, un `Ownerships` statique commun.
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
- **Animaux et dépeçage** (0.3.1) [D] : un animal mort se fouille par la même tâche 26. Il n'y a pas
  de tâche de dépeçage (RTTI : seulement `Task_Loot_Order`, `Task_Loot_AI`,
  `Task_Loot_ResourcesWeHaveStorageFor`, `Task_StripWeapons`, `Task_FeedCorpseIntoMachine`). La
  viande, les peaux, les dents, les perles sont les « death items » du modèle `ANIMAL_CHARACTER`
  (léviathan : « Leviathan Pearl » ; taureau : peau, viande, corne…), dans son inventaire une fois
  mort. « {1} is butchering {2} for meat!! » (`0x16C1780`) n'est qu'une notification de la fonction
  `0x355AD0` de la fouille, quand le corps est de la faction du joueur. Le début de l'action
  (`0x354DC0`) ne vérifie aucune distance : l'approche est celle du système de tâches.
- **Taille des corps** (données du 1.0.68, race : rayon de pathfinding / coque X×Y×Z ; modèle :
  échelle min-max) : humanoïdes 2,6 à 4 / 5×17×5, échelle 1 ; chèvre 4 / 8×12×8, 0,35-1 ; raptors
  6 / 10×16×10 (mégaraptor jusqu'à 3,5) ; taureau, garru 7 / 15×20×15 ; chien d'os, loup 10 /
  10×15×10 ; chauve-souris 10 / 12×17×12 ; crabe 10 / 25×22×25 (mégacrabe 2,25) ; araignées de fer
  et de sécurité 10 / 25×22×25 ; gorillos 12 / 21×26×21 (roi 2,5) ; araignées 14 / 25×22×25 ; bête à
  bec 16 / 18×50×18 ; skimmer 22 / 36×38×36 ; bête en cage 24 / 35×40×35 ; tortue des marais 25 /
  50×40×50 (1,5) ; léviathan 40 / 75×90×75, 0,4-1,5. Table complète : `kc::loot::CreatureRaces`.

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

- **Une porte peut rester « en fermeture » (3) pour de bon** chez l'hôte (11 octobre : « Storm Shack
  Door stays 0 (host 3) » toutes les 3 s pendant 40 min chez les trois clients ; sans doute loin de la
  caméra de l'hôte, où la porte n'est pas mise à jour, ou bloquée). Pour un client, fermée (0) et en
  fermeture (3) reviennent au même, comme ouverte (1) et en ouverture (2) : `DoorState::sameSettled`.
  Depuis 0.3.2 le client ne réapplique plus une porte déjà dans le bon sens, et abandonne après 3
  essais une porte que son jeu ne veut pas tourner, jusqu'à ce que l'état de l'hôte change.

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
  - [D, 0.3.4] `off` (lit nul accepté) : retire le perso des occupants du meuble nommé par `inWhat`,
    met `inSomething` à 0, `inWhat` à vide (type 0xB), puis rafraîchit le corps (`Character`+0x648 vt
    0x48 → +0x448 → +0xE8). Sans danger sur un perso qui n'est dans rien, mais il écrase un
    `inSomething` de 2 : ne l'appeler que sur un perso au lit (1).
- **Porter un perso couché** [D, 0.3.4] : `pickupObject` (`0x5CFF90`) appelle `setPrisonMode(false)`
  sur un corps en cage (2) mais pas `setInBed` sur un corps au lit (1) ; c'est son étape d'attache
  (`0x5CED90`) qui appelle `setInBed(false, null)` puis `setPrisonMode(false, null)` sur le corps et met
  son `inSomething` à 0. Un dormeur pris dans son lit en sort donc proprement chez le client aussi
  (Meow, 12/10 23:18:52) : le « free » de l'hôte qui suit ne trouve plus de lit à quitter.
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
- **Matériaux d'un chantier** (la jauge) [D] : liste à état+0x18 (nombre, `uint32`) / +0x20
  (données : pointeurs) ; une entrée : +0x0 `GameData*` du matériau, +0x8 quantité voulue, +0xC
  quantité apportée (flottants). Le panneau du chantier (`0x302380`, `0x302921`–`0x3029B5`) dessine
  apportée / voulue, sauf pour un bâtiment terminé. Les livraisons n'ont lieu que chez l'hôte :
  le client reçoit les quantités apportées (message 87 `BuildMaterials`, 0.3.1) et les écrit
  (`KenshiWorld::ApplyBuildMaterials`, bornées à la quantité voulue, chantier non terminé).
  `addConstructionProgress` ne réécrit +0xC que quand le chantier repart de zéro (vt 0x330).
- **Collision d'un bâtiment terminé** [D] : la construction de son apparence (vt 0x200 `0x5609E0`
  → `0x553DC0`) crée ses corps physiques (liste `Building+0x250`, nombre +0x258, données +0x260)
  **éteints** : +0x269 (collision allumée) = 0, +0x268 (physique) = « terminé ». Seuls les
  allument :
  - vt 0xE0 (`0x5626A0`, base de `Building` et `UseableStuff` ; `0x5628B2`–`0x562955`) : visible
    (+0x19D), physique (+0x268), terminé (vt 0x228 → +0) et +0x269 encore à 0 → `0xFBD70` sur
    chaque corps, +0x269 = 1 (et l'inverse quand une condition tombe) ;
  - vt 0x100 (`setVisible`, `0x548F70`), mais seulement quand la visibilité change (sortie à
    `0x548F86` sinon).
  `notifyConstructionComplete` (`0x562340`) : terminé = 1, vt 0x100(1) (sans effet : le chantier
  est déjà visible), changement des matériaux du mesh, vt 0x318(1), vt 0x310(0), vt 0x460(1),
  puis vt 0x468(1) (`0x2973E0` : +0x268 = 1) ; jamais vt 0xE0. Les bâtiments ne sont pas mis à jour
  à chaque image (`TownList::update` `0x92D850` n'appelle vt 0xE0 que pour les villes et objets à
  part) : un bâtiment fini en cours de partie restait **sans collision** jusqu'au rechargement de
  sa zone. Un bâtiment chargé d'une sauvegarde est créé terminé et `0x5609E0` appelle vt 0xE0
  (`0x56147F`). Le mod (`EnsureCollision`, `plugin/buildings.cpp`) appelle vt 0xE0 une fois quand
  terminé, visible, physique et +0x269 = 0 : chez le client après chaque état reçu, chez l'hôte
  pour chaque bâtiment suivi (rien si le jeu l'a déjà fait).
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

## 11. Carte du monde, caméra, barre d'escouade (marqueurs de carte, minicarte, pings) [D]

Adresses 1.0.68 traduites depuis KenshiLib 1.0.65 (`translate.py` : décalage +0x780 dans la zone
`MapScreen`, +0x30D0 pour ses données statiques) puis lues au désassembleur.

**Où est la carte** : `ManagementScreen::singleton` à `0x212F4F8` (lu dans `getSingleton`
`0x296CA0`, qui la crée si elle manque : le mod lit le pointeur, n'appelle pas la fonction ;
vtable `0x16DCDF0`). `ManagementScreen` : +0x8 sa fenêtre MyGUI (visible = l'écran de gestion est
ouvert, `refreshMap` `0x49B550` la teste ainsi), +0xA8 `MapScreen*` (`refreshMap` y appelle
`centerCamera`).

**`MapScreen`** (pas de vtable) : +0x10 widget principal (`getVisible` `0x48BE60` = `mainWidget->getVisible()`),
+0x18 `ScrollView` (partie visible), +0x20 `ImageBox` de la carte (texture `GUI_Map.dds` d'après
`data/gui/layout/Kenshi_OverviewWindow.layout` ; sa taille suit le zoom, sa position le
défilement), +0x194 `worldBounds` (x min, z min, …), +0x1A4 `worldSize` (x, z). Les bornes sont
écrites dans le constructeur (`0x490210`) : valables dès le chargement, carte jamais ouverte.

**Projection** (`worldToMapCoords` `0x48C3E0`, `TPoint<int>` rendu par pointeur caché, rcx =
`MapScreen`, rdx = sortie, r8 = position) :
`x_image = (x - worldBounds.x) / worldSize.x * largeur(image)`,
`y_image = (z - worldBounds.y) / worldSize.y * hauteur(image)`, arrondis ; la largeur et la
hauteur lues dans le widget (+0x28 / +0x2C de `MyGUI::Widget`, c.-à-d. `mCoord.width/height` de
l'`ICroppedRectangle` placé à +0x8). Le z croissant descend sur la carte : le nord (z
décroissant) est en haut. Le mod refait ce calcul à partir du rectangle absolu de l'image ; la
commande `mapproj` compare au résultat de la fonction du jeu.

**Ouvrir la carte** : onglet `MapTab` (« CARTE ») de la fenêtre de gestion
(`Kenshi_OverviewWindow.layout` : `Root` (Window) → `TabsMain` → `MapTab` → `MapScrollView` →
`Client` → canevas → `MapImage`), ouverte par le bouton `ShortcutMapButton` (« MAP »,
`Kenshi_MainPanel.layout`, colonne de raccourcis près de la barre d'escouade) ou par la touche
`toggle_map` de `controls.cfg` (réglage du joueur : le mod n'en suppose aucune ; dans la partie de
l'utilisateur, M est la caméra libre). Les widgets sont nommés `<préfixe>_MapImage`…, le préfixe
étant l'adresse de la `ManagementScreen` en hexadécimal sur 16 chiffres (lu en mémoire d'un jeu
lancé : `00000000D970B870_MapImage`). `MapScreen` : +0x10 `MapTab`, +0x18 `MapScrollView`, +0x20
`MapImage`, +0x28 `CameraMarker` (vérifié par les noms).

**MyGUI** (exports de `MyGUIEngine_x64.dll`, compilé avec MSVC 2010, appelés par `GetProcAddress`) :
- `Widget::getInheritedVisible` = `mInheritsVisible` (+0x41A) : **seulement les parents** (mis à jour
  par `_updateVisible` : parent nul, ou parent visible et lui-même « inherited visible ») ; la
  visibilité propre est `mVisible` (+0x46C, `Widget::getVisible`). Une fenêtre racine cachée par
  `setVisible(false)` répond donc toujours vrai à `getInheritedVisible` : « affiché » =
  `getVisible() && getInheritedVisible()`. (Le mod ne testait que le second : l'écran de gestion
  comptait comme toujours ouvert, d'où ni repères 3D ni minicarte, et les cases cachées de la barre
  d'escouade recevaient un cadre.)
- `Widget::getParent` (+0x450), `getName` (`std::string` MSVC 2010 à +0x428 : tampon de 16 octets ou
  pointeur, taille +0x10, capacité +0x18), enfants `mWidgetChild` (+0x3D8 début, +0x3E0 fin) et
  `mWidgetChildSkin` (+0x3F8 / +0x400), racines de `Gui` (+0x28 / +0x30, `Gui::getEnumerator`).
- `ICroppedRectangle::getAbsoluteCoord` (this = widget + 0x8, `TCoord<int>` rendu par pointeur caché ;
  lit +0x28/+0x2C la position absolue, +0x20/+0x24 la taille, relatifs à l'`ICroppedRectangle`).
- `RenderManager` : instance `Singleton<RenderManager>::msInstance` (export de donnée) ; slot 6 de
  sa vtable (+0x30) = `const IntSize& getViewSize() const` (vérifié : ses appelants lisent [rax] et
  [rax+4] ; le slot 7 rend `getVertexFormat` par pointeur caché) ; dans Kenshi
  (`OgreRenderManager`, dans l'exe) la taille est à +0x2C. Lu dans un jeu lancé : vue 844×774 =
  tampon = client de la fenêtre après le redimensionnement du banc (le jeu redimensionne son tampon
  et la vue MyGUI). Le mod convertit quand même vue MyGUI → tampon à chaque image.
- `LayerManager::getWidgetFromPoint(x, y)` (instance `msInstance`) : le widget du dessus à un point,
  couche du dessus d'abord, seulement ceux qui prennent la souris : sert à savoir si un portrait est
  sous une fenêtre du jeu.
- `InputManager::injectMouseMove/Press/Release` (bouton gauche = 0), `getMousePosition` : la commande
  de test `mapui` clique ainsi le bouton MAP. `TabControl::setIndexSelected` : `mapui maptab`.

**Couleurs des points du jeu** (`MapScreen::getMarkerColor` `0x48F320`, statique) :
`MarkerColourAlly` `0x212F538`, `Neutral` `0x212F548` (aussi pour un objet nul), `Enemy`
`0x212F558`, `Player` `0x212F568`, `PlayerSelected` `0x212F578`. Lecture seule : faction de
l'objet (vt 0x58), `Faction::isPlayer` (+0x250), relations avec le joueur (`0x2134690` =
`GameWorld`+0x580 `PlayerInterface*`).

**Raids et vagues d'attaque** : `Faction`+0x88 `FactionWarMgr*` (vérifié dans
`Blackboard::getMissionTarget` `0x269B60` : `platoon->getFaction()` (vt 0x58) puis +0x88, puis
`getCurrentCampaign`). `getCurrentCampaign(Platoon*)` `0x283500` → `CampaignInstance*` ou nul.
`CampaignInstance` : +0x10 `CampaignData*`, +0x70 sa faction, +0x78 `enemy` (la faction attaquée ;
vérifié dans le constructeur `0x9D4770`, 4e argument). `CampaignData`+0x12 `_isHostile`
(`isHostile` `0x2856B0`). Un perso → son escouade : `Character`+0x658 `ActivePlatoon*`, +0x78
`Platoon*`. Limite : seules les escouades chargées (personnages actifs chez l'hôte) sont vues ; un
raid encore « abstrait » loin de tout joueur n'a pas de position.

**Origine de rendu mobile** : le Ogre de Kenshi a `SceneManager::getRelativeOrigin()`
(export de `OgreMain_x64.dll`, `Vector3` rendu par pointeur caché) ; `CameraClass::getCameraPos`
(`0x1008F0`) rend `camera(+0x68)->getSceneManager()->getRelativeOrigin()` + position dérivée du nœud
(+0x70). La scène est donc dessinée en coordonnées monde − origine, et `getViewMatrix` est dans cet
espace : pour projeter une position du monde, retrancher l'origine d'abord (le mod l'intègre à
proj × vue). Sans cela tous les points sont hors écran.

**Caméra 3D** : `PlayerInterface`+0x30 `CameraClass*` (`getCamera` `0x3E7060`), `CameraClass`+0x20
`initialised` (`isInitialised` `0xA1B030`), +0x68 `Ogre::Camera*` (`getCameraPos` `0x1008F0`).
Matrices par les exports d'`OgreMain_x64.dll` : `Camera::getViewMatrix` (virtuelle, version sans
argument) et `Frustum::getProjectionMatrix` ; projection = proj × vue (lignes, Ogre). Une ligne de
la vue = l'axe z de la caméra : la direction regardée est −(m[2][0], m[2][2]) en (x, z).
[U : la vue rendue par `getViewMatrix` est la vue complète, même si Ogre rend « relatif à la
caméra » ; à confirmer en jeu, la commande `mapscene tetes` donne les points projetés.]

**Noms au-dessus des têtes** (0.3.4) [D] : `CharacterNameTag` (RTTI, vtable `0x1703950`, hérite de
`ScreenLabelInterface` ; vt 0 destructeur `0x6F0D30`, vt 1 mise à jour `0x6E8100`, vt 2 `0x6E25B0`
écrit +0x8). Créé (0x60 octets, constructeur `0x6EADC0`) par `0x5CC310` et `0x623920` pour un perso
de la faction du joueur, gardé dans `Character`+0x638 ; inscrit dans la liste globale des étiquettes
(`lektor<ScreenLabelInterface*>`, nombre `0x2133A28`, tableau `0x2133A30`). Champs : +0x8 affiché
(réglage du jeu), +0x18 « à refaire » (couleur, texte, icônes au prochain passage ; mis par les
accesseurs `0x6E28E0` (+0x21), `0x6E2900` (+0x20), `0x6E2920` (+0x24)), +0x20 un état d'affichage,
+0x21 sélectionné, +0x24 discrétion (0 à 3 : icône `StealthImage`), +0x28 `hand` du perso, +0x58
`CharacterNameTagWidget*` (0xB8 octets, fait au premier passage, constructeur `0x6E7EC0`,
`wraps::BaseLayout` sur `Kenshi_CharacterNamePanel.layout` : +0x8 `Root`, +0xA0 `NameText`
(`MyGUI::TextBox`), +0xA8 `StealthImage`, +0xB0 `LightLevelImage`). La mise à jour `0x6E8100` :
retrouve le perso par son `hand`, cache le nom si +0x8 est nul ou si le perso est trop loin de la
caméra, sinon montre `Root` ; si +0x18 : `NameText->setTextColour` (vt 0xB0) avec `NameTagSelected`
(`0x2132AE0`, blanc) quand +0x21, sinon `NameTagUnselected` (`0x2132AF0`, gris ; couleurs lues de
`data/gui/colours/kenshi_colours.xml`), `setCaption` (vt 0x88) avec le nom (décoré quand
sélectionné), icônes, puis +0x18 = 0 ; enfin place le widget au-dessus de la tête. La couleur n'est
donc reposée par le jeu qu'au passage où +0x18 est mis : le mod remet la couleur du joueur juste
après ce passage (même image), et met +0x18 pour rendre la main au jeu.

**Barre d'escouade** : `PortraitMainCellView` (vtable `0x16D26F8`), une par portrait : +0x8
widget principal (`BaseCellView::mMainWidget` ; `update` lit sa largeur à +0x28), +0xA8
`characterHandle` (hand), +0xF0 bouton cadre (`Button::setStateSelected`), +0x100 image du
portrait. `update` `0x415150` ne touche à aucune couleur (seulement taille, alpha, état
« sélectionné », images) : un cadre de couleur ajouté par-dessus n'est pas effacé par le jeu,
mais le mod dessine le sien dans l'overlay plutôt que de teinter les widgets du jeu (les cellules
sont réutilisées pour d'autres persos et la couleur d'origine du skin n'est pas lisible).

**Texture de la carte** : `data/gui/gfx/GUI_Map.dds`, 8192 × 8192 DXT1, 14 niveaux. La minicarte
lit le niveau de 2048 px (offset = somme des niveaux plus grands) et en fait une texture D3D11.
Un mod qui remplace la carte du jeu n'est pas suivi (le fichier de base est lu).

**Pas de son d'interface trouvé** : aucune fonction « jouer un son d'interface » simple dans
KenshiLib (`InventoryGUI::playSound(eventId, Item*)` dépend d'un objet) : les pings sont muets.

## 12. Barre d'escouade (dessin d'une case) et rapport de plantage du jeu [D]
- **Barre d'escouade** : `MainBarGUI` (objet à `*(0x21337C0)` ; `PlayerInterface::setCurrentPlatoon`
  `0x7F2800` écrit `PlayerInterface+0x2A8` puis l'appelle, `ActivePlatoon::addCharacterAt` aussi).
  Une `unordered_map<ActivePlatoon*, MainTabPortraitPlatoon>` ; chaque onglet a un
  `PortraitMainItemBox` (vtable `0x16D2828`, construit par `0x415E00` sur `item_box_v.layout`, son
  `MyGUI::ItemBox` à +0x08 et +0xF0), cases `PortraitMainCellView` (vtable `0x16D26F8`). La donnée
  d'une case est un `MyGUI::Any` contenant un `PortraitData*` (`Holder` vtable `0x16D1FB0`) ; le
  jeu tient aussi `unordered_map<hand, PortraitData*>` et `PortraitData*` → (`PortraitMainItemBox*`,
  bool). `PortraitData` : `hand` du personnage à +0x38 (type à +0x40). Dessin d'une case :
  `requestDrawItem` `0x426C10` → `0x415150` → `0x412D90` (hand → `Character*` : type 1 ou 0x5B,
  par la table des handles `0x2133F98`). Les infobulles médicales des portraits (« Votre Poitrine
  est en train de guérir. ») sont des chaînes du même tas.
- **Mise à jour de la barre** [D, 1.0.68] : `MainBarGUI+0x210` tableau des onglets (0x40 octets :
  +0x0 `PortraitMainItemBox*`, +0x8 `ActivePlatoon*`, +0x10 `TabItem*`, +0x18 indice), +0x218 leur
  nombre ; `PortraitMainItemBox+0xF0` → `MyGUI::ItemBox`, dont les cases (`MyGUI::Any`, 8 octets :
  le `Holder*`) sont à +0x6A8 / +0x6B0 (début / fin). `Holder<PortraitData*>` : vtable `0x16D1FB0`,
  +0x8 la `PortraitData*` ; `getItemDataAt` (`0x424290`) vérifie le type par la vtable avant.
  - `ActivePlatoon` ajoute / retire un membre (`0x796620`, `0x797240`, `swapCharacters`) : le
    portrait suit le nouveau `hand` (`PortraitManager` `0x413620`), puis un événement « escouade
    changée » va dans la file d'interface de `GameWorld` (+0x5C8, traitée par `0x7B65B0` pendant la
    frame du jeu : le `Platoon` est retrouvé par son `hand`, rien s'il n'existe plus, rien pour
    l'escouade des morts) → `MainBarGUI::platoonChanged(ActivePlatoon*, bool ajout)` (`0x4167B0`).
  - `platoonChanged` : escouade vidée (retrait, 0 membre) ou remplie (ajout, 1 membre) →
    reconstruction des onglets (`0x416140`) ; sinon, si c'est l'escouade affichée,
    `updateCurrentPlatoon` (`0x415880`) ; sinon **rien** : les autres onglets gardent leurs cases.
    `destroyPlatoon`, `changePlatoonIndex`, le renommage (`0x48DF00`), `0x7F3FB0`, `0x7F88C0` et
    `0x72D3B0` appellent aussi `0x416140`.
  - `0x416140` : garde les onglets dont l'`ActivePlatoon` (+0x78 → `Platoon`) est encore dans
    `Faction::activePlatoons`, détruit les autres, vide le `TabControl` et refait un onglet par
    escouade non vide hors escouade des morts (`0x415E00`) ; pour un onglet gardé, `0x415E00`
    redimensionne son `ItemBox` (`setRealSize`), ce qui **redessine toutes ses cases** avec leurs
    `PortraitData` d'avant : c'est là qu'ont planté les soaks de 14:54 et 20:20.
  - `updateCurrentPlatoon` (`0x415880`) : pour chaque membre de l'escouade affichée, sa
    `PortraitData` (`PortraitManager::get(hand)` `0x4139B0`, créée au besoin) ; `setItemDataAt`
    sur les cases existantes, `addItem` pour les nouvelles, `removeItemAt` pour le surplus.
  - **`PortraitManager`** (`*0x212EBE8`, construit une fois : bit 0 de `0x212EBF4`, 0xA8 octets) :
    `std::map<hand, (PortraitData*, case de texture)>` à +0x70 (tête +0x78 ; nœud : +0x0 gauche,
    +0x8 parent, +0x10 droit, +0x38 `PortraitData*`, +0x49 « nil »). `PortraitData` : 0x60 octets
    (allocateur d'Ogre), +0x0 `std::string`, +0x38 le `hand`. Elles ne sont **jamais** libérées une à
    une : `0x414850` les supprime toutes, appelée seulement par la remise à zéro du monde
    (`0x36CB80`, au chargement). Le mod vérifie donc une case en cherchant son pointeur parmi celles
    du gestionnaire (`kenshi::IsGamePortrait` dans le crochet de `0x415150`, `kenshi::CheckSquadBar`
    à chaque image : un onglet fautif est vidé par `ItemBox::removeAllItems`, puis `0x415880`).
- **Escouade des morts** [D] : le jeu la reconnaît en comparant le `hand` du `Platoon` (+0x58) à
  `PlayerInterface+0x2C8` par `hand::operator==` (vtable du `hand` `0x16852D0`, case 1 : `0xCD060`,
  champs +0x8 type, +0xC, +0x10, +0x14, +0x18 ; deux `hand` de type 0xB sont égaux). Les `Platoon`
  du joueur ont index et serial à 0 : un test par `kc::Handle::valid()` ne la trouve jamais (le mod
  l'envoyait comme une escouade ordinaire jusqu'au 10/10 au soir). `kenshi::IsDeadSquad`.
- **Boutons de vitesse** (`0x724880`) : chaque bouton porte sa vitesse en float, allumé si elle est
  `==` à la vitesse courante ; une vitesse non entière (ClockSync, 0,01 avant une pause) n'en
  allume aucun, sans autre effet. `GameWorld::setFrameSpeedMultiplier` (`0x787CB0`) range la
  vitesse (+0x700) et la passe, sous verrou, à la physique.
- **Rapporteur de plantage** : la boucle principale du jeu (`0x745490`) est dans un `catch(...)`
  (compilé /EHa : il prend aussi les violations d'accès) dont les funclets `0x1373AB0` et
  `0x1373A20` appellent `writeCrashDump(EXCEPTION_POINTERS*, const char* nom)` (`0x744D20` :
  `crashDump1.0.68_x64.dmp`, boîte « Kenshi has crashed »). Un tel plantage ne passe donc jamais
  par `SetUnhandledExceptionFilter` ; le processus sort ensuite et ses destructeurs statiques
  plantent souvent à leur tour (`0x86D6D6` appelle un destructeur par une vtable morte), et c'est
  ce second plantage que voyait le filtre du mod. Dans le dump, la pile au-dessus de `rsp` est
  déjà en partie réécrite (déroulement C++ et écriture du dump).

## 13. Atelier : recherche, établis, machines, énergie [D]
- **Recherche** : un seul objet `Research` pour la faction du joueur, `PlayerInterface+0x38`
  (`*(0x2134690)+0x38` dans le code de la fenêtre). Champs vérifiés dans le 1.0.68 : +0x0
  `ManagementScreen*`, +0x38 la file `std::deque<ResearchItem>` (VS2010 : +0x40 tableau de blocs,
  +0x48 taille du tableau, +0x50 premier indice, +0x58 nombre ; un `ResearchItem` de 16 octets par
  bloc : `GameData*` à +0, `float progress` à +8), +0xF0 l'ensemble `finished`
  (`boost::unordered_set<GameData*>`, rempli par `completeResearch`), +0x170 niveau du meilleur banc
  de recherche (int, comparé au « level » de la technologie), +0x174 nombre de chercheurs de
  l'image. D'après KenshiLib, non vérifiés : +0x10 `craftableThings`
  (`std::map<itemType, lektor<GameData*>>`), +0x130 `enabledObjects` (ce que proposent le menu de
  construction et les listes de fabrication), +0xD8 `paidFor`.
- Un **plan lu** est une technologie terminée (`learnResearch` → `completeResearch`) : il n'y a pas
  d'état « plan » à part.
- `completeResearch` dépile l'avant de la file pour **toute** entrée qui correspond : appelée sur une
  technologie qui n'est pas en tête, elle retire la mauvaise. Le mod la retire d'abord de la file
  (`stopResearch`).
- Le coût est payé une fois (`payCosts` avant `isInQueue` dans `startResearch` ; un second
  `startResearch` de la même technologie paie avant d'être refusé, d'où la vérification `isInQueue`
  du mod avant d'appeler le jeu).
- **Machines** (`UseableStuff`, vtable `0x16B21C8`) : +0x3A4 barre de progression (float, écrite par
  `UseableStuff::operate` `0x297530`), +0x3AC opérateurs max (int), +0x3B4 reçoit l'énergie des
  batteries, +0x3B5 en marche, +0x3B8, +0x3BC sortie max (> 0 : générateur), +0x3C0 énergie reçue
  dans l'image, +0x3C4 charge de batterie, +0x3C8 charge max (> 0 : batterie), +0x3D0 les opérateurs
  `std::set<hand>` (+0x3D8 nœud de tête, +0x3E0 nombre ; nœud : gauche +0, parent +8, droite +0x10,
  `hand` à +0x18, nul à +0x39). Building vt 0x300 `getUseableStuff` (lui-même ou nul), vt 0x3B0
  `getProductionBuilding`. `ProductionBuilding+0x448` : l'objet produit (`ConsumptionItem*`, quantité
  float à +0). `CraftingBuilding` (vtable `0x16B5A58`) : +0x498 les ordres (`std::deque<CraftingItem>`,
  nombre à +0x4B8, un par bloc ; `CraftingItem` : `Item*` +0, `progress01` +0xC), +0x4CC répéter.
  `Item+0xC8` : son matériau. Le bâtiment connaît sa ville par le `hand` à +0x1D0 (`Town`, vtable
  `0x1735BA8`).
- Places des filons (vu en jeu le 10/10) : `42248-gamedata.base` « Iron Resource » (Ressource Fer) 3 opérateurs, `42249-gamedata.base` « Copper Resource » (Minerai Cuivre) 2. Modèles vanilla utiles aux tests : petit banc de recherche `1875`, établi de vêtements `2929`, établi d'armures en cuir `2262`, éolienne `1999`, banque de batteries `1855` (tous `-gamedata.base`). Les technologies que renvoie `GameDataOfType(21)` en premier sont des enregistrements `<objet>.TECH.1` (une par objet à fabriquer).
- Les opérateurs ne sont ajoutés que par la tâche du personnage (`tryOperate`), donc par l'IA : chez
  un client (IA coupée) l'ensemble restait vide, d'où « 0/3 » dans la fenêtre de la mine.
- **Énergie** (`Town::updatePowerGrid`) : générateurs (ensemble de `hand` à +0x408…), batteries
  (+0x448…), consommateurs (tableau trié de `hand` +0x3D8 / +0x3E0) ; totaux +0x474 production,
  +0x478, +0x47C demande, +0x480 sortie des batteries, +0x484, +0x488 charge, +0x48C charge stockée,
  +0x490 charge max ; +0x470 vrai quand la ville tourne sur batterie. Le nom exact de +0x478 et +0x484
  n'est pas connu (le mod les copie tels quels).

## 14. Plantage `kenshi_x64+0x883B78` (lecture à 0x161) [D]
- **Où** : `CharStats::update` (`0x883B70`, vt 0x8 de `CharStats` et `CharStatsAnimal`) lit
  `this->medical` (+0x8) puis l'octet +0x161 (inconscient) : `rax = 0` et `addr = 0x161` disent que
  le `MedicalSystem*` des stats est **nul**. Appelant réel : `Character::update` (vt 0x280,
  `0x5D00B0`, retour `0x5D066D`) chez le client, vt 0x278 (`0x5C7D40`, retour `0x5C7DD2`) chez
  l'hôte, tous deux depuis la boucle du **thread de travail** `0x787940` (retours `0x787C24` /
  `0x787BF4`, thread lancé par `0x25FE48`). Les autres adresses de la pile journalisée (`0x883DFB`,
  `0x5C9765`, `0x886662`, `0x88685F`, `0x7D1461`, `0x883B90`) sont des restes d'appels précédents
  (le journal liste toutes les valeurs de la pile qui ressemblent à une adresse de code).
- **Comment un perso vivant a des stats sans `MedicalSystem`** : seul le constructeur de
  `CharStats` met +0x8 à nul, et seule la vt 0x228 du perso en construit un sur un perso existant
  (la mise en place le fait avant `init`). Les deux plantages ont suivi de quelques millisecondes
  un ordre **BUILD (tâche 2) sur un PNJ** (`addTaskNearest` : chez l'hôte `[Joueur2] order "build"`,
  chez le client `npcreq 7 2 a`). Hypothèse la plus probable (non prouvée, l'appel exact dans le
  code de construction n'est pas identifié) : la tâche traite sa cible comme un `Building` et
  appelle un de ses virtuels ; sur un `Character`, le même emplacement remplace ses stats par des
  stats neuves non initialisées, et la mise à jour suivante plante. Autre chemin possible : une
  usine de personnages qui faute après la vt 0x378 (`ActivePlatoon` écrit) et avant `init` laisse
  un perso à moitié prêt dans les listes de mise à jour.
- **Comment le client y est arrivé** : voir CHANGELOG (une exception rattrapée a laissé
  `HostCallScope` compté ; le client exécutait ensuite tous les ordres lui-même).
- **Compilateur** : sous `/EHsc` (le mod), une exception non C++ (violation d'accès) rattrapée par
  un `__except` **ne lance pas les destructeurs** des objets C++ entre la faute et le gestionnaire
  (vérifié : `TestCallScopeRepair`, une portée laissée ouverte). Kenshi lui-même est en `/EHa`.

## 15. Onglet « JOUEURS » de la fenêtre de gestion (statistiques des joueurs, 0.3.5) [D]

`plugin/player_stats_ui.cpp`. La fenêtre de gestion (`Kenshi_OverviewWindow.layout`) est faite par le
chargeur de mise en page de MyGUI ; le mod y ajoute un onglet par les **exports de
`MyGUIEngine_x64.dll`** (MyGUI 3.2, compilé avec MSVC 2010 comme le jeu : `MSVCP100`/`MSVCR100`),
exactement comme le chargeur le fait pour les onglets du jeu.

- **`ManagementScreen` +0x2D0** : le `TabControl*` « TabsMain » (lu par le jeu en `0x48BB90`
  `getIndexSelected`, `0x48BBB0` `getItemAt`, et à la création de la fenêtre en `0x49FA08`).
  ManagementScreen +0x2C8 : un widget à qui le gestionnaire d'onglets passe le nom de l'onglet choisi (vt 0x88) [D, partiel].
- **Création de la fenêtre** (`0x49FA0F`–`0x49FA4A`) : `setSmoothShow(false)`, puis
  `setButtonDefaultWidth(largeur du TabControl (+0x28) / getItemCount())`, `setIndexSelected(0)`,
  puis les noms traduits (`findItemWith("MAP")` → `setItemName`, etc.). Le mod refait le même
  partage de la barre après avoir ajouté son onglet (8 boutons).
- **Gestionnaire d'onglets** (`eventTabChangeSelect`, délégué posé en `0x49F990`–`0x49FA01`,
  fonction `0x49BF90` via le saut `0x514AB`) : index `ITEM_NONE` (`[0x212F520]`) → rien ; sinon le
  titre (+0x2C8, vt 0x88) prend le nom de l'onglet (`IItemContainer` du `TabControl` +0x478, vt 0x30),
  puis `cmp rsi, 6 ; ja` → **seuls les index 0 à 6** passent par la table de sauts (`0x49C1E0`).
  Le 8ᵉ onglet (index 7) ne déclenche rien d'autre. Les autres lectures de l'index (`0x495E4A`,
  `0x49A178`, `0x49A517`, `0x49B575`, `0x49B778`, `0x49B79B`) le comparent à 0, 2, 3 ou 5 : sans effet
  pour 7.
- **Exports MyGUI utilisés** (noms décorés dans le code ; adresses : RVA dans `MyGUIEngine_x64.dll`) :
  - `Widget::createWidgetRealT(type, skin, const FloatCoord&, Align, name)` (`0x1C46F0`) :
    coordonnées relatives à la largeur / hauteur du parent (Widget +0x28 / +0x2C), `Align` passé par
    valeur (5ᵉ argument, sur la pile), nom en 6ᵉ ;
  - `TabControl::onWidgetCreated` (`0x1A6C50`) : tout `TabItem` créé sous le `TabControl` devient un
    onglet (coordonnées et alignement du modèle « TabItem », `_insertItem(ITEM_NONE, "", item,
    Any::Null)`) ;
  - `Widget::setProperty(key, value)` (`0x1C2D00`) : copie ses deux chaînes (le mod les fabrique au
    format `std::string` de MSVC 2010 : tampon ou pointeur, taille +0x10, capacité +0x18, 40 octets) ;
    « Caption » d'un `TabItem` → `TabItem::setCaption` (`0x1A8DE0`) → `TabControl::setItemName` ;
  - `TabControl::findItemIndex(TabItem*)` (compare des pointeurs : sûr même avec un pointeur
    périmé), `getItemCount`, `getIndexSelected`, `setIndexSelected`, `setButtonDefaultWidth` ;
  - `EditBox::getVScrollPosition` / `setVScrollPosition` (garder l'endroit lu quand le texte change) ;
  - `Widget::getVisible`, `getInheritedVisible` (fenêtre ouverte).
  - `ICroppedRectangle::getWidth` lit +0x20 (Widget +0x28).
- **Habillage** : la zone de texte est une `EditBox` « Kenshi_WordWrap » (modèle `ResourceLayout` de
  `data/gui/templates/kenshi_templates.xml` : `Kenshi_GenericInputBoxSkin`, texte
  `Kenshi_EditBoxStandardText`, police `Kenshi_StandardFont_Medium`, couleur `0.078 0.031 0.024`
  presque noire sur parchemin, barre `Kenshi_ScrollBarV`), comme l'onglet DIALOGUE, avec
  `MultiLine`, `Static`, `ReadOnly`, `WordWrap`, `VisibleVScroll` et `MaxTextLength` 65536 (2048 par
  défaut dans MyGUI). Couleurs par codes `#RRGGBB` dans le texte (`##` pour un « # »).
- **Polices** (`data/gui/fonts/kenshi_fonts.xml`) : les polices de Kenshi (`Exo2`, `Sentencia`…) ne
  contiennent que les codes 32 à 126 et 8127 à 8217 (`SubstituteCode` 0) : **aucune lettre accentuée**
  ne s'affiche. Le texte de l'onglet passe par `kc::FoldAccents` (é → e, Œ → OE ; tirets et
  guillemets de 8127-8217 gardés).
- **Sûreté** : rien n'est gardé sans revérification. À chaque passage (0,5 s, fil du jeu, images
  vivantes), le `TabControl` est relu depuis le singleton (vtable vérifiée), l'onglet retrouvé par
  `findItemIndex` sur ce `TabControl`, la zone de texte parmi les enfants de l'onglet (Widget +0x3D8,
  `vector<Widget*>`). Fenêtre refaite ou nouveau monde : l'onglet est refait. Chaque appel est sous
  `__try/__except`.
