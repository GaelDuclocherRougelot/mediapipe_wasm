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

// Timing instrumentation to find the real bottleneck before attempting any
// further optimization (root-caused, not guessed).
let timings = { draw: 0, getImageData: 0, processFrame: 0, postMessage: 0 };
let timingFrames = 0;

self.onmessage = async (e) => {
  const msg = e.data;

  if (msg.type === "init") {
    try {
      const Module = await GpuVideoDemoModule({ canvas: msg.canvas });
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

    let t0 = performance.now();
    scratchCtx.drawImage(bitmap, 0, 0);
    bitmap.close();
    let t1 = performance.now();
    const { data } = scratchCtx.getImageData(0, 0, width, height);
    let t2 = performance.now();

    const result = demo.processFrame(data, width, height);
    let t3 = performance.now();

    // `result` is a view into the wasm module's own heap memory (invalidated
    // by the next allocation). Copy it into a fresh ArrayBuffer we own
    // before transferring it back to the main thread -- transferring wasm's
    // own memory would detach it from the module.
    const outBuffer = new Uint8Array(result).buffer;
    self.postMessage({ type: "result", buffer: outBuffer, width, height }, [outBuffer]);
    let t4 = performance.now();

    timings.draw += t1 - t0;
    timings.getImageData += t2 - t1;
    timings.processFrame += t3 - t2;
    timings.postMessage += t4 - t3;
    timingFrames++;
    if (timingFrames >= 30) {
      console.log(
        `[worker] avg ms/frame over ${timingFrames} frames -- ` +
        `draw: ${(timings.draw / timingFrames).toFixed(2)}, ` +
        `getImageData: ${(timings.getImageData / timingFrames).toFixed(2)}, ` +
        `processFrame: ${(timings.processFrame / timingFrames).toFixed(2)}, ` +
        `postMessage: ${(timings.postMessage / timingFrames).toFixed(2)}`
      );
      timings = { draw: 0, getImageData: 0, processFrame: 0, postMessage: 0 };
      timingFrames = 0;
    }
  }
};
