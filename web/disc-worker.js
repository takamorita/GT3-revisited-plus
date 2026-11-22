// Worker that reads the disc image.
// Receives a File and 2 SharedArrayBuffers from the page (disc-bridge.js),
// reads the requested range synchronously with FileReaderSync, and writes it to shared memory.
//
// ctrl (Int32Array): [0] state 0=idle 1=requested 2=done, [1] length (bytes read when done, -1=failure),
//                    [2] low 32 bits of position, [3] high 32 bits of position, [4] request number, [5] number of the answered request
//                    (so the answer to a request the page gave up on after 30 seconds is not mistaken for the answer to the next one)
// data (Uint8Array): bytes read
'use strict';

self.onmessage = (e) => { // only the first 'init' arrives (picking again makes the page create a new Worker)
  const file = e.data.file;
  const ctrl = new Int32Array(e.data.ctrl);
  const data = new Uint8Array(e.data.data);
  const reader = new FileReaderSync();
  // Signal that it started. The page waits for this before handing it to the engine (while the main thread spins
  // waiting on a read, this Worker's startup may not progress)
  self.postMessage({ type: 'ready' });
  for (;;) {
    const state = Atomics.load(ctrl, 0);
    if (state !== 1) {
      Atomics.wait(ctrl, 0, state);
      continue;
    }
    const len = ctrl[1];
    const pos = (ctrl[2] >>> 0) + (ctrl[3] >>> 0) * 4294967296; // Int32Array, so convert to unsigned
    const seq = ctrl[4];
    let n = -1;
    try {
      const buf = reader.readAsArrayBuffer(file.slice(pos, pos + len));
      data.set(new Uint8Array(buf), 0);
      n = buf.byteLength;
    } catch (err) {
      // NotReadableError: the file changed or was removed after it was picked
      console.error('disc-worker: read failed at', pos, len, err);
    }
    ctrl[1] = n;
    ctrl[5] = seq;
    Atomics.store(ctrl, 0, 2);
    Atomics.notify(ctrl, 0);
  }
};
