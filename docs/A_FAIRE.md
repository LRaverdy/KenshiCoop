# Bugs signalés, à traiter

Remontés par les parties entre amis. Chaque entrée garde la date et ce qu'on a vu.

## 10 octobre 2026

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
