# Tests

Deux niveaux de tests :
1. **Tests unitaires** : la session et le protocole, sans le jeu.
2. **Tests en jeu** : de vraies instances de Kenshi pilotées par un script, qui comparent ce que
   voient l'hôte et les clients.

## 1. Tests unitaires (`tests/test_main.cpp`)

```powershell
.\build.ps1                            # compile aussi kc_tests
.\build\bin\Release\kc_tests.exe       # dernière ligne : "775 checks, 0 failed" (10 octobre 2026)
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
| `TestCrashRejoin` | client figé 7 s (gardé), puis relancé avec le même compte avant la coupure : remplacé, même nom, même perso, pas de doublon, fenêtre de commerce fermée, perso arrêté |
| `TestManyPlayers` | 1 hôte + 4 clients arrivant ensemble, 120 personnages |
| `TestJoinQueue` | file d'attente des arrivées : 3 arrivées quasi simultanées (une pendant la sauvegarde, une pendant le chargement du premier), un joueur à la fois jusqu'à la fermeture de son éditeur, positions / joueur attendu / étape côté clients, liste côté hôte, personne rejeté pendant l'attente, chaque sauvegarde contient les persos des précédents ; le joueur dans l'éditeur plante (tour suivant), un joueur quitte la file (renumérotée) ; deux éditeurs ouverts ensemble : pause jusqu'à la fermeture du dernier |

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
   De même `KC_PLAYER_NAME` (debug seulement, jamais écrit dans l'ini) donne son nom de joueur à
   chaque jeu (`setup_multi` : Hote, Joueur2, Joueur3...).
5. Pour tester le relais Steam sur un seul PC :
   - mettre `[debug] steam_loopback=1` dans `KenshiCoop.ini` ;
   - lancer `KC_JOIN="steam:28100 27960" python tools/coop_test.py <expérience>` ;
   - remettre `steam_loopback=0` ensuite.
   Un vrai test entre deux comptes Steam demande deux PC.
6. Les états comparés et les journaux de chaque essai sont dans `test_out/` (ignoré par git).

### Expériences

Plusieurs joueurs sur un PC : avant chaque lancement le banc vérifie la RAM libre (arrêt propre avec un message si moins de 2 Go ; un hôte prend ~2,2 Go, un client ~0,7 Go). Sur toute erreur, les jeux lancés par le banc sont tués (aucun processus laissé derrière).

`python tools/coop_test.py <expérience> [--save X] [--keep]`

| Expérience | Ce qu'elle vérifie |
|---|---|
| `doors` (`kctest_town`, lot A) | portes ouvertes / fermées / verrouillées comme chez l'hôte ; le jeu du client ne les change pas seul ; bouton et ordres du client exécutés chez l'hôte ; crochetage ; coffre verrouillé fermé pour le client |
| `suite` | **tout en une session, RÉUSSI / ÉCHEC point par point** (ci-dessous) |
| `four` (`--clients 3`) | 1 hôte + 3 clients : chaque client comparé à l'hôte **et aux autres clients** ; chacun ne commande que son personnage |
| `run` (`--quick`) | scénario complet avec rapport de désynchronisation |
| `soak` (`--minutes 20`) | **stabilité** : vitesse 1 → 2 → 3 → 2 → 1 par phases de 90 s, rafale de 20 s au début de chaque phase (vitesse 1/2/3 et pause changées toutes les 1 à 2 s, touches de vitesse aussi chez le client), pause / reprise en pleine activité ; pendant chaque phase : escouades de l'hôte et perso du client qui bougent, combat contre un PNJ créé, K.-O. puis fouille par le client, fenêtre de commerce ouverte / fermée, bâtiments posés par les deux, perso du client téléporté à 30000 puis ramené (une fois). Le client demande une vitesse et une pause : l'horloge de l'hôte ne doit pas bouger et le client y revient. Toutes les 30 s : les deux jeux vivants, comparaison en pause (tolérances de la `suite` : escouade ≤ 0.1, 8 pour un corps au sol ; aucun manquant, état vital ou inventaire différent), aller-retour d'une commande (< 1 s), mémoire des deux processus (psutil, sinon tasklist) : ÉCHEC si elle dépasse +40 % du relevé pris après 2 min |
| `join4` (`--clients 3`) | **plusieurs joueurs** : hôte + 3 clients lancés avec chacun un faux Steam id (`KC_FAKE_STEAM_ID`) et un nom (`KC_PLAYER_NAME` : Joueur2, Joueur3...), qui rejoignent **en même temps** : l'hôte les prend un par un (file d'attente), chacun ferme l'éditeur (`editdone`) à son tour. Vérifie la file (tous entrent, aucun rejeté ; ceux qui attendent voient leur position 2..N/N, le joueur attendu et l'étape ; le dernier voit sa position avancer ; un seul client charge le monde à la fois ; l'hôte affiche la file), puis : ids de joueur distincts, chaque client a son propre perso, chaque client voit les persos de tous les joueurs (noms et positions identiques à l'hôte, en pause) ; puis tous partent proprement et l'hôte continue |
| `stress4` (`--clients 3 --minutes 15 --hop 180 --seed 4242`) | **4 joueurs partout sur la carte** : les persos de chaque client vivent dans leur propre région lointaine (±40000 autour de la maison de l'hôte, ≥ 30000 de l'hôte et des autres) et changent de région toutes les `--hop` s ; dans chaque zone : combat contre un PNJ créé, K.-O. puis fouille, bâtiment posé. Chaque client **clique partout** (fil par client, graine dérivée de `--seed`, chaque action journalisée avec sa graine pour rejouer un plantage) : déplacements (les siens et ceux des autres, refusés), parler, coffres, glisser des objets, sélection, ordres de la barre, tâches, porter, ramasser, commerce, fermer les fenêtres, vitesse / pause, construire. Tous les joueurs en **mode dieu** (`god all` de la console admin, `heal all` toutes les 8 s). Deux clients prennent les mêmes objets d'un même corps au même instant. L'hôte change de vitesse 1/2/3 et met en pause ; le dernier client part puis revient. Toutes les 30 s : tous les jeux vivants, comparaison en pause hôte / chaque client (tolérances du `soak`), persistance par zone (un perso près d'un client absent ou décalé > 3 deux relevés de suite = ÉCHEC), aller-retour, temps de frame de l'hôte (< 250 ms), mémoire par processus (+40 % après 2 min), **objet marqué** (5 donnés à chaque joueur) compté pareil chez l'hôte et chaque client et jamais en trop. À la fin, chacun ramené à la maison |
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
| `factions` | lot B : mêmes relations au départ ; relation changée par l'hôte identique chez le client ; prime donnée puis levée par l'hôte visible chez le client ; une relation changée par le jeu du client revient à celle de l'hôte |
| `ranged` (`--shooter <clé>`) | (lot C) 3 tirs d'un arbalétrier de l'hôte sur l'escouade sont refaits chez le client, sur la même trajectoire ; même point visé ; une tourelle proche tournée chez l'hôte tourne pareil chez le client ; santé et inventaires identiques ensuite |
| `build` (lot E) | une pose du client bâtie par l'hôte puis par tous au même endroit, celle de l'hôte aussi ; avancement et fin du chantier ; démontage demandé par le client ; achat d'un bâtiment à vendre (avec `--save kctest_town`) |
| `construct` (`--material`, `--task`) | vraie construction : argent et matériaux donnés aux deux joueurs ; le client pose un chantier et ordonne à son perso de le bâtir (clic droit : `newPlayerTaskSelectedCharacters`), puis l'hôte, puis les deux en même temps ; avancement des deux côtés, matériaux qui baissent pareil, chantier terminé identique. Le numéro de tâche « construire » n'est pas connu : sans `--task`, le test essaie 1 à 99 jusqu'à ce que l'avancement bouge chez l'hôte |
| `buyhouse` (`kctest_town`) | achat d'un bâtiment à vendre par le client (`buildbuy` = le bouton de confirmation de la fenêtre d'achat) puis par l'hôte : même prix débité partout, bâtiment à nous partout, porte et conteneur utilisables ; refus sans argent ; achat simultané du même bâtiment : un seul paiement. ÉCHEC explicite s'il n'y a aucun bâtiment à vendre à 3 km |
| `farlong` (`--seconds 300`) | le perso du client à plus de 30000 unités pendant 5 min : zone comparée toutes les 30 s (PNJ, santé, inventaires), combat lancé et bâtiment posé là-bas, l'escouade de l'hôte bouge ; pas de PNJ manquant ni d'écart de position qui dure (le même perso sur deux relevés de suite) près du perso du client (1000 unités pour les absents, 300 pour les positions ; les chiffres de toute la zone vont seulement dans le journal : PNJ autour de l'escouade de l'hôte que le client n'a pas chargés, trafic), l'hôte simule la zone, pas de plantage, aller-retour d'une commande < 1 s ; retour (TP admin) et comparaison |
| `progress` | compétences, argent, bulles et ordres : l'hôte décide, le client suit |
| `ground` / `clientpickup` | objets posés et ramassés ; ramassage demandé par un client |
| `anim` / `animframe` / `gait` | animations de combat et d'action ; tout ce qui est à l'écran ; allure |
| `fx` / `fxlive` | effets météo (orage forcé puis comparaison en pause ; en direct) |
| `bodies` / `dead` / `items` | où reposent les corps ; morts ; objets |
| `lootorder` / `lootclick` / `lootui` | fouille par clic droit (`lootclick` prépare une scène à tester à la main) |
| `walk` / `trace` | trace image par image des corrections de position d'un PNJ qui marche |
| `menu` | un hôte qui héberge et un second jeu laissé **au menu principal**, pour rejoindre à la main |
| `up` | un hôte et un client connectés, laissés ouverts pour un test manuel |
| `prison` (lot D) | l'hôte met le personnage du client dans la cage la plus proche, l'enchaîne, le réduit en esclavage puis le libère : même état chez le client, gardé dans la cage, tout effacé à la fin. Il faut une cage à moins de 300 m de l'escouade (`--save` d'une sauvegarde près d'une prison ou d'un camp d'esclavagistes) |
| `crashrejoin` (`--only abcdefg`) | client **tué** (`taskkill /F`) puis relancé avec le même faux id Steam et revenu : (a) au repos, (b) en combat à la vitesse 3, (c) en portant un corps, (d) fenêtre de commerce ouverte, (e) en fouillant un corps, (f) dans l'éditeur de personnage, (g) relancé avant que l'hôte ait vu la coupure. À chaque fois : l'hôte vit, voit la coupure (< 20 s) et nettoie (journal « disconnected (… » / « cleaned up: »), le joueur retrouve les mêmes persos (clés, inventaires ; sauf combat), pas de doublon, comparaison hôte / client propre, l'hôte n'est pas resté en pause |
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
| `status` | état de la session, monde prêt, numéro, entités, PNJ, PNJ manquants, joueurs en cours d'arrivée, file d'attente (`queue=` : client `2/3` + `queueWait=` + `queuePhase=saving/loading/editor`, hôte `Joueur2:editor,Joueur3:wait`, `-` sinon ; espaces des noms remplacés par `_`), sauvegarde en cours, dernière erreur |
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
| `pos <index>` / `where <clé\|npc\|index>` | position d'un personnage, et `carried=1` s'il a l'animation « porté » (la position d'un corps porté est celle du porteur) |
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
| `wake` / `npcstate` / `ko` / `kill` | le dernier PNJ créé : se relève ; état ; K.-O. (minuteur, état inconscient et chute : `knockout` seul ne fait pas tomber) ; mort |

**Progression et apparence**

| Commande | Rôle |
|---|---|
| `stats <index>` | compétences |
| `xp <index> <compétence> <quantité>` | gain d'expérience, comme le jeu le donne (refusé chez un client) |
| `money [valeur]` | argent de la faction (l'hôte peut le fixer) |
| `factions` | nombre de factions en relation avec la faction du joueur, rang, empreinte de toutes les relations (identique hôte / client = synchro) |
| `relation <nom>` | relation de la faction du joueur avec cette faction (`ours`) et l'inverse (`theirs`), alliance, guerre |
| `setrelation <nom> <valeur>` | fixe la relation dans les deux sens (chez l'hôte : comme un événement de faction ; chez un client : simule une dérive locale) |
| `bounty <index>` | primes de ce membre de l'escouade (`faction:montant`), total, crime en cours, heures de prison |
| `givebounty <index> <nom> <montant>` | (hôte) met cette prime sur ce membre |
| `factionsync` | messages de relations / primes reçus et valeurs corrigées (client), envois (hôte) |
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
| `cage <i> [off]` | (hôte, lot D) met le membre `i` dans la cage la plus proche (300 m), ou l'en sort |
| `chain <i> [off]` | (hôte, lot D) l'enchaîne à la manière du jeu (menottes créées) / le libère |
| `enslave <i> <0-3>` | (hôte, lot D) état d'esclave : 0 non, 1 esclave, 2 en fuite, 3 ancien |
| `captive <i>` | (lot D) `in=` (2 = en cage), cage, `chained`, `slave`, `slaveof`, évadé, enlevé, peine, position, nombre de captifs suivis |

**Portes et serrures (lot A)** — `<qui>` : index dans l'escouade (autour de qui chercher) ; `<quoi>` :
`door` (la porte la plus proche), `lock` (le meuble à serrure le plus proche) ou une partie du nom
(`_` pour les espaces).

| Commande | Rôle |
|---|---|
| `doors <qui> <quoi>` | les portes ou serrures à moins de 600 unités : `nom/d<état>/f<drapeaux>/L<niveau>@distance` |
| `doorstate <qui> <quoi>` | état de la plus proche : type, état, drapeaux, niveau, ouverture |
| `doorset <qui> <quoi> open\|close\|lock\|unlock\|break\|fix` | change la plus proche comme le ferait le jeu de l'hôte |
| `doorlocal <qui> <quoi> open\|…` | la même chose sans `HostCallScope` : ce que tenterait le jeu d'un client (doit être refusé) |
| `doorbutton <qui> <quoi> open\|lock` | clic sur le bouton du panneau de la porte (chez un client : part à l'hôte) |
| `doororder <qui> <tâche> <quoi>` | ordre comme un clic droit : 72 ouvrir, 73 fermer, 76 crocheter, 77 verrouiller, 78 déverrouiller, 81 défoncer |
| `doorsknown` | portes envoyées (hôte) ou connues ici (client), et nombre d'applications réussies |

**Combat à distance (lot C)**

| Commande | Rôle |
|---|---|
| `rangedlist [rayon]` | personnages ayant une arme à distance prête près du membre 0 : `nom\|clé\|combat\|distance` |
| `shoot <tireur> <cible>` | (hôte) le tireur (index d'escouade ou clé de handle) tire une fois sur la cible ; réponse : projectile créé et son orientation |
| `shots` | compteurs : tirs envoyés (hôte), refaits / échoués (client), visées et tourelles imposées, `nogun` / `noturret` / `oriented` |
| `turrets [rayon]` | tourelles près du membre 0, triées par endroit : `type@x,z>point visé` |
| `turretaim <index> <x> <y> <z>` | (hôte) la tourelle n° index tourne vers ce point |
| `rangedaim <personnage>` | état du combat à distance d'un personnage ici : mode, état, point visé, cible, arme |

**Bâtiments (lot E)**

| Commande | Rôle |
|---|---|
| `buildtypes <nom>` | modèles de bâtiments dont le nom contient ce texte (`sid=nom`) |
| `buildplace <sid> <dx> <dz> [lacet°] [membre]` | une pose comme le mode construction, à côté du membre 0 ou de ce membre (client : demandée à l'hôte ; hôte : bâtie et annoncée) |
| `furnplace <sid> <nom du bâtiment> <dx> <dz>` | un meuble dans le bâtiment à nous le plus proche de ce nom (position relative au bâtiment) |
| `buildlist [nom] [rayon] [membre]` | bâtiments autour du membre 0 (ou de ce membre) : `sid@x,y,z:avancement/drapeaux` (1 terminé, 2 en pause, 4 démontage) |
| `buildcount` | bâtiments suivis par la session, et combien sont trouvés ici |
| `buildprogress <nom> <quantité>` | (hôte) avancement d'ouvrier sur le chantier à nous le plus proche |
| `builddismantle <nom>` / `buildbuy <nom>` | confirme le démontage / l'achat comme la fenêtre du bâtiment (un client le demande à l'hôte) |
| `buildforsale [nom]` | le bâtiment à vendre le plus proche et son prix |
| `buildinfo [nom|sid@x,y,z] [membre]` | le bâtiment le plus proche (ou celui-là) : `forsale=0/1 ours=0/1 price=` |
| `buildreq <membre> <tâche> [nom]` | ce membre seul reçoit l'ordre de travailler sur le chantier à nous le plus proche, comme un clic droit (`newPlayerTaskSelectedCharacters`) |
| `givemoney <n>` | (hôte) n cats de plus pour la faction du joueur |
| `itemtypes <nom>` | modèles d'objets (hors bâtiments) dont le nom contient ce texte |
| `giveitem <sid|nom> <n> <membre>` | (hôte) n objets neufs (fabrique du jeu) dans l'inventaire de ce membre ; par nom : les objets (types 2-4) d'abord, chaque candidat essayé jusqu'à ce que la fabrique en fasse un |
| `invcount <membre|all> <sid|nom>` | combien de ces objets ce membre (ou toute l'escouade) porte |
| `ownidx [id]` | indices d'escouade (ordre de ce jeu) des persos du joueur `id` (défaut : le mien) |

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
