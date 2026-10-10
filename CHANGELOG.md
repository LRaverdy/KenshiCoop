# Journal des changements

Du plus récent au plus ancien. Chaque entrée correspond à un commit (son identifiant entre
parenthèses). L'état détaillé de chaque fonctionnalité est dans
[docs/FONCTIONNALITES.md](docs/FONCTIONNALITES.md).

## En cours (pas encore commité)

- Nouveau document docs/INSPIRATION_PZ.md : ce que le multijoueur de Project Zomboid fait (idées seulement), et les améliorations classées pour KenshiCoop.
- **Soak de 20 min (13:17–13:37, vitesses 1/2/3) : 3 échecs corrigés** (aucun plantage). Non testé en jeu.
  - *Membres d'escouade manquants chez le client, et des dizaines de corps en double* : quand un perso de l'escouade meurt, le jeu le range dans l'escouade `__DEAD_SQUAD__`, ce qui change son handle (conteneur seul : type, index et serial restent). Chez l'hôte, le nouveau handle part au client (`Rehandle`). Mais le client rejoue la mort et son propre jeu déplace aussi sa copie : son handle local change, l'alias du mod pointe sur un handle mort, « stand-in … is gone here: it can be recreated », et le mod recréait une doublure… qui mourait à son tour (54 copies du membre 4 chez le client en 20 min, le perso du client lui-même jamais recréé : « local order dropped: unknown character », « no such squad member »). Le client retrouve maintenant la copie déplacée par son serial (`FindMoved` : même type et serial, autre conteneur, un seul candidat) et la suit (`Exists`, et au retour d'une image hors jeu). Journal : « our copy … changed squads here (now …): followed ».
  - *Escouade décalée en pause (17,3 / 11,5 / 1,4 unités à p2_3)* : ni la vitesse ni la pause : chez le client, `latest` et `rendered` étaient exactement la position de l'hôte. Les trois étaient **à terre** (drapeau 4 des deux côtés) : deux K.-O. tombés là où était leur copie en pleine frappe, avant que le déplacement vers la place de l'hôte prenne, et jamais remis (seuil de 25 unités) ; ils sont ensuite morts là, 11,5 unités à côté jusqu'à la fin. Un corps K.-O. à plus de **5** unités (au lieu de 25) de celui de l'hôte est relevé et recouché à sa place, 3 fois au plus tant qu'il reste à terre. Le test traite aussi en corps au sol un perso à terre jambe brisée (drapeau 4 chez l'hôte, `vflags` 0 : 1,4 unité).
  - *Heure de jeu à 0,01 h* : vraie dérive, pas un arrondi (l'heure est écrite au millième ; écart stable à 0,002 h puis 0,010 h dès p11_30 et jusqu'à la fin). Le client ne recalait jamais son heure : il suivait seulement la vitesse et la pause, chacune reçue un peu en retard. `ClockSync` (`common/include/kc/clock.h`) : le client estime l'heure de l'hôte (dernière reçue + temps écoulé + demi aller-retour) et tourne ±2 % (±5 % au-delà de 0,006 h) tant que l'écart dépasse 0,003 h, jusqu'à moins de 0,0005 h. Journal : « clock: … behind the host's: running +2 % ». Le test `soak` accepte donc une vitesse du client à 6 % de celle de l'hôte. Test unitaire `TestClockSync`. Pas de changement de protocole.

- **Marchands ambulants, sacs à dos et animaux (protocole 33, pas encore lancé en jeu)** : un marchand sans bâtiment (caravane, nomade) vend depuis les sacs à dos portés par son escouade (constructeur de `ShopTrader`) ; le mod synchronise maintenant le contenu du sac porté par chaque personnage suivi, joueurs compris (nouveau message `BagBind`, le sac est un inventaire comme un autre), et l'hôte ouvre le commerce avec ces sacs comme comptoirs (`TradeCounter.ownerNetId`). Achats et ventes payés chez l'hôte, même pendant que la caravane marche ; la fenêtre se ferme si le marchand ou une bête du stock est à terre, meurt, se bat, s'éloigne ou n'est plus suivi. Vol dans le sac ou l'inventaire d'une bête de somme de PNJ depuis un client : crime décidé chez l'hôte (fenêtre de pillage locale, plus de faux commerce sur une bête). Un personnage qui rejoint l'escouade juste après la réponse d'un seul joueur dans une conversation (animal acheté, recrue) est à ce joueur ; un animal donné à un joueur passe dans l'escouade de son personnage (il le suit). Une seule reprise des personnages au retour d'un joueur, celle de crash-rejoin (par compte Steam, nom sans Steam). Commandes de debug `caravan`, `caravanbeast`, `caravanopen`, `squadanimals`, `bag`, `invmove bag:<qui>` ; expériences `caravan` (« marchand ambulant : ... ») et `pets` (« animaux : ... ») ; test unitaire `TestTravellingTrade`.
- **Administration de l'hôte** (fenêtre Multijoueur, section sous la liste des joueurs) : pour chaque joueur et pour « Tout le monde » (l'hôte compris) : **mode dieu** (case à cocher, retenu par joueur — compte Steam, sinon nom — et réappliqué 5 fois par seconde, donc il tient aux changements de zone et aux reconnexions), **TP** (joueur → hôte, hôte → joueur, joueur → autre joueur, joueur → point marqué = dernier clic droit de déplacement de l'hôte, ou coordonnées), **XP** (une compétence ou toutes, en points ou en niveaux, par la fonction du jeu `increaseStat` : réplication par la synchro habituelle), **soigner** (et réveiller un K.-O.), **argent commun**. Confirmation par un second clic pour un TP de perso à terre et au-delà de 1000 points ou 10 niveaux. Chaque action est journalisée (« admin: … ») et le joueur concerné reçoit un message en français (« * L'hôte t'a téléporté près de lui. »). Commandes équivalentes `admin god|tp|xp|heal|money|list` (console et canal de debug, `admin state` pour les tests) ; `god`, `heal`, `xp <id> <n>` passent par là. Refusé sur un client. Nouveau `Session::SendNotice` (un `Chat` de l'hôte à un seul joueur, message déjà existant : **pas de changement de protocole**). Nouveau module `common/src/admin.cpp` (syntaxe, calcul de l'XP du jeu, registre du mode dieu) et test unitaire `TestAdmin` ; expérience `admin` réécrite (dieu sous les coups et après reconnexion, XP et niveaux identiques chez le client, trois TP, soin, argent). Faits moteur : formule et limite de 20 d'`increaseStat`, point du sol reçu par `playerMove` (docs/MOTEUR.md). Aucune nouvelle fonction du moteur. Pas encore lancé en jeu ; la faim n'est pas remise à zéro (échelle inconnue).
- **Sécurité des acteurs (bug critique)** : quand le joueur 2 voulait dormir dans un lit ou parler à un PNJ, c'est le perso de l'hôte qui y allait. Cause trouvée au désassemblage : `unselectAll` (`0x7F8DA0`) resélectionne le perso principal et `objectSelected(…, false)` refuse de retirer le dernier ; « tout désélectionner puis sélectionner le perso du client » laissait donc le perso principal de l'hôte sélectionné, et l'ordre partait aux deux ou au plus proche (lit, conversation : `addTaskNearestSelectedCharacter`). Sûr seulement quand l'hôte n'avait rien sélectionné (comme dans les tests). Maintenant : sélection faite **exactement** des acteurs et vérifiée (sinon refus sans rien exécuter), puis sélection, escouade affichée, panneau de détails et `hand` principal de l'hôte remis dans le même appel (`kenshi::SelectExactly` / `WithSelection`, aussi pour `KeepSelection`, ramasser, la caméra des tests et `selectset`) ; témoin `SAFETY:` si un perso de la sélection de l'hôte reçoit quand même une tâche. Contrôle d'autorité central (`session_authority.cpp`, table `kMessageRules`) avant chaque gestionnaire : l'acteur nommé par une demande (ordre, contenant, apparence, dépôt d'objet) doit être un perso de **ce** joueur (un dépôt depuis un sac à dos porté, `BagBind`, est vérifié au nom de son porteur : `Session::DropActor`), revérifié à l'exécution ; refus journalisés (`auth: … refused`, `refused: actor N not owned by player P`), comptés par joueur et par règle (décroissance 60 s), réponse en français. Le client filtre avec la même table (« Action refusée : ce personnage n'est pas le tien. »). **Cible vérifiée** pour chaque numéro de tâche (`TaskTargetAllowed`) : corrige le plantage de l'hôte de `stress4` (`npcreq <perso> 2 a` : « construire » via addTaskNearest sur un PNJ → plantage dans `Character::addJob`) ; handle périmé ou destination qui n'est pas un bâtiment écartés ; changement d'escouade seulement vers celle d'un joueur. **Protocole 33** : nouveau message `Result` (90, H→C : fait / rejeté avec raison et texte ; le client retire la tâche qu'il avait affichée d'avance). Tests unitaires `TestTaskTargets`, `TestActorSafety` ; expérience `actorsafety` et commandes de debug `selected`, `charstate`, `actorstats`, `results`, `forgeorder` (pas encore lancée en jeu).
- **Arrivées simultanées : file d'attente** (essai `join4` du 10/10, 13:57) : 3 clients rejoignant en même temps, seul Joueur4 entrait ; Joueur2 et Joueur3 étaient retirés 5 min plus tard (« joining took too long »). Cause : l'hôte envoyait une sauvegarde à Joueur2 et Joueur3 (13:57:04.7), puis Joueur4 arrivait (06.5) ; l'hôte créait son personnage et sauvegardait de nouveau pour lui seul. L'empreinte du monde (les handles de l'escouade) changeait : la sauvegarde des deux premiers ne correspondait plus au monde de l'hôte, ils restaient en chargement (« world ready: 8 player characters », empreinte différente de celle attendue) jusqu'au délai. Les joueurs passent maintenant **un par un** : sauvegarde neuve pour le joueur dont c'est le tour (avec les persos de tous ceux arrivés avant), téléchargement, chargement, éditeur de personnage ; les autres attendent, connectés, et voient « File d'attente : position 2/3 — en attente de Joueur2 (création du personnage)… », mis à jour en direct ; l'hôte voit la file (panneau, fenêtre Multijoueur, `status`). Le délai de 300 s part du début du tour. Fin du tour : dans le monde et éditeur fermé, ou départ / plantage / exclusion (le suivant commence aussitôt) ; un joueur qui quitte la file la fait avancer. Plusieurs éditeurs ouverts en même temps : la pause dure jusqu'à la fermeture du dernier (« nobody is in the character editor any more: the game resumes »). **Protocole 33** (nouveau message `JoinQueue` = 72). Nouveau test unitaire `TestJoinQueue`. `join4` (`coop_test.py`) attend la file (positions vues, tous entrent l'un après l'autre, un seul chargement à la fois, file affichée chez l'hôte) ; `setup_multi` ferme l'éditeur de chaque joueur à son tour. `status` (debug) donne `queue=`. Pas encore lancé en jeu.
- **Diplomatie et réputation (branche `diplomacy`)** : audit du lot B (relations de la faction du joueur, primes : déjà là) et ajout de ce qui manquait. Nouveau message `Diplomacy` (85, **protocole 33**) en trois parties : relations entre deux factions qui ne sont pas celle du joueur (seules les paires changées depuis que l'hôte héberge : guerres, alliances déclarées après la mort d'un chef, un dialogue, une campagne), personnages uniques (`UniqueNPCManager` : chef mort, vivant, emprisonné, par les joueurs ; les états du monde du jeu n'en dépendent qu'avec les relations du joueur) et villes (faction propriétaire, variante : ville prise, détruite). Le client les impose à son jeu toutes les 3 s (`TownBase::setOverride` / `setFaction` pour les villes, table des uniques écrite comme au chargement). Nouvelle fenêtre **Diplomatie** (Ctrl+Shift+F, en français, hôte et clients) : relations de la faction du joueur avec l'état allié / neutre / ennemi / en guerre calculé comme le jeu (allié : alliance ou ≥ 50, ennemi : ≤ −30), rang, réputation, primes et peines de l'escouade (que le jeu du client n'a pas), guerres, chefs, villes. Nouvelles en français dans le panneau des deux côtés (« Diplomatie : … vous considère maintenant comme ennemi », « Prime : … est recherché par … », « Monde : Tinfist est mort (de la main des joueurs) », « Monde : Squin appartient maintenant à … »). Trois fonctions ajoutées à `kFunctions` (prologues vérifiés) : `operator[]` de la table des uniques `0x349950`, `TownBase::setOverride` `0x9FE7A0`, `TownBase::setFaction` `0x9287D0` ; faits moteur dans docs/MOTEUR.md (« Diplomatie »). Test unitaire `TestDiplomacy` (et cas `TestWire` / `TestFuzz`), expérience `diplomacy` (`kctest_town`, vérifications « diplomatie : … ») et commandes `diplo`, `diplopair`, `setdiplopair`, `unique`, `setunique`, `town`, `settownowner`, `diplosync`. Rien de tout cela n'a encore tourné en jeu.
- **Carte, minicarte, repères des joueurs et pings** (**protocole 33**, nouveaux messages `MapMarkers` = 82 et `MapPing` = 83 (80 et 81 au départ, renumérotés : 80 est `BagBind`) ; pas de version publiée) : une couleur par joueur partout (`kc::PlayerColor`). Sur la carte du jeu (M), tous les persos de tous les joueurs, même lointains, et en rouge les escouades hostiles qui nous visent (raid ou vague d'attaque du jeu contre notre faction, escouade en combat contre un des nôtres, ennemis à ~250 m), avec légende et infobulles (nom, joueur, distance). Minicarte ronde (fond : la carte du jeu, nord en haut ou tournante, zoom molette et + / −, coin au choix, Ctrl+Shift+N). Repère au-dessus de la tête des persos des joueurs, cadre coloré de leur portrait dans la barre d'escouade. Pings (clic molette ou Alt+clic sur la carte, la minicarte ou le sol ; Maj / Ctrl pour danger, butin, à l'aide) chez tout le monde pendant 10 s, 1 toutes les 0,5 s et 5 au plus par joueur. L'hôte envoie le flux 3 fois par seconde ; réglages dans la fenêtre Multijoueur (« Affichage »), retenus dans `KenshiCoop.ini`. Cinq fonctions du jeu ajoutées à `kFunctions` (projection de la carte, couleur des points, campagne d'une escouade, mise à jour et destruction d'un portrait). Test unitaire `TestMap`, expérience `map` et commandes `mapfeed`, `mapscene`, `mapproj`, `ping`, `pings`. Pas encore lancé en jeu.

- **Client qui plante puis revient** : l'hôte ne coupait pas seulement les jeux plantés mais aussi ceux figés plus de 5 à 15 s (chargement d'une zone) ; un fil de maintien répond maintenant au réseau quand le jeu est figé (au plus 90 s), un jeu tué est toujours vu en 5 à 15 s. Au départ d'un joueur (propre ou non), l'hôte efface tout ce qui porte son id (ordres, échanges d'inventaire, conteneurs, commerce, bâtiments, portes, conversations : un nouveau joueur reprenant l'id pouvait hériter d'un conteneur « ouvert » ou d'un commerce), arrête ses persos là où ils sont et les lui rend s'il revient. Un joueur relancé avant la coupure était renommé « Nom 3 » (le nom était vérifié avant de fermer l'ancienne connexion) et pouvait recevoir un nouveau personnage : l'ancienne connexion est fermée d'abord ; sans Steam, même nom sur une connexion muette depuis 2 s. Le jeu d'un client en session ne sauvegarde plus (il aurait écrit le monde de l'hôte sur une partie du joueur). Nouveau test unitaire `TestCrashRejoin`, nouvelle expérience `crashrejoin` (pas encore lancée en jeu). Pas de changement de protocole.

- **Tests à 4 joueurs (`stress4`, `join4`)** : `setup_multi` lance l'hôte + N clients (faux Steam id et nom distincts, arrivée l'un après l'autre ou en même temps, éditeur fermé). `stress4` : clients répartis dans des régions lointaines qui changent, clics en rafale à graine par client, mode dieu, deux clients sur le même corps, départ / retour d'un client, objet marqué compté partout (pas de duplication), mémoire et temps de frame. `join4` : 3 arrivées simultanées, chacun voit tout le monde, puis départs. Le banc vérifie la RAM libre avant chaque lancement (< 2 Go : arrêt propre) et tue les jeux qu'il a lancés sur toute erreur ; commandes envoyées à un même jeu sérialisées (plusieurs fils). Côté mod (debug seulement, `commands=1`) : variable `KC_PLAYER_NAME` (nom du joueur pour ce jeu, non écrit dans l'ini), commande `ownidx [id]` (indices d'escouade des persos d'un joueur), `name=` dans l'état dumpé. Rien ne supposait un seul client côté session (ids 2 à 8, `kMaxPlayers` = 8). Pas de changement de protocole. Pas encore lancé en jeu.
- **Poses de bâtiments vérifiées comme le mode construction** (farlong : la tente du client, téléporté à 54612,40576, avait été bâtie dans un lac d'acide, « built … at 54612.5,100.0,40575.9 »). Lu dans le jeu : le clic du mode construction (`0x4E27E0`) n'accepte un endroit qu'après `PreviewBuilding::placementVerification` (`0x4DD240`) et `buildingPlacementUpdate` ; un client n'envoie donc que des poses que son jeu a acceptées (seuls les aperçus acceptés arrivent à `createBuildings`). L'hôte refait maintenant ces vérifications sur chaque pose demandée par un client (`CheckPlacement`) : ville (même calcul que le jeu : `getNearestTown`, `withinBordersRange`, `getNearestWithinItsRadius`), intérieur d'un bâtiment (`UtilityT::isIndoors`), autre bâtiment à moins d'1 m, sol dans l'eau ou l'acide (`Footprint::isGroundValid` : sol sous 98, surface à 100 ; `UtilityT::getTerrainHeight`), pente (normale < 0,2). Refusée : rien n'est bâti nulle part, journal « [X] placement of … refused: invalid spot (in water or acid …) », le joueur reçoit « Impossible de construire X ici : dans l'eau ou l'acide. ». Six fonctions du jeu ajoutées à la table (prologues vérifiés au démarrage, `check_functions.py`). La commande `buildplace` passe par la même vérification (`err invalid spot: <raison>`) ; nouvelles commandes `buildplaceat`, `buildcheck`, `buildcheckat`, `groundat`, `chatlast`, option `force` (client : laisse l'hôte refuser). Tests : `build`, `construct`, `farlong`, `soak` cherchent un endroit que le jeu accepte (`place_valid`), les TP lointaines (`far`, `fartp`, `farlong`, `soak`) visent la terre ferme (`teleport_dry`) ; nouvelle expérience `placevalid` (le lac d'acide refusé par le client et par l'hôte, rien de bâti, un endroit sec accepté) ; test unitaire de la session (pose refusée par l'hôte : rien de bâti, avis au client). Pas de changement de protocole. Pas encore lancé en jeu ; collision exacte des empreintes, personnages dans le passage, nœuds d'usage et étage restent vérifiés seulement par le mode construction du joueur.

- **Ramassage par un client (test `groundpick` 0/3)** : l'hôte était en pause pendant tout le test. Le client venait d'arriver et son éditeur de personnage restait ouvert (« Coucoudz 2 is making their character: game paused », jamais levé) : jeu en pause, personne ne marche, ni l'ordre « ramasser » du jeu ni l'ordre de marche. Le test valide maintenant l'éditeur (`editdone`) comme les autres. Côté hôte, une demande de ramassage en attente ne compte plus le temps de pause (journal « the game is paused, the character will walk there once it resumes ») au lieu d'échouer au bout de 30 s. Le journal d'échec affichait une distance fausse (52095) : position de l'objet non lue ; corrigé.

- **Commerce chez le client (test `trade` 6/13)** : après l'image « hors jeu » que le jeu fait en ouvrant la fenêtre, le client oubliait toutes ses doublures (remis par 8a3a003) ; elles redevenaient des inconnus, supprimés 5 s plus tard (« removed 13 local character(s) »), le marchand compris : fenêtre fermée, vente payée localement mais jamais envoyée à l'hôte. Seules les doublures dont l'objet local a disparu sont oubliées maintenant. Et la fenêtre du marchand restait parfois vide (`tradelist merchant` → 0) alors que les comptoirs avaient le stock de l'hôte (intermittent depuis le 09/10) : le client vérifie qu'elle montre le stock et la rouvre sinon (3 fois au plus, « trade window shows none of the shop's N stacks: opened again »).
- **Test de stabilité `soak` refait** (`tools/coop_test.py soak --minutes 20`, voir docs/TESTS.md) : vitesses 1 → 2 → 3 → 2 → 1, changements rapides de vitesse et pauses, demandes de vitesse / pause du client (ignorées : le client suit l'horloge de l'hôte), activité des deux côtés (déplacements, combat, K.-O. et fouille, commerce, bâtiments, TP loin et retour). Toutes les 30 s : plantage, comparaison en pause, aller-retour d'une commande et mémoire des deux jeux (ÉCHEC au-delà de +40 % après 2 min). Aucune nouvelle commande de debug. Pas encore lancé en jeu.
- **construct / buyhouse / farlong (essais du 10/10, 12:32–12:42)** :
  - `giveitem` ne donnait rien : le nom « Building Materials » / « construction » tombait sur un autre enregistrement (type 49, que la fabrique d'objets refuse : « factory refused 1502-gamedata.base (type 49 …) »). `ItemTemplates` (donc `itemtypes`) liste maintenant les armes, armures et objets (types 2-4) d'abord, noms exacts en tête ; `giveitem` essaie chaque candidat jusqu'à ce que la fabrique en fasse un. Le test choisit le nom qui commence par « Mat… »/« Building Mat… ». L'ordre « construire » du client arrive bien à l'hôte (« order "build" (2) on Tente Medium -> ok ») : le chantier n'avançait pas faute de matériaux ; le test attend 20 s au lieu de 8 que le bâtisseur s'y mette.
  - buyhouse : la porte testée était à plus de 40 m du perso (rayon de synchro des portes `kDoorRadius` 400 ; aucune ligne « door: … open -> closed » chez l'hôte) : `doorstate` donne sa position (`at=`), le perso du client va à côté avant d'appuyer ; ouvert = état 1/2, fermé 0/3, attendu jusqu'à 5 s. Le conteneur : jusqu'à 20 s pour que le perso y marche. L'achat de l'hôte remet assez d'argent (4000 restants < prix 6000).
  - farlong : les PNJ « manquants » (9, 15…) sont des groupes qui marchent à 1700-2900 unités de l'escouade de l'hôte, zone que le client (caméra à 30000 de là) n'a pas chargée ; les écarts de position, des PNJ à plus de 1000 unités de tout joueur (le plugin ne tire pas au-delà de 300). Près du perso du client : 0 manquant, 0 écart sur les 10 relevés. Le test ne compte plus que ce qui dure (le même perso sur deux relevés de suite) près du perso du client ; les chiffres de toute la zone vont au journal.
  - Vrai défaut trouvé au passage : un corps assommé couché à 87 unités de celui de l'hôte pendant 2 min (tombé là où était la copie, loin de tout joueur, où les positions écrites ne prennent pas). Client : un corps assommé (pas mort) couché à plus de 25 unités de celui de l'hôte, à moins de 300 d'un perso de l'escouade, est relevé (au plus toutes les 5 s) et retombe là où est celui de l'hôte (`ReadyToFall`). Journal : « posture: … lies N units from the host's body: stood up to fall where the host's lies ». Non testé en jeu.

- **Trois nouvelles expériences en jeu** (`tools/coop_test.py`, voir docs/TESTS.md) : `construct` (vraie construction ordonnée par le client puis par l'hôte puis par les deux, matériaux et avancement comparés), `buyhouse` (achat d'un bâtiment à vendre : prix, propriétaire, porte et conteneur, refus sans argent, achat simultané), `farlong` (perso du client à plus de 30000 unités pendant 5 min, zone comparée toutes les 30 s, combat et bâtiment là-bas, retour). Nouvelles commandes de debug : `givemoney`, `itemtypes`, `giveitem`, `invcount`, `buildreq`, `buildinfo` ; `buildplace` et `buildlist` prennent un membre de l'escouade. `build` écrit une ligne SKIP claire quand il n'y a aucun bâtiment à vendre. Pas encore lancées en jeu ; le numéro de tâche « construire » est cherché par essai (1 à 99) faute d'être connu.

- **Tests carry / lootswap / groundpick (essais du 10/10, 02:37–02:45)** : `lootswap` et `groundpick` donnaient leurs ordres au `squad0` du client, qui est le perso de l'hôte : le client les refusait (« inventory change refused: that character belongs to Coucoudz ») ou les jetait sans rien dire (aucune ligne « pick up » chez l'hôte). Ils utilisent maintenant le perso du client (`own_index`), et le client journalise un ordre local jeté (« local order dropped: … »). `carry` : la position du jeu d'un corps porté est celle du porteur ; le test vérifie l'animation « porté » (`where` affiche `carried=0/1`) et compare à l'hôte. Pose d'un corps chez le client : lâché sans ragdoll, placé là où est celui de l'hôte, puis il tombe (`ReadyToFall`) ; il tombait de l'épaule du porteur local, à 12 unités de celui de l'hôte.

- **Tâches, TP admin, resync (tests du 10/10)** : le test `jobs` donnait la tâche 44 (suivre, ordre
  passager, jamais permanente) : il passe 31 (`STAY_CLOSE_TO_TARGET`, le « suivre » du panneau
  Tâches). Une tâche permanente donnée par un client apparaît aussi tout de suite dans son panneau
  (copie locale, gardée 5 s le temps que la liste de l'hôte arrive). TP admin d'un perso K.-O. : le
  corps est relevé d'abord, téléporté à l'image suivante, et le jeu ne peut plus le recoucher avant
  l'arrivée (il ne bougeait pas du tout). Test `suite` : deux PNJ au bord du rayon du relevé ne
  comptent plus comme manquants après le resync.
- **Tests en jeu carry / lootswap / groundpick** : `carry` lit la position du corps chez le client par sa clé (`where npc` n'existe que chez l'hôte) ; `lootswap` habille d'abord le PNJ ou le perso du client pour avoir un emplacement commun (`invsecs`, `invswap ... any`) ; les vérifications du journal de l'hôte lisaient des caractères au lieu de lignes. Ramassage par un client : l'hôte journalise la raison de chaque échec et, si l'ordre de ramassage du jeu ne fait pas bouger le perso en 4 s, l'y envoie lui-même.

- **Fix G5, suite des tests en jeu** : un mode de la barre (passif...) basculé par l'hôte avec un perso
  d'un autre joueur dans sa sélection s'applique bien au perso de l'hôte (le jeu décidait
  activé/désactivé d'après le perso du client). Test `jobs` : la commande de test donne maintenant une
  vraie tâche permanente (`shift`), et le test échoue s'il n'y a rien à retirer.
- **Fix G7 (TP admin, étages, test des bâtiments)** : le TP admin vérifie que chaque perso est arrivé et
  le re-téléporte (jusqu'à 5 fois) sinon ; un perso K.-O. (ragdoll encore active) restait à 470 unités.
  L'étage d'un perso placé passe par la fonction du jeu `_setPositionAndTeleport(pos, étage)`
  (0x65E940, prologue vérifié) au lieu d'écrire `floorGroup` : seulement si la valeur diffère, au plus
  toutes les 2 s et 3 fois par valeur, sous SEH. `buildlist` lit l'état directement sur les objets
  (rayon 1500, les plus proches d'abord). Tests `suite`, `floor`, `buildstate` adaptés.
- **État de construction des bâtiments existants** : une ville chargée par un client seul ne reste plus
  en « bâtons rouges » (le client refusait toute avancée de construction, y compris celle que le jeu
  fait en montant les bâtiments d'une ville) ; seuls les bâtiments des joueurs attendent l'hôte. L'hôte
  suit maintenant tous les chantiers près des joueurs (bâtiments achetés à réparer, villes, PNJ), pas
  seulement ceux de sa faction, et un bâtiment fini chez le client mais à réparer chez l'hôte repasse
  en chantier. Test : `coop_test.py buildstate` (sauvegarde kctest_mine).
- **Porter un corps vu par un client** : le corps est bien sur l'épaule (et plus debout sur la tête),
  et à la pose il tombe où l'hôte l'a posé sans s'envoler ; test en jeu `coop_test.py carry`.
- **Primes et relations** : les clients ne plantent plus sur une nouvelle prime : primes et crimes
  restent dans le jeu de l'hôte seulement (le client garde sa copie pour l'affichage et vide ceux de
  son jeu). Le journal ne répète plus « relation/bounty values set to the host's » chaque seconde :
  les petites variations des relations ne sont plus envoyées ni corrigées.

- **Inventaires et objets au sol (fix G2)** : échanger un objet avec un corps (bottes sur des
  bottes) marche sans refus ni duplication ; une pile lâchée sur une pile identique s'y ajoute chez
  l'hôte ; on peut poser des objets sur un corps K.-O. ; un client peut ramasser ou voler les objets
  posés en ville (marchandises, décor), avec l'ordre « ramasser » du jeu chez l'hôte.
- **Resync, TP lointaine, PNJ manquants, étages (fix G6)** :
  - un nouveau monde remet à zéro tout l'état du mod lié au monde, ce qui corrige le plantage après
    un resync ;
  - une zone qui charge n'efface plus les remplaçants des PNJ ;
  - un PNJ dont le remplaçant a disparu est recréé au lieu de manquer pour toujours ;
  - le « max offset » ne compte plus les persos hors de vue ;
  - la TP admin prévient le joueur visé et laisse 2 min à la connexion pendant que la zone charge
    (message 70) ;
  - l'étage des persos suit celui de l'hôte (message 71), donc l'étage affiché aussi ;
  - nouvelles expériences `resyncbar`, `fartp`, `missing`, `floor`.
- **Fix G5, ordres, tâches et commerce** (à vérifier en jeu) : un perso d'un autre joueur dans la
  sélection de l'hôte en est retiré au lieu de faire refuser tous ses ordres (le mode passif de l'hôte
  n'était jamais posé, d'où son perso qui attaquait avec l'ami, et il ne pouvait plus bouger le sien) ;
  retirer ou déplacer une tâche dans le panneau Tâches d'un client passe par l'hôte, qui renvoie les
  listes de tâches (message `JobList` 68, `TaskVia` 6 à 8) ; la fenêtre de commerce d'un client ne
  s'ouvre plus chez l'hôte quand le joueur a lancé la conversation ou passe par un clic droit.
  Expériences `passive`, `jobs`, `tradepaths`.

- **Robustesse (lot F)** : un PNJ bloqué chez un client (mur, porte, étage) est replacé où l'hôte
  l'a après 1 s sans progrès ; les PNJ lointains sont replacés sur la position de l'hôte ; la TP
  admin déplace aussi un personnage à terre ou porté ; un perso dans un lit ou une cage n'est plus
  jeté au sol par un K.-O. ; l'hôte vérifie le meuble visé par un ordre client ; la suite compare
  la rotation du corps hors combat ; nouvelles expériences `stuck`, `farnpc`, `beds`, `tpdown`.

- Lots lancés en parallèle (un agent par lot, chacun dans sa copie du dépôt) : portes et
  crochetage, factions et primes, combat à distance et tourelles, prisons et capture, bâtiments et
  construction, désynchro de combat et PNJ lointains. Ils seront fusionnés puis testés en jeu
  ensemble.
- **Lot B, factions et primes** (implémenté, à vérifier en jeu) : les relations de la faction du
  joueur avec chaque faction (dans les deux sens, rang et réputation), les primes, le crime en cours,
  la peine de prison et le laissez-passer de chaque personnage de l'escouade sont ceux de l'hôte
  chez tout le monde ; le jeu d'un client ne peut pas garder les siens (réimposés toutes les 2 s).
  Messages `Factions` (43) et `Bounties` (44) ; expérience `python tools/coop_test.py factions`.
- **Portes et serrures (lot A, à vérifier en jeu)** : les portes (ouvertes, fermées, verrouillées,
  défoncées) et les serrures des meubles (coffres, cages) près des joueurs suivent l'hôte ; le jeu
  d'un client ne change plus une porte de lui-même ; les boutons du panneau d'une porte cliqués par
  un client sont exécutés par l'hôte ; un coffre verrouillé ne s'ouvre pas pour un client (il faut
  crocheter). Messages `Doors` (40) et `DoorRequest` (41) ; expérience `doors`.
- **Lot D, prisons** (à vérifier en jeu) : cages, menottes, esclavage, évasion et peine de prison
  de l'hôte imposés à tous les clients (message `Captives`), pour les joueurs comme pour les PNJ ;
  le personnage en cage reste dans la cage chez le client ; le jeu du client ne peut plus changer
  ces états de lui-même. Commandes de test `cage`, `chain`, `enslave`, `captive`, expérience
  `prison`.
- **Lot C, combat à distance** (implémenté, à vérifier en jeu) : chaque tir d'arbalète, d'arc, de
  harpon ou de tourelle de l'hôte est refait chez les clients avec la même arme et **sur la même
  trajectoire** (les dégâts restent ceux de l'hôte). Le point visé des tireurs et l'orientation des
  tourelles suivent. Le jeu du client ne tire plus de lui-même. Nouveaux messages `Shots` et
  `Ranged`, expérience `ranged`, commandes `rangedlist`, `shoot`, `shots`, `turrets`, `turretaim` et
  `rangedaim`.
- **Lot E, bâtiments** (à vérifier en jeu) : poser un bâtiment ou un meuble en mode construction,
  pour tout le monde ; chez un client rien n'est bâti localement, l'hôte bâtit puis chaque joueur
  bâtit le même chantier au même endroit. Avancement, fin, pause et démontage des chantiers imposés
  par l'hôte ; un bâtiment détruit chez l'hôte disparaît partout ; achat et démontage demandés par
  un client exécutés par l'hôte, l'achat rejoué par chaque client. Expérience `build`.

## 9 octobre 2026 (soir)

### Commerce entre joueurs, correctifs de la suite, documentation
- **Commerce** : quand un marchand propose « commercer » au personnage d'un client, la fenêtre de
  commerce du jeu s'ouvre **chez ce client** (plus jamais chez l'hôte), avec le stock que l'hôte a
  dans les comptoirs du marchand. Chaque achat ou vente part à l'hôte avec le prix que le jeu du
  client a compté ; l'hôte vérifie que l'acheteur peut payer, déplace l'objet et l'argent (cats du
  joueur et du marchand), puis renvoie le stock à tous ceux qui commercent avec ce marchand. Une
  fenêtre ouverte (même celle de l'hôte) se rafraîchit quand un autre joueur achète : **un objet
  acheté par l'un n'est plus proposé aux autres**. Achat refusé (pas assez d'argent, objet déjà
  parti) : l'objet et l'argent reviennent comme avant. Marchands ambulants (sans étal) : pas
  encore gérés, le joueur est prévenu.
  - Vérifié par les tests unitaires (achat, vente, achat refusé, stock rafraîchi, argent du
    marchand, fermeture) ; vérification en jeu en cours.
- **Inventaires** : les déplacements d'objets sont maintenant comptés par type et par quantité :
  une partie de pile, ou un objet qui rejoint une pile existante, est bien rejoué par l'hôte (avant,
  ces cas revenaient en arrière). Les différences d'équipement d'un PNJ debout qui vient
  d'apparaître ne sont plus prises pour des actions du joueur.
- **K.-O. chez les clients** : le client impose directement l'état inconscient et la chute (avant,
  le personnage restait debout quelques secondes).
- **Console de l'hôte** : accents corrigés (sources compilées en UTF-8).
- **Textes en français** pour les joueurs : panneau d'état, messages, lignes de discussion (le
  journal reste en anglais).
- **Table des fonctions du jeu** : deux entrées inversées remises dans l'ordre ;
  `tools/check_functions.py` vérifie désormais l'ordre.
- **Suite de tests** : les deux points « escouades » testent enfin le vrai personnage du client
  (ils passaient sans rien vérifier) ; les fichiers de commande des tests ne s'accumulent plus dans
  le dossier de Kenshi ; nouvelle expérience `trade`.
- **Documentation** : `docs/` (fonctionnalités, architecture, notes moteur, tests, journaux), ce
  journal des changements, README à jour.

## 9 octobre 2026

### Contenants : les clients regardent dans les coffres, étagères et coffres-forts, et peuvent voler (`c5ba75a`)
- Clic droit d'un client sur un contenant : son personnage le plus proche y va via l'hôte.
- Arrivé, l'hôte donne un identifiant réseau au contenant et envoie son contenu à ce joueur, qui
  ouvre la fenêtre de fouille du jeu sur une copie.
- Les objets déplacés sont exécutés par l'hôte. Prendre dans un contenant qui n'est pas à nous est
  un vol, décidé par le jeu de l'hôte. Si le joueur est vu, la fenêtre se ferme et rien ne bouge.
- Testé en ville : un « Grand Panier » des Shinobi, objet volé sans être vu.

### Bouton de resynchronisation, fin des PNJ qui tremblent, meubles retrouvés, journaux en anglais (`8113191`)
- L'hôte peut resynchroniser un joueur ou tout le monde : le joueur recharge le monde tel qu'il est
  et retrouve son personnage.
- Côté client, les PNJ gardaient les tâches de la sauvegarde (s'asseoir, patrouiller, errer) et
  sautaient entre leur place et celle de l'hôte. Ils les abandonnent maintenant, à la manière du
  jeu (`reThinkCurrentAIAction`).
- Meubles (lits, tabourets, coffres, machines) et maisons sont cherchés dans la grille des
  bâtiments : l'hôte trouve l'objet et le bâtiment visés par l'ordre d'un client.
- Les ordres permanents sont posés sur le personnage lui-même.
- Journaux en anglais ; ce que lisent les joueurs reste en français.
- Suite de tests : 32 points, resync compris.

### Les conversations montrent leur première réplique ; un client qui part sait où il en est (`5f19c46`)
- La première réplique, arrivée avant l'ouverture de la fenêtre, est gardée.
- Un client qui quitte, ou perd l'hôte, garde une copie en pause. La fenêtre Multijoueur
  l'explique, dit comment recharger une de ses parties (Échap, puis Charger) et propose de quitter
  le jeu.

### Horloges d'animation recalées en douceur ; suite de 30 points (`f41d1f8`)
- L'horloge ne saute plus que si elle est très décalée : 1 image sur 15 000 à vitesse 1.
- Le journal nomme correctement les boutons de la barre d'escouade.
- Le rapport de synchro ne compte que les personnages immobiles.
- `coop_test.py suite` : 30 sur 30.

### Correctifs de la première partie entre amis ; console hors du jeu et journaux de tous les joueurs (`0dd792f`)
- Les conversations préparées sur les threads du jeu arrivent au bon joueur : plus de fenêtre chez
  l'hôte, plus de texte vide. Le jeu d'un client n'ouvre plus sa propre fenêtre de conversation.
- Les ordres des clients passent par les fonctions du jeu pour ce seul personnage. La sélection de
  l'hôte est restaurée à l'identique.
- Meubles et machines retrouvés par type et endroit.
- Ordres permanents et style de combat viennent de l'hôte : plus de furtif « tout seul ».
- Porter un corps : identique partout.
- Animations corrigées seulement quand elles sont vraiment décalées : plus de tremblements à
  vitesse élevée.
- Les personnages qui marchent regardent dans la bonne direction.
- Tout le monde attend pendant qu'un joueur crée son personnage.
- La pause d'un client réessaie jusqu'à prendre.
- Un joueur qui se reconnecte reprend sa connexion et son personnage. Un joueur arrivé pendant une
  sauvegarde reçoit le sien.
- Escouades : plus de rafale de nouvelles escouades.
- TP admin (« TP vers moi », commande `tp`).
- Console hors du jeu : joueurs, synchro de chacun, journal en direct, commandes. Les clients
  envoient leur journal et leurs rapports ; les journaux précédents sont archivés dans
  `KenshiCoop-logs`.

### Création du personnage avec l'éditeur de Kenshi (`8d90863`)
- Au premier arrivage, l'éditeur s'ouvre sur le nouveau personnage : race, sous-race, sexe,
  visage, cheveux, curseurs, nom.
- À la validation, l'hôte applique et montre le résultat à tous.
- Bouton « Modifier mon personnage ».

### Les escouades viennent de l'hôte (`16c0d85`)
- Noms, membres et ordre envoyés aux clients, qui s'organisent pareil.
- Le portrait déposé par un client devient une demande à l'hôte.
- Le changement de handle lié à l'escouade est suivi côté client.
- Fenêtres de test toujours côte à côte.

### Un joueur qui revient retrouve son personnage via son compte Steam (`9037003`)
- Fichier `KenshiCoop-players.txt` (compte Steam → personnage). À défaut, un personnage à son nom ;
  sinon un nouveau.
- Les tests donnent un faux identifiant Steam à chaque client.

### Pas de plantage sur un inventaire plein ; la pause ne fige plus les clients en pleine foulée (`bbef13b`)
- Un déplacement vers un inventaire plein pouvait détruire l'objet puis laisser un pointeur
  invalide : plantage. Seules les cases libres vérifiées sont utilisées désormais.
- Avant de se mettre en pause, les clients tournent quelques images au ralenti pour poser chacun
  exactement.

### Les clients refont tourner le système de tâches (`aa6dc6f`)
- Le bloquer empêchait la marche (personnages qui glissent, direction figée). Ce sont les
  décisions qui restent refusées, hook par hook.

### Harnais : 1 hôte + 3 clients (`2570dc5`)
- Chaque client est comparé à l'hôte et aux autres clients.

### Rejoindre ses amis via Steam, sans port à ouvrir (`f6d3360`)
- Trafic relayé par le P2P de Steam ; présence enrichie ; bouton « Rejoindre la partie » ; code
  Steam.
- Mode de test `steam_loopback`.

### Progression, ordres des joueurs, conversations et décisions des PNJ viennent de l'hôte (`8acd244`)
- Compétences, argent et faim viennent de l'hôte.
- Tout ordre donné par l'interface d'un client est exécuté par l'hôte.
- Bulles rejouées partout ; conversation dans une fenêtre chez le bon joueur.
- Plus aucune décision d'IA chez les clients.

### Inventaires : le glisser-déposer survit à la souris ; refus immédiat pour les personnages des autres (`52ff0d9`)

### Les objets portés peuvent de nouveau aller dans le sac (`53bd0f5`)

### Les clients posent leurs objets via l'hôte (`2b27319`)

### Les clients ramassent les objets via l'hôte (`46783b1`)

### Les objets ramassés venus de la sauvegarde disparaissent aussi chez les clients (`e17d8b9`)

### Les objets au sol suivent l'hôte (`6b1dd19`)

### Fenêtre Multijoueur et console fusionnées (`48a1080`)

### Cycles de marche et de course chez les clients ; chiffres de dégâts de l'hôte (`9cab700`)

### Les clients ne décident plus d'un effondrement (`f4cb410`)

### Tout ce qui est à l'écran : chaque animation des personnages proches, 15 fois par seconde (`f7a9b0a`)

### Les combattants regardent, se mettent en garde et tiennent leur arme comme chez l'hôte (`898580a`)

### Les animations de combat et d'action suivent l'hôte (`3aa452c`)

### Fenêtre Multijoueur et console dans le jeu (`904769f`)
- Ctrl+Shift+M et Ctrl+Shift+K.
- L'hôte peut exclure un joueur.

### Les personnages vont à l'allure de l'hôte ; les effets s'éteignent avec ceux de l'hôte (`36d6d55`)

### Effets météo de l'hôte : éclairs, rayons, tempêtes, nuages de gaz (`d9f632f`)

### Les clients ne meurent ni ne s'évanouissent plus sur leur propre jugement médical (`87331e8`)

### Suivi des personnages à travers les changements de handle ; horloges ensemble ; paquet pour les amis (`04f883d`)
- `package.ps1` fabrique `dist\KenshiCoop.zip`, avec `Installer.bat` et `Desinstaller.bat`.
- L'installeur trouve Kenshi dans n'importe quelle bibliothèque Steam.

### Un personnage par joueur, vraie fouille au clic droit, monde client plus stable (`19d2a8c`)

### Les cadavres proches des joueurs restent synchronisés et se fouillent (`95fd923`)

### Inventaires et équipement, population de PNJ, chutes au même endroit (`1b92900`)

### Météo (`6984c61`)
- 65 régions sur 65 identiques.

### Combat au corps à corps et posture (`195db30`)

### Positions exactes, remplaçants pour les personnages absents, harnais de test (`f83afee`)

### Attendre la fin de l'écriture de la sauvegarde avant de l'envoyer (`13cadfb`)

## 8 et 9 octobre 2026 : les débuts

### Sérialisation des ticks entre threads, journal des plantages (`b03921e`)

### Rejoindre = recevoir le monde de l'hôte (`2921774`)
- Depuis le menu principal.
- L'hôte sauvegarde, envoie, le client charge.

### Monde piloté par l'hôte : PNJ, santé, heure, pause (`a23c55d`)

### Plugin Kenshi : hooks, pont vers le monde, overlay, installeur (`4a1266e`)

### Cœur multijoueur : protocole, transport ENet, session pilotée par l'hôte, tests (`edc7db0`)
