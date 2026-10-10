# Bugs signalés, à traiter

Remontés par les parties entre amis. Chaque entrée garde la date et ce qu'on a vu.

## 10 octobre 2026, partie réelle en v0.3.0 (soir, 4 joueurs par Steam)

Journal de l'hôte gardé dans `kc_crashdumps/session_2215/host_live.log` (à compléter par les
journaux des clients `KenshiCoop-<pid>.log` de chacun).

- **Rejoindre est long** : nass4 a mis ~1 min 55 entre « world sent » (22:15:01) et son arrivée
  (22:16:55) ; rob, arrivé pendant ce temps, attendait dans la file et n'a reçu le monde qu'à
  22:17:04 (+1 min 45). Pistes : télécharger et charger le monde pendant l'attente (seule la
  création du perso chacun son tour) ; mesurer transfert Steam vs chargement Kenshi ; compresser
  le monde envoyé.
- **Recrutement par un client** : c'est l'hôte qui a eu la fenêtre pour accepter ; le perso
  recruté est bien arrivé dans l'escouade (chez l'hôte) mais le client ne le voit pas.
- **Fenêtre de sortie des mines invisible chez les clients** (l'inventaire de production). Vérifier
  tous les types de mines et d'ateliers / bâtiments de production.
- **Objet glissé au sol par le client 2 disparu** : il n'est pas apparu au sol, il a disparu (pas
  à chaque fois). Perte d'objet : prioritaire.
- **Joueur 3 dans une ville voisine : parler aux PNJ ne fait rien** (aucune fenêtre de dialogue).
- **Ramassage par le client 2 : objets perdus** (prioritaire, lié au point « objet glissé au sol
  disparu ») : un objet ramassé disparaît du sol mais n'arrive pas dans son inventaire ; une autre
  fois il est bien arrivé puis a disparu de l'inventaire au bout d'un moment (inventaire réécrit
  par celui de l'hôte, où l'objet n'est jamais arrivé ?).
- **Construction par le client 3** : la construction s'est bien passée, mais (1) la jauge des
  matériaux de construction du chantier ne montrait pas les matériaux apportés (chez le client) ;
  (2) une fois fini, le bâtiment n'a pas de collision chez le client (on le traverse), alors qu'un
  bâtiment normal en a. Probable : le bâtiment est créé ou fini par la synchro sans passer par ce
  qui crée sa physique (finishConstruction / l'état « fini » du jeu), à vérifier aussi pour les
  bâtiments posés par l'hôte.
- **Bâtiment construit par le joueur 3 : impossible à démanteler** (à préciser : chez lui, chez
  l'hôte, ou chez tous). Sans doute le même fond que la collision : le bâtiment fini n'est pas
  un vrai bâtiment fini pour le jeu (état, propriétaire ou faction), donc l'ordre « démanteler »
  n'est pas proposé ou est refusé (vérifier aussi la validation de la tâche démanteler dans
  `TaskTargetAllowed`).
  **Cause trouvée** dans le journal de l'hôte : « refused: client task 16 (via 1) not run: task 16
  (?) needs a task a client may send (unknown task id) » (7 fois). La tâche 16 (démanteler) n'est
  pas dans la liste des tâches qu'un client peut envoyer (`kc::TaskTargetAllowed`, protocol.cpp) :
  l'ajouter, cible = bâtiment du joueur (fini ou en chantier). Revoir toute la liste pour
  d'autres tâches légitimes manquantes (réparer, etc.).
- **Vue des étages chez le client** : le client doit forcer lui-même la vue des étages (toit /
  étages cachés) ; elle ne suit pas automatiquement quand son perso entre dans un bâtiment ou
  change d'étage, comme en solo. À voir avec le niveau d'étage (floorGroup, écritures
  désactivées : `kWriteFloors = false`).
- **rob (joueur 3) a planté à 22:32** (l'hôte l'a vu partir à 22:32:02). Juste avant, son jeu
  perdait et recréait en boucle une escouade de 17 PNJ de l'hôte (`1:96:522216800:*`), toutes les
  ~3 s (« stand-in … is gone here: it can be recreated », 467 lignes de ce genre pour lui dans la
  session). Probable cause ou facteur : cette boucle. Demander à rob son `KenshiCoop.log` et le
  `crashDump*.zip` de son dossier Kenshi (le vrai rapport du plantage est chez lui).

## 10 octobre 2026

- ~~**Persos des joueurs en double chez les clients déjà là**~~ (stress à 4 joueurs du 10/10) —
  **corrigé** (à vérifier en jeu, vérification `stress4` « aucun perso de joueur en trop chez un
  client »). Cause exacte : la photo de l'escouade qui sert à reconnaître le monde n'était prise
  qu'au moment où le monde redevenait prêt, jamais ensuite ; toute copie créée depuis (un joueur
  qui arrive) faisait de la coupure suivante un « nouveau monde ». Maintenant `kc::WorldIdentity`
  (photo à chaque image, sans les doublures ; doublures restées d'un vrai nouveau monde retirées) ;
  tests `TestWorldIdentity`, `TestNoDuplicatePlayers`. Constat d'origine : Chez le client 1, 4 copies en trop de Joueur3 et Joueur4 dans
  son escouade dès le 1er relevé, 12 à la fin ; chez le client 2, 2 puis 4 copies de Joueur4 ; le
  client 3 (arrivé le dernier) n'en a aucune. Elles apparaissent à chaque « new world (generation
  N): every per-world state reset » de ces clients (fenêtres redimensionnées, zone qui charge) : le
  jeu ne tournait pas pendant quelques images et l'ensemble des persos de l'escouade a changé (les
  copies des autres joueurs en font partie), donc `KenshiWorld::BeginFrame` compte un nouveau monde,
  vide `alias_`, les copies déjà là ne sont plus reconnues (« 2 host squad members are missing in
  the local world ») et le client en recrée.
- **Hauteur des persos dans les régions lointaines** (stress à 4 joueurs du 10/10) : à l'arrêt, des
  persos de joueurs (0,1 à 4,5 unités) et des bêtes (7 à 14) n'ont pas la même hauteur chez l'hôte
  et chez le client, avec 0,000 au sol ; la copie du client tient exactement la position de l'hôte.
  Hypothèse : l'hôte n'a pas de caméra dans ces régions et n'y a pas le même sol. Le test tolère
  15 en hauteur ; à confirmer (hauteur du terrain sous le perso chez l'un et l'autre).

- **Un join qui garde l'hôte en pause** (fix G6) : vérifié. Un joueur qui rejoint est retiré après
  `loadTimeout` (300 s). Si son jeu plante pendant le chargement, ENet le coupe en 15 s et l'hôte
  repart. Le journal dit maintenant « X left while joining (connection lost: their game quit or
  crashed while loading) ».

- ~~**PRIORITÉ — Les 3 clients plantent juste après une nouvelle prime**~~ (10/10) — **corrigé** (à
  vérifier en jeu). L'operator[] (0x5E7EE0) était bien appelé comme le jeu le fait ; mais le client
  plantait aussi au chargement de la sauvegarde contenant la prime, sans le mod : c'est la prime
  elle-même dans le jeu du client (sa police locale contre un perso que l'hôte pilote). Désormais le
  client n'a jamais de prime ni de crime dans son jeu (il vide ceux que son jeu crée) et garde la
  copie de l'hôte pour l'affichage. Reste : si le plantage arrive pendant le chargement, avant la
  connexion, on n'a pas encore pu vider la prime (à surveiller).

- **Ville lointaine en « bâtons rouges »** : un client parti seul dans une autre ville voit ses bâtiments
  comme des chantiers (bâtons rouges), alors que l'hôte la voit normalement.
  → Corrigé (à tester en jeu) : le client bloquait toute avancée de construction ; il ne bloque plus que
  celle des bâtiments des joueurs.
- ✅ **Fenêtre du marchand chez l'hôte** (corrigé, fix G5, à vérifier en jeu) : en 0.2.x, la fenêtre de
  commerce s'ouvre encore chez l'hôte et pas chez le client (au moins dans un des cas de dialogue ou de clic).
  Trouvé : toutes les fenêtres passent par `showTradeWindow` (désassemblage : 2 appelants seulement,
  l'action de dialogue « commercer » et la tâche de fouille/commerce du clic droit), mais le mod ne la
  détournait que si le perso du joueur était le **premier** côté et **hors** d'un appel du mod. Or
  l'action de dialogue passe (cible, propriétaire du dialogue) : quand c'est le joueur qui a lancé la
  conversation, son perso arrive en second et la fenêtre s'ouvrait chez l'hôte ; et l'ordre du client
  (clic droit) et sa réponse au dialogue sont exécutés par le mod, donc ignorés. Maintenant : quel que
  soit le côté et qui appelle, une fenêtre pour le perso d'un autre joueur part chez lui.
- ~~**Plantage après un resync**~~ (corrigé, fix G6, à vérifier en jeu) : après un resync, un client n'avait
  plus les cartes de ses personnages dans la barre d'escouade, puis son jeu a planté.
  Trouvé : au rechargement du monde, le mod ne vidait qu'une partie de son état. Des pointeurs vers les
  objets de l'ancien monde restaient : outils mis en main (`handTools_`), animations et chiffres de
  dégâts par perso, dialogues, bâtiments suivis, ramassages en cours, mode dieu… Le premier appel au
  jeu sur l'un d'eux tombait dans de la mémoire libérée. Tout l'état lié au monde est maintenant
  remis à zéro à chaque nouveau monde (`KenshiWorld::ResetWorldBound`). À l'inverse, une simple
  coupure du tick (une zone qui charge, un à-coup) comptait comme un nouveau monde et effaçait les
  remplaçants des PNJ en pleine partie. Un nouveau monde se reconnaît maintenant à un joueur ou à
  une escouade dont les objets ont changé. Test en jeu : expérience `resyncbar`.
- ~~**Étage qui ne change pas tout seul**~~ (corrigé, fix G6, à vérifier en jeu) : trouvé le champ du jeu,
  `CharMovement::floorGroup` (+0x334, 9 = rez-de-chaussée), d'où `getCurrentFloor()` tire l'étage.
  L'hôte l'envoie pour chaque perso (message 71 `Floors`) et le client l'écrit sur sa copie. Test :
  expérience `floor`.
  Signalement d'origine : chez un client, l'étage affiché ne suit pas automatiquement quand un
  perso monte ou descend dans un bâtiment (comme le fait le jeu en solo). Piste : chez le client, les persos
  sont placés à la position de l'hôte au lieu de prendre l'escalier eux-mêmes, et l'étage courant du
  personnage (qui pilote l'affichage) n'est pas mis à jour.
  Fix G7 : l'écriture brute était effacée à chaque image (le jeu recalcule `floorGroup` depuis la
  surface sous le perso, kenshi_x64+0x65F564). Remplacée par `_setPositionAndTeleport(pos, groupe - 9)`
  (groupes >= 9 seulement, valeur différente, 2 s d'écart, 3 essais par valeur). À vérifier en jeu dans
  un bâtiment à étages ; si le jeu remet encore sa valeur, l'écart vient de la position (hauteur) du perso.
- **TP admin d'un perso K.-O.** (fix G7, à vérifier en jeu) : la téléportation est vérifiée et refaite
  si le perso n'est pas arrivé. Les écarts de 2-4 unités en fin de `suite` venaient des corps à terre
  (ragdoll simulée par chaque jeu) : tolérance de 8 pour les persos K.-O./morts.
  Test du 10/10 (03:00) : 1304 unités, le perso n'a **pas bougé du tout** pendant les 5 reprises
  (toujours 1310 de la cible). Une téléportation ne bouge pas une ragdoll active, même désactivée dans la
  même image, et le jeu recouche le perso K.-O. aussitôt. Maintenant : une reprise relève d'abord le
  corps s'il est encore en ragdoll et ne téléporte qu'à l'image suivante (0,15 s), et
  `hk_ragdollMode` empêche le jeu de le recoucher tant que le TP n'est pas fini (`HoldsUpright`).
  Le journal dit à chaque reprise s'il était encore en ragdoll. À revérifier : `suite`.
- **`suite`, resync « (0, 0, 2) »** : les 2 « manquants » étaient deux PNJ qui couraient à 2996 unités
  du perso de l'hôte, au bord du rayon de 3000 du relevé : présents chez le client (entités
  `present=1`), sa copie juste au-delà du rayon. Pas une désynchro : `compare` ne compte plus les persos
  au bord du rayon (60 unités) présents chez le client (`edge_unlisted`).
- **Test `buildstate`** (fix G7) : `buildlist` ne trouvait aucun bâtiment (rayon 300, aller-retour par
  handle) ; lit maintenant l'état sur l'objet, rayon 1500, et donne un diagnostic si vide.
- ✅ **Tâches qu'un client ne peut pas supprimer** (corrigé, fix G5, à vérifier en jeu). Trouvé : la croix
  du panneau Tâches (`OrderCellView::onRemove`), `OrdersPanel::removeJob` et le glisser
  (`movePermajob`) appellent directement `Character::removePermajob` / `movePermajob` du perso, sans
  passer par les ordres que le mod intercepte. Maintenant ces fonctions (et `Character::removeJob`)
  sont détournées chez le client : l'hôte fait la même chose sur son perso (nouvelles valeurs
  `TaskVia` 6 à 8 de la commande `Task`), et l'hôte envoie les listes de tâches (message `JobList`,
  68) : le client retire de la sienne ce que l'hôte n'a plus. Limite : une tâche ajoutée n'apparaît
  pas dans la liste du client (elle n'y est que par la sauvegarde, après un resync).
  Test en jeu du 10/10 (`jobs`) : aucune tâche chez l'hôte, les contrôles de retrait passaient à vide.
  Trouvé : l'ordre du client arrive bien à l'hôte, mais la commande de test `jobreq` passait
  `shift = false` à `addJobSelectedCharacters` ; ce booléen fait d'un ordre une **tâche permanente**
  (liste du panneau Tâches) : sans lui, `OrdersReceiver::addJob` (`0x5086D0`) le range dans les ordres
  passagers. Le chemin réel (clic avec Maj, panneau) transmet déjà ce booléen à l'hôte. `jobreq` le met
  maintenant, et le test échoue franchement s'il n'y a pas de tâche à retirer. À revérifier : `jobs`.
  2e essai (10/10, 02:57) : l'hôte a bien exécuté l'ordre (`client task 44 (via 4) ... ok`) mais la
  tâche 44 (`FOLLOW_PLAYER_ORDER`) n'est jamais permanente : `OrdersReceiver::addJob` ne la range dans
  la liste du panneau que si sa `TaskData` est une tâche permanente (`+0x70 -> +4`). Le « suivre » du
  panneau est la tâche 31 (`STAY_CLOSE_TO_TARGET`, cas particulier de `Character::addJob` 0x5C8DA0,
  comme 45 `BODYGUARD`). Le test passe 31. Et la limite ci-dessus est levée : une tâche permanente
  donnée par un client est aussi ajoutée tout de suite à la copie locale (gardée 5 s par `ApplyJobs`,
  le temps que la liste de l'hôte l'ait) ; si l'hôte ne l'a pas, elle est retirée ensuite.
  Ancienne description : toujours là après la 0.2.0. Le bouton stop
  arrête maintenant la tâche en cours chez l'hôte, mais retirer une tâche de la liste des tâches du perso
  (panneau Tâches, clic sur la croix) ne passe que par le jeu du client : l'hôte la garde, et elle revient.
  À faire : intercepter la suppression d'une tâche (et « tout effacer ») côté client et l'exécuter chez
  l'hôte, comme les ordres.
- ~~**« TP vers moi » sur un joueur loin : il est éjecté**~~ (corrigé, fix G6, à vérifier en jeu). Trouvé : le
  jeu du client se fige pendant qu'il charge la zone d'arrivée. Or la connexion ENet coupe un pair
  muet au bout de 15 s (des deux côtés). Maintenant, avant la TP, l'hôte allonge ce délai à 2 min
  pour ce joueur et prévient son jeu (message 70 `Stall`), qui fait de même. Le délai normal revient
  ensuite. Un vrai plantage reste repéré en 15 s le reste du temps. Test : expérience `fartp`.
  Signalement d'origine : son jeu charge d'un coup la
  zone d'arrivée et ne répond plus assez longtemps pour que la connexion expire, ou il plante pendant ce
  chargement. À vérifier dans son KenshiCoop.log (déconnexion ou CRASH) ; si c'est l'attente, allonger le
  délai d'expiration pendant un TP ou faire charger la zone avant de déplacer le perso.
  Journal de l'hôte du 10/10 : `tp 3` à 00:30:51, Geoffrey part à 00:30:58 sans aucune ligne de son jeu
  entre les deux (gel ou plantage pendant le chargement de la zone).
- ~~**Relations corrigées en boucle**~~ — **corrigé**. Le journal montre des corrections « fraîches »
  (pas « the local game had changed them ») : l'hôte renvoyait les relations chaque seconde, car le jeu
  bouge sans cesse un peu ses flottants (confiance, force), et le client recopiait la valeur exacte.
  Les « 3 valeurs » apparaissaient pendant un crime (crime, faction, expiration réécrits puis effacés
  par le jeu du client). Maintenant : envoi et correction seulement au-delà du bruit (0,5 point, 1 %
  de force), et les crimes ne sont plus écrits chez le client.
- ✅ (corrigé, fix G2, à confirmer en jeu) **Fouille d'un cadavre : l'objet se duplique puis s'annule** (Geoffrey, 10/10 vers 00:35). Le journal de
  l'hôte montre des déplacements d'équipement refusés : « boots -> boots 0,0 : no room ». Quand le joueur
  glisse des bottes du corps sur son perso qui en porte déjà, son jeu fait un échange (les anciennes sortent,
  les nouvelles entrent) ; le mod envoie les deux déplacements dans le mauvais ordre, l'hôte refuse d'abord
  « pas de place », renvoie l'état réel, et l'objet revient : d'où l'aspect dupliqué puis annulé.
  À faire : envoyer d'abord ce qui libère une place, et en cas de place prise, laisser l'hôte poser l'objet
  ailleurs dans l'inventaire. Il y a aussi des refus « refused an inventory move from player 3 » à éclaircir.
  **Trouvé** : un échange est un cycle (chaque objet va sur la case de l'autre), donc aucun ordre
  d'envoi ne marche ; l'hôte reconnaît maintenant la paire et fait l'échange en une fois
  (`SwapInventoryItems`). Les autres déplacements sont remis dans l'ordre (celui qui libère une case
  d'abord, chez le client et chez l'hôte). Les « refused an inventory move from player 3 » venaient
  de l'objet que l'échange posait sur le corps : l'hôte refusait tout dépôt sur un PNJ (même K.-O.) ;
  c'est permis maintenant pour un corps à terre, et le journal dit la raison d'un refus. Enfin une
  pile lâchée sur une pile du même objet s'y ajoute chez l'hôte (elle était posée ailleurs : rebond).
- ✅ **Porter un corps (vu par un client)** — corrigé, à vérifier en jeu (`coop_test.py carry`). Le corps
  porté apparaissait debout sur la tête du porteur, puis s'envolait à la pose (PNJ comme persos de joueurs).
  Cause : `pickupObject` (0x5CFF90) refuse sans rien dire un corps en ragdoll (+0x3d4), or un client garde
  les corps KO en ragdoll ; la mise sur l'épaule échouait donc chez lui (journal « ok » trompeur) et le
  code de posture se battait avec le corps. À la pose, le corps était ensuite redressé ou téléporté
  pendant que son ragdoll démarrait, d'où la projection. Correctif : le ragdoll est retiré juste avant le
  ramassage, le succès est vérifié (sinon nouvel essai chaque seconde) ; à la pose, le corps passe en
  ragdoll tout de suite et on ne le touche plus pendant 2 s.
  Essai du 10/10 : « corps None » chez le client venait du test (`where npc` utilise le dernier PNJ créé, qui n'existe que chez l'hôte) ; il lit maintenant la position par la clé du PNJ.
  Essai du 10/10 (02:38) : le corps est bien sur l'épaule (journal client « ok ») ; la position du jeu d'un corps porté est celle du porteur, d'où « même position ». Le test vérifie maintenant l'animation « porté » et compare à l'hôte. À la pose, le corps du client tombait de l'épaule locale (12 unités plus loin) : il est maintenant lâché sans ragdoll, placé où est celui de l'hôte, puis tombe. À vérifier : `coop_test.py carry` (écart horizontal < 5).
- **Test `lootswap`** (10/10, 02:42) : l'échange visait `squad0` chez le client, le perso de l'hôte : refusé chez le client (« that character belongs to Coucoudz »), rien n'arrivait à l'hôte. Corrigé (perso du client, `own_index`). Avant : aucun emplacement commun occupé (le PNJ copié et le perso du client ne portaient rien au même endroit). Le test habille maintenant l'un ou l'autre avec les vêtements des autres membres de l'escouade de l'hôte, puis échange le premier emplacement commun (`invswap ... any`, nouvelle commande `invsecs`). Il vérifiait aussi les refus sur des caractères et non des lignes du journal : corrigé.
- **Un PNJ invisible en combat chez 2 clients sur 3** (10/10 vers 00:38). Les rapports des clients le
  confirment : chez nass4, 1 PNJ de l'hôte « pas encore là » ; chez Geoffrey, 31 en permanence ; rob, 0.
  Ces PNJ n'existent pas dans le jeu du client, et sa recréation (modèle + faction) échoue. Voir pourquoi
  (PNJ unique, modèle introuvable, zone pas chargée). Les mêmes rapports montrent aussi des persos décalés
  de 300 à 466 unités (« max offset »), à éclaircir.
  **Trouvé (fix G6, à vérifier en jeu)** : deux causes.
  1. Quand le jeu du client supprimait un remplaçant (mort puis nettoyé, ou zone déchargée), son alias
     restait. `Spawn` et `Reconcile` sautaient alors ce PNJ pour toujours : les « 31 en permanence ».
     L'alias mort est maintenant retiré et le PNJ est recréé.
  2. Une coupure du tick effaçait tous les alias (voir « Plantage après un resync »).
  Le « max offset » venait des persos loin de l'escouade du client : là, son jeu les fait à peine
  tourner et ignore les positions qu'on leur écrit, et personne ne les voit. L'écart n'est plus
  compté au-delà de 300 unités de l'escouade. Test : expérience `missing`.
- ✅ **Le perso de l'hôte en passif attaque quand un ami attaque** (corrigé, fix G5, à vérifier en jeu).
  Trouvé : l'ordre du client lui-même ne touche que son perso (désassemblage de
  `addOrderSelectedCharacters` : il parcourt la sélection, réduite à ce perso pendant l'appel). La
  cause est la sélection de l'hôte : dès qu'elle contenait un perso d'un autre joueur (clic ou cadre
  dessus, escouade créée pour un client), le mod **refusait en bloc** les ordres de l'hôte. Le bouton
  « passif » de la barre s'affichait activé (le panneau bascule le bouton avant l'appel) mais le mode
  n'était jamais posé sur son perso, qui défendait donc l'ami ; et l'hôte ne pouvait plus déplacer son
  propre perso (« sa sélection passe sur les persos des autres »). Maintenant les persos des autres
  joueurs sont retirés de la sélection de l'hôte et l'ordre part à ses persos ; l'escouade créée ou
  rejointe pour un client ne change plus la sélection de l'hôte.
  Test en jeu du 10/10 (`passive`) : la sélection mixte ne basculait toujours pas le mode du perso de
  l'hôte. Trouvé (désassemblage) : `setOrderSelectedCharacters` choisit activé/désactivé d'après le
  perso « principal » de la sélection (un `hand` global, le dernier cliqué), ici celui du client (pas
  passif, donc « activer ») ou plus rien une fois retiré. Maintenant, quand des persos d'autres joueurs
  sont retirés, l'hôte calcule la bascule d'après son propre premier perso et la pose lui-même sur ses
  persos. À revérifier : expérience `passive`.
  Ancienne description : l'hôte se met en passif ; quand un
  client ordonne à son perso d'attaquer un PNJ, le perso de l'hôte part aussi à l'attaque. Pistes : l'ordre
  du client est exécuté chez l'hôte en sélectionnant le perso du client ; si la sélection de l'hôte (son
  perso) est encore active à ce moment-là, l'ordre « attaquer » part aussi pour lui. Vérifier que pendant
  l'exécution seul le perso du client est sélectionné, et que le mode passif n'est pas retiré.
- **Bâtiment acheté à réparer** (10/10) : chez l'hôte, le bâtiment acheté apparaît en chantier (à
  réparer), mais pas chez le client, qui ne peut donc pas lancer la réparation. L'état de construction des
  bâtiments déjà présents dans la sauvegarde (et non posés pendant la partie) n'est sans doute pas envoyé
  aux clients, ou l'achat ne rejoue pas chez eux le passage en « à réparer ». Probablement lié aux villes
  vues en « bâtons rouges » chez un client (état de construction des bâtiments existants mal synchronisé).
  Précision : l'hôte a terminé la réparation (il le voit construit), le client le voit toujours en
  chantier. L'avancement et la fin des travaux sur un bâtiment existant n'arrivent donc pas chez le client.
  → Corrigé (à tester avec `coop_test.py buildstate`) : l'hôte suit tous les chantiers près des joueurs,
  et un bâtiment fini chez le client repasse en chantier si l'hôte l'a à réparer.
- ✅ (corrigé, fix G2, à confirmer en jeu) **Ramasser (voler) un objet par terre ne marche pas chez le client 4** (nass4, 10/10). À croiser avec le
  journal : l'ordre « ramasser » part bien à l'hôte, qui doit retrouver le même objet par type et endroit
  (à 15 unités près). Pistes : l'objet de la sauvegarde a un autre handle chez lui et n'est pas retrouvé,
  ou le vol (objet d'un magasin, d'une faction) n'est pas traité comme tel côté hôte.
  Journal : « [nass4] pick up Bol en Bois -> FAILED » (puis Cuivre, Matériaux Construction), tous refusés
  chez l'hôte : l'hôte ne retrouve pas l'objet visé (même type à moins de 15 unités). Probablement le
  décalage de position de nass4 (rapports « max offset » de 450 unités) : son jeu voit les objets à un
  autre endroit que l'hôte.
  **Trouvé** : pour l'ordre « aller à » sur le même objet, l'hôte le retrouvait bien (recherche de tous
  les objets) ; le « ramasser » ne cherchait que les objets « au sol » au sens strict (physiques, hors
  groupe d'objets). Les marchandises et objets de décor d'une ville n'en sont pas : jamais trouvés.
  L'hôte cherche maintenant tout objet hors inventaire, à 40 unités, et lance l'ordre « ramasser » du
  jeu (le perso y va, le vol est traité comme pour l'hôte). Le ramassage est aussi signalé aux
  clients pour ces objets-là.
  **Essai en jeu du 10/10 (`groundpick`) : 0/3 objets pris.** Le test ne voyait pas les refus (il lisait
  les 300 derniers *caractères* du journal, pas les lignes) : corrigé, il recopie les lignes « pick up »
  de l'hôte. L'hôte écrit maintenant la raison de chaque échec (objet introuvable : objets autour, même
  type plus loin et à quelle distance ; ordre du jeu impossible ; perso resté sur place ; `giveItem`
  refusé ; abandon après 30 s). Si l'ordre « ramasser » du jeu ne fait pas bouger le perso en 4 s,
  l'hôte l'y envoie lui-même et le lui fait prendre à 15 unités. À relancer : `coop_test.py groundpick`.
  **Essai en jeu du 10/10 (`groundpick`) : 0/3 objets pris.** Le test ne voyait pas les refus (il lisait les 300 derniers *caractères* du journal, pas les lignes) : corrigé, il recopie maintenant les lignes « pick up » de l'hôte. L'hôte écrit désormais la raison de chaque échec (objet introuvable : combien d'objets autour, du même type plus loin et à quelle distance ; ordre du jeu impossible ; perso resté sur place ; `giveItem` refusé ; abandon après 30 s). Si l'ordre « ramasser » du jeu ne fait pas bouger le perso en 4 s, l'hôte l'y envoie lui-même et le lui fait prendre à 15 unités. À relancer : `coop_test.py groundpick`.
  **Essai du 10/10 (02:43) : toujours 0/3, aucune ligne « pick up » chez l'hôte** : le test ordonnait au `squad0` du client (le perso de l'hôte), ordre jeté sans bruit par le client. Le test utilise le perso du client, et le client journalise désormais « local order dropped ». À relancer.
  **Essai du 10/10 (12:43) : 0/3, perso immobile chez l'hôte.** Cause : l'éditeur de personnage du client arrivé restait ouvert (le test n'envoyait pas `editdone`), donc tout le jeu était en pause (« is making their character: game paused », jamais levé) ; ni l'ordre du jeu ni la marche ne pouvaient avancer. Le test valide l'éditeur ; l'hôte ne compte plus le temps de pause dans les 30 s d'un ramassage. À relancer.
