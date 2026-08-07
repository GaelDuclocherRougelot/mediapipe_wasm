// Pool of N Web Workers, each hosting its own fully independent instance of
// the gpu_video_demo wasm module (own CalculatorGraph, own GL context bound
// to its own OffscreenCanvas). This is where the real parallelism lives:
// unlike a single CalculatorGraph (forced synchronous under Emscripten, see
// gpu_video_demo_wasm.cc), N separate worker threads with no shared state
// can each run WaitUntilIdle() concurrently on a different frame.
//
// Specific to this demo (message types "init"/"frame"/"benchmark" match
// worker.js exactly) -- not a generic reusable pool abstraction.
class WorkerPool {
  constructor() {
    this.workers = []; // [{ worker, ready, busy, id }]
    this.nextIndex = 0;
    this.seqCounter = 0;
    this.onResult = null; // (msg: {buffer,width,height,seq}) => void
    this.onWorkerError = null; // (id, message) => void
  }

  get size() {
    return this.workers.length;
  }

  // Tears down any existing pool and spins up `n` fresh workers, waiting
  // until all have replied "ready" before resolving. Rebuilding from
  // scratch (rather than incrementally adding/removing workers) keeps this
  // simple: the wasm-load + initialize() + GL-context cost is only ever
  // paid on an explicit "Apply" click, never per frame.
  async rebuild(n) {
    this.terminate();

    const readyPromises = [];
    for (let id = 0; id < n; id++) {
      const worker = new Worker("worker.js");
      const entry = { worker, ready: false, busy: false, id };
      worker.onmessage = (e) => this._handleMessage(entry, e.data);
      worker.onerror = (e) => {
        entry.busy = false;
        const message = e.message || String(e);
        if (entry._rejectReady) entry._rejectReady(new Error(message));
        if (this.onWorkerError) this.onWorkerError(id, message);
      };
      this.workers.push(entry);

      readyPromises.push(
        new Promise((resolve, reject) => {
          entry._resolveReady = resolve;
          entry._rejectReady = reject;
        })
      );

      const canvas = new OffscreenCanvas(320, 240);
      worker.postMessage({ type: "init", id, canvas }, [canvas]);
    }

    try {
      await Promise.all(readyPromises);
    } catch (err) {
      // Don't leave a half-initialized pool around after a failed rebuild.
      this.terminate();
      throw err;
    }
  }

  terminate() {
    for (const entry of this.workers) entry.worker.terminate();
    this.workers = [];
    this.nextIndex = 0;
  }

  hasFreeWorker() {
    return this.workers.some((w) => w.ready && !w.busy);
  }

  busyCount() {
    return this.workers.filter((w) => w.busy).length;
  }

  // Live mode: round-robins one frame to the next free worker. Returns the
  // assigned sequence number, or null (closing the bitmap) if every worker
  // is currently busy -- this is the per-worker backpressure that replaces
  // the old single global frameInFlight flag.
  dispatchFrame(bitmap, width, height) {
    const idx = this._findFreeWorker();
    if (idx === -1) {
      bitmap.close();
      return null;
    }
    const entry = this.workers[idx];
    entry.busy = true;
    const seq = ++this.seqCounter;
    entry.worker.postMessage({ type: "frame", bitmap, width, height, seq }, [bitmap]);
    return seq;
  }

  // Benchmark mode: sends counts[i] repeats to worker i (0/undefined =
  // skip that worker). `data` is structured-cloned to each worker that
  // gets a nonzero count -- it can't be transferred since more than one
  // worker may need its own copy. Resolves once every dispatched worker has
  // reported back.
  runBenchmarkDistributed(counts, data, width, height) {
    const promises = [];
    for (let i = 0; i < this.workers.length; i++) {
      const repeat = counts[i] || 0;
      if (repeat === 0) continue;
      const entry = this.workers[i];
      entry.busy = true;
      promises.push(
        new Promise((resolve) => {
          entry._resolveBenchmark = resolve;
        })
      );
      entry.worker.postMessage({ type: "benchmark", data, width, height, repeat });
    }
    return Promise.all(promises);
  }

  _findFreeWorker() {
    for (let i = 0; i < this.workers.length; i++) {
      const idx = (this.nextIndex + i) % this.workers.length;
      if (this.workers[idx].ready && !this.workers[idx].busy) {
        this.nextIndex = (idx + 1) % this.workers.length;
        return idx;
      }
    }
    return -1;
  }

  _handleMessage(entry, msg) {
    if (msg.type === "ready") {
      entry.ready = true;
      entry._resolveReady();
    } else if (msg.type === "error") {
      if (entry._rejectReady) entry._rejectReady(new Error(msg.message));
    } else if (msg.type === "result") {
      entry.busy = false;
      if (this.onResult) this.onResult(msg);
    } else if (msg.type === "benchmark-result") {
      entry.busy = false;
      if (entry._resolveBenchmark) entry._resolveBenchmark(msg.framesProcessed);
    }
  }
}
