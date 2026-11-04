# Étude d'équilibrage des Pokémon (X / Y et ORAS)

Demande du propriétaire (4 novembre) : une étude fondée sur les données disponibles sur internet (Smogon) et sur le game
design, pour décider de l'option « Rééquilibré » (FEATURE_IDEAS.md, « Équilibrage Pokémon »). La version fidèle ne change
rien ; tout ce qui suit est une proposition, pas une décision.

## 1. Données et méthode

- **Usage compétitif** : statistiques Smogon de septembre 2016, le dernier mois complet de la 6e génération (Soleil / Lune
  sort en novembre 2016) : `smogon.com/stats/2016-09/` fichiers `ubers-0`, `ou-0`, `uu-0`, `ru-0`, `nu-0` (Pokémon
  Showdown, tous niveaux de joueurs) et `vgc2016-0` (format officiel en double de 2016). OU : 3 128 137 combats.
- **Lecture des tiers** : chaque tier exclut les Pokémon des tiers au-dessus. Un Pokémon présent en Ubers mais absent du
  fichier OU y est **banni d'OU** (jugé trop fort par Smogon). Un usage sous ~0,5 % partout, NU compris, signale un Pokémon
  que presque personne ne joue, même là où il est autorisé.
- **Stats de base** : lues dans les jeux (`tools/personal_stats.py`), identiques dans ORAS et X ; stades finaux et
  légendaires d'après PokeAPI (FEATURE_IDEAS.md).
- **Limite** : Smogon mesure le jeu compétitif entre humains (Showdown, niveau 100 ou 50, équipes optimisées). Pkm Origins
  se joue surtout en solo contre l'IA. Un Pokémon « trop fort » en compétition peut être juste en campagne, et l'inverse.
  Les deux buts sont séparés au §4.

## 2. Ce que disent les données : les trop forts

| Pokémon | Ubers | OU | VGC 2016 | Lecture |
|---|---|---|---|---|
| Méga-Kangourex (Amour Filial) | 9,1 % (11e) | absent : banni | 32,8 % (3e) | Le plus utilisé des non-légendaires en VGC ; banni d'OU |
| Exagide (Déclic Tactique) | 12,8 % (7e) | absent : banni | 4,9 % (24e) | Banni d'OU ; joué au milieu des légendaires en Ubers |
| Amphinobi (Protéen) | 19,1 % (3e) | absent : banni | 3,7 % | 3e d'Ubers, devant presque tous les légendaires |
| Flambusard (Ailes Bourrasque) | 5,0 % | 11,6 % (5e) | 20,2 % (8e) | Fort mais **autorisé en OU** : pas banni, très présent |
| Méga-Mysdibule | 2,6 % | absent : banni | 7,8 % (20e) | Banni d'OU |
| Méga-Ectoplasma, Méga-Drattak, Méga-Lucario | 8,8 / 8,0 / 7,4 % | absents : bannis | 19,2 / 24,4 / 1,0 % | Bannis d'OU |

Conclusions :
- **Méga-Kangourex est le cas le plus net** (banni d'OU et dominant en VGC).
- **Exagide et Amphinobi** sont aussi bannis d'OU : ma première liste (de mémoire) avait oublié Amphinobi.
- **Flambusard n'était pas banni** : fort, mais dans les limites d'OU selon Smogon. Le corriger n'est pas justifié par ces
  données ; la correction de Game Freak (7e génération) allait plus loin que le jugement de Smogon.
- Les Méga-Évolutions bannies sont nombreuses : le problème vient en partie du mécanisme (un Méga gratuit par équipe).

## 3. Ce que disent les données : les trop faibles

Usage très bas dans **tous** les tiers, NU compris (maximum indiqué) : Couafarel (≤ 0,31 %), Chevroum (≤ 0,41 %), Dedenne (≤ 0,41 %),
Banshitrouye (≤ 0,08 %), Séracrawl (≤ 0,95 % en NU), Cadoizo (≤ 0,11 %), Lovdisc (≤ 0,04 %), Zarbi (≤ 0,02 %), Spinda
(≤ 0,19 %), Corayon (≤ 0,14 %), Delcatty (≤ 0,22 %), Mysdibule non-Méga (≤ 1,2 % en NU).

À l'inverse, des Pokémon de Kalos aux stats moyennes trouvent leur place : Prismillon 8,3 % en NU malgré un total de 411,
Némélios 10,4 % en NU, Sepiatroce 12,0 % en NU, Mistigrix 3,3 % en VGC (rôle de soutien). **Le total des stats ne suffit
donc pas** : un rôle clair (talent, attaques) compte plus. Couafarel (472, près de la moyenne) est quand même délaissé :
pas de rôle.

## 4. Game design : deux buts, deux réglages

**Campagne solo (contre l'IA)** — but : que chaque Pokémon rencontré soit une option viable jusqu'à la Ligue.
- Les Pokémon des premières routes (Insectes, Normal) sont faibles **par dessein** : ils évoluent tôt pour aider au début.
  Les rendre forts en fin de jeu sans casser le début : améliorer leur **liste d'attaques** et leur **talent caché**, pas
  leurs stats de base.
- Les Pokémon sans rôle (Couafarel, Chevroum, Cadoizo, Lovdisc...) : leur donner **un rôle unique** (une attaque
  signature, un talent) plutôt que des points de stats.
- Les trop forts en compétition ne posent pas problème contre l'IA : en campagne, on ne les affaiblit pas ; on renforce
  plutôt les dresseurs (FEATURE_IDEAS.md, Champions).

**Combats en ligne entre joueurs** — but : la diversité des équipes.
- Corriger en priorité ce que les données désignent : Méga-Kangourex, Exagide, Amphinobi ; mesurer avant de toucher à
  Flambusard.
- Préférer les **règles** aux changements de Pokémon quand c'est possible : formats avec liste de bannis, comme Smogon ;
  c'est réversible et ne touche pas la campagne.
- Si un Pokémon change, une seule variable, la raison écrite (FEATURE_IDEAS.md, méthode).

## 5. Propositions (option « Rééquilibré »)

1. **En ligne** : formats OU / Ubers / VGC avec les listes de bannis de Smogon de 2016 comme point de départ (aucun
   Pokémon modifié).
2. **En campagne** : listes d'attaques et talents cachés étendus pour la vingtaine de Pokémon délaissés du §3, un rôle
   chacun (liste détaillée à écrire, un Pokémon à la fois).
3. **Méga-Évolutions** : ne pas toucher en campagne ; en ligne, suivre les bannis.
4. **À ne pas faire** sans nouvelle mesure : affaiblir Flambusard ; changer les stats de base de Pokémon d'early-game.

## 6. Prochaines mesures

- Stats d'usage de plusieurs mois (2014-2016) pour vérifier que septembre 2016 n'est pas une exception.
- Taux de victoire par Pokémon contre l'IA du jeu recompilé (simulations), pour le but « campagne ».
- Listes d'attaques et talents des Pokémon délaissés, lus dans les jeux.

## 7. Talents et attaques : propositions par Pokémon (option « Rééquilibré », campagne d'abord)

Principe (remarque du propriétaire, 4 novembre) : revaloriser par les **talents** et les **attaques** avant les stats.
Les talents actuels sont lus dans les jeux (`tools/personal_stats.py`, noms de PokeAPI) ; les attaques proposées sont des
attaques de la 6e génération, écrites de mémoire : **les listes d'attaques actuelles n'ont pas encore été lues** (archive
des apprentissages, prochaine mesure), donc une attaque proposée peut déjà être apprise. Une ligne = un rôle.

| Pokémon | Talents actuels (1 / 2 / caché) | Rôle visé | Talent proposé | Attaques proposées |
|---|---|---|---|---|
| Couafarel (Normal) | Toison Épaisse (les trois) | Pivot physique, rapide | Garder Toison Épaisse ; idée originale : chaque coupe donne un second type (Diamant Roche, Pharaon Sol, Dame Fée...) | Demi-Tour, Bluff, Cage-Éclair, Façade |
| Chevroum (Plante) | Herbivore / Herbivore / Toison Herbue | Tank de soin | Caché : Régé-Force | Encornebois, Demi-Tour, Synthèse, Gonflette |
| Dedenne (Électrik / Fée) | Bajoues / Ramassage / Plus | Soutien à baies | Garder Bajoues | Recyclage, Glas de Soin, Change Éclair, Para-Spore |
| Banshitrouye (Spectre / Plante) | Ramassage / Fouille / Insomnia | Mur physique qui brûle | Caché : Corps Maudit | Feu Follet, Vampigraine, Synthèse, Ombre Portée |
| Cadoizo (Glace / Vol) | Esprit Vital / Agitation / Insomnia | Soutien rapide | Caché : Farceur | Vent Arrière, Cadeau, Blizzard, Reflet |
| Lovdisc (Eau) | Glissade / Glissade / Hydratation | Soigneur | Caché : Médic Nature | Vœu, Glas de Soin, Attraction, Ébullition |
| Zarbi (Psy) | Lévitation (les trois) | Couteau suisse | Idée originale : la lettre fixe le type de Puissance Cachée et donne un petit bonus de stat | Puissance Cachée (seule) |
| Spinda (Normal) | Tempo Perso / Pieds Confus / Contestation | Perturbateur | Garder Pieds Confus | Danse-Folle, Mimi-Queue, Coup d'Jus, Vantardise |
| Corayon (Eau / Roche) | Agitation / Médic Nature / Régé-Force | Poseur de pièges | Garder Régé-Force | Piège de Roc, Toxik, Soin, Tour Rapide |
| Delcatty (Normal) | Joli Sourire / Normalise / Peau Miracle | Soutien | Normalise avec un bonus de puissance (piste : la 7e génération l'a fait, à mesurer) | Vœu, Glas de Soin, Bâillement, Feinte |
| Séracrawl (Glace) | Tempo Perso / Corps Gel / Fermeté | Mur physique lent | Garder Fermeté | Distorsion, Gyroballe, Soin, Tour Rapide |
| Canarticho (Normal / Vol) | Regard Vif / Attention / Acharné | Attaquant à coups critiques | Caché : Sniper | Lame d'Air, Tranche-Nuit, Danse Lames ; idée : l'évolution d'Épée / Bouclier (Palarticho) en option |
| Mélokrik (Insecte) | Essaim / Essaim / Technicien | Poseur de Toile | Garder Technicien | Toile Gluante, Plaie-Croix, Taillade, Chant |
| Parasect (Insecte / Plante) | Pose Spore / Peau Sèche / Moiteur | Endormeur | Caché : Régé-Force | Spore, Plaie-Croix, Feuillage, Distorsion |

Non retenus (rôle déjà réel) : Mysdibule (sa Méga-Évolution est bannie d'OU), Charmina (Force Pure), Qulbutoké (Marque
Ombre), Lumivole / Muciole (Farceur).

À vérifier avant d'écrire une seule donnée : les listes d'attaques actuelles (déjà apprises ?), les noms français exacts
des attaques, et l'effet en campagne (simulations contre l'IA, §6).

Sources : https://www.smogon.com/stats/2016-09/ (ubers-0.txt, ou-0.txt, uu-0.txt, ru-0.txt, nu-0.txt, vgc2016-0.txt) ;
PokeAPI (https://github.com/PokeAPI/pokeapi, data/v2/csv) ; stats de base lues dans les jeux.
