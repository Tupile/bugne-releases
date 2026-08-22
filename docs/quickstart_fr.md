# Bugne : démarrage rapide

[English version](quickstart.md)

Ce guide va du bureau vide à l'enfant qui écoute sa première radio web. Il
tient en cinq étapes : acheter une carte, imprimer un boîtier, flasher le
firmware une fois, connecter l'appareil au Wi-Fi, puis ajouter des radios web
et des podcasts. La documentation complète se trouve dans ce dépôt. Chaque
étape ci-dessous renvoie vers la partie concernée.

## 1. Quoi acheter

- La carte : une **LCDWIKI ES3C28P**. Utilisez cette référence exacte. C'est
  un ESP32-S3 avec 16 Mo de flash et 8 Mo de PSRAM, un écran tactile capacitif
  de 2,8 pouces, un codec audio, un microphone, un lecteur microSD et un port
  USB. Le petit haut-parleur est fourni avec la carte. Vous n'avez rien à
  souder. Si vous souhaitez m'aider sans coût supplémentaire, vous
  pouvez la commander via ce
  [lien affilié Aliexpress](https://s.click.aliexpress.com/e/_c4OZeS8F)
  (ou ce [lien alternatif](https://s.click.aliexpress.com/e/_c3MmlBCJ) en
  cas d'indisponibilité) en vous assurant de sélectionner le modèle touch
  "ES3C28P". Pour les utilisateurs basés en France, vous pouvez également
  utiliser ce [lien affilié Amazon](https://amzn.to/3RrzKT1).
- Un câble USB de données et un ordinateur. Ils servent au premier flash
  seulement.
- 4 vis M3 de 6 mm pour fixer la carte dans le boîtier, et 4 vis M3 de
  10 mm pour fixer le capot. Si vous ne les avez pas, vous
  pouvez les trouver [ici](https://s.click.aliexpress.com/e/_c34zawnh).
- Facultatif : une carte microSD (FAT32) pour votre musique et les
  épisodes de podcast hors ligne (comme [celle-ci](https://s.click.aliexpress.com/e/_c2yej75h)
  ou [celle-là](https://s.click.aliexpress.com/e/_c3ywvSmJ) ; pour les
  utilisateurs basés en France, vous pouvez également utiliser ce
  [lien affilié Amazon](https://amzn.to/3Ta5I6J)).
- La carte a un port batterie et un chargeur pour un accu LiPo 3,7 V à une
  cellule. Le projet n'a pas testé le fonctionnement sur batterie et ne le
  conseille pas encore. Alimentez l'appareil par USB.

## 2. Quoi imprimer en 3D : le coffret seventies

<img src="../case/preview_seventies_face.png" alt="Coffret seventies" height="200">

Imprimez les trois pièces du coffret seventies depuis le dossier
[`case/`](../case). Si vous avez une imprimante BambuLab, téléchargez-les
plutôt sur
[MakerWorld](https://makerworld.com/en/models/3073793-bugne-open-source-internet-radio-podcast-player) :

- `es3c28p_seventies_corps.stl` (corps)
- `es3c28p_seventies_capot.stl` (capot arrière)
- `es3c28p_seventies_grille.stl` (grille de haut-parleur)

Imprimez-le face contre le plateau, sans supports. Sur une imprimante
multi-couleurs, utilisez `es3c28p_seventies_corps+grille.step` pour imprimer la
grille dans une seconde couleur. Une seule couleur convient aussi.

Si vous n'avez pas d'imprimante 3D, un service comme PCBWay ou Craftcloud
imprime le boîtier et vous le livre. Pour le modèle seventies, commandez
`es3c28p_seventies_corps+grille.step` et `es3c28p_seventies_capot.stl` en PLA.

Le même dossier [`case/`](../case) contient deux autres modèles, un boîtier
simple en deux pièces et un poste « vieille radio », avec les scripts CadQuery
qui génèrent tous les modèles.

## 3. Flasher le firmware (USB, une seule fois)

Une carte neuve a besoin d'un flash complet par USB. Toutes les mises à jour
suivantes s'installent par Wi-Fi depuis la page web, sans câble.

1. Branchez la carte à votre ordinateur en USB. Maintenez le bouton BOOT appuyé
   pendant que vous branchez le câble, pour passer en mode bootloader.
2. Ouvrez la page de l'[Installateur Web](https://tupile.github.io/bugne-releases/tools/web-flasher/)
   avec Chrome, Edge ou Opera.
3. Cliquez sur "Installer", choisissez le port COM de la carte, et patientez
   jusqu'à la fin de l'installation.

*(Note : vous pouvez aussi flasher hors ligne avec `bugne-flash.zip` et
`esptool`. Le [README](../README.md) donne les commandes, en anglais.)*

À la fin de l'installation, l'appareil redémarre sous Bugne.

## 4. Connexion au Wi-Fi (suivez le QR code)

1. L'appareil ne connaît encore aucun réseau Wi-Fi. Il ouvre son propre point
   d'accès et affiche un QR code à l'écran.
2. Scannez ce QR code avec votre téléphone. Votre téléphone rejoint le point
   d'accès `Bugne-Setup-XXXX`. Le XXXX est propre à votre appareil, tout comme
   le mot de passe du point d'accès, que le QR code contient.
3. La page web s'ouvre toute seule après la connexion. Sinon, ouvrez
   `http://192.168.4.1` dans le navigateur du téléphone.
4. Choisissez votre réseau Wi-Fi (2,4 GHz) et saisissez son mot de passe.
   L'appareil se connecte et le point d'accès s'arrête.
5. La page web est maintenant sur votre réseau, à l'adresse
   `http://bugne-xxxx.local`. Tapez cette adresse, ou scannez le QR code
   affiché sur l'appareil dans Réglages, puis « Page de config (QR) ».

## 5. Ajouter les premières radios web et podcasts

Ouvrez `http://bugne-xxxx.local` depuis n'importe quel téléphone ou ordinateur
sur le même Wi-Fi.

**Onglet Radios** : cherchez dans l'annuaire public radio-browser.info et
ajoutez une station en un clic. Vous pouvez aussi ajouter une station à la main,
avec son nom et l'URL de son flux. Une nouvelle station apparaît aussitôt sur
la tuile Radios web de l'appareil.

<img src="manual/img/fr/web-radios.png" width="300">

**Onglet Podcasts** : le cadre « Trouver des podcasts » cherche dans l'annuaire
Apple Podcasts. Tapez un nom, puis ajoutez un résultat en un clic : la page
remplit toute seule le titre et l'adresse RSS. Vous pouvez aussi coller une
adresse RSS à la main. « Télécharger nouveaux » enregistre les épisodes
récents sur la carte microSD pour l'écoute hors ligne.

<img src="manual/img/fr/web-podcasts.png" width="300">

Conseillé : dans l'onglet Réglages, définissez un mot de passe de page. Un
enfant ne pourra alors pas ouvrir les réglages parents depuis son propre
téléphone.

## Pour aller plus loin

- [Mode d'emploi](manual/fr.md) : usage quotidien, réveils, heures calmes,
  limite d'écoute quotidienne, jeu des tables, accordeur (expérimental), mises
  à jour et dépannage.
- [Notes matérielles](hardware.md) : brochage et détails de la carte
  (en anglais).
- [README](../README.md) : liste des fonctions, API HTTP et compilation depuis
  les sources (en anglais).
