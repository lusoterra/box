# booru-explorer

Booru client for local archives and online boorus, as a native C++/Qt6 app.
Point it at a folder of downloaded posts, browse Danbooru/Gelbooru and more
online, download in full quality, and keep favourites — no browser needed.

Local archives are treated read-only: browsing never modifies your library.

## Features

- **Library tabs**: recursive scan with per-bucket tag counts, include/exclude
  tag search (`-foo`), rating and source filters, six sort orders,
  color-coded tag autocomplete, removable filter pills, drag-and-drop files
  out to other applications.
- **Online tabs**: configurable booru list (preset engines + your own
  instances), multi-booru merged search with infinite scroll, site badges,
  safe-search defaults, per-engine tag autocomplete. Downloads save full
  quality plus the raw API sidecar, so they browse offline on rescan.
- **Favourites**: database entries (no files) that open from disk whenever
  the file is already downloaded. Select + mass-download, right-click
  actions (favourite, download, delete download/file), optional
  auto-download / auto-favourite / unfavourite-deletes-file. ♥ Favourites
  and ⭳ Downloads join multibooru merges as virtual sources.
- **Viewer** (same window, no popups): wheel-zoom/drag-pan stills, GIFs,
  looping video with seek/volume, clickable tag panel, notes + comments on
  remote posts, refresh-tags, download, favourite.
- **Tag classification**: untyped tags (Gelbooru/LOSN blobs) resolved via
  Gelbooru (with API key), anonymous Danbooru, Safebooru, then yandere —
  verdicts cached forever, manual overrides win, explicit-tag fallback.
- **App**: tabbed preferences (General/Library/Browse/Tags/Appearance),
  session restore, paranoia mode (memory-only images, no downloads),
  custom `.qss` or native theme, smooth scrolling, translations.

## Requirements

- C++17 compiler (GCC, Clang, MSVC)
- CMake 3.16+
- Qt 6: Widgets, Concurrent, Sql, Multimedia, MultimediaWidgets, Network

```sh
# Arch
sudo pacman -S qt6-base qt6-multimedia qt6-svg cmake gcc

# Debian / Ubuntu
sudo apt install qt6-base-dev qt6-multimedia-dev libqt6svg6-dev cmake g++

# Fedora
sudo dnf install qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qtsvg-devel cmake gcc-c++
```

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"   # -> build/booru-explorer
cmake --install build --prefix ~/.local
make test   # or: ctest --test-dir build --output-on-failure
```

Debug builds: `-DCMAKE_BUILD_TYPE=Debug`. `make install` / `make uninstall` /
`make clean` wrap the obvious CMake steps.

## Run

```sh
./build/booru-explorer /path/to/dump
./build/booru-explorer --help        # [--stylesheet file.qss] [--native-theme]
make run ARGS=~/pics/dump
```

No folder argument opens the welcome tab. `File → Open → Folder…` opens
archives as tabs; `Tabs` holds Close Folder, ♥ Favourites and Reload (`↺`
rescans); `Edit → Preferences…` (`Ctrl+,`) edits everything;
`Help → About` shows version and license. `＋` opens the new-tab picker
(Home, Favourites, Downloads, one booru each, or a Multibooru merge).

Options live in `~/.config/booru-explorer/settings.ini`. Data
(`store.db` favourites, `tag_types.db` verdicts, `downloads/`) lives under
`~/.local/share/booru-explorer/`; caches (scan index, thumbnails,
opt-in full images, `cache.db` tracking, 10 GB cap) under the platform
cache dir (`~/.cache/booru-explorer/`). A second
copy exits instead of risking the database.

Startup tab is configurable (welcome, last session, or any booru).
Grid: arrows move, Enter opens, `/` searches, Esc clears, F5 rescans,
right-click a post for actions.

## Local archives

`File → Open → Folder…` (or pass the folder on the command line) scans
recursively and stays read-only — the app never writes into your library.
Tags, search, and the viewer sidebar come from JSON sidecars next to the
media; files without one still browse, just untagged:

- **gallery-dl**: `<mediafile>.json` (`photo.jpg` + `photo.jpg.json`) —
  native Danbooru / Gelbooru / Szurubooru objects or the flat layout.
- **LoliSnatcher**: a folder of saved files, each with its `<stem>.json`
  metadata sibling carrying `postURL`/`fileURL` and the `tags` array
  (`Danbooru_<md5>.jpg` + `Danbooru_<md5>.json`). A `store.db` backup
  instead goes through `File → Import LoliSnatcher backup…`.

Build an archive with gallery-dl so the sidecars come along (verified
against gallery-dl 1.32):

```sh
gallery-dl --write-metadata -D ~/pics/safebooru \
  "https://safebooru.org/index.php?page=post&s=list&tags=hatsune_miku"
```

`--write-metadata` writes the per-file sidecar the scanner reads — without
it you get untagged files. Re-run the same command later for new posts;
add `--download-archive ~/pics/archive.sqlite3` to skip what you already
have.

Downloads (`<Site>_<md5>.<ext>` + pretty-printed JSON sidecar) feed the
same pipeline: rescanning the download folder browses them offline.
