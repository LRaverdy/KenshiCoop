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
  19:33:33.303  [Coucoudz 2] posture: 1:1:2712646400:6:3029174784 falls as on the host (ok, ragdoll now 0)
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
| `KenshiCoop 0.1.0 starting` | le mod démarre |
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
| `saving the world for joining players` → `world saved: N files, N KB` → `world sent to X` | sauvegarde puis envoi du monde |
| `* X is in the world` | le joueur a chargé le monde |
| `X opened the character editor: the game waits for them` / `X closed the character editor` | éditeur ouvert (partie en pause) / fermé |
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
| `[X] goes to look into Y` → `[X] opens Y` → `[X] closes the container` | contenant |
| `[X] steals Y (unseen)` / `[X] caught stealing Y` | vol réussi / vol repéré (la fenêtre se ferme) |
| `client item move done: …` / `client item move refused: …` | déplacement d'objet demandé par un client |
| `refused an inventory move from player N` | demande refusée (pas son personnage, PNJ conscient…) : l'état réel est renvoyé |

### Lignes venues des clients (préfixe `[nom]`)
| Ligne | Sens |
|---|---|
| `[X] vitals: … faints as on the host (now down=… unconscious=… dead=…)` | K.-O. rejoué chez le client. Répétée toutes les 0,5 s : il ne prend pas (correctif en cours) |
| `[X] posture: … falls as on the host (…)` | chute rejouée chez le client |
| `[X] carry: …` | corps porté ou posé comme chez l'hôte |
| `refused locally: … for a character this player does not own …` | (client) une demande visait un perso qui n'est pas à ce joueur : elle n'est pas envoyée, le joueur voit « Action refusée » |
| `auth: [X] <demande> refused: <règle> (…; n recent of this rule)` | (hôte) contrôle central : demande d'un client refusée avant son traitement ; `n` = refus récents de cette règle (décroît, demi-vie 60 s) |
| `refused: actor N not owned by player P (…)` | (hôte) l'acteur nommé n'est pas un perso de ce joueur (celui de l'hôte, d'un autre joueur, un PNJ, inconnu) : rien n'est exécuté |
| `refused: client task T (via V) not run: task … needs …` | (hôte) la cible ne convient pas à la tâche (type, handle périmé) : refusé avant les fonctions d'ordre du jeu |
| `refused: client task T … the selection could not be made exactly its actor` | (hôte) la sélection n'a pas pu être réduite au seul acteur : ordre non exécuté |
| `SAFETY: a character of the host's selection got a task from a client's order` | (hôte) ne doit jamais apparaître : un perso de l'hôte a reçu une tâche pendant un ordre de client |
| `host rejected our request (raison, seq N): …` | (client) réponse `Result` de l'hôte |
| `[X] effect N ended by the host after …` / `gone by itself …` | cycle de vie des effets météo |
| `[X] a local Y stands in for the host's` | un PNJ local remplace un PNJ de l'hôte de même type |
| `[X] removed N local character(s) the host does not have` | PNJ créés par le jeu du client, retirés |
| `[X] N host squad members are missing in the local world` | **le client n'a pas la même escouade** : mauvaise sauvegarde |
| `[X] the host paused but this game refuses to pause …` | un menu ou un éditeur bloque la pause chez lui |
| `[X] client order refused (task N opens a window)` | ordre non encore synchronisé (commerce…) |

### Plantages et erreurs
| Ligne | Sens |
|---|---|
| `CRASH code=… at … (module+0x…) …`, puis `stack[i] module+0x…` | plantage du jeu, avec le module fautif et la pile. Toujours dans le journal **de la machine qui a planté** |
| `tick: access violation caught (code …)` / `tick exception: …` | erreur rattrapée dans le mod, sans plantage. À signaler |

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
