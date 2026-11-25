# Box Share web UI

Single-page Svelte frontend for the Box read-only LAN API. It is meant to be
embedded into the Box binary as one static `index.html` (Svelte + Vite +
`vite-plugin-singlefile`), but it also runs standalone against any
Danbooru-compatible Box share server.

## Development

```sh
npm ci
npm run dev
```

Vite serves the app on `http://localhost:5173`. In development the app defaults
its API base to `http://127.0.0.1:7071`; use the settings panel to point it at
another Box instance. The server must allow CORS.

## Build

```sh
npm run build
```

`dist/index.html` is the single resource the box CMake build embeds. The
frontend does not run on Node at runtime.

The default theme is Box dark. The theme picker mirrors the desktop themes in
`assets/themes/*.ini` (Box, KeePassXC, Light, Nord, Catppuccin, Dracula, Atom
One Dark, Tokyo Night, Steam Classic), plus System for the browser/OS
preference. Booru Explorer's box mark and wordmark are used in the header,
settings dialog, footer and favicon.

The grid follows Box's layout: uniform centre-cropped thumbnails, the same
thumbnail-ratio list, a card-size slider, and a rating/score/id strip plus
filename under each card. Video playback defaults (mute, loop, volume) mirror
Box's Preferences -> Appearance -> Video playback.

## Check

```sh
npm run check
```

## API used

- `GET /posts.json?tags=...&page=N&limit=N`
- `GET /posts/<id>.json`
- `GET /tags.json?search[name_matches]=prefix*&search[order]=count&limit=N`
- `GET /counts/posts.json?tags=...`
- `GET` media/thumbnail URLs from the post fields (`file_url`,
  `preview_file_url`)

The app is read-only. It has no upload, favourite, or account features.

## Server address

When Box serves this page at `http://<box-ip>:7071/`, the API is same-origin:
open the Box LAN address on the phone or another computer, not `localhost`.
The page uses `window.location.origin`, so it automatically talks to the same
host and port it was loaded from.

A separately hosted copy can point at another server via the settings panel, or
once through the address bar:

```text
/?api=https://danbooru.donmai.us
/?server=http://192.168.1.50:7071
```

The override is saved in the `box_share_api` cookie and in localStorage, then
removed from the address bar. The target server must send CORS headers.
