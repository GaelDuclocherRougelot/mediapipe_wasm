# Compiler MediaPipe sur macOS (Apple Silicon) — build desktop CPU

Note pour démarrer sur un **clone frais de `google/mediapipe`**. Objectif : compiler un
custom calculator et faire tourner une app desktop d'exemple (CPU, sans GPU).

> ⚠️ Rien ici n'est spécifique à Quantiq. Ce sont uniquement les correctifs génériques
> pour que le repo public de Google compile sur un Mac ARM récent.

---

## 1. Pourquoi le repo Google ne compile pas « out of the box »

Le repo `google/mediapipe` n'est plus maintenu côté build desktop, et deux choses ont
bougé sous ses pieds sur macOS :

1. **OpenCV 3 a disparu de Homebrew.** MediaPipe attend une install `opencv@3` dans
   `/usr/local/opt/opencv@3` avec des dylibs *non versionnées*. Aujourd'hui `brew install opencv`
   pose OpenCV **4.x** dans `/opt/homebrew` (Apple Silicon), avec une arborescence et des noms
   de fichiers différents. → Le repo Bazel `macos_opencv` pointe dans le vide → erreurs de link.
2. **Apple Silicon = `/opt/homebrew`**, pas `/usr/local` (qui est le préfixe Homebrew Intel).
   Les chemins codés en dur dans le repo ne correspondent plus.

Le reste du build desktop CPU fonctionne. Les seules modifs *nécessaires* concernent OpenCV.

---

## 2. Prérequis

```bash
# Outils de compilation Apple
xcode-select --install

# Bazel : NE PAS installer bazel en dur. Installer bazelisk, qui lit le
# fichier .bazelversion du repo et télécharge la bonne version automatiquement.
brew install bazelisk

# OpenCV (4.x) + numpy/python pour les outils annexes
brew install opencv
brew install python@3.12
```

Vérifier la version d'OpenCV installée (on en aura besoin, elle change les chemins) :

```bash
brew list --versions opencv      # ex: opencv 4.13.0_7
ls -l /opt/homebrew/opt/opencv   # symlink -> ../Cellar/opencv/4.13.0_7
```

> Intel Mac : remplacer partout `/opt/homebrew` par `/usr/local`.

---

## 3. Les modifications à faire (2 fichiers)

### 3.1 `WORKSPACE` — faire pointer le repo `macos_opencv` vers Homebrew

Chercher le bloc `new_local_repository(name = "macos_opencv", ...)` et changer le `path` :

```python
new_local_repository(
    name = "macos_opencv",
    build_file = "@//third_party:opencv_macos.BUILD",
    path = "/opt/homebrew",          # <-- était "/usr/local"
)
```

### 3.2 `third_party/opencv_macos.BUILD` — adapter à OpenCV 4.x Homebrew

C'est LE fichier qui casse. Le remplacer par la version ci-dessous.

**Astuce robuste** : utiliser le symlink `opt/opencv` (que Homebrew met toujours à jour) et
les dylibs *non versionnées* (`libopencv_core.dylib`, qui sont aussi des symlinks). Comme ça,
**pas besoin de ré-éditer ce fichier à chaque `brew upgrade opencv`** — contrairement à un
chemin de version figé type `Cellar/opencv/4.13.0_7`.

```python
load("@bazel_skylib//lib:paths.bzl", "paths")

licenses(["notice"])  # BSD license

exports_files(["LICENSE"])

# Symlink Homebrew stable : /opt/homebrew/opt/opencv -> Cellar/opencv/<version courante>
PREFIX = "opt/opencv"

cc_library(
    name = "opencv",
    srcs = glob([
        paths.join(PREFIX, "lib/libopencv_core.dylib"),
        paths.join(PREFIX, "lib/libopencv_calib3d.dylib"),
        paths.join(PREFIX, "lib/libopencv_features2d.dylib"),
        paths.join(PREFIX, "lib/libopencv_highgui.dylib"),
        paths.join(PREFIX, "lib/libopencv_imgcodecs.dylib"),
        paths.join(PREFIX, "lib/libopencv_imgproc.dylib"),
        paths.join(PREFIX, "lib/libopencv_video.dylib"),
        paths.join(PREFIX, "lib/libopencv_videoio.dylib"),
    ]),
    hdrs = glob([paths.join(PREFIX, "include/opencv4/opencv2/**/*.h*")]),
    includes = [paths.join(PREFIX, "include/opencv4/")],
    linkstatic = 1,
    visibility = ["//visibility:public"],
)
```

Points clés vs la version d'origine (OpenCV 3) :
- `PREFIX` : `opt/opencv@3` → `opt/opencv`
- includes : `include/` → **`include/opencv4/`** (OpenCV 4 met les headers dans un sous-dossier)
- les libs existent bien en `.dylib` non versionné dans `/opt/homebrew/opt/opencv/lib/`

> Si un jour Homebrew retire les symlinks non versionnés, il faudra passer aux noms versionnés
> (`libopencv_core.413.dylib`) et mettre `PREFIX = "Cellar/opencv/4.13.0_7"`. Mais tant que
> `ls /opt/homebrew/opt/opencv/lib/libopencv_core.dylib` répond, la version ci-dessus suffit.

---

## 4. Compiler et lancer un exemple desktop (vanilla, CPU)

Pas besoin de toucher au GPU sur Mac : on désactive tout avec `MEDIAPIPE_DISABLE_GPU=1`.

```bash
# Build de l'exemple hand_tracking en CPU
bazel build -c opt \
  --define MEDIAPIPE_DISABLE_GPU=1 \
  mediapipe/examples/desktop/hand_tracking:hand_tracking_cpu

# Lancer (webcam)
GLOG_logtostderr=1 \
  bazel-bin/mediapipe/examples/desktop/hand_tracking/hand_tracking_cpu \
  --calculator_graph_config_file=mediapipe/graphs/hand_tracking/hand_tracking_desktop_live.pbtxt
```

Autres exemples CPU utiles pour comprendre le pipeline :
`mediapipe/examples/desktop/hello_world:hello_world` (le plus simple, pas d'OpenCV/caméra),
`mediapipe/examples/desktop/face_detection:face_detection_cpu`.

`hello_world` est le meilleur point de départ pour lire un graphe minimal sans dépendance
caméra.

---

## 5. Écrire son propre calculator

Un calculator = une classe C++ qui hérite de `mediapipe::CalculatorBase` (méthodes
`GetContract` / `Open` / `Process` / `Close`). Pour en ajouter un :

1. Créer `mediapipe/calculators/<moncalc>/mon_calculator.cc` + une cible `cc_library` dans
   le `BUILD` du dossier, avec `alwayslink = 1` et `deps = ["//mediapipe/framework:calculator_framework"]`.
2. Le référencer par son nom dans un graphe `.pbtxt`.
3. L'ajouter aux `deps` du `cc_binary` qui charge le graphe (sinon il n'est pas linké → le
   graphe ne trouve pas le calculator au runtime).

Le plus simple pour démarrer : copier `mediapipe/examples/desktop/hello_world/`, y brancher
un calculator maison, et builder avec la même commande qu'au §4.

---

## 6. Ce qu'il ne faut PAS toucher pour un build desktop

Pour rester minimal, ignorer tout ce qui concerne :
- **GPU** : sur Mac desktop on force `MEDIAPIPE_DISABLE_GPU=1`, rien à configurer.
- **Android / NDK / iOS** : aucune install ni modif nécessaire pour du desktop CPU.
- **OpenCV « from source » / cmake** (`third_party/BUILD`, `setup_opencv.sh`) : sert au build
  Android ou aux builds statiques. Le desktop Mac utilise directement l'OpenCV Homebrew via
  `macos_opencv`. Ne pas s'en occuper.

---

## Récap' — checklist

- [ ] `xcode-select --install`, `brew install bazelisk opencv python@3.12`
- [ ] `WORKSPACE` : `macos_opencv` → `path = "/opt/homebrew"`
- [ ] `third_party/opencv_macos.BUILD` : remplacé par la version §3.2
- [ ] `bazel build -c opt --define MEDIAPIPE_DISABLE_GPU=1 //mediapipe/examples/desktop/hand_tracking:hand_tracking_cpu`
- [ ] Ça tourne → écrire son propre calculator (§5)
