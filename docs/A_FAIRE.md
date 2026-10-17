# Bugs signalés, à traiter

Remontés par les parties entre amis. Chaque entrée garde la date et ce qu'on a vu.

## 16 octobre 2026, soir : test de la 0.3.5 avec rob1 et Geoffrey (journal de l'hôte `KenshiCoop.log`, 21:20-)

1. **Geoffrey bloqué sur « un écran dans un désert » quand il arrive pendant l'éditeur de rob1.**
   - 21:23:55 Geoffrey arrive pendant que rob1 est dans l'éditeur de personnage : monde chargé, puis
     « join queue: position 2/2, waiting for rob1 » (partie en pause). 21:26:19 rob1 ferme l'éditeur,
     tour de Geoffrey : « character editor opened: ok » chez lui, puis **plus aucune action** pendant
     2 min 30 ; il quitte à 21:28:55 (« left during their turn »).
   - Le « désert » est sans doute la scène de l'éditeur : pendant l'éditeur, les persos sont à ~65 654
     unités de la ville (« sync NEEDS A LOOK … max offset 65658 » chez rob1 pendant son éditeur, pareil
     chez Geoffrey au 2e essai).
   - Revenu à 21:29:36 : l'éditeur rouvre tout de suite et marche (renommé « plopette » 21:29:59).
     Différence : au 1er essai l'éditeur s'ouvre après 2 min d'attente en file, en pause.
   - À faire : savoir ce qu'il voyait (fenêtre d'éditeur absente ? présente mais muette ?) et lire son
     `KenshiCoop.log` ; reproduire en jeu : deux clients qui rejoignent ensemble, le 2e passe de la
     file à l'éditeur.
2. **Annuler une tâche « suivre » (croix rouge) ne l'arrête pas.**
   - L'hôte crée une tâche « suivre rob1 » pour A1 (21:31:42 « jobs: A1 now has 1 job(s) »), l'annule
     avec la croix rouge du panneau des tâches (21:31:48 « A1 now has 0 job(s) », recopié chez les deux
     clients) : A1 continue de suivre rob1. Un ordre de déplacement ailleurs l'arrête.
   - À vérifier : même chose en solo sans le mod (comportement du jeu : la croix retire la tâche de la
     liste sans arrêter l'action en cours ?) ou effet du mod (suivi des tâches / ordres renvoyés).
     Si c'est le jeu, arrêter l'action en cours quand sa tâche est retirée.
3. **Vêtements impossibles à placer chez rob1, en boucle.**
   - « inventory rebuild incomplete: cannot place 543-gamedata.base at shirt » et 1167 pour
     `1:8:657470720:2` et `1:14:1630126976:1`, toutes les ~2 s dès 21:23:27 : l'emplacement « shirt »
     refuse l'objet chez le client, il finit dans l'inventaire principal. Pas bloquant, mais journal
     inondé et tenue différente de l'hôte.
4. **Achat d'esclaves : la fenêtre s'ouvre chez l'hôte, pas chez le client.**
   - Geoffrey parle au Chef Esclavagistes (21:33:15, 21:33:21, 21:33:31) : la conversation marche chez
     lui (« they say: Tu veux faire du commerce ? », il répond « Je voudrais acheter des esclaves »,
     « conversation N: the player answered 0 (ok) »). Ensuite aucune ligne trade/fenêtre dans le
     journal : la fenêtre d'achat d'esclaves (action de dialogue, pas la fenêtre de commerce normale)
     s'affiche sur l'écran de l'hôte et rien chez le client.
   - À faire : repérer l'action de dialogue « acheter des esclaves » et la fonction qui ouvre sa
     fenêtre, la renvoyer au client qui a parlé (comme le commerce avec stand-in), fermer celle de
     l'hôte, et faire passer l'achat (paiement, esclave qui rejoint l'escouade du client) par l'hôte.
     Vérifier aussi « Je voudrais vendre des esclaves ». Tester en jeu avec l'expérience de dialogue.
5. **Scier une cage : le client ne voit pas le décompte (barre de progression).**
   - rob1 « cut lock » (285) sur Cage Prisonnier à 21:47:44, Geoffrey « cut lock » 21:48:21, « pick
     lock » (76) plusieurs fois 21:48:15-21:48:39, « force lock » (286) 21:48:46 : ordres acceptés par
     l'hôte (« -> ok »), le travail se fait chez l'hôte, mais la barre/le décompte au-dessus de la
     serrure n'apparaît pas chez le client. Les clics répétés sur « pick lock » viennent sans doute de
     là (rien ne montre que ça avance).
   - À faire : trouver où le jeu garde la progression du crochetage/sciage (sur la porte ou la tâche du
     perso) et ce qui affiche la barre, envoyer la progression de l'hôte au client qui fait l'action et
     l'afficher chez lui (barre du jeu ou overlay). Vérifier aussi crochetage et forçage.
6. **L'hôte charge une sauvegarde rapide, rob1 revient et ne voit plus ses persos dans son escouade.**
   - 21:50:03 « world unloaded », 21:50:06 nouveau monde (génération 2, horloge 47.5 h au lieu de
     54.5 h : sauvegarde plus ancienne). rob1 revient à 21:50:35 : « remembered character is not a
     living member of the squad in this world: a new one for them » puis, dans la même milliseconde,
     « 2 character(s) they had are theirs again (1 of them their own character) » (messages
     contradictoires). Ses persos sont dans l'escouade `1:55:2114746112` (« Rob », créée par lui à
     21:27), `idx 1` et `idx 2` (Taureau de Somme) ; chez lui la vue suit « Taureau de Somme ».
   - Il ne voyait plus ses persos dans la barre d'escouade. L'hôte a fait `resync 2` à 21:51:21 ;
     après, la vue suit « rob1 » (à confirmer : la resync a-t-elle réparé ?).
   - Pistes : barre d'escouade du client qui montre l'escouade 1 (celle de l'hôte) et non la sienne
     après un rechargement ; le perso mémorisé (`1:1:245483392:3`, son tout premier) n'existe pas dans
     la sauvegarde rapide, la reprise devrait le dire sans annoncer « a new one ».
   - Aussi : Geoffrey est resté connecté pendant le chargement de la sauvegarde par l'hôte (barre
     d'escouade 21:50:28, déconnecté seulement à 21:50:38) : un client reste-t-il dans l'ancien monde
     quand l'hôte charge une autre partie ? Il faut que tous les clients rechargent le nouveau monde
     (ou soient renvoyés au menu avec un message).
   - À reproduire en jeu : session à 2, le client crée son escouade, l'hôte fait une sauvegarde rapide,
     joue, recharge, le client revient.
   - **Revu à 22:09:50 avec Geoffrey, sans sauvegarde rapide : simple `resync 3`.** Il libérait des
     esclaves juste avant (« pick shackles » 22:09:49). Au retour : « 1 character(s) they had are theirs
     again » seulement, et sa barre d'escouade montre « player portraits 5-6 : 2-3 hidden » : il ne voit
     pas les persos de l'escouade où il est. Piste forte, commune aux deux cas : au départ, « player
     cleaned up: N character(s) back to the host » ; au retour, seuls les persos mémorisés lui sont
     rendus, les autres membres de son escouade (recrutés ou libérés en cours de partie, ou pas
     encore notés dans `KenshiCoop-players.txt`) restent à l'hôte et la barre les cache car ils
     « appartiennent à un autre joueur ». À vérifier : `KenshiCoop-players.txt` ne contient pour
     Geoffrey que `1:1:245483392:4`. Corriger : noter tout perso qui entre dans l'escouade d'un joueur
     (recrue, esclave libéré, prisonnier) et rendre au retour toute son escouade, pas seulement les
     persos mémorisés.
7. **Barre d'entraînement restée sur le perso du client après une resync pendant l'entraînement.**
   - rob1 s'entraîne sur Mannequin Exerc. MkIII (« order "train" (97) » 21:50:54), l'hôte fait
     `resync 2` à 21:51:21 pendant l'entraînement : déconnexion, « player gone: … halted where it
     stands » pour ses 2 persos, sauvegarde envoyée à 21:51:27, rechargement. Après, la barre
     d'entraînement reste affichée au-dessus de son perso chez lui (il relance « train » à 21:52:07).
   - Pistes : l'arrêt « halted » ne retire pas le perso du mannequin (opérateur toujours inscrit sur la
     machine, état d'entraînement gardé dans la sauvegarde envoyée) ; ou la barre du client est
     recréée au chargement sans être liée à l'état de l'hôte. Regarder ce que fait la sortie normale
     d'un mannequin (fin de tâche, retrait de l'opérateur) et l'appliquer au nettoyage d'un joueur
     parti ; vérifier aussi la même chose avec un lit, une chaise, une machine.
   - À reproduire en jeu : client qui s'entraîne, resync par l'hôte, regarder la barre chez les deux.
   - **Fait (branche night-posture, à vérifier en jeu : expérience `trainresync`).** Cause probable,
     lue au désassembleur (pas encore vue en jeu) : `reThinkCurrentAIAction` (`0x5C8330`), seul appel
     de l'ancien arrêt, ne fait **rien** tant que la tâche en cours tient le corps (`CharBody`+0x68 →
     +0x70 → octet +0x22) : le perso continuait de s'entraîner, la sauvegarde envoyée gardait l'ordre « s'entraîner », et la
     barre (une étiquette de la tâche, vtable `0x1703DE0`, détruite seulement à la fin de la tâche)
     restait chez le client, dont l'IA ne finit jamais rien. Maintenant `HaltCharacter` fait le
     « stop » du jeu (`kenshi::StopCharacter` : arrêt + ordres effacés + tâche en cours terminée),
     retire le perso de toute machine / mannequin / chaise qui le liste encore
     (`UseableStuff::stopOperating`), le sort d'un lit s'il est éveillé, et l'écrit
     (« the game's stop: ok; N order(s)… »). Chez le client, une copie qui garde des tâches ou des
     ordres de la sauvegarde après deux `reThink` voit ses ordres effacés de la même façon
     (`DropLeftoverTasks`, une ligne « tasks: … kept … »). Voir CHANGELOG 0.3.5.
8. **Geoffrey en furtif sans les animations de furtivité.**
   - « [Geoffrey] mode "stealth" -> ok » (22:01:02, 22:09:08, 22:09:30), « knock out (stealth) »
     22:01:06 : le mode passe bien chez l'hôte, mais le perso marche/court normalement au lieu de se
     déplacer accroupi. rob1 aussi passe en furtif (22:04:33-22:04:51).
   - À préciser : sur quel écran (celui de Geoffrey, de l'hôte, de rob1) l'animation manque.
   - Pistes : la posture furtive est un état du perso (pas une anim « kind=4 » jouée) que le client
     ne recopie pas, ou que la copie pilotée par l'hôte perd à chaque mise à jour de mouvement
     (vitesse/anim de marche imposées). Recopier l'état furtif de l'hôte sur toutes les copies et le
     garder ; tester en jeu : client en furtif qui marche, vu par l'hôte et par un autre client, aux
     vitesses 1, 2 et 3.
   - **Fait en partie (branche night-posture, à vérifier en jeu : expérience `sneakwatch`).** Lu au
     désassembleur : la marche accroupie suit l'état de furtivité du perso (`Character`+0xE0, 1 =
     furtif ; `setStealthMode` `0x5C7E20`, vt 0x370), pas l'ordre permanent (+0xD4). Chez l'hôte ce
     sont les actions de l'IA qui le mettent à 1 ; chez un client l'IA ne tourne pas et le
     `Character::update` de la copie le remettait à 0 à chaque image. Désormais l'hôte envoie « furtif
     en ce moment » (bit 7 des modes de `Progress`, ignoré par un client 0.3.4) et le crochet
     `hk_setStealthMode` impose cet état aux copies (marche, icône de furtivité). **Cause supposée,
     pas prouvée** : une copie proche joue déjà les animations de l'hôte ; si elle marchait debout,
     c'est peut-être le jeu de l'hôte qui ne faisait pas marcher accroupi le perso du client. Pour le
     savoir, l'hôte écrit maintenant une fois par minute « stealth: … has the stealth order on and
     walks without sneaking on the host » ; `sneakwatch` compare l'état et les animations chez
     l'hôte, le joueur et un autre client aux vitesses 1, 2 et 3.
9. **Perso de Geoffrey capturé comme esclave → à son retour, nouveau perso (éditeur).**
   - 22:10:49 « captivity: plopette in Cage Prisonnier now caged slave-state=1 slave-of=1084 (Esclavagistes)
     sentence=110h », enchaîné à 22:11:00. Geoffrey quitte/revient plusieurs fois (22:11:02, 22:12:45,
     22:14:18). À 22:13:43 : « Geoffrey's remembered character is not a living member of the squad in
     this world: a new one for them » → nouveau perso `1:1:245483392:3:1995605504`, éditeur de
     personnage ; `KenshiCoop-players.txt` ne connaît plus que celui-là (plopette `…:4:3858764544`
     oublié).
   - Un perso capturé n'est pas mort : il doit rester celui du joueur (comme en solo, où un perso
     esclave reste à toi et peut s'évader). Corriger le test « living member of the squad » : un
     perso du joueur vivant mais prisonnier/esclave/dans une cage (ou dans une autre escouade) reste
     le sien ; ne faire un nouveau perso que si le sien est vraiment mort ou introuvable, et
     l'expliquer au joueur. Ne pas écraser l'ancienne ligne de `KenshiCoop-players.txt`.
   - Lié au point 6 (persos rendus au retour) : même logique de reprise.
   - À reproduire en jeu : perso d'un client capturé par des esclavagistes, le client quitte et
     revient.
10. **Geoffrey KO : au réveil il se voit encore au sol, l'hôte le voit marcher.**
   - Son perso (`1:1:245483392:2:279541024`, « plopette », le joueur s'appelle maintenant plopette
     dans le journal) s'évanouit chez l'hôte à 22:41:29 ; « faints as on the host (now down=0
     unconscious=1) » chez lui et chez rob1. Puis **aucune ligne de réveil** : le journal entier ne
     contient aucun « wakes / gets up / comes to », alors que l'hôte le fait marcher ensuite. Il quitte
     à 22:42:47 (« player gone … halted »).
   - Code (lu le 16/10) : le réveil existe, mais côté posture, pas côté vitals. `ApplyVitals`
     (`plugin/world.cpp` ~2332) ne rejoue que la chute et la mort ; c'est `Apply` (posture, ~1080) qui
     relève une copie couchée quand l'hôte a la sienne debout (`kenshi::StandUp`), seulement si
     `kc::motion::MayStand` : `!held && !hostDown && localDown`. `held` = `PostureHeld` (~620) : porté,
     captif chez l'hôte, **ou chez le client chained / inSomething / slaveState / kidnapped / sentence
     > 0**. Le journal n'a **aucune** ligne « still down here … stand-ups » ni « up again » : le
     relèvement n'a pas été tenté. Piste forte : ce perso a été en cage (« put in its cage »
     22:23:37, « out of its cage » 22:29:43) et sa copie garde une trace de captivité (esclave,
     peine) : `held` reste vrai, le code de posture le laisse « au captif » et personne ne le relève.
   - À faire : quand l'hôte a le perso debout et libre, ne pas le compter `held` sur des restes locaux
     (se fier à `hostCaptive_`, et effacer esclave/peine/enchaîné de la copie quand l'hôte le libère) ;
     écrire une ligne quand un perso reste couché alors que l'hôte l'a debout, avec la raison (held,
     carry, captivité, ragdoll refusé). Vérifier aussi « down » (à terre mais conscient) → debout.
   - À tester en jeu : perso d'un client assommé (et un PNJ), attendre le réveil, regarder chez
     l'hôte, le client concerné et un autre client ; aussi aux vitesses 2 et 3.
   - **Fait (branche night-posture, à vérifier en jeu : expérience `kowake`).** Cause confirmée par le
     journal : à 22:41 l'**hôte lui-même** avait plopette « slave-state=2 slave-of=1084 sentence=160h »
     (esclave évadé qui marche, depuis 22:30:29), donc `hostCaptive_` le comptait captif et `held`
     restait vrai : ce n'étaient pas des restes locaux, mais toute marque d'esclave ou de peine
     comptait comme « tenu ». Maintenant seul ce qui tient vraiment la pose compte
     (`kc::motion::PoseHoldOf`) : porté ici, en cage ou au lit chez l'hôte, enchaîné chez l'hôte ; les
     marques de la copie ne comptent que tant que l'hôte n'a rien dit de ce perso. Quand l'hôte le
     tient libre, les marques restées sur la copie (chaînes, esclave, peine, enlevé, évadé) sont
     effacées (`ClearCaptivityTraces`, une ligne). Une copie K.-O. sans ragdoll (drapeau inconscient,
     pose couchée) est relevée aussi, PNJ compris ; une copie qui reste à terre alors que l'hôte a la
     sienne debout est écrite une fois par minute avec la raison (« posture: … stays down here while
     the host's walks: shackled on the host »).
11. **Geoffrey ne voit pas les persos de l'autre escouade de rob1 (vers 23:04).**
   - rob1 a plusieurs escouades (« Rob » + une autre, déplacements de persos entre elles 22:19).
     Le rapport de Geoffrey dit pourtant « 0 squad members missing, max offset 5 » : le mod ne voit
     pas d'écart.
   - Indices : « stream: plopette is sent 81 of 130 characters (around their own) », « host characters
     155 followed, 82 here: 23 within 3000 of ours, 59 farther (54 dormant, 54 left to the game's far
     update) ». L'hôte n'envoie que les persos autour de ceux du joueur : si l'autre escouade de rob1
     est loin de Geoffrey, elle n'est plus envoyée, reste figée (« dormant ») ou est retirée par le jeu
     chez lui.
   - À préciser : absents ou figés à un ancien endroit ? Loin de Geoffrey ou à côté ?
   - Piste de correction : toujours envoyer les persos des joueurs (toutes les escouades de tous les
     joueurs et de l'hôte), quelle que soit la distance, au moins position et état à basse fréquence ;
     compter dans « squad members missing » les persos des autres joueurs aussi.
   - À tester en jeu : client avec deux escouades éloignées, un autre client près d'une seule.
   - Vu aussi vers 00:21 : un raid en route, loin de la base et des joueurs, dont les PNJ « ne bougent
     pas ». Chez rob1 au même moment : 52 persos « dormant » (non envoyés, figés), et 10 doublures de
     l'escouade `1:979:3547973632` détruites par son jeu (« wandered out of zone »), recréées plus tard.
     Chez l'hôte, un groupe loin de tout perso n'avance que par à-coups (mise à jour lointaine du jeu,
     voir [[kenshi-engine-facts]]) : vérifier sur quel écran ils étaient figés.
   - **Précision : figés sur la carte (écran Carte), chez tout le monde, hôte compris.** Ce sont donc
     les marqueurs « raid » du mod qui ne bougent pas : `KenshiWorld::ReadMapThreats`
     (`plugin/map.cpp` ~685) fait la moyenne des `GetPosition` des persos de l'escouade. Loin de tout
     joueur, le jeu ne déplace plus les persos eux-mêmes (zone non chargée) mais le groupe (« platoon »)
     de façon abstraite : la position des persos reste celle où leur zone s'est déchargée, le marqueur
     aussi, alors que le raid avance probablement. À faire : pour un groupe hors zone chargée, lire la
     position du platoon (celle que le jeu utilise pour la simulation lointaine) au lieu de celle des
     persos ; vérifier en jeu en suivant un raid de loin, puis quand il arrive.
12. **Bilan désync de la session (journal 21:20-23:05, 30 700 lignes).**
   - Rapports « sync NEEDS A LOOK » : rob1 56 (contre 83 bons), Geoffrey 37/37, plopette 32/43 ;
     7 resyncs de zone automatiques, 9 resyncs complètes demandées par l'hôte, 17 déconnexions.
   - Ce que les clients corrigent en boucle (lignes du journal) :
     - **Lits : ~2 650** (« put in its bed as on the host » 963+517, « still in its bed here while the
       host has it free: taken out » 657, « out of its bed first » 520). Le jeu du client remet les PNJ
       au lit et le mod les en sort : ping-pong permanent. Empêcher l'IA locale de coucher une copie
       pilotée par l'hôte au lieu de corriger après coup.
       **Fait (branche night-posture, à vérifier en jeu : expérience `nightbeds`).** Les appels de
       `Character::setInBed(true)` (`0x32E2F0`) viennent de trois endroits : `Task_UseBed`
       (`0x351770`), `Task_OperateMachine` (`0x358670`) et le chargement (`Character` vt 0x2B8,
       `0x627B30`). Chez un client, le système de tâches fait encore tourner les tâches de la
       sauvegarde : elles couchaient les copies dans le lit de leur choix. Le crochet `hk_setInBed`
       refuse ce « au lit » sur une copie pilotée par l'hôte hors des appels du mod (le lit de
       l'hôte passe toujours par `ApplyCaptive`) ; le chargement passe. Les tâches restées de la
       sauvegarde sont aussi effacées (point 7).
     - **PNJ « dormants » non envoyés : 1 055** (« no longer sent to us … had moved N units here: put
       back »). Le jeu du client fait bouger des copies que l'hôte n'envoie plus ; même cause que le
       point 11 (envoi seulement autour des persos du joueur).
     - **Sols : ~940** (sous/au-dessus du sol de l'hôte, remis ; 57 abandons « left to the game »),
       surtout dans les bâtiments à étages.
     - **PNJ locaux en trop non supprimés : 303** (« Vagabond Affamé », « Garde caravane », « Chasseur
       Esclaves », « Fermier Hors-la-Loi » : persos que l'hôte n'a pas, dans une escouade avec une copie
       d'un perso de l'hôte, donc laissés). Le jeu du client fait apparaître ses propres PNJ dans des
       escouades de l'hôte : les retirer sans emporter la copie (sortir la copie de l'escouade
       d'abord), ou bloquer leur apparition.
     - Mouvement « gets nowhere » : 891 ; porter un corps : 372 ; 20 ordres refusés (« carry someone »,
       « pick shackles » : mauvaise cible, copies pas liées).
   - Les bugs de reprise (points 6, 7, 9, 10) s'ajoutent : persos perdus au retour, KO jamais relevé,
     barre d'entraînement restée.
   - Priorité proposée pour le prochain lot : lits (ping-pong), envoi des persos des joueurs et des
     zones éloignées (dormants), PNJ locaux en trop, puis reprise après resync/retour. Mesurer avant et
     après sur une longue partie (exp_soak) avec ces compteurs.
13. **Déguisements : on ne voit pas qu'on est déguisé, et la Nation Sacrée attaque direct (~23:15-23:20).**
   - Les joueurs (clients) portaient une tenue de déguisement ; aucun indicateur « déguisé » chez eux,
     et les Nations Sacrées les ont attaqués dès qu'ils sont passés devant.
   - Précision de l'hôte : ce sont les persos des **clients** qui se font attaquer (pas celui de
     l'hôte). Pistes (a) et (b) en tête.
   - Le mod ne gère pas du tout le déguisement (aucune occurrence de « disguise » dans `plugin/`), et
     le journal n'a rien dessus. L'IA des PNJ tourne chez l'hôte : c'est donc l'état de déguisement de
     la copie **chez l'hôte** qui compte.
   - Pistes : (a) l'équipement des persos des clients chez l'hôte n'est pas celui qu'ils voient (cf.
     les « cannot place … at shirt », 278 lignes ce soir : un vêtement mis dans l'inventaire au lieu
     d'être porté) ; (b) le jeu calcule le déguisement à l'équipement via un chemin que les demandes
     d'objets des clients (« client item move ») ne déclenchent pas ; (c) chez le client, l'état
     déguisé n'est jamais recopié depuis l'hôte, d'où l'indicateur absent.
   - À faire : trouver où le jeu garde le déguisement d'un perso (champ du Character / faction
     apparente) et ce qui le recalcule ; vérifier chez l'hôte qu'un perso de client équipé d'une
     tenue de la Nation Sacrée est bien déguisé ; recopier l'état au client pour l'indicateur.
   - À tester en jeu : client qui s'habille en tenue de paladin (vrai chemin UI, glisser dans les
     emplacements), passe devant des paladins ; comparer avec le perso de l'hôte habillé pareil.
14. **Impossible de glisser-déposer au sol un objet de l'inventaire d'un PNJ (fenêtre de pillage).**
   - Clients (plopette, ~23:28-23:29 : « loot window opened » x12) : glisser un objet de l'inventaire
     d'un PNJ à terre vers le sol ne fait rien.
   - Le journal n'a **aucune** ligne de dépôt : ni « ground: drop of … refused here » (refus du mod,
     `KenshiWorld::ClientInventoryDrop`, `plugin/ground.cpp` ~258, qui n'autorise que les
     `dropSources` = persos du joueur), ni toast « Objet non posé ». Donc soit le dépôt depuis la
     fenêtre de pillage (`ForgottenGUI::showTradeWindow`) passe par une autre fonction que
     `Inventory::dropItem` (pas accrochée), soit le refus se fait sans log.
   - À faire : vérifier en solo sans le mod que le jeu permet ce glisser au sol ; trouver la fonction
     appelée (pillage → sol) ; accepter comme source un PNJ à terre qu'un perso du joueur pille (même
     règle de portée que le pillage), envoyer la demande à l'hôte, et toujours écrire une ligne et un
     toast quand un dépôt est refusé. Tester en jeu par le vrai chemin UI (glisser, pas `uidrop`).
   - Vu en même temps : un objet au sol `100-limbs.mod` (membre de prothèse ?) « announced again »
     toutes les ~6 s (23:26:48-23:27:18) en changeant de place (1237 → 1312) : recréé à chaque fois chez
     les clients avec un nouvel id. Objet qui glisse sur une pente : à surveiller (doublons ?).
15. **Après un TP admin longue distance, rob1 et plopette n'arrivent pas à vendre au marchand.**
   - 23:37:42-43 `admin tp 2 host` / `admin tp 3 host` (vers (-61727, 324, 16327)), « the host teleports
     us far: the connection waits while the zone loads ». Puis Marchand Armures Ronin : conversation,
     « trade window type 1 … sent to that player », « trade window open: 32 shop counters » chez
     plopette (23:38:00) et rob1 (23:38:44), avec « the merchant here did not sell from the host's 32
     counter(s) (a stand-in without that shop as its home): given the shop's building ».
   - Jusqu'à 23:39:02 : **aucune** ligne « sells … » ni « inventory move asked of the host (trade) » :
     les ventes faites dans la fenêtre ne partent pas chez l'hôte (ni refus écrit).
   - 23:39:11 les deux sont nettoyés (déconnexion / resync), rob1 revient et vend normalement à
     23:39:40 (« sells Massue de Mercenaire x1 … for 324 cats ») : un rechargement répare.
   - Bizarre : l'argent du marchand passe de 25000 (23:38:00) à 22963 (23:38:43) sans aucune vente
     écrite : quelqu'un a vendu chez l'hôte sans passer par le chemin client ? (l'hôte, ou une vente
     locale du client appliquée autrement) — à éclaircir, risque de doublon d'argent/objet.
   - Pistes : après le TP lointain, la zone est rechargée chez le client : ses persos ou le marchand
     sont recréés (doublure « stand-in » refaite) et les handles/alias ne correspondent plus, donc
     l'achat/vente du côté joueur n'est plus reconnu comme un inventaire « à nous » ; ou la fenêtre
     de commerce vise des comptoirs de la doublure. Comparer avec le commerce sans TP au même endroit.
   - À tester en jeu : `admin tp` lointain d'un client, puis vente/achat chez un marchand ; vérifier
     l'argent des deux côtés.
16. **« Geoffrey », perso secondaire de rob1, invisible chez rob1 (vers minuit).**
   - rob1 contrôle un perso nommé « Geoffrey » (« floor view: follows our 'Geoffrey' » 23:48, 23:52,
     00:08:39 ; fenêtre de prospection ouverte pour lui 23:49:52) mais ne le voit pas à l'écran.
   - Piste : c'est très probablement `1:1:245483392:3:1995605504`, le perso créé pour Geoffrey à
     22:13:43 quand plopette était en cage (point 9). Geoffrey a quitté **pendant l'éditeur de
     personnage** (22:14:18 « left during their turn (their character is still to make) »), donc ce
     perso n'a jamais fini l'éditeur. Pendant l'éditeur, le jeu déplace le perso ~65 650 unités plus
     loin (point 1) et peut le cacher : chez un client qui a chargé le monde à ce moment, la copie
     peut rester cachée / dans la scène de l'éditeur. `KenshiCoop-players.txt` le donne toujours à
     Geoffrey (steam id 76561198036086638) : comment est-il devenu le perso de rob1 ?
   - À vérifier : quel handle exactement (demander une ligne de diagnostic chez rob1), sa position
     chez rob1 et chez l'hôte, état visible/caché du nœud de scène ; ce que le mod fait d'un perso
     dont l'éditeur n'a pas été fini (le rendre visible et normal partout, ou le supprimer si le
     joueur ne revient pas).
   - Contournement : resync de rob1.
17. **plopette ne peut pas prendre d'objets sur le Taureau (bête de somme) contrôlé par l'hôte.**
   - 00:08:52-00:09:39 : « inventory change refused: that character belongs to Coucoudz » (x6) : refus
     voulu par la règle de propriété (un client ne touche pas aux persos d'un autre joueur, cf. la
     sécurité « actorsafety »). Puis en essayant de poser au sol depuis le sac du taureau : « ground:
     drop of 580-gamedata.base x1 refused here … (a backpack's inventory …); **COULD NOT PUT THE ITEM
     BACK** » (x3) : l'objet n'a pas pu être remis dans le sac chez le client (perdu de son écran
     jusqu'à la prochaine recopie de l'inventaire ; rien ne bouge chez l'hôte). Dans la foulée l'hôte
     pose lui-même des 580 au sol (00:10:00-00:10:14).
   - Question de règle pour l'hôte : un joueur peut-il prendre dans l'inventaire d'un perso/animal
     d'un autre joueur ? Proposition : oui pour les bêtes de somme et les sacs (partage d'équipe), non
     pour les persos humains d'un autre joueur, ou un réglage `KenshiCoop.ini`.
   - Bugs à corriger quoi qu'il arrive : un refus doit **toujours** remettre l'objet où il était
     (« COULD NOT PUT THE ITEM BACK » ne doit jamais arriver), et le joueur doit voir un message clair
     (« Ce personnage appartient à Coucoudz »), pas seulement le journal.
   - **Suite (00:11:27-00:11:41) : pas de commerce non plus.** Une fenêtre de commerce s'ouvre pour
     plopette (« 1 shop counters … the merchant has 2060 cats » : un perso/animal de l'escouade de
     l'hôte, pas un marchand), mais chaque prise d'objet est refusée : « trade refused: price 0 does
     not fit a purchase » + toast « Le prix n'a pas pu être compté : rien n'a changé. » (x5), avec la
     fenêtre qui se ferme/rouvre à chaque essai. Entre persos de la même faction le jeu met le prix à
     0 (échange gratuit) et la vérification de prix de l'hôte le prend pour une erreur.
   - Résultat : aujourd'hui **aucun moyen** pour deux joueurs de se passer un objet sauf le poser au
     sol. Il faut un chemin d'échange entre joueurs : soit l'échange à prix 0 entre persos du même
     camp accepté (avec la règle de propriété décidée ci-dessus), soit un glisser direct d'un perso à
     l'autre quand ils sont côte à côte, demandé à l'hôte. Ne pas laisser la fenêtre clignoter
     (fermée/rouverte à chaque refus).
18. **Ferme à légumes : rob1 ne voit ni l'eau ni la récolte (output).**
   - rob1 travaille sur « Ferme Légumes S » (« operate machine » 00:46-00:49, tâche permanente) et
     l'ouvre plusieurs fois (« opens Ferme Légumes S » 00:49:41-00:50:15) : la fenêtre ne montre ni le
     niveau d'eau ni l'inventaire de sortie.
   - Hôte 00:49:28 : « machine: Ferme Légumes S crops: stage 0 -> 3, harvested 0 -> 0/20, grown -0.19,
     died 0.00 » : le stade des cultures est recopié, la récolte est vide chez l'hôte aussi (0/20), et
     la croissance est **négative** (manque d'eau ?).
   - Le mod ne recopie pas du tout l'eau des fermes (aucune occurrence de « water » hors construction
     dans `plugin/`). La sortie (récolte) passe-t-elle par la recopie des inventaires de machines
     (« inventory 7251 (a machine) differs here … with its window closed » 00:50:48) ?
   - À faire : trouver les champs de la ferme (eau stockée / besoin, fertilité, stock de récolte) et
     les recopier depuis l'hôte comme le stade des cultures ; vérifier que la récolte apparaît chez le
     client et qu'il peut la prendre (demande à l'hôte). Vérifier chez l'hôte que la ferme reçoit bien
     de l'eau (croissance négative). Tester en jeu : ferme arrosée, cycle complet jusqu'à la récolte,
     vu par l'hôte et un client, vitesses 1-3.
19. **Clic gauche sur un puits : le perso de rob1 court jusqu'au puits.**
   - « [rob1] goes to look into Puits » à chaque clic (00:53:57-00:54:24), sans ordre de sa part.
   - Cause trouvée dans le code : `hk_showInvBuilding` (`plugin/hooks.cpp` ~893) : sur un client,
     l'ouverture du panneau d'inventaire d'un bâtiment (mine, ferme, puits, machine : un simple clic
     gauche pour regarder) envoie le perso sélectionné le plus proche **marcher** jusqu'au bâtiment
     (`QueueContainerRequest` → `ContainerOpen` chez l'hôte), pour que l'hôte envoie le vrai contenu.
     C'est voulu mais gênant : en solo, regarder ne fait pas bouger.
   - À faire : demander le contenu à l'hôte **sans déplacer le perso** (message « regarder » à part,
     ou l'inventaire des machines déjà recopié : « inventory 7251 (a machine) differs here » montre
     qu'il arrive), et ne faire marcher le perso que quand le joueur prend/pose vraiment un objet
     (comme le jeu). Lié au point 18 (ferme : eau et récolte invisibles dans ce même panneau).
20. **Stockage de minerai : rob1 n'arrive pas à reprendre le minerai, il y retourne tout seul.**
   - 00:56:08-00:56:52 : rob1 tire le minerai (`42158-gamedata.base`, cuivre ?) du stockage, plusieurs
     fois : « inventory move asked of the host: 42158-gamedata.base x65 backpack_content ->
     backpack_content 0,0 » et chez l'hôte « client item move done: … x65 backpack_content ->
     backpack_content 0,0 » (« done », pas de refus), puis l'inventaire recopié le remet dans le
     stockage chez rob1. Une seule fois ça part ailleurs (« backpack_content -> main 3,0 » x1).
   - Piste forte : la demande décrit source et destination par **nom de section** ; le stockage de
     minerai a une section nommée comme celle d'un sac (`backpack_content`), le sac de rob1 aussi :
     l'hôte exécute « backpack_content → backpack_content 0,0 » **dans le même inventaire** (le stockage),
     donc rien ne bouge vraiment, et la recopie remet le minerai en place chez rob1. Vérifier dans le
     code de `MoveInventoryItem` / la demande client comment l'inventaire source et l'inventaire
     destination sont nommés (handle du perso, du sac, du bâtiment) pour un bâtiment de stockage.
   - À tester en jeu : stockage de minerai rempli, client qui en tire vers son inventaire principal,
     vers son sac, et l'inverse ; même chose pour les autres stockages (nourriture, pierre, eau).
21. **rob1 ne peut pas donner à Voth la tâche « ranger dans le stockage de fruivert » (et sans doute
    les autres stockages).**
   - Stock Fruivert posé par l'hôte à 01:01:15 (« built the host's Stock Fruivert » chez les clients).
     De 01:01:37 à 01:01:52, chaque clic de rob1 sur le stockage ne donne que « goes to look into Stock
     Fruivert / opens Stock Fruivert » (x9) : **aucune** « client order task … (Shift: a permanent
     job) » ni refus. Le travail sur le Puits passe bien, lui (01:02:32 « order "operate machine" (87)
     on Puits -> ok »).
   - Piste forte : `ClientLootContainer` (`plugin/hooks.cpp` ~494) prend tout clic droit de tâche
     `kTaskLootTarget` sur un objet qui a un inventaire et le transforme en « aller regarder dedans »
     (`QueueContainerRequest`) ; le clic droit (avec Maj pour une tâche permanente) sur un stockage,
     que le jeu traduit sans doute par cette même tâche ou une tâche de rangement proche, n'est donc
     jamais envoyé comme ordre. À vérifier : quel numéro de tâche le jeu donne pour « ranger dans ce
     stockage » ; laisser passer comme ordre normal (et tâche permanente avec Maj) tout ce qui n'est
     pas un simple pillage. Vérifier aussi que le stockage est bien fini chez l'hôte (pas encore un
     chantier) au moment du clic.
   - Lié aux points 19 et 20 (panneaux et stockages). Tester en jeu : chaque type de stockage, clic
     droit et Maj+clic droit d'un client, comparé à l'hôte.
22. **Autres lignes à regarder** : « Ce personnage appartient à un autre joueur. » chez rob1 (21:26:26,
   21:26:58, 21:27:15) ; « 1 host squad members are missing in the local world » chez rob1 à
   l'arrivée de Geoffrey (21:23:56).

## 12 octobre 2026, soir : partie entre amis en v0.3.3 (journal de l'hôte `KenshiCoop.log`, 21:35-)

1. **Geoffrey n'arrive pas à parler à Barth (marchand), puis ça marche après une resync (22:15:34).**
   - Avant la resync, l'hôte ne reçoit **aucun** ordre « talk » de Geoffrey : son jeu n'a rien envoyé
     (ses autres ordres passent : « medic » 22:11:41, « go to » 22:11:58). Après la resync : « talk order
     sent: Plopette walks to Barth » (22:15:57), conversation puis commerce normaux.
   - Indices : de 22:12 à 22:13:27 son rapport dit « 1 off, max offset 48 » (un perso décalé de ~48
     unités chez lui, en permanence) ; captivité « a merchant caged (Lit) » / « free » répétée
     (22:09-22:10) pour des marchands couchés dans leur lit, dont Barth (« captivity: Barth in Lit now
     caged » 22:09:34, « free » 22:09:43).
   - Pistes : (a) sa copie de Barth n'était pas liée à celle de l'hôte (copie locale ou stand-in sans
     alias) et le clic ne produisait pas d'ordre capturé ; (b) Barth « en captivité » dans son lit chez
     lui, ce que le jeu ne laisse pas aborder. Il faut le journal de Geoffrey (`KenshiCoop.log` de son
     dossier Kenshi, 22:11-22:15) pour trancher ; ajouter un log côté client quand un clic « parler »
     ne donne pas d'ordre.
   - **Fait (0.3.4, branche `fix-talk`, à vérifier en jeu : expérience `npctalk`)**. Le journal de l'hôte
     contient bien celui des clients : aucune ligne « client order task 12 » ni refus pendant ces
     minutes, donc le jeu de Geoffrey n'a appelé aucune fonction d'ordre. Désassemblage : le clic droit
     sur un PNJ (`0x7FB410`) ne donne « parler » que si `Character::<peut qu'on lui parle>`
     (`0x67AC00`) répond oui **sur la copie du client** ; sinon rien, sans message. Corrigé : clic sans
     ordre repéré, expliqué dans le journal du client (lié ?, piloté par l'hôte ?, captif ici / chez
     l'hôte ?, contrôle du jeu ici, distance) et l'ordre « parler » part quand même pour un PNJ debout ;
     PNJ retrouvé chez l'hôte par type et endroit si la copie n'est pas liée. Aussi : « Ce personnage
     appartient à un autre joueur. » à 22:13:58 et 22:14:14 chez Geoffrey = un clic pendant que sa
     sélection n'avait aucun perso à lui (un PNJ sélectionné ?) : maintenant écrit aussi dans son
     journal pour un ordre « parler ».
   - Reste : ce qui différait sur la copie de Barth chez Geoffrey (la nouvelle ligne « right click on …
     gave no order here » le dira à la prochaine partie : « the game's talk check here: NO » + lié ou
     non). Piste : doublure refaite après le déchargement de sa zone, sans le dialogue ou la boutique de
     l'original. Un PNJ couché « dans un lit » ici seulement (point 11) n'a pas d'ordre de secours (le
     jeu donne alors des ordres de lit) : seulement écrit.
2. **nass4 pose un corps au sol et le corps disparaît (vers 22:16:40).**
   - Corps `1:117:212280272:1` : « put down » 22:16:39, posé comme chez l'hôte chez les trois clients ;
     chez rob il était à 377 unités de celui de l'hôte (relevé puis refait tomber), et à 22:16:52
     « stand-in … for the host's 1:117:212280272:1 is gone here (no longer in the game) » chez rob1.
   - À vérifier : disparu chez qui (nass4, l'hôte, rob) ; pourquoi la copie de rob a quitté le jeu (zone
     déchargée ? détruite par le jeu quand le corps est posé loin de la copie ?). Elle doit revenir
     (« can be recreated ») : vérifier qu'elle revient vraiment.
   - **Traité (branche `fix-char-states`, 0.3.4, à tester en jeu)** : chez rob, la copie avait été
     suivie à 22:11:11 dans l'escouade locale 428, une escouade de « Bandit affamé » que l'hôte n'a pas
     (trois retirés par `Reconcile` à 22:11:20-27) ; à 22:16:51.851 `Reconcile` en retire un de plus et à
     22:16:52.852 la copie a quitté le jeu sans destruction vue (« no longer in the game ») : retirer un
     membre debout d'une escouade locale emporte vraisemblablement ses membres à terre. Un inconnu dans
     la même escouade locale qu'une de nos copies n'est plus retiré. Retour de chaque copie perdue dit
     dans le journal (« back here (made|found|adopted) N s after it left our game »), et « still not
     back » une fois après 20 s. Le délai croissant ne vaut que pour les doublures que nous avons
     créées (`spawned`) ; une copie venue de la sauvegarde (ce corps) est recréée dès 1,5 s si l'hôte
     l'envoie et qu'elle est près de nos persos. Aucun « stand-in made » n'était journalisé : on ne sait
     pas si elle est revenue le 12/10.
3. **Un membre (101-limbs.mod) lâché puis ramassé en boucle par l'hôte** (22:13:46-22:13:49, toutes les
   0,5 s chez nass4 et Geoffrey : « host dropped item 4960:3941772544 … placed here », « host picked up …
   removed here ») : un objet au sol que l'IA de l'hôte ramasse et relâche, ou un aller-retour du mod.
   - 🔧 **Corrigé dans le code (0.3.4), à vérifier en jeu.** Ni l'IA ni un joueur : le membre, tranché
     dans un combat, roulait sur une pente (de (-60860,339,15736) à (-60716,332,15700) en 3 s) ; le relevé
     de l'hôte le réannonçait (« announced again ») à chaque relevé où il avait bougé de plus de 30
     unités, et chaque client le retirait puis le recréait. Il n'est plus réannoncé qu'une fois immobile.
     À vérifier : après un combat avec membres tranchés, au plus une ligne « announced again » par objet.
5. **rob1 n'arrive pas à donner des tâches (travaux permanents) à ses persos ni aux autres (22:53).**
   - Ses ordres arrivent et l'hôte les exécute (« client task 87 (via 2) run for a character: ok », 60
     fois en quelques secondes : il reclique sans voir de résultat), mais aucun travail permanent n'est
     créé pour ses persos (pas de « jobs: … now has N job(s) »), alors que ceux de nass4 (22:11) et de
     Geoffrey (22:48) sont créés de la même façon (via 2).
   - La « Petite Ressource Cuivre » visée n'a qu'**une place**, prise (`workers 1/1`) pendant ses clics ;
     libérée à 22:54:36, son ordre suivant passe (22:54:40, `workers 0 -> 1/1`). Pourtant aucun message
     « toutes les places sont prises » ne lui est montré dans ce cas (il l'est pour la Ressource Fer).
   - Bouton « Travaux » (ordre 15, `mode "jobs (permanence)"`) envoyé deux fois en 2 s (22:53:13.8 et
     15.6) : la valeur envoyée est l'inverse de l'état **local** du perso, qui ne suit celui de l'hôte
     qu'à la synchro suivante (modes, 1 fois par seconde) : le second clic l'a sans doute éteint. À
     faire : changer l'état local tout de suite (prédiction) et/ou le bouton allumé dès l'envoi ; dire au
     joueur quand un ordre de travail n'a rien créé (machine pleine, pas de mode Travaux).
   - « aux autres » : vérifier les persos partagés (recrues de personne) et ceux d'un autre joueur
     (refus attendu, avec un message).
   - **Corrigé dans le code (v0.3.4) ; `coop_test.py clientjobs` le 13/10 : 15/17, les deux échecs
     venaient du test (corrigé : transport 88 sans cible jamais ajouté par le jeu ; combat au filon de
     la ville), à relancer** :
     - cause : en solo un travail permanent vient de **Maj + clic droit** (le bouton JOBS dit seulement
       si le perso fait ses travaux). `newPlayerTaskSelectedCharacters` lit la touche lui-même : celle
       du client n'arrivait jamais, l'hôte utilisait celle de son propre joueur. Les travaux de nass4
       et Geoffrey sont venus d'un Maj tenu chez l'hôte à ce moment ; rob n'en a jamais eu. Le client
       envoie maintenant sa touche, l'hôte exécute l'ordre avec (sans changement de protocole) ;
     - « places prises » non dit à rob : c'est **son propre perso** qui minait la Petite Ressource
       Cuivre (workers 0 -> 1 juste après son ordre de 22:51:12) ; l'ancien message se taisait dans
       ce cas. Il reçoit maintenant « ton personnage y travaille déjà… Maj + clic droit » ;
     - bouton JOBS prédit chez le client (3 s), clics identiques une fois par seconde, panneau Tâches
       sans retour en arrière, messages pour chaque cas (voir CHANGELOG v0.3.4) ;
     - perso d'un autre joueur : refusé avec « Ce personnage appartient à un autre joueur. » (déjà
       là) ; recrue à personne : travaux de base permis à tous (déjà là, maintenant aussi en permanent).
     - Non traité ici : les lignes « inventory change refused: that character belongs to X » (diff
       d'inventaire du client sur les persos des autres, lot inventaires, voir point 6).
6. **Piles de viande qui « bougent » dans l'inventaire de Geoffrey (affichage), 22:55-22:56+.**
   - Toutes les 3 s, les **trois** clients demandent à l'hôte les mêmes déplacements de
     `42334-changes_otto.mod` (viande) depuis la section `out` (sortie d'une machine : cuisinière,
     boucherie…) : Geoffrey « x2 out -> main 2,2 » et « x1 out -> out 0,0 », rob1 et nass4
     « x2 out -> out 0,0 ». Boucle sans fin, sans réponse écrite chez l'hôte.
   - Piste : le jeu du client fait tourner la machine lui-même (production locale dans `out`) ou
     regroupe les piles de la sortie autrement que l'hôte ; le diff d'inventaire du client
     (`common/src/session.cpp`, « inventory move asked of the host ») prend ce changement pour un geste
     du joueur (un conteneur compte comme « touchable » même fermé) et l'envoie.
   - Risque : si l'hôte rejoue « out -> main », la viande de la machine passe dans l'inventaire du
     perso de Geoffrey sans qu'il l'ait prise. À faire : n'envoyer un déplacement d'un conteneur que
     pendant que le joueur l'a ouvert ; empêcher la production locale des machines chez les clients ;
     log de la réponse de l'hôte à chaque InvOp refusé ou sans effet.
   - 🔧 **Corrigé dans le code (0.3.4), à vérifier en jeu.** Cause trouvée : le Feu de Camp n'est pas
     aux joueurs, l'hôte ne le suit que tant qu'un perso de joueur le travaille (nass4, 22:40:16). Quand
     il l'a oublié, il a effacé l'entité de son inventaire sans prévenir les clients, qui l'ont gardée
     avec ses derniers objets ; leur comparaison avec le vrai contenu du feu donnait un « déplacement »
     toutes les 3 s, au nom d'un netId inconnu de l'hôte, ignoré sans un mot (d'où « sans réponse »).
     Repris à 22:54 sous un nouveau netId : une seconde copie (deux demandes par client ensuite). La
     production locale n'est pas nécessaire pour l'expliquer. **L'hôte n'a pas mis de viande dans le
     perso de Geoffrey par ces demandes** : celles de la boucle n'ont rien fait (netId inconnu, ou « may not put there »,
     22:43:41) ; les sept « out -> main » exécutés (22:54:47-22:55:34) l'ont été pendant que Geoffrey
     avait le feu ouvert (« opens Feu de Camp » 22:54:44, 22:55:12, 22:55:30), un par geste, et
     l'hôte refuse un coffre ou une machine que le joueur n'a pas ouvert. Correctifs : `Unbind` de
     l'hôte quand il oublie une machine ; le client oublie une copie que l'hôte ne nomme plus (12 s)
     et la vieille copie d'un contenant suivi de nouveau ; un coffre ou une machine fermés chez le
     client ne produisent plus aucune demande ; l'hôte écrit chaque demande sans effet. La production
     locale des machines chez les clients n'est pas coupée (elle ne peut plus rien envoyer, et
     l'inventaire revient à celui de l'hôte) : à revoir si on la voit encore en jeu.
7. **rob1 n'arrive pas à parler à un PNJ, il faut une resync (23:10:36)** : même tableau que Geoffrey
   (point 1) : avant la resync, aucun ordre « talk » de rob n'arrive à l'hôte. Deux joueurs touchés le
   même soir : priorité. Ajouter un log client quand un clic « parler » ne produit pas d'ordre, et ce
   que le client sait du PNJ visé (lié à l'hôte ? stand-in ? captif ?).
   - **Fait avec le point 1 (0.3.4)**, même cause et même correction ; à vérifier en jeu.
8. **Conversation lancée par un PNJ refermée aussitôt (23:11:10)** : un « Cent Gardien » fait une
   vérification de contrebande sur rob : « conversation 25 opened for another player's character »,
   réponses envoyées à rob, puis « conversation 25 closed » 0,15 s plus tard : rob ne peut pas répondre.
   Voir ce qui ferme la conversation chez l'hôte (le jeu de l'hôte qui choisit seul ? la fenêtre de
   l'hôte ? un délai ?).
   - **Fait (0.3.4, à vérifier en jeu : `npctalk`, point 4)**. Même chose chez nass4 à 22:24:58
     (conversation 6 fermée 1 s après). Cause (désassemblage de `Dialogue::update` `0x685090`) : la
     minuterie de la réplique dite en bulle (2 s + 0,12 s par caractère) est décomptée tant que le jeu
     n'est pas en pause, et à zéro le jeu ferme la conversation ou la fait avancer seul (chez rob : suite
     dans un autre `Dialogue`, conversation 26 sans nom, « Non. » sans réponse de rob). En solo la
     fenêtre met le jeu en pause ; chez l'hôte, la conversation d'un client non. Corrigé : l'hôte garde
     cette minuterie tant que le joueur a des réponses à choisir (5 min au plus) ; la conversation qui
     continue dans l'autre dialogue garde le nom du PNJ ; une fermeture par le jeu avant toute réponse
     est décrite dans le journal (« closed by the game … before any answer »).
   - Reste : si une conversation se ferme encore tout de suite, la ligne « closed by the game » dira
     l'état de son dialogue (bulle, minuteries, répliques) pour chercher l'autre cause.
9. **nass4 à 69 fps** (23:11:07) quand les autres sont à 150-190 : voir son « sync perf ».
10. **Après un « TP moi » de l'hôte, les persos de nass4 repartent en courant tout seuls (23:11:30).**
    « admin: teleported 2 of 2 character(s) of player 4 (nass4) to the host », puis nass4 voit ses persos
    courir vers un point. Cause probable : la téléportation garde la destination et l'ordre de marche
    d'avant ; le perso y retourne. C'est aussi l'échec des tests `suite` et `tpdown` (« TP admin : le
    perso du joueur arrive près de l'hôte » : 193 à 597 unités). À faire : après une TP admin, vider la
    destination et les ordres de déplacement (comme un « stop »), sans toucher aux travaux permanents.
    **Traité (`fix-char-states`, à tester en jeu : `tpstop`, `suite`, `tpdown`)** : `TeleportCharacters`
    appelle `kenshi::StopCharacter` (halt + `AITaskSystem` `0x5074A0`, ce que fait le bouton stop du
    jeu) sur chaque perso, et l'arrête encore s'il marche au contrôle 0,3 s après.
    Essai du 13/10 : `tpstop` 4/4. `tpdown` 1/2 (193,5) : autre cause, le corps se recouchait là où
    était son nœud de scène (resté où il gisait) ; corrigé (`kenshi::PlaceBodyNode`, voir CHANGELOG
    0.3.4), à relancer. `suite` 30,1 : le membre 0 de l'hôte s'écarte du nouveau venu ; le test mesure
    depuis l'endroit visé.
11. **Meow (recrue de nass4) invisible chez nass4 et Geoffrey (23:18-23:36).** Couchée au sac de
    couchage à 23:18:10 (« put in its bed as on the host », inSomething 1, chez les trois clients), levée
    chez l'hôte à 23:18:53 (« Meow now free »), mais **aucun client ne l'a sortie du lit** (pas de « out of
    its bed » pour `1:238:411621088:1`) : restée « au lit », donc cachée, 17 minutes, pendant qu'elle
    marchait chez l'hôte. Rattrapé au cycle suivant (23:36:10, « out of its bed as on the host (ok) »).
    Entre-temps « player gone: … halted » (23:17:23 : nass4 parti puis revenu ?). À faire : quand l'hôte
    dit « free » et que la copie est encore dans un lit/une cage, la sortir à chaque rafraîchissement
    (pas seulement au changement), et log si elle y reste.
    **Traité (`fix-char-states`, à tester en jeu : `bedfree`)** : `IWorld::EnsureFree` toutes les 2 s sur
    chaque copie que l'hôte ne tient dans rien. Ce que montre le journal : à 23:18:52.86 rob porte un
    corps (l'hôte : « carry someone ») puis le lit de Meow se vide et elle est « free » (23:18:53.03) ;
    le corps est posé à 23:18:56 (« ragdoll 1 »). L'étape d'attache du portage (`0x5CED90`) appelle
    `setInBed(false)` : chez les clients Meow était déjà hors du lit quand le « free » est arrivé (rien
    à quitter, donc pas de ligne) ; à 23:24:07 les trois clients la font tomber comme chez l'hôte, ce
    qui n'arrive pas à un perso au lit (`PostureHeld`). Le lit n'est donc pas ce qui la cachait ; la
    piste retenue est le portage (animation « porté » restée, ou marque « portée » restée) : voir
    point 12. Il faut le journal de nass4 ou de Geoffrey pour trancher.
12. **« Le nom bouge mais pas le corps » (vers 23:44)** : chez un client, le nom au-dessus d'un perso suit
    sa position, le corps reste sur place. Probablement une copie restée couchée (ragdoll ou « au lit »,
    comme Meow au point 11) dont la position avance quand même. Perso et joueur concernés à préciser ; le
    journal de l'hôte ne montre pas d'écart de position (« 0 off ») à ce moment.
    **Traité en partie (`fix-char-states`, à tester en jeu)** : un corps n'est « porté » chez le client
    que tant qu'un perso local le porte (vérifié chaque seconde : une marque restée le laissait à terre
    pour toujours, sans relevage ni position) ; l'animation « porté » restée sur un corps posé est
    coupée après 2 s ; une copie debout chez l'hôte encore à terre après 3 relevages est journalisée
    (« still down here … after N stand-ups », avec ragdoll, inconscient, lit) ; commande `posturecheck`
    et points dans `bedfree` et `carry`. Cause exacte non prouvée sans le journal du client.
13. **nass4 voit des bandits près de lui qui sont en fait très loin chez l'hôte (vers 00:00, resync
    00:08:24).** Cause probable : le lot 3b de la 0.3.3 (`Session::UpdateFarCharacters`) rend au jeu du
    client les copies que l'hôte ne lui envoie plus (« dormant », 25 à 42 chez nass4 dans ses « sync
    perf »). Son jeu les fait vivre avec sa propre IA : des bandits peuvent marcher vers lui alors que ceux
    de l'hôte n'ont pas bougé. Avant, ces copies restaient figées à leur dernière position connue. À
    faire : garder le gain de FPS sans laisser l'IA du client les déplacer (copie figée : pas de
    déplacement ni d'IA, seulement la mise à jour lointaine du jeu sans mouvement), ou retirer la copie
    dès qu'elle s'éloigne de sa dernière position connue ; auto-resync (23:59:25) seulement quand 3 persos
    sont décalés depuis 15 s : ces copies-là n'y comptent pas (pas envoyées).
    **Traité (`fix-char-states`, à tester en jeu : `farfps`, commande `dormant`)** : l'IA du client est
    déjà coupée (`hk_aiUpdate4`, `hk_aiPeriodic`) ; ce qui les faisait marcher, c'est leur dernier état
    envoyé : `Apply` redonnait deux fois par seconde la destination de l'hôte (« moving ») et la session
    réimposait la cible de combat ; la mise à jour lointaine du jeu les y menait. Désormais une copie en
    sommeil reçoit un état immobile (pas de destination, pas de combat) et `IWorld::HoldStill` l'arrête
    si elle marche et la remet à sa dernière position au-delà de 8 unités (ligne « dormant: … put
    back »). La mise à jour lointaine (gain de FPS) reste.
4. **Commerce** (connu, 0.3.2) : la fenêtre se ferme quand la copie du marchand est remplacée par le
   vrai PNJ ; à faire : faire passer la fenêtre sur le vrai marchand (`ShopTrader`+0xC0) au lieu de la
   fermer.
   - 🔧 **Fait dans le code (0.3.4), à vérifier en jeu** (expérience `tradeswap`, juste après le
     chargement d'une ville). Ce que la fenêtre garde du marchand, lu au désassemblage (MOTEUR section
     7, « Ce que la fenêtre garde du marchand ») : `ShopTrader`+0xC0 et le `hand` du marchand dans la
     table des partenaires ; tout le reste est relu à chaque fois ou porte le `hand` du `ShopTrader`
     lui-même. `kenshi::RetargetTradeWindow` réécrit les deux, vérifie avec `getNPCTrader`, sinon la
     fenêtre se ferme comme avant. Argent du marchand (« merchantcats=0 » d'une doublure) : vérifié 1 s
     après l'ouverture et remis quand le marchand change ; « DID NOT STICK » au journal si le jeu ne le
     garde pas (la vraie cause du 0 n'a pas pu être vue sans le jeu : à regarder si la ligne sort).

## Lot à faire : travaux de base (agriculture, cuisine, artisanat…) donnés aux persos de chaque joueur

Demande de l'utilisateur (12/10 00:00) : « faire bien la réplication et les tests pour les attributions
de tâches sur les persos des autres players pour gérer agriculture, cuisine, etc. Tout ce qu'il faut. »
À traiter **après** le crash des sacs (point 0), sur lequel tout transport d'objets repose.

**But** : un joueur (hôte ou client) donne à ses persos n'importe quel travail de base, depuis le clic
droit ou le panneau Tâches, en tâche unique ou permanente (JOBS). Le perso le fait vraiment chez
l'hôte, et chacun voit le même résultat : la machine qui tourne, ce qu'elle produit, ses stocks, les
cultures qui poussent, la récolte, les sacs et coffres qui se remplissent. Ça doit marcher loin de
l'hôte et à x2/x3.

**Postes à couvrir** (une ligne de test chacun, gabarits du jeu de base d'abord, puis un de mod) :
- **Agriculture** : champ extérieur (planter, arroser si besoin, récolter), hydroponie intérieure,
  stockage de récolte. Pousse visible chez le client. Ferme de mod (« Ferme Légumes L ») en plus.
- **Eau** : puits, collecteur, réservoir ; remplir et vider.
- **Cuisine et transformation** : four, cuisinière, moulin, brasserie, distillerie (pain,
  ragoût, farine, grog, rhum). Entrée, sortie, et « récolter la sortie » (tâches 88-94, 113, 151, 247).
- **Mines et matériaux** : minerai de fer, cuivre, pierre ; fonderie et raffinage (plaques de fer,
  matériaux de construction, tissu, cuir…).
- **Artisanat** : établis d'armes, armures, sacs, robotique, médecine, tissage, maroquinerie.
- **Recherche** : établi de recherche, livres, progression partagée (cf. le clic droit qu'il faut
  maintenir sur l'établi).
- **Énergie** : éolienne, générateur, batteries ; l'état allumé ou éteint d'une machine selon le courant.
- **Transport** (tâche 88) : porter vers le bon stockage ; perso « tout faire tourner » ; rangement
  automatique.
- **Construction et entretien** : bâtisseur (125), réparer, démanteler (déjà testés en partie, à
  refaire dans le même lot).
- **Garde, médecin, secours** (58, 148) : déjà couverts, à remettre dans la suite.

**Pour chaque poste, vérifier** :
1. **Qui donne l'ordre** : le joueur pour son propre perso (client et hôte) ; n'importe qui pour une
   recrue « à personne » ; refus pour le perso d'un autre joueur, avec un message.
2. **Comment** : clic droit sur la machine, panneau Tâches (ajout, retrait, ordre), bouton JOBS
   (permanent), sélection de plusieurs persos de plusieurs joueurs (chacun ne commande que les siens).
3. **Chez l'hôte** : la tâche est bien dans la liste du perso, le perso va au poste, « workers » de la
   machine augmente (ligne `machine: … workers`), la production avance.
4. **Chez tous les clients** : même liste de tâches (`JobState`), perso animé au poste, état de la
   machine, contenu des stocks d'entrée et de sortie, cultures, et objets portés dans le
   **sac** sans le bug du point 0.
5. **Robustesse** : propriétaire qui change (raccourci G), joueur qui part et revient, resync, zone
   déchargée chez le client (stand-in), pause et x3, sauvegarde puis rechargement.

**Tests à écrire** :
- unitaires : règles d'ordres (qui peut quoi) pour chaque numéro de tâche de ces postes, ajoutés à
  `TestTaskTargets` ;
- en jeu (`coop_test.py`), nouvelle expérience `workstations` en plein désert, tout le monde en mode
  dieu et matériaux fournis :
  1. l'hôte et un client posent chacun une ferme, un four, un établi, un puits et une éolienne ;
  2. chacun met ses persos au travail par le clic droit et par le panneau Tâches ;
  3. on relève `workers`, les stocks, les cultures et les listes de tâches chez l'hôte et chez
     chaque client, au départ et après 2 min à x3 ;
  4. on vérifie les inventaires des sacs après chaque transport.
- Un soak de 20 min à x3 avec ces postes, 3 clients loin de l'hôte.

**Fait (v0.3.2) — corrigé dans le code, à tester en jeu** (`coop_test.py workstations`) :
- audit des postes et règles d'ordres : `TestWorkstationOrders` (chaque tâche, chaque façon, chaque
  cible) ; travaux de base permis à tous sur une recrue « à personne », refusés sur le perso d'un
  autre joueur ;
- fermier de rob sans ligne `workers` : l'ordre arrivait et tournait ; la ferme poussait encore,
  `dontNeedWorkRightNow` vrai, `tryOperate` refuse tout ouvrier et la tâche ponctuelle s'arrête
  (comportement du jeu de base, même chose sur le banc de recherche sans file). Le joueur reçoit
  maintenant la raison et le conseil du travail permanent ;
- cultures : état de la ferme envoyé (protocole 36), récolte et replantation de l'hôte rejouées chez
  le client ;
- « machine … of the host not found here » chez nass4 : machines envoyées à tous mais loin de ses
  persos (zone pas chargée chez lui). Liaison par le netId de pose en plus du type et de l'endroit, et
  journal qui dit pourquoi ;
- commandes `machines`, `crops`, `storage`, `workjob`.

**Reste** :
- lancer `workstations` en jeu, puis le soak de 20 min à x3 avec 3 clients loin de l'hôte ;
- inventaires des sacs après chaque transport (dépend du lot des sacs, point 0) ;
- fenêtre Prospecter (3p) et boutique d'animaux (3i) chez le client : corrigé dans le code, à tester en jeu ;
- hauteur fausse des poses de client (3o) : **pas un bug** (hauteur relative au sol, voir 3o).
**Ce qui manque sûrement** (constats du 11/10) :
- fermier de rob sur sa ferme : pas de ligne `workers`, donc peut-être jamais compté ;
- bâtiments des clients absents chez nass4 (« machine … of the host not found here ») ;
- fenêtre Prospecter (3p) et boutique d'animaux (3i) qui ne s'ouvrent pas chez le client ;
- hauteur fausse des poses de client (3o).

## 11 octobre 2026, soir : partie entre amis en v0.3.1, soucis pour rejoindre

Journal de l'hôte (session de 22:05, `KenshiCoop.log`) :
- 22:10:16 : rob se reconnecte après 6,2 s de silence, alors qu'il est encore dans l'éditeur de perso.
- 22:10:38 : « > resync » tapé par l'hôte : il recharge nass4 et Geoffrey. Ces déconnexions sont
  **voulues** (ce n'est pas un bug).
- 22:11:03 et 22:11:30 : rob part pendant qu'il rejoint, « connection lost: their game quit or
  crashed while loading ».
- 22:11:32 : nass4 part pendant qu'il rejoint.
- 22:11:11 : Geoffrey se reconnecte après 4,3 s de silence.

On ne voit pas la cause côté client d'ici : leurs journaux sont sur leurs PC.

**À faire :**
1. **Récupérer les journaux de rob et nass4** : `KenshiCoop.log` dans leur dossier Kenshi, et un
   éventuel `.dmp` dans `%LOCALAPPDATA%\CrashDumps`. Chercher pourquoi le jeu quitte ou plante
   pendant le chargement du monde.
2. **Plantage de l'hôte en quittant Kenshi (trouvé, correction écrite mais pas encore compilée ni
   testée).**
   - Constat : 21:57:46, kenshi_x64 pid 65828, module `KenshiCoop.dll` (build de 21:43), offset
     0x27F189, code 0xC0000409. Dump : `%LOCALAPPDATA%\CrashDumps\kenshi_x64.exe.65828.dmp`.
   - Lecture de la pile : `exit` de Kenshi → `ExitProcess` → déchargement de la DLL (destructeurs
     statiques) → `std::terminate` → `abort`. 0x27F189 est `abort`, 0x285470 est `terminate`.
   - Cause : `g_thread` (fenêtre console de l'hôte, `plugin/host_console.cpp`) est un `std::thread`
     global encore « joinable ». Kenshi quitte par `exit()` sans toujours appeler `dllStopPlugin`,
     donc `HostConsoleShutdown` ne le détache pas. Tout hôte qui a la console ouverte (ouverte
     automatiquement avec `hostConsole`) plante en quittant. Ça ne gêne pas les joueurs qui rejoignent,
     mais un client voit « lost connection to host » à ce moment-là.
   - Correction (déjà dans `host_console.cpp`, non commitée) : plus de `std::thread` global. Le thread
     est détaché dès le départ, avec `std::atomic<bool> g_running` pour savoir s'il tourne
     (`ThreadEntry` le remet à false). En bonus, la console peut se rouvrir après avoir été fermée.
   - Reste à faire :
     - compiler et lancer `kc_tests` ;
     - en jeu : héberger, ouvrir la console, quitter Kenshi par le menu, et vérifier qu'il n'y a
       aucun nouveau `.dmp` ni d'erreur 1000 dans le journal Windows ;
     - CHANGELOG, et publier quand l'utilisateur donne son accord.
   - Les autres threads sont sans risque : `Net::keepThread_` est joint par `~Net` → `Close`, et
     `steam_link` garde son `Link` derrière un pointeur jamais détruit.
0. **PRIORITÉ : plantage de l'hôte à 23:04:22 (pid 56496), en pleine partie à 4** — **corrigé dans le
   code, à tester en jeu** (12/10, v0.3.2 en cours).
   - **Cause vérifiée dans le code du jeu** (détail dans MOTEUR.md, « Quel inventaire possède un objet ») :
     ce n'est pas `InventoryItems` (l'inventaire du perso ne montre pas la section du sac : le contenu du
     sac est synchronisé à part, `BagBind`, et les demandes du client nomment le sac). C'est le **dépôt au
     sol depuis le sac** : l'hôte appelait `Inventory::dropItem` (vt 0x38) du sac sur un objet encore dans
     sa section ; le jeu le fait lâcher par le porteur, dont l'inventaire cherche la section
     « backpack_content » par son nom, ne la trouve pas, et se contente d'effacer les marques de l'objet.
     Objet au sol **et** dans la liste et la section du sac, sans nom de section : exactement « move
     done … -> 0,0 », le double « dropped » et « 580 x4 at  0,0 ». Détruit plus tard (ramassé, fusionné) :
     pointeur pendant lu par `getResourceItems` (23:04, IA de transport) ou `recalculateTotalWeight`
     (23:48, section du sac).
   - **Correction** : toute opération de l'hôte passe par l'inventaire propriétaire (parent de la section,
     `InventorySection`+0xC8), vérifie que celui-ci retrouve la section par son nom avant de retirer quoi
     que ce soit, sort l'objet du sac avant de le faire lâcher par le porteur, ne détruit qu'un objet que
     plus aucune liste ne tient. Déplacements, échanges, dépôts, achats/ventes, reconstruction chez les
     clients, plan lu (`workshop.cpp`). Contrôle `invcheck`, contrôle après chaque opération
     (`invcheck each 1`), et contrôle-réparation toutes les 3 s en partie (sauvegardes déjà abîmées).
   - **À tester en jeu** : `python tools/coop_test.py backpack` (sauvegarde `kctest_mine`), puis à la main :
     acheter **directement dans le sac** (glisser du marchand vers le sac, chemin non couvert par le banc),
     lâcher du sac au sol, ramasser, et un perso avec un travail de minage ou de transport 10 min à x3 ;
     chercher `INVCHECK` dans le journal de l'hôte (aucune ligne attendue).
   - « inventory rebuild incomplete: unknown item Étrippeurs_23 » : **pas dans ce code**. Le modèle de l'objet
     de l'hôte a un identifiant qui ressemble à un nom (des données de jeu créées pendant la partie, par
     exemple un objet nommé d'après une faction), inconnu du client : `CreateItemFromState` ne peut pas le
     créer. À traiter à part (envoyer ces données, ou les ignorer sans réessayer).
   - Rapport du mod : `CRASH (unhandled) code=c0000005 at kenshi_x64.exe+0x7494cf … addr=FFFFFFFFFFFFFFFF
     phase='game (outside KenshiCoop's tick)'`. Dump :
     `%LOCALAPPDATA%\CrashDumps\kenshi_x64.exe.56496.dmp`. Le journal complet est dans
     `KenshiCoop.log` de l'hôte, ligne ~12225.
   - Lecture du code : 0x7494C0-0x7494CF est une boucle d'une fonction d'`Inventory` (zone
     0x745000-0x74C000, voisine de `buyItem` 0x74A630). Elle parcourt un tableau d'objets
     (`[r12+0x20]`, nombre en `+0x18`) et appelle `item->vt[0x20]` (son type, comparé à 4). L'objet
     `rbx=0x1dc30e350` a une vtable illisible (`rax=0x6369ad00000000`) : c'est **un objet détruit resté
     dans la liste d'un inventaire** (pointeur pendant).
   - Appelant : `0x5CB3D1`, une fonction de `Character` juste avant `giveItem` (0x5CB400). Pile :
     0x5a5c6a, 0x60f354, 0x61c927, 0x61e8bd, 0x61f00b, soit une mise à jour du jeu, pas notre tick.
   - **Analyse du dump (11/10 23:15).** Le dump ne contient pas le tas, seulement les piles. La chaîne
     d'appels est reconstruite depuis la pile ; les noms viennent des en-têtes KenshiLib, dont les
     adresses valent les nôtres − 0x780 dans cette zone :
     `AI::periodicUpdate` (0x5112B0) → `AI::periodicUpdate(time)` (0x511130) → `AI::chooseGoal`
     (0x510EF0) → `AI::choosePermaJob` (0x50E5B0) → `Tasker::score` (0x60FD80) → `runTargetFind`
     (0x60F460) → … → `Character::findResourceDestinationForMe` (0x5A70A0) →
     `Character::findResourceDestinationFor(forwhat, out, includeMyInventoryContents)` (0x5A59A0) →
     fonction d'`Inventory` 0x749490, qui parcourt `allItems` (+0x10) et plante sur l'objet n°0.
     `hasABackpackOn` (0x5CB320) est aussi sur la pile.
   - Ce qui s'est passé : un perso avec un **travail permanent de ressources** (porter du minerai ou
     des matériaux vers un stockage, comme les « operate machine » sur la Petite Ressource Fer de
     nass4) cherche où déposer sa ressource. Il parcourt un inventaire, sans doute **son sac à dos**,
     dont le **premier objet est détruit** mais toujours dans la liste. Ça s'est passé sur le thread
     de travail du jeu, pendant la mise à jour de l'IA.
   - Suspect n°1 : la fusion de piles de `MoveInventoryItem` dans un sac (`backpack_content`). Elle
     appelle `CallRemoveDestroy(src, best, qty)` puis change la quantité de la pile visée. Vérifier
     que l'objet détruit quitte aussi `allItems` de l'inventaire du **sac** (un `ContainerItem` a son
     propre inventaire) et pas seulement sa section. Les achats de matériaux de rob dans son sac
     (22:28-22:29) et les « pick up Matériaux Construction » de nass4 et Geoffrey vont dans ce sens.
   - **Cause probable trouvée dans notre code** :
     - `InventoryItems(inv)` (`plugin/kenshi.cpp:2229`) réunit `allItems` et les objets de **toutes les
       sections** de `sectionsInSearchOrder`, y compris la section `backpack_content` que l'inventaire
       du perso montre. Mais les objets de cette section appartiennent à l'inventaire du **sac**
       (`ContainerItem`), qui a son propre `allItems`.
     - `MoveInventoryItem` trouve donc un objet du sac (`FindStack`), puis le retire ou le détruit avec
       les fonctions de l'inventaire **du perso** (`CallRemoveDestroy` / `CallRemoveReturns` sur `src`).
       L'objet est détruit, mais le `allItems` du sac garde le pointeur.
     - Plus tard, l'IA du perso (`findResourceDestinationFor` avec `includeMyInventoryContents`, via
       `hasABackpackOn`) parcourt le sac et plante sur ce pointeur. C'est exactement la chaîne du dump.
     - Même risque dans l'autre sens : `PlaceItem` dans `backpack_content` passe par la section
       (`SECV_addAt`) sans que l'inventaire du sac le compte forcément.
   - **Reproduit en direct à 23:29 et 23:31 (rob, Matériaux de Construction 580)** :
     - rob jette la pile au sol. L'hôte : « client item move done: 580-gamedata.base  ->  0,0 », avec une
       section source **vide** ;
     - l'objet de l'hôte `886:3829345792` est « dropped » deux fois (23:29:08 et 23:31:46). Il est
       **à la fois au sol et encore dans la liste d'inventaire** de rob chez l'hôte, sans section ;
     - les clients reçoivent alors un inventaire qui le contient : « inventory rebuild incomplete:
       cannot place 580-gamedata.base at  » (section vide), 60 fois. Le matériau revient dans
       l'inventaire de rob ;
     - c'est le même mélange d'inventaires perso/sac que pour le plantage : `INVV_drop` de
       l'inventaire du perso sur un objet du sac le pose au sol, mais `allItems` du sac le garde.
       Si l'objet au sol disparaît, le pointeur reste : c'est le plantage de 23:04.
   - **Même chose chez Geoffrey (23:37-23:38)** : « inventory of 46:0:0:3322:757324224 not as on the host:
     580-gamedata.base x4 at  0,0 here at backpack_content 0,0 », et « inventory rebuild incomplete …
     (nowhere: lost here) ». Le handle de type 46 est **son sac** (un `ContainerItem`). Chez l'hôte, la
     liste du sac contient 4 matériaux **sans section**. Les déplacements de Geoffrey sur ces
     matériaux sont refusés en boucle (« item not found », ~15 fois en 40 s).
   - **2e plantage de l'hôte, 23:48:30 (pid 45172)** : `CRASH (game crash reporter) code=c0000005 at
     kenshi_x64.exe+0x74932e … addr=FFFFFFFFFFFFFFFF phase='game main loop'`, `rax=0x304952d00000000`
     (vtable illisible). Toujours le code d'`Inventory` : fonctions 0x749160 ← 0x749A6C ← 0x74B660, ce
     coup-ci appelées depuis la boucle principale de `GameWorld` (0x788100), pas depuis l'IA. Même
     objet détruit resté dans une liste. Il est arrivé 17 minutes après les sacs abîmés de rob et
     Geoffrey (23:29-23:38). Rapport du jeu : `crashDump1.0.68_x64.zip` (23:48) ; pas de .dmp Windows.
   - Vu juste avant : « [nass4] machine: Éolienne / Petit Établi Rech. / Ferme Légumes L of the host not
     found here » (23:48:25). Les bâtiments posés par les clients manquent chez nass4. À regarder
     à part.
   - **Extrait du journal de Geoffrey** (`Downloads/KenshiCoop-erreurs-inventaire-2026-10-11.txt` : seulement
     ses 374 lignes d'erreurs d'inventaire, tirées de `KenshiCoop-2026-10-11_23-41-20.log`, sans la fin
     du journal ni rapport de plantage) :
     - le **premier** objet sans section dans un sac apparaît **dès 22:39:04**, 25 minutes avant le
       plantage de 23:04 : « inventory of 46:0:0:1815:2497558528 not as on the host: 580-gamedata.base
       x1 at  0,0 here at backpack_content 0,3 ». Ce sont encore des Matériaux de Construction, dans
       le sac de Geoffrey ;
     - au total, 86 lignes d'objets sans section dans un sac : 580 (32) et **1866-gamedata.base**
       (54) ; 9 « (nowhere: lost here) ». Le bug touche donc aussi un autre objet que les
       matériaux ;
     - pour le reste, ce sont des vêtements de PNJ (chemise 61, chapeaux chris_r.mod 18) et
       « unknown item Étrippeurs_23 » (22:44, nom d'objet au lieu d'un identifiant, à regarder).
   - Pour le plantage de Geoffrey à 23:41, il faut **le journal complet**
     (`KenshiCoop-logs/KenshiCoop-2026-10-11_23-41-20.log`, la fin avec `CRASH` et `stack[…]`).
   - **Correction** : pour toute opération sur un objet, retrouver l'inventaire qui le **possède**
     (celui dont `allItems` le contient : le perso ou le sac) et utiliser ses fonctions à lui pour
     retirer, détruire ou ajouter. Pour `backpack_content`, passer par l'inventaire du `ContainerItem`
     (`hasABackpackOn`, puis son inventaire). Après la correction, garde-fou dans les tests : vérifier
     chaque `allItems` après chaque opération.
   - Reproduction visée : un client pose des piles dans son sac (fusion, achat, ramassage), puis un
     perso avec un travail de minage ou de transport de ressources tourne quelques minutes à x3. Hors
     partie, avec un garde-fou : vérifier après chaque opération que chaque `allItems` ne contient
     que des objets vivants (vtable valide).
   - Suspects : nos manipulations d'objets chez l'hôte, qui détruisent ou déplacent un objet sans que
     toutes les listes le lâchent.
     - Fusion de piles dans `MoveInventoryItem` (`CallRemoveDestroy`, puis écriture de la quantité de
       la pile visée).
     - Commerce avec les comptoirs de boutique (« trade window closed for a moment: the merchant's
       stock changed », très fréquent ce soir).
     - Ramassage et dépose au sol par les clients (« host picked up / dropped item »).
     - Mise en cage et chaînes des esclaves (« Servant Inférieur now chained », 23:03).
   - À faire :
     1. ouvrir le dump : quel inventaire (r12 et son propriétaire), quel objet (rbx, son GameData s'il
        est encore lisible), qui a appelé (remonter la pile) ;
     2. relire chaque chemin qui retire ou détruit un objet chez l'hôte : un objet retiré d'une
        section doit aussi quitter la liste de l'inventaire, et ne jamais être détruit s'il est
        encore référencé ;
     3. soak test commerce, piles et sol, à x2/x3, avec 3 clients.

3. **Silences de 4 à 6 s** (rob, Geoffrey) avant une reconnexion : regarder s'ils viennent d'un long
   chargement chez eux (éditeur de perso, zone qui se charge) ou du réseau. `SkipStall` ne couvre que
   les arrêts de l'hôte.

3b. **Les FPS des clients baissent quand les joueurs s'éloignent les uns des autres** (22:15-22:27, les 3
   joueurs partis exprès loin de l'hôte).
   - Nombre de persos suivis par chaque client / fps, une ligne par minute :
     - Geoffrey : 93 persos / 133 fps → 249 / 119 → **289 / 46**.
     - nass4 : 94 / 186 → 98 / 76-94 → 261 / **10** (pointe à 22:26:06) → **288 / 53**.
     - rob1 : 93 / 164 → 249 / 94 → 307 / 112.
   - Explication probable :
     - l'hôte garde chargées les zones autour de **chaque** joueur ;
     - chaque client reçoit et garde **tous** ces persos, soit 290-307 au lieu d'une centaine ;
     - le flux ne met pourtant à jour que ceux autour du joueur (« sent 5 of 253 ») ;
     - le jeu du client fait donc vivre des persos d'endroits qu'il ne voit pas, en plus de notre
       boucle d'application par perso.
   - Le réseau va bien : 16 à 170 KB/s par joueur, ping 14-23 ms, 1-2 % de pertes. Synchro bonne :
     décalage max 0,2-2,8, sauf un perso à 119 chez nass4 à 22:26:26 qui s'est recalé à la minute
     suivante.
   - nass4 a de base la machine la plus lente (80-100 fps contre 130-190) et des mods (rebirth.mod,
     changes_otto.mod). Son horloge prend du retard 69 fois, contre 16 et 45 pour les autres.
   - **À faire :**
     1. Mesurer chez un client ce qui coûte : notre `Apply` par perso, ou la simulation du jeu.
        Ajouter le temps de notre frame dans la ligne `sync` du journal.
     2. Côté client, ne pas faire vivre les persos loin du joueur local, puisque le flux ne les met
        pas à jour. Pistes : ne pas les créer tant qu'ils ne sont pas proches, ou les geler/cacher.
        Il faut garder la cohérence au retour (recréation propre, cf. « stand-in … can be
        recreated »).
     3. Retester avec 3 clients éloignés et comparer les fps.
   - **Corrigé dans le code, à tester en jeu** (branche du lot 3b/3f, v0.3.2 en cours) :
     - mesure : ligne `sync perf` par minute chez chaque client (recopiée chez l'hôte sous `[nom]`), commandes `perf` et `standins` ;
     - cause trouvée dans notre code : chez le client, `hk_charUpdateView` mettait « dans la vue » **tous** les persos que pilote l'hôte, y compris les ~200 en sommeil près des autres joueurs (pilotés 2 fois/s mais présents dans `replicated`) : déplacement complet et animation à chaque image pour des persos que personne ne voit ici. Ils sont maintenant laissés à la mise à jour lointaine du jeu (`IWorld::SetFarCharacters`, `HookView::farAway`) ; jamais un membre d'escouade ; un perso que l'hôte renvoie repasse en complet dans la même image. Pas de destruction ni de recréation de doublures (risque de plantage) ;
     - reste : le jeu du client charge lui-même les zones autour des persos de tous les joueurs (ils sont de sa faction) et y fait vivre leurs PNJ : non touché (risque de perdre des membres d'escouade) ;
     - test : `python tools/coop_test.py farfps --clients 3` (ensemble, puis loin avec `farleave 0` puis `farleave 1`).
   - Vu en même temps, à vérifier :
     - « captivity from the host: a merchant caged » environ 2 fois par seconde chez les 3 clients
       (22:26:08-14) : un état réappliqué en boucle ? **Corrigé dans le code, à tester en jeu** (0.3.2) :
       chaque changement n'est écrit qu'une fois (les rafraîchissements de 5 s le renvoyaient quand
       l'entrée du client avait été oubliée), avec le meuble (un lit compte comme « caged » : une ville
       qui se couche, c'est des dizaines de changements), 6 lignes par 10 s au plus. Expérience
       `quietlogs`.
   - Vu en même temps, à vérifier :
     - « captivity from the host: a merchant caged » environ 2 fois par seconde chez les 3 clients
       (22:26:08-14) : un état réappliqué en boucle ?
     - 73 refus « conversation refused: that NPC is in conversation with another player's
       character » par rafales (22:18-22:22, jusqu'à 36 en une minute) : une demande répétée à chaque
       image ?
     - Inventaire de nass4 : « inventory rebuild incomplete: cannot place … at shirt » 6 fois
       (objet de mod ?).

3c. **rob n'arrive pas à acheter chez le marchand (22:28-22:29)** : refus « no room » / « trade refused: the
   item could not move » quand il pose des Matériaux de Construction (580-gamedata.base) **dans son
   sac** (`backpack_content`). Vers l'inventaire principal (`main`), ça passe.
   - **Corrigé dans le code, à tester en jeu** (v0.3.2) : fusion selon `Item::isStackable` (maximum d'une
     case, bonus du sac compris), une unité sur une unité comprise, aussi pour les achats ; ce qui ne
     s'empile pas échange sa place. `kc::StackMergeCount`, test `TestStackMerge`, expérience `backpack`.
   - **Testé en jeu le 12/10 au matin avec un Traders Backpack Medium** (bonus min 2, ×3 : Matériaux en piles
     de 6) : bonus identique chez l'hôte et le client (`bagbonus`), 1 sur 1 et pile entière fusionnés, objet
     posé sans case qui rejoint une pile pas pleine (avant : « no room »). Expérience `backpack` 40/40.
     Reste à confirmer avec rob en vrai (achat chez le marchand directement dans le sac).
   - Quelques achats dans le sac passent (22:29:10-17), puis tout est refusé, même en vendant et en
     rachetant. Le sac de rob se remplit donc chez l'hôte alors qu'il a l'air vide chez lui.
   - Cause probable : `MoveInventoryItem` (`plugin/kenshi.cpp:2531`) ne fusionne avec la pile visée que
     si `have > 1 || qty > 1`. Rob pose 1 unité sur 1 unité : son jeu en fait une pile de 2 dans la
     même case, mais l'hôte ne fusionne pas. `PlaceItem` met alors l'unité dans une autre case libre
     du sac. Chaque achat prend une nouvelle case chez l'hôte jusqu'à ce que le sac soit plein, et là
     tout est refusé.
   - Correction : fusionner quand l'objet s'empile (taille de pile max > 1, lue dans les données de
     l'objet ou via la fonction du jeu), et garder l'échange de place pour ce qui ne s'empile pas
     (deux épées). Le chemin d'achat et de vente doit suivre la même règle.
   - Test : `coop_test.py`, un client achète 10 matériaux un par un dans son sac, puis on compare
     l'inventaire du client et celui de l'hôte.
   - En attendant : rob peut acheter dans son inventaire principal, puis ranger les piles entières.
   - Confirmé en jeu (22:30) : dans son sac de marchand, les objets ne s'empilent pas ; le reste du
     commerce marche (livres achetés et vendus sans souci).

3d. **Conversations : refus en rafale, et rob ne peut pas parler au « Barman sacré » (22:30-22:35)**
   - **Refus en rafale : corrigé dans le code, à tester en jeu** (0.3.2, `coop_test.py recruits`, étape 4) :
     l'hôte connaît le propriétaire de chaque perso d'escouade (`IWorld::SetSquadOwners`, publié dans
     `HookView::squadOwner`) ; `ConversationAllowed` refuse **sans message ni compte** un autre perso
     du même joueur (`same=` dans `dialogs`), et les lignes de refus sont limitées (`LogLimiter`). Le
     client n'envoie plus qu'un ordre « parler » par PNJ et par lot d'ordres : avec deux persos
     sélectionnés, un seul part (« talk order of a second selected character … not sent »). Le Barman
     sacré de rob reste à regarder (journal de rob).
   - `dialogs` sur l'hôte à 22:33 : 1784 refus « occupé » (`dialogsBusyRefused`) depuis le début de la
     session.
   - Cas vu en direct (22:34:16, nass4 et Streak) :
     - l'ordre « parler » part avec **deux persos de nass4** sélectionnés (nass4 et Logan) ;
     - la conversation 17 s'ouvre pour nass4 ;
     - le jeu de l'hôte refait alors en boucle la tentative pour Logan, et `ConversationAllowed`
       (`plugin/world.cpp:2254`) la refuse comme si Logan était le perso d'un autre joueur ;
     - résultat : des dizaines de refus par seconde, et nass4 reçoit « Streak est occupé » en
       pleine conversation.
   - Correction :
     - dans `ConversationAllowed`, quand celui qui demande appartient au même joueur que `r.pc`,
       refuser **sans** message « occupé » et sans le compter : le second perso n'a pas à parler, comme
       dans le jeu de base ;
     - ou bien n'envoyer l'ordre « parler » que pour un seul perso de la sélection.
   - rob et le Barman sacré : aucune ligne côté hôte quand il essaie (pas d'ordre « talk », pas de
     refus). Son jeu n'envoie donc rien, ou refuse avant d'envoyer.
     - Pistes : le barman est-il chez rob un « stand-in » d'un perso de l'hôte, est-il assis ou au lit
       (les états « caged »/« in its bed » qu'on réapplique), ou une conversation restée ouverte
       dans `remoteDialogs_` ?
     - Il faut le journal de rob, ou le refaire en test avec `talkto name:Barman` chez un client dans
       la ville sainte.
     - rob a suivi le barman (ordre « stay close », tâche 31) pour qu'on l'identifie, mais le journal
       ne donne pas le nom de la cible de l'ordre. À ajouter au journal des ordres client : le nom de
       la cible.

3e. **Recrues des dialogues absentes chez les clients : Streak et Stone (22:34-22:39)**
   - **Corrigé dans le code, à tester en jeu** (0.3.2, `coop_test.py recruits`). Cause lue dans le
     désassemblage de `createRandomCharacter` (0x5836E0) : le jeu lit le booléen `unique` du modèle ;
     si le registre des uniques du monde l'a déjà (c'est le cas chez un client, qui a chargé la
     sauvegarde de l'hôte), il prend la liste « unique replacement spawn » du modèle, et rend null si
     elle est vide. Les recrues de Dialogue.mod sont des modèles uniques. L'embauche par dialogue ne
     crée rien : elle recrute le PNJ de la conversation (`PlayerInterface::recruit`) ; c'est sa copie
     chez le client qui ne pouvait pas être faite. `kenshi::CreateCharacter` met `unique` à faux le
     temps de l'appel (même valeur que lit la fabrique), puis le remet. En plus, l'hôte envoie à tous
     l'apparence (race comprise) de chaque nouveau perso d'escouade (`Session::BroadcastLooks`),
     appliquée dès que la copie existe.
   - nass4 recrute Streak (`41370-Dialogue.mod`, « hire engineer wall man », payé 5000 cats), puis Stone
     (`41299-Dialogue.mod`, « Sword for hire ex-slave »).
   - Chez l'hôte : « a new squad character (Streak) goes to nass4 » et « 1 character(s) now controlled
     by nass4 ».
   - Chez les 3 clients : « cannot recreate host character 41370-Dialogue.mod: factory did not return
     a character (template type 1 …) », réessayé à 30, 60, 120 puis 240 s. Ensuite « 2 squad members
     missing » à chaque relevé. nass4 ne voit pas ses deux recrues.
   - `CreateCharacter` (`plugin/kenshi.cpp:1838`) appelle `createRandomCharacter(factory, faction, pos,
     template, age)`. Ce gabarit de recrue, un perso de type 1 venant de Dialogue.mod, revient nul.
     Le jeu crée sans doute ces recrues autrement (l'action de dialogue d'embauche, avec le gabarit
     et la race choisis à ce moment-là).
   - À faire :
     - reproduire hors partie : `spawnnpc 41370-Dialogue.mod` sur un client ;
     - voir comment le dialogue d'embauche crée le perso (KenshiLib : `DialogActionEnum`, `HIRE`, la
       fonction de fabrique qu'il appelle) ;
     - au besoin, créer chez le client avec la race et l'apparence que l'hôte envoie (comme pour les
       persos de joueur), plutôt que le gabarit seul.
   - Conséquence signalée à 22:52 : « rob ne voit plus certains portraits, pareil pour nass4 ». Ce sont
     sans doute ceux de Streak et Stone : un perso que le client n'a pas n'a pas de portrait dans
     la barre d'escouade. À confirmer avec les noms. Si d'autres portraits manquent, regarder
     `hk_portraitUpdate` et les « removed the local character … the host does not have it ».
   - Contournement pendant la partie : `resync` depuis la console de l'hôte. Les clients rechargent le
     monde de l'hôte, qui contient les recrues (courte déconnexion).

3f. **Combat de nass4 loin de l'hôte : sa squad et les ennemis traversent les murs (22:45)**
   - Synchro bonne pendant ce combat : décalage max 1,7, aucun perso décalé. nass4 voit donc ce que
     simule l'hôte, et c'est la simulation de l'hôte qui fait passer les persos à travers les murs.
   - Hypothèse : loin de la caméra de l'hôte, le jeu ne charge pas ou ne génère pas la collision et le
     navmesh des bâtiments. Les persos de cette zone se déplacent alors en ligne droite (poursuite,
     recul en combat). Même famille que les « portes de distance caméra » déjà levées pour le dialogue
     (`SetFarTalk`) et les dégâts (`SetFarWoundVisuals`).
   - Vérifications :
     1. l'hôte place sa caméra sur le combat de nass4 : les persos contournent-ils les murs ?
     2. si oui, trouver la condition « distance caméra » qui active la collision des bâtiments ou le
        navmesh autour d'un joueur (`NavMesh` 0x21345C0, `generate(Aabb)`), et l'étendre aux
        positions des clients comme pour `hk_charUpdateView`.
   - Test : `coop_test.py`, un client dans une ville loin de l'hôte, combat contre un ennemi de l'autre
     côté d'un mur, et relever les positions.
   - **Corrigé en partie dans le code, à tester en jeu** (même branche que 3b) :
     - lu au désassemblage (docs/MOTEUR.md, « Déplacement loin de la caméra ») : **ni le navmesh ni la physique des bâtiments ne dépendent de la caméra** ; ils dépendent du chargement de la zone, que les persos des joueurs activent aussi. L'hypothèse de départ ne tient donc pas ;
     - ce qui dépend de la distance : la mise à jour complète du déplacement (`0x660A3D`) exige le perso « dans la vue » **et** un pas de moins de 0,5 s de temps de jeu (`0x660A47`). Au-delà, `moveAlongPath` sans les murs. Le premier est déjà levé (`hk_charUpdateView`) ; le second arrive quand le jeu de l'hôte gèle (zones lointaines qui se chargent avec 3 joueurs éparpillés) à x2/x3. `hk_moveUpdate` coupe maintenant ces pas en morceaux de 0,4 s pour les persos près d'un joueur client ;
     - mesure : `movestats` et la ligne `host perf` (déplacements complets, hors vue, pas coupés, plus long pas) ; `navcheck` (sur le navmesh ou non) ;
     - test : `python tools/coop_test.py farwalls` (combat de part et d'autre d'un bâtiment, caméra de l'hôte à la maison puis sur le combat). Si les persos traversent encore avec `out of view`/`left whole` à 0 et `nav=1`, la cause est ailleurs (navmesh de la ville sans ce mur : `navat` sur le mur).

3g. **rob suit Geoffrey avec la caméra mais ne voit pas les PNJ qui le frappent (22:47)**
   - Cause : `Session::UpdateStreams` (`common/src/session_streaming.cpp:44`) n'envoie à un joueur que les
     persos autour de **ses propres persos**, plus les membres d'escouade proches d'eux. La caméra du
     client n'est pas prise en compte.
   - Geoffrey, membre d'escouade, est envoyé à tout le monde. Sa cible de combat aussi : la passe
     « what a streamed character fights goes with it » va du combattant vers sa cible. Mais les PNJ qui
     **attaquent** Geoffrey ne suivent pas : rien ne va de la cible vers ses attaquants.
   - Corrections :
     1. sans changer le protocole : dans cette passe, ajouter le sens inverse. Un perso dont la
        `combatTarget` est envoyée à un joueur lui est envoyé aussi, au moins pour les cibles membres
        d'escouade ;
     2. plus complet : le client envoie la position de sa caméra (spectateur, carte), et l'hôte s'en
        sert comme centre d'intérêt en plus de ses persos. Ça change le protocole, donc il faut l'accord
        de l'utilisateur pour le changement de version.
   - Test : un client place sa caméra sur le combat d'un autre joueur loin de lui, et on compte les PNJ
     visibles chez lui et chez l'hôte.
   - **Option 1 corrigée dans le code, à tester en jeu** (0.3.2, sans changer le protocole) : dans
     `UpdateStreams`, qui combat un perso envoyé à un joueur lui est envoyé aussi ; le client recrée, même
     loin de ses persos, un PNJ qui combat un membre d'escouade (`Session::FightsSquad`). Test unitaire
     dans `TestPerPlayerStreaming`, expérience `attackers`. L'option 2 (caméra du client) reste à faire.

3h. **Journal de Geoffrey (22:12-22:51)**
   - **FPS** : 120-190 au début, **63-94 depuis 22:44** (46 au plus bas à 22:27, au moment où les joueurs
     s'éloignaient). Même cause probable qu'en 3b.
   - **Porte en boucle** : « door: Storm Shack Door stays 0 (host 3) » toutes les ~3 s depuis 22:26 :
     163 fois chez Geoffrey, 228 chez rob1, 113 chez nass4. Le client n'arrive pas à mettre cette porte
     dans l'état 3 de l'hôte et réessaie sans fin. Il faut trouver ce qu'est l'état 3 (cassée ?
     verrouillée et ouverte ?) et, si le client ne peut pas l'appliquer, arrêter de réessayer
     (abandon après N essais ou nouvel essai seulement quand l'hôte change).
     **Corrigé dans le code, à tester en jeu** (0.3.2) : 3 = « en fermeture ». La porte de l'hôte reste en
     fermeture (loin de sa caméra, ou bloquée) ; fermée chez le client, c'est juste
     (`DoorState::sameSettled`) : plus réappliquée. Une porte que le jeu du client refuse est laissée
     après 3 essais jusqu'au prochain changement de l'hôte (`doorsknown` : `givenup=`). Expérience
     `quietlogs`.
   - **Cadavres décalés** : à la fouille d'araignées, le corps de l'hôte est à 93-128 unités de celui du
     client (sur 23 fouilles : médiane 10, max 235). La correction « stood up to fall where the host's
     lies » existe, mais les corps tombés en combat restent souvent loin. Le pilleur va déjà au corps
     de l'hôte, donc ça marche, mais c'est visible.
   - **Vêtements des stand-ins** : « inventory rebuild incomplete: cannot place …-gamedata.base at
     shirt » 27 fois. Une chemise portée chez l'hôte ne va pas sur le slot chemise chez le client (slot
     occupé ou désactivé ?). Elle finit dans le sac.
   - **Recrues de dialogue** manquantes chez lui aussi (cf. 3e) : 12 échecs « cannot recreate host
     character …-Dialogue.mod ».
   - Horloge en retard 100 fois et en avance 13 fois (recalée à chaque fois).

3i. **Achat d'animaux : la fenêtre s'ouvre chez l'hôte, pas chez le client (22:50, rob et le Chef Ferme
   Sacré)** — **corrigé dans le code, à tester en jeu** (protocole 36 : `AnimalShop` / `AnimalBuy` ;
   `plugin/shops.cpp`, `common/src/session_animals.cpp` ; test `coop_test.py animalshop`, voir
   docs/TESTS.md et docs/MOTEUR.md « Boutique d'animaux et prospection »). Les trois autres fenêtres
   de la même classe (vente et achat d'esclaves, caution) s'ouvrent encore chez l'hôte.
   Sacré)**
   - Dans le journal : rob parle au Chef Ferme Sacré (« Qu'est-ce que tu veux ? » 1. Je veux juste
     commercer | 2. T'as des animaux à vendre ? | 3. Rien). Il répond 2, la conversation se ferme, et
     **rien n'est envoyé à rob**. La fenêtre d'achat d'animaux s'ouvre sur l'écran de l'hôte. À
     22:51:32, « the animal Taureau Domestiqué joins its owner's squad ».
   - Cause : l'action de dialogue « animaux à vendre » ouvre une autre fenêtre que le commerce
     d'objets. `hk_showTrade` (`plugin/hooks.cpp:735`, `ForgottenGUI::showTradeWindow`) n'intercepte
     que le commerce d'objets (type 1 ou 3) pour l'envoyer au joueur. La fenêtre des animaux passe à
     côté.
   - À faire (souhait de l'utilisateur : le client gère lui-même sa fenêtre) :
     1. trouver la fonction du jeu qui ouvre la boutique d'animaux (action de dialogue d'achat
        d'animaux dans KenshiLib, fonction `ForgottenGUI` voisine de `showTradeWindow`) et la
        hooker comme `hk_showTrade` : chez l'hôte, pour le perso d'un autre joueur, ne pas l'ouvrir et
        envoyer une demande à ce joueur ;
     2. chez le client : ouvrir la même fenêtre avec la liste d'animaux de l'hôte (race, prix) ;
     3. l'achat (et la vente) passe par l'hôte comme le commerce d'objets : argent retiré, animal
        créé chez l'hôte et ajouté à l'escouade du joueur, puis renvoyé par la synchro habituelle
        (« joins its owner's squad » marche déjà).
     - Probablement un nouveau message réseau, donc un changement de protocole : accord de
       l'utilisateur.
   - Test : un client parle à un vendeur d'animaux, répond « animaux à vendre », achète un animal, et
     on vérifie la fenêtre chez le client, l'argent et l'animal dans son escouade chez tous.

3j. **Portraits absents chez rob et nass4 : Nei, Skinner, Kihaku (signalé à 22:55)**
   - **Corrigé dans le code, à tester en jeu** (0.3.2, `coop_test.py recruits`, étapes 3 et 5) : à chaque
     application des escouades (`ClientSquads`), un membre d'une escouade de l'hôte dont la copie est
     encore un PNJ ici (`IWorld::InPlayerFactionHere` faux) est remis en adoption (`adopt`), quel que
     soit l'ordre ou la vitesse des `Bind` (G en boucle) ; un échec réessaie après une minute au lieu
     d'abandonner pour toujours. `squadcheck` donne `outside=` (doit être 0).
   - Ce sont des recrues de nass4, des PNJ de bar recrutés par dialogue : Kihaku à 22:32:19, Skinner à
     22:33:23, Nei à 22:33:42. Contrairement à Streak et Stone, ces persos existent chez les clients
     (primes, vue d'étage « follows our 'Nei' » chez nass4).
   - Chez l'hôte, `squadsfull` à 22:55 donne : Sans-Nom_0 = Scratch, Geoffrey | Rob = rob, Taureau
     Domestiqué | **fasme = Nei, Skinner, Kihaku, Logan, nass4, Stone, Streak**.
   - Ce qui s'est passé :
     - l'hôte a fait tourner le contrôle de chaque recrue avec le raccourci « confier » (G) : vous →
       Geoffrey → rob1 → nass4 en ~2 s, 6 fois entre 22:32 et 22:37 ;
     - nass4 a ensuite déplacé ses persos dans l'escouade « fasme » (squad request « move to squad »
       à 22:32:41, 22:33:57, 22:34:03) ;
     - barre d'escouade de nass4 à ce moment : « player portraits 5: 3-4 hidden ».
   - Pistes :
     1. chez le client, le PNJ recruté reste un stand-in de PNJ et n'est pas mis dans l'escouade
        locale « fasme ». Il n'a donc pas de cellule de portrait. Vérifier l'application de la
        composition d'escouade (squadsfull chez le client) après un recrutement ;
     2. le passage rapide de main en main laisse un état incohérent chez un client : ajouté, retiré,
        puis rajouté, alors que le jeu ne recrée pas sa cellule de portrait.
   - Test : un client recrute un PNJ par dialogue, l'hôte le confie avec G jusqu'au client, puis le
     client le range dans une autre escouade. Comparer `squadsfull` chez l'hôte et chez le client, et
     compter les portraits.
   - Contournement pendant la partie : `resync` (les clients rechargent le monde de l'hôte avec ses
     escouades).

3k. **`resync` de 22:56:38**
   - **Perso parasite : corrigé dans le code, à tester en jeu** (0.3.2). L'hôte retient le perso fait pour
     l'éditeur de chaque joueur (`editorChars_`) tant que l'éditeur est dû. Jamais pris pour une recrue
     (`JoinSquad` : à son joueur s'il est là, sinon à l'hôte). Supprimé (`IWorld::RemoveStrayCharacter`)
     quand son joueur part sans l'avoir fini, ou quand un nouveau perso est fait pour lui, **si le jeu le
     garde hors des escouades du joueur** ; un membre d'escouade n'est jamais supprimé par le mod (son
     portrait) : il reste à l'hôte. Le renvoi d'un perso hors faction par le jeu (bouton « renvoyer »,
     `SquadManagementScreen::dismissCharacter` 0x48FA30) n'est pas utilisé. À vérifier en jeu : un
     client quitte pendant l'éditeur, revient, puis `resync`.
   - Les 3 joueurs reviennent : rob1 en 26 s, Geoffrey en 31 s, nass4 en 39 s. Monde envoyé : 1,7 MB
     chacun, 12 persos de joueur.
   - Geoffrey : « 1 squad members missing » juste après son retour. nass4 : 45 PNJ « not there yet »
     et ~10 persos d'une même escouade (1:344:…) qui « ne restent pas couchés après 2 chutes ». rob1 :
     tout bon à 22:58, 371 fps.
   - **Perso parasite « rob1 »** : « NPC rob1 joined the player faction: a squad character now » à
     22:56:46, pendant le nettoyage des départs. `squadsfull` le montre ensuite dans l'escouade
     **fasme** de nass4. C'est sans doute le perso que rob avait commencé dans l'éditeur à 22:10-22:13
     (« never finished making their character … the editor opens again »), resté dans le monde.
     Il faut supprimer un perso d'éditeur abandonné au lieu de le laisser vivre, et ne pas l'adopter
     comme recrue (`Session::JoinSquad`, `common/src/session.cpp:1922`).
   - L'utilisateur trouve que « ça a l'air de bugger fort » après le resync. Ce qu'ils voient est à
     préciser.

3l. **Caméra de Geoffrey « complètement folle, elle va partout » (23:20, nouvelle partie)**
   - Son journal répète toutes les 0,5 s « floor view: follows our 'Geoffrey' (it followed nobody) ».
     Son jeu remet sans cesse le focus de `PlayerInterface` (+0x270 `PI_focusHand`) à « personne », et
     `UpdateFloorFocus` (`plugin/world.cpp:1497`) le remet sur Geoffrey à chaque fois, avec
     `PI_focusFloor = -1`. Déjà 149 fois dans la partie précédente.
   - Hypothèse : ce focus est aussi celui que la **caméra suit**. Le jeu le lâche quand le joueur
     déplace sa caméra lui-même ; le mod le reprend, la caméra est ramenée sur Geoffrey, et ainsi de
     suite. D'où une caméra qui saute entre ce que le joueur fait et son perso.
   - Correction :
     - ne poser ce focus qu'une fois, au chargement (le focus du monde de l'hôte ne vaut rien chez le
       client), puis le laisser au joueur, même s'il revient à « personne » ;
     - les toits et étages au-dessus de nos persos passent déjà par `UpdateRoof` : l'appeler pour le
       perso choisi sans réécrire le focus ;
     - vérifier dans le jeu de base (0x7F5400, 0x801630) si `PI+0x270` pilote la caméra.
   - Test : client, déplacer la caméra avec les touches et la souris loin de son perso, monter un
     étage, entrer dans un bâtiment. La caméra ne doit jamais revenir seule, et les toits doivent
     se cacher.
   - **Corrigé dans le code, à tester en jeu** (0.3.2). Vérifié dans le jeu : +0x270 est le perso
     « suivi » de la caméra (`startTrackCharacter` 0x7F5400, `stopTrackCharacter` 0x7F2780) ;
     `CameraClass::move` (0x6AE5D1) et `teleport` (0x6AF934) appellent `stopTrackCharacter`. Le focus
     n'est plus posé qu'une fois par monde chargé (ou s'il revient sur le perso de la sauvegarde, 3 fois
     au plus) ; `UpdateRoof` cache les toits au-dessus du perso sélectionné sans toucher au focus.
     Expérience `camfocus` (commande `camerafree`).

3m. **nass4 ne peut plus diriger Stone (23:27, nouvelle partie)**
   - **Corrigé dans le code, à tester en jeu** (0.3.2, `coop_test.py recruits`, étape 5). Le suivi
     « changed squads here » (`KenshiWorld::FindMoved`) cherche un perso de même type et même numéro
     de série : Stone et l'ancien handle de l'hôte avaient le même. Il refuse maintenant tout perso dont
     le handle est celui d'un perso que la session suit (`IWorld::SetHostKnows`) : c'est la copie de
     celui-là. Un lien déjà fait vers un membre d'escouade qui est lui-même un perso de l'hôte est
     défait (`Exists`). Les refus « order for a character the host does not know » et « refused
     locally … does not own » sont limités à 3 lignes par 10 s.

3m. **nass4 ne peut plus diriger Stone (23:27, nouvelle partie)**
   - 23:27:26-27, chez les 3 clients : « our copy of the host's 1:317:56274416:1:1129150336 changed squads
     here (now 1:46:853759744:6:1129150336): followed ». `1:46:853759744:6` est le handle de Stone, le
     6e de l'escouade fasme (c'était le focus du monde chargé à 23:08, et l'hôte téléporte ce handle à
     23:30:45).
   - Le client a donc pris **Stone** pour la copie d'un **autre** perso de l'hôte (1:317…:1, peut-être
     un PNJ du même gabarit 41299 « Sword for hire ex-slave », ou le Vert-Landais à terre que nass4
     fouillait à 23:27:12). Le vrai Stone de l'hôte n'a plus de copie chez le client.
   - Ensuite : « refused locally: order for a character this player does not own: not a squad member »
     dès 23:27:35, puis « order for a character the host does not know » à partir de 23:27:42, jusqu'à
     ~900 fois par minute. Rob et Geoffrey reçoivent aussi « inventory change refused: that character
     belongs to nass4 ».
   - À faire :
     - trouver qui associe une copie locale à un perso de l'hôte (appariement des stand-ins par
       gabarit et position, ou suivi « changed squads here … followed ») ;
     - interdire d'associer un **membre d'escouade** local à un autre perso de l'hôte que le sien ;
     - si deux persos de l'hôte se disputent une même copie, garder celui qui a le même handle ;
     - ne pas journaliser le refus à chaque image (900 lignes par minute).
   - Contournement : `resync`.

3n. **Le mode construction de l'hôte met tout le monde en pause (23:35, demande de l'utilisateur :
   « c'est chiant »)**
   - Le jeu de base met en pause quand on entre en mode construction. L'hôte simule le monde pour tout
     le monde, donc sa pause arrête tout le monde.
   - Souhait : l'hôte construit sans arrêter les autres.
   - À faire :
     - chez l'hôte en partie, trouver où le mode construction met le jeu en pause (entrée dans
       `PlayerInterface` / `BuildMode`, appel à la vitesse 0, cf. le setter UI 0x787E40) ;
     - ne pas appliquer cette pause-là pendant une session, ou la lever tout de suite, sauf si l'hôte
       avait déjà mis pause lui-même ;
     - chez un client, son mode construction ne doit rien changer pour les autres (à vérifier).
   - Test : l'hôte entre en mode construction pendant qu'un client marche et se bat. Le client ne doit
     pas s'arrêter, et la pose du bâtiment doit marcher.
   - Contournement à essayer : relancer le temps (Espace / bouton vitesse) une fois en mode
     construction.
   - **Corrigé dans le code, à tester en jeu** (0.3.2) : `PlayerBuildMode::activate` (0x4E65E0) appelle
     `GameWorld::togglePause(true)`. Détour `hk_buildActivate` : chez l'hôte avec d'autres joueurs,
     `togglePause(false)` juste après ; `togglePause` garde la pause si la vitesse est 0, donc si l'hôte
     avait mis pause lui-même. Le mode construction d'un client ne met en pause que son jeu, qui reprend
     l'horloge de l'hôte à l'image suivante (`EndFrame`). Expérience `buildpause`.

3o. **Agriculture : « pour planter c'est compliqué » (rob, 23:30-23:44)**
   - Ce qui marche :
     - rob pose « Ferme Légumes L » (`18968-small_changes_otto.mod`) à 23:30:44 ; elle est construite
       chez tout le monde, finie à 23:36:16, puis « machine: Ferme Légumes L switched on » ;
     - rob ordonne trois fois « operate machine » sur la ferme (23:36:21, 23:36:58, 23:37:06), et
       ouvre deux fois son stockage (23:37:10, 23:37:27).
   - Ce qui manque : aucune ligne « machine: Ferme Légumes L workers 0 -> 1/… » côté hôte, alors que la
     mine de fer en a. Le fermier n'a peut-être jamais été compté au travail chez l'hôte, ou
     l'ordre est rejoué chez l'hôte sans prendre.
   - Le symptôme exact reste à préciser (pas de plantation, pas de pousse, récolte impossible,
     différence entre hôte et client).
   - À tester : un client pose un champ (gabarit de base `gamedata.base`, pas seulement de mod), le fait
     construire, met un perso « fermier » dessus, à x3. Vérifier l'ordre, les « workers », la pousse et
     la récolte, chez l'hôte et chez le client.
   - Remarque de hauteur : les poses de client arrivent avec une hauteur fausse (rob -13,6 → hôte
     268,1 ; Geoffrey -19,4 → 931,1). L'hôte recale sur le sol, mais il faudra voir pourquoi le client
     envoie une hauteur pareille.
     **Expliqué, pas un bug** (0.3.2) : la hauteur d'une pose est relative au sol (`createBuildings`
     retire `getTerrainWithWaterHeight(x, z)` en 0x4D7512 / 0x4D751C, et `createBuilding` l'ajoute) ;
     -13,6 = 13,6 sous le sol, 268,1 dans le monde. Le journal du client le dit maintenant
     (« … (-13.6 from the ground) »).

3p. **La fenêtre « Prospecter » ne s'ouvre pas chez le client (23:47)** — **corrigé dans le code, à
   tester en jeu**. Cause (piste 3, en partie) : le bouton donne la tâche 150 par
   `Character::addOrder` sans passer par nos crochets d'ordres ; chez le client la tâche ne tourne
   jamais (l'hôte pilote nos persos), donc rien ne s'ouvre ni ne part vers l'hôte. Le client ouvre
   maintenant la fenêtre tout de suite (`hk_prospectButton`, `plugin/shops.cpp`) ; test
   `coop_test.py prospect`.

3p. **La fenêtre « Prospecter » ne s'ouvre pas chez le client (23:47)**
   - Le bouton PROSPECT de la barre d'ordres (`OrdersPanel`, `0x72B540`) ouvre normalement la fenêtre de
     prospection (fertilité, eau, minerais à l'endroit du perso sélectionné). Chez le client, rien ne
     s'ouvre, et le journal ne montre rien : ni ordre, ni tâche 150, ni refus.
   - Pistes :
     1. la fenêtre lit des données du lieu (`checkProspectingIsNotZero`, docs/MOTEUR.md §…) que le jeu
        du client n'a pas, ou elle demande un perso « vraiment » à lui (faction, `PlayerInterface`) ;
     2. le mod intercepte le clic sur ce bouton (relecture des boutons de la barre d'ordres côté
        client) et l'avale sans rien envoyer ni ouvrir ;
     3. la tâche 150 part vers l'hôte et la fenêtre s'ouvre chez l'hôte, comme la boutique d'animaux
        (3i). À vérifier : l'hôte voit-il une fenêtre de prospection s'ouvrir ?
   - Test : client, sélectionner un perso, PROSPECT, en plein désert et près d'une ville. Comparer avec
     l'hôte et noter chaque étape dans le journal.

Outil : `tools/dump_stack.py <fichier.dmp>` lit la pile d'un dump avec le paquet Python `minidump` (le dump n'a pas
le code de la DLL : on compare les offsets avec `dumpbin /disasm /range:` sur la même build).
dbghelp n'a pas su lire le PDB.

## Bâtiments, reste à faire (après les tests du 11 octobre)
- **Test du coffre** dans `buildfull` (3 échecs sur 25) : `conput` cherche les matériaux « par
  terre » alors qu'ils sont dans le sac, et `contcount` ne trouve pas de conteneur sur un bâtiment de
  stockage.
- **Établi de recherche** : le test le pose hors de la cabane ; le poser à l'intérieur pour de vrai.
- **Bâtiments achetés en ville** : le test `buyhouse` n'a pas encore été lancé (navigation, meubles,
  coffres, établis, pour l'hôte et pour le client).
- **Clic droit sur l'établi de recherche** : il faut garder le bouton appuyé pour voir les options
  (signalé par l'utilisateur).
- **PNJ dans les villes lointaines** : encore 16 à 57 unités d'écart en hauteur dans certaines villes
  (moins prioritaire).

## 11 octobre 2026, essai `faraway` (client dans une autre ville, caméra de l'hôte à la maison)

**Deuxième essai (12:48, a100038) : 24/33.** Écarts de position inchangés : ce sont des PNJ qui dorment
dans un lit chez l'hôte, pas chez le client (corrigé : lits synchronisés comme les cages). Dialogue :
PNJ sans réplique (le test parle d'abord au marchand). Commerce : un garde pris pour marchand (test).
Combat : perso du client sur un toit (test). Détail dans CHANGELOG. **À revérifier** :
`coop_test.py faraway --clients 3`.
- **Protocole, plus tard** : un drapeau « dans un lit » à part dans `CaptiveState` (le lit passe pour
  l'instant par le drapeau « en cage », le client distingue par la fonction du meuble).

30/42. Tout ce qui suit est corrigé dans le mod ou le test, **à revérifier en jeu** avec
`coop_test.py faraway --clients 3` (4 joueurs, chacun dans sa ville) :
- **Positions** : 33 à 51 unités d'écart à Admag, 8 à 68 au Bouclier d'Okran (tolérance 3), en hauteur
  (gardes sur les remparts, paladins sur la forteresse) ou vers un lit (paladins arrêtés chez l'hôte,
  arrivés chez le client). Cause : mise à jour de déplacement réduite hors de la vue de la caméra de
  l'hôte (`Character+0x1A9`) ; correction `hk_charUpdateView`. Avant : pire écart 51 (Admag), 68
  (Okran) ; après : à mesurer (la synchro du test donne maintenant le pire écart de chaque relevé,
  `dy` et `inview` de chaque écart).
- **Dialogue** : aucune conversation loin de la caméra de l'hôte (le perso arrive, l'étape « parler »
  ne vient jamais) ; l'hôte la lance lui-même (`UpdatePendingTalks`). Le test attend 45 s par PNJ.
- **Commerce** : fenêtre vide, `GiveShopHome` ne trouvait jamais le bâtiment d'une boutique de ville.
- **Combat** : le PNJ de test copiait un bandit de chez l'hôte et repartait chez lui ; `fight` à sens
  unique. Commandes de debug corrigées (`spawnnpc`, `fight`, clé pour `ko`/`kill`/`npcstate`).
- **Fouille de l'araignée à peau et de la bête à bec** (`lootcreature` 30/32) : pilleur envoyé au
  cadavre local, à 70-79 unités de celui de l'hôte ; il va maintenant à celui de l'hôte.

## 10 octobre 2026, partie réelle en v0.3.0 (soir, 4 joueurs par Steam)

- **~3000 refus « refused an inventory move … 556-gamedata.base (may not take from there) »** dans
  le journal de la session (des bottes) : un client redemande sans fin un déplacement d'objet sur
  un perso dont il n'a pas le droit de prendre. Trouver qui le redemande en boucle (synchro
  d'inventaire qui rejoue le geste ?) et l'arrêter.

- **Son de menu en boucle par moments** chez les joueurs. Piste principale : les fenêtres que le
  mod ouvre et ferme en boucle font jouer le son d'interface du jeu à chaque fois. Dans le
  journal de la session : 74 « trade window open », 50 « closed for a moment » (la réouverture
  automatique de la fenêtre de commerce vide, retirée en 0.3.1 par le lot s031-ui2), 276 lignes de
  conversation. À vérifier en 0.3.1 ; chercher aussi toute fenêtre (conversation, inventaire,
  conteneur) rouverte ou remplie en boucle par la synchro.

Journal de l'hôte gardé dans `kc_crashdumps/session_2215/host_live.log` (à compléter par les
journaux des clients `KenshiCoop-<pid>.log` de chacun).

- ✅ **Les clients ne peuvent pas fouiller certaines créatures** (léviathans…), alors que l'hôte le
  fait en solo ; il faut pouvoir fouiller (et dépecer) toutes les créatures, humaines comme
  animales. Corrigé, à vérifier en jeu (`coop_test.py lootcreature`).
  **Ce que dit le journal** : aucun léviathan dans cette partie, et aucun refus lié à une fouille
  d'animal : l'échec était muet. Les lignes `loot order: N units to go` (73) n'ont aucune suite
  quand la fenêtre ne s'ouvre pas (`RequestLoot` ne journalisait ni l'ouverture ni l'expiration) ;
  plusieurs ordres n'ont été suivis d'aucun déplacement d'objet (nass4 22:44:45, 23:11:53, 23:11:54,
  23:13:25). Les refus du journal sont autre chose : `refused an inventory move … 556-gamedata.base
  (may not take from there)` (près de 3000, des bottes : un client qui redemande en boucle un
  déplacement sur le perso d'un autre) et `client task 16 … unknown task id` (déjà traité : tâche 16
  = `ATTACK_ENEMIES`).
  **Causes** (désassemblage du 1.0.68 et données du jeu) : (1) **la portée** : le client n'ouvrait
  la fenêtre qu'à 12 unités en 3D de la position du corps, la racine du ragdoll, au milieu et en
  hauteur ; un léviathan a un rayon de 40 et une coque de 75 × 90 × 75 (échelle adulte 1,5), une
  bête à bec 16, une tortue des marais 25, gorillos 12, araignées 14 : le pilleur bloqué contre le
  corps n'y arrivait jamais et l'ordre expirait après 30 s sans message ; (2) **l'âge des animaux
  recréés chez les clients** : l'échelle (vt 0x390) était envoyée comme âge (vt 0x398) : un
  léviathan adulte faisait 2,05 fois la taille de sa race chez un client au lieu de 1,5, son corps
  ailleurs. Écartés : le type (seules trois classes de perso existent, `Character`,
  `CharacterHuman`, `CharacterAnimal`, toutes acceptées par `IsCharacter`), la tâche (le clic droit
  donne 26 sur un animal aussi ; il n'existe pas de tâche de dépeçage : la viande, les peaux et les
  perles sont les « death items » de l'animal, dans son inventaire une fois mort), les sections
  d'inventaire (lues par leur nom, quel que soit le perso), les droits de l'hôte (un corps mort ou
  K.-O. hors escouade se fouille, animal ou non).
  **Corrigé** : portée selon la taille du corps (`kc::loot`, rayon et coque de sa race fois son
  échelle), pilleur envoyé au bord du corps, portée élargie s'il s'est arrêté contre, attente selon
  la distance, ouverture et abandon journalisés ; âge réel des animaux recréés ; dépôt au sol
  depuis un grand corps accepté de son flanc. Tests `TestLootCreatures`, `TestBigBodyDrop`.
  **Essai en jeu du 11/10** (`lootcreature`, 26/32) : tortue et léviathan réussis ; chèvre, chien
  d'os, taureau, gorillo, araignée, bête à bec : ordre abandonné, le pilleur toujours à la distance
  de départ. Cause : l'éditeur de personnage du client, laissé ouvert par l'expérience, gardait tout
  le jeu en pause ; seuls les corps déjà à portée s'ouvraient. Corrigé (`editdone` dans
  l'expérience ; pause non comptée dans le délai d'un ordre ; « arrêté contre le corps » seulement
  après avoir marché). À relancer.

- ✅ **Rejoindre est long** (corrigé en 0.3.1, à vérifier en jeu) : nass4 a mis ~1 min 55 entre
  « world sent » (22:15:01) et son arrivée (22:16:55) ; rob, arrivé pendant ce temps, attendait
  dans la file et n'a reçu le monde qu'à 22:17:04 (+1 min 45). Pistes : télécharger et charger le
  monde pendant l'attente (seule la création du perso chacun son tour) ; mesurer transfert Steam
  vs chargement Kenshi ; compresser le monde envoyé.
  **Ce que disent les journaux** : le transfert n'était pas l'essentiel. rob a reçu 3,3 Mo et
  chargé en 18 s en tout ; au resync de 23:16, nass4 a reçu 7,6 Mo et chargé en 36 s : ses 3,3 Mo
  de 22:15 ne pouvaient pas prendre plus d'une quinzaine de secondes. Ses 1 min 55 sont surtout le
  premier chargement de Kenshi depuis le menu (rien en cache), sur son PC. Ce qui coûtait vraiment,
  c'était la file : rob attendait le chargement **et** l'éditeur de nass4 avant que sa sauvegarde
  soit même commencée. Les journaux ne donnaient aucune durée par étape (les lignes du client
  arrivent toutes à l'heure de son arrivée).
  **Corrigé** : (1) tous les joueurs qui arrivent téléchargent et chargent le monde aussitôt ; une
  sauvegarde sert tous ceux qui en attendent une (avec leurs persos et ceux des précédents), la
  suivante part dès qu'elle est finie ; seul l'éditeur de personnage passe un joueur à la fois,
  les autres attendent dans le monde en voyant leur place. Les arrivants plus anciens reçoivent
  une doublure des persos des suivants, comme les joueurs déjà là ; l'apparence faite dans
  l'éditeur pendant qu'un autre charge lui est envoyée à son arrivée. (2) Chaque jeu d'un même PC
  a son propre emplacement d'import (`KenshiCoopJoin`, `KenshiCoopJoin2`…). (3) Durées au
  journal de l'hôte : sauvegarde, compression, transfert avec débit et ping (« world delivered
  to … »), chargement, attente de l'éditeur, éditeur, et un bilan « join timings for … » ; côté
  client, débit du téléchargement et durée du chargement. (4) Le monde est compressé (XPRESS
  Huffman de Windows) : **protocole 35**. Au prochain essai réel, lire « world delivered » : si le
  relais Steam reste lent (débit bas), voir la fenêtre ENet (64 Ko en vol par aller-retour).
- **Recrutement par un client** : c'est l'hôte qui a eu la fenêtre pour accepter ; le perso
  recruté est bien arrivé dans l'escouade (chez l'hôte) mais le client ne le voit pas.
  **Corrigé (non vérifié en jeu)** : la fenêtre était l'éditeur de perso (`recruit(perso, true)`,
  ligne « rejoindre avec édition ») ; la recrue arrivait sous un nouveau handle sans que les clients
  sachent quel PNJ c'était. Recrutée sans éditeur chez l'hôte, donnée au joueur, `Bind` avec l'ancien
  handle, adoptée chez chaque client, éditeur ouvert chez le joueur (voir CHANGELOG).
- **Fenêtre de sortie des mines invisible chez les clients** (l'inventaire de production). Vérifier
  tous les types de mines et d'ateliers / bâtiments de production.
  **Corrigé (non vérifié en jeu)** : `Building::select` n'ouvre le panneau que pour un bâtiment fini
  du joueur ; un filon devient au joueur quand on le travaille, chez l'hôte seulement. L'hôte envoie
  le propriétaire (`kMachOurs`), le client l'impose. Même règle pour tous les bâtiments : ceux du
  joueur l'étaient déjà des deux côtés ; seuls mines et filons changent de propriétaire en jeu.
- ~~**Objet glissé au sol par le client 2 disparu**~~ : il n'est pas apparu au sol, il a disparu (pas
  à chaque fois). Perte d'objet : prioritaire. — **corrigé** (à vérifier en jeu), voir l'entrée
  « armes jetées » plus bas : même cause (handle local d'une doublure envoyé à la place de celui
  de l'hôte, objet déjà retiré de l'inventaire du client).
- **Joueur 3 dans une ville voisine : parler aux PNJ ne fait rien** (aucune fenêtre de dialogue).
- ~~**Ramassage par le client 2 : objets perdus**~~ (prioritaire, lié au point « objet glissé au sol
  disparu ») : un objet ramassé disparaît du sol mais n'arrive pas dans son inventaire ; une autre
  fois il est bien arrivé puis a disparu de l'inventaire au bout d'un moment (inventaire réécrit
  par celui de l'hôte, où l'objet n'est jamais arrivé ?).
- ~~**Construction par le client 3**~~ (corrigé en 0.3.1, à vérifier en jeu) : la construction s'est
  bien passée, mais (1) la jauge des matériaux de construction du chantier ne montrait pas les
  matériaux apportés (chez le client) ; (2) une fois fini, le bâtiment n'a pas de collision chez le
  client (on le traverse), alors qu'un bâtiment normal en a.
  **Causes (désassemblé, docs/MOTEUR.md section 10)** : (1) la jauge lit la quantité apportée de
  chaque matériau (`ConstructionState`+0x20, entrée +0xC), remplie seulement par les livraisons,
  qui n'ont lieu que chez l'hôte ; `BuildState` n'envoyait que l'avancement. (2) Le jeu crée les
  corps physiques d'un bâtiment éteints et seule sa mise à jour d'état (vt 0xE0) les allume ;
  `notifyConstructionComplete` ne l'appelle pas (son `setVisible(1)` ne fait rien sur un chantier
  déjà visible), et les bâtiments ne sont pas mis à jour à chaque image : un bâtiment fini en
  cours de partie reste sans collision jusqu'au rechargement de sa zone (chez l'hôte aussi selon
  le désassemblage, non vu en jeu). Les bâtiments de la sauvegarde sont créés finis et l'ont.
  **Corrections** : message 87 `BuildMaterials` (H→C, ignoré par un 0.3.0, pas de changement de
  version) avec les quantités apportées, écrites chez le client ; vt 0xE0 appelée une fois un
  bâtiment fini sans collision (client après chaque état, hôte pour les bâtiments suivis).
  À vérifier : jauge chez un client pendant les livraisons ; on ne traverse plus un bâtiment fini
  (client et hôte) ; « collision switched on » dans les journaux.
  par celui de l'hôte, où l'objet n'est jamais arrivé ?). — **corrigé en partie** (à vérifier en
  jeu). Ce que dit le journal : les ramassages des clients réussissent chez l'hôte (« pick up Rhum de
  Cactus -> ok », « the item left the ground (taken) ») ; nass4 a ensuite voulu reposer le fer qu'il
  venait de ramasser (22:24:28 puis 22:24:43 « drop … not sent ») : l'objet a quitté son inventaire
  chez lui sans partir chez l'hôte, d'où « arrivé puis disparu ». Corrigé avec les dépôts (entrée
  suivante). En plus : (1) la reconstruction d'un inventaire du client laissait le jeu **détruire** un
  objet qu'elle ne pouvait pas poser à la place de l'hôte (« cannot place 42243-rebirth.mod » puis
  « here missing », 58 fois) : il va maintenant dans le sac ; (2) le jeu du client ne ramasse plus
  jamais un objet du monde lui-même (avant : seulement refusé pour un perso suivi dans la dernière
  seconde), sinon l'objet serait dans un inventaire que l'hôte n'a pas et la synchro l'effacerait ;
  (3) l'hôte journalise où arrive un objet ramassé pour un client (« in the inventory of the
  character that was sent »). Les 8624 « host picked up item …: we do not have it » étaient des
  objets neufs (équipement d'une escouade qui apparaît, stock d'un marchand) que l'hôte annonçait
  comme ramassés : il ne les annonce plus (`kenshi::ItemPlacedInWorld`). Non trouvé : un ramassage
  dont l'objet n'arrive jamais ; si ça se reproduit, la nouvelle ligne de l'hôte dira où il est allé.
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
  **Revu** : la tâche 16 n'est pas démanteler mais `ATTACK_ENEMIES` (numérotation du jeu dans
  `fcs_enums.def`), ajoutée par le clic droit d'attaque : ajoutée à la liste, comme toutes les tâches
  de l'interface (voir CHANGELOG). Démanteler (96) était déjà permis sur un bâtiment du joueur, et
  **aucune** demande de démontage de rob n'est arrivée à l'hôte (ni `confirmDismantle`, ni tâche 96
  dans le journal) : son jeu ne lui proposait sans doute pas le bouton. Même règle que la fenêtre des
  mines (`Building::select` : bâtiment fini **du joueur**) : à vérifier avec la correction de la
  collision (bâtiment fini par la synchro sans être un vrai bâtiment fini du joueur chez le client).
- ~~**Commerce du joueur 3 (rob) : fenêtre qui clignote et vide**~~ (corrigé en 0.3.1, à vérifier en
  jeu ; cause ci-dessous) (23:07). Journal de l'hôte :
  « trade window open: 31 shop counters holding 136 stacks », puis « trade window shows none of
  the shop's 136 stacks: opened again (1..3) » et « still shows none … after 3 tries ». Le
  clignotement vient du contournement (réouverture 3 fois) ; le vrai bug est la fenêtre du jeu
  construite sans les comptoirs (marchand en ville avec 31 comptoirs). C'est le défaut
  intermittent « tradelist merchant → 0 » déjà vu en test, jamais compris : à trouver
  (comptoirs du client pas encore reliés au bâtiment quand la fenêtre se construit ? bâtiment
  du marchand différent chez le client ?). Supprimer la réouverture qui clignote une fois la
  cause corrigée.
  **Cause trouvée** : le marchand du client était une **doublure** créée par le mod. La ville de la
  Ruche a été générée à part chez l'hôte et chez rob (zone chargée après l'arrivée), avec d'autres
  handles : à 23:05:01 le jeu de rob a eu son propre « Marchand Ruche », retiré par le mod (« removed
  the local character 'Marchand Ruche': the host does not have it ») au profit d'une doublure
  (`createRandomCharacter`). Le constructeur de `ShopTrader` (`0x953850`) prend le domicile dans
  `getOwnerships(marchand)+0x38` (pour un PNJ : son escouade) ; la doublure n'en a pas, donc il vend
  depuis les sacs de son escouade : fenêtre vide. Après la reconnexion de 23:17 (sauvegarde où la
  ville existe), le même marchand marchait (23:24, 162 objets). Même chose pour nass4 (« Produits
  Commerciaux », « Marchand Construction Ronin »).
  **Correction** : avant d'ouvrir la fenêtre, le client donne au marchand le bâtiment des comptoirs
  envoyés par l'hôte quand il ne vend pas déjà depuis eux (`kenshi::GiveShopHome`, journal « trade:
  the merchant here did not sell from the host's N counter(s) … given the shop's building ») ; la
  réouverture (3 fois) est supprimée, une fenêtre vide est seulement notée au journal.
  **Essai `faraway` (11/10, Admag) : ne marchait pas** (« could not be given their building », stock
  vide, achat refusé) : `GiveShopHome` cherchait le bâtiment par `+0x238` des comptoirs, vide pour un
  meuble de ville. Il le cherche maintenant parmi les bâtiments autour des comptoirs. Les comptoirs et
  leur contenu arrivaient bien au client lointain. À revérifier : `coop_test.py faraway`.
- **PNJ saccadés et « moonwalk » chez le joueur 3 dans un village de la Ruche** (beaucoup de
  monde), quand il était à ~900 m de l'hôte : mouvements très saccadés, PNJ qui glissent à
  reculons. Pistes : débit par client trop faible pour une zone dense loin de l'hôte (priorité /
  fréquence par distance au joueur concerné, pas à l'hôte), interpolation qui suit des positions
  trop espacées, orientation / animation de marche pas mise à jour (marche jouée à l'envers quand
  le PNJ est déplacé vers l'arrière de sa direction). Mesurer : fréquence des positions reçues
  par PNJ chez ce client, écart d'orientation.
  **Cause trouvée (faraway, 11/10)** : loin de la caméra de l'hôte, son jeu déplace les PNJ avec une
  mise à jour réduite (`Character+0x1A9` à 0 : quelques pas par seconde, une autre hauteur), alors que
  le client, qui les a sous sa caméra, fait la complète. L'hôte envoie bien 20 positions/s de chaque
  perso diffusé (pas de budget par joueur), mais ces positions avancent par sauts. Le détour
  `hk_charUpdateView` met dans la vue les PNJ près d'un client (hôte) et les copies pilotées (client).
  À revérifier : `coop_test.py faraway --clients 3` (mouvement, synchro).
- ~~**Client 2 (nass4) : armes jetées / échangées sur un perso de son escouade**~~ : changé chez lui,
  **Corrigé côté client (0.3.1, non revérifié en jeu)** : positions répétées fusionnées, retard de
  rendu par perso selon sa cadence, courte extrapolation, rappel jamais en arrière contre la
  marche, direction qui suit le sens réel du déplacement, destination vérifiée. Reste à voir :
  débit par client dans une zone dense loin de l'hôte (priorité par distance au joueur concerné).
- **Client 2 (nass4) : armes jetées / échangées sur un perso de son escouade** : changé chez lui,
  pas chez les autres ni chez l'hôte. Journal de l'hôte, plusieurs fois entre 23:08 et 23:11 :
  « [nass4] drop to the ground not sent: the host does not know that inventory
  (52295-rebirth.mod) » (et 1020-gamedata.base). Le client fait le geste dans son jeu, ne
  l'envoie pas (inventaire source inconnu de l'hôte : probablement un sac à dos ou un objet
  conteneur d'un mod, ou le perso n'est pas reconnu), mais **ne l'annule pas chez lui** :
  l'objet disparaît localement puis la synchro le fait revenir ou le perd. Même cause probable que
  « objet glissé au sol disparu » et « ramassage perdu ». À corriger : (1) tout geste qui ne peut
  pas partir chez l'hôte doit être refusé localement (rien ne bouge chez le client) ; (2) faire
  connaître ces inventaires à l'hôte (sacs, conteneurs de mods, persos de l'escouade du joueur).
  **Pareil pour rob** (23:14-23:15, 9 fois) : « drop to the ground not sent: the host does not
  know that inventory (580-gamedata.base) » : 580 = matériaux de construction, posés depuis
  l'inventaire de **son propre perso**. Donc en vraie partie (Steam, rejoint après un plantage),
  le client ne retrouve pas l'inventaire de son propre perso côté hôte pour un dépôt au sol, alors
  que le test `grounddrop` (39/39, en local) passe : chercher pourquoi (identifiant du perso après
  une reconnexion, perso créé par l'éditeur, handle différent par Steam).
  **Propriété après le retour de rob, vérifiée (0.3.1)** : à son retour (22:32:45) l'hôte lui rend
  son perso (même handle 1:1:3852294912:3:1700561536, trouvé par le fichier des joueurs) ; l'entité
  garde son netId (rien n'est recréé côté hôte) et le `Bind` de `FinishJoin` le donne comme sien.
  Test `TestJoinReturns` : retour après plantage, ses persos (le sien et un donné par l'hôte,
  rehandlé entre-temps) sous les mêmes netIds, à lui chez lui, et son dépôt au sol part chez
  l'hôte. Corrigé au passage : les persos à rendre étaient retenus par handle (perdus si Kenshi
  change le handle pendant l'absence) : maintenant par netId. **La propriété n'explique donc pas
  les 9 échecs de 23:14-23:15**, 42 min après le retour. Indice pour celui qui reprend le dépôt :
  à 23:14:46.210, 0,5 s avant le premier échec, le client de rob note « our copy
  1:105:1015136704:1:644287104 of the host's 1:61:50659180:1:462158208 changed squads here (now
  1:1:3852294912:1:644287104): followed » : une doublure de PNJ de rob prend un handle de
  l'escouade du joueur (celui du perso de l'hôte, idx 1 ser 644287104) ; le message « does not
  know that inventory » veut dire qu'aucune entité du client n'a le handle que le jeu donne pour
  l'inventaire source : à chercher du côté des handles locaux qui changent (escouade), pas de la
  propriété.
  **Corrigé** (à vérifier en jeu). *Cause* : `KenshiWorld::TakeLocalDrops` nommait l'inventaire par
  le handle **local** du perso, la session le cherchait parmi les handles **de l'hôte**. Ils
  diffèrent pour toute doublure (journal : « stand-in 1:582:… for the host's 1:47:… ») et tout perso
  qui a changé d'escouade (les persos donnés à un joueur, un joueur revenu après un plantage) ;
  en local, même sauvegarde, mêmes handles : `grounddrop` passait. Les ordres passaient déjà par
  `HostHandleOf` (rob parlait aux PNJ avec ce même perso pendant que ses dépôts échouaient). Ni sac à
  dos ni conteneur de mod : 52295 et 1020 sont des armes portées. Le crochet avait déjà retiré
  l'objet chez le client. *Correction* : handle de l'hôte pour un perso ; la session publie les
  inventaires d'où un dépôt peut partir (`SetDropSources`) et la fenêtre d'inventaire refuse
  aussitôt les autres (l'objet reste ou revient à sa place, toast « Objet non posé ») ; le jeu du
  client ne pose plus jamais rien lui-même ; un sac à dos est nommé lui-même (avant : par son
  porteur, l'hôte cherchait l'objet dans le mauvais inventaire) ; un refus tardif remet l'état de
  l'hôte tout de suite. Test `TestClientDropGate`. Reste à vérifier en jeu que l'objet tenu à la
  souris revient bien à sa place (`kenshi::KeepInInventory`).
- **Parler aux PNJ : lent, et parfois impossible** (rob, Ruche, 23:14). (1) Lent : l'ordre
  « parler » part chez l'hôte, le perso marche jusqu'au PNJ, puis la conversation s'ouvre ; aucun
  retour visible pendant ce temps. (2) Impossible : « [rob] order "talk" (12) -> refused: Marchand
  Ruche is busy talking with rob » : le PNJ est jugé occupé **avec rob lui-même** (conversation
  précédente pas encore fermée côté hôte). Le refus « occupé » ne doit jamais viser le même
  joueur : il doit reprendre ou remplacer sa propre conversation. Voir aussi les ordres « parler »
  acceptés (« -> ok ») qui n'ont rien ouvert (23:14:02, 23:14:04).
  **Corrigé (non vérifié en jeu)** : jamais « occupé » pour le joueur lui-même (conversation renvoyée
  sur son écran, ou l'ancienne terminée) ; une conversation que le jeu garde entre ce PNJ et un perso
  du joueur sans fenêtre (cause probable des « -> ok » sans rien) est terminée avant l'ordre ; le
  client affiche « X va parler à Y » pendant que son perso marche.
- **rob se désynchronise peu à peu** ; un « Resynchroniser tout le monde » de l'hôte l'a remis
- ~~**rob se désynchronise peu à peu**~~ — **corrigé (0.3.1, à vérifier en jeu)**, voir plus bas ; un « Resynchroniser tout le monde » de l'hôte l'a remis
  d'aplomb. Bilans périodiques de son client (`[rob] sync:`) : jusqu'à 41 puis 69 « NPCs not there
  yet » (PNJ de l'hôte jamais apparus chez lui, 23:13-23:18, ville bondée), 288-315 persos suivis.
  À faire : (1) ces PNJ manquants doivent être demandés / recréés sans attendre (cf. la boucle de
  recréation d'escouade avant son plantage) ; (2) détection automatique : quand un client reste
  trop longtemps avec des PNJ manquants ou des écarts, resynchroniser sa zone seulement, sans
  attendre l'hôte (idée « empreinte par zone » de docs/INSPIRATION_PZ.md) ; (3) mesurer le débit
  envoyé à un client dans une zone dense.
  **Fait (0.3.1)** : cause = l'hôte envoyait à chaque client toute la foule active de son jeu
  (autour de sa caméra et de son escouade), et le client la recréait là où il n'avait rien chargé
  (9 723 « is gone here » pour rob). Maintenant (1) chaque client ne reçoit que les PNJ à moins de
  `stream_radius` (3000) de ses propres persos, recrée 2 persos par image au plus, ne compte comme
  manquants que ceux-là ; (2) resync automatique de **sa** zone après 15 s de PNJ manquants ou de
  persos décalés (sans pause ni clic, au plus une fois par minute) ; (3) ligne « stream: X is sent N
  of M characters …, K KB/s, ping, % lost » par joueur et par minute. Détail : CHANGELOG.
  Les empreintes de zone ne sont pas faites (les rapports suffisent pour décider).
- **Prisonniers portés / kidnappés qui convulsent chez nass4** : les gens qu'il a kidnappés
  (portés sur l'épaule ou attachés) alternent sans arrêt assis / debout / couchés sur lui chez lui.
  Probable : la posture (porté, à terre, assis) et la position imposées par la synchro se battent
  avec l'état « porté » local (ApplyCarry, ragdoll, posture : « falls as on the host » / « stood
  up to fall where the host's lies » à chaque image) ; un perso porté ne doit recevoir ni position
  ni posture tant qu'il est porté, et un prisonnier assis / attaché doit garder la pose de l'hôte.
  **Cause confirmée** : 1811 lignes `[nass4] posture:` dans la session, en boucle toutes les 2 à
  3 s sur le même corps (« lies 8 units from the host's body: stood up to fall where the host's
  lies » puis « falls as on the host »). C'est la correction des corps à terre ajoutée le 10/10
  (relever un corps à plus de 5 unités et le refaire tomber, censée se limiter à 3 fois) : elle
  boucle, et elle s'applique aux persos portés ou prisonniers. À corriger : ne jamais l'appliquer
  à un perso porté, attaché, en cage ou assis ; limite réelle par corps ; seuil plus large.
  **Corrigé (0.3.1, non revérifié en jeu)** : la boucle principale était la chute elle-même (1859
  « falls as on the host » chez nass4 sur des captifs enchaînés que son jeu rasseyait, une toutes
  les 2 s, sans aucune limite), plus le relevage à 5 unités. Captifs (porté, enchaîné, cage, lit,
  prisonnier, esclave) jamais touchés ; 4 chutes et 2 relevages par corps et par minute, abandon
  pour la minute après deux chutes qui ne tiennent pas ; relevage à plus de 20 unités. Voir
  CHANGELOG (En cours).
- ✅ **Geoffrey bloqué avant la création de son perso** (23:36-23:37 ; corrigé en 0.3.1, à vérifier
  en jeu). 1er essai : son jeu a quitté ou planté pendant le chargement (« left while joining
  (connection lost …) »). 2e essai : l'hôte dit « Geoffrey is back with their character » puis
  « 0 character(s) they had are theirs again » : il se souvient d'un perso de Geoffrey (fichier des
  joueurs, partie précédente ?) qui n'existe pas ou plus dans ce monde, donc il ne lui ouvre pas
  l'éditeur ; Geoffrey entre sans aucun perso. À corriger : si le perso mémorisé n'est pas dans le
  monde (ou n'est plus un perso du joueur), le traiter comme un nouveau joueur (éditeur de perso).
  Contournement : l'hôte lui donne un perso.
  **Cause réelle** (journal) : le perso existait bien. À 23:34:23 l'hôte crée le perso de Geoffrey
  (1:1:3852294912:5:981811520) et l'éditeur s'ouvre chez lui à 23:35:06 ; à 23:36:29 l'hôte lance
  « resync 4 » **pendant que Geoffrey est dans l'éditeur** : son jeu recharge, il part avant de le
  fermer. À son retour, le fichier des joueurs le retrouve (« is back with their character ») :
  perso pas nouveau, donc pas d'éditeur, alors qu'il ne l'avait jamais fait. Le « 0 character(s) »
  était trompeur : son perso lui avait déjà été rendu juste avant, il n'était pas compté (rob à
  22:32, même ligne). Au 3e essai il avait bien ce perso (diagnostic : sélection idx=5 ser=981811520,
  mode dieu et TP de l'admin sur « 1 character(s) »), mais un perso jamais personnalisé.
  **Corrigé** : un joueur dont le perso a été créé et qui part avant de fermer l'éditeur le retrouve
  à son retour (par compte Steam, en mémoire tant que l'hôte tourne) ; un perso mémorisé qui n'est
  pas un membre **vivant** de l'escouade de ce monde n'est plus rendu (nouveau perso, éditeur) ; le
  journal compte aussi le perso déjà rendu (« (1 of them their own character) »). Reste : un resync
  lancé pendant qu'un joueur est dans l'éditeur le fait toujours recharger (il retrouve l'éditeur).
- ~~**rob ne voyait plus les dégâts**~~ (sang, chiffres, barres de vie / blessures qui ne bougeaient
  plus) ; l'hôte l'a « spec » (regardé avec la caméra sur lui) et il les a revus. Comme les autres
  désyncs de rob : quelque chose dans le flux vers ce client s'arrête (vitals / effets de
  combat ?) jusqu'à un événement qui le relance. À chercher dans les bilans `[rob] sync:` et les
  envois de vitals par client : priorité ou zone d'intérêt calculée autour de l'hôte / de sa
  caméra au lieu des persos du joueur concerné ?
  **Trouvé et corrigé (0.3.1, à vérifier en jeu)** : `MedicalSystem::addWound` ne crée le chiffre de
  dégâts que si le perso est vu par la caméra (`Character+0x1A9`, test en `0x651DF5`) : sans la
  caméra de l'hôte sur rob, aucun chiffre pour ses combats. Le détour `hk_addWound` met ce drapeau,
  le temps de l'appel, pour les persos proches d'un perso d'un joueur client. Et le flux de rob était
  encombré par la foule de l'hôte (tout lui était envoyé) : il ne reçoit plus que ce qui l'entoure.
  Reste à voir : le sang (effets visuels) n'est pas répliqué du tout.
- ~~**Geoffrey : chutes de FPS, l'écran se fige par moments.**~~ Ses bilans : 104-133 images/s avec
  179-195 persos suivis, 65 images/s quand ça monte à 300 persos (23:42). Il recrée beaucoup de
  persos (199 lignes « stand-in … gone » / « cannot recreate host character » en quelques minutes,
  la fabrique échoue en boucle sur des persos uniques comme 'Cat', 'Ruka', 'Kang' : chaque essai
  coûte), et sa console reçoit des centaines de lignes « anim … » (journalisation coûteuse). Pistes :
  ne plus réessayer sans fin un perso que la fabrique refuse (liste noire par modèle) ; couper les
  journaux par image ; étaler les créations de persos sur plusieurs images (les gels = beaucoup de
  persos créés d'un coup à l'arrivée dans une zone).
  **Fait (0.3.1, à vérifier en jeu)** : un modèle refusé par la fabrique attend 30 s, 1, 2 … 10 min ;
  2 créations par image au plus (`[sync] spawns_per_frame`), les plus proches d'abord ; doublure perdue
  sans cesse : 2 s, 10 s, 30 s, 1 min, 2 min ; journaux « is gone here » (6 / 10 s), « anim in » (20),
  « anim not blocked » (10) limités ; persos qu'on ne nous envoie plus pilotés 2 fois/s seulement ;
  et surtout moins de persos suivis (seulement ceux autour de ses persos).
- **PNJ assommés vus debout chez nass4** : il les a mis K.-O., peut les piller (l'hôte les sait à
  terre), mais chez lui ils restent debout. La posture « à terre » n'est pas appliquée chez lui
  (ou relevée aussitôt : voir la boucle « stood up to fall where the host's lies » des prisonniers,
  même mécanisme). À vérifier avec la correction de cette boucle.
  **Corrigé (0.3.1, non revérifié en jeu)** : le corps tombe maintenant inconscient chez le client
  quand l'hôte le dit K.-O., et le jeu du client peut remettre en ragdoll un perso inconscient ou
  mort (`hk_ragdollMode` le refusait).
- **Cadavres qui finissent tous debout chez les clients** (signalé après la session). **Corrigé
  (0.3.1, non revérifié en jeu)** : `hk_ragdollMode` refusait au jeu du client de remettre un
  cadavre en ragdoll ; une doublure recréée pour un corps mort naissait vivante et debout ; un
  corps posé par son porteur était posé debout. Doublure tuée et couchée dès sa création, ragdoll
  permis aux morts, jamais ranimé par la santé, jamais relevé. Test « cadavre recréé ».
- **Combats de « Hep » pas fluides chez les clients** (perso de l'escouade qui n'est à aucun
  joueur, se bat beaucoup). Attaques, esquives et déplacements de combat saccadés. À mesurer :
  fréquence des positions et des animations de combat envoyées pour les persos de l'escouade
  non-joueurs, interpolation pendant les attaques (comme le test « vitesse 3 : orientation en
  marchant » qui échoue parfois).
- ~~**Vue des étages chez le client**~~ (corrigé en 0.3.1, à vérifier en jeu) : le client doit
  forcer lui-même la vue des étages (toit / étages cachés) ; elle ne suit pas automatiquement quand
  son perso entre dans un bâtiment ou change d'étage, comme en solo.
  **Cause (désassemblé)** : la vue suit le perso « suivi » du `PlayerInterface` (+0x270), pas la
  sélection (`0x801630` compare son étage au dernier vu et affiche le nouveau). Il n'est changé que
  par `0x7F5400` (double appui sur la touche d'escouade, portrait, fin de l'éditeur). Chez un client
  il restait celui de la sauvegarde chargée (le perso de l'hôte). Le `floorGroup` n'y est pour rien
  (le jeu du client le calcule lui-même) : les écritures restent désactivées.
  **Correction** : `KenshiWorld::UpdateFloorFocus` pointe ce perso suivi sur un perso du client (le
  sélectionné d'abord), sans toucher à la caméra, quand il n'est pas déjà un des siens (journal
  « floor view: follows our … »).
  **En partie corrigé (0.3.1, non revérifié en jeu)** : retard de rendu adaptatif (plus d'arrêt
  puis saut à la moindre gigue) et rappel de position indépendant des images par seconde. Rien de
  propre à Hep trouvé dans le journal : à revoir en jeu ; si c'est encore saccadé, mesurer la
  cadence des positions reçues pour lui (trace `trace` du client).
- **Vue des étages chez le client** : le client doit forcer lui-même la vue des étages (toit /
  étages cachés) ; elle ne suit pas automatiquement quand son perso entre dans un bâtiment ou
  change d'étage, comme en solo. À voir avec le niveau d'étage (floorGroup, écritures
  désactivées : `kWriteFloors = false`).
- **rob (joueur 3) a planté à 22:32** (et à 00:28:13) (l'hôte l'a vu partir à 22:32:02). Juste avant, son jeu
  perdait et recréait en boucle une escouade de 17 PNJ de l'hôte (`1:96:522216800:*`), toutes les
  ~3 s (« stand-in … is gone here: it can be recreated », 467 lignes de ce genre pour lui dans la
  session). Probable cause ou facteur : cette boucle. Demander à rob son `KenshiCoop.log` et le
  `crashDump*.zip` de son dossier Kenshi (le vrai rapport du plantage est chez lui).
  **0.3.1 (à vérifier en jeu, sans rapport de plantage)** : les deux fois, une escouade locale entière
  de doublures disparaît d'un coup ~10 s avant (00:27:50, 00:28:03 : 1:53:… 1 à 10 pour 1:899 à 1:904
  chez l'hôte), quand il « change de vue » : sa caméra part, son jeu décharge la zone. Le miroir
  d'animations gardait l'adresse de ces persos jusqu'à 1 s et écrivait dans leurs objets libérés :
  maintenant chaque perso détruit ou déchargé est retiré de tout notre état (`PurgeDestroyed`, détour
  `GameWorld::destroy`), et le miroir ne touche qu'aux persos trouvés dans l'image. Plus de boucle de
  recréation (délais croissants, recréation seulement près de ses persos). Repro en jeu à lancer :
  `coop_test.py camjump`.

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
  **Confirmé et corrigé (faraway, 11/10, à revérifier)** : 33 à 68 unités en hauteur seulement (x, z
  identiques) à Admag et au Bouclier d'Okran. Hors de la vue de sa caméra, le jeu prend pour un PNJ
  la mise à jour de déplacement réduite (`0x148AC0`) au lieu de la complète (`0x147FD0`), selon
  `Character+0x1A9` ; les persos de joueurs sont toujours dans la vue. `hk_charUpdateView` met dans la
  vue les PNJ près d'un client (hôte) et les persos pilotés par l'hôte (client).

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
  Essai du 13/10 : encore 12 unités (le corps lâché debout s'effondre à 7-12 unités de ses pieds ; le relevage n'agissait qu'au-delà de 20) et pas de BILAN (`summary()` manquait). 0.3.4 (`fix-machines`) : un corps posé est relevé une fois et refait tomber d'autant en deçà du point de l'hôte s'il en est à plus de 5 unités ; à revérifier (`carry`). Second essai du 13/10 (13:56, 6/7) : retombé au même point au millième près (mis debout à la hauteur de l'épaule lue chez l'hôte, le jeu le posait sur l'escalier) ; corrigé (`fix-tags-carry2`) : mis debout à la hauteur du sol de son porteur. Troisième essai (15:05, 6/7, même point) : le jeu remet le corps posé où il gisait avant d'être ramassé et les téléportations ne le bougent pas ; corrigé : son nœud de scène est mis sur le point de chute avant la chute ; à revérifier.
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
