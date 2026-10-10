# booru-explorer

Read-only local booru viewer as a native C++/Qt6 application. Point it at a
folder of downloaded booru posts and browse them with tag search, rating and
source filters, and a same-window viewer — no browser, no local HTTP server,
no runtime dependencies beyond Qt.

This is a from-scratch C++ port of the old Python prototypes (`localbooru.py`
gallery-dl server, `losn-explorer.py` LOSN reader). Those scripts have been
removed; all of their logic lives here now: both metadata formats, the
rating normalization, the explicit-tag fallback, and the query/suggest/stats
behavior.

## Features

- **Library**: recursive scan with per-bucket tag counts, source categories,
  include/exclude tag search (`-foo`), rating and source filters, sort orders
  (newest, oldest, top/low rated, random), tag autocomplete, result stats.
- **Speed**: SQLite scan cache (only new/changed files are re-parsed),
  determinate scan progress bar, thread-pool thumbnails, persistent thumbnail
  disk cache, huge stills downscaled at decode time.
- **Grid**: one omnibar (search + rating + source + sort + thumbnail ratio),
  removable filter pills, uniform crop-to-fill thumbnails (1:1, 4:3, 3:2,
  16:9, 3:4, custom), one info card per entry (`[G]`/`[S]`/`[Q]`/`[E]` chip +
  score + `#id`), drag-and-drop files out to other applications.
- **Sidebar**: the one tag menu in real booru order — Copyright, Characters,
  Artists, Tags, Meta. Drag dividers to resize sections; sizes are
  remembered across launches.
- **Viewer** (same window, no popups): wheel-zoom/drag-pan stills, animated
  GIFs, muted-by-default looping video with seek/volume, clickable tag panel,
  `Open post ↗` for the source post page.
- **Safety**: strictly read-only. Under-tagged posts whose tags match the
  explicit list are shown as explicit in memory only — source files are never
  modified. The viewer reports how many results were auto-flagged.

## Requirements

- C++17 compiler (GCC, Clang, MSVC)
- CMake 3.16+
- Qt 6: Widgets, Concurrent, Sql, Multimedia, MultimediaWidgets

Install on common distros:

```sh
# Arch
sudo pacman -S qt6-base qt6-multimedia cmake gcc

# Debian / Ubuntu
sudo apt install qt6-base-dev qt6-multimedia-dev cmake g++

# Fedora
sudo dnf install qt6-qtbase-devel qt6-qtmultimedia-devel cmake gcc-c++
```

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The binary lands at `build/booru-explorer`. Optional install:

```sh
cmake --install build --prefix ~/.local   # -> ~/.local/bin/booru-explorer
```

For a debug build, use `-DCMAKE_BUILD_TYPE=Debug`.

## Run

```sh
./build/booru-explorer /path/to/dump
./build/booru-explorer --help
```

With no folder argument you get a welcome page: resume the last session,
pick from recent archives, or open a folder. `Always load last session`
skips it (toggleable anytime in `Edit → Preferences → General`).

```
booru-explorer [root] [--stylesheet file.qss] [--native-theme]
```

- `File → Open Folder…` / `File → Close Folder` switches archives or goes
  back to welcome. `↺` rescans.
- Options persist to `~/.config/booru-explorer/settings.ini`
  (`Edit → Preferences…`): session restore + archive history, index/
  thumbnail caches + thumbnail size, tag classification toggle, theme
  mode + accent color + custom `.qss`, thumbnail ratio, explicit-tag list,
  default mute/loop/volume, huge-image downscale cap, window geometry, and
  sidebar section sizes.

```
make run ARGS=~/pics/dump
```

## Install

```sh
make install            # -> /usr/local (needs root)
sudo make install
make install PREFIX=~/.local   # user-local, no root
```

Installs the binary, the `.desktop` launcher (opens folders from your file
manager), and the icon. `make uninstall` removes them via the install
manifest; `make clean` drops the build tree.

## Arch package

`packaging/PKGBUILD` builds a proper pacman package from the working tree
(temp file — lift it when the project gets its own repo):

```sh
cd packaging && makepkg -sf   # -s pulls deps with pacman, needs root once
sudo pacman -U booru-explorer-*.pkg.tar.zst
```

`pkgver` tracks `CMakeLists.txt`. Before AUR upload, set the real `url=`,
pick a license (name it in `license=` and ship the file), and point
`source=` at the new repo instead of the local tree.
- `--stylesheet file.qss` layers a custom Qt stylesheet over the built-in
  theme (also via `File → Load Custom Stylesheet…`).
- `--native-theme` uses the desktop environment's Qt theme instead of the
  built-in dark one.

Caches live under the platform cache dir (`~/.cache/booru-explorer` on
Linux): per-root SQLite scan indexes in `roots/`, thumbnails in `thumbs/`.
Deleting that folder just forces a full rescan/re-decode.

## Metadata formats

Both layouts are detected side by side (read-only):

- **gallery-dl**: `<mediafile>.json` next to the media
  (`image.jpg.json`), from `gallery-dl --write-metadata --write-info-json`:
  - old flat layout (`id`, `score`, `rating`, `tags[_artist|_character|…]`,
    `width`/`height`, `source`, `date`, `title`, `category`)
  - native danbooru info.json (`tag_string_artist|character|copyright|…`,
    single-letter rating, `created_at`)
  - native gelbooru info.json (`tags` string, word rating)
- **LOSN**: `<stem>.json` sibling (`Danbooru_<md5>.jpg` +
  `Danbooru_<md5>.json`) with `serverId`, string `score`, letter rating
  (`g`/`s`/`q`/`e`), `tags: [{fullString, tagType}]`, `sources`, `postDate`,
  `postURL` (wired to the `Open post ↗` button).

Files without usable metadata still show up, rated `general` with no tags.
Flat folders without subdirectories get their source category from the
filename prefix (`Danbooru_…` → `Danbooru`).

## Tag classification

LOSN/Gelbooru sidecars carry untyped tags, so everything lands in General.
With `Edit → Preferences → Library → Classify untyped tags online`, the app
resolves each unknown tag's category (artist/copyright/character/meta) and
re-buckets it — in the sidebar, the viewer, search, and stats.

How it works: untyped tags are looked up against Gelbooru first when you
set an API key + user ID in Preferences (both required, with a Test button),
else against the public Safebooru tag API, falling back to yandere, with no
API key. Verdicts — including misses — persist in the per-root index DB, so
each tag is fetched once ever; transient network failures and rate limits
(429s are retried with backoff) show in the status bar and retry on the next
scan, and the whole pass can be toggled off (tags stay general). Safebooru
is SFW-filtered, so its verdicts can differ from Danbooru's (e.g. an artist
it files as general); yandere covers most of those gaps.

## Explicit-tag fallback

Many archives store explicit posts as `general`. Any non-explicit post whose
tags contain an entry from the explicit list is upgraded to `explicit` in
memory and marked `(inferred)` in the viewer; the result line counts
auto-flagged posts. The list lives in `Edit → Preferences → Explicit
detection` (one tag per line, add whatever you need) and persists to
`settings.ini`; the hardcoded defaults in `src/explicittags.h` apply until
you change it. Editing the list automatically invalidates cached ratings on
the next scan. `breasts`/`nipples`/`areola` are deliberately excluded from
the defaults — they appear on SFW posts too.

## Controls

Grid: arrows move, Enter opens, `/` focuses search, Esc clears filters,
F5 rescans. Drag thumbnails out to other apps.

Viewer: `←`/`→` always switch pictures, mouse Back/Forward buttons too,
Esc/Backspace or `‹ Back` returns to the grid. Stills: wheel zooms at the
cursor, drag pans, double-click toggles fit/100%. GIFs: wheel zooms, drag
pans. Video: click seeks via the slider.

## Theme

Like KeePassXC, the app pins the `Fusion` style and ships a built-in dark
palette — it ignores the desktop environment unless you pass
`--native-theme`. Custom Qt themes are welcome via `--stylesheet`.

## Layout

```
CMakeLists.txt       (cmake build + system install rules)
Makefile             (configure/build/install/uninstall wrapper)
packaging/booru-explorer.desktop
icon.png             (placeholder app icon, 512px)
src/
  main.cpp         CLI, theme bootstrap, window icon
  post.h           Post record
  explicittags.h   explicit-tag set + rating helpers
  metaparser.*     gallery-dl (flat/native) + LOSN parsing
  scanner.*        recursive scan, sidecar probing
  cache.*          SQLite scan cache
  index.*          tag counts, query, suggest, stats
  settings.*       ~/.config/booru-explorer/settings.ini
  thumbloader.*    pool thumbnails, GUI-thread video frames, disk cache
  thumbcommon.h    crop/scale/placeholder helpers
  flowlayout.*     wrapping layout (tag panel, filter pills)
  theme.*          Fusion + dark palette + accent + custom .qss
  viewerpage.*     same-window viewer (zoom/pan, gif, video)
  welcomepage.*    welcome screen + archive history
  prefsdlg.*       preferences dialog
  mainwindow.*     omnibar, sidebar, grid + delegate, drag-out
  icons.qrc        embedded app icon
testfiles/        sample LOSN dump used for testing
```
