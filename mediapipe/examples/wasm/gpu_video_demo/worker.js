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
    const { canvas, id } = msg;
    try {
      // mainScriptUrlOrBlob stays explicit even without -pthread: it's a
      // no-op safety net if pthread bootstrap machinery is ever re-enabled
      // (see historical note this fixed, commit 7b258581c), and costs
      // nothing to keep.
      const Module = await GpuVideoDemoModule({
        canvas,
        mainScriptUrlOrBlob: "gpu_video_demo.js",
      });
      demo = new Module.GpuVideoDemo();
      const initResult = demo.initialize();
      console.log(`[worker ${id}] initialize() result:`, initResult);
      self.postMessage({ type: "ready", id });
    } catch (err) {
      self.postMessage({ type: "error", id, message: String(err) });
    }
    return;
  }

  if (msg.type === "frame") {
    const { bitmap, width, height, seq } = msg;
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
    self.postMessage({ type: "result", buffer: outBuffer, width, height, seq }, [outBuffer]);
    return;
  }

  if (msg.type === "benchmark") {
    const { data, width, height, repeat } = msg;
    // Runs entirely inside this worker, without going back through the
    // event loop between frames: postMessage-ing `repeat` individual
    // "frame" messages would let structured-clone/message overhead of a
    // multi-hundred-KB RGBA buffer dominate the timing this is meant to
    // measure. The processed result is discarded -- only throughput matters
    // for a benchmark run.
    for (let i = 0; i < repeat; i++) {
      demo.processFrame(data, width, height);
    }
    self.postMessage({ type: "benchmark-result", framesProcessed: repeat });
  }
};
