// Synchronous reads of the disc image.
// All engine reads (/webdisc/<name>, browser_pre.js) end up on the main thread
// (file operations from pthreads are also passed to the main thread). Here only blocks missing from the block cache
// are requested from disc-worker.js, and the main thread waits until they are done (the main thread
// cannot sleep in Atomics.wait, so it spins).
'use strict';

class DiscBridge {
  constructor(file, { blockSize = 256 * 1024, cacheBytes = 192 * 1024 * 1024, chunkBytes = 2 * 1024 * 1024 } = {}) {
    if (typeof SharedArrayBuffer === 'undefined' || !self.crossOriginIsolated)
      throw new Error('SharedArrayBuffer is not available (the page must be served with the COOP/COEP headers)');
    this.file = file;
    this.name = file.name.replace(/[\/\\]/g, '_') || 'disc.iso';
    this.size = file.size;
    this.blockSize = blockSize;
    this.maxBlocks = Math.max(8, Math.floor(cacheBytes / blockSize));
    this.chunkBlocks = Math.max(1, Math.floor(chunkBytes / blockSize));
    this.cache = new Map(); // block number -> Uint8Array (Map order is use order: LRU)
    // ctrl: [0] state, [1] length, [2][3] position, [4] request number, [5] number of the answered request (disc-worker.js)
    this.ctrlBuf = new SharedArrayBuffer(24);
    this.seq = 0;
    this.dataBuf = new SharedArrayBuffer(this.chunkBlocks * blockSize);
    this.ctrl = new Int32Array(this.ctrlBuf);
    this.data = new Uint8Array(this.dataBuf);
    this.worker = new Worker('disc-worker.js');
    // Always wait before reading (ready): spinning on a Worker that has not started yet leaves the main thread stuck
    this.ready = new Promise((resolve, reject) => {
      this.worker.onmessage = (e) => { if (e.data && e.data.type === 'ready') resolve(this); };
      this.worker.onerror = (e) => reject(new Error('disc-worker.js: ' + (e.message || 'failed to start')));
    });
    this.worker.postMessage({ type: 'init', file, ctrl: this.ctrlBuf, data: this.dataBuf });
    this.stats = { reads: 0, bytes: 0, misses: 0, fetchedBytes: 0, waitMs: 0, maxWaitMs: 0 };
  }

  // length bytes from position (the caller keeps it within size)
  read(position, length) {
    const out = new Uint8Array(length);
    const bs = this.blockSize;
    let done = 0;
    this.stats.reads++;
    this.stats.bytes += length;
    while (done < length) {
      const pos = position + done;
      const index = Math.floor(pos / bs);
      const block = this.block(index);
      const off = pos - index * bs;
      const n = Math.min(block.length - off, length - done);
      if (n <= 0) throw new Error(`disc: no data at ${pos}`);
      out.set(block.subarray(off, off + n), done);
      done += n;
    }
    return out;
  }

  block(index) {
    let b = this.cache.get(index);
    if (b) {
      this.cache.delete(index); // move the most recently used to the back
      this.cache.set(index, b);
      return b;
    }
    this.fetch(index);
    return this.cache.get(index);
  }

  // From index, read up to chunkBlocks consecutive blocks not in the cache (doubles as read-ahead)
  fetch(index) {
    const bs = this.blockSize;
    const lastBlock = Math.floor((this.size - 1) / bs);
    let count = 1;
    while (count < this.chunkBlocks && index + count <= lastBlock && !this.cache.has(index + count)) count++;
    const pos = index * bs;
    const len = Math.min(count * bs, this.size - pos);
    const t0 = performance.now();
    const seq = this.seq = (this.seq + 1) | 0;
    const ask = () => {
      this.ctrl[1] = len;
      this.ctrl[2] = pos % 4294967296;
      this.ctrl[3] = Math.floor(pos / 4294967296);
      this.ctrl[4] = seq;
      Atomics.store(this.ctrl, 0, 1);
      Atomics.notify(this.ctrl, 0);
    };
    ask();
    // Wait for the Worker to finish reading. Even a file on the network should take a few seconds per read at most, so
    // give up after 30 seconds and treat it as a read failure (so the whole page does not freeze). If the answer to an abandoned request
    // arrives later (the answered number differs), drop it and reissue the current request (the Worker overwrote the request fields meanwhile)
    for (;;) {
      if (Atomics.load(this.ctrl, 0) === 2) {
        if (this.ctrl[5] === seq) break;
        ask();
      }
      if (performance.now() - t0 > 30000) throw new Error(`disc: no answer from the worker for 30 s (at ${pos})`);
    }
    const got = this.ctrl[1];
    Atomics.store(this.ctrl, 0, 0);
    const ms = performance.now() - t0;
    this.stats.misses++;
    this.stats.fetchedBytes += Math.max(got, 0);
    this.stats.waitMs += ms;
    this.stats.maxWaitMs = Math.max(this.stats.maxWaitMs, ms);
    if (got !== len) throw new Error(`disc: read failed at ${pos} (${len} bytes, got ${got})`);
    for (let i = 0; i < count; i++) {
      const start = i * bs;
      if (start >= got) break;
      this.cache.set(index + i, this.data.slice(start, Math.min(start + bs, got)));
    }
    while (this.cache.size > this.maxBlocks) this.cache.delete(this.cache.keys().next().value);
  }

  // Done (when a file with the same name is picked again): stop the Worker and drop the cache
  close() {
    this.worker.terminate();
    this.cache.clear();
  }

  // Measurement summary (type gt3rDiscStats() in the console)
  summary() {
    const s = this.stats;
    return {
      reads: s.reads, readMB: +(s.bytes / 1e6).toFixed(1), misses: s.misses,
      fetchedMB: +(s.fetchedBytes / 1e6).toFixed(1), waitMs: Math.round(s.waitMs),
      maxWaitMs: +s.maxWaitMs.toFixed(1), cachedMB: +((this.cache.size * this.blockSize) / 1e6).toFixed(0),
    };
  }
}

self.DiscBridge = DiscBridge;
