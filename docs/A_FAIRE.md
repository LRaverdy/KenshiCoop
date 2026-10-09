# Bugs signalés, à traiter

Remontés par les parties entre amis. Chaque entrée garde la date et ce qu'on a vu.

## 10 octobre 2026

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
- **Fenêtre du marchand chez l'hôte** : en 0.2.x, la fenêtre de commerce s'ouvre encore chez l'hôte et pas
  chez le client (au moins dans un des cas de dialogue ou de clic).
- **Plantage après un resync** : après un resync, un client n'avait plus les cartes de ses personnages dans
  la barre d'escouade, puis son jeu a planté.
- **Étage qui ne change pas tout seul** : chez un client, l'étage affiché ne suit pas automatiquement quand un
  perso monte ou descend dans un bâtiment (comme le fait le jeu en solo). Piste : chez le client, les persos
  sont placés à la position de l'hôte au lieu de prendre l'escalier eux-mêmes, et l'étage courant du
  personnage (qui pilote l'affichage) n'est pas mis à jour.
- **Tâches qu'un client ne peut pas supprimer** (« suivre »…) : toujours là après la 0.2.0. Le bouton stop
  arrête maintenant la tâche en cours chez l'hôte, mais retirer une tâche de la liste des tâches du perso
  (panneau Tâches, clic sur la croix) ne passe que par le jeu du client : l'hôte la garde, et elle revient.
  À faire : intercepter la suppression d'une tâche (et « tout effacer ») côté client et l'exécuter chez
  l'hôte, comme les ordres.
- **« TP vers moi » sur un joueur loin : il est éjecté** de la partie. Pistes : son jeu charge d'un coup la
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
- **Fouille d'un cadavre : l'objet se duplique puis s'annule** (Geoffrey, 10/10 vers 00:35). Le journal de
  l'hôte montre des déplacements d'équipement refusés : « boots -> boots 0,0 : no room ». Quand le joueur
  glisse des bottes du corps sur son perso qui en porte déjà, son jeu fait un échange (les anciennes sortent,
  les nouvelles entrent) ; le mod envoie les deux déplacements dans le mauvais ordre, l'hôte refuse d'abord
  « pas de place », renvoie l'état réel, et l'objet revient : d'où l'aspect dupliqué puis annulé.
  À faire : envoyer d'abord ce qui libère une place, et en cas de place prise, laisser l'hôte poser l'objet
  ailleurs dans l'inventaire. Il y a aussi des refus « refused an inventory move from player 3 » à éclaircir.
- ✅ **Porter un corps (vu par un client)** — corrigé, à vérifier en jeu (`coop_test.py carry`). Le corps
  porté apparaissait debout sur la tête du porteur, puis s'envolait à la pose (PNJ comme persos de joueurs).
  Cause : `pickupObject` (0x5CFF90) refuse sans rien dire un corps en ragdoll (+0x3d4), or un client garde
  les corps KO en ragdoll ; la mise sur l'épaule échouait donc chez lui (journal « ok » trompeur) et le
  code de posture se battait avec le corps. À la pose, le corps était ensuite redressé ou téléporté
  pendant que son ragdoll démarrait, d'où la projection. Correctif : le ragdoll est retiré juste avant le
  ramassage, le succès est vérifié (sinon nouvel essai chaque seconde) ; à la pose, le corps passe en
  ragdoll tout de suite et on ne le touche plus pendant 2 s.
- **Un PNJ invisible en combat chez 2 clients sur 3** (10/10 vers 00:38). Les rapports des clients le
  confirment : chez nass4, 1 PNJ de l'hôte « pas encore là » ; chez Geoffrey, 31 en permanence ; rob, 0.
  Ces PNJ n'existent pas dans le jeu du client, et sa recréation (modèle + faction) échoue. Voir pourquoi
  (PNJ unique, modèle introuvable, zone pas chargée). Les mêmes rapports montrent aussi des persos décalés
  de 300 à 466 unités (« max offset »), à éclaircir.
- **Le perso de l'hôte en passif attaque quand un ami attaque** (10/10). L'hôte se met en passif ; quand un
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
- **Ramasser (voler) un objet par terre ne marche pas chez le client 4** (nass4, 10/10). À croiser avec le
  journal : l'ordre « ramasser » part bien à l'hôte, qui doit retrouver le même objet par type et endroit
  (à 15 unités près). Pistes : l'objet de la sauvegarde a un autre handle chez lui et n'est pas retrouvé,
  ou le vol (objet d'un magasin, d'une faction) n'est pas traité comme tel côté hôte.
  Journal : « [nass4] pick up Bol en Bois -> FAILED » (puis Cuivre, Matériaux Construction), tous refusés
  chez l'hôte : l'hôte ne retrouve pas l'objet visé (même type à moins de 15 unités). Probablement le
  décalage de position de nass4 (rapports « max offset » de 450 unités) : son jeu voit les objets à un
  autre endroit que l'hôte.
