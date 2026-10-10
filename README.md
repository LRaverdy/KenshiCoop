# KenshiCoop

Mod multijoueur coopératif pour **Kenshi 1.0.68 (Steam, x64)**. L'hôte fait tourner le monde ; les
autres joueurs rejoignent **le monde de l'hôte** (pas le leur) et chacun y contrôle son propre
personnage. Tout ce qui compte (dégâts, KO, morts, IA, météo, heure, loot, dialogues) est décidé
chez l'hôte et appliqué à l'identique chez les clients.

## Installation

**Chez toi (avec les sources)** — Kenshi fermé, dans PowerShell depuis ce dossier :
```powershell
.\build.ps1      # compile (Visual Studio 2022 requis)
.\install.ps1    # copie KenshiCoop.dll dans le dossier de Kenshi et l'ajoute à Plugins_x64.cfg
.\package.ps1    # fabrique dist\KenshiCoop.zip à envoyer aux amis
```
Désinstaller : `.\install.ps1 -Uninstall`.

**Chez un ami (sans rien compiler)** :
- lui envoyer `dist\KenshiCoop.zip` ;
- il le décompresse, ferme Kenshi et double-clique `Installer.bat`. Kenshi est retrouvé tout seul
  dans ses bibliothèques Steam ;
- `Desinstaller.bat` enlève le mod.

Chaque joueur doit avoir **le même Kenshi (1.0.68 Steam) et les mêmes mods**, dans le même ordre.
Sinon le mod refuse la connexion, avec un message clair.

**Réseau** : le plus simple est de passer **par Steam** : rien à ouvrir, pas d'IP à donner. Dès que
l'hôte héberge, ses amis Steam peuvent le rejoindre de trois façons :
- dans leur fenêtre Multijoueur (Ctrl+Shift+M), bouton **Rejoindre** à côté de son nom ;
- clic droit sur son nom dans la liste d'amis Steam, puis **Rejoindre la partie** ;
- avec le **code Steam** de l'hôte, affiché avec un bouton Copier dans sa fenêtre Multijoueur, à
  coller dans « Adresse ou code Steam ».
Il faut être amis sur Steam pour la liste ; le code marche dans tous les cas.

Sans Steam, par IP : l'hôte doit être joignable en UDP sur le port 27960 (redirection de port sur
sa box, ou VPN de jeu type Radmin VPN, ZeroTier, Tailscale) et l'ami entre son IP.

Réglages : `KenshiCoop.ini` dans le dossier de Kenshi (créé au premier lancement).

| Section / clé | Rôle |
|---|---|
| `[player] name` | votre nom (par défaut celui de votre session Windows) ; c'est aussi le nom de votre personnage chez l'hôte. Si deux joueurs ont le même nom, le second devient « Nom 2 » |
| `[network] join_address` | IP ou code Steam (`steam:7656...`) de l'hôte, pour rejoindre |
| `[network] port` | port UDP (27960 par défaut, à ouvrir ou rediriger chez l'hôte) |
| `[coop] own_character` | 1 : chaque joueur qui rejoint reçoit son propre personnage (défaut) |
| `[ui] overlay` | petit panneau d'état en haut à droite |
| `[ui] host_console` | 1 : la console de l'hôte (fenêtre hors du jeu) s'ouvre toute seule quand on héberge |
| `[sync] ...` | réglages fins de la synchronisation (laisser les valeurs par défaut) |
| `[debug] commands` | canal de test automatisé : **laisser à 0** pour jouer |

## Jouer

Raccourcis (Kenshi au premier plan) :

| Touches | Action |
|---|---|
| Ctrl+Shift+H | héberger la partie chargée |
| Ctrl+Shift+M | fenêtre Multijoueur : héberger, rejoindre un ami Steam, liste des joueurs, outils de l'hôte |
| Ctrl+Shift+J | rejoindre l'hôte (`join_address`), depuis le menu principal ou une partie |
| Ctrl+Shift+L | quitter la session |
| Ctrl+Shift+G | (hôte) donner les personnages sélectionnés au joueur suivant |
| Ctrl+Shift+K | console dans le jeu (journal récent et commandes) |
| Ctrl+Shift+F | fenêtre Diplomatie : relations de ta faction, primes de l'escouade, guerres, chefs et villes (valeurs de l'hôte) |
| Ctrl+Shift+W | (hôte) montrer ou cacher la console hors du jeu |
| Ctrl+Shift+O | afficher ou masquer le panneau |
| Ctrl+Shift+D | écrire un diagnostic dans `KenshiCoop.log` |
| Ctrl+Shift+N | afficher ou masquer la minicarte |
| clic molette / Alt+clic | ping sur la carte, la minicarte ou le sol (Maj : danger, Ctrl : butin, Maj+Ctrl : à l'aide) |

1. L'hôte charge sa partie et appuie sur **Ctrl+Shift+H**.
2. L'ami clique **Rejoindre** à côté du nom de l'hôte dans sa fenêtre Multijoueur (ou colle son code
   Steam, ou son IP), même depuis le menu principal. Le jeu de l'hôte se met en pause le temps
   qu'il arrive ; le client télécharge le monde de l'hôte et le charge automatiquement.
3. La première fois, l'ami reçoit **son propre personnage** dans l'escouade de l'hôte, et
   **l'éditeur de personnage de Kenshi** s'ouvre chez lui : race, visage, cheveux, nom… Toute la
   partie attend pendant qu'il le crée. Le bouton « Modifier mon personnage » de la fenêtre
   Multijoueur le rouvre plus tard. Si plusieurs amis rejoignent en même temps, ils passent **un
   par un** : les autres attendent leur tour et voient leur place dans la file d'attente.
4. Quand il revient plus tard, même sous un autre nom, il **retrouve le même personnage** : il est
   reconnu par son compte Steam. L'hôte peut lui confier d'autres membres avec Ctrl+Shift+G.

Chacun ne peut commander que ses personnages. Côté client, **tous les ordres** sont transmis à
l'hôte, qui les fait exécuter par le personnage concerné : déplacement, fouille, premiers soins,
porter, dormir, portes, machines, parler… Exceptions pour l'instant : le **commerce** et le
recrutement au centre d'emploi, refusés avec un message.

**Outils de l'hôte**, dans la fenêtre Multijoueur ou la console :
- **Administration** : la liste des joueurs (nom, ping, personnages, persos à terre) avec, pour
  chacun et pour « Tout le monde » (toi compris) :
  - **Dieu** (case à cocher) : plus aucun dégât ni K.-O. ; reste actif aux changements de zone et
    quand le joueur se reconnecte ;
  - **TP moi** (ses persos près de toi), **Aller** (les tiens près des siens), **TP à…** (près d'un
    autre joueur, ou au *point marqué* : le dernier endroit où tu as ordonné un déplacement par
    clic droit au sol) ; un perso à terre est relevé pour le voyage, le bouton demande alors une
    confirmation ;
  - **Soigner** (blessures, sang, réveil d'un K.-O.) ;
  - **XP** : la compétence (ou toutes) et la quantité, en points d'expérience ou en niveaux, se
    règlent sous la liste ; au-delà de 1000 points ou 10 niveaux, confirmation ;
  - l'**argent commun** (cats ajoutés ou retirés).

  Le joueur concerné reçoit un message (« L'hôte t'a téléporté près de lui. »…). Les mêmes actions
  existent en commandes : `admin god <id|all|host> on|off`, `admin tp <id|all> host`,
  `admin tp host <id>`, `admin tp <id> <id>`, `admin tp <id> point`, `admin tp <id> <x> <y> <z>`,
  `admin xp <id|all|host> <compétence|all> <n> [levels]`, `admin heal <id|all|host>`,
  `admin money <n>`, `admin list` (tape `admin` pour l'aide). Un client n'a ni ces boutons ni ces
  commandes ;
- **TP vers moi** (`tp <id>`) : amène les personnages d'un joueur bloqué près de toi ;
- **Resync** (`resync <id>`) et **Resynchroniser tout le monde** : en cas de désynchro, le joueur
  recharge ton monde tel qu'il est, en quelques secondes ;
- **la console hors du jeu** : une fenêtre Windows avec les joueurs, la qualité de leur synchro et
  le journal en direct de **tout le monde**, clients compris.

**Quitter** : un client qui quitte (Ctrl+Shift+L ou « Quitter la session ») garde une copie du
monde, en pause. Kenshi ne sait pas revenir à son écran titre : pour reprendre une de tes parties,
fais Échap, puis Charger, ou utilise le bouton « Quitter le jeu ».

**Plantage** : si le jeu d'un client plante ou perd la connexion, l'hôte s'en rend compte en
quelques secondes, ses personnages s'arrêtent sur place et l'attendent. Il relance Kenshi et
rejoint : il retrouve ses personnages tels qu'ils sont. Pendant une session, le jeu du client ne
sauvegarde pas (tes propres parties ne sont jamais écrasées par le monde de l'hôte).
Pour signaler un plantage, joins le `KenshiCoop.log` de la machine qui a planté : sa ligne `CRASH`
dit où le jeu est mort et ce que faisait le mod à ce moment (`phase=`), en plus du
`crashDump*.zip` du jeu.

## Ce qui est synchronisé

- **Le monde** : le client joue dans une copie exacte du monde de l'hôte, sauvegarde transférée au
  moment de rejoindre. Tout est ensuite corrigé en continu depuis l'hôte.
- **Personnages** (escouade et PNJ) :
  - positions, déplacements, allure, direction ;
  - animations, combats au corps à corps, posture (debout ou au sol) ;
  - santé de chaque membre, K.-O., morts, corps portés.
  Le client n'a pas d'IA ni de dégâts propres : il ne peut pas diverger. Les PNJ que l'hôte fait
  apparaître sont recréés chez le client ; ceux que le jeu du client créerait de lui-même sont
  retirés.
- **Escouades** (noms, membres, chef, ordre des escouades, escouades vides), **noms des persos**,
  **ordres permanents** (furtif, tenir la position, passif, style de combat, allure…), **listes de
  tâches**, **compétences et expérience**, **argent**. Tout ce que tu changes dans la fenêtre
  Escouade passe par l'hôte : tu ne déplaces et ne renommes que tes persos ; les recrues que l'hôte
  n'a données à personne se règlent par tout le monde (le dernier clic gagne).
- **Chacun commande ses persos** : un ordre d'un joueur (dormir, parler, piller, construire…) n'est
  exécuté que par **ses** personnages, jamais par ceux de l'hôte ou d'un autre joueur ; un ordre
  impossible (mauvaise cible) est refusé avec un message, sans rien faire.
- **Dialogues** : les bulles chez tout le monde ; une conversation avec un PNJ (qu'on lui parle ou
  qu'il nous aborde : garde, mendiant, chasseur de primes...) s'ouvre chez le joueur concerné
  seulement (fenêtre du mod, bouton « Partir »), qui choisit ses réponses ; le jeu ne se met pas en
  pause pour une conversation. Un PNJ ne parle qu'à un joueur à la fois (« occupé »).
- **Heure, vitesse et pause** : celles de l'hôte, imposées en permanence. Les clients ne peuvent
  pas les changer ; un client figé un moment (une zone qui charge) est remis à l'heure de l'hôte
  d'un coup.
- **Météo** (pluie, tempêtes, éclairs, nuages de gaz…) : celle de l'hôte, région par région.
- **Inventaires et équipement** de tous les personnages proches.
- **Fouille** : clic droit « Fouiller » sur un personnage K.-O. ou mort, ou sur un **coffre, une
  étagère ou un coffre-fort**. Ton personnage y va, puis la fenêtre du jeu s'ouvre ; chaque objet
  déplacé est rejoué par l'hôte.
- **Vol** : prendre dans un contenant qui n'est pas à toi est un vol, décidé par le jeu de l'hôte.
  Si on te voit, la fenêtre se ferme et l'objet reste.
- **Objets au sol** : posés et ramassés, au même endroit pour tout le monde.
- **Cadavres** : restent synchronisés et se fouillent, près des joueurs.
- **Carte et repères** : sur la carte du jeu (onglet CARTE de la fenêtre de gestion, bouton MAP), tous les persos de tous les joueurs à la couleur
  de leur joueur et les escouades hostiles qui nous visent en rouge ; une minicarte ronde ; un
  repère au-dessus de la tête des persos des joueurs ; leur cadre de portrait coloré ; des pings
  partagés. Réglages : fenêtre Multijoueur, « Affichage ».

Détail fonctionnalité par fonctionnalité, avec ce qui est vérifié et ce qui reste à faire :
[docs/FONCTIONNALITES.md](docs/FONCTIONNALITES.md).

## Limites connues

- **Pas encore synchronisés** :
  - commerce : vérifié en ville ; avec les caravanes (marchands ambulants, sacs à dos portés) à vérifier en jeu ;
  - achat de bâtiments, construction et pose de meubles ;
  - état des portes et crochetage ;
  - prisons et esclavage ;
  - combat à distance (projectiles, tourelles) ;
  - relations de factions, primes et diplomatie (guerres entre factions, chefs, villes) :
    implémentés, à vérifier en jeu ; leur état chez l'hôte est dans la fenêtre Diplomatie (Ctrl+Shift+F) ;
  - recherche, établis de fabrication, mines et machines, énergie des avant-postes : implémentés
    (l'hôte décide, les boutons d'un client deviennent des demandes), à vérifier en jeu.
- Côté client, ne pas poser de bâtiment pour l'instant : il n'existerait probablement que chez
  toi. (Une pose qu'un client fait quand même est vérifiée par l'hôte comme le mode construction
  vérifie un endroit : dans l'eau ou l'acide, trop près d'une ville… elle est refusée et le
  joueur reçoit la raison.)
- Les corps au sol peuvent reposer à quelques dizaines de centimètres (rarement plus d'un mètre)
  de leur position chez l'hôte : la chute du ragdoll est simulée par chaque PC.
- Autour des joueurs, les positions sont exactes. Les PNJ éloignés qui marchent peuvent être
  décalés de quelques dizaines de centimètres à 1 ou 2 m : le jeu ne met à jour leur position que
  quelques fois par seconde.
- Quelques PNJ uniques ne peuvent pas être recréés chez le client s'il ne les a pas déjà (le
  panneau l'indique : « N not spawned here yet »).
- Le TP de l'hôte ne déplace pas un personnage au sol (assommé, en ragdoll).

## Dépannage

- Journal : `KenshiCoop.log` dans le dossier de Kenshi (`KenshiCoop-<pid>.log` si deux Kenshi
  tournent). Les sessions précédentes sont dans `KenshiCoop-logs\` (les 30 dernières). L'hôte voit
  aussi les lignes de chaque client dans son propre journal et dans sa console hors du jeu. Voir
  [docs/JOURNAUX.md](docs/JOURNAUX.md).
- « version/mods différents » : vérifier que les deux joueurs ont exactement les mêmes mods.
- Rien ne se passe en rejoignant par IP : vérifier l'IP, le port UDP 27960 ouvert chez l'hôte et
  le pare-feu. Par Steam : lancer le jeu depuis Steam, et être amis (ou utiliser le code Steam).
- Désynchro visible : bouton **Resync** de l'hôte.

## Documentation

Dans le dépôt seulement : comme documentation, le zip des amis ne contient que ce fichier, sous le
nom `LISEZMOI.md`.

| Document | Contenu |
|---|---|
| [docs/FONCTIONNALITES.md](docs/FONCTIONNALITES.md) | état de chaque fonctionnalité : vérifié, à vérifier, en cours, à faire |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | comment le mod marche : hooks, session, messages, flux par système, fichiers |
| [docs/MOTEUR.md](docs/MOTEUR.md) | notes sur le moteur de Kenshi 1.0.68 : adresses, structures, comportements |
| [docs/TESTS.md](docs/TESTS.md) | tests unitaires, harnais de test en jeu, commandes de debug |
| [docs/JOURNAUX.md](docs/JOURNAUX.md) | journaux, console de l'hôte, lecture et diagnostic |
| [docs/INVENTAIRE.md](docs/INVENTAIRE.md) | inventaire du code : systèmes, messages, hooks, adresses, commandes, risques |
| [docs/A_FAIRE.md](docs/A_FAIRE.md) | bugs signalés à traiter |
| [CHANGELOG.md](CHANGELOG.md) | historique des changements |

## Développement

- `build\bin\Release\kc_tests.exe` : tests du protocole et de la session, avec un monde simulé.
- `tools\coop_test.py` : lance plusieurs Kenshi sur ce PC, les pilote via le canal de debug
  (`[debug] commands=1`) et compare leurs mondes. **Il ferme tous les Kenshi en cours** : ne pas
  le lancer pendant une partie. Exemples :
  - `python tools\coop_test.py suite` (tout, point par point) ;
  - `... four` (1 hôte + 3 clients) ;
  - `... soak --minutes 15`.
  Détails dans [docs/TESTS.md](docs/TESTS.md).
