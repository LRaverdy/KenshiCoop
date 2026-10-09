# Tests

Deux niveaux de tests :
1. **Tests unitaires** : la session et le protocole, sans le jeu.
2. **Tests en jeu** : de vraies instances de Kenshi pilotées par un script, qui comparent ce que
   voient l'hôte et les clients.

## 1. Tests unitaires (`tests/test_main.cpp`)

```powershell
.\build.ps1                            # compile aussi kc_tests
.\build\bin\Release\kc_tests.exe       # dernière ligne : "617 checks, 0 failed" (9 octobre 2026)
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
| `TestSessionReplication` | positions, santé, heure et pause répliquées |
| `TestDivergenceIsCorrected` | un client qui diverge est ramené à l'état de l'hôte |
| `TestRejections` | version, exe, mods, nom, partie pleine, exclusion |
| `TestWorldAuthority` | le client n'exécute rien lui-même, ses ordres passent par l'hôte |
| `TestSpawnReplication` | personnages créés chez l'hôte puis recréés chez le client |
| `TestInventories` | inventaires identiques, fouille rejouée par l'hôte |
| `TestManyPlayers` | 1 hôte + 4 clients arrivant ensemble, 120 personnages |

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
1. Le script lance les instances et ferme le petit lanceur de Kenshi s'il apparaît.
2. Il charge la sauvegarde chez l'hôte, héberge, puis fait rejoindre chaque client.
3. Il place les fenêtres côte à côte : hôte à gauche, client à droite ; en grille de 2×2 pour 4
   joueurs.
4. Chaque client reçoit un faux identifiant Steam : variable d'environnement `KC_FAKE_STEAM_ID`,
   `76561190000000002` puis +1 par client. Le mod ne la lit que si le canal de debug est actif.
   Sans elle, deux jeux sur un même PC partagent le même compte Steam.
5. Pour tester le relais Steam sur un seul PC :
   - mettre `[debug] steam_loopback=1` dans `KenshiCoop.ini` ;
   - lancer `KC_JOIN="steam:28100 27960" python tools/coop_test.py <expérience>` ;
   - remettre `steam_loopback=0` ensuite.
   Un vrai test entre deux comptes Steam demande deux PC.
6. Les états comparés et les journaux de chaque essai sont dans `test_out/` (ignoré par git).

### Expériences

`python tools/coop_test.py <expérience> [--save X] [--keep]`

| Expérience | Ce qu'elle vérifie |
|---|---|
| `suite` | **tout en une session, RÉUSSI / ÉCHEC point par point** (ci-dessous) |
| `four` (`--clients 3`) | 1 hôte + 3 clients : chaque client comparé à l'hôte **et aux autres clients** ; chacun ne commande que son personnage |
| `run` (`--quick`) | scénario complet avec rapport de désynchronisation |
| `soak` (`--minutes 15`) | longue partie à ×3, comparaisons régulières, détection de plantage |
| `facing` | un personnage de l'hôte court dans plusieurs directions : le client doit vraiment courir, dans le même sens |
| `jitter` (`kctest_town`) | les PNJ immobiles ne tremblent pas chez le client, quelle que soit la vitesse |
| `kosquad` | des membres de l'escouade K.-O. chez l'hôte tombent et restent au sol chez le client, puis se relèvent ensemble |
| `far` | le personnage du client à environ 5 km de l'escouade de l'hôte : l'hôte simule-t-il bien sa zone, et le client voit-il la même chose ? |
| `squads` | nouvelles escouades et déplacements entre escouades, depuis l'hôte et depuis le client |
| `stuck` | la copie d'un PNJ poussée dans un mur ou sous le sol chez le client revient où l'hôte l'a |
| `farnpc` | écart des PNJ qui marchent loin de l'escouade du client |
| `beds` (`kctest_town`) | le perso du client dort dans le lit libre le plus proche, puis mine |
| `tpdown` | TP admin du perso du client mis K.-O. |
| `talk` | un PNJ parle au personnage du client : la conversation tourne chez l'hôte, la fenêtre s'ouvre chez le client |
| `progress` | compétences, argent, bulles et ordres : l'hôte décide, le client suit |
| `ground` / `clientpickup` | objets posés et ramassés ; ramassage demandé par un client |
| `anim` / `animframe` / `gait` | animations de combat et d'action ; tout ce qui est à l'écran ; allure |
| `fx` / `fxlive` | effets météo (orage forcé puis comparaison en pause ; en direct) |
| `bodies` / `dead` / `items` | où reposent les corps ; morts ; objets |
| `lootorder` / `lootclick` / `lootui` | fouille par clic droit (`lootclick` prépare une scène à tester à la main) |
| `walk` / `trace` | trace image par image des corrections de position d'un PNJ qui marche |
| `menu` | un hôte qui héberge et un second jeu laissé **au menu principal**, pour rejoindre à la main |
| `up` | un hôte et un client connectés, laissés ouverts pour un test manuel |
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
20. TP admin : le personnage du joueur arrive près de l'hôte (< 30).
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
| `status` | état de la session, monde prêt, numéro, entités, PNJ, PNJ manquants, joueurs en cours d'arrivée, sauvegarde en cours, dernière erreur |
| `load <emplacement>` | charge une sauvegarde |
| `host` / `join [adresse] [port]` / `leave` | héberger, rejoindre, quitter |
| `give <joueur> <index\|all>` | donner un ou tous les membres de l'escouade à un joueur |
| `resync [joueur]` | (hôte) ce joueur, ou 0 pour tout le monde, recharge le monde de l'hôte |
| `tpplayer <joueur>` | (hôte) les personnages de ce joueur à côté du membre 0 de l'escouade |
| `consolewin` | la console hors du jeu est-elle là, et combien de texte montre-t-elle |
| `pause [0\|1]` / `paused` / `speed <x>` | pause ; état de la pause et vitesse ; vitesse |
| `hours` | heure du jeu |

**Personnages et ordres**

| Commande | Rôle |
|---|---|
| `move <index> <x> <z>` / `moverel <index> <dx> <dz>` | ordre de déplacement |
| `teleport <index> <x> <y> <z>` | (hôte) déplacement instantané |
| `probe <index> <dx> <dz> <simple\|teleport>` | essaie une méthode de positionnement et dit ce qui tient |
| `pos <index>` / `where <clé\|npc\|index>` | position d'un personnage |
| `camto <index>` | caméra sur ce membre |
| `taskreq <sélection> <tâche> <sujet>` | sélectionne ce membre seul et donne l'ordre, comme l'interface |
| `talkreq <sélection>` | ordre de parler au PNJ le plus proche |
| `orderreq <sélection> <ordre permanent>` | bouton de la barre d'escouade pour ce membre seul |
| `modes <index>` | ordres permanents (bits : 1 furtif, 2 bloquer, 4 distance, 8 narguer, 16 tenir, 32 passif, 64 poursuivre) et style de combat |
| `carryreq <sélection> <cible>` / `carrying <index>` | ordre de porter ; qui il porte |
| `kosquad <index>` | (hôte) assomme ce membre |
| `insomething <index>` | 0 rien, 1 au lit, 2 en cage |
| `objnear <index> <rayon>` | objets autour (modèle × nombre) |
| `objreq <sélection> <tâche> <nom>` | ordre d'utiliser l'objet le plus proche dont le nom contient ce texte (`_` pour les espaces) |
| `furnparent <nom>` | de quel bâtiment l'objet le plus proche de ce nom est le meuble |
| `trace <handle> <images>` | journalise les corrections de position d'un personnage côté client |

**PNJ, combat, santé**

| Commande | Rôle |
|---|---|
| `spawnnpc <dx> <dz> [index]` | (hôte) crée un PNJ, copie d'un PNJ voisin, à côté du membre 0 (ou de ce membre) |
| `fight <index>` | ce membre attaque le dernier PNJ créé |
| `wake` / `npcstate` / `ko` / `kill` | le dernier PNJ créé : se relève ; état ; K.-O. ; mort |

**Progression et apparence**

| Commande | Rôle |
|---|---|
| `stats <index>` | compétences |
| `xp <index> <compétence> <quantité>` | gain d'expérience, comme le jeu le donne (refusé chez un client) |
| `money [valeur]` | argent de la faction (l'hôte peut le fixer) |
| `look <nom>` / `lookset <nom> <clé> <valeur>` | résumé de l'apparence ; changer un curseur |
| `editchar` / `editdone` | (client) ouvrir l'éditeur sur son personnage ; valider comme le bouton |

**Escouades**

| Commande | Rôle |
|---|---|
| `squads` | escouades vues par cette machine (`nom: membre,membre \| …`) |
| `squadmove <nom> <autreNom\|new>` | glisser ce portrait dans l'escouade d'un autre membre, ou une nouvelle |

**Dialogues**

| Commande | Rôle |
|---|---|
| `say <index> <texte…>` / `says` | faire dire une bulle ; bulles rejouées chez le client et la dernière |
| `convo <index> <k>` | (hôte) le k-ième PNJ le plus proche engage la conversation avec ce membre |
| `dialog` / `answer <n>` | (client) fenêtre de conversation affichée ; choisir une réponse |

**Objets, fouille, contenants**

| Commande | Rôle |
|---|---|
| `drop <index>` | ce membre pose un objet non équipé (répond avec son handle) |
| `pickup <index> <clé>` / `pickupreq <index> <modèle> <x,y,z>` | ramasser (hôte) ; ce qu'envoie le « ramasser » d'un client |
| `groundnear <rayon>` / `ground <clé de l'hôte>` | objets au sol autour ; cet objet est-il au sol ici ? |
| `invmove <de> <vers> <rang\|main\|worn> [qté] [x] [y] [section]` | ce que fait un glisser-déposer d'inventaire |
| `loot <cible> [index]` / `lootorder <cible>` | fouille d'un corps telle qu'un client la fait ; le chemin du clic droit |
| `containerreq <sélection> <nom>` | clic droit sur le contenant le plus proche de ce nom |
| `contake <sélection> <nom>` | dans la fenêtre ouverte, prend le premier objet |
| `contcount <nom\|any>` | piles dans le contenant le plus proche de ce nom |
| `robuststats` | (client) personnages débloqués d'un mur et PNJ lointains replacés depuis le dernier appel |
| `strand <dx> <dy> <dz>` | (client) déplace la copie locale du PNJ le plus proche (dans un mur, sous le sol) |
| `bedreq <i>` | ce membre seul reçoit l'ordre de dormir dans le lit libre le plus proche (tâche 258) |
| `minereq <i>` | ce membre seul reçoit l'ordre d'exploiter la mine la plus proche (tâche 87) |
| `tradegui` | type de fenêtre de commerce en attente dans l'interface (0 = aucune) |

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
