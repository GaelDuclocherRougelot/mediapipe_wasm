# MediaPipe custom calculators dans le navigateur (WASM) — comment ça marche

Ce document explique l'architecture des deux démos WASM du repo :

- `mediapipe/examples/wasm/quantiq_demo/` — **Stage 1** : CPU only, preuve de concept minimale.
- `mediapipe/examples/wasm/gpu_video_demo/` — **Stage 2** : GPU (WebGL), flux vidéo webcam, calculateur custom avec shader, exécution dans un Web Worker.

Les deux réutilisent le même principe de base : compiler des calculateurs MediaPipe C++ en WebAssembly avec Emscripten, et exposer un pont JS↔C++ via **Embind**.

---

## 1. Vue d'ensemble : pourquoi ce n'est pas juste "compiler avec Emscripten"

MediaPipe fournit officiellement des binaires WASM précompilés (`graph_runner.ts` et consorts) pour du JS haut-niveau clé-en-main. Mais ces bindings référencent des dizaines de symboles natifs qui n'existent **dans aucun fichier source de ce dépôt** — seuls les binaires propriétaires de Google les implémentent. Impossible de les réutiliser tels quels pour un calculateur custom.

La solution retenue ici : écrire notre **propre petit pont Embind**, minimal, qui expose exactement ce dont on a besoin (une fonction ou une classe C++), et laisser Emscripten gérer la compilation du graphe MediaPipe complet (framework + calculateurs) en `.wasm`.

---

## 2. Stage 1 — CPU only (`quantiq_demo/`)

### Fichiers

- `mediapipe/calculators/quantiq_demo/exclaim_calculator.cc` — calculateur custom simple (ajoute des `!` à une string).
- `mediapipe/examples/wasm/quantiq_demo/quantiq_demo_wasm.cc` — le pont Embind.
- `mediapipe/examples/wasm/quantiq_demo/BUILD` — règles de build.
- `mediapipe/examples/wasm/quantiq_demo/index.html` — page de test.

### Le graphe

```
"in" -> PassThroughCalculator -> "out1" -> ExclaimCalculator -> "out"
```

Construit et exécuté en entier à chaque appel (`ProcessString`), de façon synchrone, bloquante, à usage unique (`quantiq_demo_wasm.cc:21-60`).

### Le piège n°1 : `CalculatorGraph` est mono-thread sous Emscripten

Normalement, `CalculatorGraph` orchestre ses nœuds sur plusieurs threads OS. Mais `mediapipe/framework/calculator_graph.cc` force `use_application_thread = true` **inconditionnellement** dès que `__EMSCRIPTEN__` est défini — qu'on compile avec `-pthread` ou non. Tout tourne donc sur le seul thread JS qui a appelé la fonction C++.

Conséquence directe : le pattern classique `OutputStreamPoller::Next()` (bloquant, "attends que le prochain paquet arrive") **deadlock** systématiquement. Il attend un signal sur sa propre condition variable, mais comme il n'y a pas de second thread pour produire ce signal pendant que le premier est bloqué à l'attendre, rien ne se passe jamais.

**Solution : `ObserveOutputStream(name, callback)`.** Au lieu d'aller chercher activement le résultat, on enregistre un callback qui sera invoqué **automatiquement, de façon synchrone**, par la boucle de pompage interne du scheduler — cette boucle est elle-même déclenchée par `WaitUntilDone()` (single-shot, Stage 1) ou `WaitUntilIdle()` (streaming continu, Stage 2). C'est ce même thread JS qui exécute tout : pousser le paquet, faire tourner le graphe, invoquer le callback, tout ça en une seule pile d'appels synchrone.

### Le piège n°2 : le nom du binaire doit matcher partout

`wasm_cc_binary(outputs = ["quantiq_demo.js", "quantiq_demo.wasm"])` — le glue JS généré embarque en dur une référence au nom du fichier `.wasm` d'origine (celui du `cc_binary`). Si les noms divergent, le chargement échoue silencieusement ou avec une erreur peu explicite.

### Build

```bash
bazel build -c opt //mediapipe/examples/wasm/quantiq_demo:quantiq_demo_wasm
```

---

## 3. Stage 2 — GPU/WebGL, webcam, Worker (`gpu_video_demo/`)

### Fichiers

- `mediapipe/calculators/quantiq_demo/square_overlay_calculator.cc` — calculateur GPU custom (dessine un carré rouge au centre du flux vidéo, via shader).
- `mediapipe/examples/wasm/gpu_video_demo/gpu_video_demo_wasm.cc` — le pont Embind, avec état persistant (classe, pas fonction).
- `mediapipe/examples/wasm/gpu_video_demo/BUILD`
- `mediapipe/examples/wasm/gpu_video_demo/index.html` — main thread : webcam + affichage.
- `mediapipe/examples/wasm/gpu_video_demo/worker.js` — Web Worker : héberge tout le module wasm.

### 3.1 Pourquoi WebGL et pas WebGPU

WebGPU aurait demandé de vendorer Dawn (l'implémentation de référence) from scratch — aucune trace dans `MODULE.bazel`/`WORKSPACE` — et référence des fichiers C++ fantômes dans ses `BUILD` (absents du disque). WebGL, à l'inverse, a une fondation complète et sans trou : `mediapipe/gpu/gl_context_webgl.cc` implémente déjà tout le nécessaire, et Emscripten fournit GLES/WebGL nativement, sans dépendance externe à ajouter.

### 3.2 Le graphe

Un seul nœud GPU :

```
"input_video" -> SquareOverlayCalculator -> "output_video"
```

Contrairement au Stage 1, le graphe est construit **une seule fois** (`Initialize()`) et reste ouvert (`StartRun({})` appelé une fois, jamais `CloseInputStream`/`WaitUntilDone`). Chaque frame vidéo pousse un paquet et draine le graphe avec `WaitUntilIdle()` (drainer une frame sans fermer le graphe — contrairement à `WaitUntilDone()` qui attend la fermeture complète).

### 3.3 Le cycle de vie C++, exposé comme une classe Embind

```cpp
class GpuVideoDemo {
 public:
  std::string Initialize();
  emscripten::val ProcessFrame(emscripten::val rgba, int width, int height);
 private:
  absl::Status ReadBackFrame(const GpuBuffer& gpu_frame);
  CalculatorGraph graph_;
  GlCalculatorHelper gpu_helper_;
  std::vector<uint8_t> output_buffer_;
};
```

Côté JS : `new Module.GpuVideoDemo()` → `.initialize()` → `.processFrame(...)` en boucle, une fois par frame.

**`Initialize()`** (`gpu_video_demo_wasm.cc:49-75`) :
1. Parse le graphe (texte proto inline).
2. `GpuResources::Create()` — crée le contexte WebGL (voir 3.4 ci-dessous) et l'attache au graphe.
3. `gpu_helper_.InitializeForTest(...)` — initialise l'assistant GL local à notre classe.
4. `graph_.ObserveOutputStream("output_video", callback)` — enregistre le lecteur de sortie (même pattern que Stage 1).
5. `graph_.StartRun({})` — démarre le graphe, qui reste actif indéfiniment.

**`ProcessFrame(rgba, width, height)`** (`gpu_video_demo_wasm.cc:83-108`), appelée à chaque frame :
1. Copie le buffer JS (RGBA brut) dans `input_pixels_`, un `std::vector<uint8_t>` membre persistant, via un `TypedArray.set()` natif (voir 3.7 — remplace l'ancien `emscripten::vecFromJSArray`, ~31x plus lent).
2. Construit un `ImageFrame` CPU à partir de ces pixels.
3. `gpu_helper_.RunInGlContext([...])` — bascule sur le contexte GL dédié pour : uploader l'`ImageFrame` en texture GPU (`CreateSourceTexture`), récupérer un `GpuBuffer` qui la référence, et pousser ce buffer comme paquet d'entrée du graphe.
4. `graph_.WaitUntilIdle()` — fait tourner le graphe pour cette frame : le shader du `SquareOverlayCalculator` s'exécute sur GPU, le résultat sort sur `"output_video"`, ce qui invoque **synchroniquement** le callback `ReadBackFrame` (étape 5 ci-dessous), avant que `WaitUntilIdle()` ne retourne.
5. `ReadBackFrame(gpu_frame)` (`gpu_video_demo_wasm.cc:111-124`) : re-bascule sur le contexte GL, lie le framebuffer sur la texture de sortie, et fait un `glReadPixels` — copie GPU→CPU dans `output_buffer_`.
6. `ProcessFrame` retourne `emscripten::typed_memory_view(...)` — une **vue** (pas une copie) sur `output_buffer_`, directement dans la mémoire linéaire wasm. Le JS appelant doit la consommer immédiatement (avant toute autre allocation wasm), sinon `ALLOW_MEMORY_GROWTH` pourrait invalider la vue.

### 3.4 Le contexte WebGL et le sélecteur de canvas `"#canvas"`

`GpuResources::Create()` appelle en interne `GlContext::Create(...)`, qui sous Emscripten (`mediapipe/gpu/gl_context_webgl.cc`) fait :

```cpp
emscripten_webgl_create_context("#canvas", &attrs);
```

Le sélecteur `"#canvas"` est en dur, mais mappé côté JS via :

```js
EM_ASM({ specialHTMLTargets["#canvas"] = Module.canvas; });
```

**`Module.canvas` doit donc être assigné avant tout appel qui déclenche `GpuResources::Create()`.** C'est ce qui explique pourquoi, dans `worker.js`, on fait :

```js
const Module = await GpuVideoDemoModule({ canvas: msg.canvas });
```

`msg.canvas` peut être un vrai `<canvas>` DOM **ou** un `OffscreenCanvas` transféré — les deux fonctionnent, ce qui est la clé de l'architecture Worker (section 3.6).

### 3.5 Le shader : comment un calculateur GPU custom fonctionne

`SquareOverlayCalculator` hérite de `GlSimpleCalculator` (`mediapipe/gpu/gl_simple_calculator.h`), une classe de base qui gère toute la plomberie (`Open`/`Process`/`Close`, création/liaison de texture, framebuffer) — la sous-classe n'a qu'à fournir trois méthodes :

- `GlSetup()` — compile le shader une fois (`GlhCreateProgram`).
- `GlRender(src, dst)` — dessine, appelé à chaque frame.
- `GlTeardown()` — libère le programme GL.

Le fragment shader (`square_overlay_calculator.cc:37-73`) est exécuté **en parallèle, pixel par pixel, sur le GPU**. Pour chaque pixel, `sample_coordinate` contient sa position normalisée (0 à 1) dans l'image ; le shader compare sa distance au centre `(0.5, 0.5)` à un seuil (`kSquareHalfSize = 0.15`) et choisit soit la couleur rouge unie, soit le pixel de la vidéo source échantillonné via `texture2D(video_frame, sample_coordinate)`. Aucune boucle explicite sur les pixels en C++ : c'est le modèle d'exécution GPU qui parallélise ça nativement.

`GlRender` (lignes 82-128) fait le "plumbing" classique OpenGL : deux triangles couvrant tout l'écran (`GL_TRIANGLE_STRIP`, 4 sommets), un VBO pour les positions, un pour les coordonnées de texture, puis `glDrawArrays` déclenche l'exécution du shader sur chaque pixel du framebuffer cible.

### 3.6 Architecture Web Worker

Le module wasm entier (graphe MediaPipe + contexte WebGL) tourne **dans un Worker**, pas sur le thread JS principal — ainsi la page reste réactive (scroll, interactions) même si le traitement GPU/CPU par frame prend du temps.

**Important : ceci n'a rien à voir avec `-pthread`/multithreading Emscripten.** Un seul Worker héberge un module Emscripten mono-thread par défaut (comme il l'est de toute façon sous Emscripten côté scheduling MediaPipe, cf. piège n°1) — il s'exécute juste sur le thread de ce Worker plutôt que sur le thread principal de la page. Pas besoin de `SharedArrayBuffer` ni des en-têtes `COOP`/`COEP` pour ce mode. Si un calculator a besoin de vrai threading C++ interne (`std::thread`), voir la section 3.8 — `-pthread` fonctionne bien dans cette architecture, à condition d'un correctif précis.

Flux de données par frame (voir `index.html` + `worker.js`) :

1. **Main thread** (`index.html`) : dessine la frame vidéo courante (`<video>`) sur un `<canvas>` caché, puis `createImageBitmap(sourceCanvas)` — capture un instantané transférable, sans copie.
2. `worker.postMessage({type:"frame", bitmap, width, height}, [bitmap])` — transfert **zero-copy** (le `bitmap` change de propriétaire, pas de copie mémoire).
3. **Worker** (`worker.js`) : dessine le `bitmap` sur un canvas de travail (`OffscreenCanvas`), `getImageData()` pour récupérer les pixels bruts, puis `demo.processFrame(data, width, height)` — tout le pipeline C++/GPU de la section 3.3 s'exécute ici, sur ce thread.
4. Le résultat (vue mémoire wasm) est copié dans un `ArrayBuffer` neuf (`new Uint8Array(result).buffer`) — copie obligatoire, on ne peut pas transférer la mémoire du module wasm lui-même sans la détacher.
5. `self.postMessage({type:"result", buffer, width, height}, [outBuffer])` — retour vers le main thread, encore zero-copy.
6. **Main thread** : `outputCtx.putImageData(...)` affiche le résultat.

**Backpressure** : un flag `frameInFlight` empêche d'envoyer une nouvelle frame au Worker tant que la précédente n'est pas revenue — sinon, si `processFrame` est plus lent que le taux de `requestAnimationFrame`, les messages s'empileraient indéfiniment.

Build : le seul changement nécessaire par rapport au Stage 1 a été le linkopt `-sENVIRONMENT=web,worker` (au lieu de `-sENVIRONMENT=web`), pour que le glue JS généré fonctionne correctement chargé via `importScripts()` dans un Worker plutôt que comme `<script>` classique.

### Build

`-pthread` étant activé dans les `linkopts` de la cible (section 3.8), il faut aussi le passer en flags Bazel globaux — sinon la cause racine devient un échec de lien plus simple (`--shared-memory is disallowed by ... because it was not compiled with 'atomics' or 'bulk-memory' features`), car toute la chaîne de dépendances transitives (framework MediaPipe, protobuf, abseil...) doit être recompilée avec les mêmes features :

```bash
bazel build -c opt --copt=-pthread --linkopt=-pthread //mediapipe/examples/wasm/gpu_video_demo:gpu_video_demo_wasm
```

Les artefacts (`gpu_video_demo.js`, `gpu_video_demo.wasm`) sont générés en lecture seule dans `bazel-bin/` — pour les servir localement, les copier (avec `chmod u+w`) dans un répertoire à part avec `index.html`/`worker.js`.

**`-pthread` exige `SharedArrayBuffer`, donc un serveur qui envoie les en-têtes `Cross-Origin-Opener-Policy: same-origin` et `Cross-Origin-Embedder-Policy: require-corp`** — un `python3 -m http.server` classique ne les envoie pas. `serve_coop_coep.py`, dans ce même répertoire, les ajoute :

```bash
python3 mediapipe/examples/wasm/gpu_video_demo/serve_coop_coep.py /chemin/vers/le/dossier 8080
```

### 3.7 Optimisation résolue : le vrai goulot n'était pas le GPU

À 720p, le pipeline tournait initialement à ~6 FPS (`processFrame` ~266ms). L'hypothèse naturelle — `glReadPixels` (étape 3.3.5), un stall synchrone qui force le GPU à finir tout son travail avant de rendre la main au CPU — s'est révélée **fausse**. Une première tentative de correction basée sur cette hypothèse (rendu direct-à-l'écran via `mediapipe::QuadRenderer`, éliminant le readback CPU) a cassé l'affichage sans qu'on ait mesuré quoi que ce soit au préalable, et a dû être annulée.

**Méthode qui a fonctionné : instrumenter avant de corriger.** Un chronométrage (`performance.now()` côté JS dans `worker.js`, `std::chrono` côté C++ dans `ProcessFrame`) a permis de bisecter précisément où passait le temps :

| Étape | Avant | Après |
|---|---|---|
| `vecFromJSArray<uint8_t>` (copie JS→`std::vector`) | **~275ms** | ~0.25ms |
| `ImageFrame` (ctor + copie) | ~0.3ms | ~0.25ms |
| Upload texture GPU | ~1-2ms | ~1.75ms |
| Shader + readback (dont `glReadPixels`) | ~5ms (dont ~2-4ms) | inchangé |

Le vrai goulot : `emscripten::vecFromJSArray<uint8_t>` marshale le buffer **élément par élément** à travers la machinerie générique `emscripten::val` — un aller-retour JS↔C++ par octet. Pour ~3,6M octets (1280×720×4) à 720p, ça donne exactement les ~275ms observés. Le GPU, le shader et `glReadPixels` n'étaient responsables que de quelques millisecondes — un faux coupable.

**Correction** (`gpu_video_demo_wasm.cc`, méthode `ProcessFrame`) : remplacer la copie élément-par-élément par un seul appel natif `TypedArray.set()`. On expose un buffer wasm persistant (`input_pixels_`, un `std::vector<uint8_t>` membre, redimensionné seulement si la résolution change) comme vue typée via `emscripten::typed_memory_view`, puis on laisse le moteur JS faire la copie en une fois :

```cpp
emscripten::val heap_view(
    emscripten::typed_memory_view(input_pixels_.size(), input_pixels_.data()));
heap_view.call<void>("set", rgba);  // memcpy natif, une seule frontière JS↔C++ franchie
```

Résultat : `processFrame` est passé de ~266ms à ~8.5ms (gain ~31x), largement sous les 33ms nécessaires pour du 30 FPS fluide — confirmé visuellement.

**Leçon générale** : `emscripten::vecFromJSArray` est adapté à de petits tableaux, mais catastrophique pour du binaire volumineux (frames vidéo, buffers audio, etc.). Pour tout transfert de données brutes JS→wasm de taille significative, préférer un buffer wasm persistant + `TypedArray.set()` côté JS plutôt que les helpers de conversion génériques d'Embind.

### 3.8 Multithreading réel (`-pthread`) : ça marche, avec un correctif précis

Le graphe MediaPipe reste et restera toujours mono-thread côté scheduling sous Emscripten (`use_application_thread=true` inconditionnel, cf. piège n°1) — `-pthread` ne change rien à ça. Ce qu'il permet, en revanche, c'est du **vrai threading C++ interne à un calculator** (un `std::thread` manuel dans son implémentation), utile pour paralléliser un traitement lourd à l'intérieur d'un seul nœud du graphe.

**Symptôme initial** : en activant `-pthread` (flags globaux Bazel `--copt=-pthread --linkopt=-pthread` + `-sUSE_PTHREADS=1 -sPTHREAD_POOL_SIZE=N` côté `BUILD`), la page restait bloquée indéfiniment sur "Starting worker...", sans aucune erreur console — reproduit à l'identique avec un pool pré-spawné et avec la création d'un thread à la demande. Seul `PTHREAD_POOL_SIZE=0` (aucun thread jamais créé) fonctionnait.

**Cause racine** (trouvée par lecture directe du glue Emscripten généré, pas déduite) : quand un pthread doit être spawné, `libpthread.js` (`allocateUnusedWorker()`) fait `new Worker(pthreadMainJs)`, où `pthreadMainJs` vient par défaut de `_scriptName`. Or `_scriptName` est déterminé, dans un Worker, par `self.location.href` — et comme `gpu_video_demo.js` est chargé via `importScripts()` **à l'intérieur** de notre propre `worker.js` (même global scope), `self.location.href` vaut l'URL de **`worker.js`**, pas celle du glue compilé. Emscripten spawnait donc un nouveau Worker qui rechargeait `worker.js` depuis zéro, lequel attendait un message `{type:"init"|"frame"}` qui n'arrivait jamais du protocole interne pthread → hang silencieux.

**Correction** (`worker.js`) : fournir explicitement `mainScriptUrlOrBlob` à l'instanciation du module, pour court-circuiter cette détection automatique cassée :

```js
const Module = await GpuVideoDemoModule({
  canvas: msg.canvas,
  mainScriptUrlOrBlob: "gpu_video_demo.js",
});
```

Validé de bout en bout : `initialize()` réussit sans hang, un `std::thread` réel créé/exécuté/joint depuis C++ fonctionne (testé via une méthode `TestThread()` temporaire), et le pipeline webcam + carré GPU reste fluide — aucune régression.

**Leçon générale** : dès qu'un module Emscripten `-pthread` est chargé autrement que comme script d'entrée direct d'un Worker (via `importScripts()` dans un Worker custom, un bundler, un blob, etc.), l'auto-détection de l'URL du script pour spawner de nouveaux pthreads peut se tromper silencieusement. Toujours fournir `Module['mainScriptUrlOrBlob']` explicitement dans ce genre d'architecture hôte custom.

---

## 4. Récapitulatif des concepts clés

| Concept | Où | Pourquoi |
|---|---|---|
| `ObserveOutputStream` au lieu d'`OutputStreamPoller` | Stage 1 + 2 | Un poller bloquant deadlock sous Emscripten (mono-thread forcé) |
| `WaitUntilDone()` vs `WaitUntilIdle()` | Stage 1 vs Stage 2 | Single-shot fermé vs streaming continu, graphe jamais fermé |
| `Module.canvas` avant `GpuResources::Create()` | Stage 2 | `gl_context_webgl.cc` mappe `"#canvas"` sur `Module.canvas` en dur |
| `emscripten::typed_memory_view` | Stage 2 | Retourne une vue mémoire (pas une copie) — à consommer immédiatement côté JS |
| `-sENVIRONMENT=web,worker` | Stage 2 | Nécessaire pour charger le glue JS via `importScripts()` dans un Worker |
| Un seul Worker suffit, pas besoin de `-pthread` pour faire tourner le graphe hors du thread principal | Stage 2 | Le graphe MediaPipe est déjà mono-thread côté scheduling sous Emscripten de toute façon |
| Nom du `cc_binary` = nom des `outputs` du `wasm_cc_binary` | Stage 1 + 2 | Le glue JS référence en dur le nom du fichier `.wasm` d'origine |
| `TypedArray.set()` plutôt que `vecFromJSArray` pour du binaire volumineux | Stage 2 | `vecFromJSArray` marshale élément par élément — ~31x plus lent qu'un memcpy natif sur une frame vidéo |
| `mainScriptUrlOrBlob` explicite si le glue Emscripten est chargé via `importScripts()` dans un Worker custom | Stage 2 (`-pthread`) | Sans lui, l'auto-détection d'URL pour spawner un pthread résout l'URL du Worker hôte, pas celle du glue — hang silencieux |
