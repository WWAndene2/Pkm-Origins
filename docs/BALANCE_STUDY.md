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

## 7. Talents et attaques : propositions vérifiées (révision du 4 novembre)

Demande du propriétaire : vérifier toutes les propositions. La première version de ce paragraphe (idées de l'agent, de
mémoire) est remplacée ; ce qu'elle avait de faux est listé au §7.3.

### 7.1 Données utilisées

- **Ce que chaque Pokémon apprend dans X / Y et ORAS** (niveau, œuf, CT, tuteur) : tables de PokeAPI (`pokemon_moves`,
  groupes de versions 15 X-Y et 16 ROSA). **Vérifiées contre tes jeux** : attaques par niveau (ORAS a/1/9/1, X a/2/1/4) et
  attaques d'œuf (a/1/9/0, a/2/1/3) identiques pour les 721 espèces des deux jeux, 0 écart. CT et tuteurs pris chez
  PokeAPI, non recoupés avec les jeux.
- **Ce que les joueurs jouaient vraiment** : fichiers « moveset » de Smogon, NU, septembre 2016 (nombre d'équipes, talents,
  attaques).
- **Attention** : Pokémon Showdown autorise les attaques venues de jeux plus anciens (transfert par la Banque). Une attaque
  jouée sur Smogon peut donc être **impossible à apprendre en campagne** sur une seule cartouche X / Y ou ORAS ; c'est
  vérifié ligne par ligne ci-dessous.

### 7.2 Par Pokémon

| Pokémon | Joué en NU (sept. 2016) | Constat vérifié | Proposition révisée |
|---|---|---|---|
| Couafarel | 982 équipes ; Toison Épaisse ; Cotogarde, Retour, Toxik, Coup Bas, Repos | Rôle de mur physique réel ; pas de soin fiable (Repos seulement) ; Paresse et Dernier Mot non appris | Paresse (soin) ; idée originale à part : un second type par coupe (non mesurée) |
| Chevroum | 1 262 ; Herbivore ; Encornebois, Gonflette, Séisme, Lait à Boire | Rôle réel, soin déjà présent ; Demi-Tour non appris | Aucune priorité ; Demi-Tour possible pour un rôle de pivot |
| Dedenne | 1 278 ; Bajoues ; Recyclage, Frotte-Frimousse, Tonnerre | Rôle réel (baies) ; Recyclage appris au tuteur d'ORAS seulement | Recyclage aussi dans X / Y |
| Banshitrouye (Super) | 3 930 ; Fouille ; Feu Follet, Vampigraine, Synthèse | Rôle réel ; ma proposition (Corps Maudit, attaques) n'apportait rien | Aucune |
| Cadoizo | 346 ; Agitation ; Prélèvement Destin, Poing Glace, Bluff, Picots, Tour Rapide, Anti-Brume | Rôle de soutien réel ; **Anti-Brume non apprenable** dans X / Y / ORAS ; Vent Arrière non appris | Anti-Brume et Vent Arrière ; ses stats (330) restent le vrai frein |
| Lovdisc | 108 ; Hydratation ; Doux Baiser, Toxik, Ébullition, Anneau Hydro | Presque injoué ; Vœu et Glas de Soin non appris | Vœu, Glas de Soin (rôle de soigneur) ; stats (330) en cause |
| Zarbi | 53 ; Puissance Cachée seule | Une seule attaque par conception | Idée originale : la lettre fixe le type de Puissance Cachée (non mesurée) |
| Spinda | 658 ; **Contestation** (95 %) ; Surpuissance, Coup Bas, Vampipoing | Rôle réel grâce à Contestation + Surpuissance ; **Surpuissance non apprenable** en campagne | Surpuissance par tuteur ; garder Contestation (et non Pieds Confus) |
| Corayon | 412 ; Régé-Force ; Ébullition, Soin, Toxik, Piège de Roc | Rôle de poseur de pièges réel ; Tour Rapide non appris | Tour Rapide |
| Delcatty | 666 ; Normalise ; Bluff, Coup Bas, Abri, Vœu | **Vœu et Coup Bas non apprenables** en campagne (transfert seulement) | Vœu et Coup Bas par tuteur |
| Séracrawl | 2 822 ; Fermeté ; Avalanche, Tour Rapide, Soin, Séisme | Rôle réel ; Distorsion non appris ; sa Déf. Spé. (46) est le frein | Distorsion possible ; pas prioritaire |
| Canarticho | 243 ; **Acharné** (71 %) ; Lame Feuille, Rapace, Danse Lames | Attaques déjà là ; le frein est le total (352) et l'objet Bâton | Pas de nouveau talent (Sniper retiré) ; le vrai levier est hors attaques (stats ou évolution d'Épée / Bouclier, à étudier) |
| Mélokrik | 395 ; Technicien ; Toile Gluante, Aéropiqué | Rôle réel (Toile Gluante déjà apprise) | Aucune |
| Parasect | 518 ; Peau Sèche ; Spore, Plaie Croix, Vampigraine, Sabotage | Rôle réel (Spore) ; Distorsion non appris | Aucune priorité |

**Conclusion vérifiée** : pour la plupart de ces Pokémon, le rôle existe déjà et les joueurs le trouvent ; le frein est
surtout leurs **stats**. Le seul manque clair en campagne est une série d'**attaques jouées en compétition mais
impossibles à apprendre sur une seule cartouche** (Surpuissance pour Spinda, Vœu et Coup Bas pour Delcatty, Anti-Brume pour
Cadoizo) : les rendre disponibles par tuteur ne change pas l'équilibre compétitif (elles y étaient déjà) et aide la
campagne. Proposition à étendre : lister **toutes** les attaques jouées sur Smogon mais non apprenables dans X / Y / ORAS.

### 7.3 Erreurs de la première version (corrigées ci-dessus)

- Noms faux : « Cage-Éclair » (Cage Éclair), « Plaie-Croix » (Plaie Croix), « Lame d'Air » (Lame d’Air), « Chant » (Berceuse),
  « Danse-Folle » (Danse Folle), « Coup d'Jus » (n'existe pas).
- Feuillage proposé pour Parasect : attaque de la 7e génération, absente de X / Y / ORAS.
- Attaques « proposées » déjà apprises : Façade, Demi-Tour (Couafarel), Encornebois, Synthèse, Gonflette, Feu Follet,
  Vampigraine, Ombre Portée, Cadeau, Blizzard, Reflet, Attraction, Ébullition, Toxik, Soin, Gyroballe, Tranche-Nuit, Danse
  Lames, Toile Gluante, Taillade, Spore.
- Talents contraires aux données : Pieds Confus pour Spinda (95 % des joueurs utilisent Contestation), Sniper pour
  Canarticho (Acharné utilisé à 71 %), Corps Maudit pour Banshitrouye (rôle déjà rempli).
- Mimi-Queue proposée pour Spinda : attaque faible, sans intérêt pour le rôle.

Sources : https://www.smogon.com/stats/2016-09/ (ubers-0.txt, ou-0.txt, uu-0.txt, ru-0.txt, nu-0.txt, vgc2016-0.txt) ;
PokeAPI (https://github.com/PokeAPI/pokeapi, data/v2/csv) ; stats de base lues dans les jeux.
