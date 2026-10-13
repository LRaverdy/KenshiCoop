# Tests

Deux niveaux de tests :
1. **Tests unitaires** : la session et le protocole, sans le jeu.
2. **Tests en jeu** : de vraies instances de Kenshi pilotées par un script, qui comparent ce que
   voient l'hôte et les clients.

## 1. Tests unitaires (`tests/test_main.cpp`)

```powershell
.\build.ps1                            # compile aussi kc_tests
.\build\bin\Release\kc_tests.exe       # dernière ligne : "54920 checks, 0 failed" (13 octobre 2026)
.\build\bin\Release\kc_tests.exe       # dernière ligne : "54905 checks, 0 failed" (13 octobre 2026)
.\build\bin\Release\kc_tests.exe       # dernière ligne : "54907 checks, 0 failed" (13 octobre 2026, branche fix-char-states)
.\build\bin\Release\kc_tests.exe       # dernière ligne : "54945 checks, 0 failed" (13 octobre 2026)
.\build\bin\Release\kc_tests.exe       # dernière ligne : "55030 checks, 0 failed" (13 octobre 2026, branche fix-nametags)
.\build.ps1 -Asan                      # variante AddressSanitizer, dans build-asan\
```

Ils font tourner de vraies sessions (vrai réseau en boucle locale) avec un faux monde
(`FakeWorld`) :

| Test | Ce qu'il vérifie |
|---|---|
| `TestWire` | encodage et décodage de chaque message, valeurs limites |
| `TestFuzz` | les décodeurs résistent à des données aléatoires ou tronquées |
| `TestJoinFromMenu` | rejoindre depuis le menu : transfert du monde, chargement, `Ready` |
| `TestOwnCharacter` | un personnage par joueur, retrouvé à la reconnexion |
| `TestWorldIdentity` | même monde après une coupure du tick (fenêtre, zone), nouveau monde après un chargement ; les doublures ne décident jamais ; celles restées d'un monde précédent sont nommées |
| `TestNoDuplicatePlayers` | un client déjà là garde exactement une copie de chaque joueur arrivé après lui, à travers des coupures du tick et un vrai chargement |
| `TestSessionReplication` | positions, santé, heure et pause répliquées |
| `TestDivergenceIsCorrected` | un client qui diverge est ramené à l'état de l'hôte |
| `TestRejections` | version, exe, mods, nom, partie pleine, exclusion |
| `TestWorldAuthority` | le client n'exécute rien lui-même, ses ordres passent par l'hôte |
| `TestSpawnReplication` | personnages créés chez l'hôte puis recréés chez le client ; un corps mort recréé (zone rechargée) revient mort et couché |
| `TestStreamingTools` | (0.3.1) délais de réessai, limites de journal, délais des doublures perdues, décisions de resync de zone |
| `TestGroupCentres` | centres de diffusion d'un joueur : ses persos et les membres de l'escouade près d'eux (l'araignée manquante de la suite 0.3.1) |
| `TestPerPlayerStreaming` | (0.3.1) deux clients loin l'un de l'autre : chacun ne reçoit que les persos autour des siens ; foule créée 2 par image ; doublure perdue sans cesse (0.3.4 : son retour dit dans le journal, « back here (made) ») ; (0.3.4) un PNJ en sommeil (plus envoyé) qui marchait reste figé à sa dernière position, sans destination, remis en place si le jeu le bouge (`HoldStill`, `DormantCopies`) ; resync automatique de la zone d'un client (`KC_ONLY_STREAMING=1` : ces tests seuls) |
| `TestStandInArea` | (0.3.1) hôte qui envoie tout : le client ne recrée ni ne compte manquant ce qui est loin de ses persos |
| `TestCaptives` | captivité (cage, menottes, esclavage, peine) imposée et levée, journal court ; (0.3.4) une copie au lit chez le client alors que l'hôte l'a libre en est sortie au rafraîchissement suivant (`EnsureFree`), un dormeur que l'hôte garde au lit n'est jamais touché |
| `TestInventories` | inventaires identiques, fouille rejouée par l'hôte |
| `TestLootCreatures` | (0.3.1) toutes les créatures se fouillent : types de handle (1 humain, 0x5B animal) et de modèle (1, 76) ; pour chacune des 38 races du jeu, à son échelle mini, moyenne et maxi, un pilleur au bord du corps est à portée dans 8 directions (la racine du ragdoll à mi-hauteur), un pilleur trop loin ne l'est pas, le point d'approche est au bord, l'ancienne règle (12 unités en 3D) échouait pour chaque animal plus large ; les créatures du rapport par race (léviathan, bête à bec, gorillo, chien d'os, garru, taureau, chèvre, chauve-souris, araignées, tortue, crabe, araignées de fer et de sécurité, skimmer, raptor, hommes-brume, cannibales…) ; valeurs aberrantes bornées ; délai d'un ordre ; échelle contre âge d'un animal |
| `TestBigBodyDrop` | (0.3.1) un client prend une perle dans un léviathan mort chez l'hôte ; un dépôt au sol depuis le léviathan par son perso à 210 unités de la racine est accepté, refusé si le corps avait une taille humaine |
| `TestClientDropGate` | dépôts au sol d'un client (partie du 10/10, objets disparus) : la session dit au monde depuis quels inventaires un dépôt peut partir (son perso par le handle de l'hôte, le sac qu'il porte par le handle local, un coffre ouvert avec son perso à côté, un corps avec son perso à côté ; pas le perso de l'hôte, pas un PNJ, pas un coffre fermé ou loin) ; dépôt depuis son sac fait par l'hôte ; dépôt nommé par un handle que l'hôte ne connaît pas (celui d'une doublure avant le correctif) : rien envoyé, note « Objet non posé », rien de perdu ; dépôt refusé tard (corps sans perso à côté) : l'état de l'hôte revient tout de suite, pas 10 s après |
| `TestGroundDrops` | objets lâchés au sol par un client : depuis son perso (l'hôte le lâche), depuis un coffre ouvert (lâché par son perso près du coffre, nommé dans `InvOp::toNetId`), depuis le perso de l'hôte (refusé) ; dépôts forgés (perso de l'hôte, aucun perso, son perso loin du coffre) refusés par le contrôle d'autorité |
| `TestFactions` | relations de la faction du joueur et primes de l'hôte identiques chez le client, changements suivis, dérive du client corrigée |
| `TestDiplomacy` | diplomatie : seules les paires de factions changées depuis le début voyagent ; guerre, chef tué, ville prise suivis par le client ; nouvelles en français identiques des deux côtés (guerre, mort, ville, relation, prime) ; dérive du client corrigée ; le bruit des relations n'est pas renvoyé |
| `TestAnimalShop` | (A_FAIRE 3i, protocole 36) aller-retour d'`AnimalShop` et `AnimalBuy`, acheteur absent refusé ; offre du jeu pour le perso du client : fenêtre chez lui avec les 3 animaux ; il en achète un : vendu par l'hôte avec l'offre gardée, cats prises partout, l'animal est à lui et rejoint l'escouade de son perso, offre libérée ; réponse forgée (un animal que l'offre ne contient pas) refusée et offre libérée ; réponse au nom du perso de l'hôte refusée par le contrôle d'autorité ; vente impossible (argent) : le joueur reçoit la raison, rien ne change ; fenêtre fermée sans achat : offre libérée ; offre pour le perso de l'hôte jamais envoyée |
| `TestCrashRejoin` | client figé 7 s (gardé), puis relancé avec le même compte avant la coupure : remplacé, même nom, même perso, pas de doublon, fenêtre de commerce fermée, perso arrêté |
| `TestAdmin` | administration de l'hôte : syntaxe des commandes `admin`, calcul de l'XP du jeu (`increaseStat`, 20 au plus par appel, niveaux visés au plus juste), mode dieu retenu par joueur (même compte Steam = même joueur, « tout le monde » puis un seul retiré), message envoyé à un seul joueur et jamais par un client ; objets : syntaxe de `admin spawn` (500 au plus, cible here ou un joueur, fabricant et modèle), découpage en piles, répartition au sol (jamais sur le perso, 5 unités d'écart, 500 dans 12 m), recherche, catégories, favoris |
| `TestTaskTargets` | sécurité des acteurs : chaque numéro de tâche confronté à chaque sorte de cible (rien, soi, PNJ debout / à terre / mort, autre membre, objet, contenant, chantier à nous ou non, bâtiment fini, lit, cage, machine, porte, handle périmé), par les 4 façons de donner un ordre ; **toutes les tâches de l'interface** (construire, réparer, démanteler, secourir, utiliser, lits, cages, chaînes, serrures, manger, prospecter, mines et fermes, transporter, attaque du clic droit 5 + 16…) permises sur la bonne cible et nommées ; cibles dangereuses toujours refusées ; numéros inconnus refusés ; barre d'escouade et panneau Tâches ; table des règles des messages ; aller-retour de `Result` |
| `TestTalkAndRecruit` | partie du 10/10 : un PNJ en conversation avec un perso du client n'est jamais « occupé » pour ce client (autre perso : l'ancienne conversation se termine et se ferme ; même perso : renvoyée sur son écran ; conversation du jeu sans fenêtre : terminée avant l'ordre) mais l'est pour l'autre joueur ; « va parler » chez le client, une fois ; recrue dans la conversation du client : à lui tout de suite, ancien handle annoncé, adoptée chez les deux clients, éditeur chez lui seul (jamais chez l'hôte), sélectionnable chez lui |
| `TestCallScopeRepair` | plantage `0x883B78` : `RepairScopeCounts` (portées laissées ouvertes, fermées deux fois) ; une exception non C++ rattrapée par `__except` laisse en `/EHsc` la portée ouverte comptée, la réparation la remet ; tâche 27 sur un coéquipier et BUILD sur un PNJ, un coéquipier, rien ou un bâtiment fini refusés par les 4 voies, BUILD sur un chantier à nous accepté ; `TaskTargetWrongKind` (copie des jobs de l'hôte) : tâche connue avec une cible du mauvais type refusée, tâches inconnues non jugées |
| `TestDialogue` | conversations : 1 hôte + 2 clients ; la fenêtre d'un garde s'ouvre chez le bon client seulement, acteur = son perso, bulle chez tout le monde ; réponses forgées (autre joueur, perso de l'hôte, d'un autre, aucun) refusées ; la vraie réponse arrive une fois ; réponse à une réplique dépassée ou non proposée ignorée ; « parler » à un PNJ occupé refusé (`Busy`, « occupé ») pour l'autre joueur, accepté pour celui qui lui parle ; PNJ que le jeu fait parler à l'hôte : occupé ; « occupé » du jeu chez le bon joueur et chez l'hôte ; recrue donnée au joueur qui lui parlait malgré une autre réponse en même temps ; « partir » termine la conversation chez l'hôte ; départ d'un joueur : sa conversation terminée |
| `TestActorSafety` | sécurité des acteurs : 1 hôte + 2 clients ; ordres forgés (aller, arrêter, ramasser, lit, parler, mode, escouade) au nom du perso de l'hôte, d'un autre joueur, d'un PNJ, d'aucun, d'un inconnu : rien n'est exécuté, refus journalisé, `Result` rejeté (par `seq`) et message français ; idem pour regarder dans un contenant, apparence, dépôt d'objet, réponse à la conversation d'un autre ; l'ordre de son propre perso est exécuté (`Done`) ; le client refuse d'envoyer pour un perso qui n'est pas le sien ; un ordre envoyé quand même (`SendRawCommandForTest`) est refusé ; ordres de son perso visant une mauvaise cible (construire, machine, lit, porte, cage, ramasser, coffre sur un PNJ ; handle périmé ; sans cible ; parler à un membre de l'escouade) refusés, la bonne cible acceptée (premiers soins sur le perso de l'hôte compris) |
| `TestTalkRules` | (0.3.4, `kc/talk.h`) clic droit du client sans ordre du jeu : ignoré (survol, pas un perso, perso d'un joueur), seulement écrit (autre mode de l'interface, animal, à terre, aucun perso à soi sélectionné, dans un lit ou une cage ici), ordre « parler » envoyé quand même (PNJ debout) ; PNJ retrouvé par type et endroit à 300 unités au plus ; réplique gardée tant que le joueur a des réponses à choisir, 5 min au plus, jamais sans réponse ni hors d'une fenêtre de joueur ; l'ordre « parler » porte le type et la place du PNJ dans l'ordre existant (aller-retour, pas de changement de protocole) |
| `TestMap` | marqueurs de carte : 1 hôte + 2 clients reçoivent les persos (propriétaires, perso de chaque joueur, positions même très loin) et les escouades hostiles de l'hôte ; un ping de client chez l'hôte et l'autre client, refusé s'il suit le précédent de moins de 0,5 s, 5 au plus par joueur, effacé après 10 s |
| `TestManyPlayers` | 1 hôte + 4 clients arrivant ensemble, 120 personnages |
| `TestJoinQueue` | arrivées : 3 arrivées quasi simultanées (une pendant la sauvegarde, une pendant le chargement du premier) : tous dans le monde avant qu'un éditeur se ferme, au moins deux chargements en même temps, chaque sauvegarde contient les persos des précédents ; éditeur un joueur à la fois, positions / joueur dans l'éditeur côté clients, liste côté hôte, personne rejeté pendant l'attente ; apparence changée dans l'éditeur pendant qu'un autre charge : il la reçoit en arrivant ; le joueur dans l'éditeur plante (tour suivant), un joueur quitte la file (renumérotée) ; deux éditeurs ouverts ensemble : pause jusqu'à la fermeture du dernier |
| `TestJoinReturns` | un joueur parti avant de fermer l'éditeur (jeu quitté pendant le chargement, puis éditeur ouvert) le retrouve à chaque retour, sur le même perso, jusqu'à l'avoir fermé ; ensuite plus jamais ; un joueur revenu après un plantage retrouve ses deux persos (le sien et un donné par l'hôte, celui-ci rehandlé entre-temps) sous les mêmes netIds, comme siens chez lui, et son dépôt au sol part chez l'hôte |
| `TestWorldPacking` | compression du monde envoyé : aller-retour exact, taille annoncée fausse / flux tronqué / données quelconques refusés, données incompressibles envoyées telles quelles |
| `TestMachineInventoryClosed` | (0.3.4) une machine fermée chez le client : son jeu y déplace la viande, ou en « met » dans le perso du joueur : aucune demande ne part, l'hôte ne bouge rien, tout revient à l'état de l'hôte ; l'hôte oublie la machine (plus personne ne la travaille) : le client oublie sa copie (`Unbind`) ; reprise : une seule copie, au nouveau netId ; une demande au nom d'un inventaire que l'hôte ne suit pas : « not done (nothing to do) » au journal de l'hôte. Seul : `KC_ONLY_MACHINEINV=1` |
| `TestWorkshop` | atelier : recherche (connues, file, avancement) et machine de l'hôte identiques chez le client sans rien ouvrir (opérateurs, ordres de fabrication, inventaire de la machine, totaux d'énergie) ; machine loin des joueurs non envoyée ; un mineur qui part, vu partout ; mine devenue au joueur chez l'hôte : `kMachOurs` chez le client ; technologie demandée sans artefact refusée ; la même demandée deux fois d'un coup : une fois en file, payée une fois ; annulée puis redemandée sans repayer ; changement local du client défait ; plan porté par le perso du client appris chez l'hôte et consommé partout ; plan au nom du perso de l'hôte : jamais envoyé par le client, refusé par le contrôle central s'il est forgé ; ordre de fabrication du client exécuté et vu partout ; deux « répéter » d'un coup : le dernier l'emporte partout ; ordre déjà retiré refusé ; machine coupée par le client, coupée partout, panneau d'énergie suivi ; demande d'un perso trop loin refusée ; passage de niveau de l'hôte annoncé au client (« Science 5 -> 6 ») ; **ferme** : cultures de l'hôte identiques chez le client, un fermier au travail, une récolte, la replantation ; machine de l'hôte absente du monde du client comptée manquante (`machinesMissing`) |
| `TestWorkstationOrders` | postes (travaux de base) : pour chaque tâche de la ferme, de l'eau, de la cuisine, des mines, de l'artisanat, de la recherche, de l'énergie, du transport (87, 152, 221, 88-94, 113, 114, 151, 241, 247), du bâtisseur (2, 71, 95, 125), du médecin et du secours (58, 105, 148), des tourelles (146, 234) : bonne cible permise par les 4 façons de donner un ordre, mauvaise (PNJ, autre membre, objet, chantier, handle périmé…) refusée avec une raison ; panneau Tâches (retirer, déplacer) ; travaux de base donnés à une recrue « à personne » (`IsSettingsCommand`), pas les autres ordres ; bouton JOBS ; liste de tâches copiée : machine oui, PNJ jamais ; aller-retour des cultures d'une ferme dans `Machines` (protocole 36) |
| `TestClientJobs` | (v0.3.4) travaux des persos d'un client : messages de `WorkOrderNote` (déjà au travail, machine pleine en ponctuel et en permanent, JOBS éteint, déjà dans la liste, rien d'ajouté, ferme qui pousse, aucune machine : rien à dire) ; prédiction des boutons de la barre (`ModePredictions` : les deux clics de rob, refus annulé après 3 s, autres boutons et autres persos intacts) ; `OrderDebounce` (même ordre 1 fois par seconde, Maj ou autre perso : un autre ordre) ; remise en ordre d'une liste de tâches (`NextJobMove` / `ApplyJobMove` sur les 120 ordres de 5 travaux, travaux absents chez l'hôte gardés derrière) ; Maj transportée par `NewTask` ; en session : 6 clics identiques = 1 ordre chez l'hôte, Maj = un autre ordre, boutons et croix du panneau jamais retenus |
| `TestNameTags` | (v0.3.4, `plugin/map_view.h`) noms au-dessus des têtes : couleur du joueur sur le nom du jeu (`NameTagColour`, une par joueur, plus claire si sélectionné), couleur empaquetée bornée (`PackColour`) ; cadre du perso principal autour du texte centré dans la boîte du nom, nom plus long que la boîte, taille du texte illisible (toute la boîte) ; conversion des cadres de la vue MyGUI vers l'affichage (`SceneToDisplay`) ; qui écrit le nom (`WhoWritesName`) : le jeu seulement si ses noms sont affichés, le widget fait et le nom mis à jour par le jeu dans les 30 dernières images (quand le jeu en met d'autres à jour), sinon l'overlay (13/10 : nom jamais mis à jour du perso d'un joueur arrivé plus tard) |
| `TestSquadWire` | fenêtre Escouade : aller-retour de `SquadState`, `SquadRequest`, `JobState` ; demandes mal formées refusées (pas d'acteur, renommer sans escouade, nom vide, rang négatif) ; noms nettoyés (caractères de contrôle, 64 octets, lettre UTF-8 coupée) ; tâches 105 / 148 sans cible ; ce qu'est un réglage d'IA |
| `TestDeadSquad` | l'escouade des morts du jeu (`__DEAD_SQUAD__`) : l'hôte ne l'envoie pas, le client ne demande jamais la sienne, une création sous ce nom est refusée |
| `TestMotion` | (`kc/motion.h`) un marcheur que l'hôte ne bouge que 4 fois par seconde est rendu sans recul ni saut ; positions répétées fusionnées ; extrapolation courte, jamais au-delà de la destination ; direction du déplacement réel quand celle de l'hôte diverge, jamais une marche de dos (même rendu extrapolé au-delà du dernier échantillon) ; hôte en pause : son état le plus récent tel quel ; rappel jamais en arrière contre la marche et identique à 30 et 150 images/s ; posture : captifs jamais touchés, chute qui ne tient pas abandonnée (2 par minute au lieu d'une toutes les 2 s), 4 chutes et 2 relevages par minute, relevage au-delà de 20 unités ; corps posé après un portage relevé une fois au-delà de 5 unités (`MayFineRelay`) et mis debout à la hauteur du sol de son porteur quand l'hôte le lit plus de 4 unités au-dessus (`FallSpotHeight`, 13/10) ; cadavres : jamais relevés ni ranimés, doublure recréée vivante tuée et couchée ; (0.3.4) un marcheur de l'hôte qui n'avance pas (moins de 1,5 unité en 1 s) n'est pas « marché » par la copie, une vraie marche l'est dès son premier pas ; une copie à un autre étage que l'hôte (même place, plus de 20 unités d'écart en hauteur, groupe d'étage différent, numéro d'étage) est replacée, jamais un corps ni un perso tenu |
| `TestSquadFallback` | un hôte dont les escouades n'ont pas d'identifiant (`SquadState` sans escouade) : le client suit quand même l'ancien message `Squads` |
| `TestSquadWindow` | fenêtre Escouade, 1 hôte + 2 clients : mêmes escouades partout ; recrue « à personne » (pas l'avatar de l'hôte) ; renommer son escouade (fait), celle d'un autre (remis) ; nouvelle escouade du client créée chez l'hôte, son perso déposé dedans puis chef de la première ; escouade vide retirée partout ; déplacer le perso de l'hôte refusé (côté client et côté hôte) ; ordre des escouades ; renommages simultanés de l'hôte et du client (le dernier arrivé gagne, même nom partout) ; déplacements simultanés du même perso ; noms des persos (le sien passe, celui de l'hôte est remis) ; passif d'une recrue réglé par les deux clients (le dernier gagne), refusé sur l'avatar de l'hôte, ordre de déplacement sur la recrue refusé ; listes de tâches avec cibles ajoutées et réordonnées chez les deux clients |
| `TestMessageRules` | autorité : chaque valeur de `Msg` (les 256 octets passés à `MsgName`) a exactement une règle dans `kMessageRules`, et `PeekType` n'accepte que ces messages ; numéros de la fusion du protocole 33 (`JoinQueue` 72, `BagBind` 80, `MapMarkers` 82, `MapPing` 83, `Diplomacy` 85, `Result` 90) ; un client qui envoie `Diplomacy`, `MapMarkers`, `JoinQueue`, `BagBind` ou `Result` est refusé ; 5 pings d'affilée : 1 passe, 4 ignorés (`minInterval` 0,5 s), puis un autre passe |

## 2. Tests en jeu (`tools/coop_test.py`)

### ⚠ Avant de lancer
- **Le harnais ferme tous les Kenshi en cours au démarrage** (`kill_all`), et encore à la fin sauf
  avec `--keep`. **Ne jamais le lancer pendant que quelqu'un joue** : vérifier d'abord avec
  `tasklist /FI "IMAGENAME eq kenshi_x64.exe"`.
- Configuration de test, à remettre comme avant ensuite (voir plus bas) :
  - `KenshiCoop.ini` : `[debug] commands=1`, qui active le canal de commandes ;
  - `kenshi.cfg` **en mode fenêtré** (952×536 pour mettre les fenêtres côte à côte). La version
    plein écran du joueur est sauvegardée dans `kenshi.cfg.before-kenshicoop-test`.
- Mod compilé et installé (`.\build.ps1`, `.\install.ps1`).
- Sauvegardes de test, dans `%LOCALAPPDATA%\kenshi\save\` :
  - `kctest_base` : l'escouade dans le désert. C'est la sauvegarde par défaut, mais des pillards y
    passent parfois ;
  - `kctest_town` : une copie d'une partie du joueur dans la ville des voleurs Shinobi, avec des
    PNJ qui parlent, des lits et des coffres.
- Le chemin de Kenshi est en dur dans le script :
  `C:\Program Files (x86)\Steam\steamapps\common\Kenshi`.

### Fonctionnement
- **Poses et téléportations** : les expériences qui posent un bâtiment (`build`, `construct`,
  `farlong`, `soak`, `placevalid`) passent par `place_valid` : l'endroit voulu, puis des cercles
  autour (25 à 260 unités), jusqu'à ce que la vérification du jeu (`buildplace`) l'accepte. Les
  téléportations lointaines (`far`, `fartp`, `farlong`, `soak`) passent par `teleport_dry` : la
  première destination de la liste qui est de la terre ferme (`groundat` chez l'hôte, avant la TP
  si le terrain y est connu, sinon après), jamais un lac d'eau ou d'acide.
1. Le script lance les instances et ferme le petit lanceur de Kenshi s'il apparaît.
2. Il charge la sauvegarde chez l'hôte, héberge, puis fait rejoindre chaque client.
3. Il place les fenêtres côte à côte : hôte à gauche, client à droite ; en grille de 2×2 pour 4
   joueurs.
4. Chaque client reçoit un faux identifiant Steam : variable d'environnement `KC_FAKE_STEAM_ID`,
   `76561190000000002` puis +1 par client. Le mod ne la lit que si le canal de debug est actif.
   Sans elle, deux jeux sur un même PC partagent le même compte Steam.
   De même `KC_PLAYER_NAME` (debug seulement, jamais écrit dans l'ini) donne son nom de joueur à
   chaque jeu (`setup_multi` : Hote, Joueur2, Joueur3...).
5. Pour tester le relais Steam sur un seul PC :
   - mettre `[debug] steam_loopback=1` dans `KenshiCoop.ini` ;
   - lancer `KC_JOIN="steam:28100 27960" python tools/coop_test.py <expérience>` ;
   - remettre `steam_loopback=0` ensuite.
   Un vrai test entre deux comptes Steam demande deux PC.
6. Les états comparés et les journaux de chaque essai sont dans `test_out/` (ignoré par git).

### Expériences

Plusieurs joueurs sur un PC : avant chaque lancement le banc vérifie la RAM libre (arrêt propre avec un message si moins de 2 Go ; un hôte prend ~2,2 Go, un client ~0,7 Go). Sur toute erreur, les jeux lancés par le banc sont tués (aucun processus laissé derrière).

`python tools/coop_test.py <expérience> [--save X] [--keep]`

| Expérience | Ce qu'elle vérifie |
|---|---|
| `doors` (`kctest_town`, lot A) | portes ouvertes / fermées / verrouillées comme chez l'hôte ; le jeu du client ne les change pas seul ; bouton et ordres du client exécutés chez l'hôte ; crochetage ; coffre verrouillé fermé pour le client |
| `caravan` (sauvegarde avec une caravane près de l'escouade) | marchand ambulant : même caravane partout (pas de double chez le client), sac de la bête identique partout, fenêtre ouverte chez le client avec le stock, achat et vente pendant que la caravane marche (payés chez l'hôte, même argent et même stock partout), fenêtre fermée quand la bête est tuée, pillage de son sac (rien de créé ni perdu), vol dans le sac d'une bête vivante (même prime partout) |
| `pets` (sauvegarde avec un animal dans l'escouade) | animaux de l'escouade : mêmes animaux partout, animal donné au client (il lui appartient), ses ordres passent par l'hôte (position, santé, inventaire identiques), inventaire partagé sans doublon quand hôte et client y déposent en même temps, sac porté identique |
| `animalshop` (`kctest_town`) | A_FAIRE 3i : l'hôte crée un vendeur (`spawnnpc`) et trois chèvres (`spawncreature 3993-gamedata.base`) près du perso du client, puis la boutique d'animaux du jeu pour ce perso (`animalshop`) : fenêtre ouverte chez le client seulement (`animalshopstate` : `visible=1` chez lui, `visible=0 offers=1` chez l'hôte, capture `animalshop_client.png`) ; le client coche la première bête et valide (`animalbuy 0`) : fenêtre fermée, `sold=1` chez l'hôte, cats prises chez l'hôte, même argent partout, une bête de plus dans l'escouade chez l'hôte et le client, propriétaire = le client, positions identiques (la ligne « bought animal now » de l'hôte est recopiée ; en échec du 12/10 au 13/10 : le `confirm()` ouvrait l'éditeur de persos chez l'hôte, qui se mettait en pause et ne mettait plus à jour sa liste de persos, corrigé en 0.3.4, non revérifié en jeu) ; puis l'hôte achète pour lui-même (sa fenêtre, rien chez le client). Lancer : `python tools/coop_test.py animalshop` |
| `prospect` (`kctest_town`, puis une sauvegarde en plein désert) | A_FAIRE 3p : le client appuie sur PROSPECT pour son perso (`prospect <i>`) : fenêtre ouverte chez lui (`prospectstate` : `visible=1`, son nom, sa position), rien chez l'hôte (capture `prospect_client.png`) ; second appui : fermée ; l'hôte prospecte avec un de ses persos qui est dehors (groupe d'étage < 9 dans le relevé : le jeu refuse PROSPECT à l'intérieur, et le membre 0 de `kctest_town` est dans un bâtiment) (fenêtre après la tâche 150), rien chez le client ; l'hôte ne peut pas prospecter avec le perso du client. Lancer : `python tools/coop_test.py prospect` (`--save` pour le désert) |
| `suite` | **tout en une session, RÉUSSI / ÉCHEC point par point** (ci-dessous) |
| `four` (`--clients 3`) | 1 hôte + 3 clients : chaque client comparé à l'hôte **et aux autres clients** ; chacun ne commande que son personnage |
| `run` (`--quick`) | scénario complet avec rapport de désynchronisation |
| `soak` (`--minutes 20`) | **stabilité** : vitesse 1 → 2 → 3 → 2 → 1 par phases de 90 s, rafale de 20 s au début de chaque phase (vitesse 1/2/3 et pause changées toutes les 1 à 2 s, touches de vitesse aussi chez le client), pause / reprise en pleine activité ; pendant chaque phase : escouades de l'hôte et perso du client qui bougent, combat contre un PNJ créé, K.-O. puis fouille par le client, fenêtre de commerce ouverte / fermée, bâtiments posés par les deux, perso du client téléporté à 30000 puis ramené (une fois). Le client demande une vitesse et une pause (sa vitesse peut s'écarter de 5 % de celle de l'hôte, le temps que son horloge rattrape celle de l'hôte) : l'horloge de l'hôte ne doit pas bouger et le client y revient. Toutes les 30 s : les deux jeux vivants, comparaison en pause (tolérances de la `suite` : escouade ≤ 0.1, 8 pour un corps au sol, K.-O., mort ou à terre jambe brisée (drapeau 4 chez l'hôte) ; aucun manquant, état vital ou inventaire différent), aller-retour d'une commande (< 1 s), mémoire des deux processus (psutil, sinon tasklist) : ÉCHEC si elle dépasse +40 % du relevé pris après 2 min |
| `join4` (`--clients 3`) | **plusieurs joueurs** : hôte + 3 clients lancés avec chacun un faux Steam id (`KC_FAKE_STEAM_ID`) et un nom (`KC_PLAYER_NAME` : Joueur2, Joueur3...), qui rejoignent **en même temps** : ils chargent le monde ensemble, puis chacun ferme l'éditeur (`editdone`) à son tour. Vérifie les arrivées (tous entrent, aucun rejeté ; ceux qui attendent l'éditeur voient leur position 2..N/N et le joueur dans l'éditeur ; le dernier voit sa position avancer ; plusieurs clients chargent le monde en même temps ; l'hôte affiche la file), puis : ids de joueur distincts, chaque client a son propre perso, chaque client voit les persos de tous les joueurs (noms et positions identiques à l'hôte, en pause) ; puis tous partent proprement et l'hôte continue |
| `stress4` (`--clients 3 --minutes 15 --hop 180 --seed 4242`) | **4 joueurs partout sur la carte** : les persos de chaque client vivent dans leur propre région lointaine (±40000 autour de la maison de l'hôte, ≥ 30000 de l'hôte et des autres) et changent de région toutes les `--hop` s ; dans chaque zone : combat contre un PNJ créé, K.-O. puis fouille, bâtiment posé. Chaque client **clique partout** (fil par client, graine dérivée de `--seed`, chaque action journalisée avec sa graine pour rejouer un plantage) : déplacements (les siens et ceux des autres, refusés), parler, coffres, glisser des objets, sélection, ordres de la barre, tâches, porter, ramasser, commerce, fermer les fenêtres, vitesse / pause, construire. Tous les joueurs en **mode dieu** (`god all` de la console admin, `heal all` toutes les 8 s). Deux clients prennent les mêmes objets d'un même corps au même instant. L'hôte change de vitesse 1/2/3 et met en pause ; le dernier client part puis revient. Toutes les 30 s : tous les jeux vivants, comparaison en pause hôte / chaque client (escouade : 0,1 au sol, 15 en hauteur, 8 pour un corps à terre ; pas de perso de joueur en trop chez un client), persistance par zone (un perso à moins de 1500 d'un client absent deux relevés de suite, ou, à moins de 300, décalé de plus de 3 au sol (8 pour un corps à terre) ou de 15 en hauteur = ÉCHEC), aller-retour, temps de frame de l'hôte (90 % des relevés < 250 ms, aucun > 1 s), mémoire par processus (+40 % après 2 min), **objet marqué** (5 donnés à chaque joueur) compté pareil chez l'hôte et chaque client et jamais en trop (chaque perso compté une fois : un membre d'escouade figure deux fois dans un relevé). À la fin, chacun ramené à la maison. Hauteur : chaque jeu pose ses persos sur son propre sol ; au 10/10, tous les écarts de hauteur venaient avec 0,000 à 0,017 au sol, dans les régions lointaines où l'hôte n'a pas de caméra. 300 : la portée où le mod corrige les persos (`kSeenRange`, `kBodySeen`) |
| `camfocus` (`kctest_town`, 0.3.2, A_FAIRE 3l) | caméra du client : après le chargement, le perso suivi (focus de la vue des étages) est un perso du client ; `camerafree` fait ce que font les touches de caméra (le jeu lâche le perso suivi) ; pendant 6 s le focus reste « aucun » et aucune nouvelle ligne « floor view: follows our » n'apparaît (avant : 2 par seconde, caméra ramenée sans cesse) |
| `buildpause` (`kctest_base`, 0.3.2, 3n) | l'hôte ouvre le mode construction (`buildmode 1`, comme la touche B) : son jeu n'est pas en pause et le perso du client marche (> 30 unités en 3 s) ; si l'hôte avait mis pause avant, la pause reste ; le mode construction du client ne met ni l'hôte ni le client en pause |
| `quietlogs` (`kctest_town`, `--clients N`, 0.3.2, 3h) | 3 min à x3 : chez chaque client, lignes de porte « stays » (≤ 5 par minute) et « captivity from the host » (≤ 40 par minute) comptées par `logcount`. À jouer aussi en ville la nuit (les PNJ au lit comptent comme « caged ») |
| `attackers` (`kctest_base`, 0.3.2, 3g) | le perso du client est envoyé à 6 km ; un PNJ créé près d'un perso de l'hôte le combat : le client, caméra sur le perso de l'hôte, a ce PNJ (`where <clé>`) en moins de 15 s |
| `facing` | un personnage de l'hôte court dans plusieurs directions : le client doit vraiment courir, dans le même sens |
| `jitter` (`kctest_town`) | les PNJ immobiles ne tremblent pas chez le client, quelle que soit la vitesse |
| `kosquad` | des membres de l'escouade K.-O. chez l'hôte tombent et restent au sol chez le client, puis se relèvent ensemble |
| `far` | le personnage du client à environ 5 km de l'escouade de l'hôte : l'hôte simule-t-il bien sa zone, et le client voit-il la même chose ? |
| `camjump` | (0.3.1, plantages de rob) perso du client à 5 km, escouade de l'hôte dans la foule (`kctest_town`) à vitesse 3 ; la caméra du client saute 20 fois entre les deux : client vivant, toujours connecté, moins de 200 « is gone here » |
| `squads` | nouvelles escouades et déplacements entre escouades, depuis l'hôte et depuis le client |
| `recruits` (`kctest_town`, `--clients 2` par défaut, 0.3.2) | A_FAIRE 3d, 3e, 3j, 3m. (1) un client crée lui-même un perso des modèles uniques `41370-Dialogue.mod` et `41299-Dialogue.mod` (`spawncreature` : « ok … unique=1 » ; sauté si Dialogue.mod n'est pas installé) ; (2) l'hôte crée une recrue de ce modèle (sinon une copie d'un PNJ voisin) et la recrute (`recruitnpc`) ; (3) il la passe avec G (`give`) hôte → chaque joueur, 6 tours en ~2 s, et la laisse au client 1, qui la range dans l'escouade de son perso (`squadmove`) ; chez chaque client : `squadcheck` `missing=0 outside=0 conflicts=0`, mêmes `squads` que l'hôte, la recrue y figure ; (4) le client 1 sélectionne la recrue et son perso et donne un seul ordre « parler » (`selectset`, `talksel name:Chef_Voleur`, sinon Thief Leader, Marchand, Trader, puis le plus proche ; 30 s chacun : le PNJ le plus proche n'a rien à dire, 12/10 et 13/10) : la conversation s'ouvre chez lui, `busy=` de `dialogs` ne bouge pas ; (5) stress : 20 `give` d'affilée pendant que le client déplace la recrue (nouvelle escouade, puis retour), puis les mêmes vérifications. Vérifications « recrues … » et « parler a deux … ». Lancée le 12/10 et le 13/10 : 20/21, « parler à deux » venait du PNJ visé, corrigé |
| `squadui` | fenêtre Escouade et réglages d'IA : le client renomme son escouade, renomme celle de l'hôte (refusé), crée une escouade, y met son perso, le fait chef, tente de déplacer le perso de l'hôte (refusé), glisse son escouade en tête ; renommages et déplacements **simultanés** de l'hôte et du client (même résultat partout) ; nom de son perso (passe), de celui de l'hôte (refusé) ; fiche identique ; passif (valeur, pas bascule), style, tenir, allure ; réglage d'une recrue à personne et réglages simultanés hôte + client ; tâches ajoutées des deux côtés identiques (le perso du client suit celui de l'hôte, celui de l'hôte suit un autre perso de l'hôte : jamais deux persos qui se suivent l'un l'autre, le jeu retire alors le premier « rester proche »). Vérifications « escouade : … » et « ia : … » |
| `stuck` | la copie d'un PNJ poussée dans un mur ou sous le sol chez le client revient où l'hôte l'a |
| `farnpc` | écart des PNJ qui marchent loin de l'escouade du client |
| `beds` (`kctest_town`) | le perso du client dort dans le lit libre le plus proche, puis mine |
| `tpdown` | TP admin du perso du client mis K.-O. : le membre 0 de l'hôte marche 200 unités (attendu jusqu'à l'arrêt), le perso du client est mis K.-O. puis téléporté à côté ; il est à moins de 30 de l'endroit visé (la place du membre 0 au moment de la TP : deux persos posés côte à côte s'écartent ensuite) et au même endroit chez le client (< 3) ; les lignes « tp: » de l'hôte sont recopiées (nœud de scène du corps, 0.3.4). 13/10 10:43 : 193,5 (le corps reprenait sa place en se recouchant), corrigé en 0.3.4, pas encore relancé |
| `tpstop` (0.3.4) | A_FAIRE 12/10 point 10 : le perso du client marche au loin (son propre ordre, 900 unités) quand la TP admin l'amène près de l'hôte : il arrive (< 30), y est encore 8 s plus tard (< 40, il ne repart pas vers sa destination), même position chez le client (< 3), « walk and orders ended » dans le journal de l'hôte. Pas encore lancée |
| `bedfree` (`kctest_town`, 0.3.4) | A_FAIRE 12/10 points 11 et 12 : un perso de l'hôte mis au lit chez le client seul (`localbed`) en est sorti par le client tout seul (< 15 s, ligne « while the host has it free: taken out ») ; le perso du client couché chez l'hôte (`bedreq`) l'est chez le client, puis levé par un ordre de marche : levé chez le client moins de 6 s après l'hôte ; `posturecheck` : aucune copie debout chez l'hôte n'est couchée, au lit ou « portée » par personne. Pas encore lancée |
| `admin` | administration de l'hôte (commandes `admin …`) : refusée chez le client (commande et console) ; mode dieu sur le perso du client, combat contre un PNJ créé : ni sang ni membre perdus chez l'hôte, même état chez le client ; le mode dieu tient après un resync (reconnexion), puis retiré ; 100 points d'XP en attaque = le calcul du jeu (5 × 20), même valeur chez le client ; +3 niveaux dans chaque compétence, mêmes valeurs ; TP joueur → hôte, hôte → joueur, joueur → point de la carte : arrivée et même position chez le client ; soigner réveille un K.-O. ; argent +5000 partout ; objets : `admin spawn` refusé chez le client, 12 matériaux de construction (`580-gamedata.base`) près du perso du client, même nombre de piles et d'objets au sol chez le client, aux mêmes endroits (< 3), le client en ramasse un (`pickupreq`), puis même nombre au sol des deux côtés ; dieu pour tout le monde puis retiré. Pas encore lancée en jeu |
| `talk` | un PNJ parle au personnage du client : la conversation tourne chez l'hôte, la fenêtre s'ouvre chez le client |
| `dialogue` (`kctest_town`) | bac à sable : l'hôte crée un PNJ à côté du perso du client. Le perso du client lui parle (sinon le PNJ engage) : fenêtre chez le client, acteur = son perso, l'hôte suit la conversation, pas de pause chez l'hôte, aucun perso de l'hôte entraîné ; une réponse arrive à l'hôte ; le client lit « va parler » ; le même perso reparle au même PNJ : jamais « occupé », la conversation reste ; le perso de l'hôte (un autre joueur) qui parle au même PNJ (`talkto … dlg` : celui de la conversation du client, pas le PNJ créé) reçoit « occupé » et la conversation du client continue ; « partir » la ferme ; le PNJ engage lui-même (événement 3, contrôle de garde, sinon 1), d'abord le PNJ à qui le client vient de parler (`npcevent … dlg`), puis le PNJ créé et les 8 plus proches ; une TP puis un combat la ferment ; conversation et réponse à la vitesse 3. Vérifications « dialogue : … ». 13/10 (v0.3.4) : 14/16, la TP et le combat ne fermaient pas la conversation (corrigé, pas relancé) |
| `npctalk` (`kctest_town`, 0.3.4) | A_FAIRE du 12/10 au soir, points 1, 7, 8. (1) le client fait un vrai clic droit sur un PNJ par la fonction du curseur du jeu (`talkclick … click`, les 3 plus proches, puis par nom `Chef Voleur`, `Marchand`… : les PNJ les plus proches de `kctest_town` n'ont rien à répondre, 13/10) : un ordre « parler » arrive à l'hôte (celui du jeu, ou celui de secours si le jeu n'en donne pas) et une conversation s'ouvre ; (2) clic que le jeu laisse sans ordre (`talkclick … nogame`) : le journal du client le dit (« gave no order here ») et l'ordre part quand même (`sent=1`), la conversation s'ouvre ; (3) copie non liée (`talkclick … unlinked` : handle que l'hôte ne connaît pas) : l'hôte retrouve le PNJ par type et endroit, la conversation s'ouvre ; (4) à la vitesse 3, un PNJ engage la conversation (contrôle de garde, événement 3, sinon 1) : elle reste ouverte 12 s sans réponse, sans « closed by the game », puis la réponse arrive à l'hôte. Vérifications « parler : … ». 13/10 (v0.3.4) : 13/14, aucun des 5 PNJ les plus proches n'avait de conversation à réponses (test corrigé) ; 13/10 10:34 : 12/14, les étapes (2) et (3) visaient « le 3e PNJ le plus proche » **recalculé depuis la nouvelle place du perso** (à côté du Chef Voleur) : Marchand Shinobi puis un Voleur (« the NPC has nothing to say » chez l'hôte), pas le Chef Voleur ; elles visent maintenant le PNJ de l'étape (1) par son nom (`name:Chef_Voleur`), pas relancé |
| `factions` | lot B : mêmes relations au départ ; relation changée par l'hôte identique chez le client ; prime donnée puis levée par l'hôte visible chez le client ; une relation changée par le jeu du client revient à celle de l'hôte |
| `diplomacy` (`kctest_town`) | diplomatie : mêmes guerres entre factions, chefs et villes au départ (empreintes `diplo`) ; guerre déclarée par l'hôte entre deux factions identique chez le client ; paix faite par le jeu du client remise à la guerre de l'hôte ; un chef tué par les joueurs chez l'hôte l'est aussi chez le client ; une ville prise par une autre faction chez l'hôte l'est aussi chez le client ; tout est remis comme avant ; mêmes empreintes à la fin. Vérifications « diplomatie : … » |
| `ranged` (`--shooter <clé>`) | (lot C) 3 tirs d'un arbalétrier de l'hôte sur l'escouade sont refaits chez le client, sur la même trajectoire ; même point visé ; une tourelle proche tournée chez l'hôte tourne pareil chez le client ; santé et inventaires identiques ensuite |
| `placevalid` | les poses sont vérifiées comme le mode construction vérifie un endroit : le perso du client va sur la terre ferme près du lac d'acide (54612, 40576) ; ce point est de l'eau/acide chez les deux ; refusé par la vérification du client (`buildcheckat`, `buildplaceat`) et de l'hôte ; envoyé quand même (`force`), l'hôte le refuse, le journalise avec la raison, le client reçoit « dans l'eau ou l'acide », rien n'est bâti (aucune annonce, rien dans le lac chez les deux) ; une pose sur un endroit sec à côté est acceptée et bâtie au même endroit chez les deux |
| `build` (lot E) | une pose du client bâtie par l'hôte puis par tous au même endroit, celle de l'hôte aussi ; avancement et fin du chantier ; démontage demandé par le client ; achat d'un bâtiment à vendre (avec `--save kctest_town`) |
| `construct` (`--material`, `--task`) | vraie construction : argent et matériaux donnés aux deux joueurs ; le client pose un chantier et ordonne à son perso de le bâtir (clic droit : `newPlayerTaskSelectedCharacters`), puis l'hôte, puis les deux en même temps ; avancement des deux côtés, matériaux qui baissent pareil, chantier terminé identique. Le numéro de tâche « construire » n'est pas connu : sans `--task`, le test essaie 1 à 99 jusqu'à ce que l'avancement bouge chez l'hôte |
| `buyhouse` (`kctest_town`) | achat d'un bâtiment à vendre par le client (`buildbuy` = le bouton de confirmation de la fenêtre d'achat) puis par l'hôte : même prix débité partout, bâtiment à nous partout, porte et conteneur utilisables ; refus sans argent ; achat simultané du même bâtiment : un seul paiement. ÉCHEC explicite s'il n'y a aucun bâtiment à vendre à 3 km |
| `farlong` (`--seconds 300`) | le perso du client à plus de 30000 unités pendant 5 min : zone comparée toutes les 30 s (PNJ, santé, inventaires), combat lancé et bâtiment posé là-bas, l'escouade de l'hôte bouge ; pas de PNJ manquant ni d'écart de position qui dure (le même perso sur deux relevés de suite) près du perso du client (1000 unités pour les absents, 300 pour les positions ; les chiffres de toute la zone vont seulement dans le journal : PNJ autour de l'escouade de l'hôte que le client n'a pas chargés, trafic), l'hôte simule la zone, pas de plantage, aller-retour d'une commande < 1 s ; retour (TP admin) et comparaison |
| `farfps [--clients 3] [--seconds 60]` (`kctest_town`) | (3b) **fps des clients quand les joueurs s'éloignent**. Trois fenêtres de mesure sur chaque jeu (`perf` et `standins`) : tous ensemble près de l'hôte ; chaque client dans sa ville loin (plus de 30000 de l'hôte, 15000 des autres, caméra de l'hôte à la maison) avec `farleave 0` (comportement 0.3.1) ; puis avec `farleave 1` (correctif). Le journal donne pour chaque client fps, notre travail par image et persos suivis dans les trois cas. Points « loin [Jn] : fps : … » : mesures lisibles, des persos de l'hôte laissés à la mise à jour lointaine loin des autres, jamais un membre d'escouade, fps avec le correctif ≥ 95 % de celles du 0.3.1, notre travail < 4 ms par image, (0.3.4) les copies en sommeil ne bougent pas (`dormant` : aucune à plus de 20 de sa dernière position) ; puis PNJ fluides autour de chacun (`far_motion_step`, suivi de `copyfix`) et synchro (`far_zone_sync`, 2 relevés). Pas encore lancée |
| `farwalls [--seconds 40]` (`kctest_town`) | (3f) **murs loin de la caméra de l'hôte**. Le perso du client dans une ville loin ; un bâtiment près de lui (`wallsnear`) ; il est posé d'un côté, un bonedog créé juste derrière l'autre côté, puis le client ordonne l'attaque (`attackreq`). Pendant le combat, positions des deux chez l'hôte à 10 Hz (`motionrec`), `navcheck` toutes les 5 s, `movestats` avant / après. Points « loin [J2] : murs [...] : … » : positions enregistrées, **personne à plus de 20 unités dans la boîte du bâtiment**, combattants sur le navmesh. Une fois caméra de l'hôte à la maison, une fois caméra de l'hôte sur le combat : si seule la première échoue, c'est une règle de caméra du jeu de l'hôte (le journal le dit). Pas encore lancée |
| `faraway [--clients N]` (`kctest_town`, dans `run_all` avec 1 client ; 3 pour la partie à 4) | (0.3.1, session réelle : client à 900 m dans une autre ville) **tout marche loin, sans que l'hôte regarde**. Avec `--clients N` (`setup_multi`, fenêtres en mosaïque) chaque client va dans **sa** ville, à plus de 30000 de l'hôte et 15000 des autres, et tous font les mêmes étapes **en même temps** (un fil par joueur) ; points « loin [J2] : … », « loin [J3] : … ». La synchro met l'hôte en pause une fois pour tous (pas au milieu de l'étape d'un autre) et donne le pire écart par relevé (`dy`, `inview` hôte/client pour chaque écart) ; le dialogue attend 45 s par PNJ (marche de 20-30 s) ; le combat crée le PNJ au même niveau que le perso, le nomme (`fight`/`ko`/`kill <clé>`), relance le combat s'il retombe ; plus de pause pour les vitaux après le combat (relus jusqu'à 4 fois). Étapes : tout le monde soigné et en mode dieu ; l'escouade de l'hôte reste à la maison, sa caméra dessus (`campos` relevé toutes les 2 s pendant tout le test) ; le perso du client part dans une autre ville à plus de 30000 unités (liste `towns`, villes connues à marchands d'abord, `merchants <index>` doit en trouver un), `far_teleport` (terre ferme, posé sur le sol). Points « loin : … » : **1** synchro autour du perso du client (3 relevés à 20 s, hôte en pause, 3000 unités) : aucun PNJ manquant deux relevés de suite, positions à 3 unités (corps à terre 20), mêmes vitaux (sang, état, membres) ; **2** mouvement : des PNJ qui marchent près de lui (et son perso, qui marche) échantillonnés à 10 Hz chez le client pendant 5 s (`motionrec`) : jamais plus de 120° entre déplacement et regard (marche à reculons), aucun saut de plus de 25 unités par échantillon ; **5** dialogue : le client parle à un PNJ de la ville (`talkto`), fenêtre chez lui, l'hôte la suit (`dialogs`), une réponse arrive à l'hôte, puis un PNJ engage la conversation (`npcevent`) s'il en a une ; **6** commerce : clic droit sur le marchand (`npcreq … 26`, sinon `tradeopen` de l'hôte), stock non vide, un achat, même argent et mêmes inventaires ; **7** objet au sol : le client lâche du minerai (`uidrop`) et le ramasse (`pickupreq`) : une seule pile au même endroit des deux côtés, puis rien de perdu ; **3** combat : PNJ créé à côté de lui, qui se bat : blessé chez l'hôte, mêmes vitaux et blessures chez le client, chiffres de dégâts faits hors de la caméra de l'hôte (`floaters` : `forced`) et affichés chez le client (`shown`) ; **4** fouille : ce PNJ assommé, fouillé par le clic droit (`selectset` + `lootorder`, sinon `loot`), fenêtre ouverte, un objet pris, mêmes inventaires ; une chèvre tuée et fouillée aussi ; **9** une troisième ville loin de l'hôte (et à 15000 de la deuxième si possible) : synchro (2 relevés) et dialogue de nouveau ; **8** la caméra de l'hôte n'a jamais été à plus de 5000 de son escouade, son escouade n'a pas bougé, le client est toujours connecté. Pas encore lancée |
| `progress` | compétences, argent, bulles et ordres : l'hôte décide, le client suit |
| `ground` / `clientpickup` | objets posés et ramassés ; ramassage demandé par un client |
| `grounddrop` | objets lâchés par le chemin du glisser-déposer (`uidrop` = `Inventory::dropItem`) : une pile de minerai, une arme, une armure, par l'hôte puis par le client, puis les deux à la fois, puis ce qui tombe sur un K.-O. et une mort. Chaque objet une seule fois des deux côtés, même pile, à moins de 5 unités ; ramassé par l'hôte, il disparaît partout. Compteurs `groundstats` des deux côtés au journal. Pas encore lancée |
| `backpack` (`kctest_mine`, v0.3.2) | **sacs à dos** (plantages de l'hôte du 11/10 23:04 et 23:48, point 0 et 3c d'A_FAIRE) : l'hôte met un **Traders Backpack Medium** sur le perso du client (`wearbag N 1498-gamedata.base` ; bonus de pile min 2, ×3 : Matériaux en piles de 6, relevé par `bagbonus` des deux côtés) et lui donne 10 Matériaux de Construction et une arme ; le client range 1 unité dans le sac (elle rejoint une pile pas pleine s'il y en a une), en pose 1 sur une pile (**cette pile + 1** chez l'hôte), y ajoute le reste de la pile (jusqu'à 6 par pile), en ressort 3, range l'arme (ne s'empile pas), lâche les matériaux du sac au sol (**une fois au sol, plus dans le sac, une seule ligne `client item drop done`**), l'hôte les ramasse ; s'il y a un marchand à côté, achète et range l'achat dans le sac ; enfin travail permanent de minage (`objjob … 87 Ressource_Fer`) 2 min à x3. Après chaque étape : `invcheck all` propre chez l'hôte et le client, même sac des deux côtés ; à la fin : pas de `CRASH`, mêmes inventaires (`compare`), aucune ligne `INVCHECK` en erreur (contrôle après chaque opération, `invcheck each 1`) |
| `anim` / `animframe` / `gait` | animations de combat et d'action ; tout ce qui est à l'écran ; allure |
| `fx` / `fxlive` | effets météo (orage forcé puis comparaison en pause ; en direct) |
| `bodies` / `dead` / `items` | où reposent les corps ; morts ; objets |
| `lootorder` / `lootclick` / `lootui` | fouille par clic droit (`lootclick` prépare une scène à tester à la main) |
| `lootcreature [--only <nom>]` | (0.3.1) chèvre, chien d'os, taureau sauvage, gorillo, araignée à peau, bête à bec, tortue des marais, léviathan : l'hôte fait apparaître chaque créature près du perso du client (`spawncreature`) et la tue ; le client l'ordonne de fouiller (`loot`) ; la fenêtre doit s'ouvrir (`lootstats`), l'inventaire de l'hôte doit contenir ses « death items », un objet pris doit quitter le corps chez l'hôte et les deux côtés concorder. Ferme d'abord l'éditeur du client (`editdone`) et attend la fin de la pause. Premier essai (11/10) : 26/32, l'éditeur restait ouvert (jeu en pause) ; corrigé, à relancer |
| `walk` / `trace` | trace image par image des corrections de position d'un PNJ qui marche |
| `menu` | un hôte qui héberge et un second jeu laissé **au menu principal**, pour rejoindre à la main |
| `up` | un hôte et un client connectés, laissés ouverts pour un test manuel |
| `prison` (lot D) | l'hôte met le personnage du client dans la cage la plus proche, l'enchaîne, le réduit en esclavage puis le libère : même état chez le client, gardé dans la cage, tout effacé à la fin. Il faut une cage à moins de 300 m de l'escouade (`--save` d'une sauvegarde près d'une prison ou d'un camp d'esclavagistes) |
| `actorsafety` (`kctest_town`) | **sécurité des acteurs** : l'hôte sélectionne ses propres persos (comme un joueur) ; le client donne avec son perso chaque sorte d'ordre (lit, parler, piller, ramasser, commerce, tâche, construire, porte, premiers soins sur un perso de l'hôte, suivre, porter un perso de l'hôte K.-O., attaquer) : après chacun, la sélection de l'hôte est inchangée et ses persos n'ont rien reçu (pas de nouvelle tâche, pas de déplacement, pas de lit, pas de conversation ; `actorstats` leaks=0). Puis ordres forgés (`forgeorder`) au nom d'un perso de l'hôte : refusés ; et ordres de son perso visant une cible du mauvais type (dont le chemin du plantage de `stress4` : `npcreq <perso> 2 a`, construire sur un PNJ) : refusés, l'hôte et le client vivants. Contrôles « securite controle : ... ». Pas encore lancée en jeu |
| `crashrejoin` (`--only abcdefg`) | client **tué** (`taskkill /F`) puis relancé avec le même faux id Steam et revenu : (a) au repos, (b) en combat à la vitesse 3, (c) en portant un corps, (d) fenêtre de commerce ouverte, (e) en fouillant un corps, (f) dans l'éditeur de personnage, (g) relancé avant que l'hôte ait vu la coupure. À chaque fois : l'hôte vit, voit la coupure (< 20 s) et nettoie (journal « disconnected (… » / « cleaned up: »), le joueur retrouve les mêmes persos (clés, inventaires ; sauf combat), pas de doublon, comparaison hôte / client propre, l'hôte n'est pas resté en pause |
| `map` (`--clients 2`) | **carte, minicarte, repères, barre, pings** : chaque client reçoit le flux de l'hôte (âge < 2 s, mêmes persos, mêmes propriétaires et positions à 30 unités près, mêmes escouades hostiles ; un PNJ copié qui se bat contre l'escouade sert d'ennemi) ; chaque joueur a un perso « à lui » ; chaque machine dessine tous les persos, une couleur par joueur (`mapscene carte`) ; projection du jeu (`worldToMapCoords`) = la nôtre à 1,5 px près ; minicarte centrée sur son perso ; noms au-dessus des têtes à la couleur de leur joueur (nom du jeu recoloré, `tint=1`, ou nom de l'overlay quand le jeu n'en montre pas), perso principal de chaque joueur encadré, les autres non ; cadres de la barre seulement aux couleurs des joueurs ; vue 3D non couverte et cadres ou noms dessinés (`mapconv`) ; cadres dessinés = portraits du jeu relus et convertis, à 2 px près ; **écran de carte ouvert par le bouton MAP du jeu** (`mapui open`), détecté, marqueurs dessinés, image à sa place ; **fenêtre du client 1 redimensionnée deux fois** en cours de test (cadres et image de la carte toujours à leur place), puis remise en grille ; installation par `setup_multi` (chaque client ferme son éditeur à son tour de la file d'arrivée) ; ping du client 1 visible chez l'hôte et chez le client 2 à son nom, second ping trop rapide refusé, 5 au plus, effacé après 10 s. Contrôles « carte : … », « minicarte : … », « tetes : … », « barre : … », « ping : … ». 13/10 10:28 (v0.3.4) : 48/50, le client 1 voyait le nom du perso principal du client 2 (arrivé après lui) ni recoloré (`tint=0`) ni encadré : nom du jeu que le jeu ne met pas à jour, maintenant écrit par l'overlay (`why=notupdated`) ; la réponse de `mapscene tetes` était entière (~930 caractères), seuls le journal (600) et le détail (300) du test la coupaient : réponse entière au journal, détail = noms fautifs, 6 essais sur 3 s ; pas relancé. 13/10 13:46 : encore 48/50, même nom : le jeu le mettait bien à jour (`label=jeu`), mais ce perso, copie faite chez le client 1 sous un autre handle que celui de l'hôte, était cherché par le handle de l'hôte pour les cadres et la lecture `tint` ; la scène garde maintenant le handle de la copie ; pas relancé |
| `map` (`--clients 2`) | **carte, minicarte, repères, barre, pings** : chaque client reçoit le flux de l'hôte (âge < 2 s, mêmes persos, mêmes propriétaires et positions à 30 unités près, mêmes escouades hostiles ; un PNJ copié qui se bat contre l'escouade sert d'ennemi) ; chaque joueur a un perso « à lui » ; chaque machine dessine tous les persos, une couleur par joueur (`mapscene carte`) ; projection du jeu (`worldToMapCoords`) = la nôtre à 1,5 px près ; minicarte centrée sur son perso ; noms au-dessus des têtes à la couleur de leur joueur (nom du jeu recoloré, `tint=1`, ou nom de l'overlay quand le jeu n'en montre pas), perso principal de chaque joueur encadré, les autres non ; cadres de la barre seulement aux couleurs des joueurs ; ping du client 1 visible chez l'hôte et chez le client 2 à son nom, second ping trop rapide refusé, 5 au plus, effacé après 10 s. Contrôles « carte : … », « minicarte : … », « tetes : … », « barre : … », « ping : … ». Pas encore lancée en jeu |
| `machineout` (`--seconds 60`) | (0.3.4, A_FAIRE point 6) en plein désert, l'hôte pose une cuisinière `1944` près du perso du client, y met du blé ; le perso du client la travaille (`workreq`) N secondes à x3. Fenêtre fermée chez le client : même contenu partout (`storage`), **aucune** « inventory move asked of the host » chez le client, aucune « not done (nothing to do) » chez l'hôte (`logcount`). Puis le client ouvre la cuisinière et prend la sortie (`contake`) : la demande part et les inventaires sont identiques. Contrôles « sortie machine : … ». 13/10 : 5/6, l'échec venait du test (`containerreq` ne trouvait le conteneur que par son nom traduit, le test passait l'identifiant ; cuisinière restée vide, « Ble » / « Wheat » n'étant aucun objet). Maintenant : paille de blé `1913` par identifiant, `machinegive` essaie chaque section ; cuisinière vide pendant le travail : « NOT TESTED » pour la partie fenêtre fermée, puis de quoi prendre y est mis avant l'ouverture |
| `carry` | le perso 0 de l'hôte assomme un PNJ créé (`spawnnpc`, `ko`), le porte (`carrynpc`) : porté par le même perso chez le client, animation « porté » ; posé (`carrydrop`) : 10 s plus tard le corps du client est à moins de 5 unités (au sol) de celui de l'hôte, immobile ensuite, plus porté ; `posturecheck` sans rien de couché que l'hôte a debout. Contrôles « porter PNJ / poser PNJ / posture : … ». 13/10 : pas de BILAN (`summary()` manquait, ajouté) et corps à 12 unités (corrigé : un corps posé après un portage est relevé une fois et refait tomber au point de l'hôte s'il en est à plus de 5 unités). 13/10 13:56 : 6/7, la correction a tourné mais le corps est retombé au même endroit au millième près (téléporté debout à la hauteur lue chez l'hôte, 17,5 au-dessus du sol : le jeu le posait sur l'escalier, 12 unités plus loin) ; corrigé : debout à la hauteur du sol de son porteur ; pas relancé |
| `tradeswap` (`--merchant`, sauvegarde `kctest_town`) | (0.3.4, point 4) fenêtre de commerce ouverte **tout de suite** après l'arrivée (le marchand est souvent encore une doublure chez le client) ; pendant 60 s, attend « trade window: now on » (fenêtre passée au vrai marchand) ou « cannot move it » ; puis : fenêtre toujours ouverte, argent du marchand non nul, pas de « DID NOT STICK », une vente et le même argent partout. Si le marchand n'était pas une doublure remplacée : « NOT TESTED » au journal. Pas encore lancée |
| `workstations` (`--minutes 2`) | **postes de travail de base** : escouade en plein désert, soignée, en mode dieu ; l'hôte et le client posent chacun une ferme (`1911`, sinon `2021`), une cuisinière `1944`, un établi (`2929` / `2262` / `2299`), un petit banc de recherche `1875`, un puits `1921`, une éolienne `1999` (tous `-gamedata.base`), finis par `buildprogress` ; paille de blé `1913`, livres et tissus mis dedans (`machinegive`, un objet par poste, le premier qui entre) ; une recherche payable par les bancs mise en file par le client (`researchreq`), sinon aucun poste n'a rien à faire ; le client met son perso au travail au clic droit (`workreq`) et en permanent par le panneau Tâches (`workjob … 87`, puis « suivre » 31 sur un perso de l'escouade, `jobreq` : le transport 88 sans cible ne devient jamais un travail), l'hôte pareil ; au départ puis après N minutes à x3 : `machines` (ouvriers, marche, production, cultures, ordres) identique chez l'hôte et le client, `crops any` identique, `storage <poste>` identique pour chaque poste, `jobs <i>` identique pour chaque membre, tâches 87 et 31 du client chez l'hôte, au moins un poste avec des ouvriers (relevé toutes les 15 s, pas seulement à la fin ; JOBS allumé pour chaque membre qui ne l'a pas, bit 64 de `modes`, `orderreq <i> 15`, sans quoi le jeu ne fait pas les travaux permanents, et un chercheur envoyé au banc de recherche par clic droit ; les `modes` dans le détail de l'échec) ; les lignes `machine: … workers / crops / replanted / not found here` du journal de l'hôte. Contrôles « postes : … ». 13/10 : 43/45, les deux échecs venaient du test (transport 88 ; 0 ouvrier car aucun poste n'avait de travail : pas de recherche en file, cuisinière vide, établi sans tissu ni commande, ferme au désert), corrigé ; 13/10 13:59 : 44/45, recherche en file mais 0 ouvrier (JOBS des membres de l'hôte jamais vérifié), corrigé dans le test |
| `clientjobs` | **(v0.3.4) travaux des persos du client, comme en solo** (sauvegarde `kctest_base`, filon de fer `42248-gamedata.base`, 3 places). (1) bouton JOBS (`orderreq <i> 15`) deux fois : changé chez le client en 0,3 s, tenu malgré la synchro de l'hôte, le second clic revient en arrière, même état partout 5 s après ; JOBS remis allumé. (2) trois persos de l'hôte sur le filon (3/3 : la meilleure de 10 lectures sur 2 s, l'ensemble des opérateurs du jeu variant d'une image à l'autre pendant le travail, 13/10) ; ordre ponctuel du client (`workreq`) : « toutes les places sont prises » dans son chat ; Maj + clic droit (`workreq … shift`) : travail 87 ajouté chez l'hôte malgré la machine pleine, même liste chez le client, « place se libère » dans le chat ; le même encore : « déjà dans la liste », rien d'ajouté ; un ouvrier de l'hôte part : le perso du client y va de lui-même (à moins de 400 unités du filon) et prend la place en 120 s au plus ; s'il s'est battu entre-temps (défense, robustesse, esquive ou combat de masse en hausse : les combats de la ville autour du filon), la prise de place est « NON CONCLUANT » au lieu d'un échec. (3) 8 clics identiques en 0,8 s : 3 ordres exécutés au plus chez l'hôte. (4) panneau Tâches : travail « suivre » 31 sur un perso de l'escouade (`jobreq`) en second (pas le transport 88 sans cible, dont le jeu ne fait aucun travail), le dernier travail glissé en tête (`jobmove`) fait chez l'hôte et sans retour en arrière chez le client (relevé toutes les 0,5 s), puis retiré (`jobremove`) sans revenir. (5) une recrue à personne (`whoseidx` : `s`) reçoit un travail permanent du client. (6) le perso de l'hôte : rien ne change, « appartient à un autre joueur » dans le chat. Puis les lignes `work order`, `jobs:` et `client mode` du journal. Contrôles « jobs : … ». 13/10 : 15/17, les deux échecs venaient du test (transport 88 jamais ajouté ; combat au filon) |
| `research` | **atelier**. D'abord la mine : le perso du client et deux de l'hôte travaillent le même filon de fer de la sauvegarde (`workreq`, modèle `42248-gamedata.base`, 3 places ; un filon de cuivre n'en a que 2) : 3/3 et même minerai partout, clic sur le filon chez le client (`machineselect`) : au joueur et panneau ouvert, un mineur retiré vu partout. Tout le monde est soigné et réveillé (`console heal all`) et en mode dieu (`console god all`) dès le début : les combats du village couchent des persos, et un corps n'est pas téléporté pour de bon. Puis toute l'escouade est téléportée sur la terre ferme loin des villes (le membre 0, ancre du bac à sable, doit y être vraiment ; il y est renvoyé s'il en part, avant chaque pose) (l'hôte refuse une pose près d'une ville) et l'hôte pose le bac à sable par identifiant de modèle (les noms sont traduits) : petit banc de recherche `1875`, établi `2929` / `2262` / `2299`, éolienne `1999`, batteries `1855`, lampadaire / projecteur `14444` / `5363` / `5361` (tous `-gamedata.base`), livres / artefacts et tissus mis dans les bancs (`machinegive`). Recherche : même état des deux côtés ; le perso du client travaille le banc ; il met en file une technologie que les bancs peuvent payer (`researchpick 6 cost`, sinon une dont les prérequis sont remplis), l'hôte l'exécute, même file partout ; demandée de nouveau par le client et par l'hôte : une seule fois, rien consommé deux fois, inventaire du banc identique ; annulée puis remise ; avancée chez l'hôte (`researchprogress`) : connue partout, même arbre, menu de construction débloqué pareil ; le client pose et bâtit un bâtiment débloqué ; fabrication à l'établi (ordre du client, « répéter » changé par les deux en même temps, inventaire, travail de son perso, ordre retiré) ; compétences identiques ; énergie : générateur, batterie, lampe et totaux de la base identiques, générateur coupé par le client coupé partout. Contrôles « recherche : … », « fabrication : … », « mine : … », « energie : … ». Premier essai (10/10, 4/10) : bac à sable refusé près de la ville et noms non trouvés ; refait depuis |
| `cmd <pid> <commande…>` | envoie une commande de debug à une instance |

### La suite (`suite`) : 32 points

Arrivée et création :
1. Un personnage est créé pour le joueur.
2. L'éditeur de personnage s'ouvre chez le client.
3. L'hôte est en pause pendant l'édition.
4. La pause est levée à la validation.

Progression, bulles, ordres permanents :
5. Expérience identique chez le client.
6. Argent identique.
7. Bulles de dialogue affichées.
8. Furtif et « tenir la position » chez l'hôte (ordres du client).
9. Mêmes ordres chez le client.
10. Ordres retirés des deux côtés.

Porter et escouades :
11. Le client fait porter un corps à son personnage.
12. Le client voit le même corps porté.
13. Une nouvelle escouade de l'hôte est visible chez le client.
14. Le client remet son personnage dans une escouade : identique partout.

⚠ Points 13 et 14 : la suite déplace le personnage nommé `Player_2`. Or le personnage du client de
test s'appelle « Coucoudz 2 », puisque les deux instances ont le même nom dans `KenshiCoop.ini`.
Dans les journaux de la dernière suite :
```
squadmove Player_2 new -> err no Player 2
```
Ces deux points passent donc **sans rien tester**. À corriger dans `tools/coop_test.py`, en
prenant le nom du personnage du client. L'expérience `squads` a le même défaut aujourd'hui. Les
escouades avaient été vérifiées plus tôt, quand l'instance de test s'appelait encore « Player »,
puis confirmées en jeu par le joueur.

Mouvement :
15. Vitesse 1 : orientation en marchant, pire écart ≤ 30°.
16. Vitesse 1 : horloge d'animation recalée sur moins de 2 % des images.
17. Vitesse 3 : orientation (même seuil).
18. Vitesse 3 : horloge (même seuil).
19. Pause en pleine course : même position, à moins de 0,1.

Outils de l'hôte :
20. TP admin : le personnage du joueur arrive près de l'hôte (< 30 de l'endroit visé, la place du membre 0 au moment de la TP ; 0.3.4).
21. Le client le voit au même endroit (< 2).
22. Console externe ouverte chez l'hôte, avec le journal.
23. Journal : lignes du client relayées chez l'hôte.
24. Rapport de synchro du client.
25. Ordres du client écrits en clair.

Comparaison finale, partie figée :
26. Escouade identique (positions).
27. Aucun état de santé différent.
28. Aucun inventaire différent.
29. Personne ne manque chez le client.

Resync et reconnexion :
30. Resync : le joueur recharge le monde et retrouve son personnage.
31. Tout est identique ensuite.
32. Reconnexion : le joueur retrouve son personnage.

Historique des exécutions (`test_out/suite*.log`) :

| Date | Résultat | Remarques |
|---|---|---|
| 9 oct., 18 h 33 | 24/30 | horloge d'animation trop corrigée, TP admin, relais du journal |
| 9 oct., 18 h 38 | 30/30 | |
| 9 oct., 19 h 05 | 31/32 | échec « ordres retirés » (corrigé depuis) |
| 9 oct., 19 h 34 | 26/32 | une attaque de pillards pendant le test fausse 5 points (vitesse 3 ×2, TP admin ×2, positions finales). « Aucun état de santé différent » a révélé que le K.-O. ne prenait pas tout de suite chez le client : correctif en cours |

## 3. Canal de commandes de debug (`plugin/debug.cpp`)

Actif seulement avec `[debug] commands=1`.
- Le script écrit `kcp_cmd_<pid>.txt` dans le dossier de Kenshi, une commande par ligne :
  `<id> <commande> [arguments]`.
- Le mod lit ce fichier toutes les 0,1 s, l'efface, puis ajoute `<id> ok [détails]` ou
  `<id> err <raison>` à `kcp_out_<pid>.txt`.
- Chaque commande est aussi journalisée (`debug: <ligne> -> <résultat>`). Ces lignes ne sont pas
  relayées à l'hôte.
- Les fichiers `kcp_out_*.txt` s'accumulent dans le dossier de Kenshi : on peut les supprimer
  quand aucun Kenshi ne tourne.

Les commandes en dessous de `leave` exigent un monde chargé. « Index » désigne le rang dans
l'escouade triée par handle.

**Session**

| Commande | Rôle |
|---|---|
| `echo` | répond `ok` |
| `status` | état de la session, monde prêt, numéro, entités, PNJ, PNJ manquants, joueurs en cours d'arrivée, arrivées (`queue=` : client qui attend l'éditeur `2/3` + `queueWait=` + `queuePhase=editor`, hôte `Joueur2:editor,Joueur3:loading,Joueur4:wait`, `-` sinon ; espaces des noms remplacés par `_`), sauvegarde en cours, dernière erreur |
| `load <emplacement>` | charge une sauvegarde |
| `host` / `join [adresse] [port]` / `leave` | héberger, rejoindre, quitter |
| `give <joueur> <index\|all>` | donner un ou tous les membres de l'escouade à un joueur |
| `resync [joueur]` | (hôte) ce joueur, ou 0 pour tout le monde, recharge le monde de l'hôte |
| `tpplayer <joueur>` | (hôte) les personnages de ce joueur à côté du membre 0 de l'escouade |
| `admin <verbe> …` | (hôte seulement, `err` ailleurs) les actions de la section Administration, comme la console : `god <id\|all\|host> on\|off`, `xp <id\|all\|host> <compétence\|all> <n> [levels]`, `tp <id\|all> host`, `tp host <id>`, `tp <id> <id>`, `tp <id> point`, `tp <id> <x> <y> <z>`, `heal <id\|all\|host>`, `money <n>`, `spawn <sid\|nom> <n> [here\|<id>] [fabricant] [modèle]`, `list` ; `state` pour les tests : `godall=0 engine=<persos en mode dieu> p<id>=<dieu 0/1>/<persos> … point=x,z` |
| `consolewin` | la console hors du jeu est-elle là, et combien de texte montre-t-elle |
| `pause [0\|1]` / `paused` / `speed <x>` | pause ; état de la pause et vitesse ; vitesse |
| `buildmode [0\|1]` | (0.3.2) ouvre / ferme le mode construction comme la touche B ; répond `active= paused= speed= unpaused=` (pauses du mode construction levées chez l'hôte) |
| `camerafree` | (0.3.2) ce que fait le joueur en bougeant la caméra : `PlayerInterface::stopTrackCharacter` (le perso suivi lâché) ; à lire ensuite avec `floorview` |
| `logcount <texte>` | (0.3.2) lignes du journal contenant ce texte (`_` pour les espaces) depuis le premier `logcount` de ce texte (16 textes au plus) |
| `hours` | heure du jeu |

**Personnages et ordres**

| Commande | Rôle |
|---|---|
| `move <index> <x> <z>` / `moverel <index> <dx> <dz>` | ordre de déplacement |
| `teleport <index> <x> <y> <z>` | (hôte) déplacement instantané |
| `probe <index> <dx> <dz> <simple\|teleport>` | essaie une méthode de positionnement et dit ce qui tient |
| `pos <index>` / `where <clé\|npc\|index>` | position d'un personnage, et `carried=1` s'il a l'animation « porté » (la position d'un corps porté est celle du porteur) |
| `camto <index>` | caméra sur ce membre |
| `campos` | œil de la caméra de cette instance en coordonnées du monde (matrice de vue + origine de rendu de Kenshi) : `ok x,y,z fwd=fx,fz` ; `faraway` vérifie que l'hôte ne quitte jamais son escouade |
| `motionrec <secondes> <hz> <fichier> <clé>…` | ces persos relevés à cette fréquence sur le fil du jeu (position, regard, en mouvement, vitesse, à terre), lignes `m t clé x,y,z fx,fz bouge vitesse aterre`, fichier écrit à la fin (`end`) ; `faraway` y cherche la marche à reculons et les sauts |
| `towns` | chaque ville : `sid\|nom\|x,y,z\|propriétaire` (`_` pour les espaces) ; `faraway` y choisit les villes lointaines |
| `selectset <i> [<j>…]` | la sélection devient **exactement** ces membres (le perso principal que `unselectAll` garde en est retiré) ; `err` sinon |
| `selected` | indices des membres sélectionnés (triés ; `+n` : n autres objets) |
| `charstate <index>` | `tasks=` (système de tâches) `jobs=` (panneau Tâches) `in=` (0 rien, 1 lit, 2 cage) `dialog=` (0/1) `pos=x,z` |
| `actorstats` | (hôte) demandes de clients refusées (`refused=`), ordres refusés pour leur cible (`target=`), tâches arrivées sur un perso de la sélection de l'hôte pendant un ordre de client (`leaks=`, doit rester 0), sélection mal remise (`restorefail=`) |
| `results` | (client) réponses `Result` de l'hôte : nombre de refus, puis les dernières en `seq:état:raison` |
| `forgeorder <acteur> <via> <tâche> <sujet>` | (client) ordre **forgé** envoyé tel quel, sans nos contrôles : `via` 0 = aller, 1-4 = `TaskVia` ; sujet `none`, `self`, `squad<i>`, `npc`, `item`, `building` |
| `taskreq <sélection> <tâche> <sujet>` | sélectionne ce membre seul et donne l'ordre, comme l'interface |
| `talkreq <sélection>` | ordre de parler au PNJ le plus proche |
| `orderreq <sélection> <ordre permanent>` | bouton de la barre d'escouade pour ce membre seul |
| `modes <index>` | ordres permanents (bits : 1 furtif, 2 bloquer, 4 distance, 8 narguer, 16 tenir, 32 passif, 64 poursuivre) et style de combat |
| `carryreq <sélection> <cible>` / `carrying <index>` | ordre de porter ; qui il porte |
| `kosquad <index>` | (hôte) assomme ce membre |
| `insomething <index>` | 0 rien, 1 au lit, 2 en cage |
| `localbed <index>` | (client, test 0.3.4) notre copie de ce membre seule dans le lit libre le plus proche (lit « fantôme » : l'hôte l'a libre) ; `ok inSomething=1` |
| `posturecheck` | (client, 0.3.4) `ok n= freed= remade= keys=` : copies que l'hôte a debout mais couchées (`/down`), au lit ou en cage (`/in1`, `/in2`) ou jouant « porté » sans porteur (`/carriedanim`) ici ; `freed` : sorties d'un lit ou d'une cage par `EnsureFree` ; `remade` : copies revenues après avoir quitté notre jeu |
| `objnear <index> <rayon>` | objets autour (modèle × nombre) |
| `objreq <sélection> <tâche> <nom>` | ordre d'utiliser l'objet le plus proche dont le nom contient ce texte (`_` pour les espaces) |
| `objjob <sélection> <tâche> <nom>` | travail **permanent** (`addJobSelectedCharacters` avec `shift`, ce que fait Maj + clic droit) sur l'objet le plus proche dont le nom contient ce texte ; ex. `objjob 0 87 Ressource_Fer` (minage) |
| `jobmove <index> <de> <à>` | (v0.3.4) glisser un travail du panneau Tâches (`Character::movePermajob` ; `à` = -1 : à la fin) ; chez un client, demandé à l'hôte |
| `whoseidx` | (v0.3.4) chaque index d'escouade vu d'ici : `m` à moi, `s` recrue à personne, `o` à un autre joueur ou à l'hôte |
| `furnparent <nom>` | de quel bâtiment l'objet le plus proche de ce nom est le meuble |
| `trace <handle> <images>` | journalise les corrections de position d'un personnage côté client |

**PNJ, combat, santé**

| Commande | Rôle |
|---|---|
| `spawnnpc <dx> <dz> [index]` | (hôte) crée un PNJ, copie de la personne la plus proche de l'endroit (pas la première de la liste du jeu : loin de l'hôte, c'était un bandit de chez lui qui repartait vers son escouade), à côté du membre 0 (ou de ce membre), arrêté |
| `fight <index> [clé]` | ce membre et le dernier PNJ créé (ou ce perso) se battent, dans les deux sens |
| `wake` / `npcstate [clé]` / `ko [clé]` / `kill [clé]` | le dernier PNJ créé (ou ce perso : plusieurs combats à la fois, `faraway` à 3 clients) : se relève ; état ; K.-O. (minuteur, état inconscient et chute : `knockout` seul ne fait pas tomber) ; mort |
| `floaters` | chiffres de dégâts : `made=` (hôte : faits par `addWound` et envoyés), `forced=` (hôte : faits hors de sa caméra pour un perso près d'un joueur client, crochets `hk_addWound` et `hk_charUpdateView`), `shown=` (client : affichés ici) |
| `inview` | `ok forced=N` : mises à jour de persos passées « dans la vue » (mise à jour complète) par `hk_charUpdateView` depuis le lancement ; chaque ligne `char` de `state` dit aussi `inview=0/1` |
| `perf` | (3b) notre travail par image depuis le `perf` précédent (tick entier, partie session avec `Apply`, `EndFrame` ; le canal de test n'est pas compté), pire tick, durée d'une image et fps ; client : persos de l'hôte suivis, présents, près / loin des nôtres, en sommeil, laissés à la mise à jour lointaine du jeu, doublures près / loin, mises à jour complètes forcées et laissées loin par seconde ; hôte : déplacements des persos près d'un joueur client (voir `movestats`). Même texte que la ligne `sync perf` / `host perf` du journal |
| `standins` | (3b, client) `ok followed= present= squad= near= far= dormant= leftfar= farset= standins_near= standins_far= radius=` : persos de l'hôte connus ici, présents dans notre jeu, à moins de `standin_radius` de nos persos ou plus loin, en sommeil (plus envoyés), donnés à la mise à jour lointaine (`leftfar`, `farset` : adresses résolues), doublures faites ou adoptées ici |
| `copyfix` | (client, 0.3.4) `ok floors= stalled=` : depuis le lancement, copies replacées à l'étage de l'hôte (`ApplyFloor`) et marcheurs de l'hôte vus sans avancer (la copie reste à sa place) |
| `dormant` | (client, 0.3.4) `ok n= found= maxdrift= over20= halts= putback=` : copies en sommeil (plus envoyées), écart maximal (x, z) entre chacune ici et sa dernière position chez l'hôte, combien à plus de 20 unités ; depuis le lancement, combien ont été arrêtées en marche et remises en place (`HoldStill`) |
| `farleave 0\|1` | (3b, client) 0 : comportement 0.3.1 (tout perso de l'hôte en mise à jour complète), pour comparer ; 1 (défaut, remis à chaque fin de session) : ceux que l'hôte n'envoie plus sont laissés à la mise à jour lointaine du jeu |
| `movestats` | (3f, hôte) depuis le lancement, déplacements des persos près d'un perso d'un joueur client : `full=` mise à jour complète, `outofview=` réduite (hors de la vue), `split=` pas de 0,5 s ou plus coupés en morceaux pour rester complets, `longstep=` pas trop longs laissés entiers (plus de 3,2 s), `maxstepms=` plus long pas depuis le dernier relevé de la ligne `host perf` |
| `navcheck <clé\|index>...` | (3f) pour chaque perso, comme le jeu le déplace ici : `view=` (+0x1A9), `every=` (+0xE4), `nav=` sur le navmesh (`HavokCharacter`+0x14), `face=`, `state=`, `path=`, `posvalid=` navmesh sous sa position (`NavMesh::getPositionValid`), `at=` |
| `navat <x> <y> <z>` | (3f) navmesh sous ce point ici : 1 / 0, -1 illisible |
| `wallsnear <index> <rayon>` | (3f) bâtiments autour du membre d'escouade, du plus proche au plus loin, avec leur boîte (`Building`+0x27C centre, +0x288 demi-taille) : `sid\|cx,cy,cz\|hx,hy,hz` ; meubles et petits objets (demi-taille < 50) exclus |

**Progression et apparence**

| Commande | Rôle |
|---|---|
| `stats <index>` | compétences |
| `xp <index> <compétence> <quantité>` | gain d'expérience, comme le jeu le donne (refusé chez un client) |
| `money [valeur]` | argent de la faction (l'hôte peut le fixer) |
| `factions` | nombre de factions en relation avec la faction du joueur, rang, empreinte de toutes les relations (identique hôte / client = synchro) |
| `relation <nom>` | relation de la faction du joueur avec cette faction (`ours`) et l'inverse (`theirs`), alliance, guerre |
| `setrelation <nom> <valeur>` | fixe la relation dans les deux sens (chez l'hôte : comme un événement de faction ; chez un client : simule une dérive locale) |
| `bounty <index>` | primes de ce membre de l'escouade (`faction:montant`), total, crime en cours, heures de prison |
| `givebounty <index> <nom> <montant>` | (hôte) met cette prime sur ce membre |
| `factionsync` | messages de relations / primes reçus et valeurs corrigées (client), envois (hôte) |
| `diplo` | diplomatie de ce jeu : paires de factions (hors joueur), celles en guerre / alliance / paix et leur empreinte `ph`, personnages uniques morts / emprisonnés et empreinte `uh`, villes, variantes et empreinte `th` (identiques hôte / client = synchro) |
| `diplopair <A> <B>` / `setdiplopair <A> <B> <valeur> [war\|ally\|peace\|none]` | relation de A envers B et l'inverse ; la fixer dans les deux sens (chez l'hôte : comme le jeu ; chez un client : simule une dérive locale) |
| `unique <nom>` / `setunique <nom> <0\|1\|2> [player]` | état d'un personnage unique (0 mort, 1 vivant, 2 emprisonné ; `player=1` : par les joueurs) ; le fixer |
| `town <nom>` / `settownowner <ville> <faction>` | faction et variante d'une ville ; changer sa faction (`TownBase::setFaction`) |
| `diplosync` | parties `Diplomacy` reçues et valeurs corrigées (client), envois (hôte), paires changées suivies, uniques, villes |
| `look <nom>` / `lookset <nom> <clé> <valeur>` | résumé de l'apparence ; changer un curseur |
| `editchar` / `editdone` | (client) ouvrir l'éditeur sur son personnage ; valider comme le bouton |

**Escouades**

| Commande | Rôle |
|---|---|
| `squads` | escouades vues par cette machine (`nom: membre,membre \| …`) |
| `squadmove <nom> <autreNom\|new>` | glisser ce portrait dans l'escouade d'un autre membre, ou une nouvelle |
| `squadsfull` | toutes les escouades dans l'ordre de la fenêtre, vides comprises (`\| nom: a,b \| …`) |
| `squadrename <rang> <nom>` | la case de nom de l'escouade à ce rang, comme une frappe |
| `squadnew <nom>` | le bouton « nouvelle escouade », puis son nom |
| `squadorder <de> <vers>` | glisser une escouade (ordre des escouades) |
| `squadlead <nom>` | déposer ce portrait sur le premier de son escouade (il devient chef) |
| `charrename <nom> <nouveau>` | renommer ce perso (fenêtre du personnage) |
| `aiorder <nom> <ordre>` | un bouton de la barre d'escouade pour ce perso seul |
| `aimodes <nom>` | ordres permanents (bits), style, allure |
| `aishared <nom>` | 1 si ce perso est « à personne » chez cette machine |
| `jobsof <nom>` / `jobfor <nom> <tâche> <sujet>` | sa liste de tâches ; lui donner une tâche permanente sur un autre perso |
| `charinfo <nom>` | ce que montrent ses fenêtres : compétences, faim, sang, membre le plus touché |

**Dialogues**

| Commande | Rôle |
|---|---|
| `say <index> <texte…>` / `says` | faire dire une bulle ; bulles rejouées chez le client et la dernière |
| `convo <index> <k>` | (hôte) le k-ième PNJ le plus proche engage la conversation avec ce membre |
| `dialog` / `answer <n\|leave>` | (client) fenêtre de conversation affichée (`actor=`, `turn=`) ; choisir une réponse, ou partir |
| `dialogs` | (hôte) conversations affichées chez les clients (`n=`, `open=`), refus « occupé » du jeu (`busy=`) et des ordres (`orderbusy=`), fermetures forcées (`swept=`), pauses levées (`unpaused=`), refus silencieux d'un autre perso du même joueur (`same=`, 0.3.2), répliques gardées en attente de la réponse d'un joueur (`held=`, 0.3.4) |
| `talksel <k\|name:texte>` | (0.3.2) la sélection en cours (`selectset`) reçoit un seul ordre « parler » vers le k-ième PNJ le plus proche de son premier membre, comme un clic droit avec plusieurs persos sélectionnés ; répond `ok <nombre sélectionné> <nom du PNJ>` |
| `squadcheck` | (0.3.2) chaque perso d'escouade de la session : `nom:propriétaire:présent:dans la faction ici`, avec `n=`, `missing=` (absents ici), `outside=` (présents mais pas dans la faction du joueur ici : pas de portrait) et `conflicts=` (copies liées à un autre perso de l'hôte que le leur ; toujours 0) |
| `recruitnpc [clé]` | (hôte, 0.3.2) le dernier PNJ créé (ou celui-là) entre dans la faction du joueur par le `recruit` du jeu ; répond `ok <nom>` |
| `squadidx <nom>` | (0.3.2) l'index dans l'escouade triée de cette machine (celui de `give`, `taskreq`) du perso de ce nom (`_` pour les espaces) |
| `npcevent <index> <événement> [k\|npc\|dlg]` | (hôte) le k-ième PNJ le plus proche (ou le dernier créé, ou `dlg` : le PNJ de la dernière conversation d'un client, vivant et debout) envoie cet événement de dialogue à ce membre (1 parler, 3 contrôle de garde) ; la réponse finit par `talking=1` quand le dialogue du PNJ a maintenant ce membre pour interlocuteur (`talking=0` : une ligne qui ne fait que parler, ou rien) |
| `talkto <sélection> <k\|npc\|dlg\|name:texte>` | ce membre seul reçoit l'ordre de parler au k-ième PNJ le plus proche (`name:` le plus proche dont le nom contient ce texte, `_` pour les espaces) (ou au dernier créé, hôte ; `dlg` : le PNJ de la dernière conversation d'un client, hôte) |
| `talkclick <sélection> <k\|name:texte> [click\|nogame\|unlinked]` | (client, 0.3.4) ce membre seul sélectionné, clic droit sur le k-ième PNJ debout le plus proche : `click` par la fonction du curseur du jeu (`0x7FB410`, drapeau de clic droit mis le temps de l'appel), `nogame` comme si le jeu ne donnait aucun ordre (le secours seul), `unlinked` un ordre « parler » au handle inconnu de l'hôte (type et endroit seulement). Réponse `ok <nom> sent=<ordres de secours> logged=<lignes> cantalk=<contrôle du jeu ici : 1, 0, -1>` |

**Objets, fouille, contenants**

| Commande | Rôle |
|---|---|
| `drop <index>` | ce membre pose un objet non équipé (répond avec son handle) |
| `pickup <index> <clé>` / `pickupreq <index> <modèle> <x,y,z>` | ramasser (hôte) ; ce qu'envoie le « ramasser » d'un client |
| `groundnear <rayon>` / `ground <clé de l'hôte>` | objets au sol autour ; cet objet est-il au sol ici ? |
| `uidrop <index\|bag:index> <weapon\|armour\|item\|modèle>` | ce membre (ou le sac qu'il porte : `bag:N`) lâche cet objet comme le glisser-déposer vers le monde (`Inventory::dropItem`) ; client : demandé à l'hôte ; hôte, depuis un sac : l'objet quitte d'abord le sac, comme quand la souris le prend |
| `fetchitem <index> <weapon\|armour\|modèle>` | (hôte) prend un objet de ce genre à un autre membre (porté ou non) et le met dans son sac |
| `groundall <rayon> [index]` / `groundcopy <clé de l'hôte>` | objets posés dans le monde autour, `clé\|modèle\|nombre\|x,y,z` ; (client) sa copie de cet objet de l'hôte |
| `groundstats` | compteurs des objets au sol (crochets, relevé, répétitions ; côté client : créés, adoptés, fusionnés, retirés ; 0.3.4 : `scanMoved` réannoncés une fois arrêtés, `scanRolling` relevés où l'objet roulait encore) |
| `invmove <de> <vers> <rang\|main\|worn> [qté] [x] [y] [section]` | ce que fait un glisser-déposer d'inventaire ; `bag:<qui>` désigne le sac porté par ce personnage |
| `loot <cible> [index]` / `lootorder <cible>` | fouille d'un corps telle qu'un client la fait ; le chemin du clic droit |
| `spawncreature <sid> <dx> <dz> [index] [âge]` | (hôte) crée un perso de ce modèle (un animal : `3967-gamedata.base` léviathan, `3993-gamedata.base` chèvre…) près de ce membre, dans la faction d'un PNJ voisin ; il devient la cible de `ko` / `kill` / `wake` / `npcstate`. Répond aussi `unique=0/1` (0.3.2 : un modèle unique est créé quand même, voir `CreateCharacter`) ; marche aussi chez un client |
| `bodysize <cible\|spawned>` | race, échelle, âge, rayon, coque, portée de fouille (`kc::loot`), mort / K.-O. / ragdoll de ce perso |
| `lootstats` | (client) ordres de fouille en attente, fenêtres ouvertes, ordres abandonnés |
| `containerreq <sélection> <nom>` | clic droit sur le contenant le plus proche de ce nom |
| `contake <sélection> <nom>` | dans la fenêtre ouverte, prend le premier objet |
| `contcount <nom\|any>` | piles dans le contenant le plus proche de ce nom |
| `robuststats` | (client) personnages débloqués d'un mur et PNJ lointains replacés depuis le dernier appel |
| `strand <dx> <dy> <dz>` | (client) déplace la copie locale du PNJ le plus proche (dans un mur, sous le sol) |
| `bedreq <i>` | ce membre seul reçoit l'ordre de dormir dans le lit libre le plus proche (tâche 258) |
| `minereq <i>` | ce membre seul reçoit l'ordre d'exploiter la mine la plus proche (tâche 87) |
| `caravan [i]` | caravane la plus proche du membre `i` : marchand sans bâtiment dont l'escouade a des bêtes de somme ; `trader=` `key=` `members=` `animals=` `stock=` (piles de chaque membre) `back=` (piles dans les sacs portés d'où il vend) `packs=` (membres qui en portent un) |
| `bag <qui>` | le sac porté par ce personnage (`squadN`, `spawned` ou clé) : `ok <modèle> <piles> sid:qté...` |
| `baginv <qui>` | le sac porté, chaque pile avec sa place : `ok <n> sid:qté@section:x,y ...` |
| `invitems <qui>` | les piles du perso dans l'ordre que compte `invmove` : `ok <n> sid:qté@section:x,y ...` |
| `wearbag <index> [nom\|sid]` | (hôte) un sac neuf (données de type 0x2E) mis dans la case sac de ce membre, comme la fenêtre d'inventaire ; rien s'il en porte déjà un qui correspond, sinon l'ancien est lâché au sol d'abord (noms affichés en français : préférer le sid, ex. `1498-gamedata.base`) |
| `bagbonus <index>` | le sac porté par ce membre : chaque section avec son bonus de pile (`min=`, `mult=`, `one=`) et ce que son premier objet peut empiler (`sid<=max`) |
| `invcheck [all] [repair]` | contrôle des inventaires des persos du joueur (`all` : tous les persos actifs) et de leurs sacs (`kenshi::CheckInventories`) : objet libéré encore listé, objet listé hors de toute section, dans deux sections ou deux inventaires, objet d'une section absent de la liste ou marqué « hors inventaire ». `ok clean inventories=N items=M repairs=R each=0\|1` ou `err problems=K … \| <problème>; …` (chaque problème aussi dans le journal, `invcheck:`). `repair` : retire ce qui peut l'être sans toucher un objet libéré |
| `invcheck each <0\|1>` | en plus, contrôle des deux inventaires après chaque déplacement rejoué par l'hôte (ou par `invmove`) : lignes `INVCHECK after a move/a drop/a swap: clean` ou le problème |
| `caravanbeast [i]` | sa première bête de somme vivante devient la cible de `kill` / `npcstate` |
| `caravanopen [i]` | le membre `i` va près du marchand ambulant et ouvre le commerce (appel du jeu) |
| `squadanimals` | membres de l'escouade qui sont des animaux : `index:clé:piles` |
| `tradegui` | type de fenêtre de commerce en attente dans l'interface (0 = aucune) |
| `animalshop <i> [rayon] [prix]` | la boutique d'animaux du jeu (ce que fait « T'as des animaux à vendre ? ») entre le membre `i` et le PNJ (non animal) le plus proche, avec les animaux hors escouade à moins de `rayon` (500) ; chez l'hôte, pour le perso d'un autre joueur, elle s'ouvre chez ce joueur |
| `animalshopstate` | fenêtre d'animaux ici : `window` `visible` `entries`, offres gardées et animaux vendus (hôte), offre ouverte (client), puis `clé:prix[:x coché]` par animal |
| `animalbuy <n> [n…]` | coche ces animaux dans la fenêtre ouverte et appuie sur son bouton de validation ; `cats avant->après window=` |
| `prospect <i>` | sélectionne le membre `i` seul et appuie sur le bouton PROSPECT (par le crochet) |
| `prospectstate` | fenêtre de prospection ici : `visible`, et la position, la compétence et le nom qu'elle montre |
| `merchants [index]` / `tradeopen <index> <nom\|any> [near]` | marchands (avec comptoirs : `nom(comptoirs)[piles]`, un garde logé dans une caserne a des râteliers vides, `faraway` prend le plus garni) à 1500 unités du membre 0 ou de ce membre : `nom(comptoirs)` ; le jeu demande une fenêtre de commerce entre ce membre et le plus proche (`near` : il est d'abord mis à côté, hôte) |
| `cage <i> [off]` | (hôte, lot D) met le membre `i` dans la cage la plus proche (300 m), ou l'en sort |
| `chain <i> [off]` | (hôte, lot D) l'enchaîne à la manière du jeu (menottes créées) / le libère |
| `enslave <i> <0-3>` | (hôte, lot D) état d'esclave : 0 non, 1 esclave, 2 en fuite, 3 ancien |
| `captive <i>` | (lot D) `in=` (2 = en cage), cage, `chained`, `slave`, `slaveof`, évadé, enlevé, peine, position, nombre de captifs suivis |

**Carte, minicarte, repères, pings**

| Commande | Rôle |
|---|---|
| `mapfeed` | le flux de carte de cette machine (hôte : construit, client : reçu) : âge, joueurs, persos (`nom:owner=:av=:x=:z=`), escouades hostiles (`threat:kind=:n=:x=:z=`) |
| `mapscene carte` | ce que la carte dessine : ouverte (sinon `why=` : la raison), bornes, chaque perso avec propriétaire, couleur (`col=or`…), position, et à l'écran si la carte est ouverte (`sx=`, `sy=`, `vis=`) ; escouades hostiles |
| `mapscene minicarte` | centre, zoom, rotation, coin, position libre (`fx=`, `fy=` : fractions de l'écran, -1 : le coin) et taille (`size=`), où l'overlay l'a dessinée (`drawn=`, `pos=x,y`, `r=`, `free=`, `drag=`, `screen=`), persos et ennemis dans le cercle |
| `mapscene tetes` | noms au-dessus des têtes : origine de rendu (`origin=`), noms du jeu à colorer (`tags=`), cadres lus (`frames=`), couleurs remises / rendues au jeu / ratées (`recoloured=`, `restored=`, `failed=`) ; par perso : propriétaire, couleur, principal (`main=`), qui écrit le nom (`label=jeu` ou `overlay`, et pour l'overlay pourquoi : `why=off|notag|notupdated|nowidget`), nom du jeu à la bonne couleur (`tint=`), encadré (`frame=`), à l'écran ou non, point projeté (même hors écran) ; puis chaque cadre (`frame:owner=…:x=…:y=…:w=…:h=…`, pixels MyGUI) |
| `mapscene barre` | cadres de la barre d'escouade (`frame:owner=:col=:x=:y=:w=`) et cellules de portrait suivies |
| `mapscene pings` | pings dessinés ici (id, joueur, type, position, âge) |
| `mapconv` | conversions de l'overlay : tampon (`bb=`), client de la fenêtre, DPI, vue MyGUI (`gui=`), facteurs `fx`/`fy`, vue couverte, noms et cadres au-dessus des têtes dessinés (`heads=`, dont cadres autour des noms du jeu `nameFrames=`) et minicarte dessinée, carte détectée / dessinée et nombre de marqueurs ; cadres relus maintenant (`frame:owner=:raw=:conv=`) et dessinés à la dernière image (`drawn:owner=:x=:y=:w=:h=`) ; image de la carte (`map:raw=:conv=:drawn=` ou `map:why=`) |
| `mapui open\|close\|maptab\|state` | l'écran de carte du jeu par son bouton MAP (`ShortcutMapButton`, clic injecté dans MyGUI) ; `close` clique la croix de la fenêtre de gestion, ou, quand elle est hors de l'écran (fenêtre du jeu plus petite que celle de gestion) ou couverte, cache la fenêtre comme le fait sa croix (`MyGUI::Window::setVisible(false)`, réponse `ok hidden`) ; `maptab` choisit l'onglet carte ; `state` : `open=`, `window=`, `why=` |
| `mapproj <x> <z>` | `MapScreen::worldToMapCoords` du jeu comparé à notre projection (`game=` / `ours=`, taille de l'image) |
| `ping <x> <z> [type]` | ce joueur pinge ce point (0 aller ici, 1 danger, 2 butin, 3 à l'aide) ; `err` si trop tôt |
| `pings` | pings vivants dans la session (`id=:owner=:kind=:x=:z=:age=`) |

**Portes et serrures (lot A)** — `<qui>` : index dans l'escouade (autour de qui chercher) ; `<quoi>` :
`door` (la porte la plus proche), `lock` (le meuble à serrure le plus proche) ou une partie du nom
(`_` pour les espaces).

| Commande | Rôle |
|---|---|
| `doors <qui> <quoi>` | les portes ou serrures à moins de 600 unités : `nom/d<état>/f<drapeaux>/L<niveau>@distance` |
| `doorstate <qui> <quoi>` | état de la plus proche : type, état, drapeaux, niveau, ouverture |
| `doorset <qui> <quoi> open\|close\|lock\|unlock\|break\|fix` | change la plus proche comme le ferait le jeu de l'hôte |
| `doorlocal <qui> <quoi> open\|…` | la même chose sans `HostCallScope` : ce que tenterait le jeu d'un client (doit être refusé) |
| `doorbutton <qui> <quoi> open\|lock` | clic sur le bouton du panneau de la porte (chez un client : part à l'hôte) |
| `doororder <qui> <tâche> <quoi>` | ordre comme un clic droit : 72 ouvrir, 73 fermer, 76 crocheter, 77 verrouiller, 78 déverrouiller, 81 défoncer |
| `doorsknown` | portes envoyées (hôte) ou connues ici (client), nombre d'applications réussies, et `givenup=` : portes que le jeu du client n'a pas voulu tourner en 3 essais, laissées jusqu'au prochain changement chez l'hôte (0.3.2) |

**Combat à distance (lot C)**

| Commande | Rôle |
|---|---|
| `rangedlist [rayon]` | personnages ayant une arme à distance prête près du membre 0 : `nom\|clé\|combat\|distance` |
| `shoot <tireur> <cible>` | (hôte) le tireur (index d'escouade ou clé de handle) tire une fois sur la cible ; réponse : projectile créé et son orientation |
| `shots` | compteurs : tirs envoyés (hôte), refaits / échoués (client), visées et tourelles imposées, `nogun` / `noturret` / `oriented` |
| `turrets [rayon]` | tourelles près du membre 0, triées par endroit : `type@x,z>point visé` |
| `turretaim <index> <x> <y> <z>` | (hôte) la tourelle n° index tourne vers ce point |
| `rangedaim <personnage>` | état du combat à distance d'un personnage ici : mode, état, point visé, cible, arme |

**Bâtiments (lot E)**

| Commande | Rôle |
|---|---|
| `buildtypes <nom>` | modèles de bâtiments dont le nom contient ce texte (`sid=nom`) |
| `buildplace <sid> <dx> <dz> [lacet°] [membre] [force]` | une pose comme le mode construction, à côté du membre 0 ou de ce membre (client : demandée à l'hôte ; hôte : bâtie et annoncée). Vérifiée d'abord comme le mode construction vérifie un endroit : `err invalid spot: <raison> at x,z` et rien n'est posé. `force` (client seulement) saute cette vérification locale pour tester celle de l'hôte |
| `buildplaceat <sid> <x> <z> [lacet°] [force]` | la même chose à une position du monde |
| `buildcheck <sid> <dx> <dz> [lacet°] [membre]` / `buildcheckat <sid> <x> <z> [lacet°]` | seulement la vérification : `ok valid at x,z` ou `err invalid spot: <raison> at x,z` |
| `groundat <x> <z>` | le sol à cet endroit ici (sous l'eau, -99 = inconnu), la hauteur de référence du mode construction (surface de l'eau 100) et `land` / `water` / `unknown` |
| `chatlast [n]` | les n dernières lignes du chat ici, avis de l'hôte compris (` \| ` entre les lignes) |
| `toastlast` | (v0.3.4) les 8 derniers avis affichés en haut de l'écran ici (« Ce personnage appartient à un autre joueur. »…) |
| `furnplace <sid> <nom du bâtiment> <dx> <dz>` | un meuble dans le bâtiment à nous le plus proche de ce nom (position relative au bâtiment) |
| `buildlist [nom] [rayon] [membre]` | bâtiments autour du membre 0 (ou de ce membre) : `sid@x,y,z:avancement/drapeaux` (1 terminé, 2 en pause, 4 démontage) |
| `buildcount` | bâtiments suivis par la session, et combien sont trouvés ici |
| `buildprogress <nom> <quantité>` | (hôte) avancement d'ouvrier sur le chantier à nous le plus proche |
| `builddismantle <nom>` / `buildbuy <nom>` | confirme le démontage / l'achat comme la fenêtre du bâtiment (un client le demande à l'hôte) |
| `buildforsale [nom]` | le bâtiment à vendre le plus proche et son prix |
| `buildinfo [nom|sid@x,y,z] [membre]` | le bâtiment le plus proche (ou celui-là) : `forsale=0/1 ours=0/1 price=` |
| `buildreq <membre> <tâche> [nom]` | ce membre seul reçoit l'ordre de travailler sur le chantier à nous le plus proche, comme un clic droit (`newPlayerTaskSelectedCharacters`) |
| `givemoney <n>` | (hôte) n cats de plus pour la faction du joueur |
| `itemtypes <nom>` | modèles d'objets (hors bâtiments) dont le nom contient ce texte |
| `giveitem <sid|nom> <n> <membre>` | (hôte) n objets neufs (fabrique du jeu) dans l'inventaire de ce membre ; par nom : les objets (types 2-4) d'abord, chaque candidat essayé jusqu'à ce que la fabrique en fasse un |
| `invcount <membre|all> <sid|nom>` | combien de ces objets ce membre (ou toute l'escouade) porte |
| `ownidx [id]` | indices d'escouade (ordre de ce jeu) des persos du joueur `id` (défaut : le mien) |

**Météo**

| Commande | Rôle |
|---|---|
| `rollweather` | chaque région tire une nouvelle météo |
| `setweather <région> <saison> <météo>` | (hôte) force une météo |
| `weathers <fichier>` | régions, groupes d'effets et météos possibles |
| `fxhurry` | chaque groupe d'effets en place un tout de suite |

**Exports**

| Commande | Rôle |
|---|---|
| `state <fichier> [rayon]` | écrit tout ce que voit cette instance (session, heure, météo, effets, chaque personnage avec position, animations, inventaire, santé). C'est la base des comparaisons |
| `anims <fichier>` | animations jouées par chaque membre de l'escouade |
| `animstats` | (client) corrections de l'horloge d'animation depuis le dernier appel |

**Atelier** (`plugin/workshop.cpp`)

| Commande | Rôle |
|---|---|
| `research` | la recherche telle que cette instance l'a : `desk=` niveau du banc, `finished=` nombre, `fh=` empreinte des technologies connues, `queue=sid:avancement|…` |
| `researchknown <sid>` | `ok 1` si la technologie est connue ici |
| `researchenabled [type]` | ce qui est débloqué (`enabledObjects` ; type 0 : bâtiments du menu de construction) : nombre, `eh=` empreinte, puis les sids |
| `researchpick [n] [cost]` | jusqu'à n technologies dont les prérequis sont remplis ici, ni connues ni en file ; avec `cost`, seulement celles que les bancs peuvent payer |
| `workreq <index> <sid> [shift]` | ce membre seul travaille (tâche 87, comme un clic droit) l'exemplaire de ce modèle le plus proche du membre 0 (4000 unités) : mine, filon, banc, établi ; chez un client, l'ordre part à l'hôte. `shift` (v0.3.4) : Maj tenue pendant le clic (la touche du jeu, `kenshi::ScopedShift`), donc un **travail permanent** comme en solo ; la réponse donne aussi le nombre de travaux avant et après (`jobs 0->1`) |
| `researchreq <queue\|cancel> <sid>` | le bouton ajouter / retirer de la fenêtre Recherche (client : devient une demande à l'hôte) |
| `researchprogress <quantité>` | (hôte) le travail des chercheurs, accéléré : termine la technologie en tête |
| `blueprintreq <index> <nom>` | « apprendre » un plan que porte ce membre (client : demande à l'hôte) |
| `craftlist [n]` | ce que la faction sait fabriquer (`craftableThings`), par sid |
| `machine <nom>` | la machine la plus proche du membre 0 : `ops=` opérateurs/max, `names=`, `flags=` (1 marche, 2 batterie, 4 établi, 8 répéter, 16 générateur, 32 batterie, 64 au joueur), `power=`, `stored=`, `prog=`, `prod=`, `crafts=objet:avancement,…`, `town=` totaux d'énergie de sa ville |
| `machineinv <nom>` | son inventaire : piles, quantité totale, empreinte `ih=` |
| `machines` | chaque machine que la session connaît (hôte : envoyées ; client : celles de l'hôte), lue dans ce jeu, triée : `clé\|found ou MISSING\|ops=a/b\|on=0/1\|prod=x\|farm=étape,récoltés/plants,pousse,pourriture\|crafts=n` (`not here` si introuvable ici) |
| `crops [nom]` | la ferme la plus proche du membre 0 : `stage=`, `plants=`, `harvested=`, `grown=`, `died=`, `cleared=`, `hydro=`, `idle=` (pas de travail pour l'instant), `ops=`, `prod=` ; chez un client `host=` l'état reçu de l'hôte |
| `storage <nom\|any>` | l'objet à inventaire (stockage, coffre, machine) le plus proche du membre 0 : `stacks=`, `total=`, `ih=` empreinte, `items=sid:qté,…` (trié) |
| `workjob <index> <sid\|none> [tâche]` | ce membre seul reçoit un travail **permanent** (panneau Tâches, `addJobSelectedCharacters` avec `shift`) sur l'exemplaire de ce modèle le plus proche du membre 0, ou sans cible (`none` : transport 88, tout faire tourner 89) ; tâche 87 par défaut ; chez un client l'ordre part à l'hôte |
| `machineselect <nom>` | un clic sur cette machine (`Building::select`, sans demande à l'hôte) : `ours=` (à la faction du joueur ici), `windows=avant->après`, `panel=1` si son panneau d'inventaire est ouvert |
| `machinereq <nom> <addcraft base [matériau]\|removecraft i\|repeat 0/1\|power 0/1\|battery 0/1>` | hôte : exécuté ; client : les interrupteurs passent par les boutons du panneau (crochetés), les ordres de fabrication par la file des demandes comme les boutons de la fenêtre |
| `machinegive <nom> <objet> <n>` | (hôte, bac à sable) n objets neufs dans l'inventaire de cette machine |

## 4. Après les tests : remettre la configuration du joueur

1. Fermer les instances de test : uniquement celles lancées par le harnais, jamais une partie du
   joueur.
2. Remettre le plein écran : copier `kenshi.cfg.before-kenshicoop-test` sur `kenshi.cfg`, dans le
   dossier de Kenshi.
3. Dans `KenshiCoop.ini` : `[debug] commands=0`, et `steam_loopback=0` s'il avait été activé.

## 5. Captures d'écran

Pour regarder une instance de test, ne capturer **que la fenêtre de Kenshi** (`PrintWindow` sur sa
fenêtre), **jamais l'écran entier** : le joueur utilise le même PC pendant les tests. Sans mettre
non plus la fenêtre du jeu au premier plan.
