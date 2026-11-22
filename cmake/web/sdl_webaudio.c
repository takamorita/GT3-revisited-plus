/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/*
  ALTERED for GT3 Revisited's web build (cmake/Emscripten.cmake builds this file in place of SDL's
  src/audio/emscripten/SDL_emscriptenaudio.c). Playback only.

  SDL's Emscripten driver plays through a ScriptProcessorNode, whose callback (and so all of SDL's
  mixing and the app's stream callbacks) runs on the browser's main thread: whenever the main
  thread is busy (a load, a long frame) the sound stops or repeats its last buffer. Here SDL runs
  its usual audio thread (a pthread, as on the other platforms): it mixes into a ring in the wasm
  memory (shared memory: the build uses -pthread), and an AudioWorklet, on the browser's audio
  thread, plays from that ring. The main thread is not on the sound's path.

  - The audio thread keeps up to `target` frames queued (ctrl[CTRL_TARGET], ~43 ms) and sleeps
    while the ring is fuller.
  - The worklet counts its calls (ctrl[CTRL_BEAT]). When they stop for 200 ms (the context is
    suspended before a user gesture, the module is still loading, the tab's audio is suspended)
    the audio thread throws its frames away at the real-time rate, as SDL's dummy driver does, so
    the app's streams keep moving.
  - On an underrun the worklet holds the last sample and fades it out in 4 ms, and fades back in
    when frames come again (no click).
  - The ring is allocated once and kept (there is one playback device): a worklet that is still
    running after a close never reads freed memory.
*/
#include "SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_EMSCRIPTEN

#include "audio/SDL_sysaudio.h"

#include <emscripten/emscripten.h>

/* *INDENT-OFF* */ // clang-format off

enum {
    CTRL_WRITE,     // frames written by the audio thread (wraps)
    CTRL_READ,      // frames played by the worklet (wraps)
    CTRL_BEAT,      // worklet calls
    CTRL_UNDERRUNS, // worklet calls that found fewer frames than they needed, while playing
    CTRL_TARGET,    // frames the audio thread keeps queued at most
    CTRL_FRAMES,    // frames per device buffer (for the page's overlay)
    CTRL_DROPPING,  // 1 while nothing plays the ring (frames are thrown away)
    CTRL_COUNT = 16
};

#define RING_FRAMES 8192 // a power of two; 170 ms at 48 kHz
#define MAX_CHANNELS 8
#define DEVICE_FRAMES 256
#define TARGET_FRAMES 2048
#define STALL_MS 200

static Sint32 *ring_ctrl;  // CTRL_COUNT ints, then the ring (RING_FRAMES * MAX_CHANNELS floats)
static float *ring_data;

struct SDL_PrivateAudioData
{
    Uint8 *mixbuf;
    Sint32 last_beat;
    Uint64 beat_ticks; // when last_beat last changed
};

static Sint32 load_ctrl(int i) { return __atomic_load_n(&ring_ctrl[i], __ATOMIC_ACQUIRE); }
static void store_ctrl(int i, Sint32 v) { __atomic_store_n(&ring_ctrl[i], v, __ATOMIC_RELEASE); }

static Uint8 *WEBAUDIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    return device->hidden->mixbuf;
}

static bool WEBAUDIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buffer_size)
{
    const int ch = device->spec.channels;
    const Uint32 frames = (Uint32)(buffer_size / SDL_AUDIO_FRAMESIZE(device->spec));
    const Uint32 w = (Uint32)load_ctrl(CTRL_WRITE), r = (Uint32)load_ctrl(CTRL_READ);
    if (w - r > RING_FRAMES - frames) {
        return true; // full (nothing is playing it), or the read position past the written one (WaitDevice): drop
    }
    const float *src = (const float *)buffer;
    const Uint32 start = w & (RING_FRAMES - 1);
    const Uint32 first = SDL_min(frames, RING_FRAMES - start);
    SDL_memcpy(ring_data + (size_t)start * ch, src, (size_t)first * ch * sizeof(float));
    SDL_memcpy(ring_data, src + (size_t)first * ch, (size_t)(frames - first) * ch * sizeof(float));
    store_ctrl(CTRL_WRITE, (Sint32)(w + frames));
    return true;
}

static bool WEBAUDIO_WaitDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *h = device->hidden;
    const int freq = device->spec.freq;
    while (!SDL_GetAtomicInt(&device->shutdown)) {
        const Uint64 now = SDL_GetTicks();
        const Sint32 beat = load_ctrl(CTRL_BEAT);
        if (beat != h->last_beat) {
            h->last_beat = beat;
            h->beat_ticks = now;
        }
        if (now - h->beat_ticks > STALL_MS) {
            // Nothing plays the ring: throw the frames away at the real-time rate.
            store_ctrl(CTRL_DROPPING, 1);
            SDL_Delay((Uint32)SDL_max(1, device->sample_frames * 1000 / freq));
            store_ctrl(CTRL_READ, load_ctrl(CTRL_WRITE));
            return true;
        }
        store_ctrl(CTRL_DROPPING, 0);
        Uint32 queued = (Uint32)load_ctrl(CTRL_WRITE) - (Uint32)load_ctrl(CTRL_READ);
        if (queued > RING_FRAMES) {
            // The read position past the written one: the worklet of a device closed meanwhile played once more
            // after OpenDevice set both to 0. Waiting for that to drain waited for ever (no sound): start again.
            store_ctrl(CTRL_READ, load_ctrl(CTRL_WRITE));
            queued = 0;
        }
        const Uint32 target = (Uint32)SDL_clamp(load_ctrl(CTRL_TARGET), device->sample_frames, RING_FRAMES - device->sample_frames);
        if (queued + (Uint32)device->sample_frames <= target) {
            return true;
        }
        const Uint32 over = queued + (Uint32)device->sample_frames - target;
        SDL_Delay(SDL_max(1u, over * 1000u / (Uint32)freq));
    }
    return true;
}

static void WEBAUDIO_CloseDevice(SDL_AudioDevice *device)
{
    if (!device->hidden) {
        return;
    }
    MAIN_THREAD_EM_ASM({
        var SDL3 = Module['SDL3'];
        var st = SDL3.webaudio;
        SDL3.webaudio = undefined;
        if (st && st.node) {
            st.node.port.postMessage('stop');
            st.node.disconnect();
        }
    });
    SDL_free(device->hidden->mixbuf);
    SDL_free(device->hidden);
    device->hidden = NULL;
}

// (No comments inside the EM_ASM blocks: an apostrophe in one breaks the macro; nor a comma outside parentheses. The page's debug overlay
// reads Module.gt3rWebAudio.ctrl; a node made for a device closed or reopened meanwhile is not started.)
// $dynCall: SDL's dummy driver (the tests') calls it from JS; SDL's own driver used to pull it in.
EM_JS_DEPS(sdlwebaudio, "$autoResumeAudioContext,$UTF8ToString,$dynCall");

// The AudioWorklet that plays the ring (ctrl indices as in the enum above).
static const char worklet_source[] =
    "class SdlRing extends AudioWorkletProcessor {\n"
    "  constructor(o) {\n"
    "    super();\n"
    "    var p = o.processorOptions;\n"
    "    this.c = new Int32Array(p.buffer, p.ctrl, 16);\n"
    "    this.r = new Float32Array(p.buffer, p.ring, p.frames * p.channels);\n"
    "    this.ch = p.channels; this.mask = p.frames - 1;\n"
    "    this.g = 0; this.step = 1 / (0.004 * sampleRate);\n"
    "    this.last = new Float32Array(p.channels);\n"
    "    this.on = true;\n"
    "    this.port.onmessage = (e) => { if (e.data === 'stop') this.on = false; };\n"
    "  }\n"
    "  process(inputs, outputs) {\n"
    "    if (!this.on) return false;\n"
    "    var c = this.c, out = outputs[0], len = out[0].length, ch = this.ch;\n"
    "    var nout = Math.min(out.length, ch);\n"
    "    Atomics.add(c, 2, 1);\n"
    "    var r = Atomics.load(c, 1), avail = (Atomics.load(c, 0) - r) | 0;\n"
    "    if (avail < 0 || avail > this.mask + 1) { r = Atomics.load(c, 0); Atomics.store(c, 1, r); avail = 0; }\n"
    "    var n = Math.max(0, Math.min(avail, len));\n"
    "    if (n < len && this.g > 0) Atomics.add(c, 3, 1);\n"
    "    for (var i = 0; i < len; i++) {\n"
    "      if (i < n) {\n"
    "        var base = ((r + i) & this.mask) * ch;\n"
    "        for (var k = 0; k < ch; k++) this.last[k] = this.r[base + k];\n"
    "        this.g = Math.min(1, this.g + this.step);\n"
    "      } else {\n"
    "        this.g = Math.max(0, this.g - this.step);\n"
    "      }\n"
    "      for (var k = 0; k < nout; k++) out[k][i] = this.last[k] * this.g;\n"
    "    }\n"
    "    Atomics.store(c, 1, (r + n) | 0);\n"
    "    return true;\n"
    "  }\n"
    "}\n"
    "registerProcessor('sdl-webaudio-ring', SdlRing);\n";

static bool WEBAUDIO_OpenDevice(SDL_AudioDevice *device)
{
    if (device->recording) {
        return SDL_SetError("No audio recording in this build");
    }
    if (!ring_ctrl) {
        const size_t bytes = CTRL_COUNT * sizeof(Sint32) + (size_t)RING_FRAMES * MAX_CHANNELS * sizeof(float);
        ring_ctrl = (Sint32 *)SDL_aligned_alloc(64, bytes);
        if (!ring_ctrl) {
            return false;
        }
        SDL_memset(ring_ctrl, 0, bytes);
        ring_data = (float *)(ring_ctrl + CTRL_COUNT);
    }

    const int freq = MAIN_THREAD_EM_ASM_INT({
        var SDL3 = Module['SDL3'];
        if (!SDL3.audioContext) {
            SDL3.audioContext = new AudioContext();
            if ((typeof navigator.userActivation) === 'undefined') {
                autoResumeAudioContext(SDL3.audioContext);
            } else {
                var ctx = SDL3.audioContext;
                var poll = setInterval(function() {
                    if (ctx.state === 'closed') { clearInterval(poll); return; }
                    if (ctx.state === 'suspended' && navigator.userActivation.hasBeenActive) ctx.resume();
                }, 250);
            }
        }
        return SDL3.audioContext.sampleRate;
    });
    if (freq <= 0) {
        return SDL_SetError("Web Audio API is not available!");
    }

    device->hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*device->hidden));
    if (!device->hidden) {
        return false;
    }
    device->spec.format = SDL_AUDIO_F32; // web audio only supports floats
    device->spec.freq = freq;
    device->spec.channels = SDL_clamp(device->spec.channels, 1, MAX_CHANNELS);
    device->sample_frames = DEVICE_FRAMES;
    SDL_UpdatedAudioDeviceFormat(device);

    device->hidden->mixbuf = (Uint8 *)SDL_malloc(device->buffer_size);
    if (!device->hidden->mixbuf) {
        return false;
    }
    SDL_memset(device->hidden->mixbuf, device->silence_value, device->buffer_size);
    device->hidden->beat_ticks = SDL_GetTicks();

    store_ctrl(CTRL_WRITE, 0);
    store_ctrl(CTRL_READ, 0);
    store_ctrl(CTRL_TARGET, TARGET_FRAMES);
    store_ctrl(CTRL_FRAMES, DEVICE_FRAMES);
    store_ctrl(CTRL_DROPPING, 0);

    MAIN_THREAD_EM_ASM({
        var SDL3 = Module['SDL3'];
        var ctx = SDL3.audioContext;
        if (SDL3.webaudio && SDL3.webaudio.node) {
            SDL3.webaudio.node.port.postMessage('stop');
            SDL3.webaudio.node.disconnect();
        }
        var st = {};
        st.node = null;
        st.ctrl = new Int32Array(HEAPF32.buffer, $0, 16);
        SDL3.webaudio = st;
        Module.gt3rWebAudio = st;
        if (!SDL3.webaudioModule) {
            var src = UTF8ToString($4);
            SDL3.webaudioModule = ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([src], { type: 'application/javascript' })));
        }
        SDL3.webaudioModule.then(function() {
            if (SDL3.webaudio !== st) return;
            st.node = new AudioWorkletNode(ctx, 'sdl-webaudio-ring', {
                numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [$2],
                processorOptions: { buffer: HEAPF32.buffer, ctrl: $0, ring: $1, channels: $2, frames: $3 }
            });
            st.node.connect(ctx.destination);
        }).catch(function(e) { console.error('SDL web audio: the worklet did not load', e); });
    }, ring_ctrl, ring_data, device->spec.channels, RING_FRAMES, worklet_source);

    return true;
}

static bool WEBAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    const bool available = MAIN_THREAD_EM_ASM_INT({
        return (typeof(AudioContext) !== 'undefined') && (typeof(AudioWorkletNode) !== 'undefined') &&
               (typeof(SharedArrayBuffer) !== 'undefined') && (HEAPF32.buffer instanceof SharedArrayBuffer);
    });
    if (!available) {
        SDL_SetError("No audio context with AudioWorklet over shared memory available");
        return false;
    }

    impl->OpenDevice = WEBAUDIO_OpenDevice;
    impl->CloseDevice = WEBAUDIO_CloseDevice;
    impl->GetDeviceBuf = WEBAUDIO_GetDeviceBuf;
    impl->PlayDevice = WEBAUDIO_PlayDevice;
    impl->WaitDevice = WEBAUDIO_WaitDevice;
    impl->OnlyHasDefaultPlaybackDevice = true;
    impl->HasRecordingSupport = false;
    impl->ProvidesOwnCallbackThread = false; // SDL's audio thread (a pthread) mixes
    return true;
}

AudioBootStrap EMSCRIPTENAUDIO_bootstrap = {
    "emscripten", "SDL emscripten audio driver (AudioWorklet)", WEBAUDIO_Init, false, false
};

/* *INDENT-ON* */ // clang-format on

#endif // SDL_AUDIO_DRIVER_EMSCRIPTEN
