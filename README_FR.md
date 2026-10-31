# Switch Ambilight

Ambilight autonome pour Nintendo Switch hackée et rubans WLED, sans PC ni carte HDMI pendant le jeu.

**Version :** v0.6.0 (refactoring non validé sur console). Le moteur v0.5, le NRO corrigé et l'overlay ont fonctionné sur une Mariko V2, Atmosphère 1.12.1, firmware 23.0.1, avec 114 LED (36 haut / 21 droite / 36 bas / 21 gauche). La compilation de ce dépôt autonome reste à vérifier sur une installation vierge.

Le projet est **dérivé de Switch Frame Tap**, sous licence GPL-2.0. Les IDs système C20 et C21 sont toujours ceux du projet d'origine : ne pas installer les deux en parallèle.

## Installation du code source / compilation

```bash
sudo apt install -y git rsync python3 build-essential zip
bash scripts/test.sh
bash scripts/build.sh
bash scripts/package.sh
```

Cela produit `dist/switch-ambilight-v0.6.0-switch.zip` avec le sysmodule C20, le lanceur mémoire C21, le menu Tesla et le Homebrew NRO. Extrait-le sur la SD **après sauvegarde** des dossiers C20/C21 existants.

Personnalise `SD:/config/switch-ambilight/config.ini` d'après `config.example.ini` : il faut notamment renseigner **l'IP de ton WLED**. La géométrie 114 LED reste fixe en v0.5.1. Les réglages du menu sont rechargés par le sysmodule.

Garde un moyen de récupérer ta SD avec Hekate ou un lecteur de carte en cas de blocage au démarrage. Ne supprime pas le dossier système C21. Le code de capture actuel reste partiellement dérivé de la partie streaming de Switch Frame Tap, et n'est pas encore totalement allégé.

Voir le [README anglais](README.md) pour les instructions détaillées, les limitations, les crédits et la licence.

## v0.6 : état

La configuration est migrée vers `config/switch-ambilight/config.ini` lors de la première sauvegarde, avec lecture de secours de l'ancien fichier. La synchronisation des patchs audio historiques est supprimée. **Le moteur de capture et le lanceur utilisent encore du code hérité de Switch Frame Tap** ; la compilation et les essais sur console restent indispensables.

### Nettoyage audio v0.6 (branche de développement)

L'intercepteur `audout:u`, les sondes audio et le démarrage de la capture audio ont été supprimés. Les utilitaires audio liés à l'ancien NVENC subsistent temporairement. **Nouvelle étape à compiler et tester sur Switch avant fusion.**
