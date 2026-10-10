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
  - **file d'attente** : les joueurs arrivent un par un (sauvegarde, téléchargement, chargement,
    éditeur de personnage). Les autres attendent, connectés, et voient leur place, le joueur en
    cours et son étape (« File d'attente : position 2/3 — en attente de Joueur2 (création du
    personnage)… »), mise à jour en direct. L'hôte voit la file dans son panneau. Au tour suivant,
    une sauvegarde neuve contient tous ceux arrivés avant. Un joueur qui part ou plante pendant
    son tour passe la main ; un joueur qui quitte la file la fait avancer ;
  - délais : 120 s pour la sauvegarde de l'hôte, 300 s pour le téléchargement et le chargement
    côté client, comptés à partir du tour du joueur (l'attente dans la file ne compte pas).
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

### Client qui plante ou perd la connexion 🟡 (tests unitaires ; test en jeu `crashrejoin` pas encore lancé)
- **L'hôte** voit partir un client planté (jeu tué, réseau coupé) en 5 à 15 s. Un client dont le
  jeu est **figé** (chargement d'une zone, sauvegarde) n'est pas coupé : un fil de maintien répond
  au réseau à la place du jeu figé, jusqu'à 90 s.
- **Ses personnages** reviennent à l'hôte, s'arrêtent là où ils sont (marche, tâche et ramassage en
  cours abandonnés ; ils se défendent toujours ; un corps porté reste porté). Ce que le joueur avait
  demandé et qui n'est pas encore fait est jeté (ordres, échanges d'inventaire, poses de
  bâtiments, portes, conteneurs en route). Les fenêtres de conteneur et de commerce qu'il avait
  ouvertes sont libérées ; la pause « X crée son personnage » est levée.
- **Il revient** (même compte Steam ; sans Steam : même nom, ancienne connexion muette depuis 2 s) :
  il garde son nom, retrouve son personnage et ceux qu'il commandait, dans leur état actuel chez
  l'hôte. S'il revient avant que l'hôte ait vu la coupure, la nouvelle connexion remplace
  l'ancienne aussitôt.
- **Côté client**, rien de persistant n'est abîmé : le monde de l'hôte n'est chargé que dans
  l'emplacement `KenshiCoopJoin` (vidé à chaque arrivée), et le jeu du client **ne sauvegarde pas**
  pendant une session (sauvegarde, sauvegarde rapide, sauvegarde automatique refusées : elles
  écriraient le monde de l'hôte sur une partie du joueur).

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
> **Désactivé** dans le code (`world.cpp` `kStuckDetection = false`).
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
  Un personnage tombe là où celui de l'hôte est tombé. Un corps couché trop loin de celui de
  l'hôte est relevé et recouché (3 fois au plus) : il est remis debout d'autant en deçà du corps de
  l'hôte que sa chute précédente l'avait porté loin de ses pieds (sinon il retombait chaque fois au
  même écart, 7 à 12 unités pour un humain, ~75 pour une grosse bête). Non revérifié en jeu.
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
- Vérifié par la suite (« porter ») et par `coop_test.py carry` (épaule, puis pose sans projection).
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
  - les cadavres à moins de 1000 unités d'un joueur restent synchronisés ;
  - un remplaçant que le jeu du client a supprimé (mort, zone déchargée) est recréé (fix G6).

### Étages dans les bâtiments 🟡 implémenté, à vérifier en jeu
- **Le joueur** voit l'étage affiché suivre un perso qui monte ou descend, comme en solo.
- **Fonctionnement** : l'hôte envoie l'étage de chaque perso (`CharMovement::floorGroup`, message
  71). Le client l'écrit sur sa copie, qui sinon garde l'étage d'avant puisqu'elle est seulement
  placée. Expérience `floor`.

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

### Fenêtre Escouade 🟡 (tests unitaires ; expérience `squadui` pas encore lancée en jeu)
- **Le joueur** voit chez tout le monde les mêmes escouades, avec les mêmes noms, dans le même
  ordre, les escouades vides comprises, le même chef (le premier portrait) et les mêmes noms de
  personnages. Tout ce qu'il fait dans la fenêtre Escouade passe par l'hôte :
  - déposer ou échanger un portrait (changer d'escouade, devenir chef) : **seulement ses propres
    persos** ;
  - renommer une escouade, la glisser (ordre des escouades), la retirer (croix) : une escouade qui
    contient un de ses persos, ou vide ; retirer : seulement une escouade vide, jamais la dernière ;
  - créer une escouade (bouton « nouvelle escouade ») ;
  - renommer son personnage (fenêtre du personnage) ; le perso d'un autre garde son nom.
- **Conflits** : l'hôte exécute les demandes une par une, dans l'ordre où elles arrivent ; la
  dernière gagne et tout le monde reçoit le résultat. Un nom en cours de frappe reste affiché jusqu'à
  la réponse de l'hôte ; un refus remet le nom de l'hôte, avec un message en français.
- L'hôte organise toujours les escouades de tout le monde (il peut déplacer n'importe quel perso).

### Réglages d'IA de chaque personnage 🟡 (tests unitaires ; `squadui` à lancer)
- **Concerne** : passif, bloquer, tenir la position, narguer, furtif, distance, « tâches » (bouton
  JOBS, la permanence des tâches : ordre 15), allure (courir / trottiner / marcher / vitesse du
  groupe), style de combat (attaque / défense / esquive), boutons MEDIC et SECOURS (tâches
  permanentes 58 et 148), liste et ordre des tâches (panneau Tâches).
- **Qui** : le joueur du personnage ; les autres voient le résultat en direct. Les **recrues à
  personne** (persos de l'hôte jamais donnés à un joueur, l'avatar de l'hôte excepté) : l'hôte et
  **n'importe quel joueur** ; la dernière demande que l'hôte exécute gagne. Pour qu'une recrue ne
  soit plus à tout le monde, l'hôte la donne (console `give`, à lui-même compris).
- **Valeur, pas bascule** : un clic envoie la valeur voulue ; deux joueurs qui cliquent en même
  temps finissent sur la même valeur partout.
- **Tâches** : la liste de l'hôte, avec la cible de chaque tâche, est reconstruite chez chaque
  client (ajouts compris) dans le même ordre.
- **« Attribuer à »** : donner un personnage à un joueur reste réservé à l'hôte (console `give`,
  raccourci) ; tout le monde voit le nouveau propriétaire (`Bind`).
- **Pas dans Kenshi 1.0.68** : il n'existe pas de case « fuir / se replier à faible santé » ni
  « s'équiper tout seul » dans l'interface ; ces comportements sont décidés par l'IA, chez l'hôte.

### Fiches de personnage 🟡
- **Le joueur** voit chez tout le monde : le nom (`SquadState`), les compétences et attributs
  (`Progress`), la santé de chaque membre, le sang, la faim (`Vitals`), les ordres permanents et le
  style (`Progress`). Les barres de la fenêtre de stats sont calculées par le jeu à partir de ces
  valeurs. Kenshi n'a pas de « traits » de personnage dans cette version.

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
- Apprendre en travaillant (construire, fabriquer, rechercher, miner, cultiver, cuisiner) : le
  gain est calculé une seule fois, chez l'hôte ; ses 34 compétences arrivent chez le client (science,
  ingénierie, forge d'armes et d'armures, arbalètes, travail de force, agriculture, cuisine
  comprises). Le client annonce lui-même les passages de niveau de ses persos (« * Nom : Science 5
  -> 6 »), son jeu n'en faisant plus. 🟡 annonce à vérifier en jeu.

---

## Dialogues

### Bulles de dialogue ✅
- **Le joueur** voit, chez tout le monde, ce qu'un personnage dit à voix haute.
- Vérifié par la suite.

### Conversations ✅ / 🟡 (audit du 10/10 : expérience `dialogue` pas encore lancée en jeu)
- **Le joueur** voit la fenêtre de conversation de son personnage s'ouvrir **chez lui seulement**
  (fenêtre de l'overlay) : texte, puis réponses numérotées, et un bouton « Partir ». Qu'il ait parlé
  le premier (clic droit « parler ») ou qu'un PNJ l'aborde. Sa réponse part à l'hôte, qui fait
  avancer la conversation avec le code du jeu (`Dialogue::replyClicked`).
- **Fonctionnement** : toutes les conversations se déroulent dans le monde de l'hôte. Les clients
  ne montrent jamais la fenêtre de dialogue du jeu, et leur jeu ne décide rien (`sendEvent`,
  `startConversation`, `_doActions` refusés chez eux).
- Vérifié en ville (`kctest_town`, conversation avec le chef des voleurs Shinobi).

**Qui parle.** L'ordre « parler » d'un client nomme son personnage ; l'hôte vérifie qu'il est à ce
joueur (contrôle central) puis le donne à **ce personnage seul** (sécurité des acteurs : jamais un
perso de l'hôte). Chaque réponse nomme aussi son acteur : le perso du joueur dans **cette**
conversation ; une réponse au nom d'un autre perso, ou à la conversation d'un autre joueur, est
refusée et journalisée.

**Ce que produisent les réponses** : appliqué **une seule fois**, chez l'hôte (`_doActions` ne tourne
que là), puis répliqué par les synchronisations existantes :

| Issue | Comment elle arrive chez les joueurs | État |
|---|---|---|
| recrutement | le PNJ entre dans la faction du joueur : l'hôte en fait un perso d'escouade (il était suivi comme PNJ : il ne le devenait jamais avant) et le donne au joueur **qui lui parlait** (par identité), même si un autre joueur répondait à une conversation au même moment ; sinon la règle « un seul joueur venait de répondre » | 🟡 tests unitaires |
| commerce | « commerçons » : fenêtre de commerce chez le joueur (déjà là, voir Commerce) | ✅ |
| quêtes, états du monde | `Diplomacy` (états du monde, personnages uniques) | 🟡 |
| relations, primes | `Factions` (relations de la faction du joueur, primes) | ✅ |
| objets et argent donnés ou pris | inventaires et argent de l'hôte | ✅ |
| combat déclenché | IA et combat de l'hôte | ✅ |
| rejoindre l'escouade | voir recrutement | 🟡 |
| quitter l'escouade | **pas géré** : un perso qui quitte la faction du joueur reste « d'escouade » pour le mod jusqu'à ce qu'il sorte du rayon. Pas fait volontairement : l'esclavage et les prisons font aussi sortir des persos de la faction, et ce changement touche au lot escouades | ❌ |

**PNJ qui parlent aux joueurs** (garde qui contrôle, mendiant, esclavagiste, chasseur de primes,
interrogatoire de la Nation Sainte) : la conversation que le jeu de l'hôte lance avec le perso d'un
client s'ouvre chez **ce** client. Les bulles au-dessus des têtes sont montrées à tous.

**Une conversation à la fois par PNJ.**
- Ordre « parler » (12 et 126) d'un joueur vers un PNJ déjà en conversation avec quelqu'un d'autre
  (un autre joueur, l'hôte, un PNJ) : refusé avant d'être donné, `Result` « Busy », message
  « *Nom* est occupé : il parle déjà avec *X*. ».
- Au moment où le jeu de l'hôte lancerait la conversation (`startConversation`,
  `startPlayerConversation`), un PNJ pris dans la conversation d'un client ne peut ni en commencer
  une autre ni y être entraîné ; celui qui demandait voit « *Nom* est occupé : il parle déjà avec
  quelqu'un. » (chez lui, ou chez l'hôte). La conversation qui continue (un chef qui prend la suite)
  reste permise.

**Fin propre.**
- **Partir** (bouton de la fenêtre) : l'hôte termine la conversation dans son jeu
  (`Dialogue::endDialogue`), la fenêtre se ferme.
- **Joueur qui se déconnecte** : ses conversations sont terminées chez l'hôte (rien ne reste ouvert
  sur personne), son perso revient à l'hôte comme avant.
- **Combat, téléportation, K.-O., mort** : toutes les 0,5 s, l'hôte termine et ferme une
  conversation dont un personnage est à terre, mort ou disparu, que le jeu a finie sans fermer la
  fenêtre, dont le perso n'est plus à ce joueur, dont un des deux a sauté de plus de 20 m en 0,5 s
  (TP), ou qui se sont éloignés de 100 m de plus qu'au début. Pas de distance absolue : une
  conversation peut commencer de loin (essai du 10/10 : le Chef Voleur aborde le perso du client à
  plus de 40 m, et l'ancienne règle « plus de 40 m » la fermait aussitôt).
- **Réplique en double** : le jeu remplit la fenêtre deux fois par réplique (texte, puis réponses) ;
  la même réplique n'est envoyée et numérotée qu'une fois.
- **Vitesse 3** : chaque réplique porte un numéro ; une réponse à une réplique déjà dépassée est
  ignorée (journal « answer to an older line ignored »), le joueur voit la nouvelle.

**Pause : décision pour la coop.** En solo, le jeu se met en pause dès qu'une fenêtre de conversation
s'ouvre (`userPause(true)`, voir MOTEUR). En coop, **pas de pause** : une conversation ne concerne
que son joueur, le monde continue pour les autres.
- La conversation d'un client ne s'ouvre jamais chez l'hôte : pas de pause.
- Celle d'un client n'ouvre pas non plus la fenêtre du jeu chez lui : sa vitesse reste celle de l'hôte.
- La propre conversation de l'hôte, quand d'autres joueurs ont des persos : le mod lève aussitôt la
  pause que le jeu vient de mettre (journal « the host's own conversation: the game is not paused in
  co-op »). Seul, l'hôte garde le comportement du jeu.

**Vue de l'hôte** : quand un client parle près de lui, l'hôte voit les bulles (son jeu les dit), pas
la fenêtre du client.

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
- **Sécurité des acteurs** 🟡 (tests unitaires ; expérience `actorsafety` pas encore lancée en jeu) :
  un client ne fait **jamais** agir un personnage qui n'est pas le sien.
  - Bug signalé en partie réelle : le joueur 2 voulait dormir ou parler à un PNJ, et c'est le perso
    de l'hôte qui y allait. Cause (désassemblage 1.0.68) : `PlayerInterface::unselectAll` ne vide pas
    la sélection, il resélectionne le perso « principal » ; et `objectSelected(perso, false)` refuse
    de retirer le dernier sélectionné. L'hôte, qui sélectionnait « seulement » le perso du client,
    gardait donc son propre perso principal sélectionné : l'ordre partait aux deux (addOrder,
    newPlayerTask, addJob, ramasser) ou au plus proche des deux (lit, conversation). Seule une
    sélection vide chez l'hôte (juste après le chargement, comme dans les tests) était sûre.
  - Maintenant : la sélection est faite **exactement** des acteurs (le principal gardé par le jeu en
    est retiré), vérifiée, sinon l'ordre est refusé sans rien exécuter ; puis la sélection, l'escouade
    affichée, le panneau de détails et le perso principal de l'hôte sont remis, dans le même appel.
  - Chaque demande d'un client nomme son acteur ; l'hôte vérifie, à un seul endroit, qu'il est à **ce**
    joueur (ni l'hôte, ni un autre joueur, ni un PNJ, ni inconnu) avant tout traitement. Refus :
    journal anglais (`refused: actor N not owned by player P`, `auth: [nom] ... refused: ...`),
    compteur par joueur et par règle, réponse `Result` avec un texte en français affiché au joueur.
  - Le client ne demande rien pour un perso qui n'est pas le sien : refusé chez lui avec « Action
    refusée : ce personnage n'est pas le tien. ». Agir **sur** le perso d'un autre (premiers soins,
    suivre, porter un corps) reste possible : l'acteur est le sien.
  - **Cible vérifiée** : chaque numéro de tâche qu'un client peut envoyer est confronté à ce qu'est sa
    cible (construire : un chantier de la faction du joueur ; parler : un PNJ debout ; lit : un lit ;
    machine : une machine ; attaquer : un autre perso ; porter : un corps ; portes, cages...). Numéro
    inconnu, cible introuvable, périmée ou du mauvais type : refusé avant les fonctions d'ordre du jeu.
    Corrige le plantage de l'hôte du test `stress4` (ordre « construire » visant un PNJ, plantage dans
    `Character::addJob`). Changement d'escouade : seulement vers l'escouade d'un joueur.
- **Sélection mixte chez l'hôte** (fix G5) : si la sélection de l'hôte contient le perso d'un autre
  joueur, ce perso en est retiré et l'ordre (déplacement, arrêt, mode passif...) part aux persos de
  l'hôte ; avant, l'ordre entier était refusé. Expérience `passive`.
- **Panneau Tâches** (fix G5) : retirer une tâche (croix) ou la déplacer chez un client est fait par
  l'hôte sur son perso ; l'hôte envoie les listes de tâches avec leurs cibles (`JobState`) et chaque
  client fait la même liste (retire, ajoute, réordonne).
  Expérience `jobs`.
- **Refusés côté client** : recruter au centre d'emploi (55), commerce (118, 119), et les tâches de
  stockage des PNJ (124, 284). Le joueur voit le message « Commercer et ouvrir un coffre ne sont
  pas encore synchronises. » (texte à mettre à jour : les coffres marchent, voir plus bas).

### Objets au sol ✅ (glisser-déposer : 🟡 pas encore vérifié en jeu)
- **Le joueur** voit un objet posé par l'hôte au même endroit chez tout le monde, puis disparaître
  partout quand quelqu'un le ramasse. Un client qui ramasse ou dépose le demande à l'hôte : son
  personnage va chercher l'objet chez l'hôte.
- **Toutes les façons de poser** : glisser un objet d'une fenêtre d'inventaire vers le monde (perso,
  bête de somme, coffre, sac à dos), inventaire plein, l'IA d'un PNJ, et tout ce qu'aucun crochet ne
  voit (sur K.-O. ou mort s'il en tombe quelque chose, sortie d'une machine, etc.).
- **Fonctionnement** :
  - l'hôte annonce l'objet dès qu'il est au sol : crochets sur `Inventory::dropItem` (le chemin du
    glisser-déposer), `CharacterHuman::dropItem` et `CharacterAnimal::dropItem`. L'objet compte
    comme posé dès qu'il est actif hors de tout inventaire, sans attendre son corps physique (que le
    jeu crée parfois quelques images plus tard : l'ancien test l'exigeait et l'objet n'était alors
    jamais annoncé) ;
  - en plus, deux fois par seconde, l'hôte relève les objets à 30 m autour de chaque perso des
    joueurs. Un objet qui apparaît là où un perso regardait déjà est annoncé (posé par un chemin sans
    crochet) ; un objet qui disparaît de là est annoncé ramassé ; une pile qui change de nombre (ou un
    objet annoncé qui a roulé à plus de 3 m) est annoncée de nouveau. Un objet qui entre dans le champ
    parce qu'on marche, qu'on est téléporté ou qu'une zone charge est seulement noté : les clients
    l'ont par la sauvegarde ;
  - chaque objet n'est annoncé qu'une fois (les crochets s'emboîtent) ; chez le client, une annonce
    répétée est ignorée, et un objet identique (même pile) déjà posé à moins de 0,3 m et qui ne
    représente aucun autre objet de l'hôte (sa copie de la sauvegarde) est adopté au lieu d'être créé
    en double. Si la zone du client charge après coup sa propre copie au même endroit, la copie créée
    est retirée (toutes les 2 s) ;
  - un client qui lâche un objet (son perso, sa bête, le sac à dos qu'un de ses persos porte, un
    coffre qu'il a ouvert, un corps) le demande à l'hôte ; pour un coffre ou un corps, c'est son
    perso le plus proche qui le pose. L'hôte refuse depuis le perso d'un autre joueur ;
  - **rien ne bouge chez le client pour un dépôt qui ne peut pas partir** : la session publie à
    chaque instant les inventaires depuis lesquels un dépôt peut être demandé (`SetDropSources`) ;
    la fenêtre d'inventaire refuse aussitôt les autres (l'objet reste ou revient à sa place, toast
    « Objet non posé : … Rien n'a bougé. »). Le jeu du client ne pose jamais rien lui-même au sol, ni
    ne ramasse un objet du monde. Un dépôt refusé plus tard (cas rare) fait revenir tout de suite
    l'état de l'hôte, avec une note dans le chat ;
  - un perso est nommé à l'hôte par le handle de l'hôte : une doublure (perso créé par l'hôte, perso
    d'un joueur revenu après un plantage) ou un perso qui a changé d'escouade a un autre handle chez
    le client. Le dépôt envoyait le handle local : « the host does not know that inventory » et
    l'objet disparaissait (partie du 10/10, corrigé) ;
- Vérifié par les expériences `ground` (objets posés puis ramassés par l'hôte) et `clientpickup`
  (ramassage demandé par un client) ; `grounddrop` (chemin du glisser-déposer : minerai, arme,
  armure, par l'hôte, par le client, les deux à la fois, K.-O. et mort) écrite, pas encore lancée.
  Test unitaire `TestGroundDrops`.
- 🟡 Limites : les objets à plus de 30 m de tout joueur ne sont vus que par les crochets. Le
  refus local (remettre l'objet tenu à la souris à sa place) n'est pas encore vérifié en jeu.
- Ramassage par un client : l'hôte journalise où l'objet est arrivé (« in the inventory of the
  character that was sent », ou un autre perso). Il n'annonce plus comme « ramassé » un objet que
  son jeu vient de créer et donne à un inventaire (stock d'un marchand, équipement d'un PNJ qui
  apparaît) : seul un objet actif dans le monde ou rangé dans un groupe d'objets compte
  (`kenshi::ItemPlacedInWorld`). Chez le client, la reconstruction d'un inventaire ne laisse plus le
  jeu détruire un objet qu'elle ne peut pas poser à la place de l'hôte : il va dans le sac.
- 🟡 Un objet qui traînait déjà dans la sauvegarde a un autre handle chez chaque joueur : l'hôte le
  retrouve par type et endroit, le plus proche à moins de 40 unités, parmi **tous** les objets hors
  inventaire (marchandises de magasin, objets de décor de la ville, que le jeu range dans un groupe
  d'objets ou garde non physiques). Puis il donne au perso du client l'ordre « ramasser » du jeu
  lui-même : le perso y va et le prend, en le volant (avec la réaction des gardes) s'il appartient à
  quelqu'un. Pendant une pause (dont celle de l'éditeur de personnage d'un joueur qui arrive), la
  demande attend la reprise. Expérience `groundpick` (à lancer en ville).

### Inventaires, équipement, fouille des corps ✅
- **Le joueur** voit le même inventaire et le même équipement partout. Il peut fouiller un
  personnage K.-O. ou mort par clic droit : son personnage s'y rend via l'hôte, puis la fenêtre de
  fouille du jeu s'ouvre chez lui.
- **Fonctionnement** : chaque objet déplacé dans une fenêtre d'inventaire du client devient une
  demande de déplacement (`InvOp`) que l'hôte rejoue. L'hôte refuse de vider un PNJ conscient ou
  le personnage d'un autre joueur, et renvoie alors l'état réel. On peut aussi poser des objets sur
  un corps K.-O. ou mort, comme dans le jeu.
  - Un objet lâché sur une case occupée (des bottes sur un perso qui en porte déjà) : le jeu échange
    les deux objets ; l'hôte reconnaît la paire de déplacements et fait le même échange d'un coup.
  - Un déplacement qui libère une case passe avant celui qui la remplit (ranger son arme dans le sac
    puis prendre celle du corps).
  - Une pile lâchée sur une pile du même objet s'y ajoute chez l'hôte aussi (au lieu d'être posée
    ailleurs, ce qui faisait « sauter » l'inventaire du client).
  - Expérience `lootswap`, tests `TestInventorySwaps`.
  - Chez le client, l'inventaire d'une copie est refait à l'identique de celui de l'hôte, même
    dans une section d'équipement que son jeu tient désactivée (bottes, chemise d'une doublure de
    PNJ, qui finissaient dans le sac : stress à 4 joueurs du 10/10). Un objet qui reste ailleurs
    est nommé au journal (`inventory of … not as on the host`). Non revérifié en jeu.
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
- L'heure du client est recalée sur celle de l'hôte (`ClockSync`) : écart de plus de 0,002 h, il
  tourne 3 à 10 % plus vite ou plus lentement jusqu'à la rattraper ; plus de 0,008 h (un client
  figé par une zone qui charge), ou plus de 0,002 h pendant une pause de l'hôte, l'heure est
  recalée d'un coup. Le stress à 4 joueurs du 10/10 finissait à 0,16 h d'écart avec l'ancien
  rattrapage seul ; le recalage n'est pas encore vérifié en jeu.

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

### Administration de l'hôte (dieu, TP, XP, soins, argent) 🟡 implémenté, à vérifier en jeu
- **L'hôte** a, dans la fenêtre Multijoueur, une liste des joueurs (nom, ping, personnages, persos
  à terre) avec une ligne d'actions par joueur et une ligne « Tout le monde » (l'hôte compris) :
  - **Dieu** : case à cocher, l'état est affiché. Le mode dieu est retenu par joueur (compte Steam,
    sinon nom), pas par personnage : il est réappliqué 5 fois par seconde aux persos de ce joueur,
    donc il tient aux changements de zone, aux reconnexions et aux nouveaux persos. « Tout le
    monde » couvre toute l'escouade, nouveaux venus compris ; décocher un joueur pendant ce temps
    laisse les autres en mode dieu. Il s'arrête quand l'hôte cesse d'héberger. Activer le mode dieu
    soigne d'abord.
  - **TP** : ses persos près de l'hôte (« TP moi »), l'hôte près de lui (« Aller »), près d'un autre
    joueur ou au *point marqué* (« TP à… »). Le point marqué est le dernier endroit où l'hôte a
    ordonné un déplacement par clic droit au sol (`PlayerInterface::playerMove`, déjà détourné) :
    on a ainsi la hauteur du sol. Les commandes acceptent aussi des coordonnées. C'est le TP admin
    sûr (`TeleportCharacters`) : un corps à terre est relevé avant, recouché à l'arrivée, un perso
    porté est posé ; le jeu du joueur téléporté a droit à 120 s de gel (chargement de zone).
    Confirmation (second clic) quand un des persos déplacés est à terre.
  - **XP** : une compétence (ou toutes) et une quantité réglées sous la liste, en points
    d'expérience ou en niveaux. Les points passent par la fonction du jeu `increaseStat`, par
    appels de 20 au plus (le jeu refuse au-delà) : mêmes rendements décroissants qu'en jouant. Les
    niveaux aussi, chaque appel calculé pour tomber juste ; tout près de 100 (où les gains
    deviennent infimes), le reste est écrit directement après 600 appels. Les valeurs de l'hôte
    partent chez les clients par la synchro habituelle des compétences. Confirmation au-delà de
    1000 points ou 10 niveaux ; refus au-delà de 20000 points ou 100 niveaux.
  - **Soigner** : `Character::healCompletely` (blessures, sang, K.-O.), puis relève un perso resté
    inconscient.
  - **Argent** : cats ajoutés (ou retirés) à l'argent commun de l'escouade.
- Chaque action s'exécute sur le fil du jeu de l'hôte, est journalisée en anglais (« admin: … ») et
  le joueur concerné reçoit un message en français dans son fil de discussion (« * L'hôte t'a
  téléporté près de lui. ») ; l'hôte, lui, voit un message à l'écran.
- Les mêmes actions en commandes (console dans le jeu, console hors du jeu) : `admin god`,
  `admin tp`, `admin xp`, `admin heal`, `admin money`, `admin list` (voir le README). Les anciennes
  `god`, `heal` et `xp <id> <n>` (niveaux dans toutes les compétences) passent par là.
- **Le client** n'a ni la section ni les commandes : sa console répond « commande réservée à
  l'hôte », et aucun message réseau ne permet d'en demander une (pas de changement de protocole :
  le message au joueur est un `Chat` « de l'hôte », comme pour un commerce refusé).
- Tests : `TestAdmin` (syntaxe, calcul de l'XP du jeu, registre du mode dieu, message à un seul
  joueur) ; expérience `admin` (pas encore lancée en jeu).
- **Pas fait** : remettre la faim à zéro (l'échelle de la faim n'est pas connue avec certitude) ;
  ressusciter un mort.

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

### Commerce ✅ (vérifié en jeu le 9 octobre)
- **Le joueur** parle à un marchand et choisit « commercer » comme d'habitude. La fenêtre de
  commerce du jeu s'ouvre **chez lui**, jamais chez l'hôte. Elle montre le stock du marchand tel
  que l'hôte l'a. Il achète et vend par glisser-déposer ou clic droit, aux prix du jeu.
- **Cohérence** : un objet acheté par un joueur disparaît du stock partout. Une fenêtre déjà
  ouverte sur ce marchand, chez un autre joueur ou chez l'hôte, se rafraîchit d'elle-même : elle
  ne propose plus cet objet. L'argent du joueur et celui du marchand sont ceux de l'hôte.
- **Fonctionnement** :
  - l'hôte intercepte la fenêtre que son jeu ouvrirait pour le personnage d'un autre joueur, quel
    que soit le côté où ce perso arrive (dialogue lancé par le joueur ou par le marchand, clic
    droit) et même pendant l'exécution de l'ordre du client (fix G5, expérience `tradepaths`) ;
  - il envoie à ce joueur le marchand et les meubles d'où il vend (ses comptoirs, les meubles de
    son bâtiment), traités comme des contenants ouverts ;
  - le client ouvre la vraie fenêtre de commerce du jeu ;
  - chaque achat ou vente part à l'hôte avec le prix que le jeu du client a compté (il dépend de
    l'état de l'interface, que l'hôte n'a pas) ;
  - l'hôte vérifie que l'acheteur peut payer, déplace l'objet et l'argent, puis renvoie le stock ;
  - un achat refusé (pas assez d'argent, objet déjà parti) revient en arrière, objet et argent.
- **Vérifié en jeu** (expérience `trade`, marchand Shinobi en ville, 12 points sur 13 puis la
  vente à la main) :
  - la fenêtre s'ouvre chez le client et pas chez l'hôte, avec les 15 objets du stock ;
  - un achat de 745 cats est payé chez l'hôte, avec le même argent partout ;
  - une vente rapporte l'argent au client chez tout le monde ;
  - les inventaires sont identiques ;
  - la fenêtre de l'hôte sur ce marchand ne propose plus l'objet acheté par le client.
- **Sécurité** : la fenêtre du jeu montre les objets des comptoirs eux-mêmes. Quand le stock change
  sous une fenêtre ouverte, elle se ferme un instant, les comptoirs prennent le stock de l'hôte,
  puis elle se rouvre. Un personnage montré par une fenêtre n'est jamais supprimé sous elle. Ces
  deux cas faisaient planter le client.
- **Limites** :
  - la détection des objets volés à la revente n'est tirée que par le jeu du client.

### Marchands ambulants et sacs à dos 🟡 (implémenté, protocole 33, à vérifier en jeu)
- **Le joueur** commerce avec une caravane ou un nomade (un marchand sans bâtiment) comme avec un
  marchand de ville : la fenêtre du jeu s'ouvre chez lui avec le vrai stock, même pendant que la
  caravane marche. Achats et ventes sont payés chez l'hôte ; stock et argent sont les mêmes partout.
- **D'où vient le stock** : le jeu construit la fenêtre d'un marchand sans bâtiment à partir des sacs
  à dos portés par les membres de son escouade (bêtes de somme, gardes, lui-même) : le premier
  objet de la section de type 12 de chaque inventaire, et la première section de l'inventaire de ce
  sac (constructeur de `ShopTrader`, voir MOTEUR.md).
- **Sacs à dos portés** : le contenu du sac porté par chaque personnage suivi (joueurs compris) est
  synchronisé comme un inventaire, sous un identifiant à lui (message `BagBind`). Chez le client il
  est rattaché au même sac (même modèle) porté par sa copie du personnage ; un sac changé est
  rattaché à nouveau et rempli comme chez l'hôte. Un joueur range et sort ses objets de son propre
  sac : l'hôte rejoue le déplacement.
- **La fenêtre se ferme** (avec un message) quand le marchand ou une bête qui porte le stock est à
  terre, meurt, se bat, s'éloigne à plus de 150 m du joueur, ou n'est plus suivi.
- **Vol** : un clic droit sur la bête de somme d'un PNJ ouvre chez le client la fenêtre de pillage
  du jeu. Ce qu'il y prend passe par l'hôte comme un vol : le jeu de l'hôte enregistre le crime, tire
  s'il est vu (fenêtre fermée, « Pris en train de voler ! ») et la prime est celle de l'hôte partout.
- **Bête morte** : elle garde son sac, identique partout ; on la pille comme un corps (ce que le
  client prend vient de l'hôte, rien n'est créé ni perdu).
- **Sécurité** : jamais de comptoir de remplacement (l'ancien plantage dans `ShopTraderInventory`).
  Le sac est un objet du jeu (`ContainerItem`) : son inventaire est lu par
  `RootObject::getInventory` (vt 0x160), qui renvoie null pour un objet ordinaire.
- **Limites** : quand plusieurs joueurs commercent en même temps avec la même caravane, chaque
  achat rafraîchit les fenêtres ouvertes (comme en ville). Aucun modèle de caravane n'est créé par
  le harnais : l'expérience `caravan` a besoin d'une sauvegarde avec une caravane près de
  l'escouade.

### Animaux de l'escouade 🟡 (à vérifier en jeu)
- Bêtes de somme, chiens, chèvres... achetés ou apprivoisés sont des membres de l'escouade comme les
  autres : position, santé, faim (dans les signes vitaux) et inventaire sont ceux de l'hôte partout ;
  les ordres d'un client sur ses animaux passent par l'hôte.
- **À qui** : à l'hôte, sauf :
  - un personnage qui rejoint l'escouade juste après qu'un seul joueur a répondu dans une
    conversation (un animal acheté à un marchand d'animaux, une recrue) est à ce joueur ;
  - l'hôte peut en donner un à un joueur.
- **Suivre son maître** : un animal donné à un joueur passe dans l'escouade du personnage de ce
  joueur, chez l'hôte : il le suit comme le jeu fait suivre une escouade.
- Un joueur qui part rend ses personnages à l'hôte (ils s'arrêtent) ; **quand il revient (même
  compte Steam, ou même nom sans Steam), l'hôte lui rend ceux qu'il avait** (animaux et recrues
  compris), s'ils sont encore à l'hôte.
- **Limites** : si plusieurs joueurs parlaient en même temps, le nouveau venu reste à l'hôte.

### Construction, meubles, achat et démontage de bâtiments 🟡 (implémenté, à vérifier en jeu)
- **Le joueur** (hôte comme client) construit comme d'habitude : mode construction, il pose un
  bâtiment ou un meuble (lit, coffre, machine, table…) dans une maison à nous. Chez un client,
  **rien n'est construit par son jeu** : la pose part à l'hôte, dont le jeu bâtit le chantier, puis
  chaque joueur (celui qui l'a posé compris) bâtit le même chantier au même endroit. Ce que l'hôte
  pose part de la même façon chez tout le monde.
- **Avancement** : les ouvriers construisent chez l'hôte ; l'avancement du chantier, sa fin, la
  pause et le démontage sont imposés aux clients (chaque seconde ce qui change, tout toutes les
  10 s). Le jeu d'un client ne fait jamais avancer ni démonter un chantier de lui-même. L'ordre
  « construire » d'un client part à l'hôte comme les autres ordres (le chantier y est retrouvé par
  type et endroit).
- **Démontage** : un client qui confirme « démonter » le demande à l'hôte, dont le jeu le fait ;
  un bâtiment détruit pour de bon chez l'hôte (démonté, détruit) disparaît chez tout le monde.
- **Achat** : un client qui confirme l'achat d'un bâtiment à vendre le demande à l'hôte, qui vérifie
  l'argent et achète ; ensuite chaque client rejoue l'achat avec la fonction du jeu (le bâtiment
  devient à nous, portes comprises). Un achat fait par l'hôte est rejoué de même.
- **Fonctionnement** : le mode construction finit par appeler `RootObjectFactory::createBuilding`
  avec les valeurs finales (modèle, position relative au terrain ou au bâtiment parent, rotation,
  ville, plan du bâtiment dont c'est un meuble, bâtiment où il se trouve, étage). Le mod capte ces
  valeurs et chaque machine appelle la même fonction avec les mêmes valeurs, puis ce que le mode
  construction fait à un bâtiment neuf. Les bâtiments ont un autre handle sur chaque machine : ils
  sont nommés par type et endroit, plus un netId donné par l'hôte.
- **Emplacement valide** : le mode construction refuse lui-même un endroit invalide (aperçu
  rouge : eau ou acide, pente, ville, dans ou sur un autre bâtiment…) ; un client n'envoie donc que
  des poses que son jeu a acceptées. L'hôte refait en plus les vérifications du jeu sur chaque pose
  demandée par un client (ville, intérieur d'un bâtiment, autre bâtiment au même endroit, sol dans
  l'eau ou l'acide, pente : docs/MOTEUR.md section 10) : une pose refusée n'est bâtie nulle part,
  le journal de l'hôte dit pourquoi (« refused: invalid spot (in water or acid …) ») et le joueur
  reçoit « Impossible de construire X ici : dans l'eau ou l'acide. ». La commande de test
  `buildplace` passe par la même vérification. Pas refaits par l'hôte (il faudrait l'aperçu du
  jeu) : collision exacte des empreintes, personnages dans le passage, nœuds d'usage, étage.
- **Arrivée en cours de partie** : les bâtiments posés avant sont dans la sauvegarde envoyée ;
  l'hôte envoie aussitôt l'état de tous ceux qu'il suit.
- **Limites, à vérifier en jeu** :
  - les objets « is node » (posés par une autre fabrique du jeu) ne passent pas par ce chemin ;
  - le chemin des meubles posés à un étage (« plan d'étage ») n'est qu'en partie compris ;
  - la ville d'un bâtiment est retrouvée par son handle : si elle ne se résout pas, le bâtiment est
    construit sans ville ;
  - après la pose, l'hôte ne refait pas l'enregistrement de navigation des murs que fait le mode
    construction (les murs posés par un client peuvent gêner les PNJ autrement chez l'hôte).

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

### Combat à distance : arcs, arbalètes, harpons, tourelles 🟡 (implémenté, à vérifier en jeu)
- **Le joueur** voit chaque tir de l'hôte chez lui : le tireur lâche son carreau au même moment,
  et le projectile part de la même arme **sur la même trajectoire** que chez l'hôte (y compris
  sa déviation aléatoire). Les blessures restent celles de l'hôte (santé synchronisée) : chez le
  client, ce que touche le projectile ne fait aucun dégât.
- **Fonctionnement** :
  - le jeu tire avec `GunClass::shoot`, qui prend un projectile dans la réserve du jeu, le place
    au bout de l'arme et l'oriente vers le point visé, avec une déviation au hasard. Le
    projectile vole ensuite dans le sens de son orientation ;
  - chez l'hôte, un hook sur `shoot` note chaque tir : le tireur, sa cible, le point visé,
    l'orientation exacte du projectile au départ, et la tourelle s'il s'agit d'une tourelle ;
  - le message `Shots` part aussitôt aux clients ;
  - chez un client, le même hook refuse les tirs que son jeu déciderait seul. KenshiCoop tire celui
    de l'hôte avec la même arme, puis remet le projectile sur la trajectoire de l'hôte ;
  - le point visé par chaque personnage en combat à distance part 5 fois par seconde quand il
    change (message `Ranged`). Le client l'impose à chaque image, pour que le haut du corps vise
    le même endroit ;
  - les tourelles proches des joueurs envoient le point vers lequel elles tournent. Le client
    tourne la sienne avec la fonction de visée du jeu.
- **Tourelles** : un client peut ordonner à son personnage de prendre une tourelle. L'ordre part
  à l'hôte comme les autres ordres sur un meuble, la tourelle étant retrouvée par type et endroit.
  Ses tirs et sa visée arrivent ensuite comme ci-dessus.
- **Limites, à vérifier en jeu** :
  - le client tire avec l'arme que son personnage a en main localement. S'il n'en a pas (arme pas
    encore prête dans le jeu du client), le tir n'est pas montré ; il est compté dans `shots`
    (`nogun`) ;
  - le rechargement montre les animations de l'hôte. Les munitions consommées localement
    reviennent à l'état de l'hôte par la synchro des inventaires ;
  - la visée imposée et le canon des tourelles (son pointeur, cherché au premier usage) restent
    à confirmer en jeu.

### Factions, relations, primes, crimes 🟡 (implémenté, à vérifier en jeu)
> **Primes jamais écrites dans le jeu du client** depuis le 10/10 : tous les clients plantaient à
> l'apparition d'une prime. Le client garde la copie de l'hôte et l'affiche dans la fenêtre
> **Diplomatie** (Ctrl+Shift+F). Les relations, elles, sont écrites dans son jeu.
- **Le joueur** voit partout les mêmes relations de sa faction avec chaque faction (écran des
  factions du jeu : valeur, alliance, guerre, paix), dans les deux sens (ce que chaque faction pense de la
  faction du joueur compte pour les PNJ et l'interface). Il voit aussi le même rang et la même
  réputation, et, pour chaque personnage de l'escouade (ceux des clients compris), les mêmes primes
  (montant par faction, crimes), le crime en cours, la peine de prison restante et le laissez-passer.
- **Fenêtre Diplomatie** (Ctrl+Shift+F, hôte et clients, en français) : les relations de la
  faction du joueur avec chaque faction telles que l'hôte les a (valeur, état « allié / neutre /
  ennemi / en guerre » calculé comme le jeu : allié = alliance ou relation ≥ 50, ennemi = relation
  ≤ −30), le rang et la réputation, les primes et peines de chaque perso de l'escouade, et le monde
  (guerres entre factions, chefs morts ou emprisonnés, villes changées).
- **Nouvelles dans le panneau** (hôte et clients, en français, à chaque changement venu de l'hôte) :
  « Diplomatie : X vous considère maintenant comme ennemi (relation −60) », « Diplomatie : X est en
  guerre contre vous », « Prime : Bob est recherché par X (3000 cats) », « Prime : Bob n'est plus
  recherché par X ».
- **Ce qui change les relations et les primes** quand c'est un perso d'un client qui agit : tout se
  passe dans le monde de l'hôte, qui fait jouer le perso du client. Attaquer ou tuer des membres
  d'une faction, libérer des esclaves ou des prisonniers, aider dans un combat, les répliques et
  issues de quête d'une conversation (`Dialogue::_doActions`), payer sa prime, commercer, assassiner
  un chef, livrer une cible à un poste de police (la récompense est une action de dialogue : l'argent
  arrive par la synchro de l'argent) : le jeu de l'hôte applique l'effet **une fois**, chez lui, et
  il part à tous par les messages ci-dessous. Chez un client, les points de décision sont refusés
  (`affectRelations`, `setRelation`, `setCrime`, `assignBountyForCrimes`, `_doActions`, événements de
  dialogue, IA) : rien n'est appliqué deux fois.
- **Gardes** : ce sont les gardes de l'hôte qui reconnaissent un perso recherché (prime de l'hôte) ;
  ils agissent dans son monde et cela se voit chez tout le monde. Ceux du client n'agissent pas.
- **Fonctionnement** :
  - l'hôte relit chaque seconde les relations de la faction du joueur et les primes de l'escouade ;
    il envoie `Factions` / `Bounties` dès qu'une valeur change, à tout le monde, plus un envoi
    complet toutes les 15 s et à chaque joueur qui arrive ;
  - le client impose les **relations** à son jeu (écriture directe ; une relation absente est créée
    avec la fonction du jeu), à l'arrivée du message puis toutes les 2 s : si le jeu local les a
    changées au-delà du bruit (0,5 point de relation ou de confiance, 1 % de force), elles reviennent
    à celles de l'hôte. L'hôte n'envoie lui aussi qu'un vrai changement (au-delà de ce même bruit) ;
  - les **primes et crimes ne sont pas écrits dans le jeu du client** : il garde la copie de l'hôte
    (fenêtre Diplomatie, commande `bounty`) et vide toute prime ou tout crime que son propre jeu crée.
    Une prime dans le jeu du client réveille sa police (gardes, chasseurs de primes) contre un perso
    que l'hôte pilote : tous les clients plantaient quelques secondes après une nouvelle prime ;
  - le journal de l'hôte note chaque changement (« relations: … now -80 at war », « bounty: … wanted
    by … for 1500 cats », « crime: … », « prison: … »).
- **Limites** :
  - l'écran de personnage du jeu du client n'affiche pas les primes (son jeu n'en a pas) : elles
    sont dans la fenêtre Diplomatie ;
  - la victime d'un crime en cours (un PNJ précis) n'est pas envoyée, seulement la faction.
- **Test en jeu** : `python tools/coop_test.py factions` (voir [TESTS.md](TESTS.md)).

### Diplomatie : factions entre elles, chefs, villes 🟡 (implémenté, tests unitaires ; à vérifier en jeu)
- **Le joueur** voit partout le même monde que l'hôte : les guerres et alliances entre factions
  (déclarées par le jeu après la mort d'un chef, un dialogue, une campagne), l'état des personnages
  uniques (chefs de faction, PNJ nommés : vivant, mort, emprisonné, et si ce sont les joueurs), et les
  villes (faction propriétaire, ville prise, détruite ou remplacée par les états du monde).
- **Pourquoi c'est important** : les « états du monde » du jeu (`WorldEventStateQuery`) ne dépendent
  que de l'état des personnages uniques et des relations de la faction du joueur. Les variantes de
  villes, les conditions de dialogue et les campagnes les testent. Avec les mêmes états que l'hôte,
  le jeu du client prend les mêmes décisions quand il les évalue (en chargeant une zone, par exemple).
- **Fonctionnement** (message `Diplomacy`, en trois parties, chacune envoyée quand elle change et
  en entier à un joueur qui arrive ; relue toutes les 3 s chez l'hôte) :
  - **relations entre deux factions** (aucune n'étant celle du joueur) : l'hôte note toutes les paires
    quand il commence à héberger (la sauvegarde que chaque client charge les contient) et n'envoie que
    celles qui ont changé depuis (drapeaux, ou relation d'au moins 1 point). Une paire envoyée reste
    suivie (retour à la paix compris) ;
  - **personnages uniques** : la table du jeu (`UniqueNPCManager`) entière : mort / vivant /
    emprisonné et « par les joueurs ». Le client écrit l'état de l'hôte dans sa table (une entrée
    absente est créée comme le fait le chargement d'une sauvegarde) et remet « vivant » une entrée
    que l'hôte n'a pas ;
  - **villes** : faction propriétaire et variante appliquée. Le client applique la variante de l'hôte
    avec la fonction du jeu (`TownBase::setOverride`) puis le propriétaire (`TownBase::setFaction`).
    Une ville qui a une variante chez le client et aucune chez l'hôte est laissée (le jeu n'a pas de
    retour en arrière) et notée au journal ;
  - le client réimpose toutes les 3 s (son jeu ne doit pas dériver) ; le journal note les corrections
    (« diplomacy: N … values set to the host's »).
- **Nouvelles** (hôte et clients, en français) : « Diplomatie : guerre entre X et Y », « fin de la
  guerre », « X et Y sont alliés », « Monde : Tinfist est mort (de la main des joueurs) », « Monde :
  … est emprisonné par les joueurs », « Monde : Squin appartient maintenant à … », « Monde : Squin a
  changé (…) ». Le journal a la même chose en anglais (« diplomacy: », « world: »).
- **Limites, à vérifier en jeu** :
  - une paire de factions que l'hôte n'a jamais changée n'est pas réimposée chez le client (elle
    n'est pas envoyée). Le jeu du client ne peut pas la changer lui-même (ses points de décision sont
    refusés), sauf `FactionRelations::declareWar` (`0x6B3000`), appelée par la vtable, non détournée ;
  - une ville que l'hôte n'a pas encore « décidée » (zone jamais chargée chez lui) reste telle que le
    jeu du client la décide, avec les mêmes états du monde ;
  - le choix de la variante d'une ville au chargement d'une zone est déterministe (la plus lourde des
    variantes dont les états du monde sont vrais) : il devrait être le même partout, l'envoi de
    l'hôte corrige sinon ;
  - les bâtiments d'une ville détruite ou prise viennent de la décision du jeu au chargement de la
    zone, pas de la synchro (seuls ceux des joueurs sont synchronisés par le lot E).
- **Test en jeu** : `python tools/coop_test.py diplomacy` (voir [TESTS.md](TESTS.md)). Test
  unitaire `TestDiplomacy`.

### Carte, minicarte, repères des joueurs, pings 🟡 (implémenté, à vérifier en jeu)
- **Une couleur par joueur**, la même partout et chez tout le monde (`kc::PlayerColor`,
  `common/include/kc/colors.h`) : or (l'hôte), bleu, vert, magenta, orange, cyan, blanc, violet.
  Le rouge est réservé aux ennemis.
- **Carte du monde** (l'onglet CARTE de la fenêtre de gestion du jeu, ouverte par son bouton « MAP »
  à côté de la barre d'escouade ou par la touche « carte » des réglages du joueur ; le mod n'en
  suppose aucune : dans la partie de l'utilisateur, M est la caméra libre) : par-dessus la carte du jeu, chaque joueur voit **tous les persos
  de tous les joueurs** (gros point : le perso du joueur, petit : une recrue), à la couleur de leur
  joueur, même ceux qui sont très loin de lui (hors de la zone que son jeu a chargée). En rouge,
  les **escouades hostiles qui nous visent** : un raid ou une vague d'attaque du jeu dont la cible
  est notre faction (cercle qui pulse), une escouade qui se bat contre un de nos persos, ou une
  escouade que le jeu marque « ennemie » à moins de ~250 m d'un de nos persos ; le nombre de
  persos au-dessus. Une **légende** dans le coin de la carte ; au survol d'un point, une
  **infobulle** : nom, joueur (ou « recrue »), état, **distance** depuis le perso sélectionné. Les
  points suivent la carte quand on la fait glisser ou qu'on zoome (projection du jeu, relue à
  chaque image).
- **Minicarte** ronde dans un coin (au choix), centrée sur le perso sélectionné (sinon le sien),
  nord en haut ou tournant avec la caméra ; fond : la carte du jeu (`GUI_Map.dds`) découpée dans le
  cercle, sinon une grille. Persos des joueurs, ennemis, pings (flèche sur le bord quand ils sont
  hors du cercle), repère « N », échelle en mètres, boutons + / − et molette au survol, infobulles.
  Cachée au menu, pendant les chargements, dans l'éditeur de personnage et quand l'écran de
  gestion (carte, escouades…) est ouvert. Ctrl+Shift+N l'affiche ou la cache ; zoom, coin,
  rotation retenus dans `KenshiCoop.ini` (`[ui]`).
- **Repère au-dessus de la tête** de chaque perso de joueur (pas des recrues) : un petit curseur à
  la couleur du joueur, avec son nom, projeté avec la caméra du jeu ; rien quand le perso est hors
  de l'écran ou derrière la caméra.
- **Barre d'escouade** : le portrait des persos des joueurs reçoit un cadre à la couleur du joueur
  (dessiné par l'overlay sur la partie visible du portrait, relue dans le jeu à chaque image) ; les
  recrues gardent le cadre normal. Pas de cadre pour une case cachée (cases recyclées par la liste),
  coupée par le défilement, ou sous une fenêtre du jeu (fenêtre de gestion, inventaire…).
- **Fenêtre déplacée ou redimensionnée** : les rectangles lus dans l'interface du jeu (MyGUI) sont
  convertis à chaque image de la taille de la vue MyGUI vers celle du tampon d'affichage (celle où
  l'overlay dessine), axe par axe ; la souris, elle, va des pixels de la fenêtre (DPI compris) à ceux
  du tampon. Aucune position n'est gardée d'une image à l'autre.
- **Pings** : clic molette ou Alt+clic sur la carte, sur la minicarte ou sur le sol (un clic
  molette court : le clic molette tenu tourne toujours la caméra). Sans touche : « Aller ici » ;
  Maj : « Danger / ennemis » ; Ctrl : « Butin » ; Maj+Ctrl : « À l'aide ». Le ping apparaît chez
  tout le monde à la couleur et au nom de son joueur : sur la carte, la minicarte et en 3D au-dessus
  du point avec la distance ; il s'efface en 10 s. Au plus un ping toutes les 0,5 s et 5 en même
  temps par joueur (le plus ancien part). Un ping ne change rien au monde. Pas de son (aucun son
  d'interface simple trouvé dans le jeu).
- **Réglages** : fenêtre Multijoueur, section « Affichage » (carte, repères, barre d'escouade,
  minicarte, rotation, coin, pings) ; tout est retenu dans `KenshiCoop.ini`.
- **Fonctionnement** : l'hôte envoie trois fois par seconde à tout le monde (`MapMarkers`) la
  liste des joueurs, la position de chaque perso de l'escouade avec son joueur et s'il est « son »
  perso (celui créé pour lui ou qui porte son nom ; sinon son premier perso), et les escouades
  hostiles (au plus 128 persos, 32 escouades). La carte, la minicarte, les repères et la barre de
  chaque joueur, l'hôte compris, en sont tirés ; un perso présent dans le monde local est placé à
  sa position locale (fluide). Un ping de client part à l'hôte (`MapPing`), qui vérifie le rythme
  et le renvoie à tous. **Protocole 33.**
- **Journal** : ouverture / fermeture de l'écran de carte (et la raison quand rien n'y est dessiné :
  écran de gestion fermé, autre onglet, réglage…), rectangle de l'image, nombre de marqueurs dessinés,
  nombre de cadres de la barre (cachés, coupés, sous une fenêtre), tailles du tampon, de la fenêtre,
  de la vue MyGUI et DPI quand elles changent ; au plus une ligne par seconde ou deux.
- **Limites / à vérifier en jeu** : tout (aucun essai en jeu encore) ; un raid lointain encore
  « abstrait » (escouade pas chargée chez l'hôte) n'est pas montré ; le sol d'un ping 3D est pris
  plat à la hauteur du perso centré ; la projection 3D (origine de rendu mobile de Kenshi, docs/MOTEUR.md § 11) n'est pas encore vérifiée en jeu ; une fenêtre du jeu posée
  sur l'écran de carte n'en cache pas les marqueurs ; un Alt+clic gauche ne va plus au jeu tant que les pings sont
  actifs ; hors session (partie solo) la minicarte montre l'escouade locale, sans ennemis ni pings.

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

### Recherche, fabrication, mines et machines, énergie 🟡 (implémenté, protocole 34, tests unitaires ; à vérifier en jeu)
- **Ce qui existait avant** : rien de propre à la recherche ni à la fabrication. Les ordres
  (travailler une machine, miner) passaient déjà par l'hôte, et l'inventaire d'un établi n'était
  synchronisé que pendant qu'un joueur l'avait ouvert. Chez le client, les opérateurs d'une mine
  restaient à 0/3 (le jeu ne les ajoute que par l'IA, coupée chez lui) et l'énergie était calculée
  par son propre jeu.
- **Recherche (arbre technologique)** : une seule recherche pour la faction, celle de l'hôte :
  technologies connues, plans lus, file dans l'ordre, avancement, niveau du banc. Le client
  l'impose à son jeu (les technologies terminées le sont aussi chez lui : menu de construction et
  listes de fabrication débloqués) ; son jeu ne recherche, ne paie et ne termine rien lui-même.
  Ajouter, retirer une technologie dans la fenêtre Recherche, ou « apprendre » un plan, devient une
  demande à l'hôte, au nom d'un perso du joueur ; l'hôte vérifie (banc assez grand, artefacts et
  livres présents, pas déjà en file ni connue ; le plan est dans l'inventaire de ce perso) et son
  jeu consomme une seule fois. Deux joueurs qui ajoutent la même technologie : une seule fois en
  file, payée une fois ; le second reçoit « Cette recherche est déjà dans la file. ». Réordonner la
  file à la souris n'est pas transmis (l'ordre de l'hôte revient).
- **Établis** (armes, armures, arbalètes, sacs, robotique, forge) : les ordres de fabrication
  (objet, matériau, répéter) sont ceux de l'hôte ; ajouter, retirer, répéter dans la fenêtre
  Fabrication devient une demande, exécutée par l'hôte dans l'ordre d'arrivée (deux joueurs sur le
  même établi : le dernier l'emporte, partout). Avancement de chaque ordre identique. Le
  personnage qui travaille l'établi est celui du joueur, par un ordre (travail) qui passe par
  l'hôte.
- **Inventaires des machines** (établis, banc de recherche, mines, fermes, générateurs…) : celles
  suivies sont synchronisées chez tous les joueurs, ouvertes ou non (entrées et sorties).
- **Mines et machines** : qui travaille chaque machine (nombre et noms dans sa fenêtre), barre de
  progression et quantité produite : ceux de l'hôte. Plus de joueurs que de places : le jeu de
  l'hôte décide, tout le monde voit le même résultat.
- **Énergie** des avant-postes : sortie de chaque générateur, marche / arrêt, consommation, charge
  des batteries, alimenté ou non, totaux du panneau de la base : ceux de l'hôte ; le jeu du client
  ne calcule plus le réseau d'une ville dont il a reçu les totaux. Interrupteurs marche et batterie
  du panneau : demande à l'hôte.
- **Pas fait** : les totaux de stockage et de nourriture / eau de la base (aucun panneau du jeu
  trouvé pour eux ; ils se déduisent des inventaires, synchronisés) ; le choix du produit d'un
  bâtiment de production autre qu'un établi (les bâtiments de production de Kenshi ont un produit
  fixe).
- Expérience `research` (« recherche : … », « fabrication : … », « mine : … », « energie : … »),
  test unitaire `TestWorkshop`. Pas encore lancé en jeu.

### Non vérifiés
- Recrutement par dialogue.
- Ces actions tournent chez l'hôte, mais rien ne garantit encore que leur résultat apparaît
  correctement chez les clients.
