// One of the game's UDP sockets in a browser (src/net/socket_web.cpp): a WebSocket to a relay server's port for
// browsers (gt3r-relay --ws-port). The game and this worker share a Link in the game's memory: a few counters and two
// rings of messages, "in" (filled here from the WebSocket) and "out" (emptied here into it). A message is one
// datagram as the relay's gateway carries it: a kind byte, the other end's address, the datagram's bytes.
'use strict';

// The Link's counters (socket_web.cpp has the same).
const STATE = 0, IN_WRITE = 1, IN_READ = 2, OUT_WRITE = 3, OUT_READ = 4;
const CONNECTING = 0, OPEN = 1, CLOSED = 2, CLOSING = 3, DONE = 4;

self.onmessage = (e) => {
  const { memory, base, slots, slotBytes, url } = e.data;
  const ctrl = new Int32Array(memory, base, 8);
  const view = new DataView(memory);
  const bytes = new Uint8Array(memory);
  const slotSize = 4 + slotBytes;
  const inBase = base + 32, outBase = inBase + slots * slotSize;
  let ws = null, finished = false;

  const wakeReader = () => Atomics.notify(ctrl, IN_WRITE);
  // The game asked to close (or the connection went): the Link is free for its next socket.
  const finish = () => {
    if (finished) return;
    finished = true;
    try { if (ws) ws.close(); } catch (err) {}
    Atomics.store(ctrl, STATE, DONE);
    wakeReader();
    self.close();
  };
  const lost = () => {
    if (Atomics.compareExchange(ctrl, STATE, OPEN, CLOSED) !== OPEN)
      Atomics.compareExchange(ctrl, STATE, CONNECTING, CLOSED);
    wakeReader();
  };

  // The game's datagrams into the WebSocket (while it is still connecting they wait in the ring).
  const drain = () => {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    let r = Atomics.load(ctrl, OUT_READ);
    const w = Atomics.load(ctrl, OUT_WRITE);
    for (; r !== w; r++) {
      const at = outBase + (r % slots) * slotSize;
      const n = view.getInt32(at, true);
      if (n > 0 && n <= slotBytes) ws.send(bytes.slice(at + 4, at + 4 + n)); // (a copy: send takes no shared memory)
    }
    Atomics.store(ctrl, OUT_READ, r);
  };
  // Woken by the game's Atomics.notify on OUT_WRITE (emscripten_futex_wake), or once a second.
  const watch = () => {
    for (;;) {
      if (finished) return;
      if (Atomics.load(ctrl, STATE) === CLOSING) return finish();
      drain();
      const w = Atomics.load(ctrl, OUT_WRITE);
      if (ws && ws.readyState === WebSocket.OPEN && Atomics.load(ctrl, OUT_READ) !== w) continue;
      if (!Atomics.waitAsync) return void setTimeout(watch, 4);
      const wait = Atomics.waitAsync(ctrl, OUT_WRITE, w, 1000);
      if (wait.async) return void wait.value.then(watch);
    }
  };

  try {
    ws = new WebSocket(url);
  } catch (err) {
    lost();
    watch();
    return;
  }
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => {
    Atomics.compareExchange(ctrl, STATE, CONNECTING, OPEN);
    drain();
  };
  ws.onmessage = (ev) => {
    if (!(ev.data instanceof ArrayBuffer)) return;
    const n = ev.data.byteLength;
    if (n > slotBytes) return;
    const w = Atomics.load(ctrl, IN_WRITE);
    if (w - Atomics.load(ctrl, IN_READ) >= slots) return; // full: dropped, as UDP would
    const at = inBase + (w % slots) * slotSize;
    view.setInt32(at, n, true);
    bytes.set(new Uint8Array(ev.data), at + 4);
    Atomics.store(ctrl, IN_WRITE, w + 1);
    wakeReader();
  };
  ws.onerror = lost;
  ws.onclose = lost;
  watch();
};
