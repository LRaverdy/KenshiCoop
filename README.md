# KenshiCoop

Mod multijoueur coopératif pour **Kenshi 1.0.68 (Steam, x64)**. L'hôte fait tourner le monde ; les
autres joueurs rejoignent **le monde de l'hôte** (pas le leur) et chacun y contrôle son propre
personnage. Tout ce qui compte (dégâts, KO, morts, IA, météo, heure, loot) est décidé chez l'hôte
et appliqué à l'identique chez les clients.

## Installation

1. Fermer Kenshi.
2. Dans PowerShell, depuis ce dossier :
   ```powershell
   .\build.ps1      # compile (Visual Studio 2022 requis)
   .\install.ps1    # copie KenshiCoop.dll dans le dossier de Kenshi et l'ajoute à Plugins_x64.cfg
   ```
   Désinstaller : `.\install.ps1 -Uninstall`.
3. Chaque joueur doit avoir **le même Kenshi (1.0.68) et les mêmes mods**, dans le même ordre
   (le mod refuse la connexion sinon, avec un message clair).

Réglages : `KenshiCoop.ini` dans le dossier de Kenshi (créé au premier lancement).

| Section / clé | Rôle |
|---|---|
| `[player] name` | votre nom (par défaut celui de votre session Windows) ; c'est aussi le nom de votre personnage chez l'hôte. Si deux joueurs ont le même nom, le second devient « Nom 2 » |
| `[network] join_address` | IP de l'hôte (pour rejoindre) |
| `[network] port` | port UDP (27960 par défaut, à ouvrir/rediriger chez l'hôte) |
| `[coop] own_character` | 1 : chaque joueur qui rejoint reçoit son propre personnage (défaut) |
| `[ui] overlay` | petit panneau d'état en haut à droite |
| `[debug] commands` | canal de test automatisé : **laisser à 0** pour jouer |

## Jouer

Raccourcis (Kenshi au premier plan) :

| Touches | Action |
|---|---|
| Ctrl+Shift+H | héberger la partie chargée |
| Ctrl+Shift+J | rejoindre l'hôte (`join_address`) — depuis le menu principal ou une partie |
| Ctrl+Shift+L | quitter la session |
| Ctrl+Shift+G | (hôte) donner les personnages sélectionnés au joueur suivant |
| Ctrl+Shift+O | afficher / masquer le panneau |
| Ctrl+Shift+D | écrire un diagnostic dans `KenshiCoop.log` |

1. L'hôte charge sa partie et appuie sur **Ctrl+Shift+H**.
2. L'ami met l'IP de l'hôte dans `join_address` et appuie sur **Ctrl+Shift+J** (même depuis le menu).
   Le jeu de l'hôte se met en pause le temps qu'il arrive ; le client télécharge le monde de l'hôte
   et le charge automatiquement.
3. L'ami arrive avec **son propre personnage**, créé à son nom dans l'escouade de l'hôte (même race
   que l'escouade, sans équipement) et retrouvé s'il se reconnecte plus tard. L'hôte peut lui confier
   d'autres membres avec Ctrl+Shift+G. Si un 2e ami rejoint plus tard, le 1er voit aussi son personnage.

Chacun ne peut commander que ses personnages. Côté client, les ordres de déplacement, d'arrêt et de
**loot** sont transmis à l'hôte ; les autres ordres (artisanat, construction, dialogue, commerce...)
sont refusés avec un message pour l'instant.

## Ce qui est synchronisé

- **Le monde** : le client joue dans une copie exacte du monde de l'hôte (sauvegarde transférée au
  moment de rejoindre), puis tout est corrigé en continu depuis l'hôte.
- **Personnages** (escouade et PNJ) : positions, déplacements, posture (debout / au sol), combats au
  corps à corps, santé de chaque membre, KO, morts. Le client n'a pas d'IA ni de dégâts propres : il
  ne peut pas diverger. Les PNJ que l'hôte fait apparaître sont recréés chez le client ; ceux que le
  jeu du client créerait de lui-même sont retirés.
- **Heure, vitesse et pause** : celles de l'hôte, imposées en permanence.
- **Météo** (pluie, tempêtes, éclairs...) : celle de l'hôte, région par région.
- **Inventaires et équipement** (armes, armures, vêtements, sacs) de tous les personnages proches.
- **Loot** : clic droit « Fouiller » sur un personnage KO ou mort → votre personnage y va et la
  fenêtre de loot du jeu s'ouvre ; chaque objet déplacé est rejoué par l'hôte. L'hôte refuse de
  vider un PNJ conscient ou le personnage d'un autre joueur (et annule la tentative chez le client).
- **Cadavres** : restent synchronisés et lootables près des joueurs.

## Limites connues

- Les corps au sol peuvent reposer à quelques dizaines de centimètres (rarement plus d'un mètre) de
  leur position chez l'hôte : la chute du ragdoll est simulée par chaque PC.
- Les PNJ en marche peuvent avoir un léger décalage visuel (30 à 90 cm) : le jeu ne met à jour leurs
  positions que toutes les 4 images.
- Pas encore synchronisés : combat à distance (arbalètes), objets au sol et coffres, commerce,
  construction, artisanat, recrutement par dialogue.
- Quelques PNJ uniques ne peuvent pas être recréés chez le client s'il ne les a pas déjà (indiqué
  par le panneau : « N not spawned here yet »).

## Dépannage

- Journal : `KenshiCoop.log` dans le dossier de Kenshi (`KenshiCoop-<pid>.log` si deux Kenshi tournent).
- « version/mods différents » : vérifier que les deux joueurs ont exactement les mêmes mods.
- Rien ne se passe en rejoignant : vérifier l'IP, le port UDP 27960 ouvert chez l'hôte, et le pare-feu.

## Développement

- `build\bin\Release\kc_tests.exe` : tests du protocole et de la session (monde simulé).
- `tools\coop_test.py` : lance deux Kenshi sur ce PC, les pilote via le canal de debug
  (`[debug] commands=1`) et compare les deux mondes en pause. Exemples :
  `python tools\coop_test.py run --save kctest_base`, `... soak --minutes 15`.
