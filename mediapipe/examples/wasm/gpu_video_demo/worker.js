// Runs the MediaPipe wasm module entirely inside this Worker, off the main
// thread. No Emscripten -pthread/SharedArrayBuffer is needed: the module
// itself stays single-threaded (as it already is under Emscripten, see
// Stage 1/2 notes on CalculatorGraph's forced application-thread mode) --
// it just happens to be *this* worker's thread instead of the page's main
// thread. WebGL comes from an OffscreenCanvas transferred in from the main
// thread, which gl_context_webgl.cc picks up via Module.canvas.
importScripts("gpu_video_demo.js");

let demo = null;
let scratchCanvas = null;
let scratchCtx = null;

self.onmessage = async (e) => {
  const msg = e.data;

  if (msg.type === "init") {
    try {
      // Without this, Emscripten's pthread bootstrap (libpthread.js,
      // allocateUnusedWorker) resolves its own script URL via
      // self.location.href, which -- because this file was reached via
      // importScripts() rather than being the Worker's own entry script --
      // still points at worker.js instead of gpu_video_demo.js. Every new
      // pthread Worker then reloads worker.js from scratch and waits for a
      // {type:"init"|"frame"} message that never comes, hanging forever
      // with no console error. Providing mainScriptUrlOrBlob explicitly
      // bypasses that broken auto-detection.
      const Module = await GpuVideoDemoModule({
        canvas: msg.canvas,
        mainScriptUrlOrBlob: "gpu_video_demo.js",
      });
      demo = new Module.GpuVideoDemo();
      const initResult = demo.initialize();
      console.log("[worker] initialize() result:", initResult);
      self.postMessage({ type: "ready" });
    } catch (err) {
      self.postMessage({ type: "error", message: String(err) });
    }
    return;
  }

  if (msg.type === "frame") {
    const { bitmap, width, height } = msg;
    if (!scratchCanvas || scratchCanvas.width !== width || scratchCanvas.height !== height) {
      scratchCanvas = new OffscreenCanvas(width, height);
      scratchCtx = scratchCanvas.getContext("2d", { willReadFrequently: true });
    }

    scratchCtx.drawImage(bitmap, 0, 0);
    bitmap.close();
    const { data } = scratchCtx.getImageData(0, 0, width, height);

    const result = demo.processFrame(data, width, height);
    // `result` is a view into the wasm module's own heap memory (invalidated
    // by the next allocation). Copy it into a fresh ArrayBuffer we own
    // before transferring it back to the main thread -- transferring wasm's
    // own memory would detach it from the module.
    const outBuffer = new Uint8Array(result).buffer;
    self.postMessage({ type: "result", buffer: outBuffer, width, height }, [outBuffer]);
  }
};
