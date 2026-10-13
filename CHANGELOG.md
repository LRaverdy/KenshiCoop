# Journal des changements

Du plus récent au plus ancien. Chaque entrée correspond à un commit (son identifiant entre
parenthèses). L'état détaillé de chaque fonctionnalité est dans
[docs/FONCTIONNALITES.md](docs/FONCTIONNALITES.md).

## v0.3.4 (en cours)

Protocole inchangé (36).

- **TP admin d'un perso K.-O. : il ne retourne plus là où il gisait (`suite` 29/32 du 13/10 16:19,
  branche `fix-tp3`)**. « TP admin : le perso du joueur arrive près de l'hôte » donnait 1147,2 et
  « le client le voit au même endroit » 257,63 (couché). Ce n'est pas une régression des fusions
  4931ea4 / 8ec40dd / f9f0397 : elles ne touchent que le client, et la TP se fait chez l'hôte. Ce
  qui a changé, c'est la partie : à 15:27 (32/32) le perso du joueur était debout (« tp: … moved »),
  à 16:19 les combats de la sauvegarde l'avaient mis K.-O. (« faints as on the host » à 16:19:10),
  et la TP passait par le chemin du corps au sol, à 1066 unités. Journal de l'hôte : « teleported,
  its body's scene node 1066 units from there: moved with it », 0,3 s plus tard « its body's scene
  node 1066 units away again before it lies down », puis « went back 1077 away when it lay down
  again ». Le nœud de scène du corps, posé à destination, revenait tout seul à l'ancienne place.
  Cause (désassemblage) : la mise à jour de l'animation (`0x5B5590`, table virtuelle
  d'`AnimationClass`) remet ce nœud à chaque image sur une ancre, `AnimationClass`+0x258. Seule
  `AnimationClass::<set position and direction>` (`0x5B18F0`) écrit cette ancre, appelée par la mise à
  jour de `CharMovement` (`0x65FFA0`). Ni la téléportation ni `Node::setPosition` ne la déplacent. Le
  ragdoll part du nœud : selon que la demande de ragdoll passait avant ou après cette remise en
  place, le corps se couchait à destination (`tpdown`, 202 unités, réussi) ou à son ancienne place.
  `kenshi::PlaceBodyNode` déplace maintenant l'ancre avec le nœud (nouvelle entrée
  `FnAnimSetPosition`, direction nulle : l'orientation ne change pas). Tous ses appels en profitent :
  TP admin chez l'hôte, chute d'une copie et corps posé d'un portage chez le client. Pas de test
  unitaire (appel du moteur). Non relancé en jeu : à voir avec `suite` (si le perso est K.-O., plus
  de « away again » ni de « went back » dans les lignes « tp: ») et `tpdown`.
  - `tpdown` 1/2 à 16:28 (« le client le voit au même endroit » 3,17, seuil 3) est un autre cas.
    L'hôte a réussi (11,3). Le client s'est mis debout au point lu sur l'hôte au moment où celui-ci
    se couchait (24853,7 / 10541,8), puis le corps de l'hôte a encore glissé de 3,8 unités en
    s'effondrant (24857,3 / 10542,7). C'est l'écart ordinaire d'un corps qui s'effondre. Rien ne
    change ici.
  - Relancé à 17:05-17:09 avec ce correctif (`carry` 7/7, `workstations` 45/45). `tpdown` : chez
    l'hôte, 2,8 de l'endroit visé, et plus de « away again » ni de « went back » : l'ancre tient. Chez
    le client, 5,79. Il a mis sa copie debout sur la place de l'hôte à 49,94, avant que le corps de
    l'hôte se couche (~50,2) et s'effondre de 4,9 unités (24853,7 / 10543,7 → 24849,3 / 10541,2).
    La copie s'est couchée de son côté (à sa place à 0,5 près). Le seuil du test passe de 3 à 20,
    `kc::motion::kRelayOffset` : un corps K.-O. n'est relevé qu'au-delà, comme pour la vérification
    « couché » de `suite`.
  - `suite` 30/32 : cette fois, le perso du joueur était debout (« tp: … moved »). La TP a réussi,
    puis le membre 0 a marché 28,6 unités en 3 s, et le perso du joueur avec lui, dans le même sens
    (38,2 de l'endroit visé, 11,7 du membre 0). C'est le combat de la sauvegarde : membre 3 blessé
    à 17:07:25, membre 0 K.-O. à 17:07:46. Le client était à 4,02 pendant qu'ils marchaient. Si le
    membre 0 a bougé de plus de 10, être à moins de 30 de lui compte comme arrivé. Le seuil du client
    passe alors à 10 (2 à l'arrêt, 20 couché).

- **Noms des joueurs arrivés plus tard et corps posé après un portage (tests `map` 48/50 et `carry`
  6/7, 13/10 13:45-14:00, branche `fix-tags-carry2`)**.
  - `map` : chez le client 1, le nom de Joueur3 (perso principal du client 2, arrivé après lui)
    restait `label=jeu tint=0 frame=0` ; le repli sur l'overlay du correctif précédent ne s'est pas
    déclenché, et pour cause : le jeu met bien ce nom à jour (journal du client 1 : « 7 by the game's
    name tags », 13 recolorations dont celle de ce nom à son arrivée). La cause : chez un client, le
    perso d'un joueur arrivé après lui est une **copie faite sur place, sous un autre handle** que
    celui de l'hôte (alias de `KenshiWorld`). La scène gardait le handle **de l'hôte** pour ce perso,
    et les cadres (`ReadGui`) comme la lecture `tint=` le résolvaient directement
    (`kenshi::Resolve`) : rien trouvé, donc ni cadre ni couleur relue, alors que le détour de mise à
    jour colorait le bon nom (trouvé par `world.Find`, qui suit l'alias). La scène garde maintenant
    le handle **de la copie locale**. De plus, un nom déjà coloré dont le texte a repris une autre
    couleur sans que le jeu le signale (perso refait, apparence appliquée : « appearance of Joueur3
    applied » 13 s après son arrivée) est recoloré à la mise à jour suivante. Toujours un seul nom.
  - `carry` « le corps est là où celui de l'hôte est tombé » : la correction de 7c70d5c **a bien
    tourné** (« stood up once to fall on the host's spot », 13:56:10) mais le corps est retombé
    exactement au même endroit, au millième près, comme lors de l'essai d'avant ce correctif
    (24659,9 / 167,0 / 10544,9). Le décalage mesuré de sa chute était nul (-0,1 ; -0,2) : il est tombé
    là où il était debout, pas à côté. Le corps de l'hôte se lit à 17,5 unités au-dessus du sol de
    son porteur (174,6 pour un porteur à 157,1 : hauteur de l'épaule) ; notre copie, téléportée
    debout à ce point dans une maison à étage, était posée par le jeu sur le navmesh le plus proche
    en 3D (l'escalier, 12 unités plus loin et 10 plus haut), puis tombait là, à chaque fois. Un corps
    posé après un portage est maintenant mis debout **à la hauteur du sol de son porteur** (sous le
    point de l'hôte) quand l'hôte le lit à plus de 4 unités au-dessus (`kc::motion::FallSpotHeight`,
    test unitaire). Nouveau journal quand les téléportations n'ont pas mis le corps à son point de
    chute (« falls N units from its fall spot … the game kept it at … ») ; le journal « puts … down »
    donne les positions du porteur et du corps.
  - `carry`, nouvel essai (14:50-15:05, 6/7, corps **encore** à 24659,9 / 167,0 / 10544,9) : les
    nouveaux journaux montrent la vraie cause. Dans l'image même du dépôt, le corps du client est à
    24660,0 / 157,0 / 10545,1 (« puts … down … body at ») : **là où il gisait avant d'être ramassé**
    (`spawnnpc 12 4` l'avait créé à 12 et 4 unités du perso 0 : 24647,96 + 12, 10541,1 + 4). La remise
    d'un corps posé par le jeu le replace là, et les téléportations ne le bougent plus (« falls 12.3
    units from its fall spot (24647.7,157.1,10543.6): the game kept it at (24660.0,157.0,10545.1) ») ;
    la hauteur corrigée plus haut n'y changeait donc rien. Pour un corps posé après un portage que les
    téléportations n'ont pas amené à son point de chute, le **nœud de scène du corps** (d'où part le
    ragdoll, `AnimationClass::setRagdoll`) est maintenant mis sur ce point, laissé 0,25 s, puis le
    corps tombe de là (`ReadyToFall`). Journaux « put down from a carry stays N units from its fall
    spot … its body node is put there » et « falls from its body node at … » ; « puts … down » donne
    aussi la position du nœud. Seulement pour les corps posés d'un portage (pas les chutes en
    combat). Pas encore vérifié en jeu.
- **Loin de l'hôte : marcheur qui va « dans tous les sens » et PNJ un étage plus bas chez un client
  (`farfps --clients 3`, 13/10, 42/44)**. Les deux échecs existaient **avant** la 0.3.4 : le relevé
  `farfps` du 12/10 à 02:51 (avant le gel des copies en sommeil, `2ec4fda`) avait déjà une marche à
  reculons (J2, 140°) et un PNJ 65,9 unités plus haut (J3, étages 9/10). Les persos en cause ne sont
  pas des copies en sommeil (à 250-535 unités du perso du client, envoyés). Pas une régression.
  - *Marche dans tous les sens* (J3, Bouclier d'Okran, 15 pas à reculons sur 46 pour un Grand Paladin,
    1 à 5 pour ses 9 suivants) : chez l'hôte, ce chef de patrouille « marchait » (vitesse 12 à 15,
    destination à 3800 unités) **sans avancer** : même place à 0,4 unité près sur 24 s (relevés de
    14:14:27, :37, :51), regard qui change d'un relevé à l'autre, ses 14 suivants piétinant autour. Notre
    copie marchait vers un point 60 unités devant, dans une direction tirée des petits écarts de
    l'hôte, donc différente à chaque échantillon, pendant que le rappel la ramenait : elle allait dans
    tous les sens dans un carré de 8 unités (relevé à 10 Hz). Maintenant une copie dont le perso de
    l'hôte « marche » sans avancer (moins de 1,5 unité nette en 1 s) reste debout à sa place, tournée
    comme lui ; elle repart dès que l'hôte avance (`kc::motion::NoteHeadway`). Journal « motion: the
    host's … gets nowhere: ours stands on its spot » / « walks on again ». Pourquoi le jeu de l'hôte
    bloque ce chef (loin de sa caméra) n'est pas cherché ici.
  - *Un étage plus bas* (J4, Poing d'Okran, deux Paladins 82,9 et 78,0 unités sous ceux de l'hôte
    pendant tout le test) : debout à l'étage chez l'hôte (groupe d'étage 10), au rez-de-chaussée chez
    le client (groupe 9). Ce sont des doublures créées par le client à la place de l'hôte : elles
    gardent le groupe du rez-de-chaussée, et un perso ne tient que sur les surfaces de son groupe ;
    chaque position écrite (même la téléportation, faite à chaque image au-delà de 15 unités, qui ne
    touche pas au groupe) la faisait retomber. Le groupe envoyé par l'hôte (message 71) n'était plus
    appliqué depuis la 0.3.0 (écriture brute remise par le jeu, plantage après une rafale) ; la
    version par la fonction du jeu (fix G7) était restée coupée. Elle est remise, seulement pour une
    copie qui est vraiment à un autre étage que celle de l'hôte (même place au sol à 10 unités près,
    plus de 20 unités au-dessus ou au-dessous, debout, ni au lit, ni en cage, ni portée ;
    `kc::motion::FloorFixWanted`) : `_setPositionAndTeleport(place de l'hôte, étage)` du jeu, jamais
    d'écriture brute ; vérifié 2 fois par seconde au plus, 2 s entre deux placements, 3 par valeur
    de l'hôte. Journal « floor: … put on the host's floor at its spot », « stands on the host's floor
    now », « … after 3 placements: left to the game ».
  - Commande `copyfix` (étages placés, marcheurs immobiles vus), écrite par `farfps`/`faraway` après
    le relevé de mouvement. Tests unitaires `TestMotion` (4c, 4d).
  - **Deuxième essai (`farfps --clients 3`, 13/10 15:05-15:12, 38/44)** :
    - *Marche à reculons (J3, même chef de patrouille)* : la détection marchait (« gets nowhere », 62
      lignes chez J3, `copyfix` stalled=233) mais basculait : l'hôte faisait 0,3 à 2,0 unités par
      seconde autour du seuil de 1,5, d'où « walks on again » / « gets nowhere » 0,05 à 0,3 s
      d'intervalle (15:12:00.497 à .921, pendant le relevé), chaque reprise lançant la copie vers un
      autre point à 60 unités : 5 pas à reculons (jusqu'à 6,1 unités en 0,1 s). Maintenant avec
      hystérésis : immobile sous 2 unités nettes en 1 s, repart seulement au-delà de 4 ; un arrêt de
      moins de 2 s garde l'état.
    - *Un étage plus bas (J4)* : le placement a bien eu lieu (`copyfix` floors=11) et a tenu pour
      d'autres copies (« stands on the host's floor now »), mais pour les deux Paladins en cause le jeu
      du client remet le groupe 9 dans la demi-seconde, trois fois de suite (« after 3 placements:
      left to the game », 15:09:18 et 15:09:33) : à cet endroit le jeu du client ne trouve pas de sol
      d'étage sous eux. Chez l'hôte ils sont hors de sa vue (`inview=0`), immobiles au même point
      d'un essai à l'autre (-9427,3 / 1523,4 / -77169,6), regard tourné vers le bas (y -0,45 et -0,6) :
      vraisemblablement la place de la sauvegarde, que la mise à jour réduite de l'hôte ne recalcule
      pas. **Non résolu** (à vérifier caméra de l'hôte sur place). Ces copies-là étaient en plus
      téléportées à chaque image (écart de 79 > 15) sans effet : une copie dont le jeu refuse l'étage
      ne suit plus que la position au sol (ni téléportation, ni rappel en hauteur).
    - *J2 Admag, 3,2 unités* : un marcheur exactement à la place x,z de l'hôte, 3,2 unités plus bas
      (sol 2/2, hors de la vue des deux jeux ; déjà +3,0 le 12/10). La hauteur d'un marcheur loin de
      la caméra de l'hôte est celle de sa mise à jour réduite, le client le pose sur son sol. Le test
      compare maintenant un perso debout au sol (3,0) et en hauteur à part (6,0 ; un étage fait ~80).
    - *FPS (144->125, 131->119, 174->152)* : **pas notre travail par image** : 0,61/0,68, 0,77/0,76,
      0,58/0,62 ms entre les deux fenêtres (+0,07 ms au plus), alors que l'image prend 0,8 à 1,1 ms
      de plus ; l'hôte, que ce code ne touche pas, perd aussi (112 -> 105 fps), et les quatre jeux ont
      le même à-coup dans la fenêtre « correctif » (pire image 163-179 ms, contre 18-43 avant). Toute
      la machine était plus lente cette fois (« ensemble » 163-189 fps contre 195 au premier essai).
      Retiré quand même : la téléportation à chaque image des copies dont l'étage est refusé.
  - **Troisième essai (`farfps --clients 2`, 13/10 16:12-16:17, 29/30 ; fps et étages passent)** :
    reste une marche à reculons chez J2 (Admag, `1:42:2835449344:5`, 131-132° pour 3,5, 2,0 et 0,9
    unités). Ce n'est ni l'hystérésis ni un virage en marchant : relevé à 10 Hz, la copie marche le
    long de -z, s'arrête (vitesse 0 à 0,7 s) puis glisse encore de 7 unités en convergeant (3,4, 1,9,
    0,8, 0,3…) vers un point fixe, la place où le perso de l'hôte s'est arrêté, alors que sa rotation
    dessinée est déjà celle de l'hôte, qui s'est retourné (-0,86 ; 0,51) en s'arrêtant. La copie,
    arrêtée un peu court, était tirée à reculons sur les dernières unités. Maintenant, tant qu'il lui
    reste plus d'une unité jusqu'à la place de l'hôte et que le regard de l'hôte pointe à plus de 90°
    de ce chemin, la copie regarde (et est dessinée) dans le sens où elle glisse ; arrivée, elle prend
    le regard de l'hôte (`kc::motion::SlideFacing`, `TestMotion` 4c2).
    **Pas relancé en jeu.**
    le relevé de mouvement. Tests unitaires `TestMotion` (4c, 4d). **Pas relancé en jeu.**
- **Test `workstations` « au moins un poste a des ouvriers » : cause trouvée, la disposition des
  postes (13/10 16:04, relevés `charstate` / `machine` ajoutés au passage précédent)**. Le chercheur
  (membre 3), envoyé au banc de recherche à 87 unités, est resté 2 min à x3 au même endroit
  (29585,10466) avec sa tâche (`tasks=1`), le banc à 0/1 (`flags=67` : bien au joueur). Les postes
  formaient deux rangées à **36 unités** l'une de l'autre (z 10460 pour l'hôte, z 10496 pour le
  client : le client posait autour de son propre perso tel que son jeu le plaçait), espacés de 60
  (fermes comprises) : une double rangée presque jointive, et le chercheur s'était arrêté dans
  cette bande, entre la cuisinière et l'établi. Les postes n'étaient pas atteignables, à chaque
  passage de ce test ; ni le mod ni le jeu (JOBS allumé, banc qui accepte un opérateur) n'étaient en
  cause. Les deux rangées sont maintenant posées autour du membre 0, à 300 l'une de l'autre, les
  postes à 90 d'écart. Non revérifié en jeu.
- **Tests du 13/10 15:05-15:32 sur la v0.3.4 fusionnée (`recruits` 21/21, `clientjobs` 21/21)** :
  - `animalshop` 9/11 : l'animal rejoint maintenant l'escouade (journal de l'hôte : « character
    editor of 1 character(s) (mode 1) not opened here », puis « a new squad character (Chèvre) goes
    to Coucoudz 2 »). Les deux échecs restants venaient du test. « l'animal appartient au client :
    propriétaire None » : le relevé avait bien `entity 74 1:1:3769653248:5:1589599616 owner=2`, mais le
    test cherchait l'entité par sa poignée dans un tableau rangé par netId (même erreur dans `pets`) ;
    nouvelle fonction `entity_of`. « positions identiques après l'achat : 10 » : dix PNJ qui
    marchaient (~15 unités/s), relevés à quelques dixièmes de seconde d'écart de chaque côté (4 à 9
    unités) ; le contrôle passait avant seulement parce que l'éditeur avait mis l'hôte en pause. Il
    porte maintenant sur l'escouade (l'animal acheté compris), à 3 unités.
  - `dialogue` 17/18, « une téléportation ferme la conversation : err no live world » : venait du
    test. Le mod a fermé la conversation à la téléportation (hôte, 15:31:54 : « conversation 2 ends: a
    character in it jumped away », `dialogs` n=0) ; le client chargeait la zone d'arrivée (image hors
    boucle du jeu) quand le test l'a interrogé, et `wait_open` prenait la réponse « err » pour une
    fenêtre fermée. Une réponse « err » n'est plus un état (`exp_dialogue`, `exp_npctalk`).
  - `workstations` 44/45 : JOBS était allumé partout (`modes` « ok 68 0 » pour chaque membre), donc
    **ce n'était pas la cause** avancée plus bas ; recherche toujours en file (1884, 0,00), aucun
    ouvrier même après un clic droit du chercheur sur le banc (148 unités). Lu dans le jeu :
    l'ajout d'un opérateur (`0x2B5BF0`) n'est refusé par un banc de recherche (`ResearchBuilding`
    vt 0x4E8, `0x2995E0`) que si la file de recherche est vide ; ce n'est donc pas ce refus. Cause
    toujours inconnue : le test écrit maintenant toutes les 15 s l'état du chercheur (`charstate`) et
    celui du banc (`machine`, drapeaux et opérateurs) pour la trouver au prochain passage.
- **Animal acheté par un client : il rejoint enfin l'escouade (test `animalshop`, en échec depuis la
  0.3.3, 13/10 9/10)**. Cause trouvée dans le jeu (désassemblage 1.0.68) : le `confirm()` de la
  boutique d'animaux, que l'hôte exécute pour l'achat d'un client, finit par
  `ForgottenGUI::showCharacterEditor(animaux achetés, mode 1)` (l'éditeur pour nommer ses animaux,
  appel en `0x6AB5A3`). Il s'ouvrait donc **chez l'hôte** : le jeu de l'hôte s'est mis en pause à cet
  instant (relevé `h_animalshop2` du 13/10 : `paused=1`, horloge arrêtée au moment de la vente), et
  la liste des persos du joueur, que `PlayerInterface` reconstruit à chaque image depuis les pelotons
  de la faction (`0x801E26`) mais pas pendant qu'une fenêtre bloquante est affichée (`0x80168A`), n'a
  plus bougé : la chèvre était bien dans le peloton du joueur (« player squad 1 ») mais jamais dans
  la liste (« player characters list 0 », squadanimals 0, absente des relevés). L'hôte n'ouvre plus
  l'éditeur pendant une vente pour un autre joueur (`NoEditorScope`, détour de
  `showCharacterEditor`, `hk_showEditor` ; journal « character editor of N character(s) (mode 1) not
  opened here ») ; l'animal garde son nom. L'achat de l'hôte pour lui-même ouvre l'éditeur comme en
  solo. Le journal « bought animal now » dit aussi si un éditeur est ouvert. Non revérifié en jeu.
- **Test `recruits` : « parler à deux : la conversation s'ouvre chez le client » (13/10 20/21, déjà
  en échec le 12/10)** : venait du test, comme `npctalk`. `talksel 0` visait le PNJ le plus proche
  (Ribs / « Lost Person », le 12/10 un Ninja) ; l'hôte a bien fait marcher Harp jusqu'à lui puis
  « the NPC has nothing to say ». Le test essaie maintenant des PNJ qui ont une conversation, par nom
  (Chef Voleur, Thief Leader, Marchand, Trader, puis le plus proche), 30 s chacun.
- **Test `clientjobs` : « nœud de fer plein (3/3) avec les persos de l'hôte » (13/10 13/14, 20/20
  au passage précédent)** : venait de la mesure. Le test a lu 3/3 puis, 0,1 s plus tard, 1/3 : quatre
  lectures à 0,1 s d'écart donnaient 3/3, 2/3, 3/3, 1/3 (les noms changeant), alors que la
  synchronisation des machines de l'hôte voyait 3/3 de 14:25:09 à 14:25:36. L'ensemble des opérateurs
  du jeu va et vient pendant que les mineurs travaillent ; le test prend la meilleure de 10 lectures
  sur 2 s. Pourquoi l'ensemble varie d'une image à l'autre n'est pas établi.
- **Test `workstations` : « au moins un poste a des ouvriers » (13/10 44/45, recherche en file, 0
  ouvrier)** : vient très probablement de la mise en place (l'état JOBS des persos de l'hôte n'a pas
  été relevé pendant la partie). Les persos de l'hôte n'avaient leurs postes qu'en travail
  **permanent** (`workjob`), que le jeu ne fait que si l'ordre permanent JOBS du perso est allumé
  (bit 64 de `modes`), ce que rien ne vérifiait pour eux ; la cuisinière et l'établi n'avaient rien à
  travailler (paille de blé refusée, « Ble » parti dans la sortie), la ferme du désert ne pousse pas.
  Le test allume JOBS pour chaque membre qui ne l'a pas, envoie un chercheur au banc de recherche
  par clic droit (comme `research`, dont le chercheur travaille) et écrit les `modes` dans le détail
  de l'échec. Non revérifié en jeu : si ça échoue encore, la cause est dans la recherche elle-même.

- **Noms au-dessus des têtes : le perso d'un joueur arrivé plus tard (test `map`, 13/10, 48/50)**.
  Chez le client 1, le nom du perso principal du client 2 (arrivé après lui) n'était ni à sa couleur
  (`tint=0`) ni encadré, de son arrivée (10:29:54) à la fin du test (10:30:41) : le jeu de ce client
  ne mettait pas à jour ce nom (aucun widget, donc ni couleur ni cadre possibles ; 2 cadres sur 3
  pendant tout ce temps, aucune recoloration de plus), alors que l'hôte et le client 2 le montraient
  bien. Le mod note maintenant chaque nom que le jeu met à jour (détour de
  `CharacterNameTag::<update>`) : un nom que le jeu ne met pas à jour depuis 30 images alors qu'il
  met les autres à jour, ou dont le widget n'est pas fait, est écrit par l'overlay (couleur du joueur,
  cadre pour le perso principal) ; toujours un seul nom. `mapscene tetes` dit pourquoi (`why=`), le
  journal « names above heads » compte ces noms. La réponse de `mapscene tetes` n'était **pas**
  coupée (~930 caractères, entière dans le journal du client) : seuls le journal du test (600
  caractères) et le détail des contrôles (300) la coupaient, cachant l'entrée fautive ; le test écrit
  maintenant la réponse entière, nomme les noms fautifs et réessaie 3 s. Pourquoi le jeu ne met pas à
  jour ce nom (perso recréé chez le client à l'arrivée du joueur) reste à voir en jeu.
- **Test `npctalk` : les étapes 2 et 3 visaient un autre PNJ (13/10, 12/14)**. Le PNJ de l'étape 1
  était « le 3e plus proche » (Chef Voleur) ; les étapes suivantes redemandaient « le 3e plus
  proche », recalculé depuis la nouvelle place du perso, à côté du Chef Voleur : Marchand Shinobi
  (ordre de secours) puis un Voleur (copie non liée ; l'hôte : « the NPC has nothing to say »).
  Aucune conversation ne s'est ouverte. Le mod a bien fait son travail (ordre envoyé, PNJ retrouvé
  par type et endroit) ; le test vise maintenant le même PNJ par son nom (`name:Chef_Voleur`).
- **Postes de travail, sortie de machine et portage : tests du 13/10 (`machineout` 5/6,
  `workstations` 43/45, `carry` sans bilan)**.
  - `machineout` « le client ouvre la cuisinière » : **pas une régression** du correctif de la boucle
    de sortie. La commande de test `containerreq` ne cherchait le conteneur que par son **nom
    affiché** (traduit : « Cuisinière ») et le test lui passait l'identifiant `1944-gamedata.base` :
    « err no container called 1944-gamedata.base », aucune fenêtre demandée. `containerreq`,
    `contake` et `conput` acceptent maintenant aussi l'identifiant du modèle. La cuisinière était
    de plus restée vide : « Ble » et « Wheat » ne désignent aucun objet du jeu en français, et
    `machinegive` ne visait que la première section de la machine (« no room »). `machinegive`
    essaie maintenant chaque section (et dit lesquelles en cas d'échec) ; les tests donnent la paille
    de blé par son identifiant (`1913-gamedata.base`). Si la cuisinière reste vide pendant le travail,
    le test le dit (« NOT TESTED » pour la partie fenêtre fermée) et y met de quoi prendre avant
    d'ouvrir.
  - `workstations` « tâches permanentes du client (87 et 88) » : le transport 88 sans cible ne
    devient jamais un travail (voir `clientjobs`) ; le test donne maintenant « suivre » (31) sur un
    perso de l'escouade et attend 87 et 31.
  - `workstations` « au moins un poste a des ouvriers » (0 partout, persos de l'hôte compris) : ce
    n'est pas le mod, les postes n'avaient **rien à faire**. Un perso ne s'installe à un poste que
    s'il y a du travail : aucune recherche en file, cuisinière vide (ci-dessus), établi sans tissu
    (« giveItem refused (inventory full?) » : le sac du membre 0 plein de ce qui n'était pas entré)
    ni commande, ferme de chanvre en plein désert (pousse -0,20). Le test met maintenant une
    recherche en file (demandée par le client, payable par les bancs, comme `research`), ne donne
    qu'un objet par poste et relève les ouvriers à chaque lecture pendant les N minutes, pas
    seulement à la fin.
  - `carry` : l'expérience n'appelait pas `summary()` (pas de ligne BILAN, code de sortie 0).
    Ajouté.
  - `carry` « le corps est là où celui de l'hôte est tombé » (12 unités à côté) : chez le client le
    corps posé est lâché debout, placé où est celui de l'hôte puis mis en ragdoll ; un corps qui
    s'effondre finit à 7-12 unités de ses pieds, et la correction existante (relever puis refaire
    tomber, décalage mesuré compensé) ne se déclenche qu'au-delà de 20 unités. Un corps **posé
    après un portage**, vu par un joueur, qui finit à plus de 5 unités (au sol, sans la hauteur) de
    celui de l'hôte est maintenant relevé **une fois** et refait tomber d'autant en deçà du point de
    l'hôte (`kc::motion::MayFineRelay`, même budget que les autres relevages) ; journal « lies N
    units from the host's body (its fall here shifted it …): stood up once to fall on the host's
    spot ». Un perso K.-O. en combat n'est pas concerné (pas de relevage visible). Le test lit les
    positions 10 s après la pose. Test `TestMotion` (posture).
- **TP admin d'un perso à terre : le corps ne revient plus à sa place d'avant (test `tpdown`, 13/10
  10:43 : 193,5 unités de l'hôte)**. Journal de l'hôte : le corps est relevé, téléporté, recouché…
  et « went back 200 away when it lay down again », deux fois. Lu dans le jeu (désassemblage 1.0.68) :
  `Character::ragdollMode` ne fait que mettre la demande en file ; quand elle est appliquée,
  `AnimationClass::setRagdoll` (`0x5B9A10`) **crée un ragdoll neuf à l'endroit du nœud de scène du
  corps** (`AnimationClass`+0xB0, `Node::getPosition` ; il ne le remet sur le perso que s'il lit
  `Vector3::ZERO`), et la position du perso suit ensuite celle du ragdoll (`0x7D16F0`). Or la
  téléportation du mod (`CharMovement::setPositionDirectionAndTeleport`) ne bouge pas ce nœud ; seuls
  `Character::setPosition` (`0x5CA680`, via `AnimationClass` `0x5B1B00`) et la remise d'un corps posé
  par son porteur (`0x5CCBA0`) le font. Le nœud d'un corps tenu debout pendant la TP restait donc là
  où il gisait. Maintenant le nœud est placé avec le corps à chaque téléportation d'un corps
  (`kenshi::PlaceBodyNode`, `Ogre::Node::setPosition` comme le jeu), revérifié juste avant de le
  recoucher ; même chose chez le client avant chaque chute d'une copie (`ReadyToFall`). Journal :
  « tp: … teleported, its body's scene node N units from there: moved with it », et la distance du
  nœud dans « went back ». Le test mesure depuis l'endroit visé (le membre 0 au moment de la TP) et
  recopie les lignes « tp: » de l'hôte.
- **Test `suite`, « TP admin : le perso du joueur arrive près de l'hôte » (30,1 pour 30, 13/10
  08:07)** : venait du test. Le perso du client est arrivé à 22 unités de l'endroit visé (debout, « walk
  and orders ended »), puis le membre 0 de l'hôte s'est lui-même écarté de 51 unités du nouveau venu
  (posé à 7,5 unités de lui). La distance se mesure maintenant depuis l'endroit visé.
- **Test `prospect` : « l'hôte prospecte comme en solo » (déjà en échec le 12/10 à 02:37 sur
  `night-fixes`, donc pas une régression de la 0.3.4)** : le jeu refuse PROSPECT à l'intérieur (« I need
  to go outside to do this. ») ou hors du sol ferme (« I need to be stood on solid ground. »)
  (`checkPlayerOrderForProblems` `0x5D1940`, tâche 150), et dans `kctest_town` le membre 0 de l'hôte
  (Su) est dans un bâtiment (groupe d'étage 9 dans les relevés du 12/10 et du 13/10). Le test fait
  prospecter l'hôte avec un de ses persos qui est dehors (groupe d'étage < 9, rob). Non revérifié en
  jeu. Reste : chez le client, la fenêtre s'ouvre sans ces vérifications du jeu (on peut prospecter à
  l'intérieur).
- **Test `animalshop` : « l'animal rejoint l'escouade » (déjà en échec le 12/10 à 02:36, 03:03 et
  03:12, noté « reste ouvert » dans la 0.3.3 : pas une régression)**. L'achat est fait par le jeu de
  l'hôte (cats prises, même argent partout) et la chèvre est bien dans une escouade du joueur juste
  après (pas de remboursement), mais elle ne devient jamais un perso de l'escouade pour le mod (pas de
  « a new squad character … goes to ») et n'est plus dans les relevés 10 s plus tard. Cause pas
  trouvée ; l'hôte écrit maintenant où est chaque animal acheté juste après la vente (« animal shop:
  bought animal now … player squad, player characters list, at … », recopié par le test), et la
  session dit quand une recrue ou un animal attendu n'est jamais arrivé (« never joined the player's
  squads within 60 s »). À vérifier avec un vrai vendeur d'animaux.

- **Travaux des persos d'un client : test `clientjobs` corrigé (13/10, 15/17)**. Les deux échecs
  venaient du test, pas du mod. (1) « une place libérée est reprise » (2/3 au bout de 60 s) : le perso
  du client est bien allé **de lui-même** au filon quand le mineur de l'hôte est parti (1904 -> 125
  unités sans aucun ordre : son travail permanent a été repris par le jeu de l'hôte), mais il s'est
  battu dès son arrivée (défense, robustesse, esquive et combat de masse en hausse de 08:18:25 à
  08:19:01 : les combats de la ville autour du filon, tout le monde en mode dieu, le combat ne finit
  pas) ; le jeu fait passer le combat avant les travaux, comme en solo. Le test vérifie maintenant à
  part que le perso y va de lui-même, attend la place 120 s et dit « NON CONCLUANT » au lieu d'un
  échec si le perso s'est battu. (2) « deux travaux pour tester le panneau » : le transport 88 sans
  cible ne devient **jamais** un travail dans le jeu (liste 1 -> 1 chez l'hôte comme chez le client,
  et déjà dans les parties `workstations`) ; le second travail est maintenant « suivre » (31) sur un
  perso de l'escouade, que le panneau Tâches accepte (`exp_jobs`). Le mod disait déjà au joueur
  « Aucun travail permanent ajouté ». Le reste (Maj + clic droit, liste chez l'hôte et le client,
  messages, JOBS, clics répétés, recrue à personne, perso d'un autre joueur) a réussi en jeu.

- **Parler à un PNJ (partie du 12/10, Geoffrey puis rob1 bloqués jusqu'à une resync)** : chez un
  client, le clic droit sur un PNJ ne proposait « parler » que si le jeu **du client** trouvait une
  réplique « parle-moi » sur sa copie du PNJ ; sinon le clic ne faisait rien, sans ordre ni message
  (l'hôte n'a reçu aucun ordre « parler » d'eux pendant ces minutes). Maintenant un clic droit qui ne
  donne aucun ordre est repéré (crochet de `PlayerInterface::<personnage sous le curseur>`,
  `0x7FB410`) : le journal du client dit pourquoi et ce qu'il sait du PNJ (lié à l'hôte, piloté par
  l'hôte, captif ici ou chez l'hôte, contrôle « parler » du jeu ici, distance), et pour un PNJ debout
  l'ordre « parler » part quand même : l'hôte décide. L'ordre nomme aussi le PNJ par son type et sa
  place : une copie non liée au PNJ de l'hôte est retrouvée chez l'hôte. Un ordre « parler » qu'aucun
  perso à soi ne peut donner est écrit dans le journal du client.
- **Conversation lancée par un PNJ fermée aussitôt (contrôle de contrebande, rob1 23:11, nass4
  22:24)** : le jeu de l'hôte décompte la minuterie de la réplique dite en bulle et ferme ou fait
  avancer seul la conversation quand elle tombe à zéro ; en solo la fenêtre met le jeu en pause,
  chez l'hôte la conversation d'un client non. L'hôte garde maintenant cette minuterie tant que le
  joueur a des réponses à choisir (5 min au plus), et une conversation qui continue dans le dialogue
  de l'autre perso garde le nom du PNJ. Une conversation que le jeu ferme quand même avant toute
  réponse est décrite dans le journal de l'hôte.
- Tests : `TestTalkRules` (unitaire), expérience `npctalk`, commande `talkclick`, `held=` dans
  `dialogs`. À vérifier en jeu.
- **Correctif après l'essai du 13/10 (`dialogue` 14/16, `npctalk` 13/14)** :
  - *Une TP ne fermait pas la conversation* : ouverte à 08:26:30.450, TP de 2100 unités à .865,
    toujours ouverte 14 s plus tard (c'est la TP de retour qui l'a fermée). Les premières positions
    d'une conversation n'étaient relevées qu'au balayage suivant (toutes les 0,5 s), ici après la TP :
    le saut n'était pas vu, et l'écart de départ mesuré incluait déjà la TP. Une nouvelle conversation
    est maintenant placée dès l'image où l'hôte la voit.
  - *Un combat ne fermait pas la conversation* : aucune règle « combat » dans le balayage (seulement
    à terre, mort, TP, éloignés). Un combat d'un des deux persos commencé après l'ouverture, vu sur
    deux balayages de suite, la termine maintenant (un combat déjà en cours à l'ouverture non).
  - La minuterie gardée pendant que le joueur choisit n'y était pour rien : `held=0` pendant tout
    l'essai (la réplique du Chef Voleur dure plus de 20 s, jamais sous les 3 s gardées).
  - *`npctalk`, « un clic droit du client ouvre une conversation »* (test) : les 5 PNJ les plus
    proches ont bien reçu l'ordre « parler » chez l'hôte (5 vérifications réussies), mais aucun n'a de
    conversation à réponses (« the NPC has nothing to say », le Chirurgien ne fait que parler). Le test
    essaie maintenant les 3 plus proches puis le Chef Voleur et un marchand par leur nom. Le mod n'est
    pas en cause.
  - Tests unitaires : `TestTalkRules` (TP, éloignement, combat après/avant l'ouverture). Pas relancé
    en jeu.
Pas de changement de protocole (36) : se connecte aux 0.3.3 (un hôte 0.3.3 garde le défaut côté hôte,
les clients 0.3.4 s'en protègent seuls).

- **Viande d'une machine qui « bouge » toute seule (12/10, A_FAIRE point 6) : corrigé dans le code, à
  tester en jeu.** De 22:40 à 00:12, les trois clients demandaient à l'hôte toutes les 3 s les mêmes
  déplacements de viande dans la sortie d'un Feu de Camp (« x1 out -> out 0,0 », « x2 out -> main
  2,2 »), sans réponse. Cause : l'hôte ne suit une machine qui n'est pas aux joueurs (un feu de camp
  du monde) que tant qu'un perso de joueur la travaille ; quand il l'oubliait, il effaçait l'entité de
  son inventaire **sans le dire** aux clients, qui la gardaient (« un inventaire de machine reste
  synchronisé ») avec ses derniers objets. Chaque client comparait ce vieil état au contenu réel du
  feu et demandait un « déplacement » au nom d'une entité inconnue de l'hôte, qui l'ignorait en
  silence ; quand le feu était repris, une seconde entité s'ajoutait (deux demandes par client à
  partir de 22:54). L'hôte n'a **rien** mis dans le perso de Geoffrey par ces demandes : les seuls
  déplacements « out -> main » faits l'ont été pendant que Geoffrey avait le feu ouvert, un par geste
  (22:54:47-22:55:34). Corrigé :
  - l'hôte envoie `Unbind` quand il oublie l'inventaire d'une machine (ou un contenant) ;
  - le client oublie une copie d'inventaire de machine que l'hôte ne nomme plus dans sa liste de
    machines depuis 12 s (pour un hôte 0.3.3), et la vieille copie d'un contenant que l'hôte suit de
    nouveau sous un autre netId ;
  - **un coffre ou une machine ne compte comme touché par le joueur que pendant que sa fenêtre est
    ouverte** chez lui : fermé, rien de ce qui y change (production locale, autre rangement, état plus
    récent de l'hôte) ne devient une demande ; il revient à l'état de l'hôte ;
  - l'hôte écrit ce qu'il fait de chaque demande sans effet : « inventory move from player N not done
    (nothing to do) … which the host does not follow », « client item move not done: … inventory not
    found here » (en plus de « done » et « refused »).
  Test unitaire `TestMachineInventoryClosed`, expérience `machineout`.
- **Membre coupé « lâché puis ramassé » en boucle (12/10 22:13:46-49, point 3) : corrigé dans le code.**
  Personne ne lâchait ni ne ramassait rien : un membre (`101-limbs.mod`) tranché pendant un combat
  roulait sur une pente ; le relevé des objets au sol de l'hôte le réannonçait à chaque relevé (0,5 s)
  parce qu'il avait bougé de plus de 30 unités depuis le précédent, et chaque client le retirait puis le
  recréait. Un objet annoncé qui roule n'est plus réannoncé qu'une fois immobile (moins de 4 unités
  entre deux relevés), là où il s'arrête (`plugin/ground.cpp`, compteurs `scanMoved`, `scanRolling`).
- **Fenêtre de commerce sur un marchand doublure (point 4) : elle reste ouverte.** Quand le vrai PNJ de
  l'hôte remplace la doublure du marchand chez le client (environ 1 s après l'ouverture), la fenêtre
  passe sur le vrai marchand au lieu de se fermer : le seul pointeur que la fenêtre garde vers le
  marchand est `ShopTrader`+0xC0 (lu au désassemblage, voir MOTEUR section 7), plus son `hand` dans la
  table des partenaires d'échange ; les deux sont réécrits avant que la doublure soit détruite, puis
  vérifiés comme le jeu les lit (`getNPCTrader`). La fermeture reste le repli (marchand ambulant qui
  vend depuis les sacs de son escouade, autre fenêtre sur la doublure, vérification ratée). L'argent
  du marchand : vérifié 1 s après l'ouverture et remis sur le vrai marchand quand il change (« trade:
  the merchant's cats here … set to the host's … », « DID NOT STICK » s'il ne tient pas). Expérience
  `tradeswap`.
Pas de changement de protocole (36, comme la 0.3.3).

- **États des persos chez les clients (branche `fix-char-states`, bugs du 12/10 au soir ; corrigé dans le
  code et les tests unitaires, à tester en jeu)** :
  - **TP admin : plus de course vers l'ancienne destination** (A_FAIRE point 10) : chaque perso
    téléporté reçoit le « stop » du jeu (`kenshi::StopCharacter` : `CharMovement::halt` puis la file
    d'ordres vidée par `AITaskSystem` `0x5074A0`, nouvelle fonction `FnAiTasksClear`, ce que fait le
    bouton stop du jeu), ses travaux permanents restent ; arrêté une seconde fois s'il marche encore au
    contrôle 0,3 s après. Avant : « TP moi » de nass4 (23:11:30) suivi d'une course vers l'ancienne
    destination, échecs de `suite` et `tpdown` (193 à 597 unités). Expérience `tpstop`.
  - **Copies gardées hors des lits et des cages** (point 11, Meow) : toutes les 2 s, chaque copie que
    l'hôte ne tient dans rien est sortie du lit ou de la cage où elle serait encore chez le client
    (`IWorld::EnsureFree`), pas seulement quand le « free » arrive, avec une ligne si elle y reste.
    Cause trouvée pour Meow : rob l'a portée hors de son sac de couchage (23:18:52) ; l'étape
    d'attache du portage (`0x5CED90`) la sortait déjà du lit chez les clients (`setInBed(false)`), d'où
    l'absence de « out of its bed » au « free » de 23:18:53 ; elle a été posée 3 s plus tard avec
    « ragdoll 1 ». Rien dans le journal ne montre ce qui la cachait ensuite : voir le point suivant.
  - **Corps posé, copie debout chez l'hôte** (point 12, « le nom bouge mais pas le corps ») : un corps
    n'est plus tenu pour « porté » chez le client que tant qu'un perso local le porte vraiment (avant,
    une marque restée l'empêchait à jamais d'être relevé ou déplacé) ; un corps posé qui joue encore
    l'animation « porté » 2 s après la perd ; une copie que l'hôte a debout et qui reste à terre après 3
    relevages est signalée avec ce qui la tient ; les lignes de portage nomment porteur et corps.
    Commande `posturecheck` (copies debout chez l'hôte mais couchées, au lit ou « portées » par
    personne ici), expérience `bedfree`, point ajouté à `carry`.
  - **Copies en sommeil figées** (point 13, bandits près de nass4) : un PNJ que l'hôte n'envoie plus
    reste là où l'hôte l'avait en dernier : ni destination ni combat (son dernier état disait « marche
    vers X » ou « combat Y », que la mise à jour lointaine du lot 3b exécutait), arrêté s'il marche,
    remis en place au-delà de 8 unités (`IWorld::HoldStill`). Le gain de FPS reste (toujours la mise
    à jour lointaine). Commande `dormant`, point ajouté à `farfps`.
  - **Corps disparu chez rob** (point 2) : la copie du bandit à terre, suivie dans une escouade locale
    de bandits inconnus de l'hôte (22:11:11), a quitté le jeu de rob une seconde après le retrait d'un
    de ces inconnus (22:16:51-52, sans destruction vue) ; un inconnu dans la même escouade locale
    qu'une de nos copies n'est plus retiré (« not removed: … in a squad here with our copy »). Chaque
    copie revenue après une absence est dite avec sa durée (« back here (made) 2.1 s after »), une copie
    pas revenue 20 s après l'est une fois (« still not back »). Les délais 2 s, 10 s, 30 s, 1 min ne
    concernent que les doublures que nous avons créées ; une copie venue de la sauvegarde est
    recréée dès 1,5 s.
  - Tests unitaires : `TestCaptives` (copie au lit alors que l'hôte l'a libre), `TestPerPlayerStreaming`
    (PNJ en sommeil figé, retour d'une doublure dit). 54907 vérifications.
- **Travaux des persos des clients, comme en solo (A_FAIRE 12/10 point 5, rob : « la gestion des
  tâches n'est pas au point ») : corrigé dans le code, à tester en jeu (`coop_test.py clientjobs`).
  Pas de changement de protocole (toujours 36).**
  - **Cause trouvée** : en solo, un clic droit devient un travail permanent quand **Maj** est tenue ;
    c'est `newPlayerTaskSelectedCharacters` qui lit la touche lui-même (octet `0x2133449`). Le client
    n'envoyait jamais sa touche (`shift` toujours faux pour cette voie) et l'hôte exécutait l'ordre
    avec la touche **de son propre joueur** : le travail d'un client n'était permanent que si l'hôte
    tenait Maj à cet instant (nass4 22:11, Geoffrey 22:48), jamais pour rob. Maintenant le client lit
    sa touche, l'envoie (`Command.shift`, déjà dans le message) et l'hôte exécute l'ordre avec elle,
    puis remet la sienne (`kenshi::ScopedShift`). Le perso du client montre le travail tout de suite
    dans son panneau Tâches. Maj + clic droit sur un stockage donne le travail « ranger » (124) au lieu
    d'ouvrir le coffre, comme en solo.
  - **Bouton JOBS** : il ne crée pas de travail (il dit si le perso fait ceux de sa liste). Son état
    change maintenant tout de suite chez le client et y reste 3 s malgré la synchro de l'hôte : le
    second clic de rob (2 s après le premier) repartait de l'ancien état de l'hôte.
  - **Machine pleine** : le jeu met quand même le travail permanent dans la liste (vérifié au
    désassemblage, `OrdersReceiver::addJob`) ; l'hôte fait pareil pour un client.
  - **Le joueur sait pourquoi rien ne se passe** (chat) : travail permanent ajouté mais toutes les
    places prises, rien à faire pour l'instant, JOBS éteint, déjà dans sa liste ; ordre ponctuel sur
    une machine où son perso travaille déjà (le cas de rob : son perso minait déjà le filon, 1 place,
    et ses 60 clics ne disaient rien), pleine ou sans travail. Les messages disaient « bouton JOBS,
    puis clic droit » : faux, c'est « Maj + clic droit ».
  - **Clics répétés** : le même ordre part à l'hôte au plus une fois par seconde (rob : 60 en 10 s).
  - **Panneau Tâches** : une croix ou un glisser du client ne revient plus en arrière le temps que
    l'hôte le fasse ; remise dans l'ordre de l'hôte juste même si une cible manque chez le client ;
    glisser « à la fin » (-1) accepté par l'hôte. Les tâches gardées en attendant l'hôte suivent le
    perso même quand son handle local diffère de celui de l'hôte.
  - Commandes de test : `workreq <i> <sid> shift`, `jobmove`, `whoseidx`, `toastlast` ; test unitaire
    `TestClientJobs` ; expérience `clientjobs`.
- **Noms des persos à la couleur de leur joueur (branche `fix-nametags`, à tester en jeu).** Le repère
  de l'overlay (curseur + nom du joueur) s'affichait par-dessus le nom que le jeu écrit déjà au-dessus
  de la tête : deux noms l'un sur l'autre. Le repère est retiré ; c'est maintenant **le nom du jeu
  lui-même** (`CharacterNameTag`, son texte `NameText`) qui prend la couleur du joueur, pour **tous**
  les persos de ce joueur (plus clair quand le perso est sélectionné, à la place du blanc du jeu). Le
  **perso principal** de chaque joueur a en plus un **petit cadre** à sa couleur autour de son nom,
  dessiné par l'overlay. La couleur est remise juste après la mise à jour du nom par le jeu (détour
  de `CharacterNameTag::<update>`, `0x6E8100`) : pas de clignotement ; un perso qui n'est plus à un
  joueur, ou le réglage éteint, rend au nom la couleur du jeu. Si le jeu n'affiche pas les noms
  (option « Afficher les noms » éteinte), l'overlay écrit lui-même le nom du perso (pas celui du
  joueur) à la couleur du joueur, encadré pour le perso principal : jamais deux noms. Chaque machine
  colore les persos de tous les joueurs (flux de carte de l'hôte). Réglage : « Noms des persos à la
  couleur de leur joueur » (`head_markers`, section « Affichage »). Protocole inchangé. Tests :
  `TestNameTags` (unitaire), `mapscene tetes` (`label=`, `tint=`, `main=`, `frame=`, cadres lus),
  contrôles « tetes : … » de l'expérience `map`.

## v0.3.3 (12 octobre 2026)

**Protocole 36 : tout le monde doit installer cette version** (elle ne se connecte pas aux 0.3.1 et
0.3.2). Elle contient tout ce qui est dans la 0.3.2 (ci-dessous), plus les travaux de la nuit du 11 au
12 octobre : boutique d'animaux et prospection chez les clients, fermes et postes de travail pour les
persos de chaque joueur, recrues des dialogues, caméra et mode construction sans pause, FPS loin de
l'hôte. Les points sur les sacs à dos ci-dessous sont déjà dans la 0.3.2.

- **Tests en jeu de la nuit du 12/10 (fusion des 6 lots)** : sacs à dos sans plantage ni inventaire abîmé (2 min de minage à x3), recrues données avec G et rangées sans écart, caméra libre, pas de pause du mode construction, journaux calmes, attaquants vus de loin, prospection chez le client, achat d'animaux chez le client (fenêtre, vente par l'hôte, argent identique). Corrigé pendant ces tests :
  - **déplacement vers un inventaire plein** : l'hôte vérifie maintenant qu'il reste une case avant de sortir l'objet de sa place (`FitsIn`) ; avant, l'objet était sorti puis remis ;
  - **objet neuf refusé** (commandes de test `giveitem`, `machinegive`) : détruit au lieu de rester tenu par personne ;
  - **achat d'animaux** : les animaux cochés chez le client sont envoyés avec le handle de l'hôte (ses copies locales ont le leur : « chosen animal(s) unknown to the session ») ; si le jeu prend l'argent sans faire entrer l'animal dans l'escouade, l'hôte rend l'argent et le dit ;
  - **sac de marchand (12/10 au matin)** : les Matériaux de Construction ne s'empilent pas dans un sac ordinaire (1 par case), mais si dans un « Traders Backpack » (bonus de pile minimum 2, ×3 : piles de 6). Le test portait un sac ordinaire ; il porte maintenant un Traders Backpack Medium (`wearbag N 1498-gamedata.base`, qui retire l'ancien sac). Le bonus est lu pareil chez l'hôte et le client (nouvelle commande `bagbonus N`), et les fusions 1 sur 1 et pile entière marchent ;
  - **objet posé sans case précise** (transferts de l'atelier, commandes de test) : il remplit d'abord les piles du même objet qui ont de la place, comme `addItem` du jeu ; avant, sac sans case libre mais avec une pile de 1 sur 6 = « no room ». Test `backpack` : 40/40.
- **Reste ouvert après la nuit** : plantage de l'hôte (`kenshi_x64+0x6d1c7`, lecture d'un texte de l'objet) juste après qu'un test a créé de l'eau (« Eau »/« Water ») par commande de debug et l'a donnée à un perso : chemin de test seulement, l'eau est retirée du test `workstations` ; un animal acheté à un vendeur créé par commande n'entre pas dans l'escouade (le jeu refuse ces chèvres de test) : à vérifier avec un vrai vendeur d'animaux.

- **Plantages de l'hôte avec les sacs à dos (11/10, 23:04 et 23:48) : corrigé dans le code, à tester en
  jeu.** Un client qui lâchait au sol un objet de son sac : l'hôte le faisait lâcher par l'inventaire du
  sac, dont le jeu fait retirer l'objet par l'inventaire **du perso**, qui n'a pas la section du sac.
  L'objet arrivait au sol mais restait dans la liste et la section du sac (lâché deux fois, « cannot place
  … at » chez les clients, l'objet qui revient). Une fois cet objet détruit (ramassé et fusionné), l'IA de
  transport de ressources (`getResourceItems`, via `hasABackpackOn`) ou le calcul du poids de la section
  lisait un objet libéré : plantage. Désormais chaque opération de l'hôte (déplacement, échange, dépôt,
  achat et vente, reconstruction chez les clients, plan lu) passe par l'inventaire qui **possède**
  l'objet (celui de la section qui le tient), vérifie que cet inventaire retrouve la section avant tout
  retrait, sort l'objet du sac avant de le faire lâcher par le porteur, et ne détruit un objet qu'une fois
  absent de toute liste (`plugin/kenshi.cpp` : `HomeOf`, `TakeFromHome`, `DropOut`, `DisposeTaken`).
- **Contrôle et réparation des inventaires** : commande de debug `invcheck [all] [repair]` et
  `invcheck each 1` (contrôle après chaque opération) ; en partie, un contrôle toutes les 3 s des persos
  des joueurs et de leurs sacs répare ce qui reste abîmé deux fois de suite (objet libéré encore listé,
  objet parti mais encore dans la liste ou la section), ce qui protège aussi les sauvegardes déjà
  abîmées. Ligne `INVCHECK sweep` dans le journal.
- **Piles dans les sacs (3c)** : une unité posée sur une unité du même objet s'empile aussi chez l'hôte
  (avant : seulement à partir de 2, l'unité prenait une autre case et le sac se remplissait chez l'hôte
  seul, puis « no room »). La règle est celle du jeu (`Item::isStackable`, avec le bonus de pile de la
  section) ; ce qui ne s'empile pas (armes, armures, sacs) échange toujours sa place. Même règle pour
  les achats. Test unitaire `TestStackMerge`.
- Tests : expérience `backpack` (`coop_test.py`), commandes `baginv`, `invitems`, `wearbag`, `objjob`,
  `uidrop bag:N`.
- **Recrues des dialogues absentes chez les clients (Streak, Stone).** Les recrues de Dialogue.mod sont
  des modèles « uniques » : la fabrique du jeu (`createRandomCharacter` 0x5836E0) refuse d'en faire un
  second quand le monde l'a déjà, ce qui est le cas chez un client. `CreateCharacter` lève ce drapeau le
  temps de l'appel. L'hôte envoie aussi l'apparence (race comprise) de chaque nouveau perso d'escouade.
- **Recrues sans portrait (Nei, Skinner, Kihaku).** Un membre d'une escouade de l'hôte dont la copie est
  encore un PNJ chez un client entre dans la faction du joueur à la prochaine mise à jour des
  escouades, quels que soient l'ordre et la vitesse des dons (G) ; un échec réessaie après une minute.
- **Stone pris pour la copie d'un autre perso de l'hôte** (ordres de nass4 refusés, ~900 lignes par
  minute). Le suivi d'un perso dont le jeu a changé le handle ne prend plus jamais un perso qui est
  lui-même un perso de l'hôte ; un tel lien déjà fait est défait. Refus d'ordres limités à 3 lignes
  par 10 s.
- **Perso d'éditeur abandonné devenu recrue d'un autre joueur (rob1).** Le perso fait pour l'éditeur
  d'un joueur n'est jamais pris pour une recrue ; il est supprimé s'il reste hors des escouades quand
  son joueur part sans l'avoir fini ou qu'un nouveau perso est fait pour lui.
- **Parler avec deux persos sélectionnés** : un seul ordre « parler » part ; chez l'hôte, l'autre perso
  du même joueur est refusé sans message (plus de « X est occupé » dans sa propre conversation, plus
  de refus comptés ni de rafales dans le journal).
- Tests : `coop_test.py recruits` ; commandes de debug `squadcheck`, `talksel`, `recruitnpc`,
  `squadidx`, `dialogs same=`, `spawncreature … unique=`. Test unitaire : un seul ordre « parler »
  pour deux persos sélectionnés. Rien n'est encore vérifié en jeu. Protocole inchangé (35).
- **Caméra d'un client « qui va partout »** (A_FAIRE 3l) : le perso que suit la vue des étages
  (`PlayerInterface+0x270`) est celui que la caméra suit, et le jeu le lâche dès que le joueur bouge la
  caméra (`CameraClass::move` / `teleport` → `stopTrackCharacter`). Le mod le reprenait toutes les
  0,5 s. Il n'est plus posé qu'une fois par monde chargé ; les toits restent cachés par `UpdateRoof`.
- **Le mode construction de l'hôte ne met plus tout le monde en pause** (3n) : détour de
  `PlayerBuildMode::activate` (`0x4E65E0`) ; avec d'autres joueurs, `togglePause(false)` juste après,
  qui garde la pause si l'hôte l'avait mise lui-même. Celui d'un client ne touche que son jeu.
- **Portes en boucle** (3h) : « Storm Shack Door stays 0 (host 3) » toutes les 3 s chez tous les
  clients. 3 = « en fermeture » : fermée chez le client, c'est juste (`DoorState::sameSettled`) ; une
  porte que le jeu du client refuse est laissée après 3 essais jusqu'au prochain changement de l'hôte.
- **Journal « captivity from the host »** (3h) : chaque changement n'est écrit qu'une fois (même
  renvoyé par les rafraîchissements de 5 s), avec le meuble, 6 lignes par 10 s au plus.
- **Combats suivis de loin** (3g, sans changer le protocole) : qui combat un perso envoyé à un joueur
  lui est envoyé aussi, et un client recrée, même loin de ses persos, un PNJ qui combat un membre
  d'escouade : en suivant le combat d'un autre joueur, on voit ses attaquants.
- **Hauteur des poses des clients** (3o) : pas un bug. La hauteur d'une pose est relative au sol
  (`createBuildings` retire `getTerrainWithWaterHeight`, `0x4D7512`) ; le journal du client le dit.
- Tests : unitaires (portes « en fermeture », captivité répétée et en rafale, attaquants envoyés et
  recréés) ; expériences `camfocus`, `buildpause`, `quietlogs`, `attackers` ; commandes de debug
  `buildmode`, `camerafree`, `logcount`, `doorsknown` (`givenup=`).
- **Travaux de base pour les persos de chaque joueur (agriculture, eau, cuisine, artisanat,
  recherche, énergie, transport)** — corrigé dans le code, à tester en jeu (`coop_test.py
  workstations`). **Protocole 36** (`MachineState` : drapeau `kMachFarm` et `CropState`).
  - **Le fermier de rob « jamais compté » (11/10, Ferme Légumes L)** : ce n'était pas une perte
    d'ordre. L'ordre « utiliser une machine » (87) arrivait et tournait chez l'hôte, mais une ferme
    dont les cultures poussent n'a pas besoin de travail : `FarmBuilding::dontNeedWorkRightNow`
    (vt 0x4E8, `0xF6E00`) est vrai et `UseableStuff::tryOperate` (`0xF8030`) refuse alors tout
    opérateur ; la tâche ponctuelle du clic droit s'arrête aussitôt, sans « workers » (le banc de
    recherche de Geoffrey faisait pareil tant que rien n'était en file). Le joueur reçoit maintenant
    la raison en français avec la réponse de l'hôte (« les cultures poussent… donne-lui ce travail
    en permanent (bouton JOBS) »), ou « toutes les places sont prises (n/m) ».
  - **Cultures identiques partout** : la ferme d'un client poussait toute seule, mais seuls les
    fermiers de l'hôte récoltent et seul son jeu replante ; chez le client la ferme restait mûre,
    puis pourrissait et se replantait à part. L'hôte envoie maintenant l'état des cultures (étape,
    plants récoltés, pousse, pourriture, nettoyage) ; le client récolte les mêmes plants
    (`FarmBuilding::destroyAPlant`), replante quand l'hôte replante (`resetFarm`) et cale la pousse.
  - **Machines de l'hôte « introuvables » chez un client (nass4, 23:48)** : un bâtiment posé par un
    joueur est retrouvé d'abord par son netId de pose (le client a pu le construire un peu à côté),
    puis par type et endroit. Le journal dit pourquoi une machine manque (« not loaded here yet » :
    loin de nos persos, zone pas chargée — c'était le cas de nass4 ; « NOT built here? » sinon), une
    fois par machine.
  - **Recrue « à personne »** : n'importe quel joueur peut lui donner un travail de base
    (`kc::IsBaseWorkTask` : machines, fermes, transport, construction, médecin, secours, tourelles),
    au clic droit comme en permanent ; toujours refusé, avec message, sur le perso d'un autre joueur.
  - Commandes de debug `machines`, `crops`, `storage`, `workjob` ; test unitaire
    `TestWorkstationOrders` (règles de chaque tâche des postes), `TestWorkshop` et `TestSquadWindow`
    étendus ; expérience `workstations`.
- **FPS des clients quand les joueurs s'éloignent (3b).** Chez un client, les persos de l'hôte qu'il ne reçoit plus (loin de ses persos, en sommeil) étaient quand même mis « dans la vue » par `hk_charUpdateView` : déplacement complet et animation à chaque image pour ~200 persos que personne ne voit (Geoffrey 133 → 46 fps, nass4 jusqu'à 10). Ils sont laissés à la mise à jour lointaine du jeu (`IWorld::SetFarCharacters`, `HookView::farAway`) ; jamais un membre d'escouade, et un perso renvoyé par l'hôte repasse en complet dans la même image. Mesure : ligne `sync perf` par minute (notre travail par image, fps, persos près / loin), `host perf` chez l'hôte, commandes `perf`, `standins`, `farleave`. Test `farfps --clients 3`. Pas de changement de protocole (35). À tester en jeu.
- **Murs traversés loin de la caméra de l'hôte (3f).** Désassemblage : ni le navmesh ni la physique des bâtiments ne dépendent de la caméra. La mise à jour complète du déplacement (murs du navmesh) exige un pas de moins de 0,5 s de temps de jeu (`0x660A47`) : chez l'hôte, un pas plus long d'un perso près d'un joueur client (gel de l'hôte à x2/x3) est coupé en morceaux de 0,4 s (`hk_moveUpdate`). Compteurs `movestats`, `navcheck`, `navat`, `wallsnear` ; test `farwalls`. À tester en jeu.
- **Achat d'animaux par un client (A_FAIRE 3i).** « T'as des animaux à vendre ? » dans la
  conversation d'un client ouvrait la fenêtre d'achat d'animaux sur l'écran de l'hôte. L'action de
  dialogue (`CHOOSE_ANIMALS_BUYING`) fait un gestionnaire d'achat (`0x6A9440`) puis une
  `CharacterTradingWindow` (`0x72A880`) : l'hôte note le gestionnaire fait pour le perso d'un autre
  joueur, garde la fenêtre cachée et envoie l'offre à ce joueur (**protocole 36** : `AnimalShop`,
  `AnimalBuy`). Le client ouvre la même fenêtre du jeu avec ses copies ; sa validation part chez
  l'hôte, qui vérifie l'acheteur (le sien), l'offre et l'argent, puis vend avec le gestionnaire gardé
  (`confirm` du jeu : recrutement, cats prises). L'animal est au joueur et rejoint l'escouade de son
  perso (`Session::AnimalFollowsOwner`, aussi après une recrue). Fichiers : `common/src/session_animals.cpp`,
  `plugin/shops.cpp`. Tests : `TestAnimalShop`, `coop_test.py animalshop`. À tester en jeu.
- **Prospection chez un client (A_FAIRE 3p).** Le bouton PROSPECT (`0x721800`) donne la tâche 150
  par `Character::addOrder` (aucun crochet d'ordre ne la voyait) et la fenêtre s'ouvre à la fin de la
  tâche, qui ne tourne jamais chez un client : rien ne s'ouvrait, rien dans le journal. Le client
  ouvre maintenant la fenêtre tout de suite pour son perso (`ProspectingWindow::showT` : position,
  science, nom ; la fenêtre ne lit que les données du jeu). L'hôte ne peut plus prospecter avec le
  perso d'un autre joueur. Test : `coop_test.py prospect`. À tester en jeu.
- Commandes de debug : `animalshop`, `animalshopstate`, `animalbuy`, `prospect`, `prospectstate`.

## v0.3.2 (12 octobre 2026)

Compatible avec la 0.3.1 (protocole 35) : un client 0.3.1 rejoint un hôte 0.3.2 et inversement. Les
corrections des sacs et les multiplicateurs ne dépendent que de l'hôte ; la correction de la vente
protège les clients qui ont installé la 0.3.2.

- **Plantage d'un client à la vente chez un marchand** (présent en 0.3.1) : la fenêtre de commerce
  gardait l'adresse du marchand (`ShopTrader`+0xC0) quand sa copie locale était remplacée par le vrai
  PNJ envoyé par l'hôte (environ 1 s après l'ouverture) ; le rafraîchissement suivant (après une vente)
  lisait le marchand libéré (`kenshi_x64+0x95484b`). Les fenêtres d'inventaire qui montrent un perso
  sont maintenant fermées avant qu'il soit détruit, par le mod ou par le jeu (`hk_worldDestroy`) : il
  suffit de reparler au marchand. À faire ensuite : garder la fenêtre ouverte sur le vrai marchand.
- **Sacs de marchand (Traders Backpack)** : les objets s'y empilent selon le bonus du sac (minimum 2,
  ×3 : Matériaux de Construction en piles de 6), chez l'hôte comme chez le client ; un objet rangé sans
  case précise remplit d'abord les piles qui ont de la place ; l'hôte vérifie qu'il reste une case avant
  de déplacer un objet (plus de « no room » à tort, plus d'objet sorti puis remis). Test `backpack` 40/40.
- **Multiplicateurs de soin et d'expérience (hôte)** : deux curseurs « Soin » et « Expérience » (x1 à x10) dans la section Administration, enregistrés dans `KenshiCoop.ini` (`[host] heal_rate`, `xp_rate`), commandes `admin rate heal|xp <x>` et `admin rates`. L'hôte multiplie les constantes globales du jeu (`GlobalConstants` `0x2133DD0` : guérison +0x6C, guérison au repos +0x70, gain d'XP +0x138) tant qu'il héberge ; tous les persos de son monde en profitent (joueurs et PNJ), les clients reçoivent santé et compétences par la synchro habituelle. Aucun détour, aucun changement de protocole. À vérifier en jeu.

- **Plantages de l'hôte avec les sacs à dos (11/10, 23:04 et 23:48) : corrigé, testé en jeu (2 min de minage
  à x3, inventaires propres).** Un client qui lâchait au sol un objet de son sac : l'hôte le faisait lâcher par l'inventaire du
  sac, dont le jeu fait retirer l'objet par l'inventaire **du perso**, qui n'a pas la section du sac.
  L'objet arrivait au sol mais restait dans la liste et la section du sac (lâché deux fois, « cannot place
  … at » chez les clients, l'objet qui revient). Une fois cet objet détruit (ramassé et fusionné), l'IA de
  transport de ressources (`getResourceItems`, via `hasABackpackOn`) ou le calcul du poids de la section
  lisait un objet libéré : plantage. Désormais chaque opération de l'hôte (déplacement, échange, dépôt,
  achat et vente, reconstruction chez les clients, plan lu) passe par l'inventaire qui **possède**
  l'objet (celui de la section qui le tient), vérifie que cet inventaire retrouve la section avant tout
  retrait, sort l'objet du sac avant de le faire lâcher par le porteur, et ne détruit un objet qu'une fois
  absent de toute liste (`plugin/kenshi.cpp` : `HomeOf`, `TakeFromHome`, `DropOut`, `DisposeTaken`).
- **Contrôle et réparation des inventaires** : commande de debug `invcheck [all] [repair]` et
  `invcheck each 1` (contrôle après chaque opération) ; en partie, un contrôle toutes les 3 s des persos
  des joueurs et de leurs sacs répare ce qui reste abîmé deux fois de suite (objet libéré encore listé,
  objet parti mais encore dans la liste ou la section), ce qui protège aussi les sauvegardes déjà
  abîmées. Ligne `INVCHECK sweep` dans le journal.
- **Piles dans les sacs (3c)** : une unité posée sur une unité du même objet s'empile aussi chez l'hôte
  (avant : seulement à partir de 2, l'unité prenait une autre case et le sac se remplissait chez l'hôte
  seul, puis « no room »). La règle est celle du jeu (`Item::isStackable`, avec le bonus de pile de la
  section) ; ce qui ne s'empile pas (armes, armures, sacs) échange toujours sa place. Même règle pour
  les achats. Test unitaire `TestStackMerge`.
- Tests : expérience `backpack` (`coop_test.py`), commandes `baginv`, `invitems`, `wearbag`, `objjob`,
  `uidrop bag:N`.

## v0.3.1 (11 octobre 2026)

- **Antivirus** : l'installeur du zip n'utilise plus PowerShell (`Installer.bat` lançait
  `powershell -ExecutionPolicy Bypass`, ce que les antivirus signalent) : de simples fichiers batch
  (`tools/Installer.bat`, `tools/Desinstaller.bat`) cherchent Kenshi dans Steam et ses bibliothèques,
  copient la DLL et ajoutent `Plugin=KenshiCoop` à `Plugins_x64.cfg`, rien d'autre. La DLL porte une
  fiche de version (produit, description, éditeur, lien du code source : `plugin/version.rc`).

- **Toits : on restait sous le toit à l'intérieur d'une cabane.** (1) Un bâtiment à étages (la Petite
  Cabane 4092, son toit est son étage 1, 4093) : `createBuilding` crée lui-même chaque étage dans son
  propre appel, et le mod captait aussi ces appels imbriqués : le toit partait chez les clients comme un
  bâtiment à part, posé sur celui de la cabane, et jamais masqué. Seul l'appel le plus extérieur est
  maintenant une pose (`hk_createBuilding`). (2) Le jeu ne masque les étages au-dessus d'un perso que
  dans les bâtiments que son propre mode construction a faits : le mod le fait pour les autres
  (`KenshiWorld::UpdateRoof`, `Building::setFloorVisibility` 0x5482E0) pour le perso suivi du client
  et le perso sélectionné de l'hôte, et les réaffiche quand il sort. (3) Un bâtiment créé par le mod a
  tous ses étages affichés au départ. Vérifié en jeu (cabane de l'hôte et du client, des deux côtés).
- Test `buildfull` sur la Petite Cabane : 22/25 (pose, construction, entrée et étage des deux côtés,
  établi, démontage croisé ; le coffre reste à faire, test à corriger).

- **Bâtiments posés par le mod : on les traversait, chez l'hôte aussi.** Le mode construction du jeu
  met chaque bâtiment qu'il crée dans une file que le gestionnaire `0x4E4120` passe ensuite à
  `NavMesh::generate(Aabb)` et, pour un intérieur, `NavMesh::generate(hand)` (KenshiLib `NavMesh.h`,
  adresses +0x520 ici). Les poses du mod (celles d'un client, refaites chez l'hôte et chez tous) ne
  passaient pas par cette file : leur bâtiment n'était découpé dans aucune carte de navigation et
  tout le monde marchait au travers (un mur construit par tous les persos, traversé en ligne droite
  des deux côtés, la collision physique pourtant allumée). Maintenant fait comme
  `Building::setDestroyed` (`0x5578A2`) : la boîte du bâtiment (+0x27C centre, +0x288 demi-taille,
  agrandie de 5) puis son intérieur, à la pose et une fois fini (hôte et clients). Vérifié : les persos
  contournent le mur (8-9 unités au plus près, contre 0,1), même chemin chez l'hôte et le client.
- **Ping : Ctrl+clic molette** dans la vue 3D (le clic molette seul tournait aussi la caméra) ; sur la
  carte et la minicarte le clic molette seul marche toujours. Avec Ctrl+clic molette, Alt = butin.
- **Fluidité loin de l'hôte.** Le jeu ne met à jour chaque image que les persos dont l'octet
  `Character+0xE4` est levé (`0x786EF0`) ; les autres, 6 par image à tour de rôle, rattrapent leur
  temps d'un coup : bonds de 20 unités chez l'hôte loin de sa caméra, recopiés par les clients. Levé
  chez l'hôte pour les persos proches d'un joueur client. Les recalages d'horloge d'animation se font
  en douceur au lieu de sauter (2400 sauts en 10 s chez un client). Marche à reculons : la rotation
  de l'hôte est réécrite après la mise à jour de déplacement du client (`CharMovement::update`,
  crochet), et la direction de marche suit le regard de l'hôte dans les escaliers en lacets
  (`kc::motion::TravelDir`, testé) ; la copie marche vers un point 60 unités devant sur la route de
  l'hôte. Vitesse x2/x3 affichée sur les boutons du client (`0x787E40`).
- **Session** : un gel du jeu (zone lointaine qui charge) ne compte plus comme « monde perdu ».
- Tests : `buildfull` (désert dégagé repéré une fois, `open_desert.txt`), `farprobe`, `floorview`,
  commandes `buildphys`, `buildspawn`, `conput`, `gamespeed`, `floorview`, `buildingsnear`.

- **Loin de l'hôte, troisième passe (essais `faraway --clients 2` et `farprobe`, 11 octobre).**
  - *Dialogue : la vraie cause.* `Dialogue::startPlayerConversation` (`0x684320`) refuse toute
    conversation quand le perso est à plus de 9216 unités de la caméra (`0x6843CA`, distance au nœud
    de la caméra, `ja 0x6843D1`). Chez l'hôte, la caméra reste sur son escouade : un client loin ne
    pouvait parler à personne (ni l'ordre du jeu, ni la relance du mod). Pendant qu'on héberge, ce saut
    est remplacé par un NOP (`kenshi::SetFarTalk`, octets vérifiés avant, remis à la fin de la partie).
    Vérifié : Patch, Tanishi, Rane répondent loin de l'hôte, caméra à la maison.
  - *Chiffres de dégâts : la vraie cause.* `MedicalSystem::addWound` saute toute sa partie visuelle
    quand le blessé est à plus de 4000 unités de la caméra (`0x651348`, distance au carré > 16e6,
    `jbe 0x651359`), avant même de lire le drapeau « en vue » forcé en 0.3.1. `hk_addWound` lève ce
    saut le temps de l'appel pour un perso proche d'un joueur client (`kenshi::SetFarWoundVisuals`).
  - *Combat* : marche loin de l'hôte (bête blessée, mêmes blessures chez le client). Les échecs
    venaient du test : le PNJ copié d'un habitant fuyait à 80 unités/s ou allait s'asseoir. Le test
    fait attaquer un Bonedog par le client, par son clic droit (`attackreq <i> <clé>`).
  - *Pause* : un client se recale 0,8 s à vitesse 0,05 avant de se mettre en pause (0,3 s à 0,01) :
    loin de sa caméra, un perso ne prend une position écrite qu'à ses propres mises à jour, plus rares ;
    les marcheurs restaient 20 à 35 unités derrière ceux de l'hôte. Essai d'un recalage par
    téléportation des copies qui dérivent : retiré (3500 téléportations, sauts visibles, pas mieux).
  - Tests : `talkto … name:<partie>` cherche vraiment par nom (prenait le plus proche) ; le dialogue
    essaie jusqu'à six PNJ (beaucoup n'ont rien à dire, comme en solo) ; nouveau `farprobe` (dialogue
    et combat loin, caméra de l'hôte à la maison puis sur le client) ; vidages : `floor=` (étage).
  - Reste : quelques PNJ 16 à 57 unités plus haut chez le client (même étage des deux côtés sauf un),
    quelques marches à reculons.

- **Loin de l'hôte, deuxième passe (essai `faraway` 24/33 avec a100038).**
  - *Le crochet `hk_charUpdateView` marche* (contrairement à ce que montrait « vue 0/0 ») : compteurs
    `forced` 1,3 M chez l'hôte et 1,46 M chez le client ; hors pause, 78 à 79 persos sur 107-117 sont
    « dans la vue » chez l'hôte et 101 sur 107 chez le client. Les relevés de synchro sont faits en
    pause, où le drapeau retombe à 0 : le « vue 0/0 » venait de là.
  - *Positions : la vraie cause*. Hors pause, avec les deux jeux en mise à jour complète, les écarts
    restent (+78,8, +33,1, −56,7, −38,9 en hauteur, x et z exacts, au millième près d'un essai à
    l'autre). **Chaque gros écart est un PNJ qui dort** : action `sleepinbed` chez l'hôte, drapeau de
    marche levé, vitesse 0. Chez l'hôte il est dans son lit (le lit tient sa position) ; chez le client
    la copie n'y était pas : un corps debout aux coordonnées du lit, que le jeu poussait sur le toit ou
    à travers le plancher. Corrigé : le lit passe par l'état de captivité déjà envoyé pour les cages
    (même meuble retrouvé par type et place ; le client reconnaît un lit à sa fonction `BF_BED` et y met
    la copie avec `Character::setInBed`, `0x32E2F0`, `FnSetInBed`), puis aucune correction de position
    tant qu'il y est. Pas de changement de protocole (le drapeau « en cage » sert pour les deux : un
    drapeau à part attend le prochain changement de protocole). Et l'hôte ne dit plus « en marche » un
    perso au drapeau de marche levé mais à l'arrêt (`kc::motion::HostMoving`, vitesse > 0,5, testé) :
    sa copie n'allait plus jusqu'au lit (12,8 unités à plat, paladins d'Okran).
  - *Dialogue* : la relance de l'hôte (`UpdatePendingTalks`) a bien tourné à chaque essai (« started by
    the mod (the NPC has nothing to say) ») : les PNJ choisis (garde de magasin, voleur, sentinelle
    sacrée) n'ont pas de réplique pour « parle-moi ». Le test parle d'abord au marchand de la ville
    (`talkto … name:<nom>`), puis aux plus proches.
  - *Commerce* : `GiveShopHome` a marché (« given the shop's building ») ; le test avait pris un garde
    (« Cent Gardien », 16 comptoirs, 0 pile) pour marchand. `merchants` donne maintenant les piles
    (`nom(comptoirs)[piles]`) ; le test prend le plus garni et ne choisit qu'une ville dont une boutique
    a des marchandises.
  - *Combat* : le perso du client était sur un toit (posé là par la téléportation), le PNJ créé tombait
    73 unités plus bas, cinq fois. Le test descend le perso à côté du PNJ.
  - Vidages : `in=` (0 rien, 1 lit, 2 cage) ; les écarts de la synchro donnent l'action et le lit des
    deux côtés.
- **Loin de l'hôte : positions exactes, dialogue, commerce, combat (`faraway`, 30/42 au premier
  essai).** Causes trouvées dans les journaux et le désassemblage ; pas encore relancé en jeu.
  - *Positions* (Admag 33 à 51 unités d'écart, Bouclier d'Okran 8 à 68, tolérance 3) : presque
    toujours **en hauteur seulement** (x et z identiques au millième), des gardes sur les remparts
    d'Admag et des paladins sur la forteresse ; ailleurs 12,8 unités à plat pour des paladins qui
    « marchent » vers leur lit chez l'hôte sans jamais bouger (vitesse 0) et y sont arrivés chez le
    client. Ni le débit (20 positions/s pour tout perso diffusé, sans budget) ni la pause (les vidages
    en pause et hors pause donnent les mêmes écarts) : le jeu a **deux mises à jour de déplacement**.
    `Character+0x1A9` (« dans la vue de la caméra », recalculé à chaque image par `0x5C9F60`) choisit
    dans `CharMovement` (`0x660A3D`) la mise à jour complète (`0x147FD0` : physique, vrais sols et
    murs, chaque image) ou une version réduite (`0x148AC0` : quelques pas par seconde, une autre idée
    du sol). Les persos de la faction du joueur sont toujours « dans la vue » (`0x5C79C0`, faction
    +0x250), les PNJ seulement près de la caméra : chez l'hôte, les PNJ autour d'un client lointain
    prenaient la version réduite, leurs copies chez le client (sous sa caméra) la complète ; et une
    position écrite sur une copie hors de la vue du client n'était pas prise. **Correction** : détour
    `hk_charUpdateView` sur `0x5C9F60` (`FnCharUpdateView`) : après le calcul du jeu, chez l'hôte les
    PNJ près des persos d'un client (`HookView::remoteNear`, l'ensemble des chiffres de dégâts), chez
    un client tous les persos pilotés par l'hôte, sont mis « dans la vue ». Les deux jeux font alors la
    même mise à jour complète. Compteur `charsForcedInView`, commande `inview`, champ `inview=` des
    vidages.
  - *Dialogue* : l'ordre « parler » arrive et tourne (`client task 12 … ok`), le perso marche jusqu'au
    PNJ, mais l'étape « arrivé : parler » du jeu ne vient jamais loin de la caméra. L'hôte
    (`UpdatePendingTalks`) la fait lui-même quand le perso est depuis 1,5 s à moins de 45 unités du PNJ
    sans conversation (`sendEvent` du PNJ, sinon l'événement forcé), par nos crochets comme d'habitude ;
    un autre ordre du perso l'annule. Test : 45 s d'attente par PNJ (la marche prend 20-30 s ; il
    passait au suivant, plus loin, toutes les 20 s).
  - *Commerce* : `GiveShopHome` n'a jamais trouvé le bâtiment (« could not be given their building ») :
    il le cherchait par `+0x238` des comptoirs, toujours vide pour un meuble de ville. Il cherche
    maintenant parmi les bâtiments autour des comptoirs (intérieur dont le plan pointe vers eux) celui
    dont les meubles de boutique en couvrent le plus. Les comptoirs arrivaient bien au client (32
    comptoirs, 181 piles : « tous les conteneurs vont à tout le monde » est vérifié).
  - *Combat* (test et commandes de debug) : `spawnnpc` copiait le premier PNJ de la liste du jeu, un
    bandit de chez l'hôte : sa copie repartait vers son escouade à 35000 unités, et `fight` n'engageait
    que le perso du client, dont le mode combat retombait. `spawnnpc` copie la personne la plus proche
    et l'arrête ; `fight` engage les deux ; `fight`, `ko`, `kill`, `npcstate` prennent une clé.
    Le compteur `forced` des chiffres de dégâts compte aussi ceux faits grâce à `hk_charUpdateView`.
  - *Fouille des créatures* (`lootcreature` 30/32 : araignée à peau, bête à bec) : « the looter is
    still 71 units from the body … it never walked » : le cadavre du client n'est pas où est celui de
    l'hôte (70 à 79 unités pour une araignée), le pilleur était envoyé au bord du cadavre local et
    bloqué chez l'hôte. Il va au bord du corps de l'hôte et la fenêtre s'ouvre à portée de l'un ou de
    l'autre (`kc::loot::InReachEither`, testé).
  - *Test `faraway --clients N`* : 3 clients chacun dans sa ville, loin de l'hôte et les uns des
    autres, toutes les étapes en même temps, points « loin [J2] : … » ; voir docs/TESTS.md.
- **Expérience `faraway` : tout marche avec les joueurs loin l'un de l'autre, sans que l'hôte regarde.**
  Session réelle : un client à 900 m dans une autre ville avait des PNJ absents, des PNJ saccadés ou
  qui marchaient à reculons, aucun dégât affiché tant que la caméra de l'hôte n'allait pas sur lui,
  pas de dialogue, des fenêtres de commerce vides, des objets perdus (corrigé en 0.3.1 : diffusion
  par joueur, crochet `addWound`, bâtiment du marchand, handle des objets lâchés). `kctest_town` ;
  escouade de l'hôte à la maison, sa caméra dessus pendant tout le test ; le perso du client dans
  une autre ville à plus de 30000 unités, puis une troisième. Points « loin : … » : synchro autour
  du client (3 relevés), mouvement à 10 Hz chez le client (ni marche à reculons ni saut), dialogue
  (le client puis un PNJ), commerce (stock non vide, achat), objet lâché puis ramassé, combat
  (vitaux, blessures, chiffres de dégâts chez le client), fouille d'un PNJ assommé et d'une chèvre,
  caméra de l'hôte jamais à plus de 5000 de son escouade. Ajoutée à `run_all`. Pas encore lancée.
  Nouvelles commandes de debug : `campos` (œil de la caméra en coordonnées du monde), `floaters`
  (chiffres de dégâts : faits par l'hôte, faits hors de sa caméra par `hk_addWound`, affichés chez
  le client), `motionrec` (persos relevés à fréquence fixe sur le fil du jeu), `towns` (villes et
  positions) ; `merchants` prend un index de membre. Compteurs `KenshiWorld::floatersMade`,
  `woundsForcedInView`, `floatersShown` (tests seulement, aucun changement de comportement).
- **Fouille des créatures : premier essai en jeu (`lootcreature`, 26/32) et correctif.** Tortue des marais et léviathan : fenêtre ouverte, objet pris. Chèvre, chien d'os, taureau sauvage, gorillo, araignée à peau, bête à bec : « loot order dropped: the looter is still 33 units from the body » (puis 37, 47, 54, 68, 75), **exactement la distance du départ** : le pilleur n'a jamais marché. *Cause* : l'expérience n'a pas fermé l'éditeur de personnage du client qui arrivait (« Coucoudz 2 is making their character: game paused » au journal de l'hôte, jamais levé) : tout le jeu est resté en pause, aucun ordre de marche n'a avancé, et l'ordre de fouille expirait au bout de ses 30 s + marche. La tortue et le léviathan passaient parce qu'ils étaient déjà à portée (118 et 232 unités, portées 162 et 237) : pas de marche. Ni la portée, ni la position du ragdoll n'étaient en cause (le corps du client et ses tailles, `bodysize`, étaient justes). *Corrections* : (1) l'expérience fait `editdone` et attend la fin de la pause ; (2) le temps de pause ne compte plus dans le délai d'un ordre de fouille (pause de l'hôte, éditeur d'un joueur qui arrive) : l'ordre attend, « loot order waits: the game is paused » ; (3) « arrêté contre le corps » (portée × 1,5) exige que le pilleur ait marché : immobile depuis le début, il attend, il n'est pas bloqué (`kc::loot::OrderClock`). L'abandon dit maintenant « it walked / it never walked, paused N s ». Test : `TestLootCreatures` (pause de 2 min sans expirer, pas « arrêté » sans avoir marché, expiration finale). Pas encore relancé en jeu.
- **Restes de la série du 11/10 à 03 h (master f93e80b) : dialogue 13/16, squadui 19/21, research
  10/13, map 46/47.** Pas encore relancé en jeu.
  - *Dialogue, « occupé pour un autre perso »* (test) : le perso de l'hôte (un autre joueur, comme
    voulu) parlait au PNJ **créé** par le test (`talkto … npc`, un « Garde Ninja »), pas à celui de la
    conversation du client (« Chef Voleur », 3e plus proche) : rien à refuser. Nouvelle cible `dlg`
    pour `talkto` et `npcevent` : le PNJ de la dernière conversation d'un client
    (`KenshiWorld::LastDialogNpc`) ; le test l'utilise. Le refus côté mod
    (`ConversationAllowed`) n'est pas en cause.
  - *Dialogue, « un PNJ engage la conversation » et « vitesse 3 »* (test) : `npcevent` n'essayait
    que le PNJ créé (une copie qui n'a rien à dire) et les 4 plus proches (gardes et voleurs sans
    ligne pour l'événement, ou un chirurgien dont la ligne ne fait que parler). Les conversations
    lancées par un PNJ marchent en vraie partie (journal du 10/10 au soir). Le test essaie d'abord le
    PNJ à qui le client vient de parler (`dlg`), puis le PNJ créé et les 8 plus proches ;
    `npcevent` dit `talking=1` quand le PNJ a vraiment engagé ce perso (sinon 3 s d'attente au lieu
    de 8).
  - *Escouades, « ordre des escouades identique »* (mod) : le client glissait son escouade (2e) en
    tête ; deux voisines échangées donnent deux candidats (la sienne remontée, ou celle de l'hôte
    descendue), et `ClientSquads` prenait le premier trouvé, celle de l'hôte, refusée : « the host's
    order is put back » à chaque essai depuis le lot escouades. C'est maintenant l'escouade que le
    joueur peut modifier qui est demandée. Test unitaire `TestSquadWindow` (son escouade remise en
    tête).
  - *IA, « tâches du perso du client identiques »* (test) : le perso du client suivait celui de
    l'hôte, puis l'hôte faisait suivre son perso par celui du client : le jeu retire le premier
    « rester proche » quand le second ferme la boucle (0 tâche à chaque série ; la même demande du
    client dans `suite` donne bien 1 tâche). Le perso de l'hôte suit maintenant un autre perso de
    l'hôte, après que la tâche du client est arrivée.
  - *Recherche, bac à sable* (test) : les combats du village laissaient les membres 0, 1 et le perso
    du client K.-O. ; la TP d'un corps ne tenait pas (« went back 5014 away when it lay down
    again »), le membre 0 (ancre de toutes les poses) revenait au village : 7 anneaux refusés, puis
    banc de recherche posé mais « no such building » pour le finir, établis, éolienne et lampes
    « trop près d'une ville », livres non donnés, donc l'hôte refusait toutes les recherches
    (« banc de recherche trop petit, ou artefacts / livres manquants »). Maintenant : tout le monde
    soigné et réveillé (`console heal all`) et en mode dieu dès le début, de nouveau soigné avant le
    départ ; le membre 0 doit vraiment être au point choisi (sinon anneau suivant) ; avant chaque
    pose il y est renvoyé s'il en est parti ; `buildprogress` réessayé jusqu'à 12 s le temps que le
    bâtiment existe. *Non résolu* : la TP d'un corps couché (7646713) ne tient toujours pas quand le
    jeu le recouche ; le test l'évite.
  - *Carte, « écran de carte refermé »* (mod, commande de test) : la fenêtre du client venait d'être
    réduite à 672×313, la croix de la fenêtre de gestion était hors de l'écran (« the button is not
    on screen »). `mapui close` cache alors la fenêtre comme sa croix
    (`MyGUI::Window::setVisible(false)`, export de MyGUI ; l'état « ouvert » du jeu est cette
    visibilité) et répond `ok hidden`. La fonction de fermeture propre au jeu n'a pas d'adresse
    connue (la vtable de `ManagementScreen` n'a que son destructeur).
  - Pas de changement de protocole.
- **Régressions de la fusion 0.3.1 (master a2890df) corrigées.** Pas encore relancé en jeu.
  Mesures de la série du 11/10 à 02 h : suite 24/32 (32/32 en 0.3.0), dialogue 5/10, sécurité
  71/72, recherche 31/35, admin 27/28, join4 15/16.
  - *Pause en pleine course* (5,8 unités d'écart) : l'hôte en pause n'envoie plus rien, et le client
    rendait encore son coureur avec le délai adaptatif puis l'extrapolation de 0,25 s au-delà du
    dernier état : un quart de seconde de course en avant. Hôte en pause : le client montre l'état
    le plus récent tel quel, sans délai ni extrapolation (`kc::motion::PausedState`,
    `Session::HostPaused`, aussi pour `TargetOf`). Même cause pour une bonne part des écarts de
    « fin : escouade identique », mesurée en pause.
  - *Orientation en marchant à vitesse 3* (167°) : rendu au-delà du dernier échantillon
    (extrapolé), le perso a sa dernière position connue *derrière* lui ; `TravelDir` en tirait le
    sens de la marche, et « le regard suit le sens réel de la marche » le tournait vers l'arrière.
    Un sens vers la dernière position qui s'éloigne de la destination n'est plus le sens de la
    marche : la direction de la destination l'est.
  - *Personne ne manque chez le client* : la diffusion par joueur ne prenait que les persos du
    joueur pour centres, et le client ne recréait un PNJ qu'à 2500 de ses persos. Une araignée à
    2994 du perso de l'hôte et 2881 de celui du client (fin de suite), une esclave à 2528 (après la
    resynchro) n'existaient pas chez le client. Les centres d'un joueur sont maintenant ses persos
    **et les membres de l'escouade à moins du rayon d'un d'eux** (le groupe avec lequel il joue,
    `kc::GroupCentres`), côté hôte (`UpdateStreams`) comme côté client (`UpdateMyCentres`), et
    `[sync] standin_radius` passe de 2500 à 3000 (le rayon de diffusion). Un joueur seul au loin
    ne reçoit toujours que ce qui entoure ses persos.
  - *TP admin d'un perso à terre* (392,8 de l'hôte) : le perso du client était K.-O. (les combats de
    la sauvegarde de test) ; la téléportation « prenait » (position lue juste à 0,3 s), puis le corps
    recouché reprenait la place de son ragdoll, là où il gisait. Un corps est maintenant relevé,
    téléporté à une image suivante, recouché sur place, puis revérifié une fois couché ; s'il est
    reparti, il est renvoyé et laissé debout (le jeu le recouche lui-même là où il est). La
    commande de test `teleport` passe par là pour un corps (`TeleportCharacters(..., spread)`).
  - *Recherche* (« ordre du client exécuté », « générateur coupé ») : les chemins des requêtes de
    machine et de fabrication sont bien autorisés ; l'hôte les refusait « trop loin de cette
    machine » parce que le perso du client, K.-O. dans un combat du village, n'avait pas suivi le
    déplacement du bac à sable (`teleport` ne bougeait pas un ragdoll) : corrigé avec le point
    précédent.
  - *Dialogue* (aucune conversation, 5/10) : rien n'est filtré par joueur pour les conversations (ni
    les paroles, ni les fenêtres, ni « occupé ») ; dans cette série, le PNJ de test copié par
    `spawnnpc` était un garru (« Garrou »), qui n'a rien à dire, et les PNJ les plus proches pour
    `talkto` / `npcevent` comptaient les animaux. `spawnnpc` copie une personne (un animal
    seulement s'il n'y a personne), `talkto` et `npcevent` ignorent les animaux. À revérifier en jeu.
  - *Sécurité, « parler »* : le perso 2 de l'hôte bougé de 5,4 sans tâche, ni travail, ni dialogue :
    poussé par le perso du client qui passe à côté, pas un ordre. Le correctif de « parler » ne
    touche que le PNJ (fin d'une conversation périmée) ; seuil de déplacement du test 5 → 8.
  - *Mode dieu sous les coups* (sang 75,8 → 75,2) : les coups sont refusés (`hk_medDamage`), mais un
    combat faisait encore baisser le sang un peu. Le sang d'un perso en mode dieu ne descend plus
    sous son maximum depuis l'activation (`kenshi::HoldBlood`, à chaque passage de 0,2 s).
  - *join4* : avec la file 0.3.1 (tout le monde charge ensemble, seul l'éditeur va un par un), le
    dernier arrivé n'attend souvent que derrière celui d'avant : il voit sa place (2/2), qui
    n'avance pas. Le test vérifie qu'il a vu sa place, derrière un joueur passé avant lui.
  - *Tests (coop_test)* : « fin : escouade identique » compte aussi un corps simplement à terre
    (drapeau d'état 4/8), avec la tolérance de `kRelayOffset` (20) ; « TP admin, même endroit chez
    le client » tolère 20 pour un perso couché.
  - *Tests unitaires* : `TestMotion` (sens de la marche extrapolé, état en pause),
    `TestGroupCentres` (dont l'araignée de la suite). Pas de changement de protocole.
  - *Non résolu* : « fin : aucun inventaire différent » (une captive dont la copie change d'escouade
    chez le client sans arrêt) n'a pas de cause trouvée dans les journaux ; à revoir au prochain
    passage.
- **Minicarte déplaçable et redimensionnable.** Pas encore lancé en jeu.
  - *Déplacer* : clic gauche maintenu sur la minicarte et glisser (le clic gauche n'y faisait rien ;
    clic molette et Alt+clic restent aux pings, la molette au zoom). Un clic sans bouger ne change
    rien. *Redimensionner* : la poignée sur le bord en bas à droite, ou Ctrl+molette au survol
    (50 % à 250 %).
  - *Écran* : toujours entière à l'écran ; position gardée en fractions de l'écran, elle survit à
    un redimensionnement de la fenêtre. Nouveaux réglages `[ui] minimap_x`, `minimap_y` (-1 : le
    coin) et `minimap_size`.
  - *Souris* : pendant le glissé, la souris est capturée et le jeu ne reçoit aucun clic ; la
    capture se coupe d'elle-même quand l'overlay ne dessine plus, que la minicarte disparaît ou
    que le relâchement est manqué.
  - *Affichage* : le choix d'un coin remet la minicarte dans ce coin (le menu montre « position
    libre » après un glissé) ; nouveau bouton « Réinitialiser la position » (coin et taille
    normale) ; infobulles en français.
  - *Tests* : `TestMinimapPlace` (coins, fractions après un redimensionnement, bords, tailles) ;
    `mapscene minicarte` donne toujours le centre, plus la position (`fx`, `fy`, `size`, `pos`,
    `r`, `free`, `drag`, `screen`).
- **Fouille de toutes les créatures par un client (léviathans, bêtes à bec, gorillos, tortues des marais, araignées…) : corrigé.** Signalé après la partie réelle du 10/10 : l'hôte fouille ces créatures en solo, un client non. Pas encore lancé en jeu.
  - *Ce que fait le jeu* : un humanoïde (`CharacterHuman` : greenlanders, sheks, ruches, squelettes, hommes-brume, cannibales…) et un animal (`CharacterAnimal`, handle de type 0x5B : chèvre à léviathan) se fouillent pareil, tâche 26 `LOOT_TARGET` et fenêtre de fouille. Il n'y a **pas de tâche de dépeçage** : viande, peaux, dents, perles de léviathan sont les « death items » du modèle de l'animal, mis dans son inventaire à sa mort, et se prennent dans la fenêtre comme n'importe quel objet (le message « {1} is butchering {2} for meat!! » n'est qu'une notification de `Task_Loot_Order`). Les contrôles du mod (`IsCharacter` sur les trois vtables, `ClientLoot`, liste blanche des tâches, droits de l'hôte sur un corps) acceptaient déjà les animaux.
  - *Cause 1, la distance* : le client n'ouvrait la fenêtre que si le pilleur était à 12 unités (en 3D) de la position du corps. La position d'un grand corps, la racine de son ragdoll, est au milieu et en hauteur : rayon de 40 (« pathfind footprint radius ») et coque de 75 × 90 × 75 pour un léviathan, à l'échelle 1,5 adulte ; 16 pour une bête à bec, 25 pour une tortue des marais, 12 à 14 pour gorillos et araignées. Le pilleur s'arrêtait contre le corps, à 50-150 unités de son centre, et l'ordre expirait au bout de 30 s **sans rien dire**. Désormais la portée dépend du corps : `kc::loot::Reach` = 12 + 2 × rayon (le plus grand du rayon de pathfinding et de la coque, fois l'échelle), en distance au sol, la hauteur du corps en plus à la verticale ; le pilleur est envoyé au bord du corps de son côté, pas en son centre ; un pilleur arrêté depuis 1 s (bloqué par le ragdoll) a 1,5 fois cette portée ; l'ordre attend 30 s plus la marche. Taille lue dans les données de la race du corps (`Character+0x2E0` → `RaceData+0x40`, ce que lit `CharMovement` en `0x661F90`), à défaut dans la table des 38 races du jeu (`kc::loot::CreatureRaces`, tirée de gamedata.base, Newwworld.mod, rebirth.mod).
  - *Cause 2, l'âge des animaux recréés* : `ReadSpawnSource` envoyait comme âge l'échelle (vt 0x390 : lerp(échelle min, échelle max, âge)) au lieu de l'âge (vt 0x398, +0x700, ce que la fabrique reçoit). Chez un client, un léviathan adulte (échelle 0,4-1,5) avait l'âge 1,5, donc l'échelle 2,05 : plus gros, son ragdoll ailleurs que chez l'hôte ; un gorillo adulte (0,25-0,85) l'âge 0,85, donc 0,76. Les humains (1 et 1) ne changent pas.
  - *Dépôt au sol depuis un grand corps* : l'hôte acceptait un dépôt fait par un perso à 160 unités du corps au plus ; c'est maintenant 160 + 2 × rayon + hauteur (`Session::DropReachOf`, `IWorld::ReadBodySize`).
  - *Journal* : `loot order: N units to go (animal dead, race Leviathan, scale 1.50, body radius 112, reach 237)`, `loot window opened at N units (…, stopped by the body)`, `loot order dropped: the looter is still N units from the body (…)` ; commandes de test `spawncreature`, `bodysize`, `lootstats`.
  - *Tests* : `TestLootCreatures` (types de handle et de modèle, chaque race à son échelle mini, moyenne et maxi : un pilleur au bord du corps est à portée dans 8 directions, trop loin ne l'est pas, le point d'approche est au bord, l'ancienne règle échouait pour chaque animal plus large que 12 ; valeurs aberrantes bornées ; l'âge contre l'échelle), `TestBigBodyDrop` (un client prend une perle d'un léviathan mort chez l'hôte, et le dépôt depuis son flanc est accepté, refusé pour un corps de taille humaine) ; expérience `lootcreature` (chèvre, chien d'os, taureau, gorillo, araignée, bête à bec, tortue, léviathan : apparition, mort, fouille par le client, un objet pris, comparaison), pas encore lancée.
- **Administration : faire apparaître des objets.** Pas encore lancé en jeu.
  - *Fenêtre de l'hôte* (section Administration, repli « Faire apparaître des objets ») : favoris
    (matériaux de construction `580-gamedata.base`, tissu, fer, cuivre, nourriture, bandages),
    recherche et filtre par catégorie sur tous les objets que la fabrique du jeu sait faire
    (armes, arbalètes, armures, objets, sacs à dos, cartes, membres robotiques), quantité,
    fabricant et modèle pour une arme, cible « près de moi » ou un joueur, bouton
    « Faire apparaître ». 500 objets au plus par clic, confirmation au-delà de 100.
  - *Commande* : `admin spawn <sid|nom> <n> [here|<id>] [fabricant] [modèle]` (console, console
    hors du jeu, commande de test `admin spawn …`). Hôte seulement : un client est refusé.
  - *Comment* : la fabrique du jeu (`CreateItemFromState`), l'objet posé comme un objet lâché
    (`CreateGroundItem`), en piles de la taille du champ « stackable » des données, en spirale
    autour du perso ; chaque pile est annoncée aux clients tout de suite
    (`KenshiWorld::NoteItemDropped`), sans attendre le balayage du sol (qui ne regarde qu'autour
    d'un perso immobile depuis 2 s). Catégories et tailles de pile lues dans les données du jeu
    (« item function », « artifact », « stackable » ; fabricants : « weapon types »,
    « weapon models »).
  - *Messages* : journal « admin: spawned … », message à l'écran pour l'hôte, message en français
    au joueur ciblé.
  - *Tests* : `TestAdmin` (syntaxe, piles, répartition, recherche, catégories) ; étape « objets »
    de l'expérience `admin` (pas encore lancée).
- **Partie du 10/10 (0.3.0, 4 joueurs) : fenêtre de commerce vide chez un client, bâtiment d'un client sans jauge ni collision, vue des étages qui ne suit pas chez un client.** Corrigé d'après le désassemblage et le journal de l'hôte, pas encore lancé en jeu. Nouveau message 87 `BuildMaterials` (H→C) : un client 0.3.0 l'ignore (type inconnu) ; noté dans la version **35** du protocole, que le monde compressé impose de toute façon (voir « Rejoindre »).
  - *Commerce vide et qui clignote* (rob 23:06-23:15 avec « Marchand Ruche », nass4 avec « Produits Commerciaux » et « Marchand Construction Ronin ») : le marchand du client était une doublure créée par le mod. La ville, chargée après l'arrivée du client, avait été générée à part sur chaque machine (autres handles) : le jeu du client avait son propre marchand, retiré (« removed the local character 'Marchand Ruche': the host does not have it », 23:05:01) au profit d'une doublure de `createRandomCharacter`. Le constructeur de `ShopTrader` (`0x953850`) prend le domicile dans `getOwnerships(marchand)+0x38` (l'escouade d'un PNJ) : la doublure n'en a pas, il vend donc depuis les sacs de son escouade, vides. Après la reconnexion de 23:17 (la ville dans la sauvegarde envoyée), le même marchand marchait. Correction : avant d'ouvrir la fenêtre, le client donne au marchand le bâtiment des comptoirs reçus de l'hôte s'il ne vend pas déjà depuis eux (`kenshi::GiveShopHome` : le bâtiment dont `0x54ACB0` rend le plus de ces comptoirs ; `IWorld::OpenTradeWindow` reçoit les comptoirs). La réouverture (3 fois, le clignotement) est supprimée ; une fenêtre vide n'est plus que notée au journal.
  - *Jauge des matériaux d'un chantier vide chez les clients* : la jauge lit la quantité apportée de chaque matériau (`Building+0x160` +0x20, entrée +0xC), remplie par les livraisons, qui n'ont lieu que chez l'hôte. L'hôte envoie maintenant ces quantités (message 87, ce qui change chaque seconde, tout toutes les 10 s et à l'arrivée) ; le client les écrit tant que le chantier n'est pas fini.
  - *Bâtiment fini sans collision* : le jeu crée les corps physiques d'un bâtiment éteints ; seule sa mise à jour d'état (vt 0xE0, `0x5626A0`) les allume, quand il est visible, physique et fini. `notifyConstructionComplete` ne l'appelle pas (son `setVisible(1)` ne fait rien sur un chantier déjà visible) et les bâtiments ne sont pas mis à jour à chaque image : un bâtiment fini en cours de partie restait traversable jusqu'au rechargement de sa zone (selon le désassemblage chez l'hôte aussi, non vu en jeu ; les bâtiments de la sauvegarde sont créés finis et l'ont). Le mod appelle vt 0xE0 une fois, chez le client après chaque état reçu et chez l'hôte pour chaque bâtiment suivi, seulement s'il est fini, visible, physique et encore sans collision (« its collision switched on » au journal).
  - *Vue des étages* : elle suit le perso « suivi » du `PlayerInterface` (+0x270 ; `0x801630` compare son étage au dernier vu, +0x290, et affiche le nouveau, +0x1E4), pas la sélection ; seul `0x7F5400` le change (double appui sur une touche d'escouade, portrait, fin de l'éditeur). Le jeu d'un client gardait celui de la partie chargée (le perso de l'hôte). `KenshiWorld::UpdateFloorFocus` (client, toutes les 0,5 s) le pointe sur un perso du joueur, le sélectionné d'abord, sans toucher à la caméra (qui garde sa copie du `hand`), quand il n'est pas déjà un des siens. Les écritures de `floorGroup` restent désactivées (inutiles : le jeu du client calcule l'étage de chaque perso).
  - *Tests* : `TestTrade` (la fenêtre reçoit les comptoirs ; une fenêtre vide n'est plus rouverte), `TestBuildings` (matériaux apportés suivis par le client, pas réimposés sans changement), aller-retour et fuzzing de `BuildMaterials`, sa règle d'autorité.
- **Objets perdus par les clients (posés au sol, armes jetées, ramassages) : corrigé** (partie réelle du 10/10 à 4 joueurs par Steam). Pas encore vérifié en jeu.
  - *Cause* : un dépôt au sol nommait l'inventaire par le handle **local** du perso (`KenshiWorld::TakeLocalDrops`), alors que la session connaît chaque perso par le handle **de l'hôte**. Ils diffèrent pour une doublure (perso créé par l'hôte, perso d'un joueur revenu après un plantage : journal « stand-in 1:582:… for the host's 1:47:… ») et pour un perso qui a changé d'escouade. Les ordres, eux, passaient par `HostHandleOf` : rob « parlait » aux PNJ avec son perso pendant que ses dépôts depuis ce même perso échouaient. Journal de l'hôte : « the host does not know that inventory » 23 fois (rob : 580 matériaux de construction depuis son propre perso ; nass4 : 52295 / 1020, armes des persos de son escouade). Le test `grounddrop` passait en local : même sauvegarde, mêmes handles. Et le crochet de la fenêtre d'inventaire avait déjà retiré l'objet de l'inventaire du client : l'objet disparaissait, revenait au mieux 10 s plus tard, ou restait introuvable quand la reconstruction ne pouvait plus le poser (« cannot place 42243-rebirth.mod » puis « here missing »). Un objet lâché depuis un sac à dos passait par le `dropItem` de son porteur : l'hôte cherchait l'objet dans le mauvais inventaire.
  - *Correction* : `TakeLocalDrops` rend le handle de l'hôte d'un perso. **Rien ne bouge chez le client pour un geste qui ne peut pas partir** : la session publie toutes les 0,2 s les inventaires depuis lesquels un dépôt peut être demandé (`IWorld::SetDropSources`, `Session::PlanClientDrop` : ses persos, les sacs qu'ils portent, un coffre ouvert ou un corps avec un de ses persos à côté) ; le crochet d'`Inventory::dropItem` les lit (`HookView::dropSources`) et refuse aussitôt tout autre dépôt : l'objet reste, ou revient à sa place s'il était tenu à la souris (`kenshi::KeepInInventory`), toast « Objet non posé : … Rien n'a bougé. ». Un sac à dos est nommé lui-même. Le jeu du client ne pose plus jamais rien au sol lui-même (`dropItem` de n'importe quel perso hors appel de l'hôte), ni ne ramasse un objet du monde (`giveItem`). Un dépôt refusé tard par la session fait revenir tout de suite l'état de l'hôte, avec une note « Objet non posé » dans le chat.
  - *Ramassages* : l'hôte n'annonçait pas que les ramassages : chaque objet que son jeu créait et donnait à un inventaire (stock d'un marchand, équipement d'un PNJ qui apparaît) était « loose » et partait comme ramassé (8624 lignes « host picked up item …: we do not have it » chez les clients, par rafales d'indices consécutifs). Seul un objet actif dans le monde ou rangé dans un groupe d'objets compte maintenant (`kenshi::ItemPlacedInWorld`). L'hôte journalise où arrive un objet ramassé pour un client (« in the inventory of the character that was sent », ou un autre perso). La reconstruction d'un inventaire chez le client ne laisse plus le jeu détruire un objet qu'elle ne peut pas poser à la place de l'hôte : il va dans le sac.
  - *Tests* : `TestClientDropGate` (sources publiées, sac à dos, handle inconnu de l'hôte, refus tardif avec retour immédiat de l'état de l'hôte).
- **Postures et mouvements chez les clients (partie réelle du 10/10 en v0.3.0)** : captifs qui convulsent, PNJ K.-O. et cadavres debout, PNJ saccadés et « moonwalk » loin de l'hôte, combats de Hep saccadés. Non revérifié en jeu ; règles pures dans `common/include/kc/motion.h`, test `TestMotion` et test de session « cadavre recréé ».
  - *Captifs qui convulsent chez nass4 (cause confirmée)* : 1811 lignes `[nass4] posture:`. La correction de posture forçait une chute (`falls as on the host`) sur des captifs enchaînés (« Bandit affamé chained slave-state=2 ») que le jeu du client rasseyait aussitôt : une chute toutes les 2 s, sans limite (1859 chez nass4, 2861 chez rob). Et le relevage des corps à plus de 5 unités (`stood up to fall where the host's lies`, 441 fois chez nass4) se déclenchait après presque chaque chute (un corps tombe à 7-12 unités de ses pieds), son compteur « 3 fois au plus » remis à zéro à chaque changement de posture. *Correction* : jamais de chute ni de relevage pour un perso porté, enchaîné, en cage, au lit, prisonnier ou esclave (ici ou chez l'hôte : `PostureHeld`, `hostCaptive_`) ; carnet par corps (`motion::PostureBook`) jamais remis à zéro par la posture : 4 chutes et 2 relevages par minute, plus rien pendant la minute après deux chutes qui ne tiennent pas ; relevage seulement à plus de 20 unités, 10 s d'écart, et seulement s'il reste une chute pour recoucher le corps.
  - *PNJ K.-O. vus debout* : la chute de la posture ne rendait pas le corps inconscient chez le client, qui le relevait. Il tombe maintenant inconscient quand la santé de l'hôte le dit (`hostKnockedOut_`) ; `hk_ragdollMode` laisse le jeu remettre en ragdoll un perso inconscient ou mort chez le client.
  - *Cadavres qui finissent tous debout* : `hk_ragdollMode` refusait au jeu du client toute remise en ragdoll d'un perso suivi, cadavres compris ; une doublure recréée (zone déchargée puis rechargée) naissait vivante et debout ; un corps posé par son porteur était posé sans ragdoll. *Correction* : ragdoll permis pour un perso mort ou inconscient chez le client ; doublure d'un corps mort tuée et couchée dès sa création ; corps mort posé directement en ragdoll ; un perso que l'état de l'hôte dit mort ne peut jamais être ranimé par la synchro de santé (`VitalsFlagsFor`) ; s'il est vivant ici il meurt et tombe, s'il est debout alors que celui de l'hôte est en ragdoll il est recouché (`DeadBodyStep`, même limite par corps) ; jamais relevé.
  - *PNJ saccadés et « moonwalk » à 900 m de l'hôte* : (1) le jeu de l'hôte ne bouge un perso loin de son escouade que quelques fois par seconde : le client recevait la même position répétée puis un saut, et la rendait 50 ms en retard (un seul intervalle) sans extrapoler : arrêt, saut. Les répétitions d'un marcheur sont fusionnées, chaque perso a son propre retard (50 ms + son intervalle moyen, 0,45 s au plus, changé en douceur, le rendu ne recule jamais) et un marcheur continue 0,25 s sur sa lancée. (2) Le rappel de position tirait la copie en arrière pendant qu'elle marchait en avant dès qu'elle avait plus de 0,8 unité d'avance, mesurée sur l'orientation de l'hôte (souvent périmée loin de lui) : elle n'est plus retenue qu'à 2 unités/s (× vitesse du jeu) le long du vrai sens de déplacement, et avance toujours. (3) La direction imposée en marchant venait de l'orientation de l'hôte même quand le perso allait ailleurs : elle suit maintenant le sens réel du déplacement quand les deux divergent de plus de 45°.
  - *« vitesse 3 : orientation en marchant » jusqu'à 180°* : la destination n'était redonnée que si celle de l'hôte changeait ; une copie dont le chemin local s'était arrêté ou allait ailleurs marchait dans un sens pendant que le rappel la tirait dans l'autre. Vérifiée toutes les 0,5 s (persos à moins de 300 unités) et redonnée au besoin.
  - *Combats de Hep saccadés* : même cause de fond (positions rendues un intervalle en retard, arrêt puis saut à la moindre gigue) et un rappel qui dépendait des images par seconde (2,5 fois plus raide à 150 images/s qu'à 60) : retard adaptatif et rappel indépendant des images par seconde. Pas de cause propre à Hep trouvée dans le journal ; à revérifier en jeu.
- **Actions des clients refusées ou cassées (partie réelle du 10/10 en v0.3.0)** : ordres refusés à tort, « parler » refusé avec soi-même, recrue invisible chez le client, fenêtre de sortie des mines invisible. Tests unitaires ; rien de vérifié en jeu.
  - *« Tâche 16 » refusée 26 fois* : ce n'était **pas** démanteler (96, déjà permis sur un bâtiment du joueur, fini ou en chantier). La numérotation `TaskType` du jeu est dans `fcs_enums.def` (dossier de Kenshi, `enum taskType`) : 16 = `ATTACK_ENEMIES`, que le clic droit d'attaque ajoute après l'attaque (5) sur la même cible (désassemblage de `0x7FBB90` : `addOrderSelectedCharacters` avec 5, puis 16 « ajouté »). Les autres refus de la partie : 69 (déposer) avec le corps porté nommé, 29 (aller à) sur un objet, 45 (garde du corps) sur un membre de l'escouade.
  - *Liste blanche revue en entier* (`kc::TaskTargetAllowed`, protocol.cpp) contre les tâches que l'interface donne : constantes passées aux quatre fonctions d'ordres et tâche par défaut de chaque classe de bâtiment (vtable +0x418 : mines / fermes / établis 87 ou 152, stockage et fourneau 26, recherche 87, tourelle 146, meubles), complétée par la liste du jeu. Ajoutées, avec leur cible : attaquer les ennemis / défendre (9-16), attaque sans provocation (61, mal nommée « premiers soins robot » jusqu'ici), animal (227), tir (266, 267), garde du corps / suivre le chef (45, 141, 164), apporter les matériaux et bâtisseur (71, 125, 115), récolter / vider / remplir / débloquer / tout faire tourner (88-94, 113, 151, 221, 247), nourrir une machine (136), transporter et ranger (114, 184, 241, 242), fouiller les morts et dépecer (177, 238), lits (100, 106, 153), cages et chaînes (109, 111, 112, 133, 134, 166, 185, 186, 201, 207, 219, 256, 281), portes « ici » et portails (74, 75, 79, 80, 121-123, 140, 162, 180, 198, 287), crocheter une cage (76 et serrures sur une cage), prospecter (150), s'asseoir (275), entrer / sortir (120, 156, 200), nourriture (131, 232, 243), se relever / attendre (62, 63, 65, 175, 208, 264, 265). « Aller à » accepte tout ce qui est trouvé ; « déposer » un perso ou un bâtiment. `TaskLabel` nomme toutes ces tâches.
  - *« Marchand Ruche is busy talking with rob »* : `TalkTargetBusy` ne regardait que le perso, pas le joueur ; et quand le jeu gardait le PNJ « en conversation » avec un perso du joueur sans fenêtre (handle changé, conversation jamais fermée), l'ordre « parler » passait (« -> ok ») sans rien ouvrir. Maintenant jamais « occupé » pour le joueur lui-même : même perso → la conversation est renvoyée sur son écran (`ResendDialog`) ; autre perso à lui → l'ancienne est terminée et sa fenêtre fermée, la nouvelle part ; conversation fantôme du jeu avec un de ses persos → terminée (`EndConversationOf`) avant l'ordre. Le client affiche « X va parler à Y : la conversation s'ouvrira à son arrivée. » (une fois par PNJ toutes les 5 s).
  - *Recrue d'un client* : la ligne de dialogue « rejoindre » de certaines recrues appelle `PlayerInterface::recruit(perso, editor = true)` (`Dialogue::_doActions`, action 3) : l'éditeur de perso s'ouvrait **chez l'hôte**, et la recrue n'entrait dans l'escouade qu'une fois l'hôte validé. Chez les clients, leur copie restait le PNJ d'avant sous son ancien handle (« 1 host squad members are missing »), jamais recréée pour son propre joueur. Correction : crochet de `recruit` chez l'hôte (conversation d'un client) : recrutée sans éditeur ; la session la donne à ce joueur dès qu'elle apparaît (`TakeRecruits`, par identité), envoie le `Bind` avec l'ancien handle du PNJ (le client le renomme, `Rehandle`), et l'éditeur s'ouvre chez ce joueur (`EditCharacter`, « Personnalise ta recrue »). Chaque client fait entrer sa copie dans la faction du joueur (`AdoptRecruit`, le `recruit` du jeu sans éditeur) ; sa recrue absente de son monde est recréée puis adoptée.
  - *Fenêtre de sortie des mines* : `Building::select` (`0x5504A0`) n'ouvre le panneau d'inventaire d'un bâtiment que s'il est fini **et à la faction du joueur** ; une mine ou un filon devient au joueur quand un de ses persos le travaille (`Task_OperateMachine` appelle `Building::setFaction`, vtable +0xA0). Chez le client la tâche ne tourne pas : le filon restait à personne, pas de fenêtre (vrai pour tous les filons et mines). L'hôte envoie maintenant ce propriétaire (`kMachOurs` dans `MachineState`) et le client l'impose (`SetBuildingOurs`) ; l'inventaire de la machine était déjà synchronisé.
  - *Protocole* : pas de changement de format ni de numéro ; un nouveau bit `kMachOurs` (64) dans les drapeaux de machine, ignoré par un client plus ancien. Un hôte de cette version accepte plus d'ordres d'un client plus ancien.
  - *Tests* : `TestTaskTargets` (toutes les tâches de l'interface sur la bonne cible, par les 4 façons, et les cibles dangereuses toujours refusées), `TestTalkAndRecruit` (nouveau), `TestWorkshop` (`kMachOurs`). Expériences : `dialogue` (reparler au même PNJ, message au client), `mine` et `research` (clic sur le filon chez le client : à lui, panneau ouvert ; commande `machineselect`).
- **Rejoindre : tout le monde télécharge et charge le monde en même temps, seul l'éditeur de personnage passe un par un ; le monde envoyé est compressé ; durées de chaque étape au journal.** **Protocole 35** : tout le monde doit mettre à jour. Tests unitaires ; pas encore essayé en jeu.
  - *Ce qu'on a vu* (partie du 10/10, 4 joueurs par Steam) : nass4 a mis 1 min 55 entre « world sent » et son arrivée ; rob, arrivé pendant ce temps, n'a reçu le monde que 1 min 45 plus tard. La file de 0.3.0 faisait tout passer un joueur à la fois : rob attendait le chargement **et** l'éditeur de nass4 avant que sa sauvegarde soit seulement commencée. Le transfert n'était pas l'essentiel (rob : 3,3 Mo reçus et chargés en 18 s ; nass4 au resync : 7,6 Mo en 36 s) : les 1 min 55 de nass4 sont surtout le premier chargement de Kenshi chez lui. Aucune durée par étape n'était au journal.
  - *Maintenant* : une sauvegarde à la fois, faite pour tous ceux qui en attendent une (leurs persos créés d'abord, avec ceux de tous les précédents), envoyée à chacun dès qu'elle est prête ; la suivante part dès qu'elle est finie. Tous chargent en même temps. Le monde reste en pause jusqu'à la dernière fermeture d'éditeur ; un `Ready` est accepté si le client a chargé la sauvegarde qu'on lui a envoyée et si le monde de l'hôte n'a changé depuis que par les persos des arrivants suivants (il en reçoit des doublures, comme les joueurs déjà là). L'éditeur passe un joueur à la fois, dans l'ordre d'arrivée dans le monde ; les autres attendent dans le monde et voient leur place (« Création de ton personnage : position 2/3 — Joueur2 crée le sien, chacun son tour… », `JoinQueue` aussi en jeu). L'apparence faite dans un éditeur pendant qu'un autre charge lui est envoyée à son arrivée ; côté client, une apparence reçue avant son perso est gardée et appliquée quand il est là.
  - *Même PC* : chaque jeu a son propre emplacement d'import (`KenshiCoopJoin`, puis `KenshiCoopJoin2`…, réservé par un mutex nommé) : deux chargements simultanés ne s'écrasent plus.
  - *Compression* : chaque fichier de la sauvegarde est compressé une fois (XPRESS Huffman de Windows, `cabinet.dll`, `kc/pack.h`), et part tel quel s'il ne rétrécit pas. `WorldBegin` porte aussi la taille décompressée, le premier `WorldChunk` d'un fichier sa taille envoyée ; le client refuse un flux qui ne redonne pas exactement la taille annoncée.
  - *Durées au journal de l'hôte* : `world saved: … in T s, packed to N KB (P %)`, `world delivered to X: N KB in T s (R KB/s, ping N ms)` (ENet a tout acquitté : le débit réel, relais Steam compris), `X has loaded the world: T s after it was sent`, `character editor: … after T s`, et le bilan `join timings for X: … (waiting for a save, save, download + load, waiting for the editor, editor)`. Côté client : `world downloaded: … in T s (R KB/s), unpacked in T s`, `world loaded in T s`.
  - *Test en jeu à adapter* : `join4` vérifiait « un seul client charge le monde à la fois » ; il vérifie maintenant que plusieurs chargent en même temps, et ne ferme l'éditeur d'un client qu'à son tour.
- **Un joueur parti avant d'avoir fait son personnage retrouve l'éditeur à son retour** (Geoffrey, 10/10 23:36). Tests unitaires ; pas encore essayé en jeu.
  - *Cause* (journal) : l'hôte avait créé son perso et l'éditeur s'était ouvert, puis un « resync 4 » pendant l'éditeur l'a fait recharger ; à son retour, le fichier des joueurs retrouvait ce perso (« is back with their character ») : pas nouveau, donc pas d'éditeur, alors qu'il ne l'avait jamais fait. Le « 0 character(s) they had are theirs again » qui suivait était trompeur : son perso venait de lui être rendu et n'était pas compté.
  - *Correction* : l'hôte retient (par compte Steam, sinon par nom) les joueurs dont le perso a été créé et dont l'éditeur ne s'est pas fermé ; à leur retour l'éditeur rouvre sur ce perso, jusqu'à ce qu'ils le ferment. Côté jeu, un perso mémorisé dans `KenshiCoop-players.txt` qui n'est pas un membre **vivant** de l'escouade de ce monde n'est plus rendu (« remembered character is not a living member of the squad in this world: a new one for them ») : nouveau perso, éditeur. Le journal compte le perso déjà rendu (« (1 of them their own character) »).
- **Retour après un plantage : les persos rendus sont retrouvés par netId** (vérification demandée après les dépôts au sol refusés de rob). L'hôte retenait les persos d'un joueur parti par leur handle : un perso que Kenshi change d'escouade (nouveau handle) pendant l'absence n'était pas rendu. Retenus maintenant par netId, qui ne change pas. Vérifié par test : après un plantage, le joueur retrouve son perso et celui que l'hôte lui avait donné (rehandlé entre-temps) sous les mêmes netIds, à lui chez lui, et son dépôt au sol part chez l'hôte. La propriété n'explique donc pas les « host does not know that inventory » de rob (voir docs/A_FAIRE.md, indice sur un changement de handle local).
- **Diffusion par joueur : chaque client reçoit les persos autour de SES persos, plus la foule autour de l'hôte ; plus de boucle de doublures ; création des persos étalée ; resync automatique de la zone d'un client ; chiffres de dégâts hors de la caméra de l'hôte ; les adresses des persos déchargés ne sont plus gardées.** Partie réelle à 4 du 10/10 (journal `kc_crashdumps/session_2215/host_live.log`). Pas de changement de protocole (toujours 34 : aucun message nouveau ni modifié ; un client 0.3.0 marche avec un hôte 0.3.1, sans les gains côté client). Non vérifié en jeu.
  - *Causes trouvées* : (1) avec `interest_radius = 0`, l'hôte envoyait à **chaque** client **tous** les persos actifs de son jeu, ensemble décidé autour de sa caméra et de son escouade : positions 20 fois/s, animations 15 fois/s, santé, événements. rob, à ~900 m de l'hôte dans une ville bondée, recevait la foule de l'hôte (288-315 persos suivis) et la recréait là où son jeu n'avait rien chargé ; (2) ces doublures disparaissaient aussitôt (zone non chargée chez lui) et étaient refaites toutes les ~3 s : **9 723** lignes « stand-in … is gone here » pour rob (1 807 persos différents de l'hôte, 42-44 fois chacun pour certains), 203 pour Geoffrey ; une escouade locale entière disparaît d'un coup (22:31, 00:27:50, 00:28:03, juste avant ses deux plantages, au moment où il « change de vue ») ; (3) les « NPCs not there yet » (41, 69) comptaient cette foule ; (4) **chiffres de dégâts** : `MedicalSystem::addWound` ne crée le chiffre que si le perso est vu par la caméra (`Character+0x1A9`, mis à jour à chaque image par `0x5C9F60` d'après la distance à la caméra `0x7F2A20` et le cadrage ; test en `0x651DF5`, désassemblé) : un joueur qui se bat loin de la caméra de l'hôte n'en recevait aucun, jusqu'à ce que l'hôte « le regarde » ; (5) la fabrique refusait en boucle des persos uniques ('Cat', 'Ruka', 'Kang', 'Rane la Géante', 160-170 fois chacun chez rob) : réessai toutes les 2 à 15 s ; (6) toutes les doublures dues étaient créées dans la même image (gels à l'arrivée dans une ville) ; (7) **plantage probable** : le miroir d'animations gardait l'adresse d'un perso jusqu'à 1 s après sa dernière mise à jour (`replicatedAt_`, `animTargets_`) et écrivait dans son `AnimationClass` : quand une caméra qui part décharge toute une escouade de doublures, il écrivait dans de la mémoire libérée (non prouvé : pas de rapport de plantage de rob).
  - *Corrections, hôte* : `Session::UpdateStreams` (`common/src/session_streaming.cpp`, toutes les 0,5 s) : un PNJ est envoyé à un joueur s'il est à moins de `[sync] stream_radius` (3000, hystérésis ×1,25) d'un perso **de ce joueur** (un joueur sans perso suit l'escouade de l'hôte) ; les persos des joueurs et les conteneurs vont à tous ; ce qu'un perso suivi combat ou porte suit. Positions, santé, images d'animation et événements d'animation (dont les chiffres de dégâts) sont filtrés par joueur ; un perso qui entre dans la diffusion d'un joueur lui est envoyé en entier. `0` : ancien comportement.
  - *Chiffres de dégâts* : nouveau détour `MedicalSystem::addWound` (`0x6508D0`, `hk_addWound`) : pour un perso près d'un perso d'un joueur client (`IWorld::SetRemoteInterest`), le drapeau « vu par la caméra » est mis le temps de l'appel puis remis ; le chiffre est créé (hors de l'écran de l'hôte) et part chez ce joueur. Rien d'autre dans `addWound` ne lit ce drapeau.
  - *Corrections, client* : une doublure n'est créée que pour un perso que l'hôte envoie encore (« en sommeil » après 4 s sans état) et à moins de `[sync] standin_radius` (2500) de nos propres persos ; « NPCs not there yet » ne compte qu'eux. Au plus `[sync] spawns_per_frame` (2) créations par image, les plus proches d'abord, et seulement après un passage de `Reconcile` (adoption d'un perso local). Une doublure perdue revient après 2 s, puis 10 s, 30 s, 1 min, 2 min (`kc::StandInLossDelay`), résumé au plus toutes les 30 s (« N stand-in(s) vanished here lately »). Un modèle que la fabrique refuse attend 30 s, 1, 2, 4 … 10 min (`kc::Backoff`). Les lignes « is gone here » disent pourquoi (« unloaded with its zone by our game », d'après le détour `GameWorld::destroy`) et sont limitées à 6 par 10 s ; « anim in » limité à 20 lignes, « anim not blocked » à 10. Un perso en sommeil n'est plus piloté qu'une fois par 0,5 s.
  - *Adresses des persos détruits* : le détour `GameWorld::destroy` note chaque perso détruit ou déchargé ; `KenshiWorld::PurgeDestroyed` (début et fin d'image) retire son adresse de tout ce qui la garde (miroir d'animations, `replicatedAt_`, outils en main, chiffres, escouade et résolutions de l'image, file d'animations de l'hôte) et republie la vue des détours sans elle. Le miroir d'animations ne touche plus qu'aux persos trouvés par `Apply` **dans l'image**.
  - *Resync automatique de zone* : l'hôte lit les rapports de chaque client (toutes les 5 s, `kc::ZoneHealth`) : 5 PNJ manquants ou plus, ou 3 persos décalés ou plus, trois rapports de suite (15 s) → `Session::ZoneResync` : tous les persos envoyés à ce joueur sont liés à nouveau (`Bind`), leur état et leurs inventaires renvoyés ; le client les cherche aussitôt, recrée les manquants sans attendre ses délais et remet les présents exactement où l'hôte les a (`IWorld::ResyncCharacter`). Personne n'est mis en pause, l'hôte n'a rien à cliquer. Au plus une fois par minute ; après 3 essais sans bon rapport, « zone still differs for X … a full resync may be needed » puis un essai toutes les 5 min. `[sync] auto_zone_resync = 0` l'arrête.
  - *Mesure* : une ligne par joueur et par minute dans le journal de l'hôte : « stream: rob is sent 87 of 312 characters (around their own), 41.3 KB/s to them, ping 80 ms, 0.4 % lost » (`NetStats::queued`, compteur à nous : ceux d'ENet sont remis à zéro chaque seconde).
  - *Tests* : `TestStreamingTools` (délais, limites de journal, décisions de resync), `TestPerPlayerStreaming` (deux clients à 2 km l'un de l'autre et un perso de l'hôte à 2 km de l'autre côté : chacun ne reçoit que ce qui est autour de ses persos ; foule de 12 créée 2 par image au plus ; doublure perdue sans cesse : 3 fois au plus en 9 s ; client à qui il manque 6 PNJ : resync de sa zone seule, puis tout arrive), `TestStandInArea` (hôte qui envoie tout : rien de recréé ni compté loin de nos persos). `KC_ONLY_STREAMING=1 kc_tests` les lance seuls. En jeu : nouvelle expérience `coop_test.py camjump` (caméra du client qui saute entre une foule à vitesse 3 et son perso à 5 km, 20 fois).
  - *Limites* : un client qui regarde avec sa caméra une zone loin de tous ses persos (celle d'un autre joueur) n'y reçoit plus la foule de l'hôte (il faudrait envoyer la position de sa caméra à l'hôte : changement de protocole, pas fait). Le moment où la zone d'une doublure déchargée revient n'est pas détecté : elle revient au bout de son délai ; si le jeu recharge sa propre copie, `Reconcile` l'adopte ou la retire comme avant. Les empreintes de zone (docs/INSPIRATION_PZ.md §4) ne sont pas faites : la décision se fonde sur les rapports existants.

## v0.3.0 (10 octobre 2026)

Protocole 34 : tout le monde doit installer cette version pour jouer ensemble. En bref :
stabilité (plusieurs plantages corrigés, client qui plante puis revient), un client ne contrôle
plus jamais les persos de l'hôte, file d'attente pour rejoindre, 4 joueurs testés, carte /
minicarte / pings / couleurs des joueurs, panneau admin de l'hôte, marchands ambulants et
sacs à dos, recherche / fabrication / mines / énergie, escouades et IA, dialogues, diplomatie,
objets posés au sol, poses de bâtiments valides. Le détail suit.

- **Persos des joueurs en double chez les clients déjà là : corrigé** (trouvé dans le stress à 4 joueurs du 10/10 : client 1 avec 4 copies de trop de Joueur3/Joueur4 au début, 12 à la fin ; client 2 avec 2 puis 4). Non revérifié en jeu.
  - *Cause* : `KenshiWorld::BeginFrame` reconnaissait un nouveau monde, après une coupure du tick, à une escouade dont les objets avaient changé. La photo de référence n'était prise qu'au moment où le monde redevenait prêt : toute copie d'un autre joueur créée depuis (un joueur qui arrive) faisait de la coupure suivante (fenêtres réarrangées, zone qui charge) un « nouveau monde ». Celui-ci vidait les alias ; les copies déjà dans le jeu n'étaient plus reconnues (« 2 host squad members are missing in the local world ») et la session en créait d'autres, qui rendaient à leur tour la photo périmée : une série par coupure (journaux : générations 2 à 7 du client 1, chacune suivie de nouvelles copies).
  - *Correction* : `kc::WorldIdentity` (`common/include/kc/world_identity.h`) décide : photo de l'escouade **à chaque image**, **sans les doublures** (alias) ; nouveau monde = autre objet joueur, ou escouade changée après une coupure. Sur un vrai nouveau monde, les doublures créées par `Spawn` (suivies dans `spawned_`, pas les PNJ adoptés) encore là (même handle, même objet) sont détruites avant de tout oublier (`… stand-in(s) of the previous one still here, removed`).
  - *Tests* : `TestWorldIdentity` (coupure, recrue, doublure, chargement, restes) ; `TestNoDuplicatePlayers` : un client là d'abord, deux joueurs arrivent l'un après l'autre, coupures du tick (fenêtres, zones) entre et après : exactement une copie de chacun, aucun nouveau monde ; puis un vrai chargement avec les deux copies restées dans le jeu : retirées, recréées, toujours une de chaque.
  - *Les 4 PNJ absents chez le client 3 au relevé s11* : sans rapport. Pas de nouveau monde après son retour (génération 2 = le chargement du retour, 20:30:26) ; à 20:30:52 son jeu a retiré d'un coup dix doublures d'esclaves (« stand-in … is gone here: it can be recreated »), recréées après le délai de grâce : le relevé, 2 s plus tard, est tombé entre les deux. Le relevé suivant ne les signale plus.

- **Plantage du client dans la barre d'escouade, encore (soak du 10/10, 20:20:14, 13 min, vitesse 3) : même endroit que celui de 14:54 ; l'escouade des morts n'était jamais reconnue ; la barre est maintenant vérifiée et protégée.** Pas encore lancé en jeu.
  - *Ce que disent le rapport et le dump* : `kenshi_x64+0x412D96` lit le `hand` (+0x38/+0x40) d'une `PortraitData*` qui vaut `0x7020656420737075`, le texte « ups de p » (« Vous ne pourrez pas donner de cou**ps de p**ied. », la même infobulle médicale qu'à 14:54 : « ied. »). `KenshiCoop.dll+0x105523` est `map.cpp` ligne 500 (PDB du build installé, lu par dbghelp) : le retour de `o_portraitUpdate` dans `hk_portraitUpdate`, qui ne faisait que passer l'appel. La pile complète, désassemblée : file des événements d'interface de `GameWorld` (`0x7B65B0`, un événement « escouade changée ») → `MainBarGUI::platoonChanged` (`0x4167B0`) → reconstruction des onglets (`0x416140`) → `0x415E00` sur un onglet **gardé**, dont le redimensionnement redessine toutes les cases → `requestDrawItem` → `PortraitMainCellView::update` → plantage. Le `Holder` MyGUI de la case a une vtable intacte (le jeu vérifie le type avant) mais sa valeur est du texte. Le dump ne contient pas le tas : qui a écrit là reste inconnu.
  - *Ce qu'on sait maintenant du jeu* (désassemblé, docs/MOTEUR.md §12) : une `PortraitData` n'est **jamais** libérée en cours de partie (seulement toutes ensemble au chargement d'un monde) ; seul l'onglet de l'escouade affichée est rempli à nouveau quand une escouade change ; les autres gardent leurs cases jusqu'à ce qu'on les affiche, et une reconstruction des onglets les redessine telles quelles.
  - *Cause trouvée, en amont* : le test « escouade des morts » de `kenshi::PlayerSquads` ne marchait jamais (le `hand` d'un `Platoon` du joueur a index et serial à 0, donc `kc::Handle::valid()` faux). L'hôte envoyait son `__DEAD_SQUAD__` dans `SquadState`, et le client en faisait une escouade **ordinaire** (« squads: a new squad for '__DEAD_SQUAD__' » à 20:14:36) ; son propre `__DEAD_SQUAD__` comptait aussi comme une escouade vide à demander à l'hôte, à renommer, à réordonner (`changePlatoonIndex`) ou à retirer (`destroyPlatoon`). Les deux plantages ont suivi la mort de persos de joueurs chez le client (14:49:59, 20:15:04 et 20:15:21). Confiance moyenne : c'est l'état anormal que montre le journal, pas l'écriture fautive elle-même.
  - *Corrections* : l'escouade des morts est reconnue comme le jeu le fait (`hand` du `Platoon` == `PlayerInterface+0x2C8` par les champs de `hand::operator==`, son nom en secours) : jamais envoyée ni suivie, jamais renommée, réordonnée, remplie, vidée ni retirée par le mod ; un perso mort n'est jamais déplacé (`MoveToSquad`). Côté session : l'hôte n'envoie pas d'escouade nommée `__DEAD_SQUAD__`, le client ignore celle d'un hôte plus ancien et ne demande jamais la sienne, l'hôte refuse une escouade créée ou renommée sous ce nom. `kenshi::DestroyObject` (persos en trop, doublures) ne supprime jamais un perso d'une escouade du joueur (il a un portrait dans la barre) ; le journal nomme chaque perso retiré (« removed the local character '…' »).
  - *Protection de la barre* : `hk_portraitUpdate` ne laisse dessiner une case que si sa `PortraitData` est une de celles du `PortraitManager` du jeu (pointeur comparé, jamais lu) : sinon la case n'est pas redessinée (« squad bar: a portrait cell's data … is not one of the game's portraits: not drawn ») au lieu de planter. Et à chaque image, après tout ce que fait le mod (`kenshi::CheckSquadBar`) : chaque case de chaque onglet est comparée aux portraits du jeu ; un onglet fautif est vidé (`ItemBox::removeAllItems`) et le jeu remplit à nouveau l'onglet affiché (`MainBarGUI::updateCurrentPlatoon` `0x415880`, ajoutée à `kFunctions`) ; journal « squad bar check: … » avec la première case fautive (onglet, escouade, pointeur) : le prochain soak dira à quel moment la barre se corrompt. Test unitaire `TestDeadSquad`.
- **Stress à 4 joueurs du 10/10 (17 min, aucun plantage, 10/16) : les 6 échecs expliqués.** Correctifs non relancés en jeu ; relecture des relevés de l'essai avec les nouvelles vérifications du test.
  - *Heure (mod)* : client 3 jusqu'à 0,162 h d'écart, client 2 0,045 h. Journal du client 3 : 0,0005 h à 20:24:25, puis 0,16 h **en avance** à 20:24:46, pendant que les 4 jeux tournaient à ~900 ms d'aller-retour (relevé s3 : zones qui chargent, mémoire +300 Mo) ; ensuite −5 % pendant 3 min, coupé à chaque pause de l'hôte (0,16 → 0,13 → 0,11 → 0,09 → 0,05 → 0,006 h). Un rattrapage de ±5 % ne pouvait pas suivre. L'heure n'est plus seulement rattrapée : `ClockSync` demande un recalage (`World::SetGameHours`) au-delà de 0,008 h (jeux en marche) ou de 0,002 h (hôte en pause, heure exacte), au plus toutes les 1,5 s ; en deçà, ±3 % / ±10 % (au lieu de ±2 / ±5 %), dès 0,002 h. L'horloge se règle comme une nouvelle partie le fait, trouvé au désassemblage : jour (+0x8) puis `0x66CF30` (heure du jour, nouvelle entrée `FnClockSetHourOfDay` de `kFunctions`), +0xA0 étant recalculé à chaque image depuis ces deux valeurs. Journal `clock: … set to its H h`. Test `TestClockSync` : 0,162 h / 0,13 h / 0,3 h / 2 h d'écart rattrapés en moins de 6 s, et recalage en pause.
  - *Objet marqué « en trop » chez l'hôte (test)* : 40 pour 20 donnés à chaque relevé, dès le premier. Un membre d'escouade figure deux fois dans un relevé (`squad` et `char`) et `tag_total` les comptait deux fois. Compté une fois par perso : 20, puis 23 avec les 3 du corps, puis 22. Pas de duplication.
  - *Inventaires différents (mod)* : PNJ `Ninja_Nuage` (1:44:…) doublés chez les clients, bottes (`2306-clothes_v1.mod`) et chemise (`2214`) **dans le sac** (main 2,0 et 0,2) alors que l'hôte les porte. `InventorySection::canItemGoHere` (`0x74BE40`) refuse d'emblée une section désactivée (+0xD0) ; depuis « Worn items can be moved into the bag again » (953467f), la reconstruction la consultait avant de poser, et l'objet tombait dans le `addItem` du jeu. La reconstruction d'un inventaire (`RebuildInventory`) pose maintenant en mode `hostLayout` : la section est activée le temps de la vérification comme de la pose (les déplacements d'un joueur rejoués par l'hôte respectent toujours la section). Un objet qui reste ailleurs est nommé au journal (`inventory of … not as on the host`).
  - *Zones (mod et test)* : (1) des corps K.-O. couchés 7 à 12 unités (humains) et 47 à 79 (chauve-souris, araignée) de ceux de l'hôte, relève après relève, le même écart chaque fois : la position d'un corps est son bassin, qui finit à une distance à peu près fixe de ses pieds ; le client mettait sa copie debout **sur** le corps de l'hôte et la laissait tomber, donc un écart de plus. Il mesure maintenant où son corps a fini par rapport à l'endroit où il était debout (`fallFrom_`, `fallShift_`) et le remet debout d'autant en deçà (`FallSpot`). (2) Le test comparait tout à 3 unités jusqu'à 1500 : un corps à terre a 8 (comme pour l'escouade), les positions ne comptent qu'à moins de 300 (la portée où le mod corrige, au-delà le jeu du client ne fait presque pas bouger un perso : une chèvre à 689–911), la hauteur 15 (voir ci-dessous) ; l'absence compte toujours jusqu'à 1500. Reste à revoir : 4 PNJ absents chez le client 3 19 s après son retour (relevé s11) — passage, ou PNJ apparus dans la sauvegarde avec d'autres handles.
  - *Escouade en pause (test)* : les 25 écarts (0,12 à 4,55) sont **tous en hauteur**, 0,000 à 0,017 au sol, la copie du client tenant exactement la position de l'hôte (`latest`) ; tous dans les régions lointaines, l'escouade de l'hôte à la maison jamais. Ce n'est ni l'interpolation ni la pause : chaque jeu pose le perso sur son sol. Tolérance : 0,1 au sol, 15 en hauteur (inscrit dans docs/A_FAIRE.md, hypothèse à vérifier : l'hôte n'a pas de caméra là-bas).
  - *Temps de frame (test)* : 489 ms au relevé s3 (les 4 jeux à ~900 ms d'aller-retour au même moment, zones qui chargent) et 393 ms au relevé s10 (départ du client 3, l'hôte sauvegarde le monde pour son retour) ; tous les autres entre 182 et 238 ms. Vérification : 90 % des relevés sous 250 ms, aucun au-delà d'1 s.
  - *Trouvé en plus, pas corrigé* : des **persos de joueurs en double** chez les clients déjà là (client 1 : 4 copies de Joueur3/Joueur4 au début, 12 à la fin ; client 2 : 2 puis 4), une série à chaque « new world » détecté à tort (docs/A_FAIRE.md). Nouvelle vérification « aucun perso de joueur en trop chez un client » : elle échouera tant que ce n'est pas corrigé.

- **Fenêtre Escouade : corrigé après le premier essai en jeu (`squadui` 9/20, suite 26/32).** Pas encore relancé en jeu.
  - *Cause* : l'hôte nommait chaque escouade par le `hand` de son `Platoon`, qui n'est pas valide pour les escouades du joueur : `SquadState` partait sans aucune escouade. Le client, qui ignorait alors l'ancien message `Squads`, ne suivait plus du tout les escouades de l'hôte ; ses demandes (renommer, créer, réordonner) ne trouvaient aucune escouade de l'hôte. Les persos du client gardaient leur escouade et donc leur `hand`, et les commandes de test par rang (`where 3`, TP admin, horloge d'animation) visaient un autre perso que chez l'hôte : d'où les 947 unités de « pause en pleine course » et les échecs du TP, sans vraie désynchro de position.
  - *Correction* : une escouade est nommée par l'adresse de son `Platoon` chez l'hôte (`kenshi::SquadHandle`) ; le client ne la compare plus à ses propres escouades (seulement : association déjà faite, majorité des membres, escouade vide à lui, nouvelle). Filet : un `SquadState` sans escouade laisse le client suivre l'ancien message `Squads`.
  - *Expérience `squadui`* : le refus du réglage d'IA sur le perso de l'hôte n'était pas un refus quand ce perso est une recrue « à personne » (il basculait le passif avant le test de la recrue) : il vise maintenant un perso de l'hôte qui n'est pas « à personne ». « ia : taches du perso du client identiques » a échoué alors que le perso du client était à terre (K.-O. chez l'hôte) : à revoir au prochain essai.

- **Expérience `research` refaite après le premier essai en jeu (4/10, 10/10 18:12)**. Ce qui a marché : les demandes de recherche du client arrivent à l'hôte et sont refusées avec la raison (aucun banc), l'état de recherche est identique, et chez l'hôte les mineurs sont bien comptés (« machine: Minerai Cuivre workers 1 -> 2/2 »). Ce qui a échoué venait du banc d'essai : modèles cherchés par des noms anglais ou partiels alors que le jeu est en français (« Petit Établi Rech. »), bac à sable refusé près de la ville (« too close to a town »), et le test regardait un filon de fer où personne ne travaillait pendant que les mineurs allaient sur un filon de cuivre (2 places seulement). Maintenant : modèles par identifiant de `gamedata.base` (banc de recherche 1875, établis 2929 / 2262 / 2299, éolienne 1999, batteries 1855, lumières 14444 / 5363 / 5361, filon de fer 42248), mine d'abord sur le filon de fer (3 places) par la nouvelle commande `workreq <index> <sid>` (l'exemplaire le plus proche du membre 0, le même pour tous), puis escouade téléportée sur la terre ferme loin des villes avant le bac à sable, perso du client au travail sur le banc, technologie choisie parmi celles que les bancs peuvent payer (`researchpick <n> cost`). Pas encore relancé en jeu.
- **Plantage `kenshi_x64+0x883B78` (addr 0x161) chez l'hôte puis chez un client (Joueur4, 17:32:58, stress à 4) : cause trouvée.** Non testé en jeu.
  - *Où* (désassemblage) : `CharStats::update` (`0x883B70`) lit `medical->inconscient` (+0x161) sans tester `medical` (`CharStats+0x8`), nul ici. Appelé par `Character::update` (vt 0x280, retour `0x5D066D`) sur le thread de travail du jeu (`0x787940`). Un `CharStats` n'a pas de `MedicalSystem` entre son constructeur et son `init` ; la vt 0x228 d'un perso en crée un neuf, alors que le même emplacement d'un `Building` est un simple accesseur. Les deux plantages ont suivi de quelques ms un BUILD (tâche 2) sur un PNJ : la tâche traite très probablement sa cible comme un bâtiment (détail dans docs/MOTEUR.md, section 14).
  - *Comment le client y est arrivé* : à 17:31:10, `jobreq 7 27 …` (tâche 27 sur un coéquipier) passait par la copie locale d'un job (`RouteOrder`, AddJob + shift) **sans vérifier la cible** ; l'`addJob` du jeu a fauté, le tick a rattrapé l'exception (`tick: access violation caught`)… mais en `/EHsc` un `__except` ne lance pas les destructeurs : la `HostCallScope` ouverte autour de l'appel est restée comptée. Dès lors, tous les hooks prenaient les ordres du client pour ceux du mod : plus aucune ligne `client order … asked of the host` en 110 s, des jobs ajoutés en local puis retirés par la synchro (`had 1 job(s) the host's character no longer has`), et `npcreq 7 2 a` (BUILD sur « Bandit affamé ») exécuté par le jeu du client lui-même → plantage 112 ms plus tard. Prouvé par un test unitaire (une portée laissée ouverte après `RaiseException` rattrapé).
  - *Correctifs* : compteurs de portées réparés (`kc/call_scopes.h`, `MarkCallScopes` / `RepairCallScopes`) au début de chaque tick de la boucle principale et à la fin de chaque tick (les destructeurs ne descendent plus sous zéro), après l'exception d'un ordre exécuté pour un client (`CallTaskSeh`), de la copie locale d'un job (`AddJobSeh`, qui rattrape maintenant l'exception lui-même) et de l'usine de personnages (`Spawn`), avec une ligne `… call scope(s) left open by a caught exception` au journal. La copie locale d'un job chez le client vérifie la cible comme l'hôte (`TargetFlagsOf` + `TaskTargetAllowed`) et ne lance rien sinon (`client job N not shown ahead here`, l'hôte décide). Les commandes de debug qui donnent un ordre au jeu local (`taskreq`, `talkreq`, `attackreq`, `npcreq`, `jobreq`, `carryreq`, `containerreq`, `doororder`) font la même vérification chez l'hôte (`err refused: …`) ; chez un client l'ordre part toujours à l'hôte, qui le refuse. L'usine de personnages : une exception y est maintenant signalée (`exception … inside the game's character factory`) et ce modèle n'est plus recréé (chaque essai pourrait laisser un perso à moitié prêt dans les listes de mise à jour, même plantage).
  - *Après fusion de master* : la copie des jobs de l'hôte avec leurs cibles chez le client (`ApplyJobList`, `kenshi::AddPermajob` → `Character::addJob`) ne donne plus au jeu un job dont la cible trouvée ici n'est pas du bon type (`kc::TaskTargetWrongKind` : seulement pour une tâche que la table connaît ; journal `the host's job N not copied here`). Garde `CallScopeGuard` (réparation des portées à la sortie du bloc) autour des blocs `HostCallScope` de l'atelier, des escouades, de l'administration, des répliques et de la fin des conversations ; `talk` (debug) vérifie aussi sa cible.
  - *Vérifié, rien à changer* : la synchro des listes de jobs (`ApplyJobs`) ne fait que retirer des jobs, `NoteLocalJob` ne fait que noter ; l'administration ne donne pas d'ordre. Une cible qui disparaît plus tard (PNJ retiré, doublure recréée) n'atteint pas ce chemin : le job garde un `hand` (avec numéro de série) qui ne se résout plus en rien, jamais en un objet d'un autre type (pas de confusion de type possible). Les ordres que l'hôte donne lui-même par l'interface du jeu ne sont pas filtrés (l'interface ne propose pas BUILD sur un PNJ).
  - *Le dump fourni* (`crashDump1.0.68_x64.zip`, 15:32 UTC) est un autre plantage (dans `MyGUIEngine_x64.dll`, thread 54436), pas celui-ci.
  - Tests : `TestCallScopeRepair` (réparation, comportement `/EHsc`, BUILD et tâche 27 refusés quelle que soit la voie).
- **Fenêtre Escouade et réglages d'IA : l'hôte décide, tout le monde voit la même chose, sans conflit.** Tests unitaires ; expérience `squadui` pas encore lancée en jeu.
  - *Avant* : un client pouvait renommer une escouade, en créer une vide, réordonner les escouades ou changer le chef en échangeant deux portraits : rien de cela n'allait à l'hôte (l'hôte remettait son état 10 s plus tard, ou jamais pour une escouade vide). Le nom d'un personnage changé chez un client restait local, celui changé chez l'hôte n'arrivait pas aux clients. Les tâches ajoutées (panneau Tâches) n'apparaissaient pas chez les autres joueurs. Un mode de la barre d'escouade arrivait comme une bascule : deux clics proches pouvaient finir à l'inverse de ce que voyait le joueur. Le bouton SECOURS (RESCUE) d'un client était refusé (tâche 148 inconnue de la table des cibles).
  - *Maintenant* :
    - nouveaux messages `SquadState` (60, H→C), `SquadRequest` (61, C→H) et `JobState` (62, H→C), chacun avec sa règle dans `kMessageRules` (`SquadRequest` : `InGame` + `OwnCharacter`, l'acteur nommé est un perso du joueur) ;
    - `SquadState` : chaque escouade de la faction du joueur dans l'ordre de la fenêtre Escouade (vides comprises), nommée par le handle de son `Platoon` chez l'hôte, ses membres dans l'ordre (le premier est le chef), le nom de chaque perso de l'escouade et le drapeau « à personne » ; envoyé à chaque changement et toutes les 10 s ;
    - côté client, tout ce que fait la fenêtre Escouade devient une demande à l'hôte : portrait déposé (`addCharacterAt`) ou échangé (`swapCharacters`, nouveau hook), renommage d'escouade, nouvelle escouade, escouade glissée (ordre), escouade vide retirée (croix), renommage de son perso ; l'hôte les exécute une par une dans l'ordre d'arrivée (la dernière gagne) puis envoie le nouvel état à tous ; un nom en cours de frappe reste affiché jusqu'à la réponse, un refus remet celui de l'hôte ;
    - règles : seulement ses propres persos se déplacent ; renommer / réordonner / retirer une escouade qui contient un de ses persos (ou vide ; retirer : vide et pas la dernière) ; renommer seulement son perso ;
    - réglages d'IA (bascules de la barre d'escouade, style de combat, allure, tâches permanentes) : le propriétaire, et pour les **recrues à personne** (persos de l'hôte jamais donnés, sauf l'avatar de l'hôte) **l'hôte et n'importe quel joueur**, la dernière demande exécutée par l'hôte gagne ;
    - un mode de la barre d'escouade part avec la valeur voulue (`Command.shift` = absolu, `add` = valeur) : plus de bascule inversée ; les anciens clients gardent la bascule ;
    - `JobState` : la liste de tâches de chaque perso des joueurs avec la cible de chaque tâche (handle d'un perso, ou type et endroit d'un meuble) ; le client retire, ajoute (`Character::addJob`) et réordonne pour avoir la même liste que l'hôte ;
    - tâches 105, 142, 148 (secourir) acceptées sans cible ;
    - cinq fonctions ajoutées à `kFunctions` (prologues relevés) : `ActivePlatoon::swapCharacters` (hook), `Faction::changePlatoonIndex`, `Faction::destroyPlatoon`, `ActivePlatoon::setName` (avant adresse en dur), `Character::getPermajobData` ;
    - commandes de debug `squadsfull`, `squadrename`, `squadnew`, `squadorder`, `squadlead`, `charrename`, `aiorder`, `aimodes`, `aishared`, `jobsof`, `jobfor`, `charinfo` ; expérience `squadui` (vérifications « escouade : … » et « ia : … », modifications simultanées de l'hôte et du client) ; tests unitaires `TestSquadWire`, `TestSquadWindow`.
  - *Protocole* : compris dans la version 34 (non publiée, avec l'atelier) ; `Squads` et `JobList` sont encore envoyés (un client qui reçoit `SquadState` / `JobState` les ignore).
- **Dialogues : les conversations engagées par un PNJ se fermaient aussitôt** (essai `dialogue` du 10/10, 8/11 ; corrigé, pas relancé). Journal de l'hôte : « conversation 1 opened for another player's character », puis 0,2 s plus tard « conversation 1 ends: they are far apart (teleport) », à chaque fois. Le PNJ (Chef Voleur, 3e plus proche) aborde le perso du client de plus de 40 m, et le balayage fermait toute conversation à plus de 40 m (400 unités). Le client ne voyait donc jamais la fenêtre, et l'hôte n'en suivait aucune ; les vérifications « une téléportation / un combat ferme la conversation » réussissaient pour la même mauvaise raison. Maintenant : une TP est un saut (plus de 20 m d'un des deux entre deux balayages à 0,5 s d'intervalle), ou un écart qui grandit de plus de 100 m depuis le début. Aussi : la même réplique, remplie deux fois par le jeu (texte puis réponses), n'est plus envoyée ni numérotée deux fois. L'expérience essaie les 4 PNJ les plus proches pour la conversation lancée par le client (la copie créée, un « Bandit affamé », n'a rien à dire).
- **Carte, essai `map` du 10/10 17:40 (36/47) : cinq causes corrigées.** Non relancé en jeu.
  - *Persos en double dans le flux de carte* (hôte 14 pour 7, client 2 21/14, deux repères par joueur) : `HostMapMarkers` ajoutait l'escouade à la liste de travail `scratchHandles_` sans la vider ; elle tenait encore les persos proches de la passe d'intérêt du tick, escouade comprise. Vidée, et chaque perso une seule fois. Le faux monde des tests unitaires renvoie maintenant l'escouade parmi les persos proches, comme le jeu ; `TestMap` vérifie un netId par perso.
  - *Aucun repère au-dessus des têtes* (`heads=0`, vue non couverte, tous les persos « hors écran ») : le Ogre de Kenshi dessine autour d'une origine mobile (`SceneManager::getRelativeOrigin`, ajout de Kenshi : `CameraClass::getCameraPos` (`0x1008F0`) l'ajoute à la position du nœud de la caméra). La matrice de vue est dans cet espace (rendu = monde − origine) ; le mod y projetait des positions du monde. L'origine est maintenant lue et intégrée à la projection (en double) ; `mapscene tetes` la donne (`origin=`) et le point projeté même hors écran.
  - *Cadres hors de l'écran* (client 1 : 692,598 et 764,598 dans un tampon de 844×465 ; après redimensionnement 266,446 dans 672×313) : les barres de Kenshi gardent leur disposition quand la fenêtre rétrécit ; la partie visible d'un portrait (et de l'image de la carte) est aussi coupée par la vue MyGUI.
  - *« cadres aux couleurs des joueurs seulement » en échec partout* : défaut du test, pas des cadres ; `mapscene barre` écrivait `owner=1:…` comme premier champ, lu comme un nom, donc sans propriétaire. La ligne écrit `frame:owner=…`.
  - *Carte pas refermée* : le second clic visait le bouton MAP, alors sous la fenêtre de gestion. `mapui close` clique le bouton de fermeture de la fenêtre (en-tête du skin `Kenshi_WindowCX`, `Event=close`), et `mapui` refuse de cliquer un bouton recouvert par une autre fenêtre.

- **Carte : marqueurs invisibles sur l'écran de carte, cadres de la barre d'escouade mal placés** (retour de l'essai en jeu du 10/10). Causes trouvées : `Widget::getInheritedVisible` de MyGUI ne dit que si les **parents** sont visibles (désassemblé : il rend `mInheritsVisible`, la visibilité propre est `mVisible`) ; le mod le prenait pour « affiché ». La fenêtre de gestion (racine) comptait donc comme toujours ouverte : vue « couverte » en permanence, ni repères au-dessus des têtes ni minicarte (aucune ligne « minimap: » dans aucun journal) ; et les cases cachées ou recyclées de la barre d'escouade recevaient un cadre, dessiné aussi par-dessus les fenêtres du jeu (lu dans un jeu lancé : 5 cadres dont 3 empilés sous la fenêtre de gestion, 94 cases suivies). Pour la carte elle-même, le journal ne disait rien : la détection est refaite et journalisée. Maintenant : « affiché » = visible et parents visibles ; widgets de la carte vérifiés par leur nom (`<préfixe>_MapImage`, `_MapScrollView`, `_MapTab`, cherchés par nom dans la fenêtre sinon) ; carte ouverte = fenêtre de gestion affichée et onglet CARTE choisi ; partie visible de l'image et des portraits = leur rectangle coupé par chaque parent ; pas de cadre pour un portrait sous une fenêtre du jeu (`LayerManager::getWidgetFromPoint`). **Conversions** : les rectangles MyGUI sont lus avec la taille de la vue MyGUI (`RenderManager::getViewSize`) et convertis à chaque image vers le tampon d'affichage de l'overlay (`SceneToDisplay`), relus dans l'image même de l'overlay quand elle tourne sur le fil du jeu (pas de retard derrière une fenêtre qu'on déplace) ; rien n'est gardé d'une image à l'autre. **Journal** (anglais, limité) : carte ouverte / fermée avec la raison, rectangle de l'image, marqueurs dessinés ou pourquoi aucun, cadres de la barre (cachés, coupés, sous une fenêtre), tampon / client / DPI / vue MyGUI quand ils changent. Commandes de debug `mapconv` (facteurs, cadres relus et dessinés, image de la carte) et `mapui open|close|maptab|state` (bouton MAP du jeu cliqué par l'entrée de MyGUI ; aucune touche supposée, M est la caméra libre) ; `mapscene carte` donne `why=`. Expérience `map` : installée par `setup_multi` (le client 2 attendait dans la file d'arrivée que l'éditeur du client 1 se ferme), vérifie vue non couverte et repères, cadres = portraits convertis, ouvre la carte par son bouton, redimensionne la fenêtre du client 1 deux fois en cours de test. Test unitaire `TestGuiToDisplay`. Pas encore lancé en jeu.

- **Dialogues sans conflit (branche `ui-dialog`)** : audit des conversations et corrections. Non testé en jeu (expérience `dialogue`, `kctest_town`, à lancer) ; tests unitaires `TestDialogue`.
  - *Ce qui existait* : bulles chez tous ; fenêtre de la conversation d'un perso de client chez ce client seulement (crochets `setInDialog` / `setResponesGUI` / `setConversationReplyGUI`), réponse rejouée chez l'hôte par `replyClicked` ; ordre « parler » donné au seul perso du client (sécurité des acteurs) ; commerce depuis une conversation ; décisions du jeu refusées chez les clients.
  - *Acteur* : `DialogReply` nomme son acteur et la réplique répondue (format changé, **protocole 33** non publié, pas de nouveau numéro de message). Le contrôle central vérifie que l'acteur est au joueur et que c'est le perso de cette conversation. `Dialog` porte le perso du joueur et le numéro de réplique, et un nouveau genre `Busy`. Nouvelle raison de refus `Result` : `Busy` (7).
  - *Une conversation à la fois par PNJ* : « parler » à un PNJ déjà en conversation avec quelqu'un d'autre est refusé avec « *Nom* est occupé : il parle déjà avec *X*. » ; chez l'hôte, un PNJ dans la conversation d'un client ne peut pas en commencer une autre ni y être entraîné (crochets `startConversation` / `startPlayerConversation`), le demandeur voit « occupé ».
  - *Fin propre* : bouton « Partir » ; départ d'un joueur ; combat, TP (> 40 m), K.-O., mort, perso disparu ou conversation finie sans fermeture : l'hôte appelle `Dialogue::endDialogue` (nouvelle fonction `0x674830`, prologue vérifié) et la fenêtre se ferme. Vitesse 3 : une réponse à une réplique dépassée est ignorée.
  - *Pause* : le jeu met en pause dès qu'une fenêtre de conversation s'ouvre (`userPause(true)` dans `0x727820`, lu au désassembleur). Décision : pas de pause en coop ; la conversation d'un client n'ouvre rien chez l'hôte, et la propre conversation de l'hôte voit sa pause levée tout de suite quand d'autres joueurs sont là.
  - *Recrue* : un PNJ déjà suivi qui entrait dans la faction du joueur restait un PNJ pour le mod (jamais à personne). Il devient perso d'escouade et va au joueur qui lui parlait (par identité), sinon à l'ancienne règle. Quitter l'escouade : pas traité (voir FONCTIONNALITES).
  - Commandes de test `dialogs`, `npcevent`, `talkto`, `answer leave` ; `dialog` donne `actor=` et `turn=`.
- **Atelier : recherche, fabrication, mines et machines, énergie (branche `ui-research`, protocole 34, pas encore lancé en jeu).**
  - *Ce qui existait* : rien pour la recherche ni la fabrication. Les ordres de travail (machine, mine) passaient par l'hôte ; l'inventaire d'un établi n'était synchronisé que fenêtre ouverte ; chez le client, une mine travaillée par 3 persos affichait 0/3 (le jeu n'ajoute les opérateurs que par l'IA, coupée chez lui) et son jeu calculait seul l'énergie.
  - *Recherche* : la recherche de la faction (technologies connues, plans lus, file, avancement, niveau du banc) est celle de l'hôte, imposée au jeu du client (technologies terminées chez lui aussi : menu de construction et listes de fabrication débloqués). Ajouter / retirer dans la fenêtre Recherche et « apprendre » un plan deviennent `ResearchRequest` (57) au nom d'un perso du joueur ; l'hôte vérifie (banc, artefacts et livres, pas déjà en file ni connue, plan dans l'inventaire de ce perso) et paie une seule fois ; réponse `Result` en français.
  - *Établis* : ordres de fabrication (objet, matériau, répéter) et leur avancement : ceux de l'hôte ; ajouter / retirer / répéter dans la fenêtre Fabrication deviennent `MachineRequest` (59), exécutés dans l'ordre d'arrivée (le dernier l'emporte, partout).
  - *Machines et mines* : opérateurs (nombre et noms), barre de progression, quantité produite ; l'inventaire de chaque machine suivie (celles des joueurs et celles qu'un perso de joueur travaille, à 1500 unités) est synchronisé chez tous, ouvert ou non.
  - *Énergie* : sortie, marche, charge des batteries, alimenté ou non, totaux du panneau de la ville : ceux de l'hôte ; le client ne calcule plus le réseau de ces villes ; interrupteurs marche / batterie : demande à l'hôte.
  - *Compétences* : déjà calculées une fois chez l'hôte ; le client annonce maintenant les passages de niveau de ses persos (« * Nom : Science 5 -> 6 »).
  - **Protocole 34** : messages `Research` (56), `ResearchRequest` (57), `Machines` (58), `MachineRequest` (59), règles d'autorité `OwnCharacter` pour les demandes, `HostOnly` pour les autres. 20 fonctions du jeu ajoutées à `kFunctions` (prologues vérifiés ; faits moteur dans docs/MOTEUR.md, « Atelier »). Nouveaux fichiers `common/src/protocol_workshop.cpp`, `common/src/session_workshop.cpp`, `plugin/workshop.cpp`. Test unitaire `TestWorkshop`, expérience `research` (« recherche : … », « fabrication : … », « mine : … », « energie : … ») et commandes de debug `research`, `researchknown`, `researchenabled`, `researchpick`, `researchreq`, `researchprogress`, `blueprintreq`, `craftlist`, `machine`, `machineinv`, `machinereq`, `machinegive`.
  - *Pas fait* : réordonner la file de recherche à la souris (l'ordre de l'hôte revient) ; totaux de stockage et de nourriture / eau de la base (aucun panneau du jeu trouvé pour eux).

- **Plantage d'un client pendant le soak de 20 min (14:54:03, vitesses 2-3) : analysé, cause exacte non trouvée, durcissements et meilleur rapport.** Non testé en jeu.
  - *Ce que dit le dump* (`crashDump1.0.68_x64.dmp`, thread principal 9420) : accès à `0x2E6465A9` dans `kenshi_x64.exe+0x412D96` (lecture du `hand` d'un `PortraitData*` à +0x38), appelé par le dessin d'une case de la **barre d'escouade** (`PortraitMainItemBox` de `MainBarGUI`, rappel MyGUI `requestDrawItem`, `0x426C10` → `0x415150`), lui-même dans la frame du jeu (`mainLoop_GPUSensitiveStuff`, avant le tick du mod). La donnée de la case 5 (`MyGUI::Any` de type `PortraitData*`, vtable du `Holder` intacte) valait `"ied.\0\0\0\0"` : la fin d'une infobulle médicale du jeu (« Vous ne pourrez pas donner de coups de pied. » réécrite par « Votre Poitrine est en train de guérir. », trouvées juste à côté dans le tas). Le tas du jeu était donc déjà corrompu (mémoire libérée puis réutilisée) ; le dump ne contient pas assez de tas pour dire qui l'a libérée.
  - *Pas en cause* : rien ne s'est figé (le dump est daté de 14:54:03, 13 s après le dernier `buildplace`, pendant que le banc posait chez l'hôte ; les 2,5 min sans réponse sont la boîte « Kenshi has crashed »). Le `CRASH` de 14:56:24 du journal est un **second** plantage, à la sortie du processus (destructeurs statiques, `MSVCR100+0x20C4B`). `CheckPlacement` ne fait que lire (mêmes appels et arguments que `placementVerification`, vérifiés dans le code du jeu) et tourne sur le thread du jeu (journal : tous les ticks sur le thread 9420). Les vitesses non entières de `ClockSync` : `setFrameSpeedMultiplier` ne fait que les ranger, et les boutons de vitesse de la barre les comparent (`==`) sans s'en servir d'indice.
  - *Durcissements* : `RestoreSelection` (donc `WithSelection` / `KeepSelection`) ne remontre plus dans la barre d'escouade une escouade sauvegardée avant l'appel qui n'appartient plus au joueur (elle a pu être supprimée avec son dernier membre par un changement d'escouade : la barre aurait été construite depuis de la mémoire libérée). Le crochet des marqueurs de carte sur `PortraitMainCellView::update` (`0x415150`, le chemin même du plantage) ne retient une case que si sa vtable est celle de `PortraitMainCellView`, et ne lit jamais sa `PortraitData` (vérifié : le destructeur virtuel passe bien par `0x426450`, crocheté, donc pas de case morte gardée). `CheckPlacement` : seulement avec une partie chargée, et aucun appel virtuel sur un objet dont la vtable ou la fonction n'est pas dans `kenshi_x64.exe`.
  - *Rapport de plantage* : un plantage dans la frame du jeu est rattrapé par le `catch(...)` de sa boucle principale, qui écrit le dump : aucun filtre d'exception non gérée ne le voit, d'où le `CRASH` trompeur de 14:56:24. Le mod accroche maintenant ce rapporteur (`writeCrashDump`, `0x744D20`, prologue vérifié) et écrit le vrai `CRASH (game crash reporter)` avant lui, avec les registres et **ce que faisait le mod** (`phase='game main loop …'` ou `tick: debug commands`, `tick: session`…).
- **Intégration (branche `integration`) : sécurité des acteurs, file d'attente, diplomatie, carte et objets au sol réunis, protocole 33 pour tout.** Numéros des messages : `JoinQueue` 72, `BagBind` 80, `MapMarkers` **82** et `MapPing` **83** (80 et 81 sur leur branche : 80 était déjà `BagBind`), `Diplomacy` 85, `Result` 90. La table d'autorité `kMessageRules` a maintenant une règle pour **chaque** message : rôles `Handshake` (`Hello`) et `HostOnly` (tous les messages hôte → client : `Diplomacy`, `MapMarkers`, `JoinQueue`, `BagBind`... refusés d'un client, journalisés comme les autres refus), et un intervalle minimal par règle (`MapPing` : un toutes les 0,5 s par joueur, les autres ignorés sans refus, compteur `rateLimited`). `PeekType` n'accepte plus que les messages de l'énumération (`MsgName`, un `case` par message, C4062 en erreur : un nouveau message ne compile pas sans son nom). Sans la règle `MapPing`, l'hôte aurait refusé les pings des clients. Un dépôt d'objet depuis un sac à dos porté est vérifié au nom de son porteur. Nouveau test unitaire `TestMessageRules`. Pas encore lancé en jeu.
- Nouveau document docs/INSPIRATION_PZ.md : ce que le multijoueur de Project Zomboid fait (idées seulement), et les améliorations classées pour KenshiCoop.
- **Soak de 20 min (13:17–13:37, vitesses 1/2/3) : 3 échecs corrigés** (aucun plantage). Non testé en jeu.
  - *Membres d'escouade manquants chez le client, et des dizaines de corps en double* : quand un perso de l'escouade meurt, le jeu le range dans l'escouade `__DEAD_SQUAD__`, ce qui change son handle (conteneur seul : type, index et serial restent). Chez l'hôte, le nouveau handle part au client (`Rehandle`). Mais le client rejoue la mort et son propre jeu déplace aussi sa copie : son handle local change, l'alias du mod pointe sur un handle mort, « stand-in … is gone here: it can be recreated », et le mod recréait une doublure… qui mourait à son tour (54 copies du membre 4 chez le client en 20 min, le perso du client lui-même jamais recréé : « local order dropped: unknown character », « no such squad member »). Le client retrouve maintenant la copie déplacée par son serial (`FindMoved` : même type et serial, autre conteneur, un seul candidat) et la suit (`Exists`, et au retour d'une image hors jeu). Journal : « our copy … changed squads here (now …): followed ».
  - *Escouade décalée en pause (17,3 / 11,5 / 1,4 unités à p2_3)* : ni la vitesse ni la pause : chez le client, `latest` et `rendered` étaient exactement la position de l'hôte. Les trois étaient **à terre** (drapeau 4 des deux côtés) : deux K.-O. tombés là où était leur copie en pleine frappe, avant que le déplacement vers la place de l'hôte prenne, et jamais remis (seuil de 25 unités) ; ils sont ensuite morts là, 11,5 unités à côté jusqu'à la fin. Un corps K.-O. à plus de **5** unités (au lieu de 25) de celui de l'hôte est relevé et recouché à sa place, 3 fois au plus tant qu'il reste à terre. Le test traite aussi en corps au sol un perso à terre jambe brisée (drapeau 4 chez l'hôte, `vflags` 0 : 1,4 unité).
  - *Heure de jeu à 0,01 h* : vraie dérive, pas un arrondi (l'heure est écrite au millième ; écart stable à 0,002 h puis 0,010 h dès p11_30 et jusqu'à la fin). Le client ne recalait jamais son heure : il suivait seulement la vitesse et la pause, chacune reçue un peu en retard. `ClockSync` (`common/include/kc/clock.h`) : le client estime l'heure de l'hôte (dernière reçue + temps écoulé + demi aller-retour) et tourne ±2 % (±5 % au-delà de 0,006 h) tant que l'écart dépasse 0,003 h, jusqu'à moins de 0,0005 h. Journal : « clock: … behind the host's: running +2 % ». Le test `soak` accepte donc une vitesse du client à 6 % de celle de l'hôte. Test unitaire `TestClockSync`. Pas de changement de protocole.

- **Marchands ambulants, sacs à dos et animaux (protocole 33, pas encore lancé en jeu)** : un marchand sans bâtiment (caravane, nomade) vend depuis les sacs à dos portés par son escouade (constructeur de `ShopTrader`) ; le mod synchronise maintenant le contenu du sac porté par chaque personnage suivi, joueurs compris (nouveau message `BagBind`, le sac est un inventaire comme un autre), et l'hôte ouvre le commerce avec ces sacs comme comptoirs (`TradeCounter.ownerNetId`). Achats et ventes payés chez l'hôte, même pendant que la caravane marche ; la fenêtre se ferme si le marchand ou une bête du stock est à terre, meurt, se bat, s'éloigne ou n'est plus suivi. Vol dans le sac ou l'inventaire d'une bête de somme de PNJ depuis un client : crime décidé chez l'hôte (fenêtre de pillage locale, plus de faux commerce sur une bête). Un personnage qui rejoint l'escouade juste après la réponse d'un seul joueur dans une conversation (animal acheté, recrue) est à ce joueur ; un animal donné à un joueur passe dans l'escouade de son personnage (il le suit). Une seule reprise des personnages au retour d'un joueur, celle de crash-rejoin (par compte Steam, nom sans Steam). Commandes de debug `caravan`, `caravanbeast`, `caravanopen`, `squadanimals`, `bag`, `invmove bag:<qui>` ; expériences `caravan` (« marchand ambulant : ... ») et `pets` (« animaux : ... ») ; test unitaire `TestTravellingTrade`.
- **Administration de l'hôte** (fenêtre Multijoueur, section sous la liste des joueurs) : pour chaque joueur et pour « Tout le monde » (l'hôte compris) : **mode dieu** (case à cocher, retenu par joueur — compte Steam, sinon nom — et réappliqué 5 fois par seconde, donc il tient aux changements de zone et aux reconnexions), **TP** (joueur → hôte, hôte → joueur, joueur → autre joueur, joueur → point marqué = dernier clic droit de déplacement de l'hôte, ou coordonnées), **XP** (une compétence ou toutes, en points ou en niveaux, par la fonction du jeu `increaseStat` : réplication par la synchro habituelle), **soigner** (et réveiller un K.-O.), **argent commun**. Confirmation par un second clic pour un TP de perso à terre et au-delà de 1000 points ou 10 niveaux. Chaque action est journalisée (« admin: … ») et le joueur concerné reçoit un message en français (« * L'hôte t'a téléporté près de lui. »). Commandes équivalentes `admin god|tp|xp|heal|money|list` (console et canal de debug, `admin state` pour les tests) ; `god`, `heal`, `xp <id> <n>` passent par là. Refusé sur un client. Nouveau `Session::SendNotice` (un `Chat` de l'hôte à un seul joueur, message déjà existant : **pas de changement de protocole**). Nouveau module `common/src/admin.cpp` (syntaxe, calcul de l'XP du jeu, registre du mode dieu) et test unitaire `TestAdmin` ; expérience `admin` réécrite (dieu sous les coups et après reconnexion, XP et niveaux identiques chez le client, trois TP, soin, argent). Faits moteur : formule et limite de 20 d'`increaseStat`, point du sol reçu par `playerMove` (docs/MOTEUR.md). Aucune nouvelle fonction du moteur. Pas encore lancé en jeu ; la faim n'est pas remise à zéro (échelle inconnue).
- **Sécurité des acteurs (bug critique)** : quand le joueur 2 voulait dormir dans un lit ou parler à un PNJ, c'est le perso de l'hôte qui y allait. Cause trouvée au désassemblage : `unselectAll` (`0x7F8DA0`) resélectionne le perso principal et `objectSelected(…, false)` refuse de retirer le dernier ; « tout désélectionner puis sélectionner le perso du client » laissait donc le perso principal de l'hôte sélectionné, et l'ordre partait aux deux ou au plus proche (lit, conversation : `addTaskNearestSelectedCharacter`). Sûr seulement quand l'hôte n'avait rien sélectionné (comme dans les tests). Maintenant : sélection faite **exactement** des acteurs et vérifiée (sinon refus sans rien exécuter), puis sélection, escouade affichée, panneau de détails et `hand` principal de l'hôte remis dans le même appel (`kenshi::SelectExactly` / `WithSelection`, aussi pour `KeepSelection`, ramasser, la caméra des tests et `selectset`) ; témoin `SAFETY:` si un perso de la sélection de l'hôte reçoit quand même une tâche. Contrôle d'autorité central (`session_authority.cpp`, table `kMessageRules`) avant chaque gestionnaire : l'acteur nommé par une demande (ordre, contenant, apparence, dépôt d'objet) doit être un perso de **ce** joueur (un dépôt depuis un sac à dos porté, `BagBind`, est vérifié au nom de son porteur : `Session::DropActor`), revérifié à l'exécution ; refus journalisés (`auth: … refused`, `refused: actor N not owned by player P`), comptés par joueur et par règle (décroissance 60 s), réponse en français. Le client filtre avec la même table (« Action refusée : ce personnage n'est pas le tien. »). **Cible vérifiée** pour chaque numéro de tâche (`TaskTargetAllowed`) : corrige le plantage de l'hôte de `stress4` (`npcreq <perso> 2 a` : « construire » via addTaskNearest sur un PNJ → plantage dans `Character::addJob`) ; handle périmé ou destination qui n'est pas un bâtiment écartés ; changement d'escouade seulement vers celle d'un joueur. **Protocole 33** : nouveau message `Result` (90, H→C : fait / rejeté avec raison et texte ; le client retire la tâche qu'il avait affichée d'avance). Tests unitaires `TestTaskTargets`, `TestActorSafety` ; expérience `actorsafety` et commandes de debug `selected`, `charstate`, `actorstats`, `results`, `forgeorder` (pas encore lancée en jeu).
- **Arrivées simultanées : file d'attente** (essai `join4` du 10/10, 13:57) : 3 clients rejoignant en même temps, seul Joueur4 entrait ; Joueur2 et Joueur3 étaient retirés 5 min plus tard (« joining took too long »). Cause : l'hôte envoyait une sauvegarde à Joueur2 et Joueur3 (13:57:04.7), puis Joueur4 arrivait (06.5) ; l'hôte créait son personnage et sauvegardait de nouveau pour lui seul. L'empreinte du monde (les handles de l'escouade) changeait : la sauvegarde des deux premiers ne correspondait plus au monde de l'hôte, ils restaient en chargement (« world ready: 8 player characters », empreinte différente de celle attendue) jusqu'au délai. Les joueurs passent maintenant **un par un** : sauvegarde neuve pour le joueur dont c'est le tour (avec les persos de tous ceux arrivés avant), téléchargement, chargement, éditeur de personnage ; les autres attendent, connectés, et voient « File d'attente : position 2/3 — en attente de Joueur2 (création du personnage)… », mis à jour en direct ; l'hôte voit la file (panneau, fenêtre Multijoueur, `status`). Le délai de 300 s part du début du tour. Fin du tour : dans le monde et éditeur fermé, ou départ / plantage / exclusion (le suivant commence aussitôt) ; un joueur qui quitte la file la fait avancer. Plusieurs éditeurs ouverts en même temps : la pause dure jusqu'à la fermeture du dernier (« nobody is in the character editor any more: the game resumes »). **Protocole 33** (nouveau message `JoinQueue` = 72). Nouveau test unitaire `TestJoinQueue`. `join4` (`coop_test.py`) attend la file (positions vues, tous entrent l'un après l'autre, un seul chargement à la fois, file affichée chez l'hôte) ; `setup_multi` ferme l'éditeur de chaque joueur à son tour. `status` (debug) donne `queue=`. Pas encore lancé en jeu.
- **Diplomatie et réputation (branche `diplomacy`)** : audit du lot B (relations de la faction du joueur, primes : déjà là) et ajout de ce qui manquait. Nouveau message `Diplomacy` (85, **protocole 33**) en trois parties : relations entre deux factions qui ne sont pas celle du joueur (seules les paires changées depuis que l'hôte héberge : guerres, alliances déclarées après la mort d'un chef, un dialogue, une campagne), personnages uniques (`UniqueNPCManager` : chef mort, vivant, emprisonné, par les joueurs ; les états du monde du jeu n'en dépendent qu'avec les relations du joueur) et villes (faction propriétaire, variante : ville prise, détruite). Le client les impose à son jeu toutes les 3 s (`TownBase::setOverride` / `setFaction` pour les villes, table des uniques écrite comme au chargement). Nouvelle fenêtre **Diplomatie** (Ctrl+Shift+F, en français, hôte et clients) : relations de la faction du joueur avec l'état allié / neutre / ennemi / en guerre calculé comme le jeu (allié : alliance ou ≥ 50, ennemi : ≤ −30), rang, réputation, primes et peines de l'escouade (que le jeu du client n'a pas), guerres, chefs, villes. Nouvelles en français dans le panneau des deux côtés (« Diplomatie : … vous considère maintenant comme ennemi », « Prime : … est recherché par … », « Monde : Tinfist est mort (de la main des joueurs) », « Monde : Squin appartient maintenant à … »). Trois fonctions ajoutées à `kFunctions` (prologues vérifiés) : `operator[]` de la table des uniques `0x349950`, `TownBase::setOverride` `0x9FE7A0`, `TownBase::setFaction` `0x9287D0` ; faits moteur dans docs/MOTEUR.md (« Diplomatie »). Test unitaire `TestDiplomacy` (et cas `TestWire` / `TestFuzz`), expérience `diplomacy` (`kctest_town`, vérifications « diplomatie : … ») et commandes `diplo`, `diplopair`, `setdiplopair`, `unique`, `setunique`, `town`, `settownowner`, `diplosync`. Rien de tout cela n'a encore tourné en jeu.
- **Carte, minicarte, repères des joueurs et pings** (**protocole 33**, nouveaux messages `MapMarkers` = 82 et `MapPing` = 83 (80 et 81 au départ, renumérotés : 80 est `BagBind`) ; pas de version publiée) : une couleur par joueur partout (`kc::PlayerColor`). Sur la carte du jeu (onglet CARTE, bouton MAP), tous les persos de tous les joueurs, même lointains, et en rouge les escouades hostiles qui nous visent (raid ou vague d'attaque du jeu contre notre faction, escouade en combat contre un des nôtres, ennemis à ~250 m), avec légende et infobulles (nom, joueur, distance). Minicarte ronde (fond : la carte du jeu, nord en haut ou tournante, zoom molette et + / −, coin au choix, Ctrl+Shift+N). Repère au-dessus de la tête des persos des joueurs, cadre coloré de leur portrait dans la barre d'escouade. Pings (clic molette ou Alt+clic sur la carte, la minicarte ou le sol ; Maj / Ctrl pour danger, butin, à l'aide) chez tout le monde pendant 10 s, 1 toutes les 0,5 s et 5 au plus par joueur. L'hôte envoie le flux 3 fois par seconde ; réglages dans la fenêtre Multijoueur (« Affichage »), retenus dans `KenshiCoop.ini`. Cinq fonctions du jeu ajoutées à `kFunctions` (projection de la carte, couleur des points, campagne d'une escouade, mise à jour et destruction d'un portrait). Test unitaire `TestMap`, expérience `map` et commandes `mapfeed`, `mapscene`, `mapproj`, `ping`, `pings`. Pas encore lancé en jeu.
- **Objets lâchés par glisser-déposer invisibles chez le client** : l'hôte n'annonçait un objet posé que depuis `CharacterHuman::dropItem`, et seulement s'il était déjà « physique ». Le glisser-déposer passe par `Inventory::dropItem` (`0x745D90`), qui fait lâcher le propriétaire de l'inventaire : une bête de somme (`CharacterAnimal::dropItem`, sans crochet), un coffre (le perso qui l'utilise, l'objet encore marqué « dans un inventaire » à ce moment-là), un sac à dos. Et le jeu peut créer le corps physique quelques images après le dépôt : l'objet n'était alors jamais annoncé, sans rattrapage. Maintenant : crochets sur `Inventory::dropItem` et `CharacterAnimal::dropItem`, un objet compte comme posé dès qu'il est actif hors inventaire (`ItemInWorld`), et l'hôte relève deux fois par seconde les objets à 30 m des persos des joueurs (apparu là où un perso regardait : annoncé ; disparu : ramassé ; pile changée : annoncée de nouveau). Chaque objet n'est annoncé qu'une fois ; le client ignore les répétitions, adopte sa propre copie de la sauvegarde au lieu d'en créer une deuxième et retire un doublon si sa zone charge après coup. Un client qui lâche depuis un coffre ouvert le demande à l'hôte (son perso le plus proche le pose) ; avant, la demande cherchait l'objet dans le perso. Nouveau fichier `plugin/ground.cpp`, test unitaire `TestGroundDrops`, expérience `grounddrop`, commandes `uidrop`, `fetchitem`, `groundall`, `groundcopy`, `groundstats`. Pas de changement de protocole (mêmes messages `Ground` ; une pile changée = ramassée puis reposée). Pas encore lancé en jeu. Fusion avec la sécurité des acteurs : un dépôt demandé par un client nomme toujours un perso de ce joueur (son perso ou le porteur du sac ; depuis un coffre ouvert, un corps ou le sac d'un PNJ : son perso le plus proche, dans `InvOp::toNetId`, même format), vérifié par `Authorize` puis par `HostInvOp` (à moins de 160 de la source) ; le coffre est vidé par ce perso. Le test `TestGroundDrops` vérifie aussi les dépôts forgés (perso de l'hôte, aucun, perso trop loin).

- **Client qui plante puis revient** : l'hôte ne coupait pas seulement les jeux plantés mais aussi ceux figés plus de 5 à 15 s (chargement d'une zone) ; un fil de maintien répond maintenant au réseau quand le jeu est figé (au plus 90 s), un jeu tué est toujours vu en 5 à 15 s. Au départ d'un joueur (propre ou non), l'hôte efface tout ce qui porte son id (ordres, échanges d'inventaire, conteneurs, commerce, bâtiments, portes, conversations : un nouveau joueur reprenant l'id pouvait hériter d'un conteneur « ouvert » ou d'un commerce), arrête ses persos là où ils sont et les lui rend s'il revient. Un joueur relancé avant la coupure était renommé « Nom 3 » (le nom était vérifié avant de fermer l'ancienne connexion) et pouvait recevoir un nouveau personnage : l'ancienne connexion est fermée d'abord ; sans Steam, même nom sur une connexion muette depuis 2 s. Le jeu d'un client en session ne sauvegarde plus (il aurait écrit le monde de l'hôte sur une partie du joueur). Nouveau test unitaire `TestCrashRejoin`, nouvelle expérience `crashrejoin` (pas encore lancée en jeu). Pas de changement de protocole.

- **Tests à 4 joueurs (`stress4`, `join4`)** : `setup_multi` lance l'hôte + N clients (faux Steam id et nom distincts, arrivée l'un après l'autre ou en même temps, éditeur fermé). `stress4` : clients répartis dans des régions lointaines qui changent, clics en rafale à graine par client, mode dieu, deux clients sur le même corps, départ / retour d'un client, objet marqué compté partout (pas de duplication), mémoire et temps de frame. `join4` : 3 arrivées simultanées, chacun voit tout le monde, puis départs. Le banc vérifie la RAM libre avant chaque lancement (< 2 Go : arrêt propre) et tue les jeux qu'il a lancés sur toute erreur ; commandes envoyées à un même jeu sérialisées (plusieurs fils). Côté mod (debug seulement, `commands=1`) : variable `KC_PLAYER_NAME` (nom du joueur pour ce jeu, non écrit dans l'ini), commande `ownidx [id]` (indices d'escouade des persos d'un joueur), `name=` dans l'état dumpé. Rien ne supposait un seul client côté session (ids 2 à 8, `kMaxPlayers` = 8). Pas de changement de protocole. Pas encore lancé en jeu.
- **Poses de bâtiments vérifiées comme le mode construction** (farlong : la tente du client, téléporté à 54612,40576, avait été bâtie dans un lac d'acide, « built … at 54612.5,100.0,40575.9 »). Lu dans le jeu : le clic du mode construction (`0x4E27E0`) n'accepte un endroit qu'après `PreviewBuilding::placementVerification` (`0x4DD240`) et `buildingPlacementUpdate` ; un client n'envoie donc que des poses que son jeu a acceptées (seuls les aperçus acceptés arrivent à `createBuildings`). L'hôte refait maintenant ces vérifications sur chaque pose demandée par un client (`CheckPlacement`) : ville (même calcul que le jeu : `getNearestTown`, `withinBordersRange`, `getNearestWithinItsRadius`), intérieur d'un bâtiment (`UtilityT::isIndoors`), autre bâtiment à moins d'1 m, sol dans l'eau ou l'acide (`Footprint::isGroundValid` : sol sous 98, surface à 100 ; `UtilityT::getTerrainHeight`), pente (normale < 0,2). Refusée : rien n'est bâti nulle part, journal « [X] placement of … refused: invalid spot (in water or acid …) », le joueur reçoit « Impossible de construire X ici : dans l'eau ou l'acide. ». Six fonctions du jeu ajoutées à la table (prologues vérifiés au démarrage, `check_functions.py`). La commande `buildplace` passe par la même vérification (`err invalid spot: <raison>`) ; nouvelles commandes `buildplaceat`, `buildcheck`, `buildcheckat`, `groundat`, `chatlast`, option `force` (client : laisse l'hôte refuser). Tests : `build`, `construct`, `farlong`, `soak` cherchent un endroit que le jeu accepte (`place_valid`), les TP lointaines (`far`, `fartp`, `farlong`, `soak`) visent la terre ferme (`teleport_dry`) ; nouvelle expérience `placevalid` (le lac d'acide refusé par le client et par l'hôte, rien de bâti, un endroit sec accepté) ; test unitaire de la session (pose refusée par l'hôte : rien de bâti, avis au client). Pas de changement de protocole. Pas encore lancé en jeu ; collision exacte des empreintes, personnages dans le passage, nœuds d'usage et étage restent vérifiés seulement par le mode construction du joueur.

- **Ramassage par un client (test `groundpick` 0/3)** : l'hôte était en pause pendant tout le test. Le client venait d'arriver et son éditeur de personnage restait ouvert (« Coucoudz 2 is making their character: game paused », jamais levé) : jeu en pause, personne ne marche, ni l'ordre « ramasser » du jeu ni l'ordre de marche. Le test valide maintenant l'éditeur (`editdone`) comme les autres. Côté hôte, une demande de ramassage en attente ne compte plus le temps de pause (journal « the game is paused, the character will walk there once it resumes ») au lieu d'échouer au bout de 30 s. Le journal d'échec affichait une distance fausse (52095) : position de l'objet non lue ; corrigé.

- **Commerce chez le client (test `trade` 6/13)** : après l'image « hors jeu » que le jeu fait en ouvrant la fenêtre, le client oubliait toutes ses doublures (remis par 8a3a003) ; elles redevenaient des inconnus, supprimés 5 s plus tard (« removed 13 local character(s) »), le marchand compris : fenêtre fermée, vente payée localement mais jamais envoyée à l'hôte. Seules les doublures dont l'objet local a disparu sont oubliées maintenant. Et la fenêtre du marchand restait parfois vide (`tradelist merchant` → 0) alors que les comptoirs avaient le stock de l'hôte (intermittent depuis le 09/10) : le client vérifie qu'elle montre le stock et la rouvre sinon (3 fois au plus, « trade window shows none of the shop's N stacks: opened again »).
- **Test de stabilité `soak` refait** (`tools/coop_test.py soak --minutes 20`, voir docs/TESTS.md) : vitesses 1 → 2 → 3 → 2 → 1, changements rapides de vitesse et pauses, demandes de vitesse / pause du client (ignorées : le client suit l'horloge de l'hôte), activité des deux côtés (déplacements, combat, K.-O. et fouille, commerce, bâtiments, TP loin et retour). Toutes les 30 s : plantage, comparaison en pause, aller-retour d'une commande et mémoire des deux jeux (ÉCHEC au-delà de +40 % après 2 min). Aucune nouvelle commande de debug. Pas encore lancé en jeu.
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
