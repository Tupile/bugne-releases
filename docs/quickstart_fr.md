# Bugne : démarrage rapide

[English version](quickstart.md)

Du bureau vide à l'enfant qui écoute sa première radio, en cinq étapes :
acheter une carte, imprimer un boîtier, flasher le firmware une fois, la
connecter au Wi-Fi, ajouter radios et podcasts. La documentation complète
se trouve dans ce dépôt ; chaque étape ci-dessous renvoie vers la partie
concernée.

## 1. Quoi acheter

- La carte : une **LCDWIKI ES3C28P** (utilisez cette référence exacte).
  C'est un ESP32-S3 avec 16 Mo de flash et 8 Mo de PSRAM, un écran tactile
  capacitif de 2,8 pouces, un codec audio, un microphone, un lecteur
  microSD et un port USB. Le petit haut-parleur est fourni avec la carte.
  Rien à souder. Si vous souhaitez m'aider sans coût supplémentaire, vous
  pouvez la commander via ce
  [lien affilié Aliexpress](https://s.click.aliexpress.com/e/_c4OZeS8F)
  (ou ce [lien alternatif](https://s.click.aliexpress.com/e/_c3MmlBCJ) en
  cas d'indisponibilité) en vous assurant de sélectionner le modèle touch
  "ES3C28P". Pour les utilisateurs basés en France, vous pouvez également
  utiliser ce [lien affilié Amazon](https://amzn.to/3RrzKT1).
- Un câble USB de données et un ordinateur (pour le premier flash
  uniquement).
- 4 vis M3 de 6 mm pour fixer la carte dans le boîtier, et 4 vis M3 de
  10 mm pour fixer le capot du boîtier. Si vous ne les avez pas, vous
  pouvez les trouver [ici](https://s.click.aliexpress.com/e/_c34zawnh).
- Facultatif : une carte microSD (FAT32) pour votre musique et les
  épisodes de podcast hors ligne (comme [celle-ci](https://s.click.aliexpress.com/e/_c2yej75h)
  ou [celle-là](https://s.click.aliexpress.com/e/_c3ywvSmJ) ; pour les
  utilisateurs basés en France, vous pouvez également utiliser ce
  [lien affilié Amazon](https://amzn.to/3Ta5I6J)).
- La carte a un port batterie et un chargeur (LiPo 3,7 V à une cellule),
  mais le fonctionnement sur batterie n'a pas encore été testé par le
  projet et n'est pas conseillé pour l'instant : alimentez l'appareil par
  USB.

## 2. Quoi imprimer en 3D : le coffret seventies

<img src="../case/preview_seventies_face.png" alt="Coffret seventies" height="200">

Imprimez les trois pièces du coffret seventies depuis le dossier
[`case/`](../case) :

- `es3c28p_seventies_corps.stl` (corps)
- `es3c28p_seventies_capot.stl` (capot arrière)
- `es3c28p_seventies_grille.stl` (grille de haut-parleur)

Il s'imprime face contre le plateau, sans supports. Sur une imprimante
multi-couleurs, utilisez `es3c28p_seventies_corps+grille.step` pour
imprimer la grille dans une seconde couleur ; une seule couleur convient
aussi.

Si vous n'avez pas d'imprimante 3D, des services comme PCBWay ou Craftcloud
permettent de faire imprimer et livrer le boîtier. Pour le modèle seventies,
il faut faire imprimer `es3c28p_seventies_corps+grille.step` et
`es3c28p_seventies_capot.stl` en PLA.

Deux modèles alternatifs (un boîtier simple en deux pièces et un poste
« vieille radio ») se trouvent dans le même dossier [`case/`](../case),
avec les scripts CadQuery qui génèrent tous les modèles.

## 3. Flasher le firmware (USB, une seule fois)

Une carte neuve a besoin d'un flash complet par USB. Toutes les mises à
jour suivantes s'installent par Wi-Fi depuis la page web, sans câble.

1. Branchez la carte à votre ordinateur en USB.
2. Ouvrez la page de l'[Installateur Web](https://tupile.github.io/bugne-releases/tools/web-flasher/) avec Chrome, Edge ou Opera.
3. Cliquez sur "Installer", choisissez le port COM de votre carte, et patientez pendant l'installation.

*(Note : Les utilisateurs avancés peuvent toujours flasher hors-ligne avec `bugne-flash.zip` et `esptool`. Voir le manuel complet).*

À la fin, l'appareil redémarre sous Bugne.

## 4. Connexion au Wi-Fi (suivez le QR code)

1. Comme l'appareil ne connaît encore aucun réseau Wi-Fi, il ouvre son
   propre point d'accès et affiche un QR code à l'écran.
2. Scannez ce QR code avec votre téléphone. Il rejoint le point d'accès
   nommé `Bugne-Setup-XXXX` (le XXXX est propre à votre appareil, tout
   comme le mot de passe du point d'accès, contenu dans le QR code).
3. La page de configuration s'ouvre toute seule après la connexion
   (sinon, ouvrez `http://192.168.4.1` dans le navigateur du téléphone).
4. Choisissez votre réseau Wi-Fi (2,4 GHz) et saisissez son mot de passe.
   L'appareil se connecte et le point d'accès disparaît.
5. La page de configuration est désormais disponible sur votre réseau, à
   l'adresse `http://bugne-xxxx.local` : scannez le QR affiché sur
   l'appareil dans Réglages, puis « Page de config (QR) », ou tapez
   l'adresse.

## 5. Ajouter les premières webradios et podcasts

Ouvrez `http://bugne-xxxx.local` depuis n'importe quel téléphone ou
ordinateur sur le même Wi-Fi.

**Onglet Radios** : cherchez dans l'annuaire public radio-browser.info et
ajoutez une station en un clic, ou ajoutez-en une à la main avec son nom
et l'URL directe de son flux. Les stations apparaissent aussitôt sur la
tuile Webradios de l'appareil.

<img src="manual/img/fr/web-radios.png" width="300">

**Onglet Podcasts** : ajoutez un podcast avec l'URL de son flux RSS.
« Télécharger nouveaux » enregistre les épisodes récents sur la carte
microSD pour l'écoute hors ligne.

<img src="manual/img/fr/web-podcasts.png" width="300">

Conseillé : dans l'onglet Réglages, définissez un mot de passe de page
pour que les enfants ne puissent pas ouvrir les réglages parents depuis
leurs propres appareils.

## Pour aller plus loin

- [Mode d'emploi](manual/fr.md) : usage quotidien, réveils, heures
  calmes, jeu des tables, accordeur, mises à jour, dépannage.
- [Notes matérielles](hardware.md) : brochage et détails de la carte
  (en anglais).
- [README](../README.md) : liste des fonctions et compilation depuis les
  sources (en anglais).
