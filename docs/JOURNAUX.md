# Journaux et consoles

Le mod écrit un journal en anglais. L'hôte y voit aussi ce qui se passe **chez chaque client**, en
direct, dans une fenêtre hors du jeu. Ce document dit où trouver les journaux, comment les lire et
comment s'en servir pour comprendre un problème.

## Fichiers

- **`KenshiCoop.log`**, dans le dossier de Kenshi
  (`C:\Program Files (x86)\Steam\steamapps\common\Kenshi` par défaut). Une ligne par événement :
  `HH:MM:SS.mmm  message`. Le fichier est écrit sur disque à chaque ligne : il survit à un
  plantage du jeu.
- **Archive** : au démarrage, le journal de la session précédente part dans
  `KenshiCoop-logs\KenshiCoop-AAAA-MM-JJ_HH-MM-SS.log`, daté de sa dernière écriture. Les 30 plus
  récents sont gardés.
- **Deux Kenshi sur le même PC** (tests) : le second écrit `KenshiCoop-<pid>.log`, faute de pouvoir
  ouvrir `KenshiCoop.log`. Ces fichiers-là ne sont pas archivés.
- **Chez un ami** : son propre `KenshiCoop.log` et son dossier `KenshiCoop-logs`, sur son PC.
  L'essentiel arrive de toute façon dans le journal de l'hôte (voir plus bas).
- Les 500 dernières lignes restent aussi en mémoire, pour les consoles.

## Ce que l'hôte reçoit des clients

- **Les lignes des clients** : chaque client envoie à l'hôte toutes les nouvelles lignes de son
  journal, toutes les 0,5 s. L'hôte les écrit précédées du nom du joueur, avec sa propre heure :
  ```
  19:33:33.303  [Coucoudz 2] posture: 1:1:2712646400:6:3029174784 falls as on the host (ok, fall 1/4 this minute)
  ```
  - 64 lignes au plus par envoi, 512 en attente au plus.
  - Au-delà, le client signale `(N lignes non transmises)`.
  - Les lignes du canal de test (`debug: …`) ne sont pas relayées.
  - Si un client plante, ses dernières lignes peuvent ne jamais arriver : il faut alors son propre
    `KenshiCoop.log`.
- **Les rapports de synchro** : toutes les 5 s, chaque client envoie un rapport. L'hôte en écrit
  un par minute, ou toutes les 15 s quand quelque chose cloche :
  ```
  [Coucoudz 2] sync: 94 characters followed, 0 NPCs not there yet, 0 squad members missing, 0 off, max offset 2.1, 60 fps
  [Coucoudz 2] sync NEEDS A LOOK: 94 characters followed, 0 NPCs not there yet, 0 squad members missing, 2 off, max offset 40.1, 232 fps
  ```

  | Champ | Sens |
  |---|---|
  | characters followed | personnages de l'hôte connus du client |
  | NPCs not there yet | PNJ de l'hôte pas encore présents chez lui (zones en cours de chargement) |
  | squad members missing | membres de l'escouade introuvables chez lui. Grave : ce n'est pas la même sauvegarde |
  | off | personnages **immobiles** à plus de 5 unités (environ 50 cm) de leur position chez l'hôte |
  | max offset | plus grande correction de position appliquée depuis le dernier rapport |
  | fps | images par seconde du client |

  « NEEDS A LOOK » apparaît dès qu'il manque un membre d'escouade, que plus de 3 personnages sont
  décalés, que la correction dépasse 30 unités ou que le client tourne sous 15 images par seconde.
  Après une chute ou une téléportation, une grosse correction ponctuelle est normale.

## La console hors du jeu (hôte)

Fenêtre Windows « KenshiCoop — console de l'hôte ». On la met sur un second écran, ou on y passe
par Alt+Tab ; elle ne prend jamais le focus au jeu.
- **Ouverture** : toute seule quand on héberge (`[ui] host_console=1`, par défaut). Ensuite
  **Ctrl+Shift+W** dans le jeu, ou la commande `fenetre`, la montre ou la cache. La fermer ne fait
  que la cacher.
- **Ligne d'état** : état de la session, nombre d'entités et de PNJ, vitesse (et PAUSE), ton nom.
- **Tableau des joueurs** :

  | Colonne | Contenu |
  |---|---|
  | `#` | numéro du joueur (1 = hôte) |
  | Joueur | nom |
  | Ping | en ms |
  | Persos | personnages qu'il commande |
  | État | `hote`, `arrive (telechargement)`, `cree son personnage` ou `en jeu` |
  | Synchro | résumé de son dernier rapport. ⚠ quand il faut y regarder, ou quand le rapport date de plus de 20 s |

- **Journal en direct**, lignes des clients comprises (relu toutes les 250 ms).
- **Ligne de commande** : Entrée ou bouton « Envoyer ».

Dans le jeu, **Ctrl+Shift+K** ouvre une console équivalente dans l'overlay. **Ctrl+Shift+D**
écrit dans le journal un diagnostic complet : personnages de l'escouade, sélection, santé,
empreinte du monde.

### Commandes
Tapées dans l'une ou l'autre console. Chaque commande est écrite dans le journal (`> commande`),
avec sa réponse en dessous.

| Commande | Qui | Effet |
|---|---|---|
| `help` | tous | la liste des commandes |
| `fenetre` (ou `window`) | tous | ouvre ou ferme la console hors du jeu |
| `players` | tous | joueurs : numéro, nom, ping, personnages |
| `status` | tous | état de la session et de la synchro |
| `kick <id>` | hôte | retire un joueur |
| `give <id>` | hôte | donne les personnages sélectionnés au joueur `<id>` |
| `save` | hôte | sauvegarde la partie (`coop_AAAAMMJJ_HHMM`) |
| `pause [0\|1]` | hôte | pause ou reprise |
| `speed <x>` | hôte | vitesse du jeu (0,1 à 10) |
| `tp <id> [vers <id>]` | hôte | téléporte les personnages du joueur `<id>` près de ton personnage sélectionné, ou de ceux d'un autre joueur |
| `resync [id]` | hôte | le joueur `<id>`, ou tout le monde sans `id`, recharge ton monde tel qu'il est |

Un client n'a que `help`, `fenetre`, `players` et `status`.

## Lignes courantes et leur sens

### Démarrage
| Ligne | Sens |
|---|---|
| `KenshiCoop 0.3.0 starting` | le mod démarre |
| `steam: ready (id …)` | Steam répond : on peut rejoindre par Steam |
| `ready. name='…' port=… join=…` | mod prêt |
| `world ready: N player characters`, puis `---- diagnostics (world loaded) …` | une partie est chargée, avec l'état de l'escouade |
| `unsupported kenshi_x64.exe (sha256 …)` | mauvaise version de Kenshi : le mod est désactivé |
| `disabled: …` | une vérification a échoué (prologue d'une fonction…) : mod désactivé |
| `overlay unavailable: …` | pas d'overlay, mais le multijoueur fonctionne |
| `frame listener unavailable: …` | impossible de rejoindre depuis le menu principal |

### Session et arrivée d'un joueur
| Ligne | Sens |
|---|---|
| `hosting on UDP port 27960` / `steam: hosting through Steam (id …)` | partie hébergée |
| `* X is joining...` | quelqu'un se connecte |
| `X is back with their character` / `created X's own character` | personnage retrouvé (compte Steam ou nom) / nouveau personnage |
| `saving the world for X` → `world saved: N files, N KB` → `world sent to X` | sauvegarde pour le joueur dont c'est le tour, puis envoi du monde |
| `join queue: X's turn to join (N waiting after them)` / `join queue: X's turn is over (raison), N still waiting` | file d'attente des arrivées : début et fin d'un tour (raisons : dans le monde, éditeur fermé, parti, retiré, éditeur jamais ouvert, 10 min d'éditeur) |
| `join queue: X (save being made / loading / character editor), then Y, then Z` | la file à chaque changement |
| `join queue: X left during their turn` | le joueur en cours est parti ou a planté : le suivant commence |
| client : `join queue: position 2/3, waiting for X (étape)` / `join queue: our turn, the host is saving its world for us` | ce que voit un joueur qui attend |
| `* X is in the world` | le joueur a chargé le monde |
| `X opened the character editor: the game waits for them` / `X closed the character editor` | éditeur ouvert (partie en pause) / fermé |
| `nobody is in the character editor any more: the game resumes` | le dernier éditeur ouvert s'est fermé : la partie reprend |
| `new looks for X` | nouvelle apparence appliquée et envoyée aux autres |
| `X reconnected: the old connection is closed` | même compte Steam reconnecté |
| `removed X: <raison>` / `rejected a player: <raison>` | joueur exclu / refusé (version, mods, partie pleine…) |
| `resync: X reloads the host's world` | resync demandé |
| `* X left` | départ d'un joueur |
| `session failed: …` | la session s'est arrêtée (connexion perdue, monde déchargé…) |

### Ce que font les joueurs
| Ligne | Sens |
|---|---|
| `[X] order "first aid" (25) on Y -> ok` | ordre d'un client exécuté par l'hôte. `FAILED` : le jeu de l'hôte l'a refusé |
| `[X] mode "stealth" -> ok` | bouton de la barre d'escouade d'un client |
| `[X] change squad -> ok` | déplacement d'escouade |
| `[X] move -> FAILED` | un déplacement n'est journalisé qu'en cas d'échec |
| `client task N: subject NOT found by kind and place` | l'hôte ne trouve pas l'objet visé (meuble, machine) au même endroit |
| `[X] conversation with Y` / `[X] they say: "…"  answers: 1. … \| 2. …` / `[X] answers: "…"` / `[X] conversation over` | conversation d'un client, réplique par réplique |
| `[X] answer to an older line ignored (line a, now b)` | la conversation avait avancé (vitesse 3) : la réponse est ignorée |
| `[X] walks away from the conversation with Y` / `conversation N ended by the mod (ok)` | le joueur part : l'hôte termine la conversation |
| `conversation N ends: <raison>` | fermeture forcée (combat, TP, K.-O., perso disparu, fin sans fermeture) |
| `[X] order "talk" (12) -> refused: Y is busy talking with Z` / `conversation refused: that NPC is in conversation N with another player's character` | une conversation à la fois par PNJ (« occupé ») |
| `the host's own conversation: the game is not paused in co-op` | la pause que le jeu met en ouvrant la fenêtre de l'hôte est levée |
| `NPC Y joined the player faction: a squad character now` / `a new squad character (Y) goes to X, who was talking with it (recruited)` | recrue d'une conversation |
| `[X] goes to look into Y` → `[X] opens Y` → `[X] closes the container` | contenant |
| `[X] steals Y (unseen)` / `[X] caught stealing Y` | vol réussi / vol repéré (la fenêtre se ferme) |
| `client item move done: …` / `client item move refused: …` | déplacement d'objet demandé par un client |
| `[X] placement of Y at x,y,z refused: invalid spot (in water or acid (ground …, surface …))` | pose d'un client que le mode construction aurait refusée (aussi : `too close to a town`, `inside another building`, `on top of another building`, `too steep`, `the ground there is not loaded on the host`) : rien n'est bâti, le joueur reçoit la raison |
| `refused an inventory move from player N` | demande refusée (pas son personnage, PNJ conscient…) : l'état réel est renvoyé |

### Lignes venues des clients (préfixe `[nom]`)
| Ligne | Sens |
|---|---|
| `[X] vitals: … faints as on the host (now down=… unconscious=… dead=…)` | K.-O. rejoué chez le client. Répétée toutes les 0,5 s : il ne prend pas (correctif en cours) |
| `[X] posture: … falls as on the host (ok, fall N/4 this minute)` | chute rejouée chez le client ; jamais sur un perso porté, enchaîné, en cage, au lit, prisonnier ou esclave ; 4 par corps et par minute au plus |
| `[X] posture: … does not stay down here after N falls: left as it is for a minute` | deux chutes de suite n'ont pas tenu (le jeu du client remet le corps debout ou assis) : plus d'essai pendant la minute, au lieu d'une chute toutes les 2 s |
| `[X] posture: … dies and falls as on the host` / `… dead body lies down again as on the host` | corps mort chez l'hôte : la copie vivante (doublure recréée) meurt et tombe ; un cadavre debout est recouché |
| `[X] stand-in for the host's dead K made dead and lying (dead=1)` | doublure recréée pour un corps mort (zone rechargée) : morte et couchée dès sa création |
| `[X] carry: …` | corps porté ou posé comme chez l'hôte |
| `refused locally: … for a character this player does not own …` | (client) une demande visait un perso qui n'est pas à ce joueur : elle n'est pas envoyée, le joueur voit « Action refusée » |
| `auth: [X] <demande> refused: <règle> (…; n recent of this rule)` | (hôte) contrôle central : demande d'un client refusée avant son traitement ; `n` = refus récents de cette règle (décroît, demi-vie 60 s) |
| `refused: actor N not owned by player P (…)` | (hôte) l'acteur nommé n'est pas un perso de ce joueur (celui de l'hôte, d'un autre joueur, un PNJ, inconnu) : rien n'est exécuté |
| `refused: client task T (via V) not run: task … needs …` | (hôte) la cible ne convient pas à la tâche (type, handle périmé) : refusé avant les fonctions d'ordre du jeu |
| `refused: client task T … the selection could not be made exactly its actor` | (hôte) la sélection n'a pas pu être réduite au seul acteur : ordre non exécuté |
| `SAFETY: a character of the host's selection got a task from a client's order` | (hôte) ne doit jamais apparaître : un perso de l'hôte a reçu une tâche pendant un ordre de client |
| `host rejected our request (raison, seq N): …` | (client) réponse `Result` de l'hôte |
| `[X] effect N ended by the host after …` / `gone by itself …` | cycle de vie des effets météo |
| `[X] our copy A of the host's B changed squads here (now C): followed` / `our copy of the host's B changed squads here (now C): followed` | le jeu du client a changé d'escouade une copie (une mort la met dans l'escouade des morts), donc son handle : le mod la suit au lieu d'en créer une autre |
| `new world (generation N): every per-world state reset` / `…; K stand-in(s) of the previous one still here, removed` | un monde chargé (connexion, resync, chargement) : tout l'état lié au monde est remis à zéro ; doit apparaître une fois par chargement, jamais après un simple redimensionnement ou une zone qui charge |
| `[X] stand-in A for the host's B is gone here: it can be recreated` | la doublure a vraiment disparu (nettoyée, zone déchargée) : elle sera recréée |
| `[X] posture: … lies N units from the host's body: stood up to fall where the host's lies (R/2 this minute)` | corps K.-O. (jamais mort) couché à plus de 20 unités de celui de l'hôte : relevé puis recouché à sa place (2 fois par minute au plus, 10 s d'écart, seulement s'il reste une chute pour le recoucher) ; il est remis debout d'autant en deçà du corps de l'hôte que sa chute précédente l'a porté loin de ses pieds |
| `[X] clock: N h behind the host's: running +3 %` / `ahead of the host's: running -10 %` / `clock: back on the host's (…): host speed again` | l'horloge du client rattrape celle de l'hôte en tournant un peu plus vite ou plus lentement |
| `[X] clock: N h behind the host's: set to its H h` / `… (refused)` | écart trop grand (client figé par une zone qui charge, pause arrivée en retard) : heure recalée d'un coup ; « refused » : l'horloge du jeu introuvable |
| `[X] inventory of K not as on the host: SID xQ at SECTION X,Y here at … / missing` | après une reconstruction, un objet n'est pas là où l'hôte l'a (le premier trouvé) ; 3 essais puis abandon jusqu'au prochain changement |
| `[X] a local Y stands in for the host's` | un PNJ local remplace un PNJ de l'hôte de même type |
| `[X] removed N local character(s) the host does not have` | PNJ créés par le jeu du client, retirés |
| `[X] removed the local character 'Nom': the host does not have it` / `not removed: the local character 'Nom' … is in one of the player's squads` | chaque perso retiré, nommé ; un perso d'une escouade du joueur (portrait dans la barre) n'est jamais supprimé |
| `[X] N host squad members are missing in the local world` | **le client n'a pas la même escouade** : mauvaise sauvegarde |
| `[X] the host paused but this game refuses to pause …` | un menu ou un éditeur bloque la pause chez lui |
| `[X] client order refused (task N opens a window)` | ordre non encore synchronisé (commerce…) |

### Plantages et erreurs
| Ligne | Sens |
|---|---|
| `CRASH (game crash reporter\|unhandled) code=… at … (module+0x…) … phase='…'`, puis les registres et `stack[i] module+0x…` | plantage du jeu, avec le module fautif, la pile et ce que faisait le mod (`game main loop …` : dans la frame du jeu lui-même ; `tick: …` : dans un appel du mod). `game crash reporter` : le plantage que le jeu rapporte (« Kenshi has crashed », crashDump*.zip) ; `unhandled` : un plantage hors de sa boucle (souvent à la sortie, après le premier). Un seul par processus. Toujours dans le journal **de la machine qui a planté** |
| `tick: access violation caught (code …)` / `tick exception: …` | erreur rattrapée dans le mod, sans plantage. À signaler |
| `squad bar check: N of M tab(s) hold K portrait(s) the game does not have (tab T ('Escouade') item I holds 0x…); …emptied…` | **la barre d'escouade s'est corrompue** (une case pointe hors des portraits du jeu : ce qui a fait planter les soaks de 14:54 et 20:20) ; l'onglet est vidé et le jeu le remplit à nouveau. À signaler, avec les lignes juste avant |
| `squad bar: a portrait cell's data 0x… is not one of the game's portraits: not drawn (N so far)` | la même chose vue au moment du dessin : la case n'est pas redessinée au lieu de planter. À signaler |
| `squad bar check: the squad bar or the game's portraits cannot be read here` | la vérification de la barre ne peut pas se faire (autre version du jeu ?) : elle est sautée |

## Diagnostiquer

- **« Chez moi ce n'est pas pareil »** : dans la console de l'hôte, regarder la colonne Synchro
  de ce joueur et ses lignes `[nom]` autour du moment signalé. Le bouton « Resync » (ou
  `resync <id>`) lui recharge le monde tel qu'il est chez l'hôte.
- **Un ordre ne fait rien chez un client** : chercher `[nom] order … -> FAILED` ou
  `subject NOT found` dans le journal de l'hôte.
- **Un joueur bloqué** : `tp <id>`, ou le bouton « TP vers moi ». Ne marche pas sur un personnage
  à terre.
- **Un plantage** : récupérer le `KenshiCoop.log` de la machine qui a planté. Si elle a été
  relancée depuis, prendre le fichier le plus récent de son dossier `KenshiCoop-logs`.
