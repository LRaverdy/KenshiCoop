# Journal des changements

Du plus récent au plus ancien. Chaque entrée correspond à un commit (son identifiant entre
parenthèses). L'état détaillé de chaque fonctionnalité est dans
[docs/FONCTIONNALITES.md](docs/FONCTIONNALITES.md).

## En cours (pas encore commité)

- Lots lancés en parallèle (un agent par lot, chacun dans sa copie du dépôt) : portes et
  crochetage, factions et primes, combat à distance et tourelles, prisons et capture, bâtiments et
  construction, désynchro de combat et PNJ lointains. Ils seront fusionnés puis testés en jeu
  ensemble.
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
