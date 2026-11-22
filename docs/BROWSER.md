# Playing in a browser

This fork's browser build runs GT3 Revisited in Chrome or Edge on a computer, from a web page.

## What you need

- Chrome or Edge on a computer, with WebGL2.
- A disc image (`.iso` or `.chd`) that you made from your own copy of the game.

## Starting

1. Open <https://takamorita.github.io/GT3-revisited-plus/>. The first visit reloads the page once (see
   below).
2. Choose your disc image (**Choose a disc image…**), or drop it on the page: the game starts.

The image is read in the browser, where it is: it is not uploaded anywhere. The page remembers the file for the
next visit, where **Start** opens it again; the browser may ask you to allow it again.

The engine's threads use `SharedArrayBuffer`, which a browser gives only to a page served with two headers.
GitHub Pages cannot send them, so the page installs a small service worker (`coi-sw.js`) that adds them, and
reloads once under it.

### On your own computer

The same page runs from a folder on your computer, without the internet once it is there:

1. Unzip `gt3r-web.zip` from this fork's [Releases](https://github.com/takamorita/GT3-revisited-plus/releases)
   into a folder of your choice.
2. Start the server for your computer in that folder: `serve-windows-x64.exe`, `serve-macos-arm64`,
   `serve-macos-x64`, `serve-linux-x64` or `serve-linux-arm64`. It opens the browser at
   `http://localhost:8080/` (or the next free port); only this computer can connect. Close its window, or press
   Ctrl+C in it, to stop it.

Any other web server works too, with the two headers or without them (then the service worker adds them). See
[web/README.md](../web/README.md).

## Saves and settings

Saves and settings are kept in the browser's storage for the page's site: the GitHub Pages page, your own
`http://localhost:8080/` and `http://localhost:8081/` each have their own. Clearing that site's data deletes them.

To move saves in or out, open **Data** on the start page. It lists the files the game keeps (saves, replays,
screenshots, music, mods): **Download** takes a file out, **Upload** puts files into the folder shown. GT Mode saves
are in `saves/`, in a folder per region (`saves/jp/` for the Japanese disc), and are the same files as on the other
platforms: a save copied from another platform's `saves/jp/` continues in the browser.

The game's own menus work too: **FROM A FILE** (Load game, File Operation, Trade) opens the browser's file chooser,
and **Export to PS2** downloads the `.psu`, `.ps2` or `.raw` file. Camera files and the Event creator's `.zip`
import and export the same way.

A folder on the computer cannot be chosen (Settings → Audio → Music folder, and the save folder): put your songs
into `music/` with **Data** instead.

## Controls

The keyboard and gamepads, as in GT3 Revisited. Click the page once so that it receives the keys; press a button
on a gamepad so that the browser lets the page see it.

## Not tested yet

- Racing wheels and force feedback.

## Online battle

A browser has no UDP, so the browser build races online through a relay server that also takes WebSockets.
The usual relay server (`gt3r.gameversions.com`) does not: run your own `tools/gt3r-relay` (Linux or macOS;
"Running your own relay" in [dev/ONLINE.md](dev/ONLINE.md)) with a port for browsers:

```sh
gt3r-relay --ws-port 47752 --ws-allow-lan
```

- In the browser, set the relay server (in the Online battle lobby) to `ws://<the relay's address>:47752`.
  Players on other platforms set the same relay at its UDP port (`<the relay's address>:47631`), and everyone
  creates or joins sessions by code as usual. To the others a browser is an ordinary player at the relay's address.
- A page served over https (the copy on GitHub Pages) can open only `wss://`: put a TLS proxy (Caddy, nginx, ...)
  in front of the WebSocket port. The page from the bundled `serve` (http://localhost) can use `ws://`.
- In a browser there is no Host by address; Join by address goes through the relay (`--ws-allow-lan` lets it
  reach addresses in your LAN).

## Speed

Keep the window in front: browsers slow a tab in the background down to a few frames a second. The dynamic
resolution lowers the 3D resolution when the GPU cannot keep up.
