# S'inspirer du multijoueur de Project Zomboid

Ce document compare la réplication de Project Zomboid (PZ) à celle de KenshiCoop, puis propose des
améliorations classées pour KenshiCoop. Le fonctionnement actuel du mod est décrit dans
[ARCHITECTURE.md](ARCHITECTURE.md), l'état des fonctionnalités dans
[FONCTIONNALITES.md](FONCTIONNALITES.md) et les bugs ouverts dans [A_FAIRE.md](A_FAIRE.md).

## Sources et limites

- **Version étudiée** : PZ Build 42 (`projectzomboid.jar` du 9 octobre 2026, `RakNet64.dll`), dans
  le dossier Steam du jeu.
- **Méthode** : lecture seule, sans décompilateur et sans lancer le jeu. On a utilisé :
  - les noms de classes du jar ;
  - les chaînes constantes de chaque classe (messages d'erreur, noms de champs et de méthodes) ;
  - les annotations `@PacketSetting` de chaque paquet (priorité, fiabilité, canal d'ordre,
    permission requise, anti-triche) ;
  - le Lua de `media/lua` (`client/TimedActions`, `server/ClientCommands.lua`,
    `server/TransactionProcessor.lua`).
- **Ce qu'on en tire** : des idées et une architecture, **jamais du code**. PZ est propriétaire.
  Les phrases « déduit » signalent ce qui vient des noms et pas d'une lecture du comportement.

---

## 1. L'architecture de PZ en bref

**Serveur autoritaire, clients qui simulent beaucoup.** Le serveur dédié (ou l'hôte, en coop) a
le dernier mot sur le monde, les objets et la santé. Mais chaque client fait tourner une vraie
simulation locale, et le serveur lui délègue une partie du travail :
- le client simule **son propre joueur** (position, animations) et l'envoie ; le serveur le
  vérifie (vitesse, passe-muraille, distance des coups) ;
- les **zombies** proches d'un joueur sont « autorisés » à ce client (`NetworkZombieManager`,
  `zombiesAuth`, `setOwner`). Il les simule et renvoie leur état. Le serveur réattribue le
  propriétaire quand les joueurs bougent (option `SwitchZombiesOwnershipEachUpdate`) ;
- un **véhicule** a une autorité qui change de main (`BaseVehicle$Authorization` : `Server`,
  `Local`, `Remote`, `LocalCollide`, `RemoteCollide`). En général le conducteur simule la
  physique ; une collision donne une autorité temporaire ; l'autorité revient au serveur après
  un délai (`netPlayerTimeout`).

**Un paquet est une classe déclarée.** Environ 240 classes de paquets, chacune annotée avec :
- sa priorité RakNet (immédiate, haute, moyenne, basse) ;
- sa fiabilité (non fiable, séquencée, fiable, fiable ordonnée) ;
- son **canal d'ordre** : général, objets, carte/morceaux, joueur, animaux, véhicules, plus un
  canal par catégorie de données du joueur (blessures, dégâts, XP, stats, effets, santé) ;
- la **permission requise** de l'expéditeur (`requiredCapability`, ex. `TeleportToCoordinates`) ;
- **qui le traite** (`HandlingType` : serveur, client, client en chargement) ;
- une **liste de contrôles anti-triche** à passer avant traitement. Par exemple
  `PlayerHitPlayerPacket` passe `HitDamage`, `HitLongDistance`, `HitWeapon` et `Safety`.

Chaque paquet a aussi une méthode `isConsistent`. Sur un échec : « The packet %s is not
consistent », le paquet est ignoré et compté.

**Les objets passent par des transactions.** Tout déplacement d'objet
(`ItemTransactionPacket`, `zombie.core.Transaction`, `TransactionManager`) suit le même cycle :
1. **Request** : le client demande avec l'identifiant unique de l'objet, le contenant source et
   le contenant destination (`ContainerID` typé : inventaire du joueur, sac, objet du monde, sol,
   cadavre, véhicule).
2. Le serveur vérifie que :
   - la source contient bien cet objet (« source container is not contain the item ») ;
   - la destination existe et accepte l'objet (« destination container can't contain the item ») ;
   - le joueur a le droit d'y toucher (règles des refuges) ;
   - la place reste suffisante en comptant les transactions déjà en cours
     (`destinationWeightInTransaction`).
3. **Accept** : le serveur fixe la durée de l'action, en heure serveur. Ou bien **Reject**.
4. **Done** : le serveur a déplacé l'objet ; le client applique alors le résultat.

Le client **n'applique rien avant Done**. Dans `ISInventoryTransferAction`, la durée de l'action
vaut -1 jusqu'à la réponse du serveur. L'action se termine sur `isItemTransactionDone` et s'annule
sur `isItemTransactionRejected`. Les objets contestés sont retirés de l'inventaire du client par
`RemoveContestedItemsFromInventory`. Les actions longues (manger, construire, pêcher) suivent le
même cycle (`NetTimedAction`, `ActionManager` : Request, Accept, Reject, Done). Tous ces paquets
partagent le canal d'ordre « objets », en fiable ordonné.

**Zone d'intérêt par connexion.** Chaque connexion a :
- une zone chargée (`LoadedAreas`, `ClientServerMap`, largeur de grille de morceaux du client) ;
- une portée (`relevantRange`).

Le serveur envoie à « ceux pour qui c'est pertinent » (`sendToRelative`, `isRelevantTo`). Un
morceau demandé hors de cette zone est refusé (« Chunk request %d,%d stayed outside the streamed
area »). Quand il y a trop de zombies, `ZombieCountOptimiser` supprime ceux que personne ne verra
disparaître.

**Contrôle par empreintes.** On trouve des empreintes à trois niveaux :
- à la connexion, une empreinte MD5 des fichiers Lua et des scripts, par groupes puis par
  fichiers (`NetChecksum`, `ChecksumPacket`) ;
- un CRC32 par morceau de carte (`ChunkChecksum`) : le client n'a pas à retélécharger un morceau
  qu'il a déjà (`NotRequiredInZip`) ;
- une **empreinte de la liste des zombies** que connaît chaque client (`getZombieListHash`,
  `hashChanged`). Si elle diffère, le serveur renvoie la liste (`ZombieList`), et un client qui
  voit un zombie inconnu le demande (`ZombieRequest`).

**Horloge.** Le client estime le décalage avec l'horloge du serveur par aller-retour
(`TimeSyncPacket` : heure client, heure serveur, `serverTimeShift`). Le serveur diffuse l'heure du
jeu à intervalle fixe (`SyncClockPacket`, `SYNC_CLOCK_MS`). Il n'y a **pas de vitesse x2 ou x3
réglable** en multijoueur. La seule accélération est l'avance rapide quand **tous** les joueurs
dorment (`allPlayersAsleep`, `fastForwardMultiplier`). La pause n'existe que serveur vide
(`PauseEmpty`) ou pendant une sauvegarde (`StartPause`/`StopPause`).

**Garde-fous serveur.**
- **Contrôles par paquet** : `AntiCheatPlayer` refuse qu'une connexion mette à jour un joueur qui
  n'est pas à elle (« connection=[%s] tries to update not belonging player id=%d belonging to
  connection=[%s] »). `AntiCheatTarget` refuse une cible invalide, `AntiCheatSpeed` une vitesse
  au-dessus de `SpeedLimit`, `AntiCheatNoClip` un passage à travers un mur.
- **Réaction graduée** : `SuspiciousActivity` tient des compteurs par type de faute, qui
  redescendent avec le temps. Selon la politique, la faute est journalisée, le joueur est expulsé
  ou banni (`Policy` : `Log`, `Kick`, `Ban`).
- **Limites de débit** : un nombre maximal de paquets par seconde et par type
  (`MaxPacketsPerSecond`, « Packets limit has exceeded for %s »), et un ping maximal
  (`PingManager`).

---

## 2. Tableau comparatif

| Thème | Project Zomboid (B42) | KenshiCoop aujourd'hui |
|---|---|---|
| Autorité générale | Serveur autoritaire, mais clients qui simulent leur joueur, les zombies proches et le véhicule conduit | Hôte seul à simuler ; le client n'a ni IA ni dégâts, il affiche et envoie des ordres |
| Transport | RakNet : priorité, fiabilité et canal d'ordre déclarés par type de paquet | ENet, 3 canaux (0 fiable ordonné pour presque tout, 1 et 2 non fiables séquencés), Steam P2P en dessous |
| Catalogue des messages | ~240 classes, chacune avec son réglage réseau, sa permission et ses contrôles | ~50 messages dans `protocol.h`, canal vérifié à la réception, contrôles écrits au cas par cas dans `Session` |
| Propriété des entités | Propriétaire par zombie (client le plus proche, réattribué), autorité par véhicule (`Server`/`Local`/`Remote`), joueur rattaché à sa connexion | Champ `owner` dans `Bind` (0 PNJ, 1 hôte, 2 à 8 clients) ; seul le propriétaire commande ; vérifié dans chaque gestionnaire |
| Contrôle d'appartenance | Générique, au moment du dispatch, journalisé et compté (`AntiCheatPlayer`, `AntiCheatTarget`) | Par message (ordres, `InvOp`, réponses, apparence) ; hooks côté hôte sur la sélection ; bugs de sélection mixte (fix G5) |
| Mouvement du joueur local | Simulé par son client, envoyé avec une prédiction (type, temps restant) ; le serveur vérifie vitesse et collisions | Simulé par l'hôte ; le client envoie un ordre et voit son perso bouger avec ~1 aller-retour de retard (pas de prédiction) |
| Entités distantes | Interpolation ou extrapolation (`NetworkCharacter` : `interpolate`, `extrapolate`), distance de lerp maximale | Interpolation à 50 ms en arrière, 1 s d'historique, pas d'extrapolation ; rappel de 25 à 50 % par image ; saut au-delà de `snap_distance` |
| Objets | Identifiant unique par objet ; transaction Request, Accept/Reject, Done ; le client attend Done ; réservations de place ; canal d'ordre dédié | Pas d'identifiant d'objet (type + case) ; le client déplace localement, déduit des `InvOp` d'un diff toutes les 0,2 s, attend 3 s (10 s à la souris) puis revient à l'état de l'hôte |
| Échanges entre joueurs ou marchand | Fenêtre d'échange gérée par le serveur (`RequestTrading`, `TradingUIAddItem/RemoveItem/UpdateState`) ; transferts bloqués pendant un échange | Fenêtre du jeu ouverte chez le client ; achat/vente = `InvOp` avec prix ; refus = retour à l'état de l'hôte ; marchands ambulants non gérés |
| Actions longues | `NetTimedAction` : le serveur accepte et fixe la durée, le client anime puis attend Done | `Command` avec `seq` mais sans réponse ; succès ou échec visible seulement dans le journal de l'hôte ou par l'état suivant |
| Zone d'intérêt | Zone chargée et portée par connexion, envoi « aux connexions concernées », morceaux hors zone refusés, élagage des zombies | `interest_radius` (0 par défaut : tous les persos actifs), hystérésis ×1,25, cadavres < 1000, portes < 400, effets < 9000 |
| Cadences | Limiteurs par catégorie (santé, dégâts, stats, XP), paquets fiables/non fiables séparés par entité, mise à jour en plus sur événement | Cadences fixes par type (20 Hz, 5 Hz, 15 Hz…), « ce qui change » plus un envoi complet périodique ; pas de variation selon la distance (sauf `AnimFrame` < 1500) |
| Compression | Champs compacts (`IDShort`, positions en float), cache de paquets identiques (`PacketsCache`, `isHashEquals`) | Varints, quaternions sur 32 bits, envoi de ce qui change, empreintes d'inventaire ; pas de delta par rapport à un état acquitté |
| Débit et sécurité | Paquets par seconde par type, ping maximal, compteurs de fautes | Taille max des paquets, chaînes bornées, fuzzing des décodeurs ; pas de limite de débit par client |
| Arrivée et monde | Carte diffusée par morceaux à la demande, CRC32 par morceau, file de connexion, profil chargé du serveur | Sauvegarde complète de l'hôte (pause), envoyée par morceaux de 16 Ko, chargée par le client, empreinte de l'escouade |
| Reconnexion | Joueur sauvegardé côté serveur, reprise de son personnage | Personnage par compte Steam (`KenshiCoop-players.txt`), remplacement de l'ancienne connexion, fil de maintien pendant un gel |
| Heure et vitesse | Décalage d'horloge mesuré ; heure du jeu diffusée ; pas de vitesse réglable ; avance rapide si tous dorment | Vitesse et pause de l'hôte imposées ; heure non écrite (deux horloges qui partent ensemble) ; écart < 0,01 h en 15 min à ×3 |
| Détection de désynchro | Empreintes des fichiers, des morceaux et des listes de zombies ; demande d'entité inconnue | Rapport du client toutes les 5 s (manquants, décalés, correction max) ; `compare` dans les tests ; resync = rechargement complet |
| Correction de désynchro | Liste renvoyée, entité redemandée, objets contestés retirés | État réécrit au message suivant ; réimpositions périodiques (santé 0,25 s, portes 3 s…) ; resync complet manuel |
| Outils | Statistiques réseau (`StatisticManager`), dissecteur Wireshark, débogage MP du popman, journaux par type | Console hors du jeu, journaux des clients chez l'hôte, rapports de synchro, harnais `coop_test.py` |

---

## 3. Améliorations proposées pour KenshiCoop

Classement : gain sur les bugs connus d'abord, puis rapport gain / effort. Effort : **S** (≤ 1
jour), **M** (2 à 4 jours), **L** (une semaine ou plus). Presque toutes **changent le protocole** :
d'après la règle du projet, chaque montée de `kProtocolVersion` attend l'accord de l'utilisateur.

### 1. Transactions d'objets avec identifiant d'objet (le modèle Request, Accept/Reject, Done)

**Problème.** Aujourd'hui :
- le client laisse son jeu déplacer l'objet ;
- il **devine** les `InvOp` en comparant son inventaire à la dernière version de l'hôte toutes
  les 0,2 s (« disparu ici, apparu là, même type ») ;
- il revient à l'état de l'hôte après 3 s, ou 10 s pour un objet tenu à la souris.

Ce modèle explique toute une famille de bugs :
- la duplication puis l'annulation pendant la fouille d'un cadavre : un échange de bottes est un
  cycle qu'aucun ordre d'envoi ne résout. Il a fallu `SwapInventoryItems` et des règles de tri ;
- le « rebond » des piles ;
- les refus « refused an inventory move » sans contexte ;
- la fenêtre de commerce qui doit se fermer et se rouvrir quand le stock change.

Sans identifiant d'objet, l'hôte ne peut pas dire « cet objet-là n'est plus à cet endroit ». Il
ne peut que comparer des types.

**Conception.**
1. **Identifiant d'objet** : l'hôte donne un `itemId` (u32) à chaque objet qu'il réplique (dans
   `ItemState`). Il garde la table `itemId` → pointeur d'objet, et la vide au `ResetWorldBound`.
   Le client garde de son côté `itemId` → objet local, rempli à chaque `Inventory`.
2. **Nouveau message `ItemTx`** (C→H), envoyé tout de suite par les hooks de l'interface
   d'inventaire et de commerce, et non plus par un diff :
   - `txId` (u16 qui tourne, par joueur) ;
   - la liste des `(itemId, source netId, destination netId, section, x, y, quantité)` ;
   - pour un achat ou une vente : `traderNetId` et `price`.
   **L'échange de bottes devient une seule transaction à deux lignes**, appliquée par l'hôte en
   une fois ou refusée en entier.
3. **Réponse `ItemTxResult`** (H→C) : `txId`, `Accept` ou `Reject`, plus un code de raison (pas
   ton perso, objet introuvable, plus de place, pas assez d'argent, vol repéré…). Elle part sur
   le canal fiable, suivie des `Inventory` concernés.
4. **Chez le client** : l'objet reste « en attente » (grisé, ou bloqué par le hook) jusqu'à la
   réponse. En cas de `Reject`, l'inventaire de l'hôte est réappliqué aussitôt et la raison
   s'affiche dans un toast. Plus de délai de 3 s ou de 10 s.
5. **Chez l'hôte** : toutes les transactions d'un même tick sont traitées dans l'ordre. Une table
   des réservations (place promise à une transaction pas encore finie) remplace le tri « celui
   qui libère d'abord ».
6. **Le diff de 0,2 s reste** comme filet de sécurité. Il ne produit plus de `InvOp` : il note
   seulement l'écart au journal (« local change outside a transaction ») puis réapplique l'état
   de l'hôte.

**Effort** : L. Il faut toucher les hooks de l'interface d'inventaire et de commerce, le protocole,
`HostInvOp`, `ClientInventoryDiff` et les tests `TestInventorySwaps`.

**Risque** : moyen.
- Il faut trouver dans Kenshi les fonctions d'interface où intercepter le glisser-déposer (on a
  aujourd'hui le diff justement parce que ce point manque).
- Si une interception manque, le filet de sécurité garde le comportement actuel.
- À faire par étapes : d'abord `itemId` et `ItemTxResult` sur le chemin actuel, puis les hooks.

### 2. Contrôle d'autorité centralisé, au moment du dispatch

**Problème.** Chaque gestionnaire vérifie lui-même ses droits (ordres, `InvOp`, réponses,
apparence, portes, bâtiments). Les failles se trouvent aux endroits oubliés :
- le perso de l'hôte qui attaque avec l'ami, et l'hôte qui ne peut plus bouger le sien (sélection
  mixte, fix G5) ;
- les tests `lootswap` et `groundpick` qui donnaient des ordres au `squad0`, le perso de l'hôte ;
- un ordre jeté sans bruit (« local order dropped », ajouté après coup) ;
- le panneau Tâches, qui appelait `removePermajob` directement.

PZ déclare la règle **avec le type de paquet** (permission requise, liste de contrôles) et
l'applique au même endroit pour tous les paquets.

**Conception.**
1. Une table `kMessageRules` à côté de `protocol.h`. Pour chaque message C→H :
   - le canal ;
   - le rôle requis (joueur en jeu, hôte seulement, en arrivée) ;
   - les **sujets** à contrôler, désignés par extracteurs : « netId d'un perso que l'expéditeur
     possède », « contenant que l'expéditeur a ouvert (`openBy`) », « conversation de
     l'expéditeur »…
2. `Session` appelle `Authorize(peer, msg)` **avant** le gestionnaire. Un refus :
   - écrit une ligne unique au format `auth: [nom] <message> refused: <règle> (sujet X appartient
     à Y)` ;
   - incrémente un compteur par joueur et par règle (à la manière de `SuspiciousActivity`, mais
     sans expulsion : on est entre amis), affiché dans la colonne Synchro de la console ;
   - renvoie une réponse de refus quand le message en attend une (voir 1 et 3).
3. **Côté client, la même table filtre avant l'envoi.** Un ordre sur un perso qui n'est pas à
   nous est refusé avec un toast, au lieu d'être jeté sans bruit.
4. **Côté hôte, la sélection** : une seule fonction `OwnedSelection(player)` sert à tous les
   hooks d'ordre. Elle retire les persos des autres et recalcule le perso « principal ». C'est le
   fix G5, généralisé et écrit une fois.
5. Les tests unitaires parcourent la table : chaque message C→H reçoit un sujet qui n'est pas à
   l'expéditeur et doit être refusé.

**Effort** : M.

**Risque** : faible. Ce sont surtout des déplacements de code. Le danger est de refuser un cas
légitime aujourd'hui accepté (ex. déposer sur un corps à terre). Les compteurs le montrent vite
en partie réelle.

### 3. Réponse explicite à chaque ordre et à chaque demande

**Problème.** `Command` a un `seq`, mais aucune réponse ne revient. Le client ne sait pas si son
ordre a été exécuté :
- « pick up … -> FAILED » n'apparaît que dans le journal de l'hôte ;
- le panneau Tâches garde une copie locale 5 s en espérant que l'hôte suive ;
- le ramassage `groundpick` échouait en silence ;
- une porte, un achat de bâtiment ou un contenant refusé ne disent rien au joueur.

PZ termine chaque action par Accept/Reject, puis Done (`NetTimedAction`, `GeneralActionPacket`
avec `setReject`).

**Conception.**
- Un message générique `Result` (H→C) avec :
  - `kind` (Command, DoorRequest, BuildPlace, BuildAction, ContainerOpen, DialogReply…) ;
  - `seq` : on réutilise `Command.seq` et on ajoute un `seq` aux autres demandes ;
  - un état : `Accepted`, `Rejected` ou `Done` ;
  - un code de raison et un court texte.
- Le client garde une table des demandes en cours, avec un délai par type. Son overlay affiche
  les refus.
- Les prédictions locales (tâche ajoutée au panneau, chantier fantôme) sont retirées dès le
  `Rejected` au lieu d'attendre l'état suivant.
- Le harnais de test lit ces réponses au lieu de fouiller le journal de l'hôte.

**Effort** : M.

**Risque** : faible. C'est un ajout ; les clients sans réponse se comportent comme aujourd'hui.

### 4. Empreintes de zone et resync ciblée

**Problème.** Une désynchro se voit aujourd'hui de trois façons :
- les rapports du client (manquants, décalés, `max offset`) ;
- les tests ;
- les amis qui la signalent.

Pour corriger, il n'y a que le **resync complet** (rechargement de la sauvegarde). Il est lent,
il a fait planter un client, et il oblige à quitter puis rejoindre. Exemples : un PNJ invisible
chez 2 clients sur 3, les « 31 en permanence », un corps couché à 87 unités de celui de l'hôte,
des bâtiments en « bâtons rouges ». PZ compare des **empreintes** (liste des zombies connus, CRC
par morceau) et ne renvoie que ce qui diffère.

**Conception.**
1. On découpe le monde en cellules fixes, par exemple 1000 × 1000 unités, autour des joueurs.
2. Pour chaque cellule, l'hôte et le client calculent une empreinte (FNV-1a 64 bits) de valeurs
   **quantifiées**, pour ignorer le bruit :
   - les netIds présents, triés ;
   - la position arrondie à 4 unités ;
   - l'état debout, à terre ou mort ;
   - les drapeaux de captivité ;
   - l'empreinte de l'inventaire déjà calculée (`Inventory`) ;
   - l'état des portes et des bâtiments suivis.
3. Le client envoie toutes les 2 s `ZoneHashes` : la liste des `(cellule, empreinte)` près de
   ses persos.
4. L'hôte compare. Une cellule différente **deux fois de suite** déclenche une resync de cette
   cellule seulement :
   - `Bind` de toutes ses entités, y compris celles que le client devrait déjà avoir ;
   - un `Snapshot` complet sur le canal fiable ;
   - `Vitals`, `Inventory`, `Captives`, `Doors` et `BuildState` de la cellule.
5. Si l'écart persiste après trois resyncs de cellule, l'hôte journalise « zone X still differs
   for [nom] » et propose le resync complet (bouton existant).
6. Un niveau plus fin, en option : le client envoie l'empreinte de chaque entité d'une cellule
   fautive, pour que l'hôte sache **quelle** entité diffère et l'écrive au journal.

**Effort** : M à L. Le plus délicat est de choisir ce qu'on hache sans compter le bruit (ragdolls,
positions lointaines que le jeu du client ignore, au-delà de 300 unités).

**Risque** : moyen.
- Des faux positifs mèneraient à des resyncs de cellule en boucle. Le hachage quantifié, la règle
  des deux fois de suite et un plafond par minute limitent ce risque.
- Le coût CPU est faible : quelques centaines d'entités, toutes les 2 s.

### 5. Demande d'entité inconnue et cycle de vie des remplaçants

**Problème.** Un client qui reçoit un `Snapshot` pour un netId sans alias local l'ignore, puis
attend que `Spawn` ou `Reconcile` le règle. Bugs liés :
- l'alias mort qui bloquait la recréation (« 31 en permanence ») ;
- la coupure du tick qui effaçait les alias ;
- les doublures oubliées après l'image « hors jeu » du commerce, avec le marchand supprimé et la
  fenêtre fermée.

Ce sont tous des erreurs d'**état implicite** des remplaçants. PZ traite ce cas en deux temps :
le client demande ce qu'il ne connaît pas (`ZombieRequest`), et le serveur renvoie la liste si
l'empreinte change.

**Conception.**
1. Chaque alias a un **état explicite** : `Pending` (demandé), `Spawning`, `Adopted` (inconnu local
   adopté), `Live`, `Lost` (objet local disparu), `Released`. Chaque changement d'état est
   journalisé une fois, avec sa cause.
2. Un netId inconnu ou `Lost` vu dans un `Snapshot` déclenche `EntityRequest` (C→H, limité à N
   par seconde). L'hôte répond par le `Bind` complet et les états fiables de l'entité.
3. L'empreinte des netIds connus du client remonte avec son rapport de 5 s. Si elle diffère de
   celle de l'hôte pour la zone d'intérêt, l'hôte renvoie la liste des netIds attendus, comme
   `ZombieList`.
4. Plus d'effacement global des alias, sauf vrai nouveau monde.

**Effort** : M.

**Risque** : faible à moyen. Il faut un plafond, sinon une zone qui charge déclenche une rafale de
demandes.

### 6. Instantanés séquencés avec acquittement et deltas

**Problème.** Les envois « ce qui change, plus tout toutes les N s » demandent une période
complète par type : 1 s pour les positions, 2 s pour `Vitals`, 5 à 15 s pour les portes, les
factions et les captifs. Un paquet non fiable perdu laisse donc un écart jusqu'au prochain envoi
complet. Ces envois complets coûtent de la bande passante pour rien quand tout va bien, et le
plafond de 1100 octets par paquet est vite atteint avec 4 joueurs.

**Conception.**
1. Chaque `Snapshot` porte déjà `tick`. Le client renvoie `SnapAck(tick)`, au plus toutes les
   100 ms (non fiable).
2. Pour chaque client, l'hôte encode le delta par rapport au **dernier état acquitté** de chaque
   entité : bits de champs présents, puis seulement les champs changés depuis cette base. Une
   entité sans base acquittée part en entier.
3. Les états fiables (portes, captifs, bâtiments, factions) reçoivent un **numéro de version par
   entité**. Le client acquitte les versions reçues. L'hôte renvoie seulement ce qui n'est pas
   acquitté, ce qui remplace les envois complets périodiques.
4. La réimposition côté client (santé toutes les 0,25 s, portes toutes les 3 s) reste. C'est elle
   qui lutte contre la simulation locale, pas contre le réseau.

**Effort** : L. Il faut changer l'encodeur, l'historique par client et les tests de fuzzing.

**Risque** : moyen. Un bug de base produit des états faux et durables. Il faut un envoi complet
de secours si aucun acquittement n'arrive depuis 2 s, et une empreinte de contrôle dans chaque
paquet.

### 7. Cadences selon la priorité et la distance

**Problème.**
- Avec `interest_radius = 0`, l'hôte réplique **chaque** personnage actif à 20 Hz, où qu'il soit.
- Les PNJ lointains sont de toute façon mal tenus par le jeu du client (plus de 300 unités : il
  ignore les positions écrites).
- Le relevé `farlong` montre des écarts sur des PNJ à plus de 1000 unités de tout joueur.

PZ ne sert que les connexions concernées et élague ce que personne ne voit.

**Conception.**
1. Une priorité par entité et par client, recalculée toutes les 0,5 s avec le calcul d'intérêt
   existant :
   - **A**, 20 Hz : persos des joueurs, combattants et cibles à moins de 300 unités d'un membre de
     l'escouade de ce client ;
   - **B**, 5 Hz : moins de 1500 unités ;
   - **C**, 1 Hz : au-delà, ou seulement la destination et l'état debout/à terre.
2. Un événement (chute, mort, début de combat, changement de destination) force un envoi
   immédiat, quel que soit le rang.
3. Un **budget d'octets par client et par tick** : les entités sont triées par « priorité × temps
   depuis le dernier envoi » et on remplit jusqu'au budget. C'est l'accumulateur classique : rien
   n'est jamais affamé.
4. `AnimFrame`, `Vitals` et les portes suivent le même rang.

**Effort** : M.

**Risque** : faible. Le réglage par défaut peut reproduire le comportement actuel (tout en A),
puis être resserré après les tests `farlong` et `four`.

### 8. Canaux ENet par domaine

**Problème.** Presque tout passe par le canal 0, fiable ordonné. Un gros message retarde tout ce
qui suit, par blocage en tête de file : morceaux de 16 Ko du monde, inventaire complet d'un
marchand, `Bind` en rafale à l'arrivée d'un joueur. Un ordre ou une réponse d'objet attend donc
derrière une liste de portes. PZ sépare l'ordre par domaine (général, objets, carte, joueur,
véhicules, et un canal par catégorie de stats du joueur).

**Conception.** On passe d'environ 3 à 8 canaux (ENet en permet jusqu'à 255) :
- 0 contrôle et session ;
- 1 ordres et réponses ;
- 2 objets et transactions ;
- 3 transfert du monde ;
- 4 états fiables du monde (portes, bâtiments, captifs, factions) ;
- 5 dialogues et chat ;
- 6 et 7 non fiables (instantanés, santé).

La règle « chaque message n'est accepté que sur son canal » reste.

**Effort** : S.

**Risque** : faible. Seul cas à surveiller : deux messages qui dépendent l'un de l'autre et
changeraient de canal. `Bind` doit rester avant `Inventory` d'une même entité, donc on garde ces
deux-là sur le même canal ou on met en attente côté client.

### 9. Horloge : mesure du décalage et contrôle de dérive

**Problème.** Le client rend l'état à `heure de l'hôte - 50 ms`, avec l'heure de l'hôte telle
qu'elle arrive, sans estimer le décalage ni la gigue. Il y a aussi deux horloges de jeu qui
« restent ensemble parce qu'elles partent ensemble » (< 0,01 h d'écart en 15 min à ×3). Rien ne
mesure cet écart sur une partie de plusieurs heures. Les horloges d'animation corrigées à ×3 (4 %
pendant un combat) en sont peut-être un symptôme.

**Conception.**
- Le `Ping`/`Pong` existant sert à estimer `offset = hostTime - (t_envoi + rtt/2)`, lissé sur une
  fenêtre (comme `TimeSyncPacket`).
- Le délai d'interpolation devient **adaptatif** : environ 2 intervalles d'instantané plus la
  gigue mesurée, au lieu de 50 ms fixes.
- `TimeState` porte l'heure du jeu de l'hôte. Le client compare toutes les 10 s et journalise
  l'écart. Au-delà d'un seuil (ex. 0,05 h), il le corrige en douceur.
- Côté vitesse, l'idée de PZ est l'avance rapide seulement par consensus (tous les joueurs
  dorment). Une option `[coop] speed_vote` pourrait n'accepter ×2 ou ×3 que si aucun client ne
  s'y oppose (ex. un client en combat ou en commerce).

**Effort** : S pour la mesure et le journal, M pour le délai adaptatif.

**Risque** : faible.

### 10. Garde-fous de débit et de taille par client

**Problème.** L'hôte accepte tout ce qu'un client envoie, dans la limite de taille des paquets.
Un client bogué en boucle peut figer l'hôte, et le mal vient alors de partout. Exemples : une
rafale de `ClientLog`, des `ContainerOpen` répétés, des `InvOp` en boucle sur un rebond.

**Conception.**
- Un seau à jetons par client et par type de message C→H (ex. 20 ordres par seconde, 50
  transactions par seconde, 64 lignes de journal par envoi, déjà en place).
- Un dépassement est jeté, compté et journalisé une fois par minute (« [nom] sends too many X »),
  comme `PacketsCache` de PZ.

**Effort** : S.

**Risque** : très faible, à condition de choisir des plafonds larges.

### 11. Transfert du monde incrémental (arrivée et resync plus rapides)

**Problème.** Une arrivée ou un resync envoie **toute** la sauvegarde, alors que le client en a
souvent une version très proche (resync, retour après un plantage). PZ ne renvoie que les
morceaux dont le CRC diffère.

**Conception.**
1. `WorldBegin` liste les fichiers avec leur taille et leur empreinte (SHA-256 ou CRC32).
2. Le client répond avec la liste de ceux qu'il a déjà, à l'identique, dans `KenshiCoopJoin`.
3. L'hôte n'envoie que les autres.

**Effort** : S à M.

**Risque** : faible ; l'empreinte finale (`WorldEnd`) reste le contrôle.

---

## 4. Ce qu'on ne reprend pas, et pourquoi

- **Laisser le client simuler son personnage** (prédiction du joueur local comme PZ). En théorie,
  cela supprimerait le retard des ordres. Mais Kenshi n'est pas déterministe entre deux machines,
  le pathfinding et l'IA de combat tournent chez l'hôte, et toute la stabilité actuelle vient de
  « l'hôte seul simule ». Une prédiction **visuelle** reste possible et peu risquée : montrer le
  marqueur de destination et faire tourner le perso tout de suite. Un mouvement prédit, non.
- **Déléguer des PNJ à un client** (comme les zombies de PZ). Ce serait utile pour un joueur parti
  seul à l'autre bout de la carte : l'hôte n'aurait plus à tout simuler. Mais cela suppose de
  réactiver l'IA chez le client, pour un sous-ensemble choisi, et de réconcilier ensuite. C'est
  l'inverse du modèle actuel, à garder pour plus tard si la charge de l'hôte devient le
  problème.
- **Anti-triche avec expulsion ou bannissement.** On joue entre amis : on garde les compteurs et
  les journaux, sans sanction automatique.

---

## 5. Annexe : repères dans PZ (B42)

| Sujet | Classes ou fichiers repérés |
|---|---|
| Catalogue et réglages des paquets | `zombie.network.PacketTypes` (`PacketType`, canaux `PacketOrdering_*`), annotation `PacketSetting` (`priority`, `reliability`, `ordering`, `requiredCapability`, `handlingType`, `anticheats`) |
| Transactions d'objets | `zombie.core.Transaction`, `TransactionManager`, `network.packets.ItemTransactionPacket`, `network.fields.ContainerID`, `RemoveContestedItemsFromInventoryPacket`, `client/TimedActions/ISInventoryTransferAction.lua`, `server/TransactionProcessor.lua` |
| Actions longues | `zombie.core.NetTimedAction`, `ActionManager`, `NetTimedActionPacket`, `GeneralActionPacket` |
| Propriété des zombies | `zombie.popman.NetworkZombieManager`, `NetworkZombieSimulator`, `NetworkZombiePacker`, `ZombieCountOptimiser`, `LoadedAreas` |
| Véhicules | `BaseVehicle$Authorization`, `network.fields.vehicle.VehicleAuthorization`, `VehiclePhysicsReliable/UnreliablePacket` |
| Mouvement, interpolation | `zombie.characters.NetworkCharacter` (`interpolate`, `extrapolate`), `network.fields.character.Prediction`, `network.constants.PredictionTypes`, `PlayerPacket` |
| Anti-triche | `zombie.network.anticheats.*` (`AntiCheatPlayer`, `AntiCheatTarget`, `AntiCheatSpeed`, `AntiCheatNoClip`, `SuspiciousActivity`, `PacketValidator`) |
| Empreintes | `NetChecksum`, `ChecksumPacket`, `ChunkChecksum`, `getZombieListHash` |
| Monde | `PlayerDownloadServer`, `ClientChunkRequest`, `ChunkNotReadyPacket`, `NotRequiredInZipPacket`, `RequestDataManager` |
| Heure | `TimeSyncPacket`, `SyncClockPacket`, `StartPausePacket`, `GameTime` (`serverTimeShift`, `fastForward`) |
| Débit | `PacketsCache` (`maxPacketsPerSecond`), `statistics.PingManager`, `NetworkPlayerManager` (limiteurs santé, dégâts, stats) |
