# MediaPipe custom calculators dans le navigateur (WASM) — comment ça marche

Ce document explique l'architecture des deux démos WASM du repo :

- `mediapipe/examples/wasm/quantiq_demo/` — **Stage 1** : CPU only, preuve de concept minimale.
- `mediapipe/examples/wasm/gpu_video_demo/` — **Stage 2** : GPU (WebGL), flux vidéo webcam, calculateur custom avec shader, exécution dans un pool de Web Workers dont la taille est réglable depuis l'UI (profiling du speedup en direct + benchmark dédié).

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
- `mediapipe/examples/wasm/gpu_video_demo/index.html` — main thread : webcam, affichage, contrôle du nombre de workers, benchmark.
- `mediapipe/examples/wasm/gpu_video_demo/worker.js` — Web Worker : héberge une instance complète et indépendante du module wasm.
- `mediapipe/examples/wasm/gpu_video_demo/worker-pool.js` — pool de N `worker.js`, distribution des frames et des runs de benchmark (voir 3.11).

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

### 3.6 Architecture Web Worker : un pool de N, pas un seul

Chaque **Worker du pool héberge sa propre instance complète et indépendante** du module wasm (son propre graphe MediaPipe, son propre `GpuCalculatorHelper`, son propre contexte WebGL lié à son propre `OffscreenCanvas`) — pas sur le thread JS principal, qui reste réactif (scroll, interactions) même si le traitement GPU/CPU par frame prend du temps.

**Important : ceci n'a rien à voir avec `-pthread`/multithreading Emscripten.** Chaque Worker héberge un module Emscripten mono-thread par défaut (comme il l'est de toute façon sous Emscripten côté scheduling MediaPipe, cf. piège n°1) — il s'exécute juste sur le thread de ce Worker plutôt que sur le thread principal de la page. Le vrai parallélisme ne vient donc pas d'un multithreading interne à une instance, mais du fait que plusieurs Workers, chacun avec son propre état totalement indépendant, peuvent traiter des frames différentes en même temps — voir 3.11. Pas besoin de `SharedArrayBuffer` ni des en-têtes `COOP`/`COEP` pour ce mode (contrairement au vrai `std::thread` interne d'un calculator, voir 3.8).

Flux de données par frame en mode live (voir `index.html` + `worker-pool.js` + `worker.js`) :

1. **Main thread** (`index.html`) : dessine la frame vidéo courante (`<video>`) sur un `<canvas>` caché, puis `createImageBitmap(sourceCanvas)` — capture un instantané transférable, sans copie.
2. `WorkerPool.dispatchFrame(bitmap, width, height)` choisit, en round-robin, le prochain Worker **libre** du pool et lui fait `postMessage({type:"frame", bitmap, width, height, seq}, [bitmap])` — transfert **zero-copy** (le `bitmap` change de propriétaire), avec un numéro de séquence `seq` monotone croissant.
3. **Worker** (`worker.js`) : dessine le `bitmap` sur un canvas de travail (`OffscreenCanvas`), `getImageData()` pour récupérer les pixels bruts, puis `demo.processFrame(data, width, height)` — tout le pipeline C++/GPU de la section 3.3 s'exécute ici, sur ce thread, indépendamment des autres Workers du pool.
4. Le résultat (vue mémoire wasm) est copié dans un `ArrayBuffer` neuf (`new Uint8Array(result).buffer`) — copie obligatoire, on ne peut pas transférer la mémoire du module wasm lui-même sans la détacher.
5. `self.postMessage({type:"result", buffer, width, height, seq}, [outBuffer])` — retour vers le main thread, encore zero-copy.
6. **Main thread** : si `seq` est plus récent que le dernier résultat affiché, `outputCtx.putImageData(...)`. Sinon, le résultat est ignoré — avec plusieurs Workers en vol, les réponses peuvent arriver dans le désordre, et afficher un résultat périmé ferait visiblement "reculer" la vidéo.

**Backpressure par Worker** : `dispatchFrame` n'envoie une frame que s'il existe un Worker libre (`hasFreeWorker()`) ; sinon la frame courante est simplement ignorée (le `bitmap` est fermé) — jusqu'à N frames peuvent être en vol simultanément, une par Worker, contre une seule globalement avant l'introduction du pool.

Build : le seul changement nécessaire par rapport au Stage 1 a été le linkopt `-sENVIRONMENT=web,worker` (au lieu de `-sENVIRONMENT=web`), pour que le glue JS généré fonctionne correctement chargé via `importScripts()` dans un Worker plutôt que comme `<script>` classique.

### Build

Aucun flag `-pthread` n'est nécessaire pour `gpu_video_demo` : ni `CalculatorGraph` (mono-thread forcé sous Emscripten, piège n°1) ni `GpuVideoDemo` elle-même n'utilisent de `std::thread` interne — le parallélisme vient entièrement du pool de Workers JS (3.6/3.11), qui sont des threads OS indépendants sans avoir besoin de `SharedArrayBuffer`. Le build reste donc aussi simple que le Stage 1 :

```bash
bazel build -c opt //mediapipe/examples/wasm/gpu_video_demo:gpu_video_demo_wasm
```

Les artefacts (`gpu_video_demo.js`, `gpu_video_demo.wasm`) sont générés en lecture seule dans `bazel-bin/` — pour les servir localement, les copier (avec `chmod u+w`) dans un répertoire à part avec `index.html`/`worker.js`/`worker-pool.js`.

Comme il n'y a plus de `SharedArrayBuffer`, les en-têtes `COOP`/`COEP` ne sont plus strictement requis pour cette démo — un `python3 -m http.server` classique suffit. `serve_coop_coep.py`, dans ce même répertoire, reste utilisable sans effet de bord si on préfère le garder par habitude (ou pour tester d'autres démos qui, elles, en ont besoin, cf. 3.9/3.10) :

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

**Mise à jour (3.11)** : `-pthread`/`-sUSE_PTHREADS=1`/`-sPTHREAD_POOL_SIZE=4` ont depuis été **retirés** du `BUILD` de `gpu_video_demo` — la démo n'a jamais eu besoin de `std::thread` interne (le `TestThread()` de cette section était temporaire, pour la preuve), et avec un pool de N Workers (3.11), chaque instance wasm préspawnerait sinon N×4 pthreads inutiles au démarrage. Le vrai travail CPU parallèle est démontré séparément dans `pthread_sort_demo`/`pthread_search_demo` (3.9/3.10). `mainScriptUrlOrBlob` reste fourni dans `worker.js` par précaution (coût nul, filet de sécurité si `-pthread` est un jour réactivé) mais n'est plus strictement exercé.

### 3.9 Preuve de concept : du vrai travail CPU en parallèle (`pthread_sort_demo/`)

La section 3.8 prouve que la création de threads fonctionne ; elle ne prouve pas encore qu'un vrai travail CPU tourne **en parallèle** avec un gain de perf mesurable. `mediapipe/examples/wasm/pthread_sort_demo/` comble ce trou : un exemple autonome, **sans aucune dépendance MediaPipe** (juste la lib standard C++ + Embind), qui trie le même tableau aléatoire de deux façons et chronomètre les deux :

- **Mono-thread** : `std::sort` classique sur tout le tableau.
- **Multi-thread** : le tableau est découpé en `N` tranches contiguës ; chaque tranche est triée par son propre `std::thread` (tous partagent la même mémoire linéaire wasm, via `SharedArrayBuffer` — c'est ce que `-pthread` fournit concrètement) ; une fois les `N` threads joints, une fusion (`std::inplace_merge` en cascade façon tri fusion) reconstitue l'ordre global.

**Résultat mesuré** (20 000 000 entiers, 8 threads, Chrome) :

```
size=20000000 numThreads=8
single-threaded std::sort: 3485.52 ms (sorted=1)
multi-threaded (8 threads) sort+merge: 924.38 ms (sorted=1)
speedup: 3.77066x
```

Les deux résultats sont correctement triés (`sorted=1`) — ce n'est pas juste "ça ne plante pas", c'est un vrai gain de perf sur du travail réel.

**Pourquoi ~3.8x et pas 8x avec 8 threads** (attendu, pas un problème) :
1. La fusion finale reste **entièrement séquentielle** — seul le tri des tranches est parallélisé, la fusion ajoute un coût non parallélisé qui plafonne le gain global. La paralléliser complètement demanderait un algorithme de fusion parallèle (split par rang via recherche binaire), plus complexe.
2. Le tri est **memory-bound**, pas purement CPU-bound — les threads se partagent la même bande passante mémoire, donc la scalabilité linéaire avec le nombre de cœurs n'est jamais atteinte en pratique, même en C++ natif hors navigateur.

**Build** : ne nécessite **pas** les flags globaux `--copt=-pthread --linkopt=-pthread` (contrairement à `gpu_video_demo/`) — sans dépendance MediaPipe à recompiler, les `copts`/`linkopts` `-pthread` locaux à la cible suffisent :

```bash
bazel build -c opt //mediapipe/examples/wasm/pthread_sort_demo:pthread_sort_demo_wasm
```

Ce contraste confirme que l'exigence de flags globaux (section 3.8) vient bien de la nécessité de recompiler tout l'arbre de dépendances transitif avec les mêmes features `atomics`/`bulk-memory`, pas d'une propriété intrinsèque de `-pthread`. Se sert avec le même `serve_coop_coep.py` (COOP/COEP requis pour `SharedArrayBuffer`).

### 3.10 Parallélisme sans phase de merge : recherche avec arrêt anticipé (`pthread_search_demo/`)

Le tri (section 3.9) est le cas où chaque worker produit un résultat **partiel** qu'il faut recombiner (`MergeSortedChunks`) — le coût de cette fusion, entièrement séquentielle, plafonne le speedup observé (~3.8x sur 8 threads, pas 8x). `mediapipe/examples/wasm/pthread_search_demo/` illustre l'autre famille de problèmes parallélisables : la recherche d'une valeur cible, où le résultat de n'importe quel worker (dès qu'il trouve) **suffit** — pas besoin de recombiner les résultats partiels des autres.

**Mécanisme** : le tableau est découpé en `N` tranches contiguës comme pour le tri, mais au lieu d'un résultat par tranche à fusionner, un seul `std::atomic<int> found_index` (initialisé à `-1`) est partagé entre tous les workers :
- **Découverte** : le worker qui trouve la cible fait un `compare_exchange_strong(expected=-1, found_index)` — le premier à passer gagne, les autres tentatives échouent silencieusement (peu importe, un seul résultat est nécessaire).
- **Arrêt anticipé** : chaque worker relit `found_index` tous les `kCheckInterval` (4096) éléments scannés, et retourne immédiatement si un autre worker a déjà trouvé — évite de gaspiller du travail CPU une fois la réponse connue. La vérification n'est pas faite à chaque itération : un load atomique + branchement à chaque élément alourdirait inutilement la boucle chaude, alors que des lectures concurrentes sur la même ligne de cache ne se contentionnent pas entre elles (seule une écriture invaliderait le cache des autres lecteurs) — batcher la vérification est donc un compromis de perf, pas une nécessité de correction.

**Résultats mesurés** (200 000 000 entiers, 8 threads, Chrome, `serve_coop_coep.py`), trois scénarios via le paramètre `targetPercent` (position de la cible en % du tableau, `-1` = absente) :

| Scénario | Mono-thread | Multi-thread (8 threads) | Speedup |
|---|---|---|---|
| Absente (`targetPercent=-1`, pire cas : scan complet des deux côtés) | 116.9 ms | 36.2 ms | **3.23x** |
| Présente à 90 % (`targetPercent=90`) | 62.0 ms | 7.2 ms | **8.54x** |
| Présente à 1 % (`targetPercent=1`) | 1.0 ms | 5.3 ms | **0.19x** (plus lent !) |

Le cas absent retrouve un profil similaire au tri memory-bound (section 3.9) — logique, c'est un pur scan sans avantage de position. Le cas à 90 % dépasse le speedup linéaire en apparence : la cible tombe dans la tranche du dernier worker, qui la trouve après un scan court, pendant que le mono-thread doit balayer 90 % du tableau en séquentiel — les deux mesurent des quantités de travail différentes, ce n'est pas une vraie super-linéarité.

**Le cas à 1 % est le plus instructif** : quand la cible est trouvée quasi instantanément par un scan séquentiel, l'overhead de création des 8 `std::thread` (et du protocole `SharedArrayBuffer`/pthread sous-jacent) dépasse largement le travail utile — la version parallèle est mesurée **5x plus lente**. Ce n'est pas un bug, c'est la limite attendue : paralléliser n'a de sens que si le travail à faire dépasse l'overhead de mise en place des workers. Un vrai pipeline devrait donc réserver ce pattern aux recherches où l'échec (ou une position tardive) est probable, ou basculer dynamiquement mono/multi-thread selon la taille des données.

**Build** : identique à `pthread_sort_demo/` — pas de flags globaux Bazel nécessaires (pas de dépendance MediaPipe à recompiler) :

```bash
bazel build -c opt //mediapipe/examples/wasm/pthread_search_demo:pthread_search_demo_wasm
```

**Leçon générale** : face à un problème parallélisable, la forme de la combinaison finale (recombiner tous les résultats partiels vs. n'en garder qu'un premier) change fondamentalement le profil de speedup atteignable. Un `std::atomic` fait à la fois office de mécanisme de "premier gagnant" (`compare_exchange_strong`) et de signal d'arrêt anticipé (`load` périodique) — sans mutex ni condition variable — chaque fois que la sémantique du problème permet de s'arrêter dès qu'**un** worker a fini, plutôt que d'attendre que **tous** aient fini.

### 3.11 Pool de N Workers réglable depuis l'UI, et benchmark de speedup réel

Les sections 3.9/3.10 prouvent le speedup `-pthread` sur du travail CPU pur, sans dépendance MediaPipe. `gpu_video_demo` applique la même idée — plusieurs unités de calcul indépendantes traitant des parts différentes du travail en parallèle — mais avec des Web Workers plutôt que des `std::thread`, puisque `CalculatorGraph` reste de toute façon mono-thread en interne sous Emscripten (piège n°1) : le parallélisme ne peut donc pas venir de l'intérieur d'une seule instance du graphe, il doit venir de **plusieurs instances indépendantes**.

**`worker-pool.js`** (classe `WorkerPool`, spécifique à cette démo) gère le cycle de vie du pool :
- `rebuild(n)` termine le pool existant et recrée `n` Workers, chacun avec son propre `OffscreenCanvas`, et attend que tous répondent `ready` avant de résoudre — reconstruction complète à chaque changement de `n` (pas d'ajout/retrait incrémental), pour ne payer le coût de démarrage (chargement wasm + `initialize()` + contexte GL) qu'au clic sur "Apply", jamais par frame.
- `dispatchFrame(...)` (mode live) distribue en round-robin vers le prochain Worker libre.
- `runBenchmarkDistributed(counts, ...)` (mode benchmark) envoie `counts[i]` répétitions au Worker `i`.

**Mode live** : le compteur FPS agrégé de l'UI reflète le débit combiné de tous les Workers du pool. Le gain observable ici reste plafonné par le débit `requestAnimationFrame`/webcam (~30-60 FPS) — au-delà de 1-2 Workers, l'ajout de capacité de calcul supplémentaire ne se traduit plus forcément par un FPS affiché plus élevé, simplement parce que la source ne fournit pas de frames plus vite. C'est un résultat de profiling valide en soi (le pipeline sature en amont, pas en aval), mais insuffisant pour mesurer un vrai speedup de calcul.

**Mode benchmark**, dédié à cette mesure : le bouton "Run benchmark" capture une seule frame de référence, la traite `M` fois en rafale (répartie sur les `n` Workers actuels du pool via `runBenchmarkDistributed`), sans passer par `requestAnimationFrame` ni la webcam — ce qui isole le vrai coût de calcul de `processFrame`. Une répétition est envoyée en **un seul message `benchmark{data, width, height, repeat}`** par Worker plutôt qu'en `M` messages `frame` individuels : à ~307 Ko par frame RGBA (320×240×4), cloner et transiter par l'event loop `M` fois aurait pollué la mesure de débit avec de l'overhead de messagerie plutôt que du calcul réel. Le worker boucle en interne sur `demo.processFrame(...)`, sans renvoyer les images (seul le débit compte).

Le run mesure deux temps avec `performance.now()` côté main thread (le C++ ne peut pas chronométrer across-Workers) :
1. **Baseline 1 Worker** : les `M` répétitions envoyées à `workers[0]` du pool courant (pas de nouveau spawn).
2. **`n` Workers** : les mêmes `M` répétitions réparties en `M/n` par Worker.

`speedup = temps(1 worker) / temps(n workers)`, affiché avec le débit en frames/s de chaque phase — même format de sortie que `pthread_sort_demo`/`pthread_search_demo` (3.9/3.10). Comme pour ces démos, ne pas s'attendre à un speedup linéaire en `n` : chaque Worker porte son propre contexte WebGL indépendant, mais tous partagent le même pilote/GPU physique sous-jacent, qui peut sérialiser une partie du travail entre contextes.

**Résultat mesuré** (1000 frames 320×240, 8 workers, Chrome) :

```
frames=1000 workers=8
1 worker:   1089.0 ms (918.3 fps)
8 workers:  701.0 ms (1426.5 fps)
speedup: 1.55x
```

Speedup nettement inférieur à celui de `pthread_sort_demo` (~3.8x/8 threads) ou `pthread_search_demo` (jusqu'à ~8.5x) — attendu, et pour une raison différente de la fusion séquentielle (3.9) ou de l'overhead de création de threads (3.10) : ici, le travail n'est pas CPU-bound mais **GPU-bound**. Le CPU peut bien lancer 8 workers en parallèle, mais leurs 8 contextes WebGL indépendants soumettent tous leurs commandes de rendu au **même GPU physique**, via le même processus GPU du navigateur, qui sérialise les command buffers entrants — un goulot d'étranglement matériel partagé, analogue en esprit à la bande passante mémoire partagée qui plafonne le tri (3.9), mais situé une couche plus bas (driver/GPU plutôt que RAM). `glReadPixels` (readback GPU→CPU, un point de synchronisation qui stall le pipeline GPU pour ce contexte) ajoute de la contention supplémentaire quand 8 contextes l'appellent concurremment. À ~1.1ms/frame en single-worker (320×240, déjà très rapide grâce à l'optimisation 3.7), l'overhead fixe par frame (dispatch, synchronisation GPU) pèse aussi proportionnellement plus que sur un gros batch CPU-bound.

**Leçon générale** : la nature du goulot d'étranglement détermine la stratégie de parallélisation qui paie. Paralléliser des `std::thread` CPU-bound sur des cœurs indépendants (3.9/3.10) et paralléliser des Workers GPU-bound partageant un seul GPU physique (ici) ne donnent pas le même profil de speedup, même avec la même architecture "N unités indépendantes, pas de merge" — le vrai plafond n'est pas toujours là où l'architecture logicielle le laisse penser.

**Cas limites** : `n` est clampé à un minimum de 1 côté UI (0/invalide → 1) ; `n` > `navigator.hardwareConcurrency` est autorisé avec un avertissement non bloquant (comme 3.9/3.10, pas de plafond dur) ; les boutons "Apply" et "Run benchmark" se désactivent mutuellement le temps de leur opération respective, pour éviter qu'une reconstruction du pool ne termine des Workers avec un run de benchmark encore en attente de leur réponse (ce qui figerait le bouton benchmark indéfiniment sans reconstruction complète de page).

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
| `-pthread` produit un vrai gain de perf mesuré (~3.8x/8 threads), pas juste "ça ne plante pas" | `pthread_sort_demo/` | N `std::thread` partageant la même mémoire wasm (`SharedArrayBuffer`) trient des tranches en parallèle ; le speedup n'est pas linéaire à cause de la fusion séquentielle et de la bande passante mémoire partagée |
| `std::atomic<int>` comme signal "premier gagnant" + arrêt anticipé, sans merge | `pthread_search_demo/` | Un `compare_exchange_strong` pour désigner le gagnant, un `load` périodique pour que les autres workers s'arrêtent tôt ; speedup jusqu'à ~8.5x (cible tardive) mais peut être **plus lent** qu'un seul thread si la cible est trouvée quasi instantanément (overhead de création des threads non amorti) |
| Pool de N Web Workers, chacun sa propre instance wasm indépendante | Stage 2 (`worker-pool.js`) | Puisque `CalculatorGraph` reste mono-thread en interne sous Emscripten, le parallélisme ne peut venir que de plusieurs instances totalement indépendantes, pas d'un `-pthread` interne au graphe |
| Benchmark en rafale (un message par Worker, `repeat` en boucle interne) plutôt que `M` messages individuels | Stage 2 (3.11) | Isole le vrai coût de calcul du coût de messagerie/structured-clone d'une frame vidéo répétée `M` fois |
