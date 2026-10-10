# Inventaire du code

Carte de tout ce qui existe dans le code, établie en relisant master `1e2d48c` (10 octobre 2026,
protocole 33, environ 25 500 lignes). L'état de chaque fonctionnalité au sens joueur est dans
[FONCTIONNALITES.md](FONCTIONNALITES.md), les bugs ouverts dans [A_FAIRE.md](A_FAIRE.md).

États utilisés : **vérifié en jeu** (vu en partie ou par le harnais en jeu), **testé unitairement**
(`tests/test_main.cpp` seulement), **jamais testé**.

## 1. Fonctionnalités et systèmes

| Système | Fichiers et fonctions principales | État |
|---|---|---|
| Session, connexion, transfert du monde | `common/src/session.cpp` (Hello/Welcome, WorldBegin/Chunk/End, Ready), `plugin/main.cpp`, `plugin/world.cpp` (sauvegarde KenshiCoopHost/Join) | vérifié en jeu, testé unitairement |
| Transport ENet + relais Steam P2P | `common/src/net*.cpp`, `plugin/steam_link.cpp` (thread du relais) | vérifié en jeu |
| Un perso par joueur, éditeur | `session.cpp` (Bind/Unbind, EditCharacter/EditState), hooks CloseCharacterEditor, SquadAddCharacterAt | vérifié en jeu |
| Positions, déplacements, orientation | `session.cpp` Snapshot, `world.cpp` Apply | vérifié en jeu |
| Détection des persos bloqués / recalage des PNJ lointains | `world.cpp`:559 `kStuckDetection` | **désactivé** (code mort) |
| Animations | hooks Anim*, messages Anim/AnimFrame, `world.cpp` | vérifié en jeu |
| Combat au corps à corps, santé, K.-O., mort | hooks MedApplyDamage, MedKnockout, DeclareDead, ReassessCollapse, RagdollMode ; Vitals | vérifié en jeu |
| Porter un corps | hooks PickupCharacter, AnimSetCarryMode ; `world.cpp` ApplyCarry | vérifié en jeu, bug ouvert (corps sur la tête) |
| Population de PNJ, cadavres | hook CreateRandomCharacter, AIPeriodicUpdate, Squads | vérifié en jeu |
| Escouades, ordres permanents, style | Squads, hook SetStandingOrder, Progress | vérifié en jeu |
| Compétences, XP, argent | hook IncreaseStat, Progress | vérifié en jeu |
| Outils en main (pioche...) | `kenshi.cpp` JobTool/SetHandTool, `world.cpp` ReadTool/ApplyTool, champ `CharProgress.tool` | jamais testé en partie |
| Dialogues, conversations | hooks Say, SetInDialog, SetResponses..., Dialog/DialogReply | vérifié en jeu |
| Ordres d'un client | hooks AddOrderSelected, AddTaskNearest, AddJobSelected, NewPlayerTaskSelected, SetOrderSelected, StopCharactersMovement, PlayerMoveOrderDefault ; Command | vérifié en jeu (suppression de tâche non gérée) |
| Objets au sol | hooks PickupItem, GiveItem, DropItemHuman ; Ground | vérifié en jeu |
| Inventaires, fouille | Inventory/InvOp, `world.cpp` | vérifié en jeu, bug d'ordre des échanges |
| Contenants, vol | ContainerOpen/Opened/Close, `kenshi.cpp` (ImStealin, notifyTheftFrom) | vérifié en jeu, vol 🟡 |
| Atelier : recherche, établis, machines et mines, énergie | `common/src/session_workshop.cpp` (Research/ResearchRequest, Machines/MachineRequest, entités `machine`), `plugin/workshop.cpp` (hooks startResearch, stopResearch, payCosts, progressResearch, learnResearch, _addCraft, _removeCraft, CraftingQueue, updatePowerGrid, togglePowerButton, toggleBattButton) | testé unitairement, jamais testé en partie |
| Panneau d'inventaire d'un bâtiment (mine) | hook ShowInventoryBuilding (`hooks.cpp`) ; le jeu ne l'ouvre que pour un bâtiment du joueur : propriétaire de l'hôte imposé au client (`kMachOurs`, `SetBuildingOurs`, 11/10) | harnais `mine` seulement ; correction du 11/10 non vérifiée en jeu |
| Commerce | hook ShowTradeWindow, TradeOpen, `kenshi.cpp`:2060-2066 | vérifié en jeu (9/10) |
| Heure, vitesse, pause | TimeState | vérifié en jeu |
| Météo et effets | hooks RegionUpdateBT, SeasonGetNewWeather, Effect* ; Weather, Effects | vérifié en jeu |
| Resync, TP admin, kick, sauvegarde | `main.cpp` console, Resync | vérifié en jeu (TP lointain éjecte) |
| Admin : heal, xp, god, money | `main.cpp`, `kenshi.cpp` HealCompletely/SetGodMode | harnais `admin` seulement |
| Lot A — portes | `plugin/doors.cpp`, `common/src/session_doors.cpp` ; Doors/DoorRequest | harnais `doors`, pas en partie |
| Lot B — factions, relations | `plugin/factions.cpp`, `session_factions.cpp`, `protocol_factions.cpp` ; Factions | harnais `factions` ; boucle de correction signalée |
| Lot B — primes | `factions.cpp`:245 `ApplyBounties` | **désactivé côté client** (retour immédiat), Bounties toujours envoyé |
| Lot C — combat à distance, tourelles | `plugin/ranged.cpp`, `session_ranged.cpp`, `protocol_ranged.cpp` ; hooks GunShoot, ProjectileGet ; Shots/Ranged | harnais `ranged`, pas en partie |
| Lot D — prisons, chaînes, esclavage | `plugin/prisons.cpp`, `session_prisons.cpp` ; hooks SetPrisonMode, SetChainedMode, SetSlaveState ; Captives | harnais `prison`, pas en partie |
| Lot E — construction, achat, démontage | `plugin/buildings.cpp`, `session_buildings.cpp` ; 7 hooks ; Build* | harnais `build` ; bâtiments existants non synchronisés |
| Overlay, console de l'hôte | `plugin/overlay.cpp`, `plugin/host_console.cpp` | vérifié en jeu |
| Administration de l'hôte (dieu, TP, XP, soins, argent) | `plugin/admin.cpp`, `common/src/admin.cpp` ; hooks MedApplyDamage, MedKnockout, IncreaseStat, PlayerMove ; Chat (message au joueur) | `TestAdmin` ; harnais `admin` pas encore lancé |
| Journaux et rapports client | ClientLog/ClientReport, `util.cpp` | vérifié en jeu |

## 2. Messages réseau (`common/include/kc/protocol.h`)

Traités dans `session.cpp` sauf mention contraire. S→C : hôte vers client ; C→S : client vers hôte.

| Val | Message | Sens | Rôle |
|---|---|---|---|
| 1 | Hello | C→S | version, nom (champ `worldHash` jamais lu) |
| 2 | Welcome | S→C | id joueur, accepté |
| 3 | Reject | S→C | refus (version, plein, nom) |
| 4 / 5 | PlayerJoined / PlayerLeft | S→C | liste des joueurs |
| 6 | Chat | ↔ | discussion |
| 7 / 8 | Bind / Unbind | S→C | perso attribué à un joueur |
| 9 | Snapshot | S→C, non fiable | positions, destinations |
| 10 | Command | C→S | ordres du client |
| 11 | TimeState | S→C | heure, vitesse, pause |
| 12 / 13 | Ping / Pong | ↔ | latence |
| 14 | Vitals | S→C, non fiable | santé, K.-O., mort |
| 15–17 | WorldBegin / Chunk / End | S→C | sauvegarde de l'hôte |
| 18 | Ready | C→S | monde chargé |
| 19 | Weather | S→C | météo |
| 20 / 21 | Inventory / InvOp | S→C / C→S | inventaires, déplacements d'objets |
| 22 | Effects | S→C | effets météo |
| 23 / 24 | Anim / AnimFrame | S→C (24 non fiable) | animations |
| 25 | Ground | S→C | objets au sol |
| 26 | Progress | S→C | stats, modes, style, outil |
| 27 / 28 | Dialog / DialogReply | S→C / C→S | conversations |
| 29 | Squads | S→C | escouades |
| 30 | Appearance | ↔ | apparence |
| 31 / 32 | EditCharacter / EditState | S→C / C→S | éditeur de perso |
| 33 / 34 | ClientLog / ClientReport | C→S | journal et rapport du client |
| 35 | Resync | S→C | recharger le monde |
| 36–38 | ContainerOpen / Opened / Close | C→S / S→C / ↔ | contenants |
| 39 | TradeOpen | S→C | fenêtre de commerce (comptoirs, ou sacs portés : `ownerNetId`) |
| 80 | BagBind | S→C | sac à dos porté par un personnage : son netId, son porteur, son modèle (contenu en `Inventory`) |
| 40 / 41 | Doors / DoorRequest | S→C / C→S | portes (`ClientDoorsPacket` :2452, `HostDoorPacket` :1436) |
| 43 | Factions | S→C | relations (`ClientFactionsPacket` :2091) |
| 44 | Bounties | S→C | primes (ignorées par le client, voir §7) |
| 46 / 47 | Shots / Ranged | S→C | tirs, visée (`ClientRangedPacket` :2453) |
| 49 | Captives | S→C | prisonniers (`OnCaptives` :2303) |
| 52–55 | BuildPlace / BuildState / BuildRemove / BuildAction | ↔ / S→C / S→C / ↔ | construction (`session_buildings.cpp`, aiguillage :1407 et :2304) |

Valeurs libres : 42, 45, 48, 50, 51.

## 3. Hooks (`plugin/hooks.cpp`, table `defs[]` ligne 1194)

Règle générale : chez un client, le hook refuse ce que le jeu local décide seul (dégâts, morts,
ordres, crimes, météo...) sauf pendant un `HostCallScope` (application d'un message de l'hôte) ;
chez l'hôte, il observe et diffuse.

- **Boucle et ordres** : MainLoop (pompe la session), PlayerMove, AddOrderSelected, AddTaskNearest,
  AddJobSelected, NewPlayerTaskSelected, SetOrderSelected, StopCharactersMovement,
  PlayerMoveOrderDefault : chez le client, envoyés à l'hôte en Command au lieu d'agir.
  AIPeriodicUpdate : IA des persos de l'hôte coupée chez le client.
- **Santé** : MedApplyDamage, MedKnockout (refusés chez le client ; mode dieu chez l'hôte),
  DeclareDead, ReassessCollapse, RagdollMode.
- **Objets et progression** : PickupItem, GiveItem, DropItemHuman, IncreaseStat.
- **Escouade, éditeur** : SquadAddCharacterAt, CloseCharacterEditor, SetStandingOrder, PickupCharacter.
- **Fenêtres** : ShowTradeWindow (commerce passé par l'hôte), ShowInventoryBuilding (demande le
  contenu réel à l'hôte).
- **Prisons** : SetPrisonMode, SetChainedMode, SetSlaveState.
- **Dialogues** : Say, SetInDialog, SetResponses, SetReplyText, SendEvent, SendEventOverride,
  StartConversation, StartPlayerConversation, DoActions ; EndDialogue (appelée seulement).
- **IA, factions** : SensoryDialogAssessment, SensoryAssessCrimes, BlackboardUpdate,
  BlackboardPeriodic, FactionWarPeriodic, UniqueSquadPeriodic, AffectRelationsAmount,
  AffectRelationsEvent, SetRelation, SetCrime, AssignBounty.
- **Étiquettes** : CreateScreenLabel, LabelSetTracking, LabelSetColor.
- **PNJ** : CreateRandomCharacter (pas de création locale chez le client).
- **Météo** : RegionUpdateBT, SeasonGetNewWeather, EffectHandlerCtor, EffectAffectObjects,
  EffectStop, RegionUpdateEffects.
- **Animations** : AnimStartCombat/Run/End, AnimPlayAction, AnimStopAction, AnimStopActionNamed,
  AnimStart/EndStumble, AnimSetCombatMode, AnimSetCarryMode, DrawWeapon, SheatheWeapon,
  AnimGuardLegs/Upper, AnimationSelection, TrackAnimationMovement, SingleAnimUpdate,
  CombatMovementUpdate.
- **Portes** (`doors.cpp`) : DoorOpen, DoorClose, DoorLock, DoorUnlock, DoorOpenButton, DoorLockButton.
- **Distance** (`ranged.cpp`) : GunShoot, ProjectileGet.
- **Construction** (`buildings.cpp`) : CreateFromPreviews, CreateBuilding, BuyMeCallback,
  ConfirmDismantle, AddConstructionProgress, AddDismantleProgress, WorldDestroy.

## 4. Adresses du jeu

### Table `kFunctions` (`plugin/kenshi.cpp`, enum dans `kenshi.h`) : 189 entrées vérifiées par octets de prologue

« hook » = accrochée ; sinon appelée seulement.

| # | Fonction | RVA | |
|---|---|---|---|
| 0–6 | MainLoop, PlayerMove, AddOrderSelected, NewPlayerTaskSelected, SetOrderSelected, StopCharactersMovement, PlayerMoveOrderDefault | 0x788A00, 0x7FA850, 0x7F9E20, 0x7FA650, 0x7F3880, 0x7F6470, 0x5D22B0 | hook |
| 7 | AIUpdate4Frame | 0x5965B0 | **absente de `defs[]`** : ni accrochée ni appelée ? à vérifier |
| 8 | AIPeriodicUpdate | 0x5112B0 | hook |
| 9–12 | SetFrameSpeedMultiplier, UserPause, HandleResolve, TogglePause | 0x787CB0, 0x787FB0, 0x2676E0, 0x787D40 | |
| 13–15 | MedApplyDamage, MedKnockout, DeclareDead | 0x64F300, 0x644980, 0x7A6200 | hook |
| 16–18 | SaveManagerGet, Save, Load | 0x37DD00, 0x47B920, 0x47B480 | |
| 19, 20, 22 | CreateRandomCharacter, WorldDestroy, RagdollMode | 0x5836E0, 0x799AF0, 0x5CBD60 | hook |
| 21 | EndCombatMode | 0x5C91C0 | |
| 23–24 | RegionUpdateBT, SeasonGetNewWeather | 0x9DDE50, 0x9DD980 | hook |
| 25–26 | InstanceSetupWeather, CreateItem | 0x9DCF60, 0x580750 | |
| 27, 30, 31 | ShowTradeWindow, AddTaskNearest, AddJobSelected | 0x791830, 0x7FAE70, 0x7F5A90 | hook |
| 28–29 | IsRagdoll, Recruit | 0x7D1440, 0x692820 | |
| 32–35 | effets météo | 0x1034F0, 0x1020E0, 0x100B00, 0x9DCAF0 | hook |
| 36–50 | animations | 0x5B7800 … 0x5B1700 | hook |
| 51 | RunAnimationLayer | 0x5B7AC0 | |
| 52–54 | SectionCanItemGoHere, SectionValidPosition, SectionFootprintTaken | 0x74BE40, 0x74BEC0, 0x7466F0 | |
| 55–65 | PickupItem, GiveItem, DropItemHuman, 3 étiquettes, ReassessCollapse, AnimationSelection, TrackAnimationMovement, CombatMovementUpdate, IncreaseStat | 0x7FB3A0 … 0x8C5DF0 | hook |
| 66–67 | ObjectSelected, UnselectAll | 0x7F7F20, 0x7F8DA0 | |
| 68–77 | dialogues (dont replyClicked 0x683DF0 appelée seulement) | 0x67FD80 … 0x680560 | hook |
| 78 | TaskSystemUpdate | 0x50D920 | |
| 79–89 | IA et factions | 0x85A5F0 … 0x853EC0 | hook |
| 90, 92 | FocusCamera, CreateSquad | 0x7F37F0, 0x7F4910 | |
| 91, 93 | SquadAddCharacterAt, CloseCharacterEditor | 0x796620, 0x6E22B0 | hook |
| 94–95 | ShowCharacterEditor, SetAppearanceData | 0x6E32F0, 0x5B9E90 | |
| 96–102 | maps GameData, std::string::assign | 0x6C7C0 … 0x2E6110, 0x69BC0 | |
| 103–104, 107 | CharAddOrder, CharAddJob, DropCarried | 0x5D20D0, 0x5C8DA0, 0x5CE1E0 | |
| 105–106 | SetStandingOrder, PickupCharacter | 0x5CAA50, 0x5CFF90 | hook |
| 108, 110 | SetCurrentPlatoon, ReThinkAIAction | 0x7F2800, 0x5C8330 | |
| 109 | ShowLoadWindow | 0x4824C0 | **plus utilisée** |
| 111–114 | GetOwnerships, InteriorShopFurniture, GetNpcTrader, CharTakeMoney | 0x7956A0, 0x54ACB0, 0x70E2D0, 0x7965F0 | |
| 115 | RClickAutoTrade | 0x713D20 | tests seulement |
| 116 | GetRelationData | 0x6B4C60 | |
| 117 | BountyMapIndex (operator[]) | 0x5E7EE0 | **suspectée du plantage des clients** |
| 118–128 | portes, prisons, GunShoot, ProjectileGet | 0x297040 … 0x43A2F0 | hook |
| 129–134 | construction | 0x4D72A0 … 0x2A2860 | hook |
| 135–136 | ClearUsageNodes, CalculateSaleValue | 0x54C4D0, 0x7AD300 | |
| 137 | HealCompletely | 0x6464C0 | traduite de KenshiLib, vérifiée en partie |
| 138 | ShowInventoryBuilding | 0x6E6640 | hook |
| 162–166 | SquadSwapCharacters, ChangePlatoonIndex, DestroyPlatoon, SquadSetName, CharGetPermajobData | 0x792E60, 0x7F3440, 0x6BA9D0, 0x4BE480, 0x5C8F10 | fenêtre Escouade (`squads.cpp`) ; SquadSwapCharacters : hook |
| fin | TerrainHeight, TerrainWithWaterHeight, IsIndoors, GetNearestTown, WithinBordersRange, GetNearestWithinItsRadius | 0x9B3710, 0x9B3720, 0x9B2BA0, 0x927F10, 0x926D50, 0x928890 | validité d'une pose (`buildings.cpp`) |
| 188 | ClockSetHourOfDay | 0x66CF30 | recaler l'horloge d'un client (`kenshi::SetGameHours`) |

### Adresses écrites en dur hors de la table (non vérifiées au démarrage)

| Adresse | Rôle | Fichier |
|---|---|---|
| 0x2134110, 0x2133F90, 0x21303D0, 0x2133573, 0x21337B0, 0x2128190 | GameWorld, table des handles, horloge, chemin de sauvegarde, GUI, météo | `kenshi.h`:21-26 |
| 0x2132BE8 / 0x2132BF0, 0x2132B58 | fenêtres de commerce ouvertes, objet tiré à la souris | `kenshi.h`:27-29 |
| 0xED6504 / 0xED64FE | operator new / delete du jeu | `kenshi.h`:30-31, **en double** `kenshi.cpp`:979 et :3058 |
| 0xED650A / 0xED64F8 | allocateur du mode construction (**6 octets d'écart avec les précédents, à confirmer**) | `buildings.cpp`:34 |
| 0x168BAF0 | vtable lektor | `kenshi.h`:32, **en double** `kenshi.cpp`:3057 |
| 0x16F9EB8 … 0x168C128, 0x16D3F78, 0x16D5258, 0x16D5138 | vtables (Character, AI, effets, tourelles) | `kenshi.h`:34-47, 634-636 |
| 0x6508D0–0x651FF1 | bornes de addWound | `kenshi.h`:573 |
| 0x16DFB00 | callback d'aimantation de bâtiment | `buildings.cpp`:33 |
| 0x2134100 | `TownList*` (villes) ; slots vt 0x40, 0x58, 0x268, 0x2A0 de `TownBase` | `buildings.cpp` (`TownTooClose`, sous `__try`) |
| 0x21337B0 (+0x1C0), 0x16EFE88 | GUI / éditeur, vtable lektor<Character*> | `kenshi.cpp`:837-839, `debug.cpp`:575 |
| 0x6E2DF0, 0x6E5740, 0x70CEF0 | drapeau de la fenêtre de commerce, fermeture, callback | `kenshi.cpp`:2060-2066 (sous `__try`) |
| 0x2011F78 | liste des techniques de combat | `kenshi.cpp`:2646 |
| 0x1E3A5F8, 0x2247DA0 | handle vide, Quaternion::IDENTITY | `kenshi.cpp`:2973-2974 |
| 0x21349C0, 0x9FF210 | zoneMgr, ZoneSpacialGrid::getObjects | `kenshi.cpp`:3054-3056 |
| 0x16BE898 | vtable Task_OperateMachine | `kenshi.cpp`:3386 |

Slots virtuels appelés en dur (`VSlot`) : 0x160 (`kenshi.cpp`:1775), 0x58 getFaction (:2102),
0x298 ImStealin (:2111), 0x348 notifyTheftFrom (:2114), 0x2F0 getSpecialFunction (:2252), 0x48, 0x198
et 0x1A0 (attachements des mains, :3395-3425).

Décalages non vérifiés : MedicalSystem à Character+0x458 (mode dieu, `hooks.cpp` hk_medDamage /
hk_medKnockout) ; `ReadOperatorCount` +0x3E0 ; CharBody +0x648 / Tasker +0x68 / outil +0x88 (outils en main).

## 5. Commandes

### Console de l'hôte (`plugin/main.cpp`)

- `help` : liste des commandes.
- `fenetre` / `window` : ouvre ou ferme la fenêtre de console.
- `players` : joueurs et leurs numéros.
- `status` : état de la session.
- `kick <id>` : éjecte un joueur.
- `give <id> <objet> [n]` : donne un objet.
- `resync [id]` : renvoie le monde à un joueur ou à tous.
- `tp <id> [<versId>]` : téléporte les persos d'un joueur.
- `save` : sauvegarde.
- `pause` : pause.
- `speed <n>` : vitesse du jeu.
- `heal <id|all>` : soin complet.
- `xp <id|all> <n>` : +n niveaux partout.
- `god <id|all> [off]` : mode dieu.
- `money` / `argent` : argent de l'escouade.

### Commandes de debug (`plugin/debug.cpp`, actives avec `[debug] commands=1`, via `kcp_cmd`)

- **Session** : host, join, leave, load, status, state, echo, console, consolewin, resync, trace, probe.
- **Persos et lieux** : pos, where, move, moverel, teleport, tpplayer, camto, squadmove, squads, look,
  lookset, stats, modes, xp, money, vitals, kill, ko, kosquad, wake, fight, npcstate, spawnnpc, strand,
  insomething.
- **Animations** : anims, animstats.
- **Objets et contenants** : give, drop, pickup, pickupreq, ground, groundnear, objnear, objreq,
  invmove, loot, lootorder, containerreq, contake, contcount, minereq.
- **Commerce** : merchants, shopcounters, tradegui, tradelist, tradeopen, tradebuy, tradesell,
  tradestate, closewindows.
- **Dialogues** : say, says, dialog, convo, answer (dont `answer leave`), talkreq, talkto, npcevent, dialogs.
- **Ordres** : orderreq, taskreq, bedreq, carryreq, carrying.
- **Éditeur** : editchar, editdone.
- **Monde** : hours, pause, paused, speed, weathers, setweather, rollweather, fxhurry.
- **Portes** : doors, doorsknown, doorstate, doorset, doorlocal, doororder, doorbutton.
- **Factions** : factions, factionsync, relation, setrelation, bounty, givebounty.
- **Prisons** : cage, chain, captive, enslave.
- **Construction** : buildtypes, buildlist, buildcount, buildplace, buildprogress, buildbuy,
  buildforsale, builddismantle, furnplace, furnparent, buildreq, buildinfo, givemoney, itemtypes,
  giveitem, invcount.
- **Divers** : robuststats.

### Expériences du harnais (`tools/coop_test.py`, lancées par `tools/run_all.py`)

run, dead, items, lootui, lootorder, lootclick, soak, walk, trace, bodies, clientpickup, doors,
prison, build, construct, buyhouse, farlong, mine, admin, trade, factions, talk, ranged, jitter, suite, squads, far, kosquad, facing,
menu, four, progress, ground, animframe, anim, gait, fxlive, fx, up, cmd. Chacune lance un hôte et des
clients locaux, rejoue un scénario par les commandes de debug et vérifie les journaux.

## 6. Configuration et fichiers écrits

`KenshiCoop.ini` (créé avec les valeurs par défaut s'il manque, `plugin/util.cpp`) :

| Section | Clé | Défaut | Rôle |
|---|---|---|---|
| player | name | Player | nom affiché |
| network | join_address, port | — | dernière adresse rejointe (réécrite par `SaveConnection`) |
| sync | snap_distance, destination_epsilon, interest_radius | — | recalage, précision, rayon d'intérêt (0 : partout) |
| sync | stream_radius, standin_radius, spawns_per_frame, auto_zone_resync | 3000, 2500, 2, 1 | diffusion par joueur (0.3.1) : rayon autour des persos de chaque joueur, rayon de recréation, créations par image, resync automatique de zone |
| coop | own_character | 1 | un nouveau perso par joueur |
| ui | overlay, host_console | 1, 1 | overlay, console de l'hôte |
| debug | commands, steam_loopback | 0, 0 | commandes de debug, Steam en boucle locale |

Fichiers écrits dans le dossier du jeu : `KenshiCoop.log` (ou `KenshiCoop-<pid>.log` pour une 2e
instance), archive `KenshiCoop-logs/` (30 gardés), registre `KenshiCoop-players.txt`, sauvegardes
`KenshiCoopHost` / `KenshiCoopJoin`, fichiers `kcp_cmd` / `kcp_out` du harnais.

## 7. Risques et problèmes

### Code désactivé ou mort
- `plugin/factions.cpp`:245 `ApplyBounties` retourne 0 tout de suite : le reste de la fonction ne
  sert plus, mais l'hôte envoie toujours Bounties (44) chaque seconde pour rien.
- `plugin/world.cpp`:559 `kStuckDetection = false` : la détection de perso bloqué et le recalage des
  PNJ lointains, documentés « implémenté, à vérifier », ne tournent pas.
- `FnShowLoadWindow` (#109) n'est plus appelée ; `FnAIUpdate4Frame` (#7) n'est ni dans `defs[]` ni
  appelée ailleurs à notre connaissance.
- `Hello::worldHash` est envoyé mais jamais vérifié.
- `plugin/hooks.h`:21 et :30 : `InHostCall()` déclarée deux fois.

### Plantages probables
- operator[] des primes 0x5E7EE0 (#117) : cause suspectée du plantage des 3 clients du 10/10 ; à ne
  pas réactiver sans test dédié.
- Mode dieu : suppose MedicalSystem à Character+0x458 (`hooks.cpp` hk_medDamage) ; si le décalage est
  faux, le test lit un pointeur quelconque (sans plantage, mais mode dieu inopérant).
- `HealCompletely` 0x6464C0 appelée sans `__try` dédié hors `CallVoid` ; vérifiée en partie.
- Allocateur du mode construction 0xED650A / 0xED64F8 (`buildings.cpp`:34) différent de
  operator new/delete 0xED6504 / 0xED64FE : une mémoire allouée par l'un et libérée par l'autre
  corromprait le tas.
- `debug.cpp`, `world.cpp`, `prisons.cpp`, `util.cpp` n'ont aucun `__try` : tout appel au jeu y
  dépend des gardes des fonctions de `kenshi.cpp`.
- Fenêtre de commerce : partage les `Item*` avec les comptoirs du magasin ; un rafraîchissement
  pendant que la fenêtre est ouverte peut laisser des pointeurs libérés.
- Après un resync, plus de cartes d'escouade puis plantage (A_FAIRE) : les caches de handles de
  `world.cpp` ne sont peut-être pas tous vidés.

### Threads
- Le relais Steam (`steam_link.cpp`) tourne sur son propre thread ; il ne doit toucher que la file
  de paquets.
- Certains hooks de dialogue sont appelés depuis des threads de travail du jeu ; `g_hostCall` est
  local au thread (`HostCallScope`), donc un appel lancé par l'hôte puis exécuté sur un autre thread
  n'est pas reconnu comme tel.
- Mode dieu protégé par mutex (`kenshi.cpp`, `g_godMutex`) : correct.

### Adresses
- Adresses en dur hors table (§4) : aucune vérification d'octets au démarrage ; une autre version
  du jeu les rendrait fausses en silence. operator new/delete et la vtable lektor sont déclarées en
  double (`kenshi.h` et `kenshi.cpp`).

### Incohérences doc / code (corrigées dans FONCTIONNALITES.md)
- Les primes étaient décrites comme appliquées chez le client : elles sont désactivées.
- Détection de perso bloqué décrite « implémentée » : elle est coupée.

### Bugs ouverts
Voir [A_FAIRE.md](A_FAIRE.md) : ordre des échanges d'équipement, corps porté, bâtiments existants
en chantier, suppression de tâche, TP lointain, relations corrigées en boucle, ramassage chez nass4,
perso passif qui attaque, PNJ invisibles.
