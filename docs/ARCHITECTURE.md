# Architecture de KenshiCoop

Ce document explique comment le mod est construit et comment chaque système circule entre l'hôte
et les clients. Les adresses et structures du jeu sont dans [MOTEUR.md](MOTEUR.md), l'état de
chaque fonctionnalité dans [FONCTIONNALITES.md](FONCTIONNALITES.md).

## Vue d'ensemble

```
   Kenshi (hôte)                                              Kenshi (client)
 ┌──────────────────────┐                                  ┌──────────────────────┐
 │ jeu (seul à simuler) │                                  │ jeu (copie, sans IA) │
 │   ▲ hooks MinHook    │                                  │   ▲ hooks MinHook    │
 │ plugin/  KenshiWorld │                                  │ plugin/  KenshiWorld │
 │   ▲ IWorld           │                                  │   ▲ IWorld           │
 │ common/  Session     │   ENet (UDP), 3 canaux           │ common/  Session     │
 │   Net ───────────────┼── direct, ou relayé par Steam ───┼── Net                │
 └──────────────────────┘   (plugin/steam_link)            └──────────────────────┘
```

- **`common/`** : le cœur multijoueur, **indépendant du jeu**. Il contient le protocole,
  l'enveloppe ENet et la session hôte/client. Il ne parle au jeu qu'à travers l'interface
  `kc::IWorld`, ce qui permet de le tester entièrement avec un faux monde (`tests/`).
- **`plugin/`** : la DLL chargée par Kenshi. On y trouve :
  - les hooks ;
  - l'accès à la mémoire et aux fonctions du jeu (`kenshi.cpp`) ;
  - l'implémentation d'`IWorld` (`world.cpp`) ;
  - l'overlay ImGui, la console hors du jeu, le relais Steam et le canal de test.

## Chargement de la DLL

1. `install.ps1` :
   - copie `KenshiCoop.dll` dans le dossier de Kenshi ;
   - ajoute `Plugin=KenshiCoop` à `Plugins_x64.cfg`, après une sauvegarde
     `Plugins_x64.cfg.before-kenshicoop`.
   Kenshi la charge alors comme un **plugin Ogre** au démarrage, avant tout chargement de partie.
2. `dllStartPlugin` (`plugin/main.cpp`) épingle la DLL en mémoire, pour qu'elle reste chargée
   jusqu'à la fin du processus, puis appelle `Start()`, qui :
   - ouvre le journal `KenshiCoop.log` et archive le précédent ;
   - installe un filtre de plantage, qui écrit `CRASH ...` et la pile dans le journal ;
   - calcule le **SHA-256 de `kenshi_x64.exe`**. Seule la version 1.0.68 Steam est acceptée
     (`kenshi::kSupportedExeSha256`) ; sinon le mod se désactive et l'écrit dans le journal ;
   - lance `kenshi::Init` : base du module et **vérification des premiers octets (prologue) de
     chaque fonction** de la table `kFunctions`. À la moindre différence, le mod reste désactivé
     plutôt que de risquer un plantage ;
   - lit `KenshiCoop.ini` (créé avec des valeurs par défaut s'il manque) ;
   - initialise Steam ;
   - crée `KenshiWorld` et `kc::Session` ;
   - installe les hooks, l'overlay (hook de `IDXGISwapChain::Present`) et un **frame listener
     Ogre**, qui fait tourner le mod dans les menus et les écrans de chargement.
3. `dllStopPlugin` quitte la session, ferme la console et l'overlay, retire les hooks et ferme le
   journal.

## La boucle (tick)

- **Pendant le jeu** : le hook de `GameWorld::mainLoop_GPUSensitiveStuff` appelle
  `RunTick(live = true)` juste après la boucle principale du jeu.
- **Dans les menus, les chargements, ou en pause** (la boucle principale ne tourne plus) : dès que
  la boucle principale n'a pas tourné depuis 0,2 s, le frame listener appelle `RunTick` à chaque
  image. Le monde n'est considéré comme vivant que si une partie chargée est en pause.
- **Sûreté** :
  - les ticks sont sérialisés par un mutex : un tick qui en chevaucherait un autre est sauté ;
  - les exceptions C++ et les violations d'accès (SEH) sont rattrapées et journalisées. Aucune
    exception ne remonte dans le code du jeu.

Ordre dans `Tick()` (`main.cpp`) :
1. `KenshiWorld::BeginFrame` : escouade de l'image, nouvelle « génération » quand un monde se charge.
2. Messages courts à l'écran (toasts), raccourcis clavier, actions de l'overlay.
3. Canal de test (si `[debug] commands=1`), entretien de Steam, relais du journal et console hors
   du jeu.
4. `Session::Tick` : réseau, puis `HostTick` ou `ClientTick`.
5. Resync et départ de la session, côté client.
6. `KenshiWorld::EndFrame` :
   - impose l'heure, la vitesse et la pause de l'hôte chez les clients ;
   - recale les horloges d'animation ;
   - publie la `HookView`.
7. Publication du modèle de l'overlay.

## Hooks

- **Moteur** : MinHook. Les cibles viennent de `kenshi::kFunctions`
  (`plugin/kenshi.cpp`) : nom, RVA dans `kenshi_x64.exe` 1.0.68 et 12 octets de prologue vérifiés
  au démarrage. Tous les détours sont activés d'un coup.
- **Appels vers le jeu** : chaque appel du mod passe par une petite fonction `__try/__except`
  (`...Seh`). Une faute du jeu donne `false` au lieu d'un plantage.
- **`HostCallScope`** (`g_hostCall`, par thread) : quand le mod appelle lui-même le jeu, par
  exemple pour rejouer un K.-O. de l'hôte chez un client ou exécuter l'ordre d'un client chez
  l'hôte, les hooks de blocage le laissent passer.
- **`AnimReplayScope`** : même principe pour rejouer une animation de l'hôte.
- **`HookView`** : publiée à chaque image par `EndFrame`, et lisible depuis n'importe quel thread
  du jeu (l'IA tourne sur d'autres threads). Elle contient :
  - session active ou non, rôle client ;
  - `squadForeign` : les membres de l'escouade que cette machine ne commande pas ;
  - `controllable` : les handles qu'elle commande ;
  - `replicated` : chez un client, les personnages pilotés par l'hôte ;
  - `facing` (direction voulue) et `anims` (animations de l'hôte) ;
  - la vitesse du jeu.
  `KenshiWorld::ClientActive()` est un booléen atomique pour les hooks très fréquents.

### Les hooks par rôle (`plugin/hooks.cpp`)

**Ordres du joueur** :
- `playerMove`, `addOrderSelectedCharacters`, `newPlayerTaskSelectedCharacters`,
  `addTaskNearestSelectedCharacter`, `addJobSelectedCharacters`, `setOrderSelectedCharacters`,
  `stopCharactersMovement` :
  - chez un client, l'ordre devient une `Command` pour l'hôte et ne s'exécute pas localement ;
  - chez l'hôte, il est refusé si la sélection contient le personnage d'un autre joueur.
- Le clic droit « fouiller » est détourné vers la fouille via l'hôte : `ClientLoot` pour un corps,
  `ClientLootContainer` pour un contenant.
- `playerMoveOrderDefault` sert de filet de sécurité : rien d'autre que le mod ne peut faire
  bouger un personnage que cette machine ne commande pas.

**Ce qu'un client ne décide jamais** (refusé hors `HostCallScope`) :
- IA : `AI::update4Frame`, `AI::periodicUpdate`, `Blackboard::update/periodicUpdate`,
  `FactionWarMgr::periodicUpdate` (raids), `FactionUniqueSquadManager::periodicUpdate` ;
- santé : `MedicalSystem::applyDamage` (sauf au chargement d'une sauvegarde), `knockout`,
  `reassessCollapseMode`, `Character::declareDead`, `ragdollMode` (pour les personnages pilotés
  par l'hôte) ;
- population : `RootObjectFactory::createRandomCharacter` (le client ne peuple pas le monde
  lui-même) ;
- expérience : `increaseStat` ;
- ordres permanents : `Character::setStandingOrder` ; ramasser un corps : `pickupObject` ;
- dialogues et décisions : `Dialogue::sendEvent/sendEventOverride/startConversation/
  startPlayerConversation/_doActions`, `SensoryData::dialogAssessmentUpdate/assessCrimes` ;
- relations et crimes : `FactionRelations::affectRelations` (deux variantes), `setRelation`,
  `BountyManager::setCrime/assignBountyForCrimes` ;
- météo : `Season::getNewWeather`, `EffectHandler::affectObjects`, et la reconstruction des effets
  (`updateWeatherEffects`) sauf après un changement de météo venu de l'hôte ;
- objets :
  - `Character::giveItem` depuis le sol est refusé. Le « ramasser » du joueur passe par
    `PlayerInterface::pickupItem`, qui est redemandé à l'hôte ;
  - `CharacterHuman::dropItem` est redemandé à l'hôte ;
- animations : `AnimationClass::*` (combat, actions, trébuchés, modes combat et portage, gardes),
  `drawWeapon`, `sheatheWeapon` sur les personnages pilotés par l'hôte. `animationSelection` est
  sautée pour eux, et `trackAnimationMovement` est coupé : les animations ne les déplacent pas.

**Ce que l'hôte capture** :
- `Dialogue::say` : les bulles ;
- `Dialogue::setInDialog`, `setResponesGUI`, `setConversationReplyGUI` : la conversation d'un
  autre joueur part chez lui et la fenêtre de l'hôte reste fermée ;
- `giveItem` et `dropItem` : les objets au sol ;
- les hooks d'animation ;
- `createScreenLabel`, `setTracking`, `setColor` : les chiffres de dégâts créés par `addWound` ;
- `WeatherRegion::updateBT` : la météo de chaque région ;
- `EffectHandler::EffectHandler` : chez un client, pour placer l'effet à l'endroit de l'hôte.

**Divers** :
- `ActivePlatoon::addCharacterAt` : déplacement d'escouade, redemandé à l'hôte par un client ;
- `ForgottenGUI::closeCharacterEditor` : validation de l'éditeur ;
- `combatMovementUpdate` : impose la direction de l'hôte en combat ;
- `SingleAnimation::update` : impose temps et poids des animations de l'hôte ;
- `EffectHandler::stop` : diagnostic.

## Session (`common/`)

### États
`Idle`, `Hosting`, `Connecting`, `Handshake`, `Downloading`, `Loading`, `Connected`, `Failed`.

### Transport
- ENet, un message par paquet, trois canaux :

  | Canal | Type | Messages |
  |---|---|---|
  | 0 | fiable, ordonné | tout le reste |
  | 1 | non fiable, séquencé | `Snapshot`, `AnimFrame` |
  | 2 | non fiable, séquencé | `Vitals` |

- Chaque type de message n'est accepté que sur son canal.
- Un pair muet est coupé après 5 à 15 s (`enet_peer_timeout` 5 s / 15 s), ce qui repère vite un
  jeu planté. `Net::ExpectSilence` porte ce délai à 30 s / 2 min pendant qu'un jeu charge une zone
  (TP admin, message `Stall`), puis le remet.
- Fil de maintien (`Net::KeepAlive`) : si le fil du jeu n'a pas appelé `Poll` depuis 0,3 s (jeu
  figé), il appelle `enet_host_service` lui-même (sous `mu_`, sans jamais attendre le fil du jeu)
  pour acquitter ce qui arrive et répondre aux pings ; les événements sont gardés pour le `Poll`
  suivant, dans l'ordre. Un jeu figé reste donc connecté, un jeu planté se tait et est coupé. Au
  plus 90 s (un jeu bloqué pour de bon finit coupé). `Net::silentMs` : temps depuis le dernier
  paquet d'un pair.
- Départ d'un joueur, propre ou non (`Session::OnDisconnect` → `ForgetPlayer`) : ses persos
  reviennent à l'hôte et sont arrêtés au tick suivant (`IWorld::HaltCharacter`), mémorisés par
  compte (`leftOwned_`) et rendus s'il revient ; tout ce qui porte son id est effacé (ordres,
  `pendingInvOps_`, apparences, conteneurs demandés ou en route, `openBy`, `trades_`, poses et
  achats de bâtiments, portes, conversations, `buildSyncedPlayers_`, `factionsServed_`) : l'id
  sera peut-être celui d'un autre joueur. Une nouvelle connexion du même compte (ou, sans compte,
  du même nom sur une connexion muette depuis 2 s) remplace l'ancienne **avant** le choix du nom
  et de l'id.
- Client : le hook de `SaveManager::save` refuse toute sauvegarde tant que le client est en session.
- Un nouveau monde (`KenshiWorld::BeginFrame`) est un autre joueur ou une escouade dont les objets
  ont changé, pas une simple coupure du tick. Il remet tout l'état lié au monde à zéro
  (`ResetWorldBound`) : alias, pointeurs, demandes en attente, caches.
- Sérialisation little-endian vérifiée à chaque lecture (`kc/wire.h`) : varints, chaînes bornées.
- Les quaternions sont compressés sur 32 bits (« smallest three »).

### Cadences côté hôte

| Quoi | Cadence |
|---|---|
| Instantanés (`Snapshot`) | 20 Hz, seulement ce qui bouge ou change, sinon un rafraîchissement par seconde ; paquets ≤ 1100 octets |
| Santé (`Vitals`) | 5 Hz, ce qui change, sinon toutes les 2 s |
| Animations à l'écran (`AnimFrame`) | 15 Hz, personnages à moins de 1500 unités d'un membre de l'escouade |
| État des animations (`Anim` État / Arme) | toutes les secondes, plus chaque événement au moment où il arrive |
| Heure (`TimeState`) | toutes les 0,5 s, et à chaque changement de vitesse ou de pause |
| Inventaires (`Inventory`) | vérifiés toutes les 0,5 s, envoyés s'ils ont changé (empreinte) |
| Progression (`Progress`) | chaque seconde ce qui change, tout toutes les 20 s ; l'argent quand il change |
| Escouades (`Squads`) | vérifiées toutes les 0,5 s, envoyées si elles changent, sinon toutes les 10 s |
| Météo (`Weather`) | dès qu'une région change, et tout toutes les 10 s |
| Effets météo (`Effects`) | dès qu'ils apparaissent, bougent ou finissent ; l'ensemble complet toutes les 5 s |
| Intérêt (qui est répliqué) | recalculé toutes les 0,5 s |

### Cadences côté client

| Quoi | Cadence |
|---|---|
| Positions | chaque image, vers l'état interpolé à 50 ms en arrière, avec 1 s d'historique |
| Santé | réimposée toutes les 0,25 s |
| Compétences et ordres permanents | à l'arrivée, et toutes les 3 s |
| Argent | à l'arrivée, et toutes les 3 s |
| Escouades | toutes les 2 s |
| Comparaison d'inventaire (`InvOp`) | toutes les 0,2 s |
| Lignes du journal vers l'hôte | toutes les 0,5 s |
| Rapport de synchro | toutes les 5 s |
| Présence des PNJ | vérifiée toutes les secondes |

## Entités, netIds et handles

- **Handle** : le `hand` de Kenshi, avec `type`, `container`, `containerSerial`, `index` et
  `serial`. Pour les personnages et les objets venus **de la même sauvegarde**, il est identique
  sur toutes les machines. C'est ce qui permet de les apparier.
- **Entité** : tout ce que l'hôte réplique reçoit un **netId**. `Bind` (netId, handle, propriétaire,
  membre de l'escouade, moyen de recréer le personnage) le fait connaître, `Unbind` l'oublie.
  - Propriétaire : 0 pour un PNJ du monde, 1 pour l'hôte, 2 à 8 pour les clients. Seul le
    propriétaire peut commander un membre de l'escouade.
- **Le handle change** quand un personnage meurt ou change d'escouade :
  - l'hôte reconnaît le même personnage par son identité (`IWorld::Identity`, l'adresse de
    l'objet), garde son netId et renvoie un `Bind` avec `previous` ;
  - le client fait pointer le nouveau handle vers sa propre copie (`Rehandle`, table `alias_`) ;
  - quand c'est le client qui change ses escouades, `LocalRehandled` met cette table à jour.
- **Remplaçants** (stand-ins) : un personnage que l'hôte a et pas le client est recréé
  (`Spawn` : modèle et faction), ou adopté parmi les « inconnus » de même type que le jeu du client
  a créés (`Reconcile`). Le client garde l'association handle de l'hôte → handle local
  (`HostHandleOf` dans l'autre sens).
- **Meubles, bâtiments, objets de ville** : **leurs handles diffèrent d'une machine à l'autre**.
  On les désigne donc par **type et endroit** : l'identifiant de modèle (`sid`) et la position.
  On les retrouve avec une requête dans les grilles spatiales du jeu (`ObjectsNear`, grilles
  +0x48 bâtiments et meubles, +0x80 objets).
- **Contenants ouverts** : ce sont des entités sans personnage (`container = true`), connues
  seulement des joueurs qui les ont ouverts (`openBy`).
- **Intérêt** : avec `[sync] interest_radius = 0` (défaut), tous les personnages actifs du jeu de
  l'hôte sont répliqués, ainsi que les cadavres à moins de 1000 unités d'un membre de l'escouade.
  Avec un rayon, un PNJ ne disparaît qu'au-delà de 1,25 fois le rayon (hystérésis).

## Modèle d'autorité

- L'hôte est seul à simuler.
- **Ce que le client envoie** : des demandes, jamais d'état :
  - ordres (`Command`) ;
  - déplacements d'objets (`InvOp`) ;
  - ouvertures et fermetures de contenants ;
  - réponses de conversation ;
  - nouvelle apparence de **son** personnage ;
  - état de son éditeur ;
  - journaux et rapports.
- **Ce que l'hôte vérifie** :
  - qu'un ordre concerne un personnage du joueur ;
  - qu'un déplacement d'objet part de ses personnages, d'un PNJ à terre ou d'un contenant qu'il a
    ouvert, et va vers ses personnages ou ce contenant ;
  - qu'une réponse vise une conversation de ce joueur ;
  - qu'une apparence est celle de son personnage.
- **Divergence** : tout état répliqué est réécrit par le message suivant. Le client ne peut pas
  diverger durablement. Une prédiction refusée est annulée, parce que l'hôte renvoie l'état réel.

## Messages (`common/include/kc/protocol.h`)

Version du protocole : **27** au moment de la rédaction. Elle augmente à chaque changement de
format, et une version différente est refusée à la connexion.

| # | Message | Sens | Rôle |
|---|---|---|---|
| 1 | Hello | C→H | version, empreinte de l'exe, des mods, nom, identifiant Steam |
| 2 | Welcome | H→C | ton numéro de joueur, heure de l'hôte, liste des joueurs |
| 3 | Reject | H→C | refus ou exclusion, avec la raison |
| 4 | PlayerJoined | H→C | un joueur arrive |
| 5 | PlayerLeft | H→C | un joueur part |
| 6 | Chat | ⇄ | ligne de discussion |
| 7 | Bind | H→C | netId ↔ handle, propriétaire, escouade, moyen de le recréer, ancien handle |
| 8 | Unbind | H→C | entité oubliée |
| 9 | Snapshot | H→C (non fiable) | position, rotation, destination, drapeaux (bouge, court, à terre, mort), cible de combat, allure, corps porté |
| 10 | Command | C→H | ordre pour un personnage du joueur (aller, arrêter, ramasser, tâche, changement d'escouade) |
| 11 | TimeState | H→C | vitesse, pause, heure du jeu |
| 12 / 13 | Ping / Pong | ⇄ | mesure du ping |
| 14 | Vitals | H→C (non fiable) | sang, minuteur de K.-O., faim, inconscient ou mort, chair, étourdissement et bandage de chaque membre |
| 15 | WorldBegin | H→C | début du transfert du monde (taille, nombre de fichiers) |
| 16 | WorldChunk | H→C | morceau d'un fichier de la sauvegarde (16 Ko) |
| 17 | WorldEnd | H→C | fin du transfert et empreinte attendue |
| 18 | Ready | C→H | monde chargé (empreinte vérifiée par l'hôte) |
| 19 | Weather | H→C | météo de chaque région |
| 20 | Inventory | H→C | inventaire complet d'une entité |
| 21 | InvOp | C→H | déplacement ou dépôt d'objet fait dans l'interface d'inventaire |
| 22 | Effects | H→C | effets météo placés, déplacés ou terminés |
| 23 | Anim | H→C | début ou fin d'animation (attaque, action, trébuché, modes, arme, chiffres de dégâts) |
| 24 | AnimFrame | H→C (non fiable) | tout ce que joue chaque personnage proche (nom, temps, poids, vitesse, horloge) |
| 25 | Ground | H→C | objet posé au sol ou ramassé |
| 26 | Progress | H→C | compétences, ordres permanents, style de combat, argent de la faction |
| 27 | Dialog | H→C | bulles (à tous) ; ouverture, texte et fermeture d'une conversation (au joueur concerné) |
| 28 | DialogReply | C→H | réponse choisie |
| 29 | Squads | H→C | escouades : nom et membres dans l'ordre |
| 30 | Appearance | ⇄ | apparence complète et nom d'un personnage (éditeur) |
| 31 | EditCharacter | H→C | ouvre l'éditeur sur ton nouveau personnage |
| 32 | EditState | C→H | éditeur ouvert ou fermé (l'hôte met la partie en pause en attendant) |
| 33 | ClientLog | C→H | lignes du journal du client |
| 34 | ClientReport | C→H | rapport de synchro (toutes les 5 s) |
| 35 | Resync | H→C | recharge le monde de l'hôte maintenant |
| 36 | ContainerOpen | C→H | mon personnage veut regarder dans ce contenant (type, endroit) |
| 37 | ContainerOpened | H→C | il y est : netId du contenant (son contenu suit en `Inventory`) |
| 38 | ContainerClose | ⇄ | fenêtre fermée (client) ou à fermer (hôte : vol repéré, trop loin) |
| 43 | Factions | H→C | relations de la faction du joueur avec chaque faction, dans les deux sens ; rang et réputation |
| 44 | Bounties | H→C | primes par faction, crime en cours, peine de prison et laissez-passer de chaque personnage de l'escouade |
| 40 | Doors | H→C | portes et serrures près des joueurs : ouverte/fermée, verrouillée, niveau de serrure, cassée (lot A) |
| 41 | DoorRequest | C→H | le joueur a cliqué un bouton du panneau d'une porte (ouvrir, verrouiller) (lot A) |
| 49 | Captives | H→C | lot D : personnages en cage, enchaînés, esclaves, évadés, enlevés, en peine de prison (et ceux libérés, une fois) |
| 46 | Shots | H→C | (lot C) projectiles tirés par les personnages et tourelles de l'hôte : tireur, cible, point visé, orientation de départ, tourelle |
| 47 | Ranged | H→C | (lot C) point visé des personnages en combat à distance, tourelles proches des joueurs, fins de combat à distance |
| 52 | BuildPlace | ⇄ | lot E : une pose du mode construction (client : demande à l'hôte, netId 0 ; hôte : à bâtir par tous, avec son netId) |
| 53 | BuildState | H→C | lot E : avancement des chantiers suivis (terminé, en pause, en démontage), avec type et endroit |
| 54 | BuildRemove | H→C | lot E : un bâtiment suivi a été détruit pour de bon chez l'hôte |
| 55 | BuildAction | ⇄ | lot E : acheter / démonter (client : demande ; hôte : rejeu d'un achat chez tous) |
| 70 | Stall | H→C | fix G6 : ton jeu va se figer (TP lointaine, zone à charger), la connexion attend jusqu'à 2 min |
| 71 | Floors | H→C | fix G6 : groupe d'étage (`CharMovement::floorGroup`) des persos, à chaque changement et toutes les 5 s |

## Les flux, système par système

### Rejoindre (`Session::HostJoinFlow`, `KenshiWorld` export et import)
1. Le client se connecte et envoie `Hello`. L'hôte vérifie la version, l'exe, les mods et le nom.
   - Un nom déjà pris devient « Nom 2 ».
   - Une connexion du même compte Steam remplace l'ancienne.
   - L'hôte répond `Welcome` et annonce `PlayerJoined` aux autres.
2. L'hôte **gèle son monde** (`HoldForJoin` : vraie pause, réimposée si quelqu'un appuie sur
   lecture). Avec `own_character=1`, il retrouve ou crée le personnage du joueur
   (`EnsurePlayerCharacter`) **avant** de sauvegarder.
3. L'hôte sauvegarde dans l'emplacement `KenshiCoopHost`. Le jeu efface sa demande avant d'avoir
   tout écrit : l'hôte attend que `quick.save` existe et que la taille du dossier ne bouge plus
   pendant 1 s.
4. Il envoie `WorldBegin`, des `WorldChunk` et `WorldEnd`, avec des chemins validés et des tailles
   bornées.
5. Le client écrit les fichiers dans `KenshiCoopJoin` (seul dossier qu'il efface), demande le
   chargement, puis attend un monde dont l'empreinte (les handles de l'escouade) correspond depuis
   une seconde. Il envoie alors `Ready`.
6. L'hôte vérifie l'empreinte puis appelle `FinishJoin`. Il envoie au joueur :
   - les `Bind` de toutes les entités, l'heure et les inventaires ;
   - la météo complète, puis les effets une seconde plus tard ;
   - les escouades ;
   - `EditCharacter` si le personnage vient d'être créé.

### Positions (`SendSnapshots`, `KenshiWorld::Apply`)
- Le client garde 1 s d'instantanés par entité et rend l'état à `heure de l'hôte − 50 ms`. Il
  interpole entre deux instantanés, sans extrapoler ; au-delà de `snap_distance`, il saute.
- `Apply` fait, dans l'ordre :
  1. abandonne les tâches que le personnage avait dans la sauvegarde (`reThinkCurrentAIAction`,
     au plus toutes les 2 s) ;
  2. aligne la posture (à terre ou debout) après 0,3 s de différence ; une chute est d'abord
     amenée à l'endroit de l'hôte (`ReadyToFall`) ;
  3. impose l'allure de l'hôte ;
  4. donne la destination de l'hôte à la locomotion du jeu ;
  5. impose la direction ;
  6. tire la position vers celle de l'hôte : 25 % de l'écart par image, 50 % au-delà de 2 unités.
- Quand l'hôte se met en pause, le client tourne 0,3 s à vitesse 0,01 (`EndFrame`), pose chaque
  personnage exactement, puis se met en pause et réessaie toutes les 0,5 s si le jeu refuse.

### Santé (`SendVitals`, `KenshiWorld::ApplyVitals`)
- Les valeurs sont écrites directement (`WriteVitals`). Mort et K.-O. sont rejoués une fois, à
  l'endroit de l'hôte.
- Si le jeu du client a de lui-même marqué un personnage mort ou inconscient alors que l'hôte dit
  non, ces marques sont effacées.

### Progression (`SendProgress`, `ApplyProgress`)
- 34 compétences, ordres permanents (bits) et style de combat.
- Les ordres permanents sont posés avec `Character::setStandingOrder` sur le personnage lui-même,
  pas via la sélection.
- L'argent de la faction du joueur, lu dans ses Ownerships.

### Escouades (`SendSquads`, `ApplySquads`)
- L'hôte lit les escouades, dans l'ordre d'apparition de leur premier membre.
- Le client prend pour chaque escouade de l'hôte l'escouade locale qui a déjà le plus de ses
  membres, ou en crée une (4 au plus par passe). Il y range les membres dans l'ordre, la renomme,
  puis montre une de ses escouades si la barre d'escouade est vide.
- Le portrait déposé par un client devient `Command SquadMove`.

### Ordres (`RouteOrder`, `Session::ApplyCommand`, `KenshiWorld::Order`, `RunPlayerTask`)
1. Le client note par quelle fonction de l'interface l'ordre est passé (`TaskVia`), le numéro de
   tâche, le sujet (handle, plus type et position si ce n'est pas un personnage) et le bâtiment de
   destination (handle, type, position).
2. L'hôte retrouve le sujet et le bâtiment, par type et endroit s'il le faut.
3. Il sélectionne **ce seul personnage**, appelle la même fonction d'interface, puis restaure
   exactement la sélection, l'escouade affichée et le personnage du panneau de détails
   (`WithSelection`).
4. Chaque ordre est journalisé : `[nom] order "..." (n) on X -> ok/FAILED`.

### Dialogues (`SendDialogs`, `KenshiWorld::Note*`)
- Les bulles de l'hôte partent à tout le monde.
- Pour la conversation d'un personnage d'un autre joueur, les hooks de la fenêtre de dialogue de
  l'hôte produisent `Open`, `Text` (texte et réponses) et `Close`, envoyés **à ce joueur
  seulement**. La fenêtre de l'hôte reste fermée.
- Le client affiche une fenêtre de l'overlay ; la réponse revient en `DialogReply`, que l'hôte
  rejoue avec `Dialogue::replyClicked`.

### Inventaires (`SendInventories`, `ClientInventoryDiff`, `HostInvOp`)
1. L'hôte envoie l'inventaire complet de chaque entité dont l'empreinte a changé ; celui d'un
   contenant ne part qu'à ceux qui l'ont ouvert.
2. Le client reconstruit l'inventaire local à l'identique (3 essais).
3. Toutes les 0,2 s, il compare ce que montre son jeu à la dernière version de l'hôte. Chaque
   objet disparu ici et apparu là, du même type, devient un `InvOp Move`.
4. Le client attend la réponse de l'hôte 3 s au plus. Un objet parti sans arriver nulle part
   (tenu à la souris) laisse 10 s avant le retour à l'état de l'hôte.
5. L'hôte vérifie les droits, fait passer le test de vol si l'objet sort d'un contenant vers un
   personnage, déplace l'objet (`MoveInventoryItem`), puis renvoie les deux inventaires.
6. Un objet jeté par le client devient `InvOp Drop`.

### Factions et primes (`common/src/session_factions.cpp`, `plugin/factions.cpp`)
1. Chaque seconde, l'hôte lit les relations de la faction du joueur (`IWorld::ReadFactions`) et les
   primes de chaque membre de l'escouade (`ReadBounties`), encode les deux messages et compare leur
   empreinte à la dernière envoyée.
2. Changement : envoi à tous les joueurs en jeu (et une ligne de journal par valeur qui a bougé).
   Sinon : envoi aux seuls joueurs qui viennent d'arriver. Envoi complet toutes les 15 s.
3. Le client garde la dernière version et l'impose (`ApplyFactions`, `ApplyBounties`) à son arrivée
   puis toutes les 2 s. Les valeurs que le jeu local avait changées sont comptées
   (`factionsync`), et notées au journal au plus toutes les 30 s.
4. Les fonctions du jeu qui créent une entrée manquante : `FactionRelations::getRelationData`
   (`0x6B4C60`) et l'`operator[]` de la table des primes (`0x5E7EE0`). Le reste est écrit
   directement (voir [MOTEUR.md](MOTEUR.md), « Factions, relations et primes »).
### Portes et serrures (`HostDoors`, `ClientDoors`, `common/src/session_doors.cpp`, `plugin/doors.cpp`)
1. Toutes les 0,5 s, l'hôte lit (`KenshiWorld::ReadDoors`) les portes (`DoorStuff`) et les meubles
   à serrure (`DoorLock`) à moins de 400 unités de chaque membre de l'escouade. Une entrée : type,
   endroit, état de la porte, ouverture, drapeaux (verrouillée, cassée, serrure cassée, se verrouille
   en se fermant), niveau de serrure.
2. Il envoie à tous les joueurs en jeu celles qui ont changé (`Doors`), et toutes les 5 s la liste
   complète (par morceaux de 256).
3. Le client garde l'état de l'hôte par « type + endroit ». Il retrouve sa copie de l'objet
   (`FindDoor`, mise en cache par handle) et applique l'état (`ApplyDoor`, sous `HostCallScope`) :
   ouvrir et fermer par les fonctions du jeu, verrou et niveau écrits directement. Il réapplique
   tout toutes les 3 s (64 objets par image au plus).
4. Chez le client, les hooks `openDoor`, `closeDoor`, `lockDoor`, `unlockDoor` refusent tout appel
   hors `HostCallScope`. Les hooks des boutons du panneau (`openButton`, `lockButton`) mettent le
   clic en file : il part à l'hôte en `DoorRequest`, et l'hôte appuie sur le même bouton
   (`ExecuteDoorRequest`).
5. Les ordres sur une porte (tâches 72, 73, 76, 77, 78, 81) passent par le chemin normal des
   ordres : la porte est retrouvée chez l'hôte par type et endroit.
6. `HostContainers` refuse d'ouvrir un contenant dont la serrure tient (`ContainerLocked`).

### Contenants (`HostContainers`, `ClientContainers`)
1. Clic droit chez le client : `ContainerOpen` avec le personnage, le type et l'endroit.
2. L'hôte retrouve le contenant (même type à moins de 30 unités) et lui donne un netId. Il fait
   marcher le personnage jusqu'à 25 unités au plus.
3. Une fois le personnage arrivé, l'hôte note le joueur dans `openBy` et envoie `ContainerOpened`,
   puis le contenu en `Inventory`.
4. Le client retrouve le contenant chez lui, attend son contenu, puis ouvre la fenêtre de fouille
   du jeu (`showTradeWindow`, type 2).
5. La fermeture vient du client (fenêtre fermée), ou de l'hôte : vol repéré, ou personnage à plus
   de 50 unités.

### Captivité (lot D : `HostCaptives`, `ClientCaptives`, `plugin/prisons.cpp`)
1. L'hôte relit toutes les 0,5 s la captivité de chaque entité (`KenshiWorld::ReadCaptive` :
   cage par type et endroit, menottes, propriétaire, état d'esclave, faction maître, évadé,
   enlevé, peine) et envoie ce qui a changé ; un personnage libéré part une fois « vide », puis
   est oublié. Toutes les 5 s, tous les captifs repartent.
2. Le client garde l'état de l'hôte par netId, l'impose dès réception puis toutes les 2 s
   (`ApplyCaptive`) : `setPrisonMode` sur la même cage locale, puis les champs.
3. Hooks : `setPrisonMode`, `setChainedMode` (personnages répliqués) et `setSlaveState` sont
   refusés chez un client hors `HostCallScope`.
4. `KenshiWorld::Apply` saute les corrections de position d'un personnage que l'hôte tient en
   cage ; `ApplyVitals` le met inconscient sans le faire tomber.
5. Les évasions, crochetages et libérations sont des ordres : chemin habituel (`RouteOrder` →
   hôte).
### Combat à distance (`session_ranged.cpp`, `plugin/ranged.cpp`, lot C)
1. Chez l'hôte, le hook de `GunClass::shoot` note chaque tir après coup (sur le fil qui tire) :
   - le tireur et sa cible ;
   - le point visé ;
   - l'orientation du projectile, attrapé par le hook de la réserve de projectiles ;
   - la tourelle, par type et endroit.
2. `HostRanged` envoie les tirs dont le tireur est une entité suivie (`Shots`, fiable). Toutes les
   0,2 s, il envoie aussi ce qui a changé (tout, toutes les 2 s) :
   - le point visé des personnages en combat à distance ;
   - les fins de combat à distance ;
   - les tourelles à moins de 400 unités d'un joueur.
3. Chez le client, `ClientRanged` traite les tirs à l'image suivante. `ReplayShot` prend l'arme du
   personnage, ou celle de la tourelle locale retrouvée par type et endroit, et appelle `shoot`
   dans une `HostCallScope`. Il remet ensuite le nouveau projectile sur l'orientation de l'hôte.
4. Les points visés sont réimposés à chaque image, et les tourelles tournées avec `aimAt`.
5. Le hook de `shoot` refuse chez les clients tout tir qui ne vient pas de KenshiCoop. Les
   dégâts des projectiles du client sont refusés comme tous les autres (`applyDamage`).
### Bâtiments (lot E : `session_buildings.cpp`, `plugin/buildings.cpp`)
1. Mode construction : `<PreviewGroup>::createBuildings` (`0x4D72A0`) appelle
   `RootObjectFactory::createBuilding` (`0x57CC70`) pour chaque bâtiment posé. Les hooks captent
   les arguments (`BuildArgs`) ; chez un client, la fabrique renvoie null (rien n'est bâti, le mode
   construction le supporte : son message d'erreur est une fonction vide en version finale).
2. Les arguments deviennent un `BuildPlace` : pointeurs remplacés par type + endroit (bâtiment
   parent du plan, bâtiment où il se trouve, bâtiment auquel il s'accroche) et handle de la ville.
3. Client → `BuildPlace` (netId 0) ; l'hôte bâtit (`ExecutePlacement` : même fabrique, puis
   `setupMiningResourceLevel` et `clearUsageNodes` comme le mode construction), donne un netId et
   renvoie le `BuildPlace` à tous ; chaque client bâtit pareil et associe netId → handle local.
4. L'hôte suit ces bâtiments et les chantiers proches des joueurs (toutes les 3 s) ; `BuildState`
   part chaque seconde pour ce qui change, toutes les 10 s pour tout, et en entier à un joueur qui
   arrive. Le client impose l'avancement (`setConstructionProgress`, vt 0x238), la fin
   (`notifyConstructionComplete`), la pause et le démontage.
5. Hook de `GameWorld::destroy` chez l'hôte : un bâtiment suivi détruit pour de bon → `BuildRemove`.
6. Achat / démontage confirmés chez un client (hooks de `buyMeCallback` / `confirmDismantle`) →
   `BuildAction` ; l'hôte exécute ; un achat réussi est rejoué par chaque client.

### Objets au sol (`Ground`, `PickUp`)
- L'hôte rapporte chaque objet posé (type, endroit) et ramassé.
- Le client crée une copie à l'endroit de l'hôte, ou retire l'objet : sa copie, ou le même type à
  moins de 15 unités pour un objet venu de la sauvegarde.
- « Ramasser » chez un client devient `Command PickUp` : le personnage marche jusqu'à l'objet chez
  l'hôte, qui le lui donne à moins de 15 unités (délai 30 s).

### Animations (`Anim`, `AnimFrame`)
- Les événements (fiables) rejouent attaques, actions, trébuchés, modes, armes et chiffres de
  dégâts.
- `AnimFrame` décrit tout ce qui est joué à l'écran. Le client démarre ce qui manque, coupe le
  reste, et met temps et poids de chaque animation à ceux de l'hôte, en phase pour les boucles.
- L'horloge maîtresse est une phase entre 0 et 1, recalée en douceur dans `EndFrame`.

### Météo et effets (`Weather`, `Effects`)
- Après la mise à jour d'une région par le jeu (thread d'arrière-plan), l'hôte lit sa météo.
- Le client écrit celle de l'hôte **avant** sa propre mise à jour.
- Les effets (éclairs, rayons, tempêtes, nuages de gaz) à moins de 9000 unités d'un joueur sont
  annoncés avec leurs tirages aléatoires. Le client les place dans le même groupe d'effets, au
  même endroit, et les fait bouger et disparaître avec ceux de l'hôte.

### Heure (`TimeState`)
- Le client impose vitesse et pause, avec les mêmes appels que la barre d'espace et F2 à F4.
- L'heure elle-même n'est pas écrite : le jeu la recalcule à chaque image. Les deux horloges
  partent de la même sauvegarde et tournent à la même vitesse, mais cela seul dérive : chaque
  changement de vitesse ou pause arrive en retard chez le client (demi aller-retour, une image).
  Le soak de 20 min finissait à 0,01 h (36 s de jeu) d'écart.
- `ClockSync` (`common/include/kc/clock.h`, côté client) : l'hôte envoie son heure avec chaque
  `TimeState` (deux fois par seconde). Le client estime l'heure de l'hôte « maintenant » (dernière
  heure reçue + vitesse × temps écoulé + demi aller-retour ; heures de jeu par seconde mesurées sur
  les messages de l'hôte) et la compare à la sienne. Écart > 0,003 h, les deux jeux tournant : sa
  vitesse passe à ±2 % de celle de l'hôte (±5 % au-delà de 0,006 h) jusqu'à moins de 0,0005 h.
  Jamais en pause, jamais au-delà de 0,25 h d'écart (un autre monde, pas une dérive).

### Éditeur de personnage
1. `EditCharacter` : le client ouvre l'éditeur sur son personnage.
2. `EditState` : l'hôte met la partie en pause tant qu'un joueur édite (10 min au plus).
3. Validation (hook `closeCharacterEditor`) : le client envoie `Appearance` (toutes les valeurs de
   la GameData d'apparence et le nom).
4. L'hôte l'applique (le jeu reconstruit le corps) et la renvoie aux autres. Les éditions de
   l'hôte partent à tout le monde.

### Resync
- `RequestResync` (bouton, ou commande `resync [id]`) envoie `Resync` au joueur.
- Le client quitte la session et rejoint la dernière adresse 1,5 s plus tard (`steam::LastJoin`) :
  il recharge le monde actuel de l'hôte.

### Journaux des clients et rapports
- Le client recopie chaque nouvelle ligne de son journal, sans l'horodatage et sans les lignes du
  canal de test, et les envoie en `ClientLog`. L'hôte les écrit sous la forme `[nom] ...`.
- `ClientReport` donne le nombre de personnages suivis, les PNJ et membres d'escouade manquants,
  les personnages décalés de plus de 5 unités, la plus grande correction et le nombre d'images par
  seconde. Détails dans [JOURNAUX.md](JOURNAUX.md).

### Console hors du jeu (`host_console.cpp`)
- Fenêtre Win32 avec son propre thread.
- Le thread du jeu publie un modèle (ligne d'état, joueurs) toutes les 0,5 s. La fenêtre relit le
  journal en mémoire toutes les 250 ms, et transmet les commandes tapées au thread du jeu.

### Relais Steam (`plugin/steam_link.cpp`)
- Le mod utilise l'API plate de `steam_api64.dll`, déjà chargée par Kenshi.
- Les paquets P2P partent en non fiable : la fiabilité est assurée par ENet.
- **Côté hôte**, chaque ami connecté par Steam est relié à un socket UDP local qui parle au port
  ENet de l'hôte.
- **Côté client**, un port UDP local reçoit la connexion d'ENet et tout part vers le compte Steam
  de l'hôte.
- Le relais tourne sur son propre thread.
- La MTU d'ENet est abaissée à 1200 octets, la taille d'un datagramme Steam.
- La présence enrichie contient :
  - `kc_host` : annonce la partie aux amis ;
  - `connect` = `+kc_join <id>` : le bouton « Rejoindre la partie » de Steam ;
  - `status` ;
  - `kc_join`, côté client.
- Le mode de test `[debug] steam_loopback=1` remplace Steam par de l'UDP local (port hôte 28100).

## Fichiers source

| Fichier | Contenu |
|---|---|
| `common/include/kc/protocol.h`, `src/protocol.cpp` | messages, encodage et décodage, libellés des tâches et ordres permanents |
| `common/include/kc/session.h`, `src/session.cpp` | session hôte et client, interface `IWorld` |
| `common/include/kc/net.h`, `src/net.cpp` | enveloppe ENet mono-thread |
| `common/include/kc/wire.h` | `Writer` et `Reader` vérifiés |
| `plugin/main.cpp` | démarrage, tick, raccourcis, commandes de console, modèle de l'overlay, resync, départ |
| `plugin/hooks.cpp`, `hooks.h` | tous les hooks, `HostCallScope`, exécution des ordres côté hôte |
| `plugin/kenshi.cpp`, `kenshi.h` | table des fonctions, offsets, accès à la mémoire du jeu |
| `plugin/world.cpp`, `world.h` | `KenshiWorld` (`IWorld`), `HookView` |
| `plugin/overlay.cpp`, `overlay.h` | overlay ImGui : panneau d'état, fenêtre Multijoueur, console, conversation |
| `plugin/host_console.cpp`, `host_console.h` | console Windows hors du jeu |
| `plugin/steam_link.cpp`, `steam_link.h` | relais Steam P2P, amis, présence |
| `plugin/util.cpp`, `util.h` | journal (et son archive), configuration, SHA-256 |
| `plugin/debug.cpp`, `debug.h` | canal de commandes de test |
| `tests/test_main.cpp` | tests unitaires (protocole, fuzzing, sessions avec faux monde) |
| `tools/coop_test.py` | harnais de test en jeu |
| `build.ps1`, `install.ps1`, `package.ps1` | compilation, installation, zip pour les amis |

## Configuration (`KenshiCoop.ini`, dossier de Kenshi)

| Section / clé | Défaut | Rôle |
|---|---|---|
| `[player] name` | nom du compte Windows | nom du joueur et de son personnage |
| `[network] join_address` | `127.0.0.1` | IP ou `steam:<id>` de l'hôte |
| `[network] port` | 27960 | port UDP |
| `[sync] snap_distance` | 15 | au-delà, un personnage est téléporté au lieu d'être interpolé |
| `[sync] destination_epsilon` | 2 | une destination n'est redonnée que si elle bouge de plus que ça |
| `[sync] interest_radius` | 0 | 0 : tous les personnages actifs sont répliqués |
| `[coop] own_character` | 1 | un personnage par joueur qui rejoint |
| `[ui] overlay` | 1 | panneau d'état en haut à droite |
| `[ui] host_console` | 1 | la console hors du jeu s'ouvre seule quand on héberge |
| `[debug] commands` | 0 | canal de test (voir [TESTS.md](TESTS.md)) : **0 pour jouer** |
| `[debug] steam_loopback` | 0 | relais « Steam » sur UDP local (tests à deux instances) |
