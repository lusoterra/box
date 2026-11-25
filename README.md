# Box (BOoru eXplorer)

Box (**Bo**oru e**x**plorer) is a Booru client for online boorus and local
*gallery-dl* and similar archives, as a native C++/Qt6 app. Point it at a folder
of downloaded posts, browse Danbooru/Gelbooru and more online, download in full
quality, and keep favourites.

![env](docs/img/environment.jxl)

## Supported boorus

Box talks each site's native API. Any instance of a supported engine can be
added in Preferences → Booru manager (base URL + engine type); only
Safebooru ships preconfigured, so a fresh install starts SFW-only.

<details>
<summary><b>Engines and example instances</b></summary>

| Engine | Example instance | Notes |
| --- | --- | --- |
| Danbooru | danbooru.donmai.us · safebooru.donmai.us | Safebooru (SFW subset) is the shipped default; API keys supported |
| Gelbooru | gelbooru.com · safebooru.org | API credentials unlock restricted posts |
| Moebooru | yande.re · konachan.com | |
| e621 / e926 | e926.net | e621 API; e926 is the SFW mirror of the same database |
| AnimePictures | anime-pictures.net | SFW anime illustrations |
| Szurubooru | your own instance | per-item categories supported |
| Shimmie | your own instance | |
| HyBooru | your own instance | tag hydration on first open |
| Nuuru | soybooru.com | proof-of-work clearance solves itself in-app |
| Philomena | derpibooru.org | Philomena v1 hosts |
| Booru-on-Rails | twibooru.org | the v3 Ruby codebase Philomena replaced |

</details>

New booru entries default to the `furry:g` tag, expanded to
`furry rating:general` on the wire: furry posts stay allowed, but only
general-rated ones by default. Existing entries keep their own default
tags.


## Requirements

- C++20 compiler (GCC, Clang, MSVC)
- CMake 3.16+
- Qt 6: Widgets, Concurrent, Sql, Multimedia, MultimediaWidgets, Network,
  WebEngine (cookie capture / PoW-wall browsing)

```sh
# Arch
sudo pacman -S qt6-base qt6-multimedia qt6-svg qt6-webengine cmake gcc

# Debian / Ubuntu
sudo apt install qt6-base-dev qt6-multimedia-dev libqt6svg6-dev qt6-webengine-dev cmake g++

# Fedora
sudo dnf install qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qtsvg-devel qt6-qtwebengine-devel cmake gcc-c++
```

Ugoira downloads need `ffmpeg` in `PATH` at runtime (`+cjxl` for animated
JXL); everything else degrades gracefully without them.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"   # -> build/box
cmake --install build --prefix ~/.local
ctest --test-dir build --output-on-failure
```

Debug builds: `-DCMAKE_BUILD_TYPE=Debug`. `cmake --install` also installs
the desktop entry, the `booru-explorer` icon, `box(1)` and the bash, zsh
and fish completions.

For a local system install that always replaces older package/manual
copies (and removes a stale `/usr/local/bin/box` that would shadow
`/usr/bin/box`), use the incremental helper:

```sh
./scripts/install-latest.sh          # /usr
BOX_PREFIX=~/.local ./scripts/install-latest.sh
```

Pacman packaging lives in the separate `box-git` repo (PKGBUILD builds
from `github.com/lusoterra/box`).

## Local archives

Build an archive with gallery-dl so the sidecars come along (verified
against gallery-dl 1.32):

```sh
gallery-dl --write-metadata -D ~/pics/safebooru \
  "https://safebooru.org/index.php?page=post&s=list&tags=hatsune_miku"
```

`--write-metadata` writes the per-file sidecar the scanner reads - without
it you get untagged files. Re-run the same command later for new posts;
add `--download-archive ~/pics/archive.sqlite3` to skip what you already
have. The `--write-info-json` is also useful.

## Hydrus library

Enable Preferences → Hydrus, then open `Hydrus` from the ＋ menu. The tab
keeps its own `hydrus.db` and media folder (defaults under the data dir),
imports files, folders or gallery-dl URLs, stores media as
`<sha256>.<ext>`, and pulls native namespaced tags from a local `hydd` or
Hydrus client API. It is read-only towards Hydrus and independent from the
favourites/downloads library; automatic tags below a configurable network
count can be masked through hydd's `/hyd/tag_counts` extension (when the
endpoint is unavailable, all tags are kept and the tab explains why).

## LAN sharing

`File → Share library on LAN…` starts a read-only Danbooru API over your
LAN. Pick the Hydrus tab, downloads, favourites, a local archive folder,
or any mix; duplicate rows are collapsed. The default bind is
`0.0.0.0:7071`, editable in the dialog and saved in `settings.ini`
(`share/*`).

```sh
# From another machine on the LAN:
curl 'http://192.168.1.10:7071/posts.json?tags=cat&limit=10'
curl 'http://192.168.1.10:7071/tags.json?search[name_matches]=blue*'
```

Add it to LoliSnatcher, or to another Box instance, as a Danbooru entry
with that base URL. Endpoints: `/posts.json`, `/posts/<id>.json`,
`/tags.json`, `/counts/posts.json`, `/share/file/<id>`,
`/share/thumb/<id>`. Media serves with Range support; local thumbnails
are cached under `~/.cache/box/share-thumbs`. There is no authentication
and nothing is writable; anyone who can reach the address can browse the
selected posts. The server stops when Box quits; there is no auto-start.
`API is live` in the bottom-right status bar reopens the picker while it is
running.

## Duplicate detection

`Tools → Find duplicate media…` (`Ctrl+Shift+D`) hashes decoded pixels
(plus SHA-256 for exact bytes) and groups duplicates per source family:
local archives, downloads/downloaded favourites, and the Hydrus tab never
match each other. Matching stills works across PNG/WebP/BMP and other
lossless conversions; heavily recompressed JPEGs may fall outside the
perceptual tolerance.

Each group lets you pick the copy to keep (the starred suggestion favours
the richest tag set) and delete the rest. Deletions go to the trash and
remove the matching JSON sidecars; archive cleanup is opt-in via
Preferences → Library, and Hydrus deletions also remove the `hydrus.db`
row. The hash cache lives in `dedup.db` (derived data: deleting it just
forces a rescan).

## Themes

Bundled themes live in `assets/themes/*.ini` and are embedded into the binary
(reconfigure after adding one). You can also drop a file into
`~/.config/box/themes/` to add or override a theme without rebuilding.
Each file is a tiny `[theme]` INI with `name`, `order`, `dark`, and hex
colours (`window`, `base`, `alternate`, `button`, `text`, `dim`,
`disabled`, `border`, `accent`); see `assets/themes/nord.ini` for the shape.

## License

This project is licensed under the **GNU Affero General Public License v3.0 or later** (AGPL-3.0-or-later) - see the [LICENSE](LICENSE) file for details.

[![AGPL-3.0](https://www.gnu.org/graphics/agplv3-with-text-162x68.png)](https://www.gnu.org/licenses/agpl-3.0.html)

### Third-party components

- **Qt 6** (Widgets, Network, Sql, Multimedia, WebEngine) - LGPLv3/GPLv3,
  see <https://www.qt.io/licensing/>
- **miniz** (`src/library/miniz/`) - MIT, see `src/library/miniz/LICENSE`
- **Lucide icons** (`assets/icons/`) - ISC, see `assets/icons/LICENSE.lucide`
- **FFmpeg** through Qt Multimedia for video decoding/encoding
  (LGPL/GPL depending on the distribution's build)
- **libjxl / cjxl** (optional, only for animated JXL conversion) - BSD-3-Clause
