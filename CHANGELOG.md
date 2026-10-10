# Journal des changements

Du plus récent au plus ancien. Chaque entrée correspond à un commit (son identifiant entre
parenthèses). L'état détaillé de chaque fonctionnalité est dans
[docs/FONCTIONNALITES.md](docs/FONCTIONNALITES.md).

## En cours (pas encore commité)

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
