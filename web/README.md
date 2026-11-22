# GT3 Revisited in a browser

The game built with Emscripten (WebAssembly, WebGL2, threads on Web Workers) and the page that
starts it. Chrome and Edge are tested. No game data is included: the player picks a disc image
(`.iso` / `.chd`) made from their own disc, and the page reads it in the browser, where it is; it
is not uploaded anywhere. The page remembers the picked file for the next visit (the File System
Access API); saves and settings stay in the browser's IndexedDB.

## Build

With [emsdk](https://github.com/emscripten-core/emsdk) (tested with 7.0.0) installed and activated:

```bash
source emsdk/emsdk_env.sh
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-web --target gt3r gt3r_tests
```

`cmake/Emscripten.cmake` holds the web settings. The tests run under Node.js:

```bash
GT3R_SOURCE_DIR_MOUNT=$PWD node build-web/gt3r_tests.js
```

(The four racing wheel input tests fail in a browser build; the rest pass.)

## Serve

The engine's threads need `SharedArrayBuffer`, so the page must be served with the
`Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers. `web/serve.mjs` (Node.js 18
or later, no dependencies) does that on localhost, which browsers treat as a secure origin:

```bash
mkdir -p dist
cp web/index.html web/start.js web/data-browser.js web/debug.js web/disc-bridge.js web/disc-worker.js web/coi-sw.js dist/
cp build-web/gt3r.js build-web/gt3r.wasm dist/
cp LICENSE dist/LICENSE.txt && cp THIRD-PARTY-NOTICES.md dist/THIRD-PARTY-NOTICES.txt
node web/serve.mjs dist 8080
```

Then open <http://localhost:8080/> and pick the disc image. Any other web server works if it sends
the same headers.

On a host that cannot send headers, such as GitHub Pages, `coi-sw.js` adds them: `index.html` registers
this service worker when the page is not cross-origin isolated and reloads once under it. Served with the
headers, the page does not register it.

`web/serve.zig` is the same server as a small executable without dependencies (about 50 to 260 KB),
for players: placed next to `index.html` and started, it serves its own folder on the first free port
from 8080, on this computer only, and opens the browser there (`serve [folder] [--port N] [--no-open]`).
It builds with [Zig](https://ziglang.org/) 0.15 for any of its targets from one machine:

```bash
zig build-exe web/serve.zig -O ReleaseSmall -fstrip -target x86_64-windows   # or x86_64-macos, aarch64-macos, x86_64-linux-musl
```

## Files

| File | |
|---|---|
| `index.html`, `start.js` | the start page: pick, remember or drop a disc image, then load `gt3r.js` |
| `data-browser.js` | the start page's Data: the files the game keeps in the browser (saves, replays, screenshots, music, mods), to download, upload, rename or delete |
| `disc-bridge.js`, `disc-worker.js` | the disc image read in blocks by a worker, for the engine's synchronous reads |
| `coi-sw.js` | the service worker that adds the two headers where the server cannot (GitHub Pages) |
| `debug.js` | a debug overlay (F9): frame times, long frames, the 3D settings |
| `serve.mjs` | a small static server with the headers above (Node.js) |
| `serve.zig` | the same as a small executable for players (Zig) |
