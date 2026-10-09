# Bugs signalés, à traiter

Remontés par les parties entre amis. Chaque entrée garde la date et ce qu'on a vu.

## 10 octobre 2026

- **PRIORITÉ — Les 3 clients plantent juste après une nouvelle prime** (10/10, 00:51:50–00:52:00 ; l'hôte
  continue). Juste avant : Geoffrey crochète un coffre-fort (crime), l'ouvre, puis devient « recherché par
  les Cités Unies pour 1000 cats » à 00:51:43 (sa première prime). Suspect : la synchro des primes (lot B)
  crée chez chaque client la nouvelle entrée de prime avec l'operator[] du jeu (0x5E7EE0), jamais testé
  en vraie partie. À confirmer avec le KenshiCoop.log / crashDump d'un client.

- **Ville lointaine en « bâtons rouges »** : un client parti seul dans une autre ville voit ses bâtiments
  comme des chantiers (bâtons rouges), alors que l'hôte la voit normalement.
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
- **Relations corrigées en boucle** : chez rob, 3 valeurs de relations ou de primes reprennent sans arrêt
  la valeur du jeu local et le mod les remet chaque seconde (journal « factions: 3 relation/bounty values
  set to the host's »). Trouver qui les change chez le client et le bloquer.
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
- **Porter un corps (vu par un client)** : le corps porté par l'hôte apparaît debout sur sa tête au lieu
  d'être sur l'épaule, et quand l'hôte le pose, il s'envole. Le correctif de la 0.2.0 (ne plus déplacer un
  perso porté) ne suffit pas : chez le client, la mise sur l'épaule ne prend pas (le corps n'est pas en
  ragdoll quand on l'attache), et à la pose le corps est relâché à une position fausse, ce qui le projette
  (comme un ragdoll déplacé juste avant sa chute). À reprendre avec un test en jeu dédié (porter puis
  poser, regardé chez le client).
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
