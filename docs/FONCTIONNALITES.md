# État des fonctionnalités

État au 9 octobre 2026 (dernier commit : `c5ba75a`, plus le travail en cours décrit à la fin).
Chaque section dit ce que voit le joueur, comment ça marche en deux mots, et les limites connues.

| Symbole | Sens |
|---|---|
| ✅ | vérifié (suite de tests automatique, expérience du harnais ou partie réelle) |
| 🟡 | implémenté, reste à vérifier en jeu |
| 🔧 | en cours d'écriture |
| ❌ | pas encore fait |

Principe général : **l'hôte est seul à simuler**. Les clients jouent dans une copie du monde de
l'hôte et n'ont ni IA, ni dégâts, ni décisions propres : tout ce que leur jeu voudrait décider
seul est refusé par des hooks, et tout ce qui compte arrive de l'hôte (voir
[ARCHITECTURE.md](ARCHITECTURE.md)).

Résultats de la suite automatique (`python tools/coop_test.py suite`, détail dans [TESTS.md](TESTS.md)) :
- 9 oct., 19 h 05 : **31/32** (seul échec : retirer les ordres permanents, corrigé depuis) ;
- 9 oct., 19 h 34 : **26/32**. Pendant ce test, des pillards ont attaqué l'escouade dans la
  sauvegarde, ce qui fausse 5 des 6 échecs (orientation et horloge d'animation à vitesse 3, TP
  admin sur un personnage à terre, positions finales). Le 6ᵉ a révélé un vrai bug (KO côté
  client), en cours de correction (voir « En cours »).

---

## Rejoindre une partie

### Connexion et transfert du monde ✅
- **Le joueur** rejoint depuis le menu principal ou depuis une partie chargée (fenêtre
  Multijoueur, Ctrl+Shift+M). L'hôte se met en pause le temps de l'arrivée. Le client télécharge
  le monde de l'hôte (barre de progression) et le charge tout seul.
- **Fonctionnement** : l'hôte gèle son monde, le sauvegarde dans l'emplacement `KenshiCoopHost`,
  puis envoie les fichiers par morceaux de 16 Ko. Le client les écrit dans `KenshiCoopJoin`, les
  charge, puis vérifie l'empreinte du monde (les handles de l'escouade). Il annonce ensuite
  `Ready` et reçoit tous les personnages.
- **Contrôles** à la connexion :
  - version du protocole ;
  - même `kenshi_x64.exe` (SHA-256) ;
  - même `data\mods.cfg` (mêmes mods, même ordre) ;
  - nom valide ;
  - place libre (8 joueurs au plus, hôte compris).
- **Garde-fous** :
  - l'hôte reste en pause même si quelqu'un appuie sur lecture pendant une arrivée ;
  - un joueur qui arrive pendant la sauvegarde pour un autre déclenche une nouvelle sauvegarde,
    qui contient son personnage ;
  - délais : 120 s pour la sauvegarde de l'hôte, 300 s pour le téléchargement et le chargement
    côté client.
- **Limites** : tous les joueurs doivent avoir la même version de Kenshi (1.0.68 Steam) et les
  mêmes mods.

### Jouer via Steam ✅ — par IP ✅
- **Le joueur** voit ses amis Steam qui hébergent dans la fenêtre Multijoueur (bouton
  **Rejoindre**). Il peut aussi passer par « Rejoindre la partie » dans la liste d'amis Steam, ou
  coller le code Steam de l'hôte. Rien à ouvrir sur la box.
- **Fonctionnement** : le trafic UDP de la session passe par le P2P de Steam
  (`ISteamNetworking`, relayé par les serveurs Steam au besoin). La présence enrichie
  (`kc_host`, `connect`) annonce la partie aux amis. Par IP, l'hôte doit ouvrir le port UDP
  27960 (ou utiliser un VPN de jeu).
- Testé en vraie partie entre amis le 9 octobre.

### Partie à 4 joueurs ✅
- Expérience `four` du harnais (1 hôte + 3 clients, 9 oct., 15 h 37) :
  - aucun écart de santé, de combat ni d'inventaire, entre l'hôte et chaque client comme entre
    clients ;
  - positions identiques à 0,01 près hors combat ;
  - écarts de plusieurs dizaines d'unités sur les corps tombés après un combat. Ce test date
    d'avant les correctifs de la soirée : à refaire.

### Un personnage par joueur, éditeur de personnage ✅
- **Le joueur** qui rejoint pour la première fois reçoit un personnage à son nom dans l'escouade
  de l'hôte, et l'éditeur de personnage de Kenshi s'ouvre chez lui : race, sexe, visage, cheveux,
  curseurs et nom. **Toute la partie est en pause** pendant qu'il le crée (10 minutes au plus).
  À la validation, tout le monde voit le résultat. Le bouton « Modifier mon personnage » de la
  fenêtre Multijoueur rouvre l'éditeur.
- **Fonctionnement** :
  - l'hôte crée la recrue avant de sauvegarder le monde pour le joueur (même espèce que le premier
    membre de l'escouade) ;
  - le client ouvre `ForgottenGUI::showCharacterEditor` en mode « debug » (race et sexe
    modifiables, rien de tiré au hasard) ;
  - à la validation (hook `closeCharacterEditor`), toutes les valeurs d'apparence et le nom vont à
    l'hôte, qui les applique et les renvoie aux autres joueurs.
- Vérifié par la suite (« arrivée », « création de perso : pause pendant l'édition / levée à la
  validation »).

### Reconnexion : on retrouve son personnage ✅
- **Le joueur** qui revient, même sous un autre nom, retrouve le même personnage. S'il relance
  son jeu avant que l'ancienne connexion ait expiré, la nouvelle remplace l'ancienne : pas de
  personnage en double.
- **Fonctionnement** : le client envoie son identifiant Steam. L'hôte tient
  `KenshiCoop-players.txt` (dossier de Kenshi), une ligne par compte :
  `<steamId> <handle du perso> <nom>`. À défaut, il prend un membre de l'escouade qui porte le
  nom du joueur et n'appartient à aucun autre compte ; sinon il crée un nouveau personnage.
- Vérifié par la suite (« reconnexion »).

---

## Personnages

### Positions, déplacements, allure ✅
- **Le joueur** voit chaque personnage au même endroit que chez l'hôte, qui marche ou court comme
  chez l'hôte.
- **Fonctionnement** :
  - instantanés (snapshots) 20 fois par seconde ;
  - le client garde 50 ms de retard et interpole entre deux instantanés, sans extrapolation ;
  - la locomotion du jeu anime le personnage vers la destination de l'hôte, et sa position est
    tirée en continu vers celle de l'hôte ;
  - au-delà de `snap_distance` (15 unités par défaut), téléportation directe.
- **Mesuré** : personnages immobiles exacts à 0,01 unité près (1 unité ≈ 10 cm).

### Orientation ✅ (vitesse 1) — 🟡 (vitesse 3)
- La suite compare maintenant la rotation du corps (ce que voient les joueurs) et ignore les
  échantillons en combat ou à terre : l'échec à 178° venait d'un personnage pris dans l'attaque des
  pillards. Le compteur de l'horloge d'animation ne compte plus les personnages en combat ou à
  terre (leur horloge est toujours corrigée).
- **Le joueur** voit un personnage qui marche regarder dans le même sens que chez l'hôte. Ce
  bug, signalé par les amis, est corrigé.
- **Fonctionnement** : le client impose la direction de l'hôte :
  - exactement à l'arrêt et en combat ;
  - en marchant, dès que l'écart dépasse environ 25° ;
  - en combat, le hook `combatMovementUpdate` remplace en plus la direction que le jeu du client
    calcule.
- **Mesures de la suite** :
  - vitesse 1 : pire écart 2° (19 h 05) et 13° (19 h 34) ;
  - vitesse 3 : 6° à 19 h 05 ; échec à 178° à 19 h 34, pendant l'attaque de pillards. À relancer.

### Animations ✅
- **Le joueur** voit les mêmes coups, parades, esquives, actions (s'asseoir, soigner…), trébuchés,
  armes en main et gardes que chez l'hôte. Le cycle de marche est en phase.
- **Fonctionnement** :
  - événements d'animation (fiables) ;
  - plus, 15 fois par seconde, la liste de tout ce que joue chaque personnage à moins de 150 m
    d'un membre de l'escouade (nom, temps, poids, vitesse) ;
  - le client ne choisit plus d'animation pour ces personnages : il joue celles de l'hôte ;
  - l'horloge des cycles de marche se recale en douceur : saut seulement si l'écart est grand,
    sinon 10 % de l'écart par image ;
  - les chiffres de dégâts de l'hôte s'affichent aussi chez les clients.
- **Mesures** : horloge corrigée sur 1 image sur 15 000 à vitesse 1 (suite de 19 h 05 : 2 sur
  14 466). À vitesse 3, 168 sur 17 882 à 19 h 05, mais 4 % à 19 h 34, pendant le combat.

### PNJ bloqué chez le client (mur, porte, autre étage) 🟡 implémenté, à vérifier en jeu
- **Le joueur** ne voit plus un PNJ coincé dans une maison chez lui alors qu'il se bat dehors chez
  l'hôte.
- **Fonctionnement** : le client suit la progression de chaque personnage vers la position de
  l'hôte. Si l'écart reste au-dessus de 3 unités (× la vitesse du jeu) sans diminuer d'au moins
  20 % pendant 1 s (un mur, une porte fermée ou un autre étage bloque la marche et le rappel), il
  le téléporte exactement à la position de l'hôte, combat compris. Les personnages à terre ne sont
  pas concernés (ragdoll). Ligne de journal `stuck: ... put there`.
- Test : expérience `stuck` (la copie d'un PNJ est poussée dans un mur ou sous le sol chez le
  client seulement).

### Combat au corps à corps ✅ — désynchro signalée 🟡
- **Le joueur** voit chaque combattant attaquer la même cible que chez l'hôte. Les coups sont
  animés localement, l'issue (blessures, KO, mort) vient de l'hôte.
- **Fonctionnement** : la cible de combat voyage dans les instantanés, et le client engage la
  même cible (`initCombatMode`).
- **Limite signalée par les amis** : un PNJ bloqué dans une maison chez un client alors qu'il
  était dehors chez l'hôte. Pas encore reproduit ni corrigé.

### Santé, K.-O., mort ✅ (un correctif en cours 🔧)
- Un personnage dans un lit ou une cage (le sommeil est un K.-O.) reçoit seulement l'état
  inconscient chez le client, jamais la chute qui le sortirait de là ; idem s'il est porté.
- **Le joueur** voit chaque blessure de membre, saignement, faim, K.-O. et mort comme chez l'hôte.
  Un personnage tombe là où celui de l'hôte est tombé.
- **Fonctionnement** :
  - les valeurs de santé partent 5 fois par seconde ;
  - le client les réimpose toutes les 0,25 s, parce que sa simulation locale (saignement,
    soins) les fait dériver ;
  - les clients ne décident jamais seuls d'un dégât, d'un K.-O., d'une chute ou d'une mort : les
    hooks les refusent.
- **En cours** : le K.-O. imposé par l'hôte n'armait côté client qu'un minuteur, que les valeurs
  de l'hôte écrasaient aussitôt. Le personnage restait debout quelques secondes. Le client impose
  désormais l'état inconscient et la chute directement (pas encore commité ni testé).

### Porter un corps ✅
- **Le joueur** voit le même corps sur la même épaule partout. Un client ne peut pas ramasser un
  corps lui-même : c'est l'hôte qui exécute l'ordre « porter ».
- Vérifié par la suite (« porter »).
- **Limite** : le mod n'ajoute aucun garde-fou propre, par exemple contre le fait de porter
  quelqu'un qui en porte un autre (signalé par les amis). Ce sont les règles du jeu de l'hôte qui
  s'appliquent. À vérifier en jeu.

### Population de PNJ, cadavres ✅
- **Le joueur** voit les mêmes PNJ que l'hôte, y compris ceux apparus après la sauvegarde
  (escouades errantes, renforts). Les corps restent au même endroit et se fouillent.
- **Fonctionnement** :
  - un PNJ absent chez le client est recréé à partir de son modèle et de sa faction ;
  - un PNJ que le jeu du client crée de lui-même remplace un PNJ manquant de même type (PNJ
    uniques), sinon il est retiré ;
  - les cadavres à moins de 1000 unités d'un joueur restent synchronisés.

---

## Escouade et progression

### Escouades ✅
- **Le joueur** voit les mêmes escouades, avec les mêmes noms et le même ordre, chez tout le
  monde. Un client qui glisse son portrait vers une autre escouade le demande à l'hôte. Les
  personnages des autres joueurs ne bougent pas.
- L'hôte organise les escouades de tout le monde.
- Vérifié avec l'expérience `squads`, puis confirmé en jeu.
- ⚠ Les deux points « escouades » de la suite passent aujourd'hui sans rien tester : ils
  utilisent un mauvais nom de personnage (voir [TESTS.md](TESTS.md)).

### Ordres permanents et style de combat ✅
- **Concerne** : furtif, bloquer, tenir la position, passif, narguer, poursuivre, distance ;
  attaque / défense / esquive.
- **Le joueur** voit les mêmes cases cochées partout. Le jeu du client ne peut plus les changer
  seul (avant, un personnage passait parfois en furtif tout seul).
- Vérifié par la suite (« modes »).

### Compétences, expérience, argent ✅
- **Le joueur** voit les mêmes niveaux et les mêmes cats partout. Les clients ne gagnent pas
  d'expérience eux-mêmes : tout gain passe par `increaseStat`, refusé chez eux, et les valeurs de
  l'hôte arrivent chaque seconde.
- Vérifié par la suite (« XP », « argent »).

---

## Dialogues

### Bulles de dialogue ✅
- **Le joueur** voit, chez tout le monde, ce qu'un personnage dit à voix haute.
- Vérifié par la suite.

### Conversations ✅
- **Le joueur** voit la fenêtre de conversation d'un PNJ qui parle à son personnage s'ouvrir
  **chez lui seulement** (fenêtre de l'overlay) : texte, puis réponses numérotées. Sa réponse
  part à l'hôte, qui fait avancer la conversation. Chez l'hôte, la fenêtre reste fermée.
- **Fonctionnement** : toutes les conversations se déroulent dans le monde de l'hôte. Les clients
  ne montrent jamais la fenêtre de dialogue du jeu.
- Vérifié en ville (`kctest_town`, conversation avec le chef des voleurs Shinobi).

---

## Ordres et objets

### Ordres donnés par un client ✅ / 🟡 selon l'ordre
- **Le joueur** donne ses ordres comme d'habitude : premiers soins, suivre, manger, attaquer,
  porter, ouvrir une porte, dormir, utiliser une machine, parler… L'ordre part à l'hôte, qui le
  fait exécuter par **ce personnage seul**, avec les fonctions d'ordre du jeu. La sélection,
  l'escouade affichée et le panneau de détails de l'hôte reviennent exactement comme avant.
- **Meubles, machines et maisons** : leurs handles diffèrent d'une machine à l'autre. L'hôte
  retrouve donc l'objet « du même type au même endroit ».
- **État par ordre** :
  - ✅ déplacement (expérience `four`), porter et ordres permanents (suite), ramasser (expérience
    `clientpickup`) ;
  - 🟡 arrêt (pas de test dédié) ;
  - 🟡 dormir dans un lit et miner : l'hôte vérifie maintenant que le handle envoyé par le client
    désigne bien un objet du même type au même endroit (sinon il le cherche par type et endroit :
    un handle de meuble peut désigner un autre objet chez l'hôte). Test : expérience `beds`
    (lit libre le plus proche, puis mine), commandes `bedreq` et `minereq` ;
  - 🟡 miner ou utiliser une machine (bug signalé par les amis, corrigé depuis par la recherche
    « type + endroit ») ;
  - 🟡 les autres ordres.
- **Refusés côté client** : recruter au centre d'emploi (55), commerce (118, 119), et les tâches de
  stockage des PNJ (124, 284). Le joueur voit le message « Commercer et ouvrir un coffre ne sont
  pas encore synchronises. » (texte à mettre à jour : les coffres marchent, voir plus bas).

### Objets au sol ✅
- **Le joueur** voit un objet posé par l'hôte au même endroit chez tout le monde, puis disparaître
  partout quand quelqu'un le ramasse. Un client qui ramasse ou dépose le demande à l'hôte : son
  personnage va chercher l'objet chez l'hôte.
- Vérifié par les expériences `ground` (objets posés puis ramassés par l'hôte) et `clientpickup`
  (ramassage demandé par un client).
- 🟡 Un objet qui traînait déjà dans la sauvegarde a un autre handle chez chaque joueur : le client
  le retrouve par type et endroit (à 15 unités près). C'est implémenté, mais aucun test ne l'a
  confirmé.

### Inventaires, équipement, fouille des corps ✅
- **Le joueur** voit le même inventaire et le même équipement partout. Il peut fouiller un
  personnage K.-O. ou mort par clic droit : son personnage s'y rend via l'hôte, puis la fenêtre de
  fouille du jeu s'ouvre chez lui.
- **Fonctionnement** : chaque objet déplacé dans une fenêtre d'inventaire du client devient une
  demande de déplacement (`InvOp`) que l'hôte rejoue. L'hôte refuse de vider un PNJ conscient ou
  le personnage d'un autre joueur, et renvoie alors l'état réel.
- **Limite** : un objet tenu à la souris pendant un glisser-déposer laisse 10 s avant le retour à
  l'état de l'hôte.

### Contenants (coffres, étagères, coffres-forts) et vol ✅ — vol repéré 🟡
- **Le joueur** fait clic droit sur un contenant. Son personnage le plus proche y va via l'hôte,
  puis la fenêtre de fouille du jeu s'ouvre chez lui sur une copie du contenu de l'hôte. Il peut
  prendre, déposer ou voler.
- **Fonctionnement** :
  - l'hôte retrouve le contenant par type et endroit ;
  - il lui attribue un identifiant réseau et n'envoie son contenu qu'aux joueurs qui l'ont ouvert ;
  - prendre dans un contenant qui n'est pas à nous est un vol, décidé par le jeu de l'hôte avec ses
    propres règles : le crime est enregistré et les propriétaires peuvent s'en apercevoir ;
  - si le joueur est vu, la fenêtre se ferme (« Pris en train de voler ! ») et l'objet reste ;
  - la fenêtre se ferme aussi si le personnage s'éloigne (« trop loin »).
- **Vérifié en ville** (`kctest_town`) : un « Grand Panier » des Shinobi, objet volé sans être
  vu, contenant vide des deux côtés. Le cas « pris en flagrant délit » n'a pas encore été vérifié
  en jeu.
- **Limite** : chez le jeu, prendre dans un bâtiment de catégorie « CAMPING » n'est pas un vol.
  Le test de vol du mod ne reprend pas explicitement cette exception. Il ne compte pas de vol quand
  le contenant n'a pas de propriétaire : ce cas est peut-être couvert, à vérifier.

---

## Monde

### Heure, vitesse, pause ✅
- **Le joueur** a l'heure, la vitesse et la pause de l'hôte. Chez un client, espace et F2/F3/F4
  n'ont pas d'effet durable.
- Quand l'hôte met en pause, chaque client tourne encore 0,3 s au ralenti pour poser tout le monde
  exactement où l'hôte s'est arrêté, puis se met en pause : le jeu n'enregistre aucune position
  pendant la pause.
- Vérifié par la suite (« pause en pleine course », 0,000 à 0,024 d'écart).

### Météo et effets météo ✅
- **Le joueur** a la même météo par région, ainsi que les mêmes éclairs, rayons, tempêtes et
  nuages de gaz, au même endroit. Ce qu'un effet inflige est décidé par l'hôte.
- Vérifié en jeu : 65 régions sur 65 identiques, y compris après un changement forcé.

---

## Outils de l'hôte

### Resynchroniser ✅
- **L'hôte** dispose de boutons « Resync » (par joueur) et « Resynchroniser tout le monde » dans
  la fenêtre Multijoueur, ou de la commande `resync [id]`. Le joueur quitte la session puis la
  rejoint aussitôt : il recharge le monde de l'hôte tel qu'il est maintenant et retrouve son
  personnage.
- Vérifié par la suite.

### Téléportation admin (« TP vers moi ») ✅ — perso à terre ou porté 🟡 implémenté, à vérifier en jeu
- Un personnage à terre (K.-O.) est désormais déplacé aussi : il quitte le ragdoll, est téléporté,
  puis se recouche 0,5 s plus tard s'il est toujours inconscient (un ragdoll lancé juste après une
  téléportation serait projeté). Un personnage porté par quelqu'un est d'abord posé.
- Test : expérience `tpdown`.
- **L'hôte** dispose d'un bouton « TP vers moi » par joueur, ou de la commande
  `tp <id> [vers <id>]`. Les personnages du joueur arrivent à côté du personnage sélectionné de
  l'hôte (ou de ceux d'un autre joueur) : pratique pour débloquer quelqu'un.
- Vérifié par la suite (31/32 à 19 h 05).
- **Limite** : ne déplace pas un personnage à terre. Le moteur ne téléporte pas un ragdoll actif,
  ce qui explique l'échec de 19 h 34.

### Quitter la session 🟡 (vérifié en test, pas encore entre amis)
- **Le joueur** qui quitte, ou qui perd l'hôte, garde une copie du monde **en pause**. La fenêtre
  Multijoueur explique qu'il ne joue plus dans la partie de l'hôte et propose « Quitter le jeu ».
  Kenshi ne sait pas revenir à son écran titre ; pour reprendre une de ses parties : Échap, puis
  Charger.

### Journaux et console de l'hôte ✅ (accents 🔧)
- **L'hôte** dispose d'une fenêtre Windows **hors du jeu** (Ctrl+Shift+W, ouverte automatiquement
  quand il héberge) :
  - la liste des joueurs (ping, personnages, état, qualité de la synchro) ;
  - le journal en direct, avec **les lignes de chaque client** ;
  - une ligne de commande.
- Les journaux sont en anglais et ceux des sessions précédentes sont archivés. Détails dans
  [JOURNAUX.md](JOURNAUX.md).
- **En cours** : les accents de cette fenêtre s'affichaient mal (« Ã‰tat ») ; correction par la
  compilation des sources en UTF-8.

---

## En cours et à faire

### Commerce 🔧
- **Demande** : un commerce cohérent. Si un joueur achète un objet, le marchand ne doit plus le
  proposer à personne.
- **Ce qui se passe aujourd'hui** :
  - côté client, l'ordre « commercer » est refusé ;
  - l'action « Commercer » d'une conversation tourne chez l'hôte et ouvrirait la fenêtre sur
    l'écran de l'hôte.
- **Conception en cours** :
  1. L'hôte intercepte `showTradeWindow` pour le personnage d'un autre joueur.
  2. Il envoie à ce joueur le marchand et ses comptoirs (meubles `BF_SHOP`), traités comme des
     contenants ouverts.
  3. Le client ouvre la vraie fenêtre de commerce du jeu sur ces copies.
  4. Chaque achat ou vente part à l'hôte avec son prix, calculé par le client : le prix dépend
     d'un état de l'interface que l'hôte n'a pas.
  5. L'hôte vérifie, déplace l'objet et l'argent, puis envoie le nouveau stock à tous les joueurs
     qui ont ouvert ce marchand.

### Achat de bâtiments ❌

### Construction et meubles ❌
- **Demande** : tout le monde, clients compris, peut construire et placer des meubles.
- **État** :
  - l'ordre « construire » d'un client part bien à l'hôte, mais l'avancement du chantier n'est
    pas synchronisé ;
  - la pose d'un bâtiment n'est pas interceptée : un bâtiment posé par un client n'existerait a
    priori que chez lui (non testé, à éviter pour l'instant).

### Portes, serrures, crochetage 🟡 (implémenté, à vérifier en jeu)
- **Le joueur** voit chaque porte comme chez l'hôte : ouverte ou fermée (avec l'animation et le son
  du jeu), verrouillée ou non, défoncée. Les serrures des meubles (coffres, cages, chaînes) suivent
  aussi : un coffre verrouillé chez l'hôte l'est chez tout le monde.
- **Ses actions** :
  - ordres sur une porte (ouvrir, fermer, crocheter, verrouiller, déverrouiller, défoncer) : comme
    tous les ordres, ils partent à l'hôte, dont le jeu les exécute avec le personnage du joueur ;
    le résultat (porte ouverte, serrure crochetée ou non, expérience) revient comme tout changement ;
  - boutons du panneau d'une porte (« Ouvrir / Fermer », « Verrouiller ») : le clic part à l'hôte,
    qui appuie sur le même bouton dans son jeu ;
  - un coffre verrouillé ne s'ouvre pas pour un client : message « C'est verrouillé : il faut
    d'abord crocheter la serrure. ».
- **Fonctionnement** :
  - toutes les 0,5 s, l'hôte lit les portes et serrures à moins de 40 m de chaque membre de
    l'escouade et envoie celles qui ont changé ; toutes les 5 s, il les envoie toutes ;
  - le client retrouve chaque objet par type et endroit (les handles diffèrent d'une machine à
    l'autre) et impose l'état de l'hôte, puis le réimpose toutes les 3 s (zone chargée plus tard,
    changement local) ;
  - chez un client, le jeu ne peut plus ouvrir, fermer, verrouiller ni déverrouiller une porte de
    lui-même (hooks sur `openDoor`, `closeDoor`, `lockDoor`, `unlockDoor`).
- **À vérifier en jeu** (expérience `doors`) : que les portes sont bien dans la grille des
  bâtiments de la zone (sinon elles ne seraient pas trouvées), le crochetage par un client, le
  bouton du panneau, le coffre verrouillé.
- **Limites** : une porte fermée chez l'hôte peut rester ouverte un court instant chez le client
  (le temps du message) ; un personnage du client ne passe jamais une porte que l'hôte a laissée
  fermée.

### Prisons, cages, chaînes, esclavage, peines 🟡 (implémenté, à vérifier en jeu)
- **Le joueur** voit chaque personnage enfermé, enchaîné ou réduit en esclavage comme chez
  l'hôte : dans la même cage, dans la pose de la cage, avec les menottes, le statut d'esclave (et
  sa faction maître), l'état « prisonnier évadé » ou « enlevé », et la peine de prison en cours.
  À la libération, tout revient à la normale partout. Cela vaut pour les joueurs comme pour les
  PNJ (prisonniers des camps d'esclavagistes, des prisons des villes).
- **Fonctionnement** :
  - l'hôte relit deux fois par seconde la captivité de chaque personnage suivi et envoie ce qui
    change (`Captives`), plus tous les captifs toutes les 5 s (joueur arrivé en retard, perte) ;
  - le client retrouve la cage chez lui « par type et endroit » et y met son personnage avec la
    fonction du jeu (`setPrisonMode`) : position, pose, occupation de la cage ;
  - les autres états (menottes, propriétaire, esclavage, faction, évadé, enlevé, peine) sont
    écrits directement : l'objet « menottes » arrive, lui, avec l'inventaire ;
  - le jeu du client ne peut plus mettre ni sortir de cage, enchaîner ou changer le statut
    d'esclave de lui-même (hooks) ; un personnage que l'hôte tient en cage n'est plus corrigé en
    position (pas de bagarre de position), et un K.-O. en cage ne le fait pas tomber hors de la
    cage.
- **Évasions et libérations** : crocheter la cage ou les menottes, s'évader, ouvrir la cage d'un
  autre, le porter dehors : ce sont des ordres, qui partent déjà à l'hôte et y sont exécutés par
  son jeu ; le changement d'état revient ensuite chez tout le monde. (L'état des serrures
  elles-mêmes relève du lot « portes et serrures ».)
- **Limites / à vérifier en jeu** : l'animation exacte en cage (celle du meuble) ; le cas d'une
  cage introuvable chez le client (journalisé une fois) ; les champs de la peine de prison
  (`BountyManager` +0x98 / +0xA0) sont déduits de la disposition voisine vérifiée, pas lus en jeu.

### IA hostile qui porte et emprisonne les joueurs 🟡 (implémenté, à vérifier en jeu)
- **Porter** : synchronisé pour tous les personnages, PNJ compris (vérifié dans le code : la
  cible portée part dans les instantanés pour chaque entité, et le client la porte pareil).
- **Emprisonner, enchaîner, réduire en esclavage** : c'est le jeu de l'hôte qui le fait ; l'état
  arrive chez les clients comme ci-dessus. Le personnage du joueur reste là où l'hôte le garde ;
  les ordres de son joueur partent à l'hôte, dont le jeu décide (il refuse de sortir d'une cage
  fermée).

### Dégâts à distance : arcs, arbalètes, tourelles ❌
- Les dégâts eux-mêmes viennent de l'hôte (santé synchronisée) et les animations sont reproduites.
- Les projectiles, l'utilisation des tourelles par un client et les animations propres au tir ne
  sont ni traités ni vérifiés.

### Factions, relations, primes, crimes 🟡 (implémenté, à vérifier en jeu)
- **Le joueur** voit partout les mêmes relations de sa faction avec chaque faction (écran des
  factions : valeur, alliance, guerre, paix), dans les deux sens (ce que chaque faction pense de la
  faction du joueur compte pour les PNJ et l'interface). Il voit aussi le même rang et la même
  réputation, et, pour chaque personnage de l'escouade, les mêmes primes (montant par faction,
  crimes), le crime en cours, la peine de prison restante et le laissez-passer.
- **Fonctionnement** :
  - l'hôte relit chaque seconde les relations de la faction du joueur et les primes de l'escouade ;
    il envoie `Factions` / `Bounties` dès qu'une valeur change, à tout le monde, plus un envoi
    complet toutes les 15 s et à chaque joueur qui arrive ;
  - le client impose ces valeurs à son jeu (écriture directe des données de relation et de prime ;
    une relation ou une prime absente est créée avec les fonctions du jeu), à l'arrivée du message
    puis toutes les 2 s : si le jeu local les a changées, elles reviennent à celles de l'hôte ;
  - les hooks existants empêchent toujours le jeu du client de décider seul d'un crime, d'une prime
    ou d'un changement de relation ; ce qui vient de l'hôte (dialogue « payer sa prime », guerre
    déclarée, prime fixée par un garde) arrive par ces messages ;
  - le journal de l'hôte note chaque changement (« relations: … now -80 at war », « bounty: … wanted
    by … for 1500 cats », « crime: … », « prison: … »).
- **Limites** :
  - une prime que l'hôte n'a plus est vidée (montant et crimes à 0) chez le client plutôt que
    retirée : l'interface ne l'affiche plus, comme chez l'hôte ;
  - les relations entre deux factions qui ne sont pas celle du joueur ne sont pas envoyées (elles
    ne changent que l'IA, qui tourne chez l'hôte) ;
  - la victime d'un crime en cours (un PNJ précis) n'est pas envoyée, seulement la faction.
- **Test en jeu** : `python tools/coop_test.py factions` (voir [TESTS.md](TESTS.md)).

### Précision des PNJ lointains 🟡 implémenté, à vérifier en jeu
- Loin de l'escouade du client (plus de 300 unités), le jeu ne déplace un personnage que quelques
  fois par seconde et garde sa propre idée de sa position : le rappel progressif n'y tient pas. Le
  client le replace donc sur la position de l'hôte (téléportation) jusqu'à 4 fois par seconde dès
  qu'il s'en écarte de plus d'1 unité.
- Test : expérience `farnpc` (pire écart des marcheurs lointains, objectif < 3 unités).

### Joueurs éloignés, précision des PNJ lointains 🟡
- Avec `interest_radius=0` (défaut), l'hôte réplique **chaque** personnage actif de son jeu, où
  qu'il soit. Un joueur à l'autre bout de la carte garde donc sa zone simulée par l'hôte.
- L'expérience `far` du harnais compare un combat à environ 5 km de l'escouade de l'hôte avec le
  même combat à côté.
- **Limite connue** : le jeu ne déplace les PNJ lointains que quelques fois par seconde. Ceux qui
  marchent loin des joueurs peuvent être décalés de quelques dizaines de centimètres à 1 ou 2 m.

### Non vérifiés
- Recrutement par dialogue.
- Artisanat et production des machines.
- Ces actions tournent chez l'hôte, mais rien ne garantit encore que leur résultat apparaît
  correctement chez les clients.
